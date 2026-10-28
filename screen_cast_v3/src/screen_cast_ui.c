/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */



#include "screen_cast_ui.h"
#include "screen_cast.h"
#include "sc_qr.h"

#include <rtthread.h>
#include <string.h>
#include <stdio.h>

#include "lvgl.h"
#include "app_mem.h"

#ifdef PKG_USING_CJSON
    #include "cJSON.h"
#endif

#define LOG_TAG     "SC_CAST"
#include "log.h"

#ifndef LV_IMG_CF_JPG
    #define LV_IMG_CF_JPG LV_IMG_CF_RAW
#endif

#define SC_UI_REFRESH_MS        20

#define SC_UI_QR_MIN_SIZE       140
#define SC_UI_QR_MAX_SIZE       300
#define SC_UI_QR_TIP            "Scan with the phone app to connect"

typedef struct
{
    lv_obj_t   *scr;
    lv_obj_t   *photo;          /* full screen JPEG view */
    lv_obj_t   *nav_cont;       /* navigation overlay container */
    lv_obj_t   *turn_icon;      /* turn direction icon (ezip) */
    lv_obj_t   *road_info;      /* "x m turn into y road, speed z km/h" */
    lv_obj_t   *other_info;     /* remaining time / distance / traffic lights */
    lv_timer_t *timer;

    lv_img_dsc_t jpg_dsc;       /* descriptor backed by an app owned buffer */
    lv_img_dsc_t icon_dsc;      /* descriptor backed by the icon copy buffer */
    uint8_t     *icon_data;     /* copy of the icon frame */
    uint32_t     icon_data_len;
    uint32_t     icon_data_max;

    lv_obj_t   *qr_page;        /* whole "scan to connect" page */
    lv_obj_t   *qr_code;
    lv_obj_t   *qr_tip;
    bool        qr_visible;
    bool        qr_ready;       /* payload already written into the QR widget */

    sc_map_type_t map_type;
} sc_ui_ctx_t;

static sc_ui_ctx_t *p_ui = NULL;

