/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 SiFli camera project */

#include "camera_sw_isp_pipeline.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "camera_sw_isp.h"

#define CAMERA_ISP_FLAG_AWB_STATS CAMERA_SW_ISP_FLAG_AWB_STATS
#define CAMERA_ISP_FLAG_AWB_APPLY CAMERA_SW_ISP_FLAG_AWB_APPLY
#define CAMERA_ISP_FLAG_CCM CAMERA_SW_ISP_FLAG_CCM
#define CAMERA_ISP_FLAG_GAMMA CAMERA_SW_ISP_FLAG_GAMMA
#define CAMERA_ISP_FLAG_FILTER CAMERA_SW_ISP_FLAG_FILTER
#define CAMERA_ISP_FLAG_TONE CAMERA_SW_ISP_FLAG_TONE
#define CAMERA_ISP_FLAG_VIVID CAMERA_SW_ISP_FLAG_VIVID
#define CAMERA_ISP_FLAG_DITHER CAMERA_SW_ISP_FLAG_DITHER
#define CAMERA_ISP_FLAG_ALL CAMERA_SW_ISP_FLAG_ALL

#define CAMERA_ISP_MAX_WIDTH 640U
#define CAMERA_ISP_FLAT_RANGE 12U
#define CAMERA_ISP_FUSED_COLOR_FLAGS \
    (CAMERA_ISP_FLAG_AWB_STATS | CAMERA_ISP_FLAG_AWB_APPLY | \
     CAMERA_ISP_FLAG_GAMMA | CAMERA_ISP_FLAG_TONE | CAMERA_ISP_FLAG_VIVID)

static const float camera_isp_default_ccm[9] = {
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f,
};

static const float camera_isp_vivid_ccm[9] = {
     1.052575f, -0.044025f, -0.008550f,
    -0.022425f,  1.030975f, -0.008550f,
    -0.022425f, -0.044025f,  1.066450f,
};

static const int8_t camera_isp_default_sharpen[9] = {
     0, -1,  0,
    -1,  5, -1,
     0, -1,  0,
};

static int camera_isp_clamp_u8(int value)
{
    if (value < 0)
        return 0;
    return value > 255 ? 255 : value;
}

static int camera_isp_clamp_float_u8(float value)
{
    return camera_isp_clamp_u8((int)lroundf(value));
}

static float camera_isp_clamp_float(float value)
{
    if (value < 0.0f)
        return 0.0f;
    return value > 255.0f ? 255.0f : value;
}

static void camera_isp_build_gamma_lut(uint8_t lut[256])
{
    const float black = 8.0f / 255.0f;
    uint32_t index;

    for (index = 0U; index < 256U; index++)
    {
        float normalized = index / 255.0f;
        int value;

        normalized = normalized <= black ? 0.0f :
                     (normalized - black) / (1.0f - black);
        value = (int)lroundf(powf(normalized, 0.5f) * 255.0f);
        lut[index] = (uint8_t)camera_isp_clamp_u8(value);
    }
}

