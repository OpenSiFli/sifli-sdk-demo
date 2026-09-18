/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_sensor.h"

#include <bf0_hal.h>
#include <drv_io.h>
#include <rthw.h>
#include <rtthread.h>
#include <string.h>

#include "camera_service.h"
#include "data_bus_adapter.h"

#if !defined(SENSOR_USING_OV2640) || !defined(CAMERA_OV2640_INTERFACE_DVP) || \
    !defined(CAMERA_DVP_BACKEND_DCMI)
#error "This application requires OV2640 8-bit DVP through the hardware DCMI backend"
#endif

#define CAMERA_SENSOR_FRAME_SIZE (CAMERA_LIVE_WIDTH * CAMERA_LIVE_HEIGHT * 2U)
#define CAMERA_SENSOR_SETTLE_MS 200U
#define CAMERA_SENSOR_STOP_TIMEOUT_MS 200U
#define CAMERA_SENSOR_PHOTO_WARMUP_FRAMES 9U

static camera_handler_instance_t *s_camera;
static bus_adapter_t *s_data_bus;
static uint8_t *s_buffer;
static int s_configured;
static int s_streaming;
static int s_recovery_required;
static int s_photo_configured;
static int s_quiescent = 1;
/* Restored from storage before open; retained across modes and recovery. */
static uint16_t s_rotation_degrees;

typedef struct
{
    uint32_t count;
    rt_tick_t first;
    rt_tick_t last;
    rt_tick_t min_interval;
    rt_tick_t max_interval;
} camera_sensor_event_timing_t;

static volatile int s_photo_timing_active;
static volatile camera_sensor_event_timing_t s_photo_vsync_timing;
static volatile camera_sensor_event_timing_t s_photo_frame_timing;

void __real_HAL_DCMI_IRQHandler(DCMI_HandleTypeDef *handle);

static void camera_sensor_record_event(volatile camera_sensor_event_timing_t *timing)
{
    rt_tick_t now = rt_tick_get();

    if (timing->count == 0U)
        timing->first = now;
    else
    {
        rt_tick_t interval = (rt_tick_t)(now - timing->last);

        if (timing->count == 1U || interval < timing->min_interval)
            timing->min_interval = interval;
        if (interval > timing->max_interval)
            timing->max_interval = interval;
    }
    timing->last = now;
    ++timing->count;
}

/* Observe enabled interrupt flags before HAL clears them. Do not change the
 * interrupt enable state or snapshot sequence; unobserved edges are unknown. */
void __wrap_HAL_DCMI_IRQHandler(DCMI_HandleTypeDef *handle)
{
    if (s_photo_timing_active && handle->Instance == hwp_dcmi)
    {
        uint32_t pending = handle->Instance->MIS;

        if ((pending & DCMI_FLAG_VSYNCRI) != 0U)
            camera_sensor_record_event(&s_photo_vsync_timing);
        if ((pending & DCMI_FLAG_FRAMERI) != 0U)
            camera_sensor_record_event(&s_photo_frame_timing);
    }
    __real_HAL_DCMI_IRQHandler(handle);
}

static void camera_sensor_photo_timing_begin(void)
{
    camera_sensor_event_timing_t empty = {0};
    rt_base_t level = rt_hw_interrupt_disable();

    s_photo_vsync_timing = empty;
    s_photo_frame_timing = empty;
    s_photo_timing_active = 1;
    rt_hw_interrupt_enable(level);
}

static void camera_sensor_print_timing(const char *event, const camera_sensor_event_timing_t *timing)
{
    unsigned long average_ms = timing->count > 1U ?
        (unsigned long)((uint64_t)(rt_tick_t)(timing->last - timing->first) *
                        1000U / RT_TICK_PER_SECOND / (timing->count - 1U)) : 0UL;

    rt_kprintf("camera.photo.timing: event=%s count=%lu interval_ms=%lu/%lu/%lu (avg/min/max)\n",
               event, (unsigned long)timing->count, average_ms,
               (unsigned long)((uint64_t)timing->min_interval * 1000U / RT_TICK_PER_SECOND),
               (unsigned long)((uint64_t)timing->max_interval * 1000U / RT_TICK_PER_SECOND));
}

