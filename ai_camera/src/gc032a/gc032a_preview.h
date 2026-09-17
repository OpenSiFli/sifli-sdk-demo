#ifndef AI_CAMERA_GC032A_PREVIEW_H
#define AI_CAMERA_GC032A_PREVIEW_H

#include <stdint.h>

int gc032a_preview_start(void);
int gc032a_preview_stop(void);
int gc032a_preview_recover(void);
int gc032a_preview_is_running(void);

/* Hand the next complete 640x480 RGB565 frame to the caller's buffer, which
 * must hold GC032A_FRAME_SIZE bytes.  The preview thread copies the frame right
 * after capturing it and keeps running, so the live picture only stutters for
 * that one copy.  The caller's buffer must stay untouched until this returns. */
int gc032a_preview_deliver_frame(uint8_t *dst, uint32_t capacity,
                                 uint32_t timeout_ms);

#endif