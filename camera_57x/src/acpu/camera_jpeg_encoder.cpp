#include "camera_jpeg_encoder.h"

#include <JPEGENC.h>
#include <limits.h>

static int camera_jpeg_encode_pixels(
    const uint8_t *pixels, uint16_t width, uint16_t height, uint32_t pitch,
    camera_jpeg_quality_t quality, uint8_t pixel_type, uint8_t *output,
    uint32_t output_capacity, uint32_t *output_size)
{
    static JPEGENC encoder;
    JPEGENCODE state;
    int size;

    if ((pixels == NULL) || (width == 0U) || (height == 0U) ||
        (pitch < (uint32_t)width * 2U) || (quality > CAMERA_JPEG_QUALITY_LOW) ||
        (output == NULL) || (output_capacity > INT_MAX) ||
        (output_capacity < 1024U) || (output_size == NULL))
        return -1;

    *output_size = 0U;
    if (encoder.open(output, (int)output_capacity) != JPEGE_SUCCESS)
        return -1;
    if (encoder.encodeBegin(&state, width, height, pixel_type,
                            JPEGE_SUBSAMPLE_420, (uint8_t)quality) !=
        JPEGE_SUCCESS)
        return -1;
    if (encoder.addFrame(&state, const_cast<uint8_t *>(pixels),
                         (int)pitch) != JPEGE_SUCCESS)
        return -1;

    size = encoder.close();
    if (size <= 0)
        return -1;
    *output_size = (uint32_t)size;
    return 0;
}

extern "C" int camera_jpeg_encode_rgb565(
    const uint8_t *pixels, uint16_t width, uint16_t height, uint32_t pitch,
    camera_jpeg_quality_t quality, uint8_t *output, uint32_t output_capacity,
    uint32_t *output_size)
{
    return camera_jpeg_encode_pixels(pixels, width, height, pitch, quality,
                                     JPEGE_PIXEL_RGB565, output,
                                     output_capacity, output_size);
}

extern "C" int camera_jpeg_encode_rgb565_be(
    const uint8_t *pixels, uint16_t width, uint16_t height, uint32_t pitch,
    camera_jpeg_quality_t quality, uint8_t *output, uint32_t output_capacity,
    uint32_t *output_size)
{
    return camera_jpeg_encode_pixels(pixels, width, height, pitch, quality,
                                     JPEGE_PIXEL_RGB565_BE, output,
                                     output_capacity, output_size);
}
