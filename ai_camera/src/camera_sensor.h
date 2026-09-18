/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AI_CAMERA_SENSOR_H
#define AI_CAMERA_SENSOR_H

#include <stdint.h>
#include <camera_handle.h>

#define CAMERA_SENSOR_PHOTO_WIDTH 1600U
#define CAMERA_SENSOR_PHOTO_HEIGHT 1200U

typedef struct
{
    camera_stream_frame_t capture;
    /* Cumulative queue drops since this stream started, sampled after dequeue. */
    uint32_t queue_dropped_total;
} camera_sensor_frame_t;

/* Sensor lifecycle and configuration run on the stream thread. */
int camera_sensor_open(void);
int camera_sensor_close(void);
/* buffer holds two contiguous, 64-byte-aligned RGB565 images, and size is
 * their total byte capacity. DCMI continuously alternates between the two images. */
int camera_sensor_start(uint8_t *buffer, uint32_t size);
int camera_sensor_stop(void);
/* True only after DMA and completion callbacks have released their buffers.
 * If false after any error, keep all camera buffers reserved until a later
 * successful stop/close. This query does not stop the camera. */
int camera_sensor_is_quiescent(void);
int camera_sensor_configure_photo(void);
/* Select 0/180 degrees, including before open. When capture is active, cache
 * the target for recovery and return busy without touching sensor registers. */
int camera_sensor_set_rotation(uint16_t degrees);
/* Start continuous reception once for ten complete UXGA JPEG images. Discard
 * the first nine frames and retain the tenth at the start of the 64-byte-aligned
 * buffer. capacity covers the whole receive sequence, without an extra timed wait.
 * timeout_ms covers the capture sequence; stopping may take a further 200 ms.
 * jpeg_size returns only the final JPEG length. Check is_quiescent after failure. */
int camera_sensor_capture_photo(uint8_t *buffer, uint32_t capacity, uint32_t timeout_ms,
                                uint32_t *jpeg_size);
/* Reapply the original SVGA profile; the caller discards resumed preview frames. */
int camera_sensor_restore_preview(void);
/* Dequeue one completed-frame event without stopping continuous capture. */
int camera_sensor_wait(camera_sensor_frame_t *frame, uint32_t timeout_ms);
/* These queries use the static bus backend and may run on the UI thread. */
int camera_sensor_frame_is_valid(const camera_sensor_frame_t *frame);
/* Invalidate the CPU cache for a completed DMA frame. This does not stop DMA
 * or prevent reuse; validate the generation again after GPU reads finish. */
int camera_sensor_prepare_frame(const camera_sensor_frame_t *frame);

#endif
