/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CAMERA_IMAGE_H
#define CAMERA_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_IMAGE_MAX_WIDTH 1600U
#define CAMERA_IMAGE_MAX_HEIGHT 1200U

/*
 * Read the original dimensions of a supported baseline JPEG header.
 * Checks the header and SOI/EOI markers; pixel data is validated by decode.
 * Returns 0 on success, -1 on failure. Failed calls set dimensions to zero.
 * Probe and decode share an 8 KiB workspace and must be serialized.
 */
int camera_image_probe_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                            uint16_t *width, uint16_t *height);

/*
 * Decode one complete baseline JPEG to packed RGB565, low byte first.
 * scale 0..3 divides each dimension by 1, 2, 4 or 8, respectively.
 * The output buffer must hold width * height * 2 bytes and must not overlap
 * the JPEG input. On failure, dimensions are zero and pixels may be partial.
 * Returns 0 on success, -1 for invalid arguments or an unsupported/bad image.
 * Uses a shared 8 KiB workspace: calls must be serialized by the media thread.
 */
int camera_image_decode_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                             uint8_t *pixels, uint32_t capacity, uint8_t scale,
                             uint16_t *width, uint16_t *height);

#ifdef __cplusplus
}
#endif

#endif
