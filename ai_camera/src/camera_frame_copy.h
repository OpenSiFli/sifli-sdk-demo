/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AI_CAMERA_FRAME_COPY_H
#define AI_CAMERA_FRAME_COPY_H

#include <stdint.h>

typedef struct
{
    uint32_t total_ms;
    uint32_t submit_ms;
    uint32_t resume_ms;
    uint32_t prepare_ms;
    uint32_t finish_ms;
} camera_frame_copy_timing_t;

/* Copy one completed RGB565 half from DCMI into a private frame using GPDMA.
 * src and dst are full-frame bases; half is 0 for top and 1 for bottom.
 * Width and height are in pixels. Height must be even, buffers must not
 * overlap, and their bases and half sizes must be aligned to 32 bytes.
 *
 * Call serially from one thread. DCMI owns the source and the CPU must not
 * write it. The caller keeps both buffers borrowed until return, then checks
 * the source half's capture generation. The destination is exclusive during
 * copying. A failed or timed-out DMA is stopped before either buffer is
 * released; destination cache maintenance is complete on return.
 *
 * timing may be NULL. All durations are whole milliseconds rounded down
 * from RT-Thread ticks. prepare_ms includes DMA initialization and source/
 * destination cache maintenance before submission. submit_ms covers
 * HAL_DMA_Start_IT(), including channel allocation and HAL cache cleaning.
 * resume_ms covers successful completion callback to thread resumption;
 * finish_ms covers destination cache invalidation after DMA stops.
 * Returns RT_EOK or a negative RT-Thread error.
 */
int camera_frame_copy_half(uint8_t *dst, const uint8_t *src,
                           uint16_t width, uint16_t height, unsigned int half,
                           camera_frame_copy_timing_t *timing);

#endif
