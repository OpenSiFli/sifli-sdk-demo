/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_service.h"

#include <bf0_hal.h>
#include <mem_section.h>
#include <rtthread.h>
#include <rthw.h>
#include <string.h>

#include "camera_epic.h"
#include "camera_image.h"
#include "camera_sw_jpeg_encoder.h"
#include "camera_sensor.h"

#if defined(SOC_SF32LB57X) && defined(BSP_USING_DCMI)
#include <drv_dcmi.h>
#if defined(BSP_USING_LCD) && defined(BSP_USING_LCDC) && \
    defined(BSP_LCDC_USING_DPI) && defined(HAL_RAMLESS_LCD_ENABLED) && \
    defined(DMA_SUPPORT_DYN_CHANNEL_ALLOC)
#define CAMERA_LOG_LCD_DMA
extern LCDC_HandleTypeDef *get_drv_lcd_handler(void);
#endif
#endif

#define CAMERA_FRAME_BYTES (CAMERA_LIVE_WIDTH * CAMERA_LIVE_HEIGHT * 2U)
#define CAMERA_FRAME_SLOTS 3
#define CAMERA_THUMB_BYTES (CAMERA_THUMB_WIDTH * CAMERA_THUMB_HEIGHT * 2U)
#define CAMERA_THUMB_SLOT_BYTES ((CAMERA_THUMB_BYTES + 63U) & ~63U)
#define CAMERA_THUMB_RET_SLOTS (CAMERA_PAGE_SIZE / 2U)
#define CAMERA_JPEG_CAPACITY CAMERA_FRAME_BYTES
#define CAMERA_PHOTO_BYTES (CAMERA_PHOTO_WIDTH * CAMERA_PHOTO_HEIGHT * 2U)
#define CAMERA_PREVIEW_SETTLE_FRAMES 2U
#define CAMERA_CAPTURE_TIMEOUT_MS 5000U
#define CAMERA_FPS_WINDOW_MS 5000U

typedef struct
{
    uint32_t sequence;
    unsigned int readers;
    int writing;
    int valid;
} camera_slot_t;

typedef struct
{
    camera_media_kind_t kind;
    uint32_t token;
    uint32_t offset;
    uint32_t count;
    char path[PHOTO_STORE_PATH_MAX];
} camera_request_t;

/* The stream and still capture own this arena at different times. DMA and
 * independent preview frames keep their original sizes in preview mode. */
static union
{
    struct
    {
        uint8_t dma[2U * CAMERA_FRAME_BYTES];
        uint8_t frames[2][CAMERA_FRAME_BYTES];
    } preview;
    uint8_t photo[CAMERA_PHOTO_BYTES];
} s_image_arena L2_RET_BSS_SECT(camera_images) __attribute__((aligned(64)));
typedef char camera_arena_size_check[
    sizeof(s_image_arena) == 4U * CAMERA_FRAME_BYTES ? 1 : -1];
#define s_dma_frame (s_image_arena.preview.dma)
#define s_frames_ret (s_image_arena.preview.frames)
static uint8_t s_thumbnails_ret[CAMERA_THUMB_RET_SLOTS][CAMERA_THUMB_SLOT_BYTES]
    L2_RET_BSS_SECT(camera_thumbnails) __attribute__((aligned(64)));
static uint8_t s_jpeg_work[CAMERA_SW_JPEG_ENCODER_STATIC_WORKSPACE_SIZE]
    __attribute__((aligned(8)));
L2_NON_RET_BSS_SECT_BEGIN(frambuf)
L2_NON_RET_BSS_SECT(frambuf,
    ALIGN(64) static uint8_t s_frame_extra[CAMERA_FRAME_BYTES]);
L2_NON_RET_BSS_SECT(frambuf,
    ALIGN(64) static uint8_t s_thumbnails_nonret[CAMERA_PAGE_SIZE - CAMERA_THUMB_RET_SLOTS]
        [CAMERA_THUMB_SLOT_BYTES]);
L2_NON_RET_BSS_SECT_END

static uint8_t *const s_frames[CAMERA_FRAME_SLOTS] =
{
    s_frames_ret[0], s_frames_ret[1], s_frame_extra
};

static camera_slot_t s_slots[CAMERA_FRAME_SLOTS];
static rt_mutex_t s_lock;
static rt_sem_t s_media_sem;
static rt_sem_t s_photo_sem;

typedef enum
{
    CAMERA_PHOTO_COMMAND_NONE,
    CAMERA_PHOTO_COMMAND_CAPTURE,
    CAMERA_PHOTO_COMMAND_HOLD,
    CAMERA_PHOTO_COMMAND_PREPARE_PREVIEW,
    CAMERA_PHOTO_COMMAND_RESTORE,
    CAMERA_PHOTO_COMMAND_ROTATE,
} camera_photo_command_t;

static camera_photo_command_t s_photo_command;
static camera_capture_stage_t s_capture_stage;
static int s_photo_status;
static int s_photo_memory_owned;
/* Accessed only by the capture thread; no preview DMA is started by this step. */
static int s_photo_preview_prepared;
static camera_request_t s_request;
static camera_media_result_t s_result;
static int s_latest = -1;
static int s_displayed = -1;
static int s_photo_slot = -1;
static uint32_t s_sequence;
static uint32_t s_delivered_sequence;
static int s_live;
static int s_camera_status = -RT_ERROR;
static int s_media_busy;
static int s_result_ready;
static int s_started;
static int s_settings_ready;
static uint16_t s_saved_rotation_degrees;
static uint16_t s_rotation_target_degrees;

typedef enum
{
    CAMERA_DROP_NONE = 0,
    CAMERA_DROP_NO_SLOT,
    CAMERA_DROP_STALE,
    CAMERA_DROP_COPY,
    CAMERA_DROP_LATE,
    CAMERA_DROP_COUNT
} camera_drop_t;

typedef struct
{
    uint32_t count;
    uint32_t sum_ms[4];
    uint32_t max_ms[4];
} camera_copy_stats_t;

