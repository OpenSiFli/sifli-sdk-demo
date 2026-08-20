/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2013-2024 OpenMV, LLC.
 *
 * RGB565 neighborhood logic adapted from OpenMV imlib/filter.c.
 */

#include "camera_sw_isp.h"

#include <stddef.h>
#include <string.h>

#define CAMERA_SW_ISP_FILTER_LINES 3U

static uint16_t rgb565_be_load(const uint8_t *pixel)
{
    return (uint16_t)(((uint16_t)pixel[0] << 8) | pixel[1]);
}

static void rgb565_be_store(uint8_t *pixel, uint16_t value)
{
    pixel[0] = (uint8_t)(value >> 8);
    pixel[1] = (uint8_t)value;
}

static uint16_t rgb565_pack(int red, int green, int blue)
{
    return (uint16_t)(((uint32_t)red << 11) |
                      ((uint32_t)green << 5) | (uint32_t)blue);
}

static int clamp_channel(int value, int maximum)
{
    if (value < 0)
        return 0;
    return value > maximum ? maximum : value;
}

static uint8_t rgb565_luminance(uint16_t pixel)
{
    uint32_t red = (pixel >> 11) & 0x1fU;
    uint32_t green = (pixel >> 5) & 0x3fU;
    uint32_t blue = pixel & 0x1fU;
    uint32_t red8 = (red << 3) | (red >> 2);
    uint32_t green8 = (green << 2) | (green >> 4);
    uint32_t blue8 = (blue << 3) | (blue >> 2);

    return (uint8_t)((77U * red8 + 150U * green8 + 29U * blue8) >> 8);
}

static uint16_t row_pixel(const uint8_t *row, int x, uint16_t width)
{
    if (x < 0)
        x = 0;
    else if (x >= (int)width)
        x = (int)width - 1;
    return rgb565_be_load(row + (uint32_t)x * 2U);
}

uint32_t camera_sw_isp_filter_scratch_size(uint16_t width)
{
    return (uint32_t)width * 2U * CAMERA_SW_ISP_FILTER_LINES;
}

static int camera_sw_isp_filter_core(uint8_t *pixels, uint16_t width,
                               uint16_t height, uint32_t stride,
                               uint8_t flat_range,
                               const int8_t sharpen_kernel[9],
                               uint8_t *scratch, uint32_t scratch_size)
{
    uint32_t row_bytes = (uint32_t)width * 2U;
    uint8_t *previous;
    uint8_t *current;
    uint8_t *next;
    uint32_t y;

    if ((pixels == NULL) || (width == 0U) || (height == 0U) ||
        (stride < row_bytes) || (sharpen_kernel == NULL) ||
        (scratch == NULL) ||
        (scratch_size < camera_sw_isp_filter_scratch_size(width)))
        return -1;

    previous = scratch;
    current = scratch + row_bytes;
    next = scratch + row_bytes * 2U;
    memcpy(previous, pixels, row_bytes);
    memcpy(current, pixels, row_bytes);
    memcpy(next, pixels + (height > 1U ? stride : 0U), row_bytes);

    for (y = 0U; y < height; y++)
    {
        uint8_t *output = pixels + y * stride;
        uint32_t x;

        for (x = 0U; x < width; x++)
        {
            const uint8_t *rows[3] = {previous, current, next};
            uint8_t minimum = 255U;
            uint8_t maximum = 0U;
            int red_sum = 0;
            int green_sum = 0;
            int blue_sum = 0;
            uint32_t row_index;
            uint32_t kernel_index = 0U;

            for (row_index = 0U; row_index < 3U; row_index++)
            {
                int dx;

                for (dx = -1; dx <= 1; dx++)
                {
                    uint16_t pixel = row_pixel(rows[row_index],
                                               (int)x + dx, width);
                    uint8_t level = rgb565_luminance(pixel);

                    if (level < minimum)
                        minimum = level;
                    if (level > maximum)
                        maximum = level;
                }
            }

            for (row_index = 0U; row_index < 3U; row_index++)
            {
                int dx;

                for (dx = -1; dx <= 1; dx++)
                {
                    uint16_t pixel = row_pixel(rows[row_index],
                                               (int)x + dx, width);
                    int weight = sharpen_kernel[kernel_index++];

                    if ((uint8_t)(maximum - minimum) <= flat_range)
                        weight = 1;
                    red_sum += (int)((pixel >> 11) & 0x1fU) * weight;
                    green_sum += (int)((pixel >> 5) & 0x3fU) * weight;
                    blue_sum += (int)(pixel & 0x1fU) * weight;
                }
            }
            if ((uint8_t)(maximum - minimum) <= flat_range)
            {
                red_sum /= 9;
                green_sum /= 9;
                blue_sum /= 9;
            }
            rgb565_be_store(output + x * 2U,
                            rgb565_pack(clamp_channel(red_sum, 31),
                                        clamp_channel(green_sum, 63),
                                        clamp_channel(blue_sum, 31)));
        }

        if (y + 1U < height)
        {
            uint8_t *recycled = previous;
            uint32_t source_y = y + 2U;

            previous = current;
            current = next;
            next = recycled;
            if (source_y < height)
                memcpy(next, pixels + source_y * stride, row_bytes);
            else
                memcpy(next, current, row_bytes);
        }
    }
    return 0;
}

int camera_sw_isp_filter(camera_sw_frame_t *frame, uint8_t flat_range,
                         const int8_t kernel[9], void *scratch,
                         uint32_t scratch_size)
{
    if (frame == NULL)
        return -1;
    return camera_sw_isp_filter_core(frame->data, frame->width, frame->height,
                                     frame->stride, flat_range, kernel,
                                     (uint8_t *)scratch, scratch_size);
}
