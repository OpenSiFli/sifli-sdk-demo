/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2013-2024 OpenMV, LLC. (adapted RGB565 routines)
 *
 * Portable software ISP component. All buffers and frames are caller-owned;
 * the component never allocates memory and never stores frame pointers across
 * calls. Only CAMERA_SW_PIXEL_RGB565_BE frames are currently supported.
 */

#ifndef CAMERA_SW_ISP_H
#define CAMERA_SW_ISP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RGB565 big-endian is the only input and output representation. */
typedef struct
{
    uint8_t *data;
    uint16_t width;
    uint16_t height;
    uint32_t stride;
} camera_sw_isp_frame_t;

/* Compatibility is local to this component's public header. */
typedef camera_sw_isp_frame_t camera_sw_frame_t;

typedef struct
{
    uint32_t luminance_histogram[64];
    uint32_t red_sum;
    uint32_t green_sum;
    uint32_t blue_sum;
    uint32_t valid_pixels;
    uint32_t dark_pixels;
    uint32_t near_saturated_pixels;
    uint32_t saturated_pixels;
    uint8_t luminance_min;
    uint8_t luminance_max;
    uint8_t red_gain_q6;
    uint8_t blue_gain_q6;
} camera_sw_isp_stats_t;

typedef struct
{
    uint8_t black_point;
    uint8_t white_point;
    uint8_t lut[256];
} camera_sw_isp_tone_map_t;

int camera_sw_isp_stats(const camera_sw_frame_t *frame,
                        camera_sw_isp_stats_t *stats);
int camera_sw_isp_awb_apply(camera_sw_frame_t *frame,
                            const camera_sw_isp_stats_t *stats);
int camera_sw_isp_ccm(camera_sw_frame_t *frame, const float matrix[9]);
int camera_sw_isp_gamma(camera_sw_frame_t *frame, float gamma,
                        float contrast, float brightness,
                        uint8_t black_level);
int camera_sw_isp_tone_build(const camera_sw_isp_stats_t *stats,
                             camera_sw_isp_tone_map_t *map);
int camera_sw_isp_tone_apply(camera_sw_frame_t *frame,
                             const camera_sw_isp_tone_map_t *map);
uint32_t camera_sw_isp_filter_scratch_size(uint16_t width);
int camera_sw_isp_filter(camera_sw_frame_t *frame, uint8_t flat_range,
                         const int8_t kernel[9], void *scratch,
                         uint32_t scratch_size);
int camera_sw_isp_quantize_rgb888(uint8_t *output, uint16_t x, uint16_t y,
                                  uint8_t red, uint8_t green, uint8_t blue,
                                  int dither_enabled);
int camera_sw_isp_dither_rgb565_be(camera_sw_isp_frame_t *frame,
                                   int dither_enabled);

#ifdef __cplusplus
}
#endif

#endif