typedef struct
{
    uint32_t drops[CAMERA_DROP_COUNT];
    camera_copy_stats_t copy;
} camera_delivery_stats_t;

static camera_delivery_stats_t s_delivery;

static struct
{
    rt_tick_t begin;
    uint32_t received;
    uint32_t published;
    uint32_t displayed;
    uint32_t queue_dropped_total;
    int streaming;
    int active;
} s_fps;

#if defined(SOC_SF32LB57X) && defined(BSP_USING_DCMI)
typedef struct
{
    uint32_t instance;
    uint32_t index;
    uint32_t state;
    uint32_t channel;
    uint32_t ccr;
} camera_dma_snapshot_t;

/* The caller disables interrupts. Never read an inactive handle's old CCR. */
static void camera_snapshot_dma(camera_dma_snapshot_t *snapshot,
                                const DMA_HandleTypeDef *dma, int active)
{
    snapshot->instance = (uint32_t)dma->Instance;
    snapshot->index = dma->ChannelIndex;
    snapshot->state = (uint32_t)dma->State;
    if (active && snapshot->index < 32U && (snapshot->index & 3U) == 0U &&
        snapshot->instance == (uint32_t)DMA1_Channel1 + (snapshot->index >> 2) *
            ((uint32_t)DMA1_Channel2 - (uint32_t)DMA1_Channel1))
    {
        snapshot->channel = (snapshot->index >> 2) + 1U;
        snapshot->ccr = dma->Instance->CCR;
    }
}

static void camera_log_dma_assignment(void)
{
    struct rt_dcmi_device *dcmi = rt_get_dcmi_device();
    camera_dma_snapshot_t capture = {0};
    uint32_t dma_cr = 0U;
    rt_base_t level;
#ifdef CAMERA_LOG_LCD_DMA
    LCDC_HandleTypeDef *lcd = get_drv_lcd_handler();
    camera_dma_snapshot_t display = {0};
    uint32_t lcd_running = 0U;
#endif

    level = rt_hw_interrupt_disable();
    camera_snapshot_dma(&capture, &dcmi->hdma,
                         dcmi->hdma.State == HAL_DMA_STATE_BUSY);
    if (dcmi->handle.Instance != RT_NULL)
        dma_cr = dcmi->handle.Instance->DMA_CR;
#ifdef CAMERA_LOG_LCD_DMA
    if (lcd != RT_NULL)
    {
        lcd_running = lcd->running;
        /* DPI reserves a DMA channel directly; its handle can stay READY. */
        camera_snapshot_dma(&display, &lcd->hdma_handle, lcd_running != 0U);
    }
#endif
    rt_hw_interrupt_enable(level);

    rt_kprintf("camera.dma: dcmi instance=%08lx dma1_ch=%lu state=%lu index=%lu "
               "ccr=%08lx dma_cr=%08lx\n",
               (unsigned long)capture.instance, (unsigned long)capture.channel,
               (unsigned long)capture.state, (unsigned long)capture.index,
               (unsigned long)capture.ccr, (unsigned long)dma_cr);
#ifdef CAMERA_LOG_LCD_DMA
    rt_kprintf("camera.dma: lcd instance=%08lx dma1_ch=%lu state=%lu index=%lu "
               "ccr=%08lx running=%lu\n",
               (unsigned long)display.instance, (unsigned long)display.channel,
               (unsigned long)display.state, (unsigned long)display.index,
               (unsigned long)display.ccr, (unsigned long)lcd_running);
#endif
}
#endif

/* The service mutex protects window changes and all frame counters. */
static void camera_fps_reset_locked(void)
{
    s_fps.begin = rt_tick_get();
    s_fps.received = 0U;
    s_fps.published = 0U;
    s_fps.displayed = 0U;
    s_fps.queue_dropped_total = 0U;
    memset(&s_delivery, 0, sizeof(s_delivery));
    s_fps.active = s_live && s_fps.streaming;
}

static void camera_fps_set_streaming(int enabled)
{
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_fps.streaming = enabled;
    camera_fps_reset_locked();
    rt_mutex_release(s_lock);
}

static void camera_fps_record_received(uint32_t queue_dropped_total)
{
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (s_fps.active)
    {
        ++s_fps.received;
        s_fps.queue_dropped_total = queue_dropped_total;
    }
    rt_mutex_release(s_lock);
}

static uint32_t camera_elapsed_ms(rt_tick_t begin)
{
    return (uint32_t)((uint64_t)(rt_tick_get() - begin) * 1000U /
                      RT_TICK_PER_SECOND);
}

/* Copy metrics cover attempted frames; each rejected frame has one reason.
 * Record after the source-validity check so the stats lock cannot delay it. */
static void camera_record_copy(uint32_t age_ms,
                               const camera_epic_timing_t *timing)
{
    const uint32_t values[4] =
    {
        age_ms, timing->total_ms, timing->submit_ms, timing->resume_ms
    };
    unsigned int index;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (s_fps.active)
    {
        camera_copy_stats_t *stats = &s_delivery.copy;
        ++stats->count;
        for (index = 0U; index < 4U; ++index)
        {
            stats->sum_ms[index] += values[index];
            if (values[index] > stats->max_ms[index])
                stats->max_ms[index] = values[index];
        }
    }
    rt_mutex_release(s_lock);
}

static void camera_record_drop(camera_drop_t reason)
{
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (s_fps.active && reason > CAMERA_DROP_NONE && reason < CAMERA_DROP_COUNT)
        ++s_delivery.drops[reason];
    rt_mutex_release(s_lock);
}

static int camera_reserve_live_frame(void)
{
    int slot = -1;
    int index;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    for (index = 0; index < CAMERA_FRAME_SLOTS; ++index)
    {
        if (index != s_latest && s_slots[index].readers == 0U &&
            !s_slots[index].writing)
        {
            slot = index;
            s_slots[index].writing = 1;
            s_slots[index].valid = 0;
            break;
        }
    }
    rt_mutex_release(s_lock);
    return slot;
}

