#ifndef AI_CAMERA_GC032A_CAMERA_H
#define AI_CAMERA_GC032A_CAMERA_H

#include <stddef.h>
#include <stdint.h>

#include "gc032a_config.h"

typedef enum
{
    GC032A_CAMERA_OK = 0,
    GC032A_CAMERA_ERROR = -1,
    GC032A_CAMERA_INVALID = -2,
    GC032A_CAMERA_BUSY = -3,
    GC032A_CAMERA_TIMEOUT = -4,
} gc032a_camera_status_t;

typedef struct
{
    const uint8_t *data;
    size_t size;
    uint32_t sequence;
} gc032a_frame_t;

int gc032a_camera_init(void);
/* Both stream buffers remain caller-owned and must be exactly one VGA frame.
 * A frame returned by wait_frame is borrowed and may be overwritten by DMA
 * after the next completed frame; copy it before retaining it. */
int gc032a_camera_start(uint8_t *first, uint8_t *second, size_t frame_size);
int gc032a_camera_wait_frame(gc032a_frame_t *frame, int32_t timeout_ticks);
int gc032a_camera_stop(void);
int gc032a_camera_capture_one(uint8_t *buffer, size_t capacity,
                              size_t *captured_size);
/* Stop/deinitialize the sole SDK handle, then reopen the fixed DVP mode. */
int gc032a_camera_recover(void);
void gc032a_camera_deinit(void);

#endif