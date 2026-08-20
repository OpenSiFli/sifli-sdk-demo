#include "camera_app.h"

#include <rtconfig.h>
#include <cpu_usage_profiler.h>
#include <finsh.h>
#include <mem_section.h>
#include <rthw.h>
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "../image_processing/camera_image_processor.h"
#include "camera_metrics.h"
#ifdef CAMERA_SENSOR_ARDUCAM_OV2640
#include "../camera/arducam_backend.h"
#define CAMERA_SOURCE_SLOT_SIZE ARDUCAM_PREVIEW_SLOT_SIZE
#define CAMERA_SOURCE_WIDTH ARDUCAM_PREVIEW_WIDTH
#define CAMERA_SOURCE_HEIGHT ARDUCAM_PREVIEW_HEIGHT
#define CAMERA_SOURCE_FORMAT CAMERA_PIXEL_JPEG
#else
#include "../camera/gc032a_backend.h"
#include "../camera/gc032a_regs.h"
#define CAMERA_SOURCE_SLOT_SIZE GC032A_FRAME_SIZE
#define CAMERA_SOURCE_WIDTH 640U
#define CAMERA_SOURCE_HEIGHT 480U
#define CAMERA_SOURCE_FORMAT CAMERA_PIXEL_RGB565_BE
#endif
#include "../camera/frame_pool.h"
#include "../display/camera_display.h"
#include "../display/photo_decoder.h"
#include "../input/camera_keys.h"
#include "../storage/photo_storage.h"

#define CAMERA_APP_EVENT_FRAME (1U << 0)
#define CAMERA_APP_EVENT_SHUTTER (1U << 1)
#define CAMERA_APP_EVENT_PAUSED (1U << 2)
#define CAMERA_APP_EVENT_RESUME (1U << 3)
#define CAMERA_APP_EVENT_ALBUM (1U << 4)
#define CAMERA_APP_EVENT_NEXT (1U << 5)
#define CAMERA_APP_STATS_INTERVAL_MS 5000U
#define CAMERA_APP_STATS_WINDOW_MS 5000U

L2_NON_RET_BSS_SECT_BEGIN(camera_app_buffers)
L2_NON_RET_BSS_SECT(camera_app_buffers,
                    ALIGN(64) static uint8_t camera_dma_buffers[2][CAMERA_SOURCE_SLOT_SIZE]);
L2_NON_RET_BSS_SECT(camera_app_buffers,
                    ALIGN(64) static uint8_t camera_stable_buffers[2][CAMERA_SOURCE_SLOT_SIZE]);
L2_NON_RET_BSS_SECT(camera_app_buffers,
                    ALIGN(64) static uint8_t camera_preview_buffer[CAMERA_DISPLAY_BUFFER_SIZE]);
L2_NON_RET_BSS_SECT_END

static frame_pool_t camera_pool;
static struct rt_mutex camera_pool_lock;
static struct rt_event camera_events;
static rt_thread_t capture_thread;
static rt_thread_t display_thread;
static volatile rt_bool_t camera_running;
static volatile rt_bool_t camera_saving;
static volatile rt_bool_t shutter_busy;
static rt_bool_t pause_requested;
static rt_bool_t album_active;
static uint16_t album_index;
static uint32_t captured_frames;
static uint32_t displayed_frames;
static uint32_t dropped_frames;
static uint32_t failed_frames;
static uint32_t capture_period_ticks;
static uint32_t capture_period_samples;
static uint32_t copy_ticks;
static uint32_t copied_frames;
static uint32_t display_ticks;
static int last_capture_error;

typedef struct
{
    uint32_t captured;
    uint32_t displayed;
    uint32_t dropped;
    uint32_t failed;
    uint32_t decode_samples;
    uint32_t decode_total_ms;
    uint32_t decode_max_ms;
    uint32_t encode_samples;
    uint32_t encode_total_ms;
    uint32_t encode_max_ms;
} camera_window_stats_t;

static camera_window_stats_t camera_window;

static const camera_source_ops_t *camera_app_source(void)
{
#ifdef CAMERA_SENSOR_ARDUCAM_OV2640
    return arducam_backend_ops();
#else
    return gc032a_backend_ops();
#endif
}

static void camera_app_shutter(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (!shutter_busy)
    {
        shutter_busy = RT_TRUE;
        rt_event_send(&camera_events, CAMERA_APP_EVENT_SHUTTER);
    }
    rt_hw_interrupt_enable(level);
}

