/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>

#include "bf0_ble_gap.h"
#include "bf0_sibles.h"
#include "bf0_sibles_internal.h"
#include "bf0_sibles_advertising.h"
#include "ble_connection_manager.h"
#include "bf0_sibles_serial_trans_service.h"

#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "app_mem.h"

#define LOG_TAG "screencast"
#include "log.h"

extern void lv_ex_data_pool_init(void);

#define SCREEN_CAST_CATEID      0x20

#define SC_FRAME_MAGIC          0x53C1
#define SC_FRAME_TYPE_JPEG      0

#pragma pack(push, 1)
typedef struct
{
    uint16_t magic;
    uint8_t  type;
    uint8_t  reserved;
} sc_frame_hdr_t;
#pragma pack(pop)

#define SC_FRAME_BUF_MAX        (64 * 1024)
#define SC_FRAME_SLOT_NUM       3

enum sc_slot_state
{
    SC_SLOT_FREE = 0,
    SC_SLOT_WRITING,
    SC_SLOT_READY,
    SC_SLOT_DISPLAYING,
};

typedef struct
{
    uint8_t  *data;
    uint32_t  len;
    uint8_t   state;
} sc_frame_slot_t;

typedef struct
{
    uint8_t         conn_idx;

    sc_frame_slot_t slot[SC_FRAME_SLOT_NUM];
    struct rt_mutex  lock;

    lv_obj_t        *img;
    lv_img_dsc_t     dsc;
    rt_mailbox_t     mb_handle;
} sc_env_t;

static sc_env_t g_sc_env;

static sc_env_t *sc_get_env(void)
{
    return &g_sc_env;
}

/* ---------------------------------------------------------------------------
 * BLE advertising
 * ------------------------------------------------------------------------- */
SIBLES_ADVERTISING_CONTEXT_DECLAR(g_sc_adv_context);

#define SC_LOCAL_NAME "SIFLI_SCREENCAST"

static uint8_t sc_advertising_event(uint8_t event, void *context, void *data)
{
    switch (event)
    {
    case SIBLES_ADV_EVT_ADV_STARTED:
    {
        sibles_adv_evt_startted_t *evt = (sibles_adv_evt_startted_t *)data;
        LOG_I("ADV start result %d, mode %d", evt->status, evt->adv_mode);
        break;
    }
    case SIBLES_ADV_EVT_ADV_STOPPED:
    {
        sibles_adv_evt_stopped_t *evt = (sibles_adv_evt_stopped_t *)data;
        LOG_I("ADV stopped reason %d, mode %d", evt->reason, evt->adv_mode);
        break;
    }
    default:
        break;
    }
    return 0;
}

static uint8_t sc_advertising_start(void)
{
    sibles_advertising_para_t para = {0};
    char local_name[] = SC_LOCAL_NAME;
    uint8_t ret;

    para.own_addr_type = GAPM_STATIC_ADDR;
    para.config.adv_mode = SIBLES_ADV_CONNECT_MODE;
    para.config.mode_config.conn_config.duration = 0x0;
    para.config.mode_config.conn_config.interval = 0x30;
    para.config.max_tx_pwr = 0x7F;
    para.config.is_auto_restart = 1;

    para.adv_data.completed_name = rt_malloc(rt_strlen(local_name) + sizeof(sibles_adv_type_name_t));
    if (para.adv_data.completed_name == NULL)
    {
        LOG_E("advertising name alloc failed");
        return SIBLES_ADV_NOT_ALLOWED;
    }
    para.adv_data.completed_name->name_len = rt_strlen(local_name);
    rt_memcpy(para.adv_data.completed_name->name, local_name, para.adv_data.completed_name->name_len);
    para.evt_handler = sc_advertising_event;

    ret = sibles_advertising_init(g_sc_adv_context, &para);
    if (ret != SIBLES_ADV_NO_ERR)
    {
        LOG_E("advertising init failed %d", ret);
    }
    else
    {
        ret = sibles_advertising_start(g_sc_adv_context);
        if (ret != SIBLES_ADV_NO_ERR)
        {
            LOG_E("advertising start failed %d", ret);
        }
    }

    rt_free(para.adv_data.completed_name);
    return ret;
}