static void camera_finish_live_frame(int slot, int publish)
{
    if (slot < 0)
        return;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (publish && s_capture_stage == CAMERA_CAPTURE_IDLE && !s_photo_memory_owned)
    {
        s_slots[slot].sequence = ++s_sequence;
        if (s_fps.active)
            ++s_fps.published;
        s_slots[slot].valid = 1;
        s_latest = slot;
        s_camera_status = 0;
    }
    s_slots[slot].writing = 0;
    rt_mutex_release(s_lock);
}

/* Caller holds s_lock. All readers must have detached before this point. */
static int camera_photo_reserve_memory(void)
{
    int index;

    for (index = 0; index < CAMERA_FRAME_SLOTS; ++index)
        if (s_slots[index].readers != 0U || s_slots[index].writing)
            return -RT_EBUSY;
    for (index = 0; index < CAMERA_FRAME_SLOTS; ++index)
    {
        s_slots[index].writing = 1;
        s_slots[index].valid = 0;
    }
    s_latest = -1;
    s_displayed = -1;
    s_photo_memory_owned = 1;
    return RT_EOK;
}

/* Called only after still DMA and encoding have retired, or after a verified
 * switch back to the preview DMA region. */
static void camera_photo_release_memory(void)
{
    int index;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (s_photo_memory_owned)
    {
        for (index = 0; index < CAMERA_FRAME_SLOTS; ++index)
        {
            s_slots[index].writing = 0;
            s_slots[index].valid = 0;
        }
        s_latest = -1;
        s_displayed = -1;
        s_photo_memory_owned = 0;
    }
    rt_mutex_release(s_lock);
}

static int camera_photo_prepare(int *opened, int *streaming, int *close_pending)
{
    int status = camera_sensor_stop();

    s_photo_preview_prepared = 0;
    camera_fps_set_streaming(0);
    if (status != RT_EOK || !camera_sensor_is_quiescent())
    {
        *close_pending = camera_sensor_close() != RT_EOK;
        *opened = 0;
        *streaming = 0;
        return status != RT_EOK ? status : -RT_EIO;
    }
    *streaming = 0;
    *close_pending = 0;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    status = camera_photo_reserve_memory();
    rt_mutex_release(s_lock);
    if (status != RT_EOK)
        return status;
    if (!*opened)
    {
        status = camera_sensor_open();
        *opened = status == RT_EOK;
    }
    if (status == RT_EOK)
        status = camera_sensor_configure_photo();
    if (status == RT_EOK)
        status = camera_sensor_capture_photo(s_image_arena.photo,
                        sizeof(s_image_arena.photo), CAMERA_CAPTURE_TIMEOUT_MS);
    if (status == RT_EOK && !camera_sensor_is_quiescent())
        status = -RT_EIO;
    return status;
}

/* Only sensor registers change here. The captured pixels and JPEG output stay
 * reserved while the media thread encodes and saves the photo. */
static int camera_photo_prepare_preview(int opened, int close_pending)
{
    int status;

    s_photo_preview_prepared = 0;
    if (!opened || close_pending || !s_photo_memory_owned ||
        !camera_sensor_is_quiescent())
        return -RT_EIO;
    status = camera_sensor_restore_preview();
    if (status == RT_EOK && !camera_sensor_is_quiescent())
        status = -RT_EIO;
    s_photo_preview_prepared = status == RT_EOK;
    return status;
}

static int camera_photo_restore(int *opened, int *streaming, int *close_pending)
{
    unsigned int index;
    int enabled;
    int status = *opened && !*close_pending
                     ? (s_photo_preview_prepared ? RT_EOK : camera_sensor_restore_preview())
                     : -RT_ERROR;

    s_photo_preview_prepared = 0;
    *streaming = 0;
    if (status != RT_EOK)
    {
        status = camera_sensor_close();
        *opened = 0;
        if (status == RT_EOK)
        {
            status = camera_sensor_open();
            *opened = status == RT_EOK;
        }
    }
    if (status == RT_EOK && !camera_sensor_is_quiescent())
        status = -RT_EIO;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    enabled = s_live;
    rt_mutex_release(s_lock);
    if (status == RT_EOK && enabled)
    {
        status = camera_sensor_start(s_dma_frame, sizeof(s_dma_frame));
        *streaming = status == RT_EOK;
        for (index = 0U; status == RT_EOK && index < CAMERA_PREVIEW_SETTLE_FRAMES; ++index)
        {
            camera_sensor_frame_t frame;

            status = camera_sensor_wait(&frame, CAMERA_CAPTURE_TIMEOUT_MS);
            if (status == RT_EOK && !camera_sensor_frame_is_valid(&frame))
                status = -RT_EIO;
        }
    }
    if (status != RT_EOK)
    {
        int stop_status = camera_sensor_close();

        *close_pending = stop_status != RT_EOK;
        *opened = 0;
        *streaming = 0;
    }
    else
        *close_pending = 0;
    if (status == RT_EOK || camera_sensor_is_quiescent())
        camera_photo_release_memory();
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_camera_status = status;
    rt_mutex_release(s_lock);
    camera_fps_set_streaming(status == RT_EOK && *streaming);
    return status;
}

static int camera_preview_rotate(int *opened, int *streaming, int *close_pending)
{
    uint16_t degrees;
    int status;
    int rotation_status;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    degrees = s_rotation_target_degrees;
    rt_mutex_release(s_lock);
    camera_fps_set_streaming(0);
    s_photo_preview_prepared = 0;
    status = camera_sensor_stop();
    /* Cache the saved target even after a failed stop, so recovery reopens
     * with the persisted direction. Busy capture never writes registers. */
    rotation_status = camera_sensor_set_rotation(degrees);
    s_photo_preview_prepared = status == RT_EOK && rotation_status == RT_EOK;
    return camera_photo_restore(opened, streaming, close_pending);
}