static int camera_isp_apply_fused_color(
    uint8_t *pixels, uint16_t width, uint16_t height, uint32_t stride,
    const camera_sw_isp_stats_t *stats,
    const camera_sw_isp_tone_map_t *tone_map, int dither_enabled)
{
    uint8_t gamma_lut[256];
    uint32_t minimum_candidates = ((uint32_t)width * height + 99U) / 100U;
    uint32_t red_gain_q8 = 256U;
    uint32_t blue_gain_q8 = 256U;
    uint32_t y;

    if ((pixels == NULL) || (stats == NULL) || (tone_map == NULL) ||
        (width == 0U) || (height == 0U) ||
        (stride < (uint32_t)width * 2U))
        return -1;
    if (stats->valid_pixels >= minimum_candidates)
    {
        red_gain_q8 = (uint32_t)stats->red_gain_q6 * 4U;
        blue_gain_q8 = (uint32_t)stats->blue_gain_q6 * 4U;
    }
    camera_isp_build_gamma_lut(gamma_lut);
    for (y = 0U; y < height; y++)
    {
        uint8_t *row = pixels + y * stride;
        uint32_t x;

        for (x = 0U; x < width; x++)
        {
            uint8_t *out = row + x * 2U;
            uint16_t pixel = (uint16_t)(((uint16_t)out[0] << 8) | out[1]);
            uint32_t red_code = (pixel >> 11) & 0x1fU;
            uint32_t green_code = (pixel >> 5) & 0x3fU;
            uint32_t blue_code = pixel & 0x1fU;
            float red = (float)red_code * 255.0f / 31.0f;
            float green = (float)green_code * 255.0f / 63.0f;
            float blue = (float)blue_code * 255.0f / 31.0f;
            int luminance;
            int mapped;
            int new_red;
            int new_green;
            int new_blue;

            red = red * (float)red_gain_q8 / 256.0f;
            blue = blue * (float)blue_gain_q8 / 256.0f;
            {
                float peak = red > green ? red : green;

                if (blue > peak)
                    peak = blue;
                if (peak > 255.0f)
                {
                    red = red * 255.0f / peak;
                    green = green * 255.0f / peak;
                    blue = blue * 255.0f / peak;
                }
            }

            luminance = (int)(77.0f * red + 150.0f * green +
                              29.0f * blue) >> 8;
            mapped = gamma_lut[luminance];
            red = camera_isp_clamp_float(red + mapped - luminance);
            green = camera_isp_clamp_float(green + mapped - luminance);
            blue = camera_isp_clamp_float(blue + mapped - luminance);

            luminance = (int)(77.0f * red + 150.0f * green +
                              29.0f * blue) >> 8;
            mapped = tone_map->lut[luminance];
            if (luminance == 0)
            {
                red = 0;
                green = 0;
                blue = 0;
            }
            else
            {
                float scale = (float)mapped / (float)luminance;

                red = camera_isp_clamp_float(red * scale);
                green = camera_isp_clamp_float(green * scale);
                blue = camera_isp_clamp_float(blue * scale);
            }

            new_red = camera_isp_clamp_float_u8(
                camera_isp_vivid_ccm[0] * red +
                camera_isp_vivid_ccm[1] * green +
                camera_isp_vivid_ccm[2] * blue);
            new_green = camera_isp_clamp_float_u8(
                camera_isp_vivid_ccm[3] * red +
                camera_isp_vivid_ccm[4] * green +
                camera_isp_vivid_ccm[5] * blue);
            new_blue = camera_isp_clamp_float_u8(
                camera_isp_vivid_ccm[6] * red +
                camera_isp_vivid_ccm[7] * green +
                camera_isp_vivid_ccm[8] * blue);
            if (camera_sw_isp_quantize_rgb888(
                    out, (uint16_t)x, (uint16_t)y, (uint8_t)new_red,
                    (uint8_t)new_green, (uint8_t)new_blue,
                    dither_enabled) != 0)
                return -1;
        }
    }
    return 0;
}

static int camera_isp_apply_gamma_rgb888(uint8_t *pixels, uint16_t width,
                                         uint16_t height, uint32_t stride,
                                         int dither_enabled)
{
    uint8_t gamma_lut[256];
    uint32_t y;

    if ((pixels == NULL) || (width == 0U) || (height == 0U) ||
        (stride < (uint32_t)width * 2U))
        return -1;

    camera_isp_build_gamma_lut(gamma_lut);
    for (y = 0U; y < height; y++)
    {
        uint8_t *row = pixels + y * stride;
        uint32_t x;

        for (x = 0U; x < width; x++)
        {
            uint8_t *out = row + x * 2U;
            uint16_t pixel = (uint16_t)(((uint16_t)out[0] << 8) | out[1]);
            float red = (float)((pixel >> 11) & 0x1fU) * 255.0f / 31.0f;
            float green = (float)((pixel >> 5) & 0x3fU) * 255.0f / 63.0f;
            float blue = (float)(pixel & 0x1fU) * 255.0f / 31.0f;
            int luminance = (int)(77.0f * red + 150.0f * green +
                                  29.0f * blue) >> 8;
            int mapped = gamma_lut[luminance];

            red = camera_isp_clamp_float(red + mapped - luminance);
            green = camera_isp_clamp_float(green + mapped - luminance);
            blue = camera_isp_clamp_float(blue + mapped - luminance);
            if (camera_sw_isp_quantize_rgb888(
                    out, (uint16_t)x, (uint16_t)y,
                    (uint8_t)camera_isp_clamp_float_u8(red),
                    (uint8_t)camera_isp_clamp_float_u8(green),
                    (uint8_t)camera_isp_clamp_float_u8(blue),
                    dither_enabled) != 0)
                return -1;
        }
    }
    return 0;
}

