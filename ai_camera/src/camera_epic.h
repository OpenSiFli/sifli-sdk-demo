/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AI_CAMERA_EPIC_H
#define AI_CAMERA_EPIC_H

#include <stdint.h>

typedef struct
{
    uint32_t total_ms;  /* Cache maintenance through function completion. */
    uint32_t submit_ms; /* drv_epic_copy(), including shared EPIC acquisition. */
    uint32_t resume_ms; /* Final copy callback to waiting thread resumption. */
} camera_epic_timing_t;

/* Render one complete RGB565 frame into a private frame buffer.
 * Width and height are in pixels and must be in 1..INT16_MAX+1. Buffers
 * must not overlap; their bases and full frame sizes must be aligned to
 * 32 bytes. The frame size must fit the signed cache-maintenance length.
 *
 * Call serially from one thread after the shared EPIC driver is open. The
 * caller keeps both buffers borrowed until return and checks the source
 * frame's capture generation afterwards. DCMI owns the source; the CPU must
 * not write it. The destination is exclusive while rendering. The driver
 * performs destination cache maintenance before the final copy callback.
 * Error recovery drains the driver before releasing either buffer.
 *
 * timing may be NULL. Durations are whole milliseconds rounded down from
 * RT-Thread ticks; resume_ms is zero when submission or the completion wait
 * fails, including callbacks received during error recovery.
 * Returns RT_EOK or a negative RT-Thread error. The shared driver's timeout
 * recovery may invoke completion after an abort; source validity is checked
 * separately by the caller.
 */
int camera_epic_render_frame(uint8_t *dst, const uint8_t *src,
                             uint16_t width, uint16_t height,
                             camera_epic_timing_t *timing);

#endif