static void sc_ui_show_nav(bool show)
{
    if (p_ui->nav_cont == NULL)
    {
        return;
    }
    if (show)
    {
        lv_obj_clear_flag(p_ui->nav_cont, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(p_ui->nav_cont, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Frame size the phone should encode with (the `Sz` field in the QR code).
 *
 * Same rule as solution2.0: cap at 840x480 and align down to 16 pixels, so the
 * JPEG stays small enough to travel over the BLE link.
 */
static int32_t sc_ui_cast_w(void)
{
    int32_t w = LV_HOR_RES > 840 ? 840 : LV_HOR_RES;
    return (w / 16) * 16;
}

static int32_t sc_ui_cast_h(void)
{
    int32_t h = LV_VER_RES > 480 ? 480 : LV_VER_RES;
    return (h / 16) * 16;
}

static void sc_ui_show_qr(bool show)
{
    if (p_ui->qr_page == NULL || p_ui->qr_visible == show)
    {
        return;
    }

    if (show)
    {
        /* The map image widget is created later, so raise the QR page to the top */
        lv_obj_move_foreground(p_ui->qr_page);
        lv_obj_clear_flag(p_ui->qr_page, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(p_ui->qr_page, LV_OBJ_FLAG_HIDDEN);
    }
    p_ui->qr_visible = show;
}

/**
 * @brief Create the "scan to connect" page.
 *
 * The phone app cannot scan or connect by itself, it only takes the device
 * address from the scanned QR code, so the address must be on screen. The
 * payload is filled in by @ref sc_ui_update_qr once BLE is powered and the
 * public address is readable.
 */
static void sc_ui_create_qr_page(void)
{
    int32_t qr_size = LV_VER_RES - 88;

    if (qr_size > SC_UI_QR_MAX_SIZE)
    {
        qr_size = SC_UI_QR_MAX_SIZE;
    }
    if (qr_size < SC_UI_QR_MIN_SIZE)
    {
        qr_size = SC_UI_QR_MIN_SIZE;
    }

    p_ui->qr_page = lv_obj_create(p_ui->scr);
    lv_obj_remove_style_all(p_ui->qr_page);
    lv_obj_set_size(p_ui->qr_page, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(p_ui->qr_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p_ui->qr_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p_ui->qr_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(p_ui->qr_page, LV_ALIGN_CENTER, 0, 0);

#if LV_USE_QRCODE
    p_ui->qr_code = sc_qr_create(p_ui->qr_page, qr_size, lv_color_black(), lv_color_white());
    if (p_ui->qr_code != NULL)
    {
        lv_obj_align(p_ui->qr_code, LV_ALIGN_CENTER, 0, -20);
    }
#else
    LOG_W("LV_USE_QRCODE is off, enable CONFIG_LV_USE_QRCODE to show the qr code");
#endif

    p_ui->qr_tip = lv_label_create(p_ui->qr_page);
    lv_label_set_text(p_ui->qr_tip, SC_UI_QR_TIP);
    lv_obj_set_style_text_color(p_ui->qr_tip, lv_color_white(), 0);
    lv_label_set_long_mode(p_ui->qr_tip, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(p_ui->qr_tip, LV_HOR_RES - 40);
    lv_obj_set_style_text_align(p_ui->qr_tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(p_ui->qr_tip, LV_ALIGN_BOTTOM_MID, 0, -16);
}

/**
 * @brief Fill the QR widget once the device address becomes readable.
 *
 * Called from the refresh timer: the BLE stack powers up asynchronously and
 * @ref sc_build_connect_qr fails until the public address is ready. Retrying
 * here is simpler and more robust than hooking the BLE power-on event.
 */
static void sc_ui_update_qr(void)
{
#if LV_USE_QRCODE
    char payload[128];
    int len;

    if (p_ui->qr_ready || p_ui->qr_code == NULL)
    {
        return;
    }
    // build the payload string
#ifdef USING_SCREEN_CAST_WIFI
    // WiFi variant: the phone joins the P2P GO hotspot, then connects to the device TCP server
    len = sc_build_wifi_qr(payload, sizeof(payload), sc_ui_cast_w(), sc_ui_cast_h());
#else
    len = sc_build_connect_qr(payload, sizeof(payload), sc_ui_cast_w(), sc_ui_cast_h(),
                              SC_CAST_TYPE_BLE);
#endif
    if (len <= 0)
    {
        return;
    }

    /* Encode the payload into a QR pattern and put it on screen */
    //Example payload: Tt:1;Mac:0300ab0000c8;Ql:0.2;Sz:832x480;Tp:1
    //Tt:1      cast transport type 1 (BLE)
    //Mac:...   device MAC address
    //Ql:       JPEG compression quality 0.2
    //Sz:       requested frame size 832x480
    //Tp:1      navigation info version, decides the NAV_INFO layout the phone sends
    sc_qr_set_text(p_ui->qr_code, payload);

    p_ui->qr_ready = true;
    LOG_I("qr payload: %s", payload);
#endif
}

static lv_obj_t *sc_ui_ensure_label(lv_obj_t **label, int32_t y)
{
    if (*label == NULL)
    {
        *label = lv_label_create(p_ui->nav_cont);
        lv_label_set_long_mode(*label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(*label, LV_HOR_RES - 40);
        lv_obj_set_style_text_color(*label, lv_color_white(), 0);
        lv_obj_align(*label, LV_ALIGN_TOP_MID, 0, y);
    }
    return *label;
}

/* Copy of the JPEG frame currently on screen. The LVGL decoder only reads it in
 * the refresh phase after sc_ui_handle_jpeg(), so the UI keeps its own copy and
 * the ring buffer slot can be released right away. */
static uint8_t *s_jpg_frame;
static uint32_t s_jpg_frame_size;

/* JPEG display copy buffer: prefers the PSRAM pool (app_cache_alloc falls back
 * to the system heap on failure) and keeps 64-byte alignment, as the hardware
 * JPEG decoder requires. The original implementation used rt_malloc_align for a
 * permanent ~30KB of SRAM, which competed with WiFi/LVGL and made frame
 * allocation fail; it now lives in PSRAM. */
static uint8_t *sc_ui_jpg_alloc(uint32_t size)
{
    const uint32_t alignment = 64;
    uint32_t total = size + alignment - 1 + sizeof(void *);
    void *raw = app_cache_alloc(total, CACHE_PSRAM);
    uint8_t *aligned;

    if (raw == NULL)
    {
        return NULL;
    }
    aligned = (uint8_t *)(((uintptr_t)raw + sizeof(void *) + alignment - 1) & ~(uintptr_t)(alignment - 1));
    *(void **)(aligned - sizeof(void *)) = raw;
    return aligned;
}

static void sc_ui_jpg_free(uint8_t *aligned)
{
    if (aligned)
    {
        app_cache_free(*(void **)(aligned - sizeof(void *)));
    }
}

static void sc_ui_handle_jpeg(void)
{
    sc_frame_t *jpg = sc_frame_acquire(SC_DATA_TYPE_JPEG);
    if (jpg == NULL)
    {
        return;
    }

    if (s_jpg_frame_size < jpg->size)
    {
        uint8_t *new_buf = sc_ui_jpg_alloc(jpg->size);

        if (new_buf == NULL)
        {
            sc_frame_release(SC_DATA_TYPE_JPEG);
            return;
        }
        if (s_jpg_frame)
        {
            sc_ui_jpg_free(s_jpg_frame);
        }
        s_jpg_frame = new_buf;
        s_jpg_frame_size = jpg->size;
    }
    rt_memcpy(s_jpg_frame, jpg->data, jpg->size);
    sc_frame_release(SC_DATA_TYPE_JPEG);

    if (p_ui->photo == NULL)
    {
        p_ui->photo = lv_img_create(p_ui->scr);
        lv_obj_center(p_ui->photo);
    }

    p_ui->jpg_dsc.data = s_jpg_frame;
    p_ui->jpg_dsc.data_size = jpg->size;
    p_ui->jpg_dsc.header.cf = LV_IMG_CF_JPG;
    p_ui->jpg_dsc.header.w = 0;
    p_ui->jpg_dsc.header.h = 0;


    lv_img_cache_invalidate_src(&p_ui->jpg_dsc);
    lv_img_set_src(p_ui->photo, &p_ui->jpg_dsc);
    lv_obj_center(p_ui->photo);
    lv_obj_invalidate(p_ui->photo);
}

#ifdef PKG_USING_CJSON
static void sc_ui_update_turn_icon(uint8_t *pic_data, uint32_t pic_len)
{
    lv_img_header_t header;

    if (pic_len <= sizeof(lv_img_header_t))
    {
        return;
    }

    if (LV_RES_OK != lv_img_decoder_get_info(pic_data, &header))
    {
        LOG_I("icon decoder get info failed");
        return;
    }

    if (!p_ui->icon_data || p_ui->icon_data_max < pic_len)
    {
        if (p_ui->icon_data)
        {
            rt_free(p_ui->icon_data);
        }
        p_ui->icon_data = rt_malloc(pic_len);
        if (p_ui->icon_data == NULL)
        {
            p_ui->icon_data_max = 0;
            return;
        }
        p_ui->icon_data_max = pic_len;
    }
    p_ui->icon_data_len = pic_len;
    rt_memcpy(p_ui->icon_data, pic_data, pic_len);

    rt_memset(&p_ui->icon_dsc, 0, sizeof(p_ui->icon_dsc));
    p_ui->icon_dsc.data = p_ui->icon_data + sizeof(lv_img_header_t);
    p_ui->icon_dsc.data_size = p_ui->icon_data_len - sizeof(lv_img_header_t);
    p_ui->icon_dsc.header.cf = header.cf;
    p_ui->icon_dsc.header.w = header.w;
    p_ui->icon_dsc.header.h = header.h;

    if (p_ui->turn_icon == NULL)
    {
        p_ui->turn_icon = lv_img_create(p_ui->nav_cont);
    }
    lv_img_set_decoder_src(p_ui->turn_icon, &p_ui->icon_dsc);
    lv_obj_align(p_ui->turn_icon, LV_ALIGN_TOP_LEFT, 20, 20);
}

static void sc_ui_handle_nav(void)
{
    sc_frame_t *nav = sc_frame_acquire(SC_DATA_TYPE_NAV_INFO);
    if (nav == NULL)
    {
        return;
    }

    uint32_t json_len;
    uint32_t pic_len;
    uint8_t *json_data;
    uint8_t *pic_data;

    if (nav->size < 8)
    {
        sc_frame_release(SC_DATA_TYPE_NAV_INFO);
        return;
    }

    rt_memcpy(&json_len, nav->data, 4);
    if (nav->size < 8 + json_len)
    {
        sc_frame_release(SC_DATA_TYPE_NAV_INFO);
        return;
    }
    rt_memcpy(&pic_len, nav->data + 4 + json_len, 4);
    json_data = nav->data + 4;
    pic_data = nav->data + 4 + json_len + 4;

    cJSON *root = cJSON_Parse((const char *)json_data);
    if (root == NULL)
    {
        LOG_I("nav json parse failed");
        sc_frame_release(SC_DATA_TYPE_NAV_INFO);
        return;
    }

    cJSON *item;
    char *road = NULL;
    int distance = 0, speed = 0, remain_sec = 0, remain_dis = 0, remain_lights = 0;
    int icon_changed = 0;

    item = cJSON_GetObjectItem(root, "roadName");
    if (cJSON_IsString(item)) road = item->valuestring;
    item = cJSON_GetObjectItem(root, "distance");
    if (cJSON_IsNumber(item)) distance = item->valueint;
    item = cJSON_GetObjectItem(root, "speed");
    if (cJSON_IsNumber(item)) speed = item->valueint;
    item = cJSON_GetObjectItem(root, "remainTime");
    if (cJSON_IsNumber(item)) remain_sec = item->valueint;
    item = cJSON_GetObjectItem(root, "remainDistance");
    if (cJSON_IsNumber(item)) remain_dis = item->valueint;
    item = cJSON_GetObjectItem(root, "remainLights");
    if (cJSON_IsNumber(item)) remain_lights = item->valueint;
    item = cJSON_GetObjectItem(root, "turnIconChanged");
    if (cJSON_IsNumber(item)) icon_changed = item->valueint;

    char remain_time_str[24];
    if (remain_sec > 3600)
    {
        rt_snprintf(remain_time_str, sizeof(remain_time_str), "%d小时%d分钟", remain_sec / 3600, remain_sec % 3600 / 60);
    }
    else
    {
        rt_snprintf(remain_time_str, sizeof(remain_time_str), "%d分钟", remain_sec / 60);
    }

    char remain_dis_str[24];
    if (remain_dis > 1000)
    {
        rt_snprintf(remain_dis_str, sizeof(remain_dis_str), "%.1f公里", remain_dis / 1000.0f);
    }
    else
    {
        rt_snprintf(remain_dis_str, sizeof(remain_dis_str), "%d米", remain_dis);
    }

    lv_label_set_text_fmt(sc_ui_ensure_label(&p_ui->road_info, 30),
                          "%d米进入%s, 速度:%dkm/h", distance, road ? road : "", speed);

    lv_label_set_text_fmt(sc_ui_ensure_label(&p_ui->other_info, 0),
                          "剩余%s %s, 红绿灯:%d个", remain_time_str, remain_dis_str, remain_lights);
    lv_obj_align(p_ui->other_info, LV_ALIGN_BOTTOM_MID, 0, -30);

    if (icon_changed || p_ui->turn_icon == NULL)
    {
        sc_ui_update_turn_icon(pic_data, pic_len);
    }

    cJSON_Delete(root);
    sc_frame_release(SC_DATA_TYPE_NAV_INFO);
}
#else
static void sc_ui_handle_nav(void)
{
    sc_frame_t *nav = sc_frame_acquire(SC_DATA_TYPE_NAV_INFO);
    if (nav)
    {
        sc_frame_release(SC_DATA_TYPE_NAV_INFO);
    }
}
#endif /* PKG_USING_CJSON */

static void sc_ui_refresh_cb(lv_timer_t *timer)
{
    sc_map_type_t type;

    /* BLE stack powers up asynchronously; the QR payload can only be built once
     * the public address is readable */
    sc_ui_update_qr();

    /* Show the "scan to connect" page while no cast stream is arriving; it
     * comes back automatically after a session ends */
    sc_ui_show_qr(!sc_is_casting());

    /* This example plays no audio, it only releases the buffer so the ring
     * does not fill up */
    sc_frame_t *mp3 = sc_frame_acquire(SC_DATA_TYPE_MP3);
    if (mp3)
    {
        sc_frame_release(SC_DATA_TYPE_MP3);
    }

    sc_ui_handle_jpeg();

    type = sc_get_map_type();
    if (type != p_ui->map_type)
    {
        p_ui->map_type = type;
        LOG_I("map type %d", type);
        sc_ui_show_nav(type == SC_MAP_TYPE_NAV_MODE);
    }

    if (type == SC_MAP_TYPE_NAV_MODE)
    {
        sc_ui_handle_nav();
    }
}

void screen_cast_ui_init(void)
{
    if (p_ui != NULL)
    {
        return;
    }

    p_ui = rt_calloc(1, sizeof(sc_ui_ctx_t));
    if (p_ui == NULL)
    {
        LOG_E("no memory for ui ctx");
        return;
    }

    p_ui->scr = lv_scr_act();
    lv_obj_set_style_bg_color(p_ui->scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p_ui->scr, LV_OPA_COVER, 0);
    p_ui->map_type = SC_MAP_TYPE_NULL;

    p_ui->nav_cont = lv_obj_create(p_ui->scr);
    lv_obj_remove_style_all(p_ui->nav_cont);
    lv_obj_set_size(p_ui->nav_cont, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(p_ui->nav_cont, LV_OPA_TRANSP, 0);
    lv_obj_align(p_ui->nav_cont, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(p_ui->nav_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p_ui->nav_cont, LV_OBJ_FLAG_SCROLLABLE);

    sc_ui_create_qr_page();
    sc_ui_show_qr(true);

    p_ui->timer = lv_timer_create(sc_ui_refresh_cb, SC_UI_REFRESH_MS, NULL);

    LOG_I("screen cast ui inited (%dx%d)", LV_HOR_RES, LV_VER_RES);
}