/* The capture thread alone configures or starts the sensor. PREPARE_PREVIEW
 * can overlap encoding, but RESTORE starts DMA and releases the still pixels
 * only after encoding and saving have retired. */
static int camera_stream_photo_command(int *opened, int *streaming, int *close_pending)
{
    camera_photo_command_t command;
    camera_capture_stage_t stage;
    int status;
    const char *phase;
    rt_tick_t begin;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    command = s_photo_command;
    stage = s_capture_stage;
    rt_mutex_release(s_lock);
    if (command != CAMERA_PHOTO_COMMAND_CAPTURE &&
        command != CAMERA_PHOTO_COMMAND_PREPARE_PREVIEW &&
        command != CAMERA_PHOTO_COMMAND_RESTORE &&
        command != CAMERA_PHOTO_COMMAND_ROTATE)
        return stage != CAMERA_CAPTURE_IDLE;
    begin = rt_tick_get();
    if (command == CAMERA_PHOTO_COMMAND_CAPTURE)
    {
        phase = "capture";
        status = camera_photo_prepare(opened, streaming, close_pending);
    }
    else if (command == CAMERA_PHOTO_COMMAND_PREPARE_PREVIEW)
    {
        phase = "preview_config";
        status = camera_photo_prepare_preview(*opened, *close_pending);
    }
    else if (command == CAMERA_PHOTO_COMMAND_ROTATE)
    {
        phase = "rotate";
        status = camera_preview_rotate(opened, streaming, close_pending);
    }
    else
    {
        phase = "restore";
        status = camera_photo_restore(opened, streaming, close_pending);
    }
    rt_kprintf("camera.photo: %s status=%d elapsed_ms=%lu quiescent=%d\n",
               phase, status, (unsigned long)camera_elapsed_ms(begin), camera_sensor_is_quiescent());
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_photo_status = status;
    s_photo_command = CAMERA_PHOTO_COMMAND_HOLD;
    rt_mutex_release(s_lock);
    rt_sem_release(s_photo_sem);
    return 1;
}

static void camera_stream_worker(void *parameter)
{
    int opened = 0;
    int streaming = 0;
    int close_pending = 0;
    int slot = -1;
    int startup_status;
    uint16_t startup_rotation;

    (void)parameter;
    for (;;)
    {
        int ready;

        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        ready = s_settings_ready;
        startup_rotation = s_saved_rotation_degrees;
        rt_mutex_release(s_lock);
        if (ready)
            break;
        rt_thread_mdelay(1);
    }
    (void)camera_sensor_set_rotation(startup_rotation);
    /* Prewarm the sensor alongside LCD initialization. DCMI and EPIC still
     * wait for the UI's first flush and its set_live request below. */
    startup_status = camera_sensor_open();
    opened = startup_status == RT_EOK;
    close_pending = startup_status != RT_EOK && !camera_sensor_is_quiescent();
    for (;;)
    {
        camera_sensor_frame_t frame;
        camera_epic_timing_t timing;
        uint32_t age_ms;
        int valid;
        int enabled;
        int status;

        if (camera_stream_photo_command(&opened, &streaming, &close_pending))
        {
            rt_thread_mdelay(1);
            continue;
        }
        if (close_pending)
        {
            if (camera_sensor_close() != RT_EOK)
            {
                rt_thread_mdelay(500);
                continue;
            }
            close_pending = 0;
            opened = 0;
            streaming = 0;
            camera_photo_release_memory();
        }

        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        enabled = s_live;
        rt_mutex_release(s_lock);
        if (!enabled)
        {
            camera_finish_live_frame(slot, 0);
            slot = -1;
            if (streaming)
            {
                camera_fps_set_streaming(0);
                status = camera_sensor_stop();
                if (status == RT_EOK)
                    streaming = 0;
                else
                {
                    close_pending = 1;
                    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
                    s_camera_status = status;
                    rt_mutex_release(s_lock);
                    rt_kprintf("camera: stream stop failed %d\n", status);
                }
            }
            rt_thread_mdelay(close_pending ? 500 : 10);
            continue;
        }

        status = RT_EOK;
        if (!opened)
        {
            status = camera_sensor_open();
            opened = status == RT_EOK;
        }
        if (status == RT_EOK && !streaming)
        {
            status = camera_sensor_start(s_dma_frame, sizeof(s_dma_frame));
            streaming = status == RT_EOK;
            if (streaming)
                rt_kprintf("camera.stream: mode=frame buffers=2 frame_bytes=%lu\n",
                           (unsigned long)CAMERA_FRAME_BYTES);
#if defined(SOC_SF32LB57X) && defined(BSP_USING_DCMI)
            if (streaming)
                camera_log_dma_assignment();
#endif
            camera_fps_set_streaming(streaming);
        }
        if (status == RT_EOK)
            status = camera_sensor_wait(&frame, CAMERA_CAPTURE_TIMEOUT_MS);
        if (status != RT_EOK)
        {
            camera_finish_live_frame(slot, 0);
            slot = -1;
            rt_mutex_take(s_lock, RT_WAITING_FOREVER);
            s_camera_status = status;
            rt_mutex_release(s_lock);
            camera_fps_set_streaming(0);
            close_pending = camera_sensor_close() != RT_EOK;
            opened = 0;
            streaming = 0;
            rt_kprintf("camera: stream failed %d, retrying\n", status);
            rt_thread_mdelay(500);
            continue;
        }

        /* HT and TC each deliver one complete image from the two-frame ring. */
        camera_fps_record_received(frame.queue_dropped_total);
        if (!camera_sensor_frame_is_valid(&frame))
        {
            camera_record_drop(CAMERA_DROP_STALE);
            continue;
        }
        slot = camera_reserve_live_frame();
        if (slot < 0)
        {
            camera_record_drop(CAMERA_DROP_NO_SLOT);
            continue;
        }
        age_ms = camera_elapsed_ms(frame.capture.timestamp_ticks);
        status = camera_epic_render_frame(s_frames[slot], frame.capture.buffer,
                                           CAMERA_LIVE_WIDTH, CAMERA_LIVE_HEIGHT,
                                           &timing);
        valid = status == RT_EOK && camera_sensor_frame_is_valid(&frame);
        camera_record_copy(age_ms, &timing);
        camera_finish_live_frame(slot, valid);
        if (!valid)
            camera_record_drop(status != RT_EOK ? CAMERA_DROP_COPY : CAMERA_DROP_LATE);
        slot = -1;
    }
}