/* Receive a complete frame and publish it for display. */
static void sc_on_frame(uint8_t *payload, uint16_t len)
{
    sc_env_t *env = sc_get_env();
    uint8_t *jpg = payload;
    uint32_t jpg_len = len;

    /* Accept both "with header" and "raw JPEG" frames. */
    if (len >= sizeof(sc_frame_hdr_t))
    {
        sc_frame_hdr_t *hdr = (sc_frame_hdr_t *)payload;
        if (hdr->magic == SC_FRAME_MAGIC)
        {
            if (hdr->type != SC_FRAME_TYPE_JPEG)
            {
                LOG_W("unsupported frame type %d, drop", hdr->type);
                return;
            }
            jpg = payload + sizeof(sc_frame_hdr_t);
            jpg_len = len - sizeof(sc_frame_hdr_t);
        }
    }

    if (jpg_len == 0 || jpg_len > SC_FRAME_BUF_MAX)
    {
        LOG_W("bad frame len %d, drop", jpg_len);
        return;
    }

    rt_mutex_take(&env->lock, RT_WAITING_FOREVER);
    int widx = -1;
    for (int i = 0; i < SC_FRAME_SLOT_NUM; i++)
    {
        if (env->slot[i].state == SC_SLOT_FREE)
        {
            widx = i;
            break;
        }
    }
    if (widx < 0)
    {
        for (int i = 0; i < SC_FRAME_SLOT_NUM; i++)
        {
            if (env->slot[i].state == SC_SLOT_READY)
            {
                widx = i;
                break;
            }
        }
    }
    if (widx >= 0)
    {
        env->slot[widx].state = SC_SLOT_WRITING;
    }
    rt_mutex_release(&env->lock);

    if (widx < 0)
    {
        LOG_W("no slot available, drop frame");
        return;
    }

    sc_frame_slot_t *slot = &env->slot[widx];
    rt_memcpy(slot->data, jpg, jpg_len);
    slot->len = jpg_len;
    /* Publish the newest frame and discard stale ready frames. */
    rt_mutex_take(&env->lock, RT_WAITING_FOREVER);
    for (int i = 0; i < SC_FRAME_SLOT_NUM; i++)
    {
        if (i != widx && env->slot[i].state == SC_SLOT_READY)
        {
            env->slot[i].state = SC_SLOT_FREE;
        }
    }
    slot->state = SC_SLOT_READY;
    rt_mutex_release(&env->lock);

    LOG_D("frame received, jpeg %d bytes (slot %d)", jpg_len, widx);
}

static void sc_serial_tran_callback(ble_serial_tran_event_t event, uint8_t *data)
{
    switch (event)
    {
    case BLE_SERIAL_TRAN_OPEN:
    {
        ble_serial_open_t *chan = (ble_serial_open_t *)data;
        LOG_I("serial tran channel open, handle %d", chan->handle);
        break;
    }
    case BLE_SERIAL_TRAN_DATA:
    {
        ble_serial_tran_data_t *d = (ble_serial_tran_data_t *)data;
        LOG_D("serial tran DATA: cate_id=0x%02x len=%d", d->cate_id, d->len);
        if (d->cate_id == SCREEN_CAST_CATEID)
        {
            sc_on_frame(d->data, d->len);
        }
        break;
    }
    case BLE_SERIAL_TRAN_CLOSE:
        LOG_I("serial tran channel close");
        break;
    case BLE_SERIAL_TRAN_ERROR:
    {
        ble_serial_tran_error_t *e = (ble_serial_tran_error_t *)data;
        LOG_W("serial tran error %d", e->error);
        break;
    }
    default:
        break;
    }
}
/* Register the callback for the screen-cast category. */
BLE_SERIAL_TRAN_EXPORT(SCREEN_CAST_CATEID, sc_serial_tran_callback);


static void sc_display_init(void)
{
    sc_env_t *env = sc_get_env();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    env->img = lv_img_create(scr);
    lv_obj_center(env->img);
}
/* Select the latest ready frame for display. */
static void sc_display_update(void)
{
    sc_env_t *env = sc_get_env();
    int new_idx = -1;
    int old_idx = -1;

    rt_mutex_take(&env->lock, RT_WAITING_FOREVER);
    for (int i = 0; i < SC_FRAME_SLOT_NUM; i++)
    {
        if (env->slot[i].state == SC_SLOT_READY)
        {
            new_idx = i;
        }
        else if (env->slot[i].state == SC_SLOT_DISPLAYING)
        {
            old_idx = i;
        }
    }
    if (new_idx >= 0)
    {
        env->slot[new_idx].state = SC_SLOT_DISPLAYING;
    }
    rt_mutex_release(&env->lock);

    if (new_idx < 0)
    {
        return;
    }

    sc_frame_slot_t *slot = &env->slot[new_idx];

    lv_img_cache_invalidate_src(&env->dsc);

    env->dsc.data = slot->data;
    env->dsc.data_size = slot->len;
    env->dsc.header.cf = LV_IMG_CF_JPG;
    env->dsc.header.w = 0;
    env->dsc.header.h = 0;

    lv_img_set_src(env->img, &env->dsc);
    lv_obj_center(env->img);
    lv_obj_invalidate(env->img);

    if (old_idx >= 0)
    {
        rt_mutex_take(&env->lock, RT_WAITING_FOREVER);
        env->slot[old_idx].state = SC_SLOT_FREE;
        rt_mutex_release(&env->lock);
    }

    LOG_D("display update: jpeg %d bytes (slot %d)", slot->len, new_idx);
}