static void camera_app_album(void)
{
    rt_event_send(&camera_events, CAMERA_APP_EVENT_ALBUM);
}

static void camera_app_next(void)
{
    rt_event_send(&camera_events, CAMERA_APP_EVENT_NEXT);
}

static void camera_capture_thread(void *parameter)
{
    const camera_source_ops_t *source = camera_app_source();
    rt_bool_t stream_running = RT_TRUE;
    rt_tick_t last_capture_tick = 0;

    (void)parameter;
    while (camera_running)
    {
        camera_stream_frame_t dma_frame;
        rt_bool_t pause;
        int result;

        if (!stream_running)
        {
            result = source->stream_start((uint8_t *)camera_dma_buffers,
                                          sizeof(camera_dma_buffers));
            if (result != 0)
            {
                rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
                failed_frames++;
                pause = pause_requested;
                rt_mutex_release(&camera_pool_lock);
                if (pause)
                {
                    rt_event_send(&camera_events, CAMERA_APP_EVENT_PAUSED);
                    rt_event_recv(&camera_events, CAMERA_APP_EVENT_RESUME,
                                  RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                                  RT_WAITING_FOREVER, RT_NULL);
                }
                else
                {
                    rt_thread_mdelay(100);
                }
                continue;
            }
            stream_running = RT_TRUE;
            last_capture_tick = 0;
        }

        result = source->stream_wait(&dma_frame, 2000U);
        if (result == 0)
        {
            rt_tick_t now = rt_tick_get();

            rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
            captured_frames++;
            camera_window.captured++;
            last_capture_error = 0;
            if (last_capture_tick != 0)
            {
                capture_period_ticks += now - last_capture_tick;
                capture_period_samples++;
            }
            last_capture_tick = now;
            pause = pause_requested;
            rt_mutex_release(&camera_pool_lock);
        }
        else
        {
            source->stream_stop();
            stream_running = RT_FALSE;
            last_capture_tick = 0;
            rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
            failed_frames++;
            last_capture_error = result;
            pause = pause_requested;
            rt_mutex_release(&camera_pool_lock);
        }

        if (pause && stream_running)
        {
            source->stream_stop();
            stream_running = RT_FALSE;
        }

        if (result == 0)
        {
            uint8_t *destination;
            uint8_t slot;
            int store_result;

            rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
            store_result = frame_pool_begin_capture(&camera_pool, &destination,
                                                    &slot);
            rt_mutex_release(&camera_pool_lock);
            if (store_result == 0)
            {
                rt_tick_t copy_start = rt_tick_get();
                rt_tick_t elapsed;

                memcpy(destination, dma_frame.data, dma_frame.size);
                elapsed = rt_tick_get() - copy_start;
                rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
                store_result = frame_pool_complete_capture(
                    &camera_pool, slot, dma_frame.size, dma_frame.sequence);
                if (store_result == 0)
                {
                    copy_ticks += elapsed;
                    copied_frames++;
                }
                else
                {
                    frame_pool_cancel_capture(&camera_pool, slot);
                    failed_frames++;
                }
                rt_mutex_release(&camera_pool_lock);
                if (store_result == 0)
                    rt_event_send(&camera_events, CAMERA_APP_EVENT_FRAME);
            }
            else
            {
                rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
                dropped_frames++;
                camera_window.dropped++;
                rt_mutex_release(&camera_pool_lock);
            }
        }

        if (pause)
        {
            rt_event_send(&camera_events, CAMERA_APP_EVENT_PAUSED);
            rt_event_recv(&camera_events, CAMERA_APP_EVENT_RESUME,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, RT_NULL);
        }
        else if (result != 0)
        {
            rt_thread_mdelay(100);
        }
    }

    if (stream_running)
        source->stream_stop();
}

static int camera_app_acquire_frame(camera_frame_t *frame)
{
    int result;

    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    result = frame_pool_acquire_latest(&camera_pool, frame);
    rt_mutex_release(&camera_pool_lock);
    return result;
}

static void camera_app_release_frame(camera_frame_t *frame)
{
    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    frame_pool_release(&camera_pool, frame);
    rt_mutex_release(&camera_pool_lock);
}