static uint8_t camera_isp_filter_scratch[
    3U * CAMERA_ISP_MAX_WIDTH * 2U]
    __attribute__((aligned(64)));

static void camera_isp_copy_stats(camera_sw_isp_stats_t *destination,
                                  const camera_sw_isp_stats_t *source)
{
    memcpy(destination->luminance_histogram, source->luminance_histogram,
           sizeof(destination->luminance_histogram));
    destination->red_sum = source->red_sum;
    destination->green_sum = source->green_sum;
    destination->blue_sum = source->blue_sum;
    destination->valid_pixels = source->valid_pixels;
    destination->dark_pixels = source->dark_pixels;
    destination->near_saturated_pixels = source->near_saturated_pixels;
    destination->saturated_pixels = source->saturated_pixels;
    destination->luminance_min = source->luminance_min;
    destination->luminance_max = source->luminance_max;
    destination->red_gain_q6 = source->red_gain_q6;
    destination->blue_gain_q6 = source->blue_gain_q6;
}

int camera_sw_isp_process_rgb565_be(uint8_t *pixels, uint16_t width,
                                 uint16_t height, uint32_t stride,
                                 uint32_t flags,
                                 camera_sw_isp_result_t *result,
                                 camera_sw_isp_now_ms_t now_ms)
{
    camera_sw_isp_stats_t stats;
    camera_sw_isp_tone_map_t tone_map;
    camera_sw_frame_t frame;
    uint32_t started;
    uint32_t completed = 0U;

    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    if ((pixels == NULL) || (width == 0U) || (height == 0U) ||
        (stride < (uint32_t)width * 2U) ||
        ((flags & ~CAMERA_ISP_FLAG_ALL) != 0U) ||
        (((flags & CAMERA_ISP_FLAG_AWB_APPLY) != 0U) &&
         ((flags & CAMERA_ISP_FLAG_AWB_STATS) == 0U)) ||
        (((flags & CAMERA_ISP_FLAG_TONE) != 0U) &&
         ((flags & CAMERA_ISP_FLAG_AWB_STATS) == 0U)) ||
        (((flags & CAMERA_ISP_FLAG_VIVID) != 0U) &&
         ((flags & CAMERA_ISP_FLAG_CCM) != 0U)) ||
        (((flags & CAMERA_ISP_FLAG_DITHER) != 0U) &&
         ((flags & ~CAMERA_ISP_FLAG_DITHER) != CAMERA_ISP_FUSED_COLOR_FLAGS) &&
         ((flags & ~CAMERA_ISP_FLAG_DITHER) != CAMERA_ISP_FLAG_GAMMA)) ||
        ((flags != 0U) && (now_ms == NULL)))
        return -1;
    if (flags == 0U)
        return 0;
    frame.data = pixels;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;

    if ((flags & CAMERA_ISP_FLAG_AWB_STATS) != 0U)
    {
        started = now_ms();
        if (camera_sw_isp_stats(&frame, &stats) != 0)
            goto error;
        result->awb_stats_ms = now_ms() - started;
        camera_isp_copy_stats(&result->stats, &stats);
        completed |= CAMERA_ISP_FLAG_AWB_STATS;
    }
    if ((flags & ~CAMERA_ISP_FLAG_DITHER) == CAMERA_ISP_FUSED_COLOR_FLAGS)
    {
        started = now_ms();
        if (camera_sw_isp_tone_build(&stats, &tone_map) != 0)
            goto error;
        result->tone_ms = now_ms() - started;
        started = now_ms();
        /* Keep all color stages in 8-bit precision until one final RGB565 pack. */
        if (camera_isp_apply_fused_color(
                pixels, width, height, stride, &stats, &tone_map,
                (flags & CAMERA_ISP_FLAG_DITHER) != 0U) != 0)
            goto error;
        result->gamma_ms = now_ms() - started;
        completed |= CAMERA_ISP_FLAG_AWB_APPLY |
                    CAMERA_ISP_FLAG_GAMMA |
                    CAMERA_ISP_FLAG_TONE |
                    CAMERA_ISP_FLAG_VIVID;
        if ((flags & CAMERA_ISP_FLAG_DITHER) != 0U)
            completed |= CAMERA_ISP_FLAG_DITHER;
        result->applied_flags = completed;
        return 0;
    }
    if (flags == (CAMERA_ISP_FLAG_GAMMA | CAMERA_ISP_FLAG_DITHER))
    {
        started = now_ms();
        /* Gamma and the final RGB565 pack share one 8-bit quantization step. */
        if (camera_isp_apply_gamma_rgb888(
                pixels, width, height, stride, 1) != 0)
            goto error;
        result->gamma_ms = now_ms() - started;
        result->applied_flags = CAMERA_ISP_FLAG_GAMMA | CAMERA_ISP_FLAG_DITHER;
        return 0;
    }
    if ((flags & CAMERA_ISP_FLAG_AWB_APPLY) != 0U)
    {
        started = now_ms();
        if (camera_sw_isp_awb_apply(&frame, &stats) != 0)
            goto error;
        result->awb_apply_ms = now_ms() - started;
        completed |= CAMERA_ISP_FLAG_AWB_APPLY;
    }
    if ((flags & CAMERA_ISP_FLAG_CCM) != 0U)
    {
        started = now_ms();
        if (camera_sw_isp_ccm(&frame, camera_isp_default_ccm) != 0)
            goto error;
        result->ccm_ms = now_ms() - started;
        completed |= CAMERA_ISP_FLAG_CCM;
    }
    if ((flags & CAMERA_ISP_FLAG_GAMMA) != 0U)
    {
        /* Restore FLAT mid-tones before applying the percentile tone map. */
        started = now_ms();
        if (camera_sw_isp_gamma(&frame, 2.0f, 1.0f, 0.0f, 8U) != 0)
            goto error;
        result->gamma_ms = now_ms() - started;
        completed |= CAMERA_ISP_FLAG_GAMMA;
    }
    if ((flags & CAMERA_ISP_FLAG_TONE) != 0U)
    {
        started = now_ms();
        if ((camera_sw_isp_tone_build(&stats, &tone_map) != 0) ||
            (camera_sw_isp_tone_apply(&frame, &tone_map) != 0))
            goto error;
        result->tone_ms = now_ms() - started;
        completed |= CAMERA_ISP_FLAG_TONE;
    }
    if ((flags & CAMERA_ISP_FLAG_VIVID) != 0U)
    {
        started = now_ms();
        if (camera_sw_isp_ccm(&frame, camera_isp_vivid_ccm) != 0)
            goto error;
        result->ccm_ms = now_ms() - started;
        completed |= CAMERA_ISP_FLAG_VIVID;
    }
    if ((flags & CAMERA_ISP_FLAG_FILTER) != 0U)
    {
        if (width > CAMERA_ISP_MAX_WIDTH)
            goto error;
        started = now_ms();
        frame.data = pixels;
        if (camera_sw_isp_filter(
                &frame, CAMERA_ISP_FLAT_RANGE, camera_isp_default_sharpen,
                camera_isp_filter_scratch,
                sizeof(camera_isp_filter_scratch)) != 0)
            goto error;
        result->filter_ms = now_ms() - started;
        completed |= CAMERA_ISP_FLAG_FILTER;
    }
    result->applied_flags = completed;
    return 0;

error:
    result->applied_flags = 0U;
    return -1;
}
