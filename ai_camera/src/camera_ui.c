/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_ui.h"
#include "camera_ui_font.h"
#include "camera_service.h"
#include "camera_keys.h"

#include <bf0_hal.h>
#include <drv_epic.h>
#include <littlevgl2rtt.h>
#include <lv_ex_data.h>
#include <lvgl.h>
#include <rtthread.h>
#include <string.h>

#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 0
#error "camera_ui requires native RGB565 LVGL colors"
#endif

#define CAMERA_UI_WIDTH 1024
#define CAMERA_UI_HEIGHT 600
#define CAMERA_UI_PHOTO_INFO_HEIGHT 40U
#define CAMERA_UI_HOME_COUNT 6U
#define CAMERA_UI_STACK_SIZE 6144U
#define CAMERA_UI_PRIORITY 20U
#define CAMERA_UI_POLL_MS 10U
#define CAMERA_UI_DIAG_WINDOW_MS 5000U
#define CAMERA_UI_CONTROL_X (CAMERA_LIVE_WIDTH + 16U)
#define CAMERA_UI_CONTROL_WIDTH (CAMERA_UI_WIDTH - CAMERA_UI_CONTROL_X - 16U)
#define CAMERA_UI_HOME_THUMB_WIDTH 85U
#define CAMERA_UI_HOME_THUMB_HEIGHT 64U
#define CAMERA_UI_GALLERY_THUMB_WIDTH 160U
#define CAMERA_UI_GALLERY_THUMB_HEIGHT 120U

typedef enum
{
    CAMERA_VIEW_HOME,
    CAMERA_VIEW_GALLERY,
    CAMERA_VIEW_PHOTO
} camera_view_t;

static rt_thread_t s_thread;
static lv_group_t *s_key_group;
static lv_obj_t *s_roots[3];
static lv_obj_t *s_status[3];
static lv_obj_t *s_live_image;
static lv_obj_t *s_capture_overlay;
static lv_obj_t *s_capture_message;
static lv_obj_t *s_camera_status;
static lv_obj_t *s_full_image;
static lv_obj_t *s_photo_info;
static lv_obj_t *s_home_tiles[CAMERA_UI_HOME_COUNT];
static lv_obj_t *s_home_images[CAMERA_UI_HOME_COUNT];
static lv_obj_t *s_gallery_tiles[CAMERA_PAGE_SIZE];
static lv_obj_t *s_gallery_images[CAMERA_PAGE_SIZE];
static lv_obj_t *s_capture_button;
static lv_obj_t *s_gallery_button;
static lv_obj_t *s_back_button;
static lv_obj_t *s_previous_button;
static lv_obj_t *s_next_button;
static lv_obj_t *s_page_label;
static lv_img_dsc_t s_live_image_dsc;
static lv_img_dsc_t s_full_image_dsc;
static lv_img_dsc_t s_thumb_dsc[CAMERA_PAGE_SIZE];
static camera_live_frame_t s_live_frame = {.slot = -1};
static uint32_t s_displayed_sequence;
static int s_has_displayed_sequence;
static size_t s_item_count;
static uint32_t s_gallery_offset;
static uint32_t s_photo_offset;
static uint32_t s_total;
static uint32_t s_next_token;
static uint32_t s_pending_token;
static camera_media_kind_t s_pending_kind;
static camera_view_t s_view;
static camera_view_t s_photo_origin;
static int s_loading;
static int s_capture_busy;
static int s_capture_pending;
static int s_capture_prompt_flushed;
static int s_capture_prompt_visible;
static camera_capture_stage_t s_capture_stage;
static int s_display_started;
static int s_refresh_home;
static const char *s_after_page_status;
static const uint8_t s_black_pixel[2] = {0, 0};
static const lv_img_dsc_t s_empty_image = {
    .header = {.cf = LV_IMG_CF_TRUE_COLOR, .w = 1, .h = 1},
    .data_size = sizeof(s_black_pixel),
    .data = s_black_pixel,
};

static void camera_ui_request_page(uint32_t offset);
static void camera_ui_update_controls(void);
static void camera_ui_focus_available(void);

typedef struct
{
    uint32_t count;
    uint64_t sum_ticks;
    rt_tick_t max_ticks;
} camera_ui_timing_t;

typedef struct
{
    rt_tick_t begin;
    int active;
    camera_ui_timing_t handler;
    camera_ui_timing_t frame_update;
    camera_ui_timing_t flush_total;
    camera_ui_timing_t idle;
    uint32_t source_expired;
    uint32_t redraw_video_only;
    uint32_t redraw_sidebar;
} camera_ui_diag_t;

static camera_ui_diag_t s_ui_diag;

static void camera_ui_diag_reset(void)
{
    memset(&s_ui_diag, 0, sizeof(s_ui_diag));
}

static void camera_ui_timing_record(camera_ui_timing_t *timing, rt_tick_t begin)
{
    rt_tick_t elapsed;

    if (!s_ui_diag.active)
        return;
    elapsed = (rt_tick_t)(rt_tick_get() - begin);
    ++timing->count;
    timing->sum_ticks += elapsed;
    if (elapsed > timing->max_ticks)
        timing->max_ticks = elapsed;
}

static unsigned long camera_ui_timing_avg_ms(const camera_ui_timing_t *timing)
{
    return timing->count != 0U
               ? (unsigned long)(timing->sum_ticks * 1000U /
                                 RT_TICK_PER_SECOND / timing->count)
               : 0UL;
}

static unsigned long camera_ui_ticks_ms(rt_tick_t ticks)
{
    return (unsigned long)((uint64_t)ticks * 1000U / RT_TICK_PER_SECOND);
}