static void camera_app_pause_stream(void)
{
    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    pause_requested = RT_TRUE;
    rt_mutex_release(&camera_pool_lock);
    rt_event_recv(&camera_events, CAMERA_APP_EVENT_PAUSED,
                  RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                  RT_WAITING_FOREVER, RT_NULL);
}

static void camera_app_resume_stream(void)
{
    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    pause_requested = RT_FALSE;
    rt_mutex_release(&camera_pool_lock);
    rt_event_send(&camera_events, CAMERA_APP_EVENT_RESUME);
}

static void camera_app_save_photo(void)
{
    const camera_source_ops_t *source = camera_app_source();
    camera_frame_t frame;
    char path[PHOTO_STORAGE_PATH_SIZE];
    uint32_t saved_size = 0U;
    int result;

    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    camera_saving = RT_TRUE;
    rt_mutex_release(&camera_pool_lock);
    camera_app_pause_stream();

    if (source->capture_still != RT_NULL)
    {
        result = photo_storage_save_jpeg(source->capture_still, path,
                                         sizeof(path), &saved_size);
        if (result == PHOTO_STORAGE_OK)
            rt_kprintf("camera: saved %s (%u bytes)\n", path, saved_size);
        else
            rt_kprintf("camera: save failed stage=%d errno=%d\n",
                       result, rt_get_errno());
    }
    else if (camera_app_acquire_frame(&frame) == 0)
    {
        camera_display_show(&frame, RT_NULL);
#ifdef CAMERA_SENSOR_GC032A
        const uint8_t *jpeg = RT_NULL;
        uint32_t jpeg_size = 0U;
        uint32_t encode_ms = 0U;
        uint32_t write_ms = 0U;

        result = camera_image_encode_rgb565_be(
            frame.data, frame.width, frame.height, frame.width * 2U,
            &jpeg, &jpeg_size, &encode_ms);
        if (result == 0)
        {
            rt_tick_t write_started;

            rt_kprintf("camera: encoded jpeg %ux%u size=%u elapsed=%u ms\n",
                       frame.width, frame.height, jpeg_size, encode_ms);
            rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
            camera_window.encode_samples++;
            camera_window.encode_total_ms += encode_ms;
            if (encode_ms > camera_window.encode_max_ms)
                camera_window.encode_max_ms = encode_ms;
            rt_mutex_release(&camera_pool_lock);
            write_started = rt_tick_get();
            result = photo_storage_save_jpeg_data(
                jpeg, jpeg_size, path, sizeof(path));
            write_ms = (uint32_t)((rt_tick_get() - write_started) * 1000U /
                                  RT_TICK_PER_SECOND);
        }
        if (result == PHOTO_STORAGE_OK)
            rt_kprintf("camera: saved %s (%u bytes) encode=%u write=%u ms\n",
                       path, jpeg_size, encode_ms, write_ms);
        else
            rt_kprintf("camera: save failed stage=%d errno=%d\n",
                       result, rt_get_errno());
#else
        result = photo_storage_save_bmp(&frame, path, sizeof(path));
        if (result == PHOTO_STORAGE_OK)
            rt_kprintf("camera: saved %s (921654 bytes)\n", path);
        else
            rt_kprintf("camera: save failed stage=%d errno=%d\n",
                       result, rt_get_errno());
#endif
        camera_app_release_frame(&frame);
    }
    else
    {
        rt_kprintf("camera: shutter frame unavailable\n");
    }

    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    camera_saving = RT_FALSE;
    shutter_busy = RT_FALSE;
    rt_mutex_release(&camera_pool_lock);
    camera_app_resume_stream();
}

static const char *camera_app_filename(const char *path)
{
    return path[0] == '/' ? path + 1 : path;
}