static uint8_t *camera_thumbnail_buffer(size_t index)
{
    if (index < CAMERA_THUMB_RET_SLOTS)
        return s_thumbnails_ret[index];
    return s_thumbnails_nonret[index - CAMERA_THUMB_RET_SLOTS];
}

static int camera_reserve_work_frame(void)
{
    rt_tick_t begin = rt_tick_get();

    for (;;)
    {
        int slot = -1;
        int index;

        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        for (index = 0; index < CAMERA_FRAME_SLOTS; ++index)
        {
            if (index != s_latest && s_slots[index].readers == 0U &&
                !s_slots[index].writing)
            {
                slot = index;
                break;
            }
        }
        if (slot < 0 && s_latest >= 0 &&
            s_slots[s_latest].readers == 0U && !s_slots[s_latest].writing)
            slot = s_latest;
        if (slot >= 0)
        {
            s_slots[slot].writing = 1;
            s_slots[slot].valid = 0;
            if (slot == s_latest)
                s_latest = -1;
        }
        rt_mutex_release(s_lock);
        if (slot >= 0)
            return slot;
        if (camera_elapsed_ms(begin) >= CAMERA_CAPTURE_TIMEOUT_MS)
            return -RT_EBUSY;
        rt_thread_mdelay(1);
    }
}

static void camera_release_work_frame(int slot)
{
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_slots[slot].writing = 0;
    rt_mutex_release(s_lock);
}

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint16_t source_width;
    uint16_t source_height;
    uint32_t file_size;
} camera_decoded_info_t;

static int camera_load_jpeg(const char *path, uint8_t *pixels,
                            uint32_t capacity, uint16_t target_width,
                            uint16_t target_height, camera_decoded_info_t *info)
{
    size_t size = 0U;
    int work_slot = camera_reserve_work_frame();
    int status;
    uint8_t scale;

    memset(info, 0, sizeof(*info));
    if (work_slot < 0)
        return work_slot;
    status = photo_store_read_jpeg(path, s_frames[work_slot],
                                   CAMERA_JPEG_CAPACITY, &size);
    if (status == RT_EOK)
        status = camera_image_probe_jpeg(s_frames[work_slot], size,
                                         &info->source_width, &info->source_height);
    if (status == RT_EOK)
    {
        for (scale = 0U; scale <= 3U; ++scale)
            if ((info->source_width >> scale) == target_width &&
                (info->source_height >> scale) == target_height)
                break;
        if (scale > 3U)
            status = -RT_EINVAL;
        else
            status = camera_image_decode_jpeg(s_frames[work_slot], size, pixels,
                        capacity, scale, &info->width, &info->height);
    }
    if (status == RT_EOK &&
        (info->width != target_width || info->height != target_height))
        status = -RT_EINVAL;
    if (status == RT_EOK)
    {
        info->file_size = (uint32_t)size;
        mpu_dcache_clean(pixels, (uint32_t)info->width * info->height * 2U);
    }
    camera_release_work_frame(work_slot);
    return status;
}

static int camera_load_photo(const char *path, camera_media_result_t *result)
{
    int slot = camera_reserve_work_frame();
    camera_decoded_info_t info;
    int status;

    if (slot < 0)
        return slot;
    status = camera_load_jpeg(path, s_frames[slot], CAMERA_FRAME_BYTES,
                              CAMERA_LIVE_WIDTH, CAMERA_LIVE_HEIGHT, &info);
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_slots[slot].writing = 0;
    if (status == RT_EOK)
    {
        /* The UI detaches this image before submitting another media job. */
        s_slots[slot].readers++;
        s_photo_slot = slot;
        result->photo = s_frames[slot];
        result->width = info.width;
        result->height = info.height;
        result->source_width = info.source_width;
        result->source_height = info.source_height;
        result->file_size = info.file_size;
    }
    rt_mutex_release(s_lock);
    return status;
}

static int camera_load_photo_at(uint32_t offset, camera_media_result_t *result)
{
    int status = photo_store_list(offset, result->items, 1U,
                                  &result->count, &result->total);

    if (status != RT_EOK)
        return status;
    if (result->count != 1U)
        return -RT_EINVAL;
    return camera_load_photo(result->items[0].path, result);
}

static int camera_save_photo(camera_media_result_t *result)
{
    static const camera_sw_jpeg_quality_t qualities[] =
    {
        CAMERA_SW_JPEG_QUALITY_HIGH, CAMERA_SW_JPEG_QUALITY_MEDIUM,
        CAMERA_SW_JPEG_QUALITY_LOW,
    };
    camera_sw_jpeg_encoder_t encoder;
    camera_sw_jpeg_frame_t frame;
    uint32_t size = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    unsigned int attempt;
    int status = -RT_ERROR;
    rt_tick_t begin;

    begin = rt_tick_get();

    frame.data = s_image_arena.photo;
    frame.width = CAMERA_PHOTO_WIDTH;
    frame.height = CAMERA_PHOTO_HEIGHT;
    frame.stride = CAMERA_PHOTO_WIDTH * 2U;
    frame.pixel_format = CAMERA_SW_JPEG_PIXEL_RGB565;
    for (attempt = 0U; attempt < sizeof(qualities) / sizeof(qualities[0]); ++attempt)
    {
        status = camera_sw_jpeg_encoder_init(&encoder, s_jpeg_work, sizeof(s_jpeg_work));
        if (status != RT_EOK)
            break;
        status = camera_sw_jpeg_encode_to_buffer(&encoder, &frame, qualities[attempt],
                    s_frame_extra, CAMERA_JPEG_CAPACITY, &size);
        if (status == RT_EOK)
            break;
    }
    rt_kprintf("camera.photo: encode status=%d elapsed_ms=%lu bytes=%lu quality=%u\n",
               status, (unsigned long)camera_elapsed_ms(begin), (unsigned long)size,
               (unsigned int)(attempt < sizeof(qualities) / sizeof(qualities[0])
                                  ? qualities[attempt] : CAMERA_SW_JPEG_QUALITY_LOW));
    if (status == RT_EOK &&
        (size == 0U || size > CAMERA_JPEG_CAPACITY ||
         camera_image_probe_jpeg(s_frame_extra, size, &width, &height) != RT_EOK ||
         width != CAMERA_PHOTO_WIDTH || height != CAMERA_PHOTO_HEIGHT))
        status = -RT_EIO;
    if (status == RT_EOK)
    {
        begin = rt_tick_get();
        status = photo_store_save_jpeg(s_frame_extra, size, &result->items[0]);
        rt_kprintf("camera.photo: save status=%d elapsed_ms=%lu bytes=%lu\n",
                   status, (unsigned long)camera_elapsed_ms(begin), (unsigned long)size);
    }
    if (status == RT_EOK)
    {
        result->source_width = width;
        result->source_height = height;
        result->file_size = size;
        result->count = 1U;
    }
    return status;
}