static void camera_sensor_photo_timing_end(void)
{
    camera_sensor_event_timing_t vsync;
    camera_sensor_event_timing_t frame;
    rt_base_t level = rt_hw_interrupt_disable();

    s_photo_timing_active = 0;
    vsync = s_photo_vsync_timing;
    frame = s_photo_frame_timing;
    rt_hw_interrupt_enable(level);
    camera_sensor_print_timing("vsync", &vsync);
    camera_sensor_print_timing("frame", &frame);
}

static int camera_sensor_result(camera_handle_status_t status)
{
    switch (status)
    {
    case CAMERA_OK:
        return RT_EOK;
    case CAMERA_ERRORTIMEOUT:
        return -RT_ETIMEOUT;
    case CAMERA_ERRORRESOURCE:
        return -RT_EBUSY;
    case CAMERA_ERRORPARAMETER:
        return -RT_EINVAL;
    case CAMERA_ERRORNOMEMORY:
        return -RT_ENOMEM;
    default:
        return -RT_ERROR;
    }
}

/* A callback may have woken this thread before leaving the backend. Yield so
 * the worker can finish; an exhausted retry budget never releases ownership. */
static int camera_sensor_stop_idle(void)
{
    rt_tick_t started = rt_tick_get();
    rt_tick_t limit = rt_tick_from_millisecond(CAMERA_SENSOR_STOP_TIMEOUT_MS);
    int result;

    if (s_camera == RT_NULL)
    {
        s_quiescent = 1;
        return RT_EOK;
    }
    do
    {
        result = camera_sensor_result(camera_stop_stream(s_camera));
        if (result == RT_EOK)
            result = camera_sensor_result(camera_stop_capture(s_camera));
        if (result == RT_EOK)
        {
            s_streaming = 0;
            s_buffer = RT_NULL;
            s_quiescent = 1;
            return RT_EOK;
        }
        if (result != -RT_EBUSY || (rt_tick_t)(rt_tick_get() - started) >= limit)
            break;
        rt_thread_delay(1);
    } while (1);
    s_recovery_required = 1;
    s_quiescent = 0;
    return result;
}

int camera_sensor_is_quiescent(void)
{
    return s_quiescent;
}

int camera_sensor_close(void)
{
    int result;

    s_configured = 0;
    s_photo_configured = 0;
    result = camera_sensor_stop_idle();
    if (result != RT_EOK || s_camera == RT_NULL)
        return result;
    result = camera_sensor_result(camera_deinit(&s_camera));
    if (result != RT_EOK)
    {
        s_recovery_required = 1;
        return result;
    }
    s_streaming = 0;
    s_recovery_required = 0;
    s_buffer = RT_NULL;
    s_quiescent = 1;
    BSP_CAMERA_PowerDown();
    return RT_EOK;
}

int camera_sensor_open(void)
{
    const camera_capture_config_t configuration =
    {
        .pixformat = PIXFORMAT_RGB565,
        .framesize = FRAMESIZE_SVGA,
        .quality = 10,
    };
    int result;

    if (s_camera != RT_NULL && s_configured && !s_photo_configured && !s_recovery_required)
        return RT_EOK;
    if (s_camera != RT_NULL)
    {
        result = camera_sensor_close();
        if (result != RT_EOK)
            return result;
    }
    s_data_bus = bus_adapter_find(CAMERA_DATA_BUS_ADAPTER_NAME);
    if (s_data_bus == RT_NULL)
        return -RT_ENOSYS;
    BSP_CAMERA_PowerUp();
    result = camera_sensor_result(camera_handler_instance_init(&s_camera));
    if (result == RT_EOK)
        result = camera_sensor_result(camera_change_settings(s_camera, &configuration));
    if (result == RT_EOK)
        result = camera_sensor_result(camera_set_rotation(s_camera, s_rotation_degrees));
    if (result != RT_EOK)
    {
        if (s_camera != RT_NULL)
            (void)camera_sensor_close();
        else
            BSP_CAMERA_PowerDown();
        return result;
    }
    /* Preview exposure can continue converging after complete frames arrive. */
    rt_thread_mdelay(CAMERA_SENSOR_SETTLE_MS);
    s_configured = 1;
    s_photo_configured = 0;
    s_recovery_required = 0;
    s_quiescent = 1;
    return RT_EOK;
}