static void camera_app_show_album_photo(rt_bool_t next)
{
    char path[PHOTO_STORAGE_PATH_SIZE];
    uint32_t jpeg_size = 0U;
    int result;

    if (next)
        result = photo_storage_find_next_jpeg(album_index, path,
                                              sizeof(path), &album_index);
    else
        result = photo_storage_find_latest_jpeg(path, sizeof(path),
                                                &album_index);
    if (result == PHOTO_STORAGE_ERROR_EMPTY)
    {
        camera_display_show_message("SD CARD", "NO PHOTOS");
        rt_kprintf("camera: album empty\n");
        return;
    }
    if (result != PHOTO_STORAGE_OK)
    {
        camera_display_show_message("SD CARD", "READ ERROR");
        rt_kprintf("camera: album scan failed stage=%d errno=%d\n",
                   result, rt_get_errno());
        return;
    }

    result = photo_storage_load_jpeg(path, (uint8_t *)camera_dma_buffers,
                                     sizeof(camera_dma_buffers), &jpeg_size);
    if (result != PHOTO_STORAGE_OK)
    {
        camera_display_show_message(camera_app_filename(path),
                                    result == PHOTO_STORAGE_ERROR_TOO_LARGE ?
                                    "UNSUPPORTED" : "READ ERROR");
        rt_kprintf("camera: album load failed file=%s stage=%d errno=%d\n",
                   path, result, rt_get_errno());
        return;
    }
    uint32_t decode_ms = 0U;
    result = camera_display_show_photo((const uint8_t *)camera_dma_buffers,
                                       jpeg_size,
                                       camera_app_filename(path),
                                       &decode_ms);
    if (result == PHOTO_DECODER_UNSUPPORTED)
        camera_display_show_message(camera_app_filename(path), "UNSUPPORTED");
    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    if (result == 0)
    {
        camera_window.decode_samples++;
        camera_window.decode_total_ms += decode_ms;
        if (decode_ms > camera_window.decode_max_ms)
            camera_window.decode_max_ms = decode_ms;
    }
    rt_mutex_release(&camera_pool_lock);
    rt_kprintf("camera: album file=%s size=%u decoder=%s result=%d\n",
               path, jpeg_size, photo_decoder_backend()->name, result);
}

static void camera_app_toggle_album(void)
{
    if (!album_active)
    {
        camera_app_pause_stream();
        album_active = RT_TRUE;
        album_index = 0U;
        camera_app_show_album_photo(RT_FALSE);
        rt_kprintf("camera: album opened, KEY2 preview KEY3 next\n");
    }
    else
    {
        album_active = RT_FALSE;
        camera_app_resume_stream();
        rt_kprintf("camera: preview resumed\n");
    }
}

typedef struct
{
    rt_tick_t tick;
    uint32_t displayed;
    uint32_t period_ticks;
    uint32_t period_samples;
    uint32_t copy_ticks;
    uint32_t copied;
    uint32_t display_ticks;
} camera_stats_checkpoint_t;

static void camera_app_print_stats(camera_stats_checkpoint_t *last)
{
    rt_tick_t now = rt_tick_get();
    uint32_t elapsed_ms = (uint32_t)((now - last->tick) * 1000U /
                                     RT_TICK_PER_SECOND);

    if (elapsed_ms >= CAMERA_APP_STATS_INTERVAL_MS)
    {
        uint32_t captured;
        uint32_t displayed;
        uint32_t dropped;
        uint32_t failed;
        uint32_t period_total;
        uint32_t period_count;
        uint32_t copy_total;
        uint32_t copy_count;
        uint32_t display_total;
        uint32_t frames;
        uint32_t fps_x10;
        uint32_t period_ms_x10;
        uint32_t copy_ms_x10;
        uint32_t display_ms_x10;
        int capture_error;

        rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
        captured = captured_frames;
        displayed = displayed_frames;
        dropped = dropped_frames;
        failed = failed_frames;
        period_total = capture_period_ticks;
        period_count = capture_period_samples;
        copy_total = copy_ticks;
        copy_count = copied_frames;
        display_total = display_ticks;
        capture_error = last_capture_error;
        rt_mutex_release(&camera_pool_lock);

        frames = displayed - last->displayed;
        fps_x10 = elapsed_ms == 0U ? 0U : frames * 10000U / elapsed_ms;
        period_ms_x10 = camera_metrics_average_ms_x10(
            period_total - last->period_ticks,
            period_count - last->period_samples, RT_TICK_PER_SECOND);
        copy_ms_x10 = camera_metrics_average_ms_x10(
            copy_total - last->copy_ticks,
            copy_count - last->copied, RT_TICK_PER_SECOND);
        display_ms_x10 = camera_metrics_average_ms_x10(
            display_total - last->display_ticks, frames, RT_TICK_PER_SECOND);

        rt_kprintf("camera: captured=%u displayed=%u dropped=%u failed=%u "
                   "fps=%u.%u period=%u.%u copy=%u.%u display=%u.%u ms "
                   "last_error=%d\n",
                   captured, displayed, dropped, failed,
                   fps_x10 / 10U, fps_x10 % 10U,
                   period_ms_x10 / 10U, period_ms_x10 % 10U,
                   copy_ms_x10 / 10U, copy_ms_x10 % 10U,
                   display_ms_x10 / 10U, display_ms_x10 % 10U,
                   capture_error);
        *last = (camera_stats_checkpoint_t){
            .tick = now,
            .displayed = displayed,
            .period_ticks = period_total,
            .period_samples = period_count,
            .copy_ticks = copy_total,
            .copied = copy_count,
            .display_ticks = display_total,
        };
    }
}