static void camera_media_capture(camera_media_result_t *result)
{
    rt_tick_t begin = rt_tick_get();

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_photo_command = CAMERA_PHOTO_COMMAND_CAPTURE;
    rt_mutex_release(s_lock);
    rt_sem_take(s_photo_sem, RT_WAITING_FOREVER);
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    result->status = s_photo_status;
    if (result->status == RT_EOK)
        s_capture_stage = CAMERA_CAPTURE_SAVING;
    rt_mutex_release(s_lock);
    if (result->status == RT_EOK)
    {
        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        s_photo_command = CAMERA_PHOTO_COMMAND_PREPARE_PREVIEW;
        rt_mutex_release(s_lock);
        result->status = camera_save_photo(result);
        /* Retire the configuration command even if encoding or saving fails.
         * A failed preconfiguration is retried by the normal restore path. */
        rt_sem_take(s_photo_sem, RT_WAITING_FOREVER);
    }

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_capture_stage = CAMERA_CAPTURE_RESTORING;
    s_photo_command = CAMERA_PHOTO_COMMAND_RESTORE;
    rt_mutex_release(s_lock);
    rt_sem_take(s_photo_sem, RT_WAITING_FOREVER);
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    result->preview_status = s_photo_status;
    s_photo_command = CAMERA_PHOTO_COMMAND_NONE;
    s_capture_stage = CAMERA_CAPTURE_IDLE;
    rt_mutex_release(s_lock);
    rt_kprintf("camera.photo: total status=%d preview_status=%d elapsed_ms=%lu\n",
               result->status, result->preview_status,
               (unsigned long)camera_elapsed_ms(begin));
}

static void camera_media_rotate(camera_media_result_t *result)
{
    uint16_t degrees = s_saved_rotation_degrees == 0U ? 180U : 0U;

    result->status = photo_store_save_rotation(degrees);
    if (result->status != RT_EOK)
    {
        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        s_capture_stage = CAMERA_CAPTURE_IDLE;
        result->preview_status = s_camera_status;
        rt_mutex_release(s_lock);
        return;
    }
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_saved_rotation_degrees = degrees;
    s_rotation_target_degrees = degrees;
    s_photo_command = CAMERA_PHOTO_COMMAND_ROTATE;
    rt_mutex_release(s_lock);
    rt_sem_take(s_photo_sem, RT_WAITING_FOREVER);
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    result->status = s_photo_status;
    result->preview_status = s_camera_status;
    s_photo_command = CAMERA_PHOTO_COMMAND_NONE;
    s_capture_stage = CAMERA_CAPTURE_IDLE;
    rt_mutex_release(s_lock);
    rt_kprintf("camera.rotate: saved_degrees=%u status=%d\n",
               (unsigned int)degrees, result->status);
}

static void camera_media_worker(void *parameter)
{
    (void)parameter;
    /* Load the saved direction here before the capture thread opens the sensor. */
    uint16_t rotation = 0U;
    int storage_status = photo_store_init();
    int settings_status = storage_status;

    if (storage_status != RT_EOK)
        rt_kprintf("camera: storage initialization failed %d\n", storage_status);
    else
        settings_status = photo_store_load_rotation(&rotation);
    if (settings_status != RT_EOK)
    {
        rotation = 0U;
        rt_kprintf("camera.rotate: load failed %d, using 0 degrees\n", settings_status);
    }
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_saved_rotation_degrees = rotation;
    s_rotation_target_degrees = rotation;
    s_settings_ready = 1;
    rt_mutex_release(s_lock);
    rt_kprintf("camera.rotate: loaded_degrees=%u\n", (unsigned int)rotation);
    for (;;)
    {
        camera_request_t request;
        size_t index;

        rt_sem_take(s_media_sem, RT_WAITING_FOREVER);
        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        request = s_request;
        rt_mutex_release(s_lock);
        memset(&s_result, 0, sizeof(s_result));
        s_result.kind = request.kind;
        s_result.token = request.token;
        s_result.offset = request.offset;

        if (request.kind == CAMERA_MEDIA_CAPTURE)
        {
            camera_media_capture(&s_result);
        }
        else if (request.kind == CAMERA_MEDIA_ROTATE)
        {
            camera_media_rotate(&s_result);
        }
        else if (request.kind == CAMERA_MEDIA_PAGE)
        {
            s_result.status = photo_store_list(request.offset, s_result.items,
                request.count, &s_result.count, &s_result.total);
            if (s_result.status == 0)
            {
                for (index = 0U; index < s_result.count; ++index)
                {
                    camera_decoded_info_t info;
                    int status = camera_load_jpeg(s_result.items[index].path,
                        camera_thumbnail_buffer(index), CAMERA_THUMB_BYTES,
                        CAMERA_THUMB_WIDTH, CAMERA_THUMB_HEIGHT, &info);
                    if (status == RT_EOK)
                        s_result.thumbnails[index] = camera_thumbnail_buffer(index);
                    else
                        rt_kprintf("camera: thumbnail unavailable %s (%d)\n",
                                   s_result.items[index].path, status);
                }
            }
        }
        else if (request.kind == CAMERA_MEDIA_PHOTO)
        {
            s_result.status = camera_load_photo(request.path, &s_result);
        }
        else if (request.kind == CAMERA_MEDIA_PHOTO_INDEX)
        {
            s_result.status = camera_load_photo_at(request.offset, &s_result);
        }
        else
            s_result.status = -RT_EINVAL;
        if (s_result.status != 0 && request.kind != CAMERA_MEDIA_CAPTURE)
            rt_kprintf("camera: media kind=%u status=%d offset=%u path=%s\n",
                       (unsigned int)request.kind, s_result.status,
                       request.offset, request.path);

        rt_mutex_take(s_lock, RT_WAITING_FOREVER);
        s_result_ready = 1;
        rt_mutex_release(s_lock);
    }
}