int camera_sensor_start(uint8_t *buffer, uint32_t size)
{
    camera_stream_config_t configuration = {0};
    int result;

    if (!s_configured || s_photo_configured || s_camera == RT_NULL || s_recovery_required)
        return -RT_ERROR;
    if (s_streaming)
        return -RT_EBUSY;
    if (buffer == RT_NULL || size != 2U * CAMERA_SENSOR_FRAME_SIZE ||
        ((uintptr_t)buffer & 63U) != 0U ||
        (uintptr_t)buffer > UINTPTR_MAX - size)
        return -RT_EINVAL;
    configuration.buffers[0] = buffer;
    configuration.buffers[1] = buffer + CAMERA_SENSOR_FRAME_SIZE;
    configuration.buffer_size = CAMERA_SENSOR_FRAME_SIZE;
    s_quiescent = 0;
    result = camera_sensor_result(camera_start_stream_mode(s_camera, &configuration,
                                    CAMERA_STREAM_MODE_FRAME));
    if (result != RT_EOK)
    {
        s_recovery_required = 1;
        (void)camera_sensor_stop_idle();
        return result;
    }
    s_buffer = buffer;
    s_streaming = 1;
    return RT_EOK;
}

int camera_sensor_stop(void)
{
    return camera_sensor_stop_idle();
}

static int camera_sensor_configure(framesize_t framesize)
{
    camera_capture_config_t configuration =
    {
        .pixformat = framesize == FRAMESIZE_UXGA ? PIXFORMAT_JPEG : PIXFORMAT_RGB565,
        .framesize = framesize,
        .quality = 10,
    };
    int result;

    if (s_camera == RT_NULL)
        return -RT_ERROR;
    result = camera_sensor_stop_idle();
    if (result != RT_EOK)
        return result;
    s_configured = 0;
    s_photo_configured = 0;
    result = camera_sensor_result(camera_change_settings(s_camera, &configuration));
    if (result == RT_EOK)
        result = camera_sensor_result(camera_set_rotation(s_camera, s_rotation_degrees));
    if (result != RT_EOK)
    {
        s_recovery_required = 1;
        return result;
    }
    s_configured = 1;
    s_photo_configured = framesize == FRAMESIZE_UXGA;
    s_recovery_required = 0;
    return RT_EOK;
}

int camera_sensor_configure_photo(void)
{
    return camera_sensor_configure(FRAMESIZE_UXGA);
}

int camera_sensor_restore_preview(void)
{
    return camera_sensor_configure(FRAMESIZE_SVGA);
}

int camera_sensor_set_rotation(uint16_t degrees)
{
    int result;

    if (degrees != 0U && degrees != 180U)
        return -RT_EINVAL;
    /* The media thread persists the target before requesting a live change.
     * Reopening after any error must apply that same saved target. */
    s_rotation_degrees = degrees;
    if (s_streaming || !s_quiescent)
        return -RT_EBUSY;
    if (s_camera == RT_NULL)
        return RT_EOK;
    if (!s_configured || s_recovery_required)
        return -RT_ERROR;
    result = camera_sensor_result(camera_set_rotation(s_camera, degrees));
    if (result != RT_EOK)
        s_recovery_required = 1;
    return result;
}