static void camera_display_thread(void *parameter)
{
    camera_stats_checkpoint_t last = {.tick = rt_tick_get()};

    (void)parameter;
    while (camera_running)
    {
        rt_uint32_t events = 0;

        rt_event_recv(&camera_events,
                      CAMERA_APP_EVENT_FRAME | CAMERA_APP_EVENT_SHUTTER |
                      CAMERA_APP_EVENT_ALBUM | CAMERA_APP_EVENT_NEXT,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      rt_tick_from_millisecond(1000), &events);
        if ((events & CAMERA_APP_EVENT_ALBUM) != 0U)
            camera_app_toggle_album();
        if (((events & CAMERA_APP_EVENT_NEXT) != 0U) && album_active)
            camera_app_show_album_photo(RT_TRUE);
        if ((events & CAMERA_APP_EVENT_SHUTTER) != 0U)
        {
            if (album_active)
            {
                rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
                shutter_busy = RT_FALSE;
                rt_mutex_release(&camera_pool_lock);
            }
            else
            {
                camera_app_save_photo();
            }
        }
        else if (((events & CAMERA_APP_EVENT_FRAME) != 0U) && !album_active)
        {
            camera_frame_t frame;

            if (camera_app_acquire_frame(&frame) == 0)
            {
                rt_tick_t display_start = rt_tick_get();
                uint32_t decode_ms = 0U;
                int result = camera_display_show(&frame, &decode_ms);
                rt_tick_t elapsed = rt_tick_get() - display_start;

                rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
                display_ticks += elapsed;
                if (result == 0)
                {
                    displayed_frames++;
                    camera_window.displayed++;
                    if (frame.format == CAMERA_PIXEL_JPEG)
                    {
                        camera_window.decode_samples++;
                        camera_window.decode_total_ms += decode_ms;
                        if (decode_ms > camera_window.decode_max_ms)
                            camera_window.decode_max_ms = decode_ms;
                    }
                }
                else
                {
                    failed_frames++;
                    camera_window.failed++;
                }
                rt_mutex_release(&camera_pool_lock);
                camera_app_release_frame(&frame);
            }
        }
        camera_app_print_stats(&last);
    }
}

int camera_app_start(void)
{
    const camera_source_ops_t *source = camera_app_source();

    if (camera_running)
        return 0;

    frame_pool_init(&camera_pool, camera_stable_buffers[0],
                    camera_stable_buffers[1], CAMERA_SOURCE_SLOT_SIZE,
                    CAMERA_SOURCE_WIDTH, CAMERA_SOURCE_HEIGHT,
                    CAMERA_SOURCE_FORMAT);
    rt_mutex_init(&camera_pool_lock, "cam_pool", RT_IPC_FLAG_PRIO);
    rt_event_init(&camera_events, "cam_evt", RT_IPC_FLAG_PRIO);

    if (camera_display_open(camera_preview_buffer,
                            sizeof(camera_preview_buffer)) != 0)
    {
        rt_kprintf("camera: init failed stage=display\n");
        return -1;
    }
    if (source->open() != 0)
    {
        rt_kprintf("camera: init failed stage=sensor\n");
        camera_display_close();
        return -1;
    }
    if ((source->stream_start == RT_NULL) ||
        (source->stream_wait == RT_NULL) ||
        (source->stream_stop == RT_NULL))
    {
        rt_kprintf("camera: init failed stage=stream_ops\n");
        source->close();
        camera_display_close();
        return -1;
    }
    if (camera_keys_init(camera_app_shutter, camera_app_album,
                         camera_app_next) != 0)
    {
        rt_kprintf("camera: init failed stage=keys\n");
        source->close();
        camera_display_close();
        return -1;
    }

    capture_thread = rt_thread_create("cam_cap", camera_capture_thread,
                                      RT_NULL, 4096, 12, 10);
    display_thread = rt_thread_create("cam_disp", camera_display_thread,
                                      RT_NULL, 6144, 13, 10);
    if ((capture_thread == RT_NULL) || (display_thread == RT_NULL))
    {
        rt_kprintf("camera: init failed stage=threads\n");
        if (capture_thread != RT_NULL) rt_thread_delete(capture_thread);
        if (display_thread != RT_NULL) rt_thread_delete(display_thread);
        source->close();
        camera_display_close();
        return -1;
    }

    if (source->stream_start((uint8_t *)camera_dma_buffers,
                             sizeof(camera_dma_buffers)) != 0)
    {
        rt_kprintf("camera: init failed stage=stream\n");
        rt_thread_delete(capture_thread);
        rt_thread_delete(display_thread);
        source->close();
        camera_display_close();
        return -1;
    }

    camera_running = RT_TRUE;
    rt_thread_startup(display_thread);
    rt_thread_startup(capture_thread);
    rt_kprintf("camera: preview started 640x480 -> 390x450, "
               "KEY1 shutter KEY2 album KEY3 next\n");
    return 0;
}

