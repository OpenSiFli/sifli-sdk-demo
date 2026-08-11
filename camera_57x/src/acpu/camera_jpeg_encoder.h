#ifndef CAMERA_JPEG_ENCODER_H
#define CAMERA_JPEG_ENCODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    CAMERA_JPEG_QUALITY_BEST = 0,
    CAMERA_JPEG_QUALITY_HIGH,
    CAMERA_JPEG_QUALITY_MEDIUM,
    CAMERA_JPEG_QUALITY_LOW,
} camera_jpeg_quality_t;

int camera_jpeg_encode_rgb565(const uint8_t *pixels, uint16_t width,
                              uint16_t height, uint32_t pitch,
                              camera_jpeg_quality_t quality, uint8_t *output,
                              uint32_t output_capacity,
                              uint32_t *output_size);
int camera_jpeg_encode_rgb565_be(const uint8_t *pixels, uint16_t width,
                                 uint16_t height, uint32_t pitch,
                                 camera_jpeg_quality_t quality,
                                 uint8_t *output, uint32_t output_capacity,
                                 uint32_t *output_size);

#ifdef __cplusplus
}
#endif

#endif