static void camera_ui_diag_report(void)
{
    camera_ui_diag_t stats;
    rt_tick_t now;
    rt_tick_t elapsed;

    if (!s_ui_diag.active || s_view != CAMERA_VIEW_HOME)
        return;
    now = rt_tick_get();
    elapsed = (rt_tick_t)(now - s_ui_diag.begin);
    if (elapsed < rt_tick_from_millisecond(CAMERA_UI_DIAG_WINDOW_MS))
        return;
    stats = s_ui_diag;
    camera_ui_diag_reset();
    s_ui_diag.begin = now;
    s_ui_diag.active = 1;

    /* This preview window is independent of camera.fps; handler includes flush_total. */
    rt_kprintf("camera.ui: window_ms=%lu avg/max_ms handler=%lu/%lu(n=%lu) frame_update=%lu/%lu(n=%lu)\n",
               camera_ui_ticks_ms(elapsed),
               camera_ui_timing_avg_ms(&stats.handler), camera_ui_ticks_ms(stats.handler.max_ticks),
               (unsigned long)stats.handler.count,
               camera_ui_timing_avg_ms(&stats.frame_update), camera_ui_ticks_ms(stats.frame_update.max_ticks),
               (unsigned long)stats.frame_update.count);
    rt_kprintf("camera.ui.flush: avg/max_ms total=%lu/%lu(n=%lu) idle=%lu/%lu(n=%lu)\n",
               camera_ui_timing_avg_ms(&stats.flush_total), camera_ui_ticks_ms(stats.flush_total.max_ticks),
               (unsigned long)stats.flush_total.count,
               camera_ui_timing_avg_ms(&stats.idle), camera_ui_ticks_ms(stats.idle.max_ticks),
               (unsigned long)stats.idle.count);
    rt_kprintf("camera.ui.redraw: video_only=%lu with_sidebar=%lu source_expired=%lu\n",
               (unsigned long)stats.redraw_video_only, (unsigned long)stats.redraw_sidebar,
               (unsigned long)stats.source_expired);
}

static void camera_ui_status(const char *text)
{
    lv_label_set_text(s_status[s_view], text);
}

static void camera_ui_set_disabled(lv_obj_t *object, int disabled)
{
    if (disabled)
        lv_obj_add_state(object, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(object, LV_STATE_DISABLED);
}

static lv_obj_t *camera_ui_label(lv_obj_t *parent, const char *text,
                                  int x, int y, int width)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &camera_ui_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xcbd5e1), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

static void camera_ui_key_target(lv_obj_t *object)
{
    lv_obj_set_style_outline_color(object, lv_color_hex(0xffd166),
                                   LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(object, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(object, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_group_add_obj(s_key_group, object);
}

static lv_obj_t *camera_ui_button(lv_obj_t *parent, const char *text,
                                   int x, int y, int width, int height,
                                   lv_event_cb_t callback, uintptr_t value)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_t *label;

    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2379e8), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x164b90),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1b3555),
                              LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    label = camera_ui_label(button, text, 0, 0, width);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)value);
    camera_ui_key_target(button);
    return button;
}

static lv_obj_t *camera_ui_root(lv_obj_t *screen)
{
    lv_obj_t *root = lv_obj_create(screen);

    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, CAMERA_UI_WIDTH, CAMERA_UI_HEIGHT);
    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

