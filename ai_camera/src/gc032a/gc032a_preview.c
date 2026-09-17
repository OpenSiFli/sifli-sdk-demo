#include "gc032a_preview.h"

#include <bf0_hal.h>
#include <drv_epic.h>
#include <drv_lcd.h>
#include <mem_section.h>
#include <rtdevice.h>
#include <rtconfig.h>
#include <rtthread.h>
#include <string.h>

#include "device_ui.h"
#include "gc032a_camera.h"
#include "gc032a_preview_geometry.h"

#define GC032A_PREVIEW_WIDTH LCD_HOR_RES_MAX
#define GC032A_PREVIEW_HEIGHT LCD_VER_RES_MAX
/* The panel keeps scanning the LVGL framebuffer with a 2-byte-per-pixel
 * stride (1024 * 2 bytes per line). A 3-byte RGB888 preview buffer is read
 * with that same 2048-byte stride, which shifts every line by 1/3 of the
 * screen and shows the frame as three ghost images. Matching the scanned
 * stride with RGB565 output keeps the geometry correct. */
#define GC032A_PREVIEW_BUFFER_SIZE \
    (GC032A_PREVIEW_WIDTH * GC032A_PREVIEW_HEIGHT * 2U)
#define GC032A_PREVIEW_STACK_SIZE 4096U
#define GC032A_PREVIEW_PRIORITY 17U
#define GC032A_PREVIEW_WAIT_MS 2000U
/* A frame costs ~2.4 s of capture plus ~0.4 s of render, so stopping the
 * preview has to wait out a whole period before giving up on the thread. */
#define GC032A_PREVIEW_EXIT_MS 6000U

static uint8_t s_frame_buffer[GC032A_FRAME_SIZE]
    L2_RET_BSS_SECT(s_frame_buffer) __attribute__((aligned(64)));
L2_NON_RET_BSS_SECT_BEGIN(frambuf)
L2_NON_RET_BSS_SECT(frambuf,
                    ALIGN(64) static uint8_t s_display_buffer_psram2[
                        GC032A_PREVIEW_BUFFER_SIZE]);
L2_NON_RET_BSS_SECT_END
L2_NON_RET_BSS_SECT_BEGIN(gc032a_preview_buffer)
L2_NON_RET_BSS_SECT(gc032a_preview_buffer,
                    ALIGN(64) static uint8_t s_display_buffer_psram1[
                        GC032A_PREVIEW_BUFFER_SIZE]);
L2_NON_RET_BSS_SECT_END

static rt_device_t s_lcd;
static rt_thread_t s_thread;
static rt_sem_t s_exit_sem;
static struct rt_semaphore s_flush_sem;
static uint8_t *s_display_buffer;
static volatile rt_bool_t s_running;

/* Still-photo handover: the photo thread registers its own buffer, the preview
 * thread copies the next complete frame into it and immediately continues.  The
 * two threads therefore never share the capture buffer, and the live picture
 * only stutters for the duration of that single frame copy. */
static rt_mutex_t s_frame_lock;
static uint8_t *s_deliver_frame;
static volatile rt_bool_t s_deliver_pending;
static volatile int s_deliver_result;
static struct rt_completion s_deliver_done;

static rt_err_t gc032a_preview_flush_done(rt_device_t device, void *buffer)
{
    (void)device;
    (void)buffer;
    rt_sem_release(&s_flush_sem);
    return RT_EOK;
}

