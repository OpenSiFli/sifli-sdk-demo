/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_SW_JPEG_COMMON_H
#define CAMERA_SW_JPEG_COMMON_H

#include <stdint.h>

typedef enum
{
    CAMERA_SW_JPEG_PIXEL_RGB565 = 0,
    CAMERA_SW_JPEG_PIXEL_RGB565_BE,
} camera_sw_jpeg_pixel_format_t;

typedef struct
{
    uint8_t *data;
    uint16_t width;
    uint16_t height;
    uint32_t stride;
    camera_sw_jpeg_pixel_format_t pixel_format;
} camera_sw_jpeg_frame_t;

#endif