static void camera_ui_home_background_event(lv_event_t *event)
{
    static const lv_area_t *saved_clip;
    static lv_area_t background_clip;
    lv_event_code_t code = lv_event_get_code(event);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_area_t root_area;
    lv_area_t live_area;
    lv_area_t right_area;

    if (lv_event_get_target(event) != s_roots[CAMERA_VIEW_HOME] || draw_ctx == NULL)
        return;
    if (code == LV_EVENT_DRAW_MAIN_END)
    {
        if (saved_clip != NULL)
        {
            draw_ctx->clip_area = saved_clip;
            saved_clip = NULL;
        }
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN_BEGIN)
        return;
    saved_clip = NULL;
    if (s_view != CAMERA_VIEW_HOME || s_live_image == NULL || s_live_frame.slot < 0 ||
        draw_ctx->clip_area == NULL || lv_obj_has_flag(s_live_image, LV_OBJ_FLAG_HIDDEN) ||
        s_live_frame.pixels == NULL || s_live_image_dsc.data != s_live_frame.pixels ||
        lv_img_get_src(s_live_image) != &s_live_image_dsc ||
        s_live_image_dsc.header.cf != LV_IMG_CF_TRUE_COLOR ||
        s_live_image_dsc.header.w != CAMERA_LIVE_WIDTH ||
        s_live_image_dsc.header.h != CAMERA_LIVE_HEIGHT ||
        s_live_image_dsc.data_size != CAMERA_LIVE_WIDTH * CAMERA_LIVE_HEIGHT * 2U ||
        lv_img_get_angle(s_live_image) != 0 || lv_img_get_zoom(s_live_image) != LV_IMG_ZOOM_NONE ||
        lv_img_get_offset_x(s_live_image) != 0 || lv_img_get_offset_y(s_live_image) != 0 ||
        _lv_obj_get_layer_type(s_live_image) != LV_LAYER_TYPE_NONE ||
        _lv_obj_get_layer_type(s_roots[CAMERA_VIEW_HOME]) != LV_LAYER_TYPE_NONE ||
        lv_obj_get_style_opa(s_live_image, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_style_img_opa(s_live_image, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_style_blend_mode(s_live_image, LV_PART_MAIN) != LV_BLEND_MODE_NORMAL ||
        lv_obj_get_style_clip_corner(s_live_image, LV_PART_MAIN) ||
        lv_obj_get_style_pad_left(s_live_image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_right(s_live_image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_top(s_live_image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_bottom(s_live_image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_border_width(s_live_image, LV_PART_MAIN) != 0)
        return;

    lv_obj_get_coords(s_roots[CAMERA_VIEW_HOME], &root_area);
    lv_obj_get_coords(s_live_image, &live_area);
    if (lv_area_get_width(&root_area) != CAMERA_UI_WIDTH ||
        lv_area_get_height(&root_area) != CAMERA_UI_HEIGHT ||
        live_area.x1 != root_area.x1 || live_area.y1 != root_area.y1 ||
        lv_area_get_width(&live_area) != CAMERA_LIVE_WIDTH ||
        lv_area_get_height(&live_area) != CAMERA_LIVE_HEIGHT)
        return;

    right_area = root_area;
    right_area.x1 += CAMERA_LIVE_WIDTH;
    if (!_lv_area_intersect(&background_clip, draw_ctx->clip_area, &right_area))
        lv_area_set(&background_clip, 0, 0, -1, -1);

    /* The opaque live image redraws the left area after this background.
     * An empty intersection skips the background during video-only redraws. */
    saved_clip = draw_ctx->clip_area;
    draw_ctx->clip_area = &background_clip;
}

static void camera_ui_image_desc(lv_img_dsc_t *descriptor,
                                  const uint8_t *pixels,
                                  uint16_t width, uint16_t height)
{
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->header.cf = LV_IMG_CF_TRUE_COLOR;
    descriptor->header.w = width;
    descriptor->header.h = height;
    descriptor->data_size = (uint32_t)width * height * 2U;
    descriptor->data = pixels;
}

static void camera_ui_fit_thumbnail(lv_obj_t *image,
                                    uint16_t target_width, uint16_t target_height)
{
    uint32_t zoom = (uint32_t)target_width * LV_IMG_ZOOM_NONE / CAMERA_THUMB_WIDTH;
    uint32_t height_zoom = (uint32_t)target_height * LV_IMG_ZOOM_NONE / CAMERA_THUMB_HEIGHT;

    if (zoom > height_zoom)
        zoom = height_zoom;
    lv_img_set_pivot(image, 0, 0);
    lv_img_set_zoom(image, (uint16_t)zoom);
    lv_obj_set_pos(image, 0, 0);
}

static void camera_ui_fit_photo(void)
{
    uint32_t viewport_height = CAMERA_UI_HEIGHT - CAMERA_UI_PHOTO_INFO_HEIGHT;
    uint32_t zoom = viewport_height * LV_IMG_ZOOM_NONE / CAMERA_LIVE_HEIGHT;
    uint32_t width = (CAMERA_LIVE_WIDTH * zoom + LV_IMG_ZOOM_NONE - 1U) / LV_IMG_ZOOM_NONE;
    uint32_t height = (CAMERA_LIVE_HEIGHT * zoom + LV_IMG_ZOOM_NONE - 1U) / LV_IMG_ZOOM_NONE;

    lv_img_set_pivot(s_full_image, 0, 0);
    lv_img_set_zoom(s_full_image, (uint16_t)zoom);
    lv_obj_set_pos(s_full_image, (CAMERA_UI_WIDTH - width) / 2U,
                   (viewport_height - height) / 2U);
}
static int camera_ui_detach_media(void)
{
    size_t index;

    for (index = 0U; index < CAMERA_UI_HOME_COUNT; ++index)
        lv_obj_add_flag(s_home_tiles[index], LV_OBJ_FLAG_HIDDEN);
    for (index = 0U; index < CAMERA_PAGE_SIZE; ++index)
        lv_obj_add_flag(s_gallery_tiles[index], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_full_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_photo_info, LV_OBJ_FLAG_HIDDEN);
    /* A media request may immediately overwrite every thumbnail/photo buffer. */
    if (drv_epic_wait_done() != RT_EOK)
        return -RT_ERROR;
    for (index = 0U; index < CAMERA_PAGE_SIZE; ++index)
    {
        lv_img_cache_invalidate_src(&s_thumb_dsc[index]);
        lv_img_set_src(s_gallery_images[index], &s_empty_image);
        if (index < CAMERA_UI_HOME_COUNT)
            lv_img_set_src(s_home_images[index], &s_empty_image);
    }
    lv_img_cache_invalidate_src(&s_full_image_dsc);
    lv_img_set_src(s_full_image, &s_empty_image);
    return 0;
}

static void camera_ui_release_live(void)
{
    lv_obj_add_flag(s_live_image, LV_OBJ_FLAG_HIDDEN);
    if (s_live_frame.slot < 0 || drv_epic_wait_done() != RT_EOK)
        return;
    lv_img_cache_invalidate_src(&s_live_image_dsc);
    lv_img_set_src(s_live_image, &s_empty_image);
    camera_service_release_frame(&s_live_frame);
    s_live_frame.slot = -1;
}

static void camera_ui_set_view(camera_view_t view)
{
    unsigned index;

    for (index = 0U; index < 3U; ++index)
    {
        if (index == (unsigned)view)
            lv_obj_clear_flag(s_roots[index], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_roots[index], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_view != view)
        camera_ui_diag_reset();
    s_view = view;
    if (s_display_started)
        camera_service_set_live(view == CAMERA_VIEW_HOME);
    if (view != CAMERA_VIEW_HOME)
        camera_ui_release_live();
    lv_group_focus_obj(view == CAMERA_VIEW_HOME ? s_capture_button :
                       (view == CAMERA_VIEW_GALLERY ? s_back_button : s_roots[view]));
}

static uint32_t camera_ui_token(void)
{
    ++s_next_token;
    if (s_next_token == 0U)
        ++s_next_token;
    return s_next_token;
}

static void camera_ui_request_page(uint32_t offset)
{
    uint32_t count = s_view == CAMERA_VIEW_HOME ? CAMERA_UI_HOME_COUNT
                                                : CAMERA_PAGE_SIZE;
    uint32_t token;
    int result;

    if (s_loading)
        return;
    if (camera_ui_detach_media() < 0)
    {
        camera_ui_status("读取失败");
        return;
    }
    token = camera_ui_token();
    result = camera_service_request_page(offset, count, token);
    if (result < 0)
    {
        camera_ui_status("读取失败");
        camera_ui_update_controls();
        return;
    }
    s_pending_kind = CAMERA_MEDIA_PAGE;
    s_pending_token = token;
    s_loading = 1;
    if (s_view == CAMERA_VIEW_GALLERY)
        s_gallery_offset = offset;
    camera_ui_status("加载中");
    camera_ui_update_controls();
}

static void camera_ui_capture_prompt(const char *text)
{
    lv_label_set_text(s_capture_message, text);
    lv_obj_center(s_capture_message);
    lv_obj_clear_flag(s_capture_overlay, LV_OBJ_FLAG_HIDDEN);
    s_capture_prompt_visible = 1;
}

static void camera_ui_capture_event(lv_event_t *event)
{
    (void)event;
    if (s_loading || s_view != CAMERA_VIEW_HOME)
        return;
    if (camera_ui_detach_media() < 0)
    {
        camera_ui_status("拍摄失败");
        return;
    }
    camera_ui_release_live();
    if (s_live_frame.slot >= 0)
    {
        camera_ui_status("拍摄失败");
        return;
    }
    s_pending_kind = CAMERA_MEDIA_CAPTURE;
    s_pending_token = camera_ui_token();
    s_loading = 1;
    s_capture_busy = 1;
    s_capture_pending = 1;
    s_capture_prompt_flushed = 0;
    s_capture_stage = CAMERA_CAPTURE_CAPTURING;
    camera_ui_diag_reset();
    camera_ui_capture_prompt("拍摄中...");
    camera_ui_status("");
    camera_ui_update_controls();
}

static void camera_ui_poll_capture(void)
{
    camera_capture_stage_t stage;

    if (!s_capture_busy)
        return;
    if (s_capture_pending)
    {
        /* The LCD must show the black prompt before capture reuses raw buffers.
         * Submit from the outer UI loop, after the completed full-screen flush. */
        if (!s_capture_prompt_flushed)
            return;
        s_capture_pending = 0;
        if (camera_service_capture(s_pending_token) < 0)
        {
            s_capture_busy = 0;
            s_loading = 0;
            s_after_page_status = NULL;
            s_refresh_home = 0;
            camera_ui_capture_prompt("加载中...");
            camera_ui_status("相机未就绪");
            camera_ui_update_controls();
            return;
        }
    }
    stage = camera_service_capture_stage();
    if (stage == s_capture_stage || stage == CAMERA_CAPTURE_IDLE)
        return;
    s_capture_stage = stage;
    camera_ui_capture_prompt(stage == CAMERA_CAPTURE_SAVING ? "保存中..." :
                             (stage == CAMERA_CAPTURE_RESTORING ? "加载中..." : "拍摄中..."));
}

static void camera_ui_gallery_event(lv_event_t *event)
{
    (void)event;
    if (s_loading)
        return;
    s_after_page_status = NULL;
    camera_ui_set_view(CAMERA_VIEW_GALLERY);
    camera_ui_request_page(0U);
}

static void camera_ui_home_event(lv_event_t *event)
{
    (void)event;
    if (s_loading)
        return;
    s_after_page_status = NULL;
    camera_ui_set_view(CAMERA_VIEW_HOME);
    camera_ui_request_page(0U);
}

static void camera_ui_page_event(lv_event_t *event)
{
    uintptr_t next = (uintptr_t)lv_event_get_user_data(event);
    uint32_t offset;

    if (s_loading || s_view != CAMERA_VIEW_GALLERY)
        return;
    if (next)
    {
        if (s_total <= s_gallery_offset ||
            s_total - s_gallery_offset <= CAMERA_PAGE_SIZE)
            return;
        offset = s_gallery_offset + CAMERA_PAGE_SIZE;
    }
    else
    {
        if (s_gallery_offset == 0U)
            return;
        offset = s_gallery_offset >= CAMERA_PAGE_SIZE
                     ? s_gallery_offset - CAMERA_PAGE_SIZE : 0U;
    }
    camera_ui_request_page(offset);
}

static void camera_ui_request_photo(uint32_t offset)
{
    uint32_t token;
    int result;

    if (s_loading)
        return;
    if (camera_ui_detach_media() < 0)
    {
        camera_ui_status("读取失败");
        return;
    }
    token = camera_ui_token();
    result = camera_service_request_photo_at(offset, token);
    if (result < 0)
    {
        camera_ui_status("读取失败");
        camera_ui_update_controls();
        return;
    }
    s_photo_offset = offset;
    s_pending_kind = CAMERA_MEDIA_PHOTO_INDEX;
    s_pending_token = token;
    s_loading = 1;
    camera_ui_status("加载中");
    camera_ui_update_controls();
}

static void camera_ui_thumbnail_event(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    uint32_t offset;

    if (s_loading || s_view == CAMERA_VIEW_PHOTO || index >= s_item_count)
        return;
    offset = (s_view == CAMERA_VIEW_HOME ? 0U : s_gallery_offset) + (uint32_t)index;
    s_photo_origin = s_view;
    s_photo_offset = offset;
    camera_ui_set_view(CAMERA_VIEW_PHOTO);
    camera_ui_request_photo(offset);
}

static void camera_ui_step_photo(int next)
{
    uint32_t offset = s_photo_offset;

    if (s_loading || s_view != CAMERA_VIEW_PHOTO || s_total == 0U || offset >= s_total)
        return;
    if (next)
    {
        if (offset == s_total - 1U)
            return;
        ++offset;
    }
    else
    {
        if (offset == 0U)
            return;
        --offset;
    }
    camera_ui_request_photo(offset);
}

static void camera_ui_photo_close_event(lv_event_t *event)
{
    (void)event;
    if (s_loading || s_view != CAMERA_VIEW_PHOTO)
        return;
    camera_ui_set_view(s_photo_origin);
    camera_ui_request_page(s_photo_origin == CAMERA_VIEW_HOME ? 0U :
                           s_photo_offset / CAMERA_PAGE_SIZE * CAMERA_PAGE_SIZE);
}

static lv_obj_t *camera_ui_tile(lv_obj_t *parent, int x, int y,
                                 int width, int height, size_t index,
                                 lv_obj_t **image)
{
    lv_obj_t *tile = lv_btn_create(parent);

    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, width, height);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tile, camera_ui_thumbnail_event, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    *image = lv_img_create(tile);
    lv_img_set_src(*image, &s_empty_image);
    lv_obj_clear_flag(*image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_HIDDEN);
    camera_ui_key_target(tile);
    return tile;
}

static void camera_ui_create(void)
{
    lv_obj_t *screen = lv_scr_act();
    size_t index;

    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    for (index = 0U; index < 3U; ++index)
        s_roots[index] = camera_ui_root(screen);
    s_live_image = lv_img_create(s_roots[CAMERA_VIEW_HOME]);
    lv_img_set_src(s_live_image, &s_empty_image);
    lv_obj_set_pos(s_live_image, 0, 0);
    lv_obj_add_flag(s_live_image, LV_OBJ_FLAG_HIDDEN);
    s_capture_overlay = camera_ui_root(s_roots[CAMERA_VIEW_HOME]);
    lv_obj_set_size(s_capture_overlay, CAMERA_LIVE_WIDTH, CAMERA_LIVE_HEIGHT);
    lv_obj_clear_flag(s_capture_overlay, LV_OBJ_FLAG_CLICKABLE);
    s_capture_message = camera_ui_label(s_capture_overlay, "", 0, 0, CAMERA_LIVE_WIDTH);
    lv_obj_clear_flag(s_capture_message, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_capture_message);
    lv_obj_add_flag(s_capture_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_roots[CAMERA_VIEW_HOME], camera_ui_home_background_event,
                       LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(s_roots[CAMERA_VIEW_HOME], camera_ui_home_background_event,
                       LV_EVENT_DRAW_MAIN_END, NULL);
    s_camera_status = camera_ui_label(s_roots[CAMERA_VIEW_HOME],
                                       "加载中", CAMERA_UI_CONTROL_X, 204,
                                       CAMERA_UI_CONTROL_WIDTH);
    s_capture_button = camera_ui_button(s_roots[CAMERA_VIEW_HOME], "拍摄",
                                         CAMERA_UI_CONTROL_X, 24,
                                         CAMERA_UI_CONTROL_WIDTH, 72,
                                         camera_ui_capture_event, 0U);
    s_gallery_button = camera_ui_button(s_roots[CAMERA_VIEW_HOME], "打开图库",
                                         CAMERA_UI_CONTROL_X, 112,
                                         CAMERA_UI_CONTROL_WIDTH, 72,
                                         camera_ui_gallery_event, 0U);
    s_status[CAMERA_VIEW_HOME] = camera_ui_label(s_roots[CAMERA_VIEW_HOME],
                                                 "", CAMERA_UI_CONTROL_X, 246,
                                                 CAMERA_UI_CONTROL_WIDTH);
    for (index = 0U; index < CAMERA_UI_HOME_COUNT; ++index)
        s_home_tiles[index] = camera_ui_tile(s_roots[CAMERA_VIEW_HOME],
                                              CAMERA_UI_CONTROL_X +
                                                  (int)(index % 2U) *
                                                  (CAMERA_UI_CONTROL_WIDTH - CAMERA_UI_HOME_THUMB_WIDTH),
                                              316 + (int)(index / 2U) * 92,
                                              CAMERA_UI_HOME_THUMB_WIDTH,
                                              CAMERA_UI_HOME_THUMB_HEIGHT, index,
                                              &s_home_images[index]);
    s_back_button = camera_ui_button(s_roots[CAMERA_VIEW_GALLERY], "返回",
                                      32, 24, 128, 56,
                                      camera_ui_home_event, 0U);
    s_page_label = camera_ui_label(s_roots[CAMERA_VIEW_GALLERY], "0 / 0",
                                    384, 36, 256);
    s_previous_button = camera_ui_button(s_roots[CAMERA_VIEW_GALLERY], "上一页",
                                          316, 524, 176, 56,
                                          camera_ui_page_event, 0U);
    s_next_button = camera_ui_button(s_roots[CAMERA_VIEW_GALLERY], "下一页",
                                      532, 524, 176, 56,
                                      camera_ui_page_event, 1U);
    s_status[CAMERA_VIEW_GALLERY] = camera_ui_label(s_roots[CAMERA_VIEW_GALLERY],
                                                    "", 212, 280, 600);
    for (index = 0U; index < CAMERA_PAGE_SIZE; ++index)
        s_gallery_tiles[index] = camera_ui_tile(s_roots[CAMERA_VIEW_GALLERY],
                                                 44 + (int)(index % 5U) * 194,
                                                 132 + (int)(index / 5U) * 220,
                                                 CAMERA_UI_GALLERY_THUMB_WIDTH,
                                                 CAMERA_UI_GALLERY_THUMB_HEIGHT, index,
                                                 &s_gallery_images[index]);
    s_full_image = lv_img_create(s_roots[CAMERA_VIEW_PHOTO]);
    lv_img_set_src(s_full_image, &s_empty_image);
    camera_ui_fit_photo();
    lv_obj_add_flag(s_full_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(s_full_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_roots[CAMERA_VIEW_PHOTO], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_roots[CAMERA_VIEW_PHOTO], camera_ui_photo_close_event,
                        LV_EVENT_CLICKED, NULL);
    s_status[CAMERA_VIEW_PHOTO] = camera_ui_label(s_roots[CAMERA_VIEW_PHOTO],
                                                  "", 212, 282, 600);
    lv_obj_clear_flag(s_status[CAMERA_VIEW_PHOTO], LV_OBJ_FLAG_CLICKABLE);
    s_photo_info = camera_ui_label(s_roots[CAMERA_VIEW_PHOTO], "", 0,
                                    CAMERA_UI_HEIGHT - CAMERA_UI_PHOTO_INFO_HEIGHT, CAMERA_UI_WIDTH);
    lv_obj_set_height(s_photo_info, CAMERA_UI_PHOTO_INFO_HEIGHT);
    lv_obj_set_style_bg_color(s_photo_info, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_photo_info, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_photo_info, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_photo_info, LV_OBJ_FLAG_HIDDEN);
    lv_group_add_obj(s_key_group, s_roots[CAMERA_VIEW_PHOTO]);
    camera_ui_set_view(CAMERA_VIEW_HOME);
}

static void camera_ui_update_controls(void)
{
    size_t index;
    int last_page = s_total <= s_gallery_offset ||
                   s_total - s_gallery_offset <= CAMERA_PAGE_SIZE;

    camera_ui_set_disabled(s_capture_button, s_loading);
    camera_ui_set_disabled(s_gallery_button, s_loading);
    camera_ui_set_disabled(s_back_button, s_loading);
    camera_ui_set_disabled(s_previous_button, s_loading || s_gallery_offset == 0U);
    camera_ui_set_disabled(s_next_button, s_loading || last_page);
    for (index = 0U; index < CAMERA_UI_HOME_COUNT; ++index)
        camera_ui_set_disabled(s_home_tiles[index], s_loading);
    for (index = 0U; index < CAMERA_PAGE_SIZE; ++index)
        camera_ui_set_disabled(s_gallery_tiles[index], s_loading);
    if (!s_loading)
        camera_ui_focus_available();
}

static void camera_ui_show_page(const camera_media_result_t *result)
{
    size_t index;
    size_t limit = s_view == CAMERA_VIEW_HOME ? CAMERA_UI_HOME_COUNT
                                              : CAMERA_PAGE_SIZE;
    uint32_t pages;
    char page_text[32];

    s_total = result->total;
    s_item_count = result->count > limit ? limit : result->count;
    for (index = 0U; index < s_item_count; ++index)
    {
        lv_obj_t *image = s_view == CAMERA_VIEW_HOME ? s_home_images[index]
                                                      : s_gallery_images[index];
        lv_obj_t *tile = s_view == CAMERA_VIEW_HOME ? s_home_tiles[index]
                                                     : s_gallery_tiles[index];

        if (result->thumbnails[index] == NULL)
            continue;
        camera_ui_image_desc(&s_thumb_dsc[index], result->thumbnails[index],
                              CAMERA_THUMB_WIDTH, CAMERA_THUMB_HEIGHT);
        lv_img_set_src(image, &s_thumb_dsc[index]);
        camera_ui_fit_thumbnail(image,
            s_view == CAMERA_VIEW_HOME ? CAMERA_UI_HOME_THUMB_WIDTH
                                       : CAMERA_UI_GALLERY_THUMB_WIDTH,
            s_view == CAMERA_VIEW_HOME ? CAMERA_UI_HOME_THUMB_HEIGHT
                                       : CAMERA_UI_GALLERY_THUMB_HEIGHT);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_view == CAMERA_VIEW_GALLERY)
    {
        s_gallery_offset = result->offset;
        pages = s_total / CAMERA_PAGE_SIZE + (s_total % CAMERA_PAGE_SIZE != 0U);
        rt_snprintf(page_text, sizeof(page_text), "%lu / %lu",
                    (unsigned long)(pages == 0U ? 0U
                                                : s_gallery_offset / CAMERA_PAGE_SIZE + 1U),
                    (unsigned long)pages);
        lv_label_set_text(s_page_label, page_text);
    }
    if (s_after_page_status != NULL && s_view == CAMERA_VIEW_HOME)
    {
        camera_ui_status(s_after_page_status);
        s_after_page_status = NULL;
    }
    else
        camera_ui_status(s_item_count == 0U ? "暂无照片" : "");
}

static void camera_ui_poll_media(void)
{
    camera_media_result_t result;

    if (camera_service_poll_result(&result) <= 0)
        return;
    if (!s_loading || result.token != s_pending_token ||
        result.kind != s_pending_kind)
        return;
    s_loading = 0;
    if (result.kind == CAMERA_MEDIA_PHOTO_INDEX)
        s_total = result.total;
    if (result.kind == CAMERA_MEDIA_CAPTURE)
    {
        /* Capture results are published only after preview restoration. */
        s_capture_busy = 0;
        s_capture_pending = 0;
        s_capture_prompt_flushed = 0;
        camera_ui_capture_prompt(result.preview_status == RT_EOK ? "加载中..." : "相机未就绪");
        s_after_page_status = result.status == 0 ? "已保存" : NULL;
        s_refresh_home = result.status == 0;
        camera_ui_status(result.status == 0 ? "已保存" : "拍摄失败");
    }
    else if (result.kind == CAMERA_MEDIA_ROTATE)
    {
        /* Saving settings may reclaim an old photo on a full volume. */
        s_after_page_status = result.status == RT_EOK ? NULL : "失败";
        s_refresh_home = 1;
        camera_ui_status(result.status == RT_EOK ? "" : "失败");
    }
    else if (result.status != 0)
    {
        s_item_count = 0U;
        camera_ui_status("读取失败");
    }
    else if (result.kind == CAMERA_MEDIA_PAGE)
        camera_ui_show_page(&result);
    else if (result.kind == CAMERA_MEDIA_PHOTO || result.kind == CAMERA_MEDIA_PHOTO_INDEX)
    {
        if (result.photo == NULL || result.width != CAMERA_LIVE_WIDTH ||
            result.height != CAMERA_LIVE_HEIGHT)
            camera_ui_status("读取失败");
        else
        {
            char metadata[64];

            rt_snprintf(metadata, sizeof(metadata), "%u x %u | JPEG | %lu KiB",
                        (unsigned)result.source_width, (unsigned)result.source_height,
                        (unsigned long)(result.file_size / 1024U));
            lv_label_set_text(s_photo_info, metadata);
            lv_obj_clear_flag(s_photo_info, LV_OBJ_FLAG_HIDDEN);
            camera_ui_image_desc(&s_full_image_dsc, result.photo,
                                  result.width, result.height);
            lv_img_set_src(s_full_image, &s_full_image_dsc);
            camera_ui_fit_photo();
            lv_obj_clear_flag(s_full_image, LV_OBJ_FLAG_HIDDEN);
            camera_ui_status("");
        }
    }
    camera_ui_update_controls();
}

static int camera_ui_key_target_available(lv_obj_t *object)
{
    return object != NULL && !lv_obj_has_state(object, LV_STATE_DISABLED) &&
           lv_obj_is_visible(object);
}

static void camera_ui_focus_available(void)
{
    if (!camera_ui_key_target_available(lv_group_get_focused(s_key_group)))
        lv_group_focus_next(s_key_group);
}

static void camera_ui_rotate(void)
{
    uint32_t token = camera_ui_token();

    /* Rotation warmup uses the same receive buffers as the live image. */
    camera_ui_release_live();
    if (s_live_frame.slot >= 0 || camera_service_rotate(token) != RT_EOK)
    {
        camera_ui_status("失败");
        return;
    }
    s_pending_kind = CAMERA_MEDIA_ROTATE;
    s_pending_token = token;
    s_loading = 1;
    camera_ui_status("加载中");
    camera_ui_update_controls();
}

static void camera_ui_poll_keys(void)
{
    camera_key_action_t action;

    /* All LVGL events run here, after the previous render and LCD swap.
     * Drain input before media completion so busy-time clicks are discarded. */
    while (camera_keys_poll(&action) > 0)
    {
        lv_obj_t *focused;

        if (s_loading)
            continue;
        if (action == CAMERA_KEY_BACK)
        {
            if (s_view == CAMERA_VIEW_PHOTO)
                camera_ui_photo_close_event(NULL);
            else if (s_view == CAMERA_VIEW_GALLERY)
                camera_ui_home_event(NULL);
            else
                camera_ui_rotate();
            continue;
        }
        if (s_view == CAMERA_VIEW_PHOTO &&
            (action == CAMERA_KEY_PREVIOUS || action == CAMERA_KEY_NEXT))
        {
            camera_ui_step_photo(action == CAMERA_KEY_NEXT);
            continue;
        }
        if (action == CAMERA_KEY_PREVIOUS)
            lv_group_focus_prev(s_key_group);
        else if (action == CAMERA_KEY_NEXT)
            lv_group_focus_next(s_key_group);
        else if (action == CAMERA_KEY_CONFIRM)
        {
            camera_ui_focus_available();
            focused = lv_group_get_focused(s_key_group);
            if (camera_ui_key_target_available(focused))
                lv_event_send(focused, LV_EVENT_CLICKED, NULL);
        }
    }
}

static void camera_ui_update_live(void)
{
    camera_live_frame_t next = {.slot = -1};
    lv_timer_t *refresh = _lv_disp_get_refr_timer(NULL);
    rt_tick_t update_begin;

    lv_timer_resume(refresh);
    if (s_view != CAMERA_VIEW_HOME)
    {
        camera_ui_release_live();
        return;
    }
    if (s_capture_busy)
        return;
    if (s_live_frame.slot >= 0)
    {
        lv_timer_ready(refresh);
        return;
    }
    update_begin = rt_tick_get();
    if (camera_service_take_frame(&next) <= 0)
    {
        const char *status = camera_service_camera_status() < 0
                                 ? "相机未就绪"
                                 : (lv_obj_has_flag(s_live_image, LV_OBJ_FLAG_HIDDEN) ? "加载中" : "");

        if (strcmp(lv_label_get_text(s_camera_status), status) != 0)
            lv_label_set_text(s_camera_status, status);
        if (s_capture_prompt_visible && camera_service_camera_status() < 0 &&
            strcmp(lv_label_get_text(s_capture_message), "相机未就绪") != 0)
            camera_ui_capture_prompt("相机未就绪");
        if (camera_service_camera_status() < 0)
            lv_obj_add_flag(s_live_image, LV_OBJ_FLAG_HIDDEN);
        else if (!lv_obj_has_flag(s_live_image, LV_OBJ_FLAG_HIDDEN))
            lv_timer_pause(refresh);
        return;
    }
    /* Select the newest completed DMA source immediately before rendering. */
    lv_img_cache_invalidate_src(&s_live_image_dsc);
    camera_ui_image_desc(&s_live_image_dsc, next.pixels,
                          CAMERA_LIVE_WIDTH, CAMERA_LIVE_HEIGHT);
    lv_img_set_src(s_live_image, &s_live_image_dsc);
    lv_obj_set_pos(s_live_image, 0, 0);
    lv_obj_clear_flag(s_live_image, LV_OBJ_FLAG_HIDDEN);
    s_live_frame = next;
    if (s_capture_prompt_visible)
    {
        lv_obj_add_flag(s_capture_overlay, LV_OBJ_FLAG_HIDDEN);
        s_capture_prompt_visible = 0;
    }
    if (lv_label_get_text(s_camera_status)[0] != '\0')
        lv_label_set_text(s_camera_status, "");
    lv_timer_ready(refresh);
    camera_ui_timing_record(&s_ui_diag.frame_update, update_begin);
}

typedef struct
{
    lv_area_t area;
    int valid;
} camera_ui_damage_t;

static camera_ui_damage_t s_display_damage[2];
static void (*s_lcd_rounder)(lv_disp_drv_t *, lv_area_t *);
static void (*s_lcd_render_start)(lv_disp_drv_t *);
static void (*s_lcd_flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
static void (*s_lcd_wait)(lv_disp_drv_t *);

static void camera_ui_damage_add(camera_ui_damage_t *damage, const lv_area_t *area)
{
    if (damage->valid)
        _lv_area_join(&damage->area, &damage->area, area);
    else
    {
        damage->area = *area;
        damage->valid = 1;
    }
}

static void camera_ui_round_area(lv_disp_drv_t *driver, lv_area_t *area)
{
    if (s_lcd_rounder != NULL)
        s_lcd_rounder(driver, area);

    /* Keep aligned damage before the SDK's RAMLESS area merger replaces
     * the LVGL invalid-area list with the full screen. Each physical LCD
     * buffer redraws these regions on its next use as the back buffer. */
    camera_ui_damage_add(&s_display_damage[0], area);
    camera_ui_damage_add(&s_display_damage[1], area);
}

static void camera_ui_render_start(lv_disp_drv_t *driver)
{
    lv_disp_t *display = _lv_refr_get_disp_refreshing();
    lv_disp_draw_buf_t *buffers = driver->draw_buf;
    unsigned buffer = buffers->buf_act == buffers->buf1 ? 0U : 1U;
    int last_area = -1;
    unsigned index;

    if (s_lcd_render_start != NULL)
        s_lcd_render_start(driver);
    RT_ASSERT(display != NULL && display->driver == driver);
    RT_ASSERT(buffers->buf_act == buffers->buf1 || buffers->buf_act == buffers->buf2);

    /* Use the saved damage, not the SDK-expanded invalid areas. */
    for (index = 0U; index < display->inv_p; ++index)
    {
        if (!display->inv_area_joined[index])
            last_area = (int)index;
    }
    RT_ASSERT(last_area >= 0);
    RT_ASSERT(s_display_damage[buffer].valid);
    if (s_view != CAMERA_VIEW_HOME)
    {
        lv_area_t screen = {0, 0, CAMERA_UI_WIDTH - 1, CAMERA_UI_HEIGHT - 1};

        camera_ui_damage_add(&s_display_damage[0], &screen);
        camera_ui_damage_add(&s_display_damage[1], &screen);
    }

    /* LVGL 8 computes the final area's index before render_start_cb. Keep
     * that index so one direct-mode flush completes and swaps the buffer. */
    for (index = 0U; index < display->inv_p; ++index)
        display->inv_area_joined[index] = 1;
    display->inv_area_joined[last_area] = 0;
    display->inv_areas[last_area] = s_display_damage[buffer].area;
    if (s_ui_diag.active && s_view == CAMERA_VIEW_HOME)
    {
        if (s_display_damage[buffer].area.x2 < CAMERA_LIVE_WIDTH)
            ++s_ui_diag.redraw_video_only;
        else
            ++s_ui_diag.redraw_sidebar;
    }
}

static void camera_ui_flush(lv_disp_drv_t *display, const lv_area_t *area,
                            lv_color_t *pixels)
{
    const lv_area_t *previous_clip = display->draw_ctx->clip_area;
    rt_tick_t flush_begin = rt_tick_get();
    int live = s_view == CAMERA_VIEW_HOME && s_live_frame.slot >= 0;
    uint32_t live_sequence = s_live_frame.sequence;
    unsigned buffer = pixels == display->draw_buf->buf1 ? 0U : 1U;

    RT_ASSERT(lv_disp_flush_is_last(display));
    RT_ASSERT(pixels == display->draw_buf->buf1 || pixels == display->draw_buf->buf2);
    /* Rendering has completed even when a stale camera source prevents this
     * buffer from being presented. Its static regions are already current. */
    s_display_damage[buffer].valid = 0;

    /* LVGL has waited for all GPU reads before invoking flush_cb. The LCD
     * scans the rendered display buffer, so the DCMI source is now free. */
    if (live)
    {
        int valid = camera_service_frame_is_valid(&s_live_frame);

        camera_service_release_frame(&s_live_frame);
        lv_timer_pause(_lv_disp_get_refr_timer(NULL));
        if (!valid)
        {
            lv_disp_draw_buf_t *buffers = display->draw_buf;

            ++s_ui_diag.source_expired;
            /* Keep the current LCD front buffer. LVGL toggles buf_act after
             * flush_cb; cancel that toggle so it renders into the same back
             * buffer next time instead of writing the LCD's front buffer. */
            buffers->buf_act = buffers->buf_act == buffers->buf1 ? buffers->buf2 : buffers->buf1;
            lv_disp_flush_ready(display);
            return;
        }
    }
    /* The SDK LCD adapter reads its window from the draw context. */
    display->draw_ctx->clip_area = display->draw_ctx->buf_area;
    mpu_dcache_clean(pixels, CAMERA_UI_WIDTH * CAMERA_UI_HEIGHT * sizeof(*pixels));
    s_lcd_flush(display, area, pixels);
    /* DPI keeps scanning the old buffer until the LCD swap completes. */
    while (display->draw_buf->flushing)
        s_lcd_wait(display);
    /* Includes cache maintenance, display submission, and LCD completion wait. */
    camera_ui_timing_record(&s_ui_diag.flush_total, flush_begin);
    display->draw_ctx->clip_area = previous_clip;
    if (s_capture_pending && s_view == CAMERA_VIEW_HOME && s_capture_prompt_visible)
        s_capture_prompt_flushed = 1;
    if (live && (!s_has_displayed_sequence || s_displayed_sequence != live_sequence))
    {
        s_displayed_sequence = live_sequence;
        s_has_displayed_sequence = 1;
        camera_service_record_displayed();
    }
}

static int camera_ui_configure_display(void)
{
    lv_disp_t *display = lv_disp_get_default();
    lv_disp_drv_t *driver;
    lv_disp_draw_buf_t *buffers;

    if (display == NULL || display->driver == NULL)
        return -RT_ERROR;
    driver = display->driver;
    buffers = driver->draw_buf;
    if (driver->hor_res != CAMERA_UI_WIDTH || driver->ver_res != CAMERA_UI_HEIGHT ||
        buffers == NULL || buffers->buf1 == NULL || buffers->buf2 == NULL ||
        buffers->size < CAMERA_UI_WIDTH * CAMERA_UI_HEIGHT ||
        driver->flush_cb == NULL || driver->wait_cb == NULL)
        return -RT_ERROR;

    s_lcd_rounder = driver->rounder_cb;
    s_lcd_render_start = driver->render_start_cb;
    s_lcd_flush = driver->flush_cb;
    s_lcd_wait = driver->wait_cb;
    lv_area_set(&s_display_damage[0].area, 0, 0, CAMERA_UI_WIDTH - 1, CAMERA_UI_HEIGHT - 1);
    s_display_damage[0].valid = 1;
    s_display_damage[1] = s_display_damage[0];
    driver->direct_mode = 1;
    driver->full_refresh = 0;
    driver->rounder_cb = camera_ui_round_area;
    driver->render_start_cb = camera_ui_render_start;
    driver->flush_cb = camera_ui_flush;
    return RT_EOK;
}

static void camera_ui_thread(void *parameter)
{
    rt_err_t result;

    (void)parameter;
    result = littlevgl2rtt_init("lcd");
    if (result != RT_EOK)
    {
        rt_kprintf("camera_ui: LVGL initialization failed %d\n", result);
        return;
    }
    result = camera_ui_configure_display();
    if (result != RT_EOK)
    {
        rt_kprintf("camera_ui: full-screen double buffer configuration failed\n");
        return;
    }
    lv_ex_data_pool_init();
    s_key_group = lv_group_create();
    if (s_key_group == NULL)
    {
        rt_kprintf("camera_ui: key focus group allocation failed\n");
        return;
    }
    lv_group_set_wrap(s_key_group, true);
    camera_ui_create();
    result = camera_keys_start();
    if (result != RT_EOK)
        rt_kprintf("camera_ui: ADC keys initialization failed %d\n", result);
    lv_refr_now(NULL);
    s_display_started = 1;
    camera_service_set_live(s_view == CAMERA_VIEW_HOME);
    camera_ui_request_page(0U);
    for (;;)
    {
        rt_tick_t handler_begin;
        rt_tick_t idle_begin;
        rt_uint32_t delay_ms;

        camera_ui_update_live();
        if (!s_ui_diag.active && s_view == CAMERA_VIEW_HOME && s_live_frame.slot >= 0)
        {
            s_ui_diag.begin = rt_tick_get();
            s_ui_diag.active = 1;
        }
        handler_begin = rt_tick_get();
        delay_ms = lv_task_handler();
        camera_ui_timing_record(&s_ui_diag.handler, handler_begin);

        camera_ui_poll_keys();
        camera_ui_poll_capture();
        camera_ui_poll_media();
        if (s_refresh_home && !s_loading && s_view == CAMERA_VIEW_HOME)
        {
            s_refresh_home = 0;
            camera_ui_request_page(0U);
        }
        camera_service_report_fps();
        camera_ui_diag_report();
        idle_begin = rt_tick_get();
        if (delay_ms == 0U)
            rt_thread_delay(1);
        else
        {
            if (delay_ms > CAMERA_UI_POLL_MS)
                delay_ms = CAMERA_UI_POLL_MS;
            rt_thread_mdelay(delay_ms);
        }
        camera_ui_timing_record(&s_ui_diag.idle, idle_begin);
    }
}

int camera_ui_start(void)
{
    rt_err_t result;

    if (s_thread != RT_NULL)
        return 0;
    s_thread = rt_thread_create("camera_ui", camera_ui_thread, RT_NULL,
                                CAMERA_UI_STACK_SIZE, CAMERA_UI_PRIORITY,
                                RT_THREAD_TICK_DEFAULT);
    if (s_thread == RT_NULL)
        return -RT_ENOMEM;
    result = rt_thread_startup(s_thread);
    if (result != RT_EOK)
    {
        rt_thread_delete(s_thread);
        s_thread = RT_NULL;
    }
    return result;
}