static int gc032a_preview_lcd_open(void)
{
    struct rt_device_graphic_info info;

    /* The RAMLESS DPI panel is scanned continuously from the framebuffer that
     * LVGL registered through drv_lcd_fb (buf2_1, RGB565). After
     * device_ui_pause() acknowledges, this preview takes over that scan-out by
     * flushing its own buffer with SF_GRAPHIC_CTRL_LCDC_FLUSH, which is the
     * same control the LVGL framebuffer path uses.
     * RTGRAPHIC_CTRL_SET_BUF_FORMAT must NOT be used here: it changes the
     * format of the whole layer, including the RGB565 buffer the panel keeps
     * scanning, which corrupts everything LVGL draws.
     * LVGL keeps the device open, so the reference count is left untouched. */
    s_lcd = rt_device_find("lcd");
    if (s_lcd == RT_NULL)
        return -1;
    if (rt_device_control(s_lcd, RTGRAPHIC_CTRL_GET_INFO, &info) != RT_EOK ||
        info.width != GC032A_PREVIEW_WIDTH ||
        info.height != GC032A_PREVIEW_HEIGHT)
        return -1;
    rt_sem_init(&s_flush_sem, "preview_flush", 0, RT_IPC_FLAG_FIFO);
    s_display_buffer = s_display_buffer_psram2;
    memset(s_display_buffer_psram2, 0, sizeof(s_display_buffer_psram2));
    memset(s_display_buffer_psram1, 0, sizeof(s_display_buffer_psram1));
    return 0;
}

static void gc032a_preview_lcd_close(void)
{
    if (s_lcd != RT_NULL)
        rt_device_set_tx_complete(s_lcd, RT_NULL);
    s_lcd = RT_NULL;
    s_display_buffer = RT_NULL;
    /* device_ui_resume() lets LVGL redraw every object, so its own framebuffer
     * scan-out and the RGB565 layer format are restored automatically. */
}

/* Hand the finished frame to the LCDC. The format travels with the flush
 * request; RGB565 matches the stride the panel already scans. */
static int gc032a_preview_submit(void)
{
    lcd_flush_info_t flush_info;

    memset(&flush_info, 0, sizeof(flush_info));
    flush_info.cmpr_rate = 0U;
    flush_info.pixel = s_display_buffer;
    flush_info.color_format = RTGRAPHIC_PIXEL_FORMAT_RGB565;
    flush_info.window.x0 = 0;
    flush_info.window.y0 = 0;
    flush_info.window.x1 = GC032A_PREVIEW_WIDTH - 1U;
    flush_info.window.y1 = GC032A_PREVIEW_HEIGHT - 1U;
    flush_info.pixel_area = flush_info.window;

    mpu_dcache_clean(s_display_buffer, GC032A_PREVIEW_BUFFER_SIZE);
    rt_device_set_tx_complete(s_lcd, gc032a_preview_flush_done);
    if (rt_device_control(s_lcd, SF_GRAPHIC_CTRL_LCDC_FLUSH, &flush_info) !=
        RT_EOK)
        return -1;
    if (rt_sem_take(&s_flush_sem,
                    rt_tick_from_millisecond(GC032A_PREVIEW_WAIT_MS)) !=
        RT_EOK)
        return -1;
    s_display_buffer = (s_display_buffer == s_display_buffer_psram2)
                           ? s_display_buffer_psram1
                           : s_display_buffer_psram2;
    return 0;
}