int camera_app_is_running(void)
{
    return camera_running ? 1 : 0;
}

int camera_app_is_saving(void)
{
    return camera_saving ? 1 : 0;
}

static int camera_app_stats_cmd(int argc, char **argv)
{
    camera_window_stats_t win;
    uint32_t window_ms = CAMERA_APP_STATS_WINDOW_MS;
    int32_t hcpu_usage_x100 = -1;
    int32_t acpu_usage_x100 = -1;
    float hcpu_usage;

    if (argc >= 2)
    {
        int value = atoi(argv[1]);

        if (value > 0)
            window_ms = (uint32_t)value;
    }

    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    camera_window = (camera_window_stats_t){0};
    rt_mutex_release(&camera_pool_lock);

    rt_thread_mdelay(window_ms);

    rt_mutex_take(&camera_pool_lock, RT_WAITING_FOREVER);
    win = camera_window;
    camera_window = (camera_window_stats_t){0};
    rt_mutex_release(&camera_pool_lock);

    hcpu_usage = cpu_get_usage();
    if (hcpu_usage >= 0.0f)
        hcpu_usage_x100 = (int32_t)(hcpu_usage * 100.0f);
    (void)camera_image_query_acpu_usage(&acpu_usage_x100);

    rt_kprintf("camera stats: window=%u ms\n", window_ms);
    rt_kprintf("camera stats: frames captured=%u displayed=%u "
               "dropped=%u failed=%u\n",
               win.captured, win.displayed, win.dropped, win.failed);
    if (win.decode_samples != 0U)
        rt_kprintf("camera stats: decode n=%u avg=%u ms max=%u ms\n",
                   win.decode_samples,
                   win.decode_total_ms / win.decode_samples,
                   win.decode_max_ms);
    else
        rt_kprintf("camera stats: decode n=0\n");
    if (win.encode_samples != 0U)
        rt_kprintf("camera stats: encode n=%u avg=%u ms max=%u ms\n",
                   win.encode_samples,
                   win.encode_total_ms / win.encode_samples,
                   win.encode_max_ms);
    else
        rt_kprintf("camera stats: encode n=0\n");
    if ((hcpu_usage_x100 >= 0) && (acpu_usage_x100 >= 0))
        rt_kprintf("camera stats: cpu hcpu=%d.%02d%% acpu=%d.%02d%%\n",
                   hcpu_usage_x100 / 100, hcpu_usage_x100 % 100,
                   acpu_usage_x100 / 100, acpu_usage_x100 % 100);
    else if (hcpu_usage_x100 >= 0)
        rt_kprintf("camera stats: cpu hcpu=%d.%02d%% acpu=n/a\n",
                   hcpu_usage_x100 / 100, hcpu_usage_x100 % 100);
    else if (acpu_usage_x100 >= 0)
        rt_kprintf("camera stats: cpu hcpu=n/a acpu=%d.%02d%%\n",
                   acpu_usage_x100 / 100, acpu_usage_x100 % 100);
    else
        rt_kprintf("camera stats: cpu hcpu=n/a acpu=n/a\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(camera_app_stats_cmd, camera_stats,
                     "collect camera statistics for a time window");