int camera_sensor_capture_photo(uint8_t *buffer, uint32_t capacity, uint32_t timeout_ms,
                                uint32_t *jpeg_size)
{
    camera_capture_request_t request = {0};
    int stop_result;
    int result;

    if (jpeg_size != RT_NULL)
        *jpeg_size = 0U;
    if (!s_configured || !s_photo_configured || s_camera == RT_NULL ||
        s_streaming || s_recovery_required || !s_quiescent)
        return -RT_ERROR;
    if (buffer == RT_NULL || jpeg_size == RT_NULL || capacity < 64U || (capacity & 63U) != 0U ||
        ((uintptr_t)buffer & 63U) != 0U ||
        (uintptr_t)buffer > UINTPTR_MAX - capacity ||
        timeout_ms == 0U || timeout_ms > INT32_MAX)
        return -RT_EINVAL;
    request.buffer = buffer;
    request.buffer_size = capacity;
    camera_sensor_photo_timing_begin();
    /* One continuous hardware run receives the settling frames and the saved
     * frame. The backend returns only the final JPEG at the buffer start. */
    s_quiescent = 0;
    result = camera_sensor_result(camera_capture_frames_timeout(s_camera, &request,
                                  CAMERA_SENSOR_PHOTO_WARMUP_FRAMES + 1U, timeout_ms));
    stop_result = camera_sensor_stop_idle();
    if (stop_result != RT_EOK)
    {
        result = stop_result;
        goto done;
    }
    if (result != RT_EOK)
        goto done;
    if (request.frame_size < 4U || request.frame_size > capacity ||
        buffer[0] != 0xffU || buffer[1] != 0xd8U ||
        buffer[request.frame_size - 2U] != 0xffU ||
        buffer[request.frame_size - 1U] != 0xd9U)
    {
        result = -RT_EIO;
        goto done;
    }
    *jpeg_size = request.frame_size;
    rt_kprintf("camera.photo: sensor_jpeg width=1600 height=1200 bytes=%lu warmup=%lu quality=10 mode=continuous\n",
               (unsigned long)*jpeg_size, (unsigned long)CAMERA_SENSOR_PHOTO_WARMUP_FRAMES);
done:
    camera_sensor_photo_timing_end();
    return result;
}

int camera_sensor_wait(camera_sensor_frame_t *frame, uint32_t timeout_ms)
{
    int result;

    if (frame == RT_NULL || !s_streaming || s_camera == RT_NULL || s_recovery_required)
        return -RT_EINVAL;
    memset(frame, 0, sizeof(*frame));
    result = camera_sensor_result(camera_get_stream_frame(s_camera, &frame->capture,
                                  rt_tick_from_millisecond(timeout_ms)));
    if (result != RT_EOK)
        return result;
    if (frame->capture.error != 0)
        return frame->capture.error;
    if (frame->capture.frame_size != CAMERA_SENSOR_FRAME_SIZE ||
        frame->capture.buffer_size != CAMERA_SENSOR_FRAME_SIZE ||
        (frame->capture.buffer != s_buffer &&
         frame->capture.buffer != s_buffer + CAMERA_SENSOR_FRAME_SIZE) ||
        frame->capture.event_type != CAMERA_STREAM_EVENT_FRAME ||
        !frame->capture.is_complete)
        return -RT_EIO;
    result = camera_sensor_result(camera_get_stream_dropped_count(
                                   s_camera, &frame->queue_dropped_total));
    return result;
}

static bus_stream_frame_t camera_sensor_bus_frame(const camera_sensor_frame_t *frame)
{
    bus_stream_frame_t result = {0};

    result.buffer = frame->capture.buffer;
    result.size = frame->capture.frame_size;
    result.sequence = frame->capture.sequence;
    result.error = frame->capture.error;
    result.event_type = BUS_STREAM_EVENT_FRAME;
    return result;
}

int camera_sensor_frame_is_valid(const camera_sensor_frame_t *frame)
{
    bus_stream_frame_t source;

    if (frame == RT_NULL || !frame->capture.is_complete)
        return 0;
    source = camera_sensor_bus_frame(frame);
    return bus_adapter_stream_frame_valid(s_data_bus, &source);
}

int camera_sensor_prepare_frame(const camera_sensor_frame_t *frame)
{
    if (!camera_sensor_frame_is_valid(frame))
        return 0;
    SCB_InvalidateDCache_by_Addr(frame->capture.buffer, frame->capture.frame_size);
    return camera_sensor_frame_is_valid(frame);
}