static int gc032a_preview_render(const gc032a_frame_t *frame)
{
    gc032a_preview_geometry_t geometry;
    EPIC_LayerConfigTypeDef input[2];
    EPIC_LayerConfigTypeDef output;

    if (frame == RT_NULL || frame->data == RT_NULL ||
        frame->size != GC032A_FRAME_SIZE ||
        gc032a_preview_geometry_calculate(GC032A_FRAME_WIDTH, 
                                          GC032A_FRAME_HEIGHT,
                                          GC032A_PREVIEW_WIDTH, 
                                          GC032A_PREVIEW_HEIGHT, &geometry) != 0)
        return -1;

    memset(s_display_buffer, 0, GC032A_PREVIEW_BUFFER_SIZE);

    HAL_EPIC_LayerConfigInit(&input[0]);
    input[0].data = s_display_buffer;
    input[0].color_mode = EPIC_COLOR_RGB565;
    input[0].width = GC032A_PREVIEW_WIDTH;
    input[0].total_width = GC032A_PREVIEW_WIDTH;
    input[0].height = GC032A_PREVIEW_HEIGHT;
    input[0].data_size = GC032A_PREVIEW_BUFFER_SIZE;
    input[0].alpha = 255U;

    HAL_EPIC_LayerConfigInit(&input[1]);
    input[1].data = (uint8_t *)frame->data;
    input[1].color_mode = EPIC_INPUT_RGB565_SWAP;
    input[1].width = GC032A_FRAME_WIDTH;
    input[1].total_width = GC032A_FRAME_WIDTH;
    input[1].height = GC032A_FRAME_HEIGHT;
    input[1].data_size = GC032A_FRAME_SIZE;
    input[1].alpha = 255U;
    input[1].transform_cfg.angle = geometry.angle_tenths;
    input[1].transform_cfg.scale_x = geometry.epic_scale;
    input[1].transform_cfg.scale_y = geometry.epic_scale;
    input[1].transform_cfg.pivot_x = GC032A_FRAME_WIDTH / 2U;
    input[1].transform_cfg.pivot_y = GC032A_FRAME_HEIGHT / 2U;
    input[1].x_offset = (int16_t)geometry.crop_x;
    input[1].y_offset = (int16_t)geometry.crop_y;

    HAL_EPIC_LayerConfigInit(&output);
    output.data = s_display_buffer;
    output.color_mode = EPIC_COLOR_RGB565;
    output.width = GC032A_PREVIEW_WIDTH;
    output.total_width = GC032A_PREVIEW_WIDTH;
    output.height = GC032A_PREVIEW_HEIGHT;
    output.data_size = GC032A_PREVIEW_BUFFER_SIZE;
    output.alpha = 255U;

    /* The frame buffer was filled by the DCMI DMA, so drop any stale cache
     * lines before EPIC reads the pixels back from memory. */
    mpu_dcache_invalidate((void *)frame->data, frame->size);
    if (drv_epic_blend(input, 2, &output, RT_NULL) != RT_EOK ||
        drv_epic_wait_done() != RT_EOK)
        return -1;
    mpu_dcache_invalidate(s_display_buffer, GC032A_PREVIEW_BUFFER_SIZE);
    return gc032a_preview_submit();
}

/* Both helpers run on the preview thread and serve the still photo that
 * registered a buffer with gc032a_preview_deliver_frame(). */
static void gc032a_preview_deliver_to_waiter(void)
{
    if (rt_mutex_take(s_frame_lock, RT_WAITING_FOREVER) != RT_EOK)
        return;
    if (!s_deliver_pending || s_deliver_frame == RT_NULL)
    {
        rt_mutex_release(s_frame_lock);
        return;
    }
    /* One frame copy: the preview stutters for exactly this long. */
    memcpy(s_deliver_frame, s_frame_buffer, GC032A_FRAME_SIZE);
    s_deliver_result = 0;
    s_deliver_pending = RT_FALSE;
    rt_mutex_release(s_frame_lock);
    rt_completion_done(&s_deliver_done);
}

static void gc032a_preview_fail_waiter(void)
{
    if (s_frame_lock == RT_NULL)
        return;
    if (rt_mutex_take(s_frame_lock, RT_WAITING_FOREVER) != RT_EOK)
        return;
    if (!s_deliver_pending)
    {
        rt_mutex_release(s_frame_lock);
        return;
    }
    s_deliver_result = -1;
    s_deliver_pending = RT_FALSE;
    rt_mutex_release(s_frame_lock);
    rt_completion_done(&s_deliver_done);
}

static int gc032a_preview_stream_start(void)
{
    return gc032a_camera_start(s_frame_buffer, RT_NULL, GC032A_FRAME_SIZE);
}

