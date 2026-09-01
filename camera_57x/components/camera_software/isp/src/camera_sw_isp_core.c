/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2013-2024 OpenMV, LLC.
 *
 * RGB565 routines adapted from OpenMV imlib/isp.c.
 */

#include "camera_sw_isp.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define AWB_GAIN_ONE_Q8 256U
#define AWB_GAIN_MIN_Q8 128U
#define AWB_GAIN_MAX_Q8 512U
#define AWB_GAIN_DEADBAND_Q8 8U

static uint16_t rgb565_be_load(const uint8_t *pixel)
{
    return (uint16_t)(((uint16_t)pixel[0] << 8) | pixel[1]);
}

static void rgb565_be_store(uint8_t *pixel, uint16_t value)
{
    pixel[0] = (uint8_t)(value >> 8);
    pixel[1] = (uint8_t)value;
}

static uint16_t rgb565_pack(uint32_t red, uint32_t green, uint32_t blue)
{
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static int rgb565_image_valid(const void *pixels, uint16_t width,
                              uint16_t height, uint32_t stride)
{
    return (pixels != NULL) && (width != 0U) && (height != 0U) &&
           (stride >= (uint32_t)width * 2U);
}

static int clamp_channel(int value, int maximum)
{
    if (value < 0)
        return 0;
    return value > maximum ? maximum : value;
}

static uint32_t awb_minimum_candidates(uint16_t width, uint16_t height)
{
    return ((uint32_t)width * height + 99U) / 100U;
}

static uint32_t awb_gain_q8(uint32_t green_sum, uint32_t channel_sum)
{
    uint64_t numerator = (uint64_t)green_sum * AWB_GAIN_ONE_Q8;
    uint32_t gain =
        (uint32_t)((numerator + channel_sum / 2U) / channel_sum);

    if (gain < AWB_GAIN_MIN_Q8)
        gain = AWB_GAIN_MIN_Q8;
    else if (gain > AWB_GAIN_MAX_Q8)
        gain = AWB_GAIN_MAX_Q8;
    if ((gain >= AWB_GAIN_ONE_Q8 - AWB_GAIN_DEADBAND_Q8) &&
        (gain <= AWB_GAIN_ONE_Q8 + AWB_GAIN_DEADBAND_Q8))
        return AWB_GAIN_ONE_Q8;
    return gain;
}

static int ccm_is_identity(const float matrix[9])
{
    return (matrix[0] == 1.0f) && (matrix[1] == 0.0f) &&
           (matrix[2] == 0.0f) && (matrix[3] == 0.0f) &&
           (matrix[4] == 1.0f) && (matrix[5] == 0.0f) &&
           (matrix[6] == 0.0f) && (matrix[7] == 0.0f) &&
           (matrix[8] == 1.0f);
}

static void tone_identity(camera_sw_isp_tone_map_t *map)
{
    uint32_t index;

    map->black_point = 0U;
    map->white_point = 255U;
    for (index = 0U; index < 256U; index++)
        map->lut[index] = (uint8_t)index;
}

static uint32_t bayer_offset(uint32_t x, uint32_t y, uint32_t step)
{
    static const uint8_t bayer[16] = {
        0U, 8U, 2U, 10U,
        12U, 4U, 14U, 6U,
        3U, 11U, 1U, 9U,
        15U, 7U, 13U, 5U,
    };
    uint32_t value = bayer[(y & 3U) * 4U + (x & 3U)];

    return ((2U * value + 1U) * step + 16U) / 32U;
}

int camera_sw_isp_core_stats_be(const uint8_t *pixels, uint16_t width,
                           uint16_t height, uint32_t stride,
                           camera_sw_isp_stats_t *stats)
{
    uint32_t y;

    if (!rgb565_image_valid(pixels, width, height, stride) ||
        (stats == NULL))
        return -1;

    memset(stats, 0, sizeof(*stats));
    stats->luminance_min = 255U;
    stats->red_gain_q6 = 64U;
    stats->blue_gain_q6 = 64U;
    for (y = 0U; y < height; y++)
    {
        const uint8_t *row = pixels + y * stride;
        uint32_t x;

        for (x = 0U; x < width; x++)
        {
            uint16_t pixel = rgb565_be_load(row + x * 2U);
            uint32_t red = (pixel >> 11) & 0x1fU;
            uint32_t green = (pixel >> 5) & 0x3fU;
            uint32_t blue = pixel & 0x1fU;
            uint32_t red8 = (red << 3) | (red >> 2);
            uint32_t green8 = (green << 2) | (green >> 4);
            uint32_t blue8 = (blue << 3) | (blue >> 2);
            uint32_t luminance =
                (77U * red8 + 150U * green8 + 29U * blue8) >> 8;
            uint32_t red6 = red * 2U;
            uint32_t blue6 = blue * 2U;
            uint32_t minimum = red6 < green ? red6 : green;
            uint32_t maximum = red6 > green ? red6 : green;
            uint32_t chroma_limit;

            if (blue6 < minimum)
                minimum = blue6;
            if (blue6 > maximum)
                maximum = blue6;
            chroma_limit = maximum / 2U;
            if (chroma_limit < 4U)
                chroma_limit = 4U;

            stats->luminance_histogram[luminance >> 2]++;
            if (luminance < stats->luminance_min)
                stats->luminance_min = (uint8_t)luminance;
            if (luminance > stats->luminance_max)
                stats->luminance_max = (uint8_t)luminance;
            if (luminance <= 8U)
                stats->dark_pixels++;
            else if (luminance >= 240U)
                stats->saturated_pixels++;
            if ((luminance > 16U) && (luminance < 224U) &&
                ((maximum - minimum) <= chroma_limit))
            {
                stats->red_sum += red6;
                stats->green_sum += green;
                stats->blue_sum += blue6;
                stats->valid_pixels++;
            }
            if (luminance >= 224U)
                stats->near_saturated_pixels++;
        }
    }
    if ((stats->valid_pixels >= awb_minimum_candidates(width, height)) &&
        (stats->red_sum != 0U) && (stats->green_sum != 0U) &&
        (stats->blue_sum != 0U))
    {
        uint32_t red_gain = awb_gain_q8(stats->green_sum, stats->red_sum);
        uint32_t blue_gain = awb_gain_q8(stats->green_sum, stats->blue_sum);

        stats->red_gain_q6 = (uint8_t)((red_gain + 2U) >> 2);
        stats->blue_gain_q6 = (uint8_t)((blue_gain + 2U) >> 2);
    }
    return 0;
}

int camera_sw_isp_core_awb_apply_be(uint8_t *pixels, uint16_t width,
                               uint16_t height, uint32_t stride,
                               const camera_sw_isp_stats_t *stats)
{
    uint32_t red_gain;
    uint32_t blue_gain;
    uint32_t minimum_candidates;
    uint32_t y;

    if (!rgb565_image_valid(pixels, width, height, stride) || (stats == NULL))
        return -1;
    minimum_candidates = awb_minimum_candidates(width, height);
    if (stats->valid_pixels < minimum_candidates)
        return 0;
    if ((stats->red_sum == 0U) || (stats->green_sum == 0U) ||
        (stats->blue_sum == 0U))
        return -1;

    red_gain = awb_gain_q8(stats->green_sum, stats->red_sum);
    blue_gain = awb_gain_q8(stats->green_sum, stats->blue_sum);
    if ((red_gain == AWB_GAIN_ONE_Q8) &&
        (blue_gain == AWB_GAIN_ONE_Q8))
        return 0;
    for (y = 0U; y < height; y++)
    {
        uint8_t *row = pixels + y * stride;
        uint32_t x;

        for (x = 0U; x < width; x++)
        {
            uint8_t *out = row + x * 2U;
            uint16_t pixel = rgb565_be_load(out);
            uint32_t red =
                ((((pixel >> 11) & 0x1fU) * red_gain) + 128U) >> 8;
            uint32_t green = (pixel >> 5) & 0x3fU;
            uint32_t blue =
                (((pixel & 0x1fU) * blue_gain) + 128U) >> 8;
            uint32_t red6 = red * 2U;
            uint32_t blue6 = blue * 2U;
            uint32_t peak = red6 > green ? red6 : green;

            if (blue6 > peak)
                peak = blue6;
            if (peak > 63U)
            {
                red6 = (red6 * 63U + peak / 2U) / peak;
                green = (green * 63U + peak / 2U) / peak;
                blue6 = (blue6 * 63U + peak / 2U) / peak;
                red = (red6 + 1U) >> 1;
                blue = (blue6 + 1U) >> 1;
            }
            if (red > 31U)
                red = 31U;
            if (blue > 31U)
                blue = 31U;
            rgb565_be_store(out, rgb565_pack(red, green, blue));
        }
    }
    return 0;
}

int camera_sw_isp_core_ccm_be(uint8_t *pixels, uint16_t width, uint16_t height,
                         uint32_t stride, const float matrix[9])
{
    int coefficients[9];
    uint32_t index;
    uint32_t y;

    if (!rgb565_image_valid(pixels, width, height, stride) ||
        (matrix == NULL))
        return -1;
    if (ccm_is_identity(matrix))
        return 0;
    for (index = 0U; index < 9U; index++)
    {
        float scale = (index % 3U) == 1U ? 32.0f : 64.0f;
        int maximum = scale == 32.0f ? 512 : 1024;
        int value;

        if (!isfinite(matrix[index]))
            return -1;
        value = (int)lroundf(matrix[index] * scale);
        coefficients[index] = value > maximum ? maximum : value;
    }

    for (y = 0U; y < height; y++)
    {
        uint8_t *row = pixels + y * stride;
        uint32_t x;

        for (x = 0U; x < width; x++)
        {
            uint8_t *out = row + x * 2U;
            uint16_t pixel = rgb565_be_load(out);
            int red = (pixel >> 11) & 0x1f;
            int green = (pixel >> 5) & 0x3f;
            int blue = pixel & 0x1f;
            int new_red = clamp_channel(
                (coefficients[0] * red + coefficients[1] * green +
                 coefficients[2] * blue +
                 (int)bayer_offset(x, y, 64U)) >> 6, 31);
            int new_green = clamp_channel(
                (coefficients[3] * red + coefficients[4] * green +
                 coefficients[5] * blue +
                 (int)bayer_offset(x, y, 32U)) >> 5, 63);
            int new_blue = clamp_channel(
                (coefficients[6] * red + coefficients[7] * green +
                 coefficients[8] * blue +
                 (int)bayer_offset(x, y, 64U)) >> 6, 31);

            rgb565_be_store(out, rgb565_pack((uint32_t)new_red,
                                              (uint32_t)new_green,
                                              (uint32_t)new_blue));
        }
    }
    return 0;
}

int camera_sw_isp_core_gamma_be(uint8_t *pixels, uint16_t width, uint16_t height,
                           uint32_t stride, float gamma, float contrast,
                           float brightness, uint8_t black_level)
{
    uint8_t luminance_lut[256];
    float black;
    uint32_t index;
    uint32_t y;

    if (!rgb565_image_valid(pixels, width, height, stride) ||
        !isfinite(gamma) || !isfinite(contrast) || !isfinite(brightness) ||
        (gamma <= 0.0f) || (black_level == UINT8_MAX))
        return -1;
    if ((gamma == 1.0f) && (contrast == 1.0f) && (brightness == 0.0f) &&
        (black_level == 0U))
        return 0;

    black = black_level / 255.0f;
    gamma = 1.0f / gamma;
    for (index = 0U; index < 256U; index++)
    {
        float normalized = index / 255.0f;
        int value;

        normalized = normalized <= black ? 0.0f :
                     (normalized - black) / (1.0f - black);
        value = (int)lroundf((powf(normalized, gamma) * contrast +
                                  brightness) * 255.0f);

        luminance_lut[index] = (uint8_t)clamp_channel(value, 255);
    }
    for (y = 0U; y < height; y++)
    {
        uint8_t *row = pixels + y * stride;
        uint32_t x;

        for (x = 0U; x < width; x++)
        {
            uint8_t *out = row + x * 2U;
            uint16_t pixel = rgb565_be_load(out);
            uint32_t red = (pixel >> 11) & 0x1fU;
            uint32_t green = (pixel >> 5) & 0x3fU;
            uint32_t blue = pixel & 0x1fU;
            uint32_t red8 = (red << 3) | (red >> 2);
            uint32_t green8 = (green << 2) | (green >> 4);
            uint32_t blue8 = (blue << 3) | (blue >> 2);
            uint32_t luminance =
                (77U * red8 + 150U * green8 + 29U * blue8) >> 8;
            int delta = (int)luminance_lut[luminance] - (int)luminance;
            int out_red = clamp_channel((int)red8 + delta, 255);
            int out_green = clamp_channel((int)green8 + delta, 255);
            int out_blue = clamp_channel((int)blue8 + delta, 255);

            rgb565_be_store(out, rgb565_pack(
                (uint32_t)clamp_channel((out_red + 4) >> 3, 31),
                (uint32_t)clamp_channel((out_green + 2) >> 2, 63),
                (uint32_t)clamp_channel((out_blue + 4) >> 3, 31)));
        }
    }
    return 0;
}

int camera_sw_isp_core_tone_build(const camera_sw_isp_stats_t *stats,
                             camera_sw_isp_tone_map_t *map)
{
    uint64_t total = 0U;
    uint64_t cumulative = 0U;
    uint64_t clip_count;
    uint32_t low_bin = 0U;
    uint32_t high_bin = 63U;
    uint32_t black;
    uint32_t white;
    uint32_t span;
    uint32_t index;

    if ((stats == NULL) || (map == NULL))
        return -1;
    for (index = 0U; index < 64U; index++)
        total += stats->luminance_histogram[index];
    tone_identity(map);
    if (total == 0U)
        return 0;

    clip_count = total / 100U;
    for (index = 0U; index < 64U; index++)
    {
        cumulative += stats->luminance_histogram[index];
        if (cumulative > clip_count)
        {
            low_bin = index;
            break;
        }
    }
    cumulative = 0U;
    for (index = 64U; index > 0U; index--)
    {
        cumulative += stats->luminance_histogram[index - 1U];
        if (cumulative > clip_count)
        {
            high_bin = index - 1U;
            break;
        }
    }
    if ((high_bin <= low_bin) || ((high_bin - low_bin) <= 16U))
        return 0;

    black = low_bin * 4U;
    if (black > 24U)
        black = 24U;
    white = high_bin * 4U + 3U;
    if (white < 231U)
        white = 231U;
    span = white - black;
    map->black_point = (uint8_t)black;
    map->white_point = (uint8_t)white;
    for (index = 0U; index < 256U; index++)
    {
        uint32_t stretched;

        if (index <= black)
            stretched = 0U;
        else if (index >= white)
            stretched = 255U;
        else
            stretched = ((index - black) * 255U + span / 2U) / span;
        map->lut[index] = (uint8_t)((index + stretched + 1U) / 2U);
    }
    return 0;
}

int camera_sw_isp_core_tone_apply_be(uint8_t *pixels, uint16_t width,
                                uint16_t height, uint32_t stride,
                                const camera_sw_isp_tone_map_t *map)
{
    uint32_t y;

    if (!rgb565_image_valid(pixels, width, height, stride) || (map == NULL))
        return -1;
    for (y = 0U; y < height; y++)
    {
        uint8_t *row = pixels + y * stride;
        uint32_t x;

        for (x = 0U; x < width; x++)
        {
            uint8_t *out = row + x * 2U;
            uint16_t pixel = rgb565_be_load(out);
            uint32_t red = (pixel >> 11) & 0x1fU;
            uint32_t green = (pixel >> 5) & 0x3fU;
            uint32_t blue = pixel & 0x1fU;
            uint32_t red8 = (red << 3) | (red >> 2);
            uint32_t green8 = (green << 2) | (green >> 4);
            uint32_t blue8 = (blue << 3) | (blue >> 2);
            uint32_t luminance =
                (77U * red8 + 150U * green8 + 29U * blue8) >> 8;
            uint32_t mapped = map->lut[luminance];
            uint32_t out_red;
            uint32_t out_green;
            uint32_t out_blue;

            if (luminance == 0U)
            {
                out_red = 0U;
                out_green = 0U;
                out_blue = 0U;
            }
            else
            {
                uint32_t scale = (mapped << 8) / luminance;

                out_red = (red8 * scale + 128U) >> 8;
                if (out_red > 255U)
                    out_red = 255U;
                out_green = (green8 * scale + 128U) >> 8;
                if (out_green > 255U)
                    out_green = 255U;
                out_blue = (blue8 * scale + 128U) >> 8;
                if (out_blue > 255U)
                    out_blue = 255U;
            }

            out_red = (out_red + bayer_offset(x, y, 8U)) >> 3;
            if (out_red > 31U)
                out_red = 31U;
            out_green = (out_green + bayer_offset(x, y, 4U)) >> 2;
            if (out_green > 63U)
                out_green = 63U;
            out_blue = (out_blue + bayer_offset(x, y, 8U)) >> 3;
            if (out_blue > 31U)
                out_blue = 31U;

            rgb565_be_store(out, rgb565_pack(out_red, out_green, out_blue));
        }
    }
    return 0;
}
