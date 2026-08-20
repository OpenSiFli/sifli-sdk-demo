/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2013-2024 OpenMV, LLC. (adapted RGB565 routines)
 *
 * Portable software ISP API. Algorithm code is in the adjacent core unit.
 */

#include "camera_sw_isp.h"

#include <stddef.h>

static const uint8_t camera_sw_isp_bayer_4x4[16] = {
    0U, 8U, 2U, 10U, 12U, 4U, 14U, 6U,
    3U, 11U, 1U, 9U, 15U, 7U, 13U, 5U,
};

int camera_sw_isp_quantize_rgb888(uint8_t *output, uint16_t x, uint16_t y,
                                  uint8_t red, uint8_t green, uint8_t blue,
                                  int dither_enabled)
{
    uint32_t threshold = dither_enabled ?
        ((uint32_t)camera_sw_isp_bayer_4x4[
            ((uint32_t)(y & 3U) << 2U) | (x & 3U)] * 255U) / 16U : 127U;
    uint32_t red_code;
    uint32_t green_code;
    uint32_t blue_code;

    if (output == NULL)
        return -1;
    red_code = ((uint32_t)red * 31U + threshold) / 255U;
    green_code = ((uint32_t)green * 63U + threshold) / 255U;
    blue_code = ((uint32_t)blue * 31U + threshold) / 255U;
    if (red_code > 31U)
        red_code = 31U;
    if (green_code > 63U)
        green_code = 63U;
    if (blue_code > 31U)
        blue_code = 31U;
    output[0] = (uint8_t)((red_code << 3U) | (green_code >> 3U));
    output[1] = (uint8_t)(((green_code & 0x7U) << 5U) | blue_code);
    return 0;
}

int camera_sw_isp_dither_rgb565_be(camera_sw_isp_frame_t *frame,
                                   int dither_enabled)
{
    uint16_t y;

    if ((frame == NULL) || (frame->data == NULL) || (frame->width == 0U) ||
        (frame->height == 0U) ||
        (frame->stride < (uint32_t)frame->width * 2U))
        return -1;
    if (!dither_enabled)
        return 0;
    for (y = 0U; y < frame->height; y++)
    {
        uint16_t x;
        uint8_t *row = frame->data + (uint32_t)y * frame->stride;

        for (x = 0U; x < frame->width; x++)
        {
            uint8_t *out = row + (uint32_t)x * 2U;
            uint16_t pixel = (uint16_t)((uint16_t)out[0] << 8U | out[1]);
            uint8_t red = (uint8_t)((((pixel >> 11U) & 0x1fU) * 255U +
                                     15U) / 31U);
            uint8_t green = (uint8_t)((((pixel >> 5U) & 0x3fU) * 255U +
                                       31U) / 63U);
            uint8_t blue = (uint8_t)(((pixel & 0x1fU) * 255U + 15U) / 31U);

            if (camera_sw_isp_quantize_rgb888(out, x, y, red, green, blue,
                                               1) != 0)
                return -1;
        }
    }
    return 0;
}

int camera_sw_isp_core_stats_be(const uint8_t *, uint16_t, uint16_t, uint32_t,
                                camera_sw_isp_stats_t *);
int camera_sw_isp_core_awb_apply_be(uint8_t *, uint16_t, uint16_t, uint32_t,
                                    const camera_sw_isp_stats_t *);
int camera_sw_isp_core_ccm_be(uint8_t *, uint16_t, uint16_t, uint32_t,
                              const float[9]);
int camera_sw_isp_core_gamma_be(uint8_t *, uint16_t, uint16_t, uint32_t,
                                float, float, float, uint8_t);
int camera_sw_isp_core_tone_build(const camera_sw_isp_stats_t *,
                                  camera_sw_isp_tone_map_t *);
int camera_sw_isp_core_tone_apply_be(uint8_t *, uint16_t, uint16_t, uint32_t,
                                     const camera_sw_isp_tone_map_t *);

static int camera_sw_frame_valid(const camera_sw_frame_t *frame)
{
    return (frame != NULL) &&
           (frame->data != NULL) && (frame->width != 0U) &&
           (frame->height != 0U) &&
           (frame->stride >= (uint32_t)frame->width * 2U);
}

int camera_sw_isp_stats(const camera_sw_frame_t *frame,
                        camera_sw_isp_stats_t *stats)
{
    if ((frame == NULL) || (stats == NULL))
        return -1;
    if (!camera_sw_frame_valid(frame))
        return -1;
    return camera_sw_isp_core_stats_be(frame->data, frame->width,
                                       frame->height, frame->stride, stats);
}

int camera_sw_isp_awb_apply(camera_sw_frame_t *frame,
                            const camera_sw_isp_stats_t *stats)
{
    if ((frame == NULL) || (stats == NULL))
        return -1;
    if (!camera_sw_frame_valid(frame))
        return -1;
    return camera_sw_isp_core_awb_apply_be(
        frame->data, frame->width, frame->height, frame->stride,
        stats);
}

int camera_sw_isp_ccm(camera_sw_frame_t *frame, const float matrix[9])
{
    if (frame == NULL)
        return -1;
    if (!camera_sw_frame_valid(frame))
        return -1;
    return camera_sw_isp_core_ccm_be(frame->data, frame->width, frame->height,
                                     frame->stride, matrix);
}

int camera_sw_isp_gamma(camera_sw_frame_t *frame, float gamma,
                        float contrast, float brightness,
                        uint8_t black_level)
{
    if (frame == NULL)
        return -1;
    if (!camera_sw_frame_valid(frame))
        return -1;
    return camera_sw_isp_core_gamma_be(frame->data, frame->width, frame->height,
                                       frame->stride, gamma, contrast,
                                       brightness, black_level);
}

int camera_sw_isp_tone_build(const camera_sw_isp_stats_t *stats,
                             camera_sw_isp_tone_map_t *map)
{
    if ((stats == NULL) || (map == NULL))
        return -1;
    return camera_sw_isp_core_tone_build(stats, map);
}

int camera_sw_isp_tone_apply(camera_sw_frame_t *frame,
                             const camera_sw_isp_tone_map_t *map)
{
    if ((frame == NULL) || (map == NULL))
        return -1;
    if (!camera_sw_frame_valid(frame))
        return -1;
    return camera_sw_isp_core_tone_apply_be(
        frame->data, frame->width, frame->height, frame->stride,
        map);
}