static void gc032a_preview_thread(void *parameter)
{
    unsigned int failures = 0U;
    unsigned int displayed = 0U;

    (void)parameter;
    while (s_running)
    {
        gc032a_frame_t frame;
        int result = gc032a_camera_wait_frame(
            &frame, rt_tick_from_millisecond(GC032A_PREVIEW_WAIT_MS));

        if (result == GC032A_CAMERA_OK)
        {
            /* A waiting still photo gets this complete frame before anything
             * else happens; the preview then carries on as usual. */
            gc032a_preview_deliver_to_waiter();
            /* The serial front end already delivers RGB565 and the frame is
             * complete when wait_frame returns, so EPIC consumes the capture
             * buffer directly. */
            result = gc032a_camera_stop();
            if (result == GC032A_CAMERA_OK)
                result = gc032a_preview_render(&frame);
            if (result == GC032A_CAMERA_OK && s_running)
                result = gc032a_preview_stream_start();
        }
        if (result != GC032A_CAMERA_OK)
        {
            failures++;
            rt_kprintf("preview: frame error %d count=%u\n", result,
                       failures);
            (void)gc032a_camera_stop();
            if (!s_running)
                break;
            if (gc032a_camera_recover() != GC032A_CAMERA_OK ||
                gc032a_preview_stream_start() != GC032A_CAMERA_OK)
                rt_thread_mdelay(100U);
        }
        else
        {
            failures = 0U;
            displayed++;
            if (displayed == 1U || displayed % 100U == 0U)
                rt_kprintf("preview: displayed=%u sequence=%u\n",
                           displayed, frame.sequence);
        }
    }
    (void)gc032a_camera_stop();
    gc032a_preview_fail_waiter();
    gc032a_preview_lcd_close();
    device_ui_resume();
    rt_sem_release(s_exit_sem);
}

int gc032a_preview_start(void)
{
    int result;

    if (s_running)
        return GC032A_CAMERA_BUSY;
    if (s_frame_lock == RT_NULL)
    {
        s_frame_lock = rt_mutex_create("cam_frame", RT_IPC_FLAG_PRIO);
        if (s_frame_lock == RT_NULL)
            return GC032A_CAMERA_ERROR;
    }
    s_deliver_frame = RT_NULL;
    s_deliver_pending = RT_FALSE;
    if (device_ui_pause() != 0)
        return GC032A_CAMERA_ERROR;
    rt_thread_mdelay(40U);
    if (gc032a_preview_lcd_open() != 0)
    {
        gc032a_preview_lcd_close();
        device_ui_resume();
        return GC032A_CAMERA_ERROR;
    }
    result = gc032a_preview_stream_start();
    if (result != GC032A_CAMERA_OK)
    {
        gc032a_preview_lcd_close();
        device_ui_resume();
        return result;
    }
    if (s_exit_sem == RT_NULL)
        s_exit_sem = rt_sem_create("cam_prev", 0, RT_IPC_FLAG_FIFO);
    if (s_exit_sem == RT_NULL)
    {
        (void)gc032a_camera_stop();
        gc032a_preview_lcd_close();
        device_ui_resume();
        return GC032A_CAMERA_ERROR;
    }
    s_running = RT_TRUE;
    s_thread = rt_thread_create("cam_prev", gc032a_preview_thread, RT_NULL,
                                GC032A_PREVIEW_STACK_SIZE,
                                GC032A_PREVIEW_PRIORITY, 10U);
    if (s_thread == RT_NULL)
    {
        s_running = RT_FALSE;
        (void)gc032a_camera_stop();
        gc032a_preview_lcd_close();
        device_ui_resume();
        return GC032A_CAMERA_ERROR;
    }
    rt_thread_startup(s_thread);
    rt_kprintf("preview: started 640x480 RGB565 -> 1024x600 RGB565 center "
               "via lcdc flush\n");
    return GC032A_CAMERA_OK;
}

