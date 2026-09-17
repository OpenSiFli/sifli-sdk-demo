/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_sensor.h"

#include <bf0_hal.h>
#include <drv_io.h>
#include <rtthread.h>
#include <string.h>

#include "camera_service.h"

#if !defined(SENSOR_USING_OV2640) || !defined(CAMERA_OV2640_INTERFACE_DVP) || \
    !defined(CAMERA_DVP_BACKEND_DCMI)
#error "This application requires OV2640 8-bit DVP through the hardware DCMI backend"
#endif

#define CAMERA_SENSOR_FRAME_SIZE (CAMERA_LIVE_WIDTH * CAMERA_LIVE_HEIGHT * 2U)
#define CAMERA_SENSOR_SETTLE_MS 200U
#define CAMERA_SENSOR_STOP_TIMEOUT_MS 200U
#define CAMERA_SENSOR_PHOTO_WARMUP_FRAMES 10U

static camera_handler_instance_t *s_camera;
static uint8_t *s_buffer;
static int s_configured;
static int s_streaming;
static int s_recovery_required;
static int s_photo_configured;
static int s_quiescent = 1;
/* Restored from storage before open; retained across modes and recovery. */
static uint16_t s_rotation_degrees;

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
        .pixformat = PIXFORMAT_RGB565,
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

int camera_sensor_capture_photo(uint8_t *buffer, uint32_t capacity, uint32_t timeout_ms)
{
    camera_capture_request_t request = {0};
    rt_tick_t started;
    uint32_t frame;
    int result = RT_EOK;

    if (!s_configured || !s_photo_configured || s_camera == RT_NULL ||
        s_streaming || s_recovery_required || !s_quiescent)
        return -RT_ERROR;
    if (buffer == RT_NULL || capacity < CAMERA_SENSOR_PHOTO_SIZE ||
        ((uintptr_t)buffer & 63U) != 0U ||
        (uintptr_t)buffer > UINTPTR_MAX - CAMERA_SENSOR_PHOTO_SIZE ||
        timeout_ms == 0U || timeout_ms > INT32_MAX)
        return -RT_EINVAL;
    request.buffer = buffer;
    request.buffer_size = CAMERA_SENSOR_PHOTO_SIZE;
    started = rt_tick_get();
    /* Discard settling frames before the final exposure. The sensor keeps
     * running while MCU receive is stopped. */
    for (frame = 0U; frame <= CAMERA_SENSOR_PHOTO_WARMUP_FRAMES; ++frame)
    {
        uint64_t elapsed = (uint64_t)(rt_tick_t)(rt_tick_get() - started) *
                           1000U / RT_TICK_PER_SECOND;
        int stop_result;

        if (elapsed >= timeout_ms)
            return -RT_ETIMEOUT;
        request.frame_size = 0U;
        s_quiescent = 0;
        result = camera_sensor_result(camera_capture_single_timeout(s_camera, &request,
                                      timeout_ms - (uint32_t)elapsed));
        stop_result = camera_sensor_stop_idle();
        if (stop_result != RT_EOK)
            return stop_result;
        if (result != RT_EOK)
            return result;
        if (request.frame_size != CAMERA_SENSOR_PHOTO_SIZE)
            return -RT_EIO;
    }
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

int camera_sensor_frame_is_valid(const camera_sensor_frame_t *frame)
{
    return frame != RT_NULL && s_camera != RT_NULL && s_streaming &&
           !s_recovery_required && frame->capture.error == 0 &&
           camera_stream_frame_is_valid(s_camera, &frame->capture);
}