int camera_service_start(void)
{
    rt_thread_t stream;
    rt_thread_t media;

    if (s_started)
        return RT_EOK;
    s_lock = rt_mutex_create("cam_state", RT_IPC_FLAG_PRIO);
    s_media_sem = rt_sem_create("cam_media", 0, RT_IPC_FLAG_FIFO);
    s_photo_sem = rt_sem_create("cam_photo", 0, RT_IPC_FLAG_FIFO);
    if (s_lock == RT_NULL || s_media_sem == RT_NULL || s_photo_sem == RT_NULL)
        goto failed;
    stream = rt_thread_create("cam_stream", camera_stream_worker, RT_NULL,
                              4096U, 21U, 10U);
    if (stream == RT_NULL)
        goto failed;
    media = rt_thread_create("cam_media", camera_media_worker, RT_NULL,
                             8192U, 22U, 10U);
    if (media == RT_NULL)
    {
        rt_thread_delete(stream);
        goto failed;
    }
    s_started = 1;
    rt_thread_startup(media);
    rt_thread_startup(stream);
    return RT_EOK;

failed:
    if (s_photo_sem != RT_NULL)
        rt_sem_delete(s_photo_sem);
    s_photo_sem = RT_NULL;
    if (s_media_sem != RT_NULL)
        rt_sem_delete(s_media_sem);
    if (s_lock != RT_NULL)
        rt_mutex_delete(s_lock);
    s_media_sem = RT_NULL;
    s_lock = RT_NULL;
    return -RT_ENOMEM;
}

void camera_service_set_live(int enabled)
{
    if (!s_started)
        return;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    enabled = enabled != 0;
    if (s_live != enabled)
    {
        s_live = enabled;
        camera_fps_reset_locked();
    }
    rt_mutex_release(s_lock);
}

void camera_service_record_displayed(void)
{
    if (!s_started)
        return;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (s_fps.active)
        ++s_fps.displayed;
    rt_mutex_release(s_lock);
}

void camera_service_report_fps(void)
{
    rt_tick_t now;
    rt_tick_t elapsed;
    uint32_t received;
    uint32_t published;
    uint32_t displayed;
    uint32_t queue_dropped_total;
    uint32_t received_x10;
    uint32_t published_x10;
    uint32_t displayed_x10;
    uint32_t window_ms;
    camera_delivery_stats_t delivery;
    const camera_copy_stats_t *stats;
    uint32_t divisor;

    if (!s_started)
        return;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    now = rt_tick_get();
    elapsed = (rt_tick_t)(now - s_fps.begin);
    if (!s_fps.active || elapsed < rt_tick_from_millisecond(CAMERA_FPS_WINDOW_MS))
    {
        rt_mutex_release(s_lock);
        return;
    }
    received = s_fps.received;
    published = s_fps.published;
    displayed = s_fps.displayed;
    queue_dropped_total = s_fps.queue_dropped_total;
    delivery = s_delivery;
    memset(&s_delivery, 0, sizeof(s_delivery));
    s_fps.begin = now;
    s_fps.received = 0U;
    s_fps.published = 0U;
    s_fps.displayed = 0U;
    rt_mutex_release(s_lock);

    received_x10 = (uint32_t)((uint64_t)received * 10U * RT_TICK_PER_SECOND / elapsed);
    published_x10 = (uint32_t)((uint64_t)published * 10U * RT_TICK_PER_SECOND / elapsed);
    displayed_x10 = (uint32_t)((uint64_t)displayed * 10U * RT_TICK_PER_SECOND / elapsed);
    window_ms = (uint32_t)((uint64_t)elapsed * 1000U / RT_TICK_PER_SECOND);
    rt_kprintf("camera.fps: received=%lu.%lu published=%lu.%lu displayed=%lu.%lu "
               "frames=%lu/%lu/%lu window_ms=%lu\n",
               (unsigned long)(received_x10 / 10U), (unsigned long)(received_x10 % 10U),
               (unsigned long)(published_x10 / 10U), (unsigned long)(published_x10 % 10U),
               (unsigned long)(displayed_x10 / 10U), (unsigned long)(displayed_x10 % 10U),
               (unsigned long)received, (unsigned long)published, (unsigned long)displayed,
               (unsigned long)window_ms);
    rt_kprintf("camera.drop: no_slot=%lu stale=%lu copy=%lu late=%lu queue_total=%lu\n",
               (unsigned long)delivery.drops[CAMERA_DROP_NO_SLOT],
               (unsigned long)delivery.drops[CAMERA_DROP_STALE],
               (unsigned long)delivery.drops[CAMERA_DROP_COPY],
               (unsigned long)delivery.drops[CAMERA_DROP_LATE],
               (unsigned long)queue_dropped_total);
    stats = &delivery.copy;
    divisor = stats->count != 0U ? stats->count : 1U;
    rt_kprintf("camera.copy: engine=epic scope=frame n=%lu avg/max_ms age=%lu/%lu "
               "total=%lu/%lu submit=%lu/%lu resume=%lu/%lu\n",
               (unsigned long)stats->count,
               (unsigned long)(stats->sum_ms[0] / divisor), (unsigned long)stats->max_ms[0],
               (unsigned long)(stats->sum_ms[1] / divisor), (unsigned long)stats->max_ms[1],
               (unsigned long)(stats->sum_ms[2] / divisor), (unsigned long)stats->max_ms[2],
               (unsigned long)(stats->sum_ms[3] / divisor), (unsigned long)stats->max_ms[3]);
}

