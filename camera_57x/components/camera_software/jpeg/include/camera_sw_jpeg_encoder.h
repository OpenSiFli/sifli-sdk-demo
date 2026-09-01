/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2021 BitBank Software, Inc.; Larry Bank
 *
 * Portable JPEG encoder component built on the BitBank JPEGENC core. The
 * caller owns the input frame and the output buffer; this component does not
 * allocate memory and does not retain pointers across calls.
 */

#ifndef CAMERA_SW_JPEG_ENCODER_H
#define CAMERA_SW_JPEG_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#include "camera_sw_jpeg_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    CAMERA_SW_JPEG_QUALITY_BEST = 0,
    CAMERA_SW_JPEG_QUALITY_HIGH,
    CAMERA_SW_JPEG_QUALITY_MEDIUM,
    CAMERA_SW_JPEG_QUALITY_LOW,
} camera_sw_jpeg_quality_t;

typedef int (*camera_sw_jpeg_sink_fn)(void *context, const uint8_t *data,
                                      size_t size);

typedef struct
{
    void *workspace;
    uint32_t workspace_size;
} camera_sw_jpeg_encoder_t;

#define CAMERA_SW_JPEG_ENCODER_STATIC_WORKSPACE_SIZE 4096U

uint32_t camera_sw_jpeg_encoder_workspace_size(void);
int camera_sw_jpeg_encoder_init(camera_sw_jpeg_encoder_t *encoder,
                                void *workspace, uint32_t workspace_size);
int camera_sw_jpeg_encode_to_buffer(camera_sw_jpeg_encoder_t *encoder,
                                    const camera_sw_jpeg_frame_t *frame,
                                    camera_sw_jpeg_quality_t quality,
                                    uint8_t *output, uint32_t output_capacity,
                                    uint32_t *output_size);
int camera_sw_jpeg_encode_to_sink(camera_sw_jpeg_encoder_t *encoder,
                                  const camera_sw_jpeg_frame_t *frame,
                                  camera_sw_jpeg_quality_t quality,
                                  camera_sw_jpeg_sink_fn sink,
                                  void *sink_context, uint32_t *output_size);

#ifdef __cplusplus
}
#endif

#endif