int gc032a_preview_is_running(void)
{
    return s_running ? 1 : 0;
}

/* Hand the next complete 640x480 RGB565 frame to dst, which must hold
 * GC032A_FRAME_SIZE bytes and must not be reused until this call returns.  The
 * preview thread does the copy right after the frame is captured and keeps
 * running, so the live picture only stutters for that one copy. */
int gc032a_preview_deliver_frame(uint8_t *dst, uint32_t capacity,
                                 uint32_t timeout_ms)
{
    rt_err_t wait_result;

    if (dst == RT_NULL || capacity < GC032A_FRAME_SIZE ||
        s_frame_lock == RT_NULL)
        return GC032A_CAMERA_INVALID;
    rt_completion_init(&s_deliver_done);
    if (rt_mutex_take(s_frame_lock, RT_WAITING_FOREVER) != RT_EOK)
        return GC032A_CAMERA_ERROR;
    if (!s_running)
    {
        rt_mutex_release(s_frame_lock);
        return GC032A_CAMERA_ERROR;
    }
    if (s_deliver_pending)
    {
        rt_mutex_release(s_frame_lock);
        return GC032A_CAMERA_BUSY;
    }
    s_deliver_frame = dst;
    s_deliver_result = -1;
    s_deliver_pending = RT_TRUE;
    rt_mutex_release(s_frame_lock);

    wait_result = rt_completion_wait(&s_deliver_done,
                                     rt_tick_from_millisecond(timeout_ms));
    if (wait_result != RT_EOK)
    {
        /* Cancel, so the preview thread cannot copy into a buffer the caller
         * has already given up on. */
        if (rt_mutex_take(s_frame_lock, RT_WAITING_FOREVER) == RT_EOK)
        {
            s_deliver_pending = RT_FALSE;
            s_deliver_frame = RT_NULL;
            rt_mutex_release(s_frame_lock);
        }
        return GC032A_CAMERA_TIMEOUT;
    }
    s_deliver_frame = RT_NULL;
    return (s_deliver_result == 0) ? GC032A_CAMERA_OK : GC032A_CAMERA_ERROR;
}

int gc032a_preview_stop(void)
{
    if (!s_running)
        return GC032A_CAMERA_OK;
    s_running = RT_FALSE;
    if (rt_sem_take(s_exit_sem,
                    rt_tick_from_millisecond(GC032A_PREVIEW_EXIT_MS)) != RT_EOK)
    {
        /* The frame in flight owns the display, so never leave the UI paused
         * waiting for a thread that is late: the thread still finishes the
         * handover of the panel when it does exit. */
        rt_kprintf("preview: stop timed out, resuming UI\n");
        device_ui_resume();
        return GC032A_CAMERA_TIMEOUT;
    }
    s_thread = RT_NULL;
    rt_kprintf("preview: stopped\n");
    return GC032A_CAMERA_OK;
}

int gc032a_preview_recover(void)
{
    int result = gc032a_preview_stop();

    if (result != GC032A_CAMERA_OK)
        return result;
    return gc032a_preview_start();
}

static void gc032a_preview_start_command(void)
{
    rt_kprintf("preview: start result=%d\n", gc032a_preview_start());
}
MSH_CMD_EXPORT_ALIAS(gc032a_preview_start_command, gc032a_preview_start,
                     Start GC032A live preview);

static void gc032a_preview_stop_command(void)
{
    rt_kprintf("preview: stop result=%d\n", gc032a_preview_stop());
}
MSH_CMD_EXPORT_ALIAS(gc032a_preview_stop_command, gc032a_preview_stop,
                     Stop GC032A live preview);

static void gc032a_preview_recover_command(void)
{
    rt_kprintf("preview: recover result=%d\n", gc032a_preview_recover());
}
MSH_CMD_EXPORT_ALIAS(gc032a_preview_recover_command, gc032a_preview_recover,
                     Recover GC032A live preview);