int camera_service_take_frame(camera_live_frame_t *frame)
{
    int result = 0;

    if (!s_started || frame == RT_NULL)
        return -RT_EINVAL;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (s_capture_stage == CAMERA_CAPTURE_IDLE && !s_photo_memory_owned &&
        s_latest >= 0 && s_slots[s_latest].valid &&
        !s_slots[s_latest].writing &&
        s_slots[s_latest].sequence != s_delivered_sequence)
    {
        frame->slot = s_latest;
        frame->pixels = s_frames[s_latest];
        frame->sequence = s_slots[s_latest].sequence;
        s_slots[s_latest].readers++;
        s_displayed = s_latest;
        s_delivered_sequence = frame->sequence;
        result = 1;
    }
    rt_mutex_release(s_lock);
    return result;
}

void camera_service_release_frame(camera_live_frame_t *frame)
{
    if (!s_started || frame == RT_NULL || frame->slot < 0 ||
        frame->slot >= CAMERA_FRAME_SLOTS)
        return;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (s_slots[frame->slot].readers > 0U)
        s_slots[frame->slot].readers--;
    if (s_displayed == frame->slot)
        s_displayed = -1;
    rt_mutex_release(s_lock);
    frame->slot = -1;
    frame->pixels = RT_NULL;
}

static int camera_submit(camera_request_t *request)
{
    if (!s_started)
        return -RT_ERROR;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (s_media_busy || s_photo_memory_owned || s_capture_stage != CAMERA_CAPTURE_IDLE)
    {
        rt_mutex_release(s_lock);
        return -RT_EBUSY;
    }
    if ((request->kind == CAMERA_MEDIA_CAPTURE || request->kind == CAMERA_MEDIA_ROTATE) &&
        (!s_live || s_camera_status != RT_EOK))
    {
        rt_mutex_release(s_lock);
        return -RT_ERROR;
    }
    /* The caller has hidden the old photo and waited for EPIC. */
    if (s_photo_slot >= 0)
    {
        s_slots[s_photo_slot].readers--;
        s_photo_slot = -1;
    }
    if (request->kind == CAMERA_MEDIA_CAPTURE)
    {
        int index;

        for (index = 0; index < CAMERA_FRAME_SLOTS; ++index)
            if (s_slots[index].readers != 0U)
            {
                rt_mutex_release(s_lock);
                return -RT_EBUSY;
            }
        s_capture_stage = CAMERA_CAPTURE_CAPTURING;
        s_latest = -1;
        s_displayed = -1;
        for (index = 0; index < CAMERA_FRAME_SLOTS; ++index)
            s_slots[index].valid = 0;
        camera_fps_reset_locked();
    }
    if (request->kind == CAMERA_MEDIA_ROTATE)
    {
        s_capture_stage = CAMERA_CAPTURE_RESTORING;
        s_latest = -1;
        camera_fps_reset_locked();
    }
    s_request = *request;
    s_media_busy = 1;
    rt_mutex_release(s_lock);
    rt_sem_release(s_media_sem);
    return 0;
}

int camera_service_capture(uint32_t token)
{
    camera_request_t request = {0};
    request.kind = CAMERA_MEDIA_CAPTURE;
    request.token = token;
    return camera_submit(&request);
}

int camera_service_rotate(uint32_t token)
{
    camera_request_t request = {0};
    request.kind = CAMERA_MEDIA_ROTATE;
    request.token = token;
    return camera_submit(&request);
}

camera_capture_stage_t camera_service_capture_stage(void)
{
    camera_capture_stage_t stage;

    if (!s_started)
        return CAMERA_CAPTURE_IDLE;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    stage = s_capture_stage;
    rt_mutex_release(s_lock);
    return stage;
}

int camera_service_request_page(uint32_t offset, uint32_t count, uint32_t token)
{
    camera_request_t request = {0};
    if (count == 0U || count > CAMERA_PAGE_SIZE)
        return -RT_EINVAL;
    request.kind = CAMERA_MEDIA_PAGE;
    request.offset = offset;
    request.count = count;
    request.token = token;
    return camera_submit(&request);
}

int camera_service_request_photo(const char *path, uint32_t token)
{
    camera_request_t request = {0};
    if (path == RT_NULL || strlen(path) >= sizeof(request.path))
        return -RT_EINVAL;
    request.kind = CAMERA_MEDIA_PHOTO;
    request.token = token;
    memcpy(request.path, path, strlen(path) + 1U);
    return camera_submit(&request);
}

int camera_service_request_photo_at(uint32_t offset, uint32_t token)
{
    camera_request_t request = {0};
    request.kind = CAMERA_MEDIA_PHOTO_INDEX;
    request.offset = offset;
    request.token = token;
    return camera_submit(&request);
}

int camera_service_poll_result(camera_media_result_t *result)
{
    if (!s_started || result == RT_NULL)
        return 0;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (!s_result_ready)
    {
        rt_mutex_release(s_lock);
        return 0;
    }
    *result = s_result;
    s_result_ready = 0;
    s_media_busy = 0;
    rt_mutex_release(s_lock);
    return 1;
}

int camera_service_camera_status(void)
{
    int status;
    if (!s_started)
        return -RT_ERROR;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    status = s_camera_status;
    rt_mutex_release(s_lock);
    return status;
}