int sc_ble_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    sc_env_t *env = sc_get_env();
    switch (event_id)
    {
    case BLE_POWER_ON_IND:
    {
        if (env->mb_handle)
        {
            rt_err_t ret = rt_mb_send(env->mb_handle, BLE_POWER_ON_IND);
            if (ret != RT_EOK)
            {
                LOG_E("BLE mailbox send failed %d", ret);
            }
        }
        break;
    }
    case BLE_GAP_CONNECTED_IND:
    {
        ble_gap_connect_ind_t *ind = (ble_gap_connect_ind_t *)data;
        env->conn_idx = ind->conn_idx;
        LOG_I("connected, conn_idx %d", ind->conn_idx);
        ble_gap_update_conn_param(BLE_GAP_CREATE_UPDATE_CONN_PARA(ind->conn_idx, 12, 12, 0, 500));
        break;
    }
    case SIBLES_MTU_EXCHANGE_IND:
    {
        sibles_mtu_exchange_ind_t *ind = (sibles_mtu_exchange_ind_t *)data;
        LOG_I("MTU %d", ind->mtu);
        break;
    }
    case BLE_GAP_DISCONNECTED_IND:
    {
        ble_gap_disconnected_ind_t *ind = (ble_gap_disconnected_ind_t *)data;
        LOG_I("disconnected, reason %d", ind->reason);
        break;
    }
    default:
        break;
    }
    return 0;
}
BLE_EVENT_REGISTER(sc_ble_event_handler, NULL);

#ifndef NVDS_AUTO_UPDATE_MAC_ADDRESS_ENABLE
ble_common_update_type_t ble_request_public_address(bd_addr_t *addr)
{
    int ret = bt_mac_addr_generate_via_uid_v2(addr);
    if (ret != 0)
    {
        LOG_W("generate mac address failed %d", ret);
        return BLE_UPDATE_NO_UPDATE;
    }
    return BLE_UPDATE_ONCE;
}
#endif

static void sc_ble_thread(void *param)
{
    (void)param;
    sc_env_t *env = sc_get_env();
    RT_ASSERT(env->mb_handle);
    if (!env->mb_handle)
    {
        LOG_E("BLE mailbox creation failed");
        return;
    }
    sifli_ble_enable();

    while (1)
    {
        uint32_t value;
        rt_err_t ret = rt_mb_recv(env->mb_handle, (rt_uint32_t *)&value, RT_WAITING_FOREVER);
        if (ret != RT_EOK)
        {
            LOG_E("BLE mailbox receive failed %d", ret);
            continue;
        }
        if (value == BLE_POWER_ON_IND)
        {
            ble_serial_tran_init();
            if (sc_advertising_start() == SIBLES_ADV_NO_ERR)
            {
                LOG_I("BLE powered on, advertising as \"%s\"", SC_LOCAL_NAME);
            }
        }
    }
}

int main(void)
{
    sc_env_t *env = sc_get_env();

    rt_err_t ret = rt_mutex_init(&env->lock, "sc", RT_IPC_FLAG_FIFO);
    RT_ASSERT(ret == RT_EOK);
    if (ret != RT_EOK)
    {
        LOG_E("frame mutex initialization failed %d", ret);
        return RT_ERROR;
    }
    /* Allocate frame buffers. */
    for (int i = 0; i < SC_FRAME_SLOT_NUM; i++)
    {
        env->slot[i].data = rt_malloc_align(SC_FRAME_BUF_MAX, 64);
        if (!env->slot[i].data)
        {
            LOG_E("frame buffer %d allocation failed", i);
            return RT_ENOMEM;
        }
        env->slot[i].len = 0;
        env->slot[i].state = SC_SLOT_FREE;
    }

    ret = littlevgl2rtt_init("lcd");
    RT_ASSERT(ret == RT_EOK);
    if (ret != RT_EOK)
    {
        LOG_E("LVGL initialization failed %d", ret);
        return ret;
    }
    lv_ex_data_pool_init();
    sc_display_init();

    env->mb_handle = rt_mb_create("sc", 8, RT_IPC_FLAG_FIFO);
    RT_ASSERT(env->mb_handle);
    if (!env->mb_handle)
    {
        LOG_E("BLE mailbox creation failed");
        return RT_ENOMEM;
    }

    rt_thread_t tid = rt_thread_create("sc_ble", sc_ble_thread, NULL, 2048,
                                       RT_THREAD_PRIORITY_MAX / 2, 10);
    RT_ASSERT(tid);
    if (!tid)
    {
        LOG_E("BLE thread creation failed");
        return RT_ERROR;
    }
    ret = rt_thread_startup(tid);
    RT_ASSERT(ret == RT_EOK);
    if (ret != RT_EOK)
    {
        LOG_E("BLE thread startup failed %d", ret);
        return ret;
    }

    while (1)
    {
        sc_display_update();
        uint32_t ms = lv_task_handler();
        rt_thread_mdelay(ms ? ms : 1);
    }
    return RT_EOK;
}
