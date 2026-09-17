#include "gc032a_camera.h"

#include <rtthread.h>

#include "gc032a_serial.h"

#define GC032A_CAPTURE_TIMEOUT_MS 2000U

typedef enum
{
    GC032A_STATE_CLOSED = 0,
    GC032A_STATE_READY,
    GC032A_STATE_STREAMING,
} gc032a_state_t;

static gc032a_state_t s_state;
static uint8_t *s_stream_buffer;
static uint32_t s_sequence;

int gc032a_camera_init(void)
{
    if (s_state != GC032A_STATE_CLOSED)
        return GC032A_CAMERA_OK;
    if (gc032a_serial_open() != 0)
    {
        rt_kprintf("camera: GC032A serial init failed\n");
        return GC032A_CAMERA_ERROR;
    }
    s_state = GC032A_STATE_READY;
    rt_kprintf("camera: GC032A 2-line serial 640x480 RGB565\n");
    return GC032A_CAMERA_OK;
}

int gc032a_camera_start(uint8_t *first, uint8_t *second, size_t frame_size)
{
    if (first == RT_NULL || frame_size != GC032A_FRAME_SIZE)
        return GC032A_CAMERA_INVALID;
    if (s_state == GC032A_STATE_STREAMING)
        return GC032A_CAMERA_BUSY;
    if (gc032a_camera_init() != GC032A_CAMERA_OK)
        return GC032A_CAMERA_ERROR;
    /* The serial front end captures one frame at a time into a single buffer,
     * so the caller's second slot is accepted but not required. */
    (void)second;
    s_stream_buffer = first;
    s_sequence = 0U;
    s_state = GC032A_STATE_STREAMING;
    return GC032A_CAMERA_OK;
}

int gc032a_camera_wait_frame(gc032a_frame_t *frame, int32_t timeout_ticks)
{
    uint32_t captured = 0U;
    uint32_t timeout_ms;

    if (frame == RT_NULL || s_state != GC032A_STATE_STREAMING ||
        s_stream_buffer == RT_NULL)
        return GC032A_CAMERA_INVALID;
    timeout_ms = (timeout_ticks > 0)
                     ? (uint32_t)(((uint32_t)timeout_ticks * 1000U) /
                                  RT_TICK_PER_SECOND)
                     : GC032A_CAPTURE_TIMEOUT_MS;
    if (timeout_ms == 0U)
        timeout_ms = GC032A_CAPTURE_TIMEOUT_MS;
    if (gc032a_serial_capture(s_stream_buffer, GC032A_FRAME_SIZE, timeout_ms,
                              &captured) != 0)
        return GC032A_CAMERA_ERROR;
    frame->data = s_stream_buffer;
    frame->size = captured;
    frame->sequence = ++s_sequence;
    return GC032A_CAMERA_OK;
}

int gc032a_camera_stop(void)
{
    if (s_state != GC032A_STATE_STREAMING)
        return GC032A_CAMERA_OK;
    s_state = GC032A_STATE_READY;
    s_stream_buffer = RT_NULL;
    return GC032A_CAMERA_OK;
}

int gc032a_camera_capture_one(uint8_t *buffer, size_t capacity,
                              size_t *captured_size)
{
    uint32_t captured = 0U;

    if (buffer == RT_NULL || captured_size == RT_NULL ||
        capacity < GC032A_FRAME_SIZE)
        return GC032A_CAMERA_INVALID;
    *captured_size = 0U;
    if (s_state == GC032A_STATE_STREAMING)
        return GC032A_CAMERA_BUSY;
    if (gc032a_camera_init() != GC032A_CAMERA_OK)
        return GC032A_CAMERA_ERROR;
    if (gc032a_serial_capture(buffer, GC032A_FRAME_SIZE,
                              GC032A_CAPTURE_TIMEOUT_MS, &captured) != 0)
        return GC032A_CAMERA_ERROR;
    *captured_size = captured;
    return GC032A_CAMERA_OK;
}

int gc032a_camera_recover(void)
{
    /* A live preview session owns the front end: deinit would close the DCMI
     * device under the preview thread and clear its streaming state. */
    if (s_state == GC032A_STATE_STREAMING)
        return GC032A_CAMERA_BUSY;
    gc032a_camera_deinit();
    return gc032a_camera_init();
}

void gc032a_camera_deinit(void)
{
    gc032a_serial_close();
    s_stream_buffer = RT_NULL;
    s_state = GC032A_STATE_CLOSED;
}