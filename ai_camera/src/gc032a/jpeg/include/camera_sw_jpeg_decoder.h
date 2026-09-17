/*
 * SPDX-License-Identifier: LicenseRef-ChaN-TJpgDec
 *
 * Copyright (C) 2021, ChaN
 *
 * Portable JPEG decoder component built on the Tiny JPEG Decompressor core.
 * The caller owns the input stream, the work buffer, and the output frame;
 * this component does not allocate memory and does not retain pointers across
 * calls.
 *
 * Output capacity contract: before the call, the caller sets output->data,
 * output->stride (destination row bytes), and output->height (available
 * rows). On success the component overwrites output->width, output->height,
 * output->stride, and output->pixel_format; decoded frames are tightly packed
 * RGB565_BE with stride equal to width * 2.
 *
 * `reader`, `reader_context`, `work`, and `output` must all be non-NULL; the
 * component rejects NULL contexts because it cannot otherwise guarantee that
 * the reader callback receives a valid context.
 */

#ifndef CAMERA_SW_JPEG_DECODER_H
#define CAMERA_SW_JPEG_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "camera_sw_jpeg_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef size_t (*camera_sw_jpeg_reader_fn)(void *context, uint8_t *buffer,
                                           size_t size);

int camera_sw_jpeg_decode(camera_sw_jpeg_reader_fn reader,
                          void *reader_context, uint8_t *work,
                          uint32_t work_size, camera_sw_jpeg_frame_t *output);

#ifdef __cplusplus
}
#endif

#endif
