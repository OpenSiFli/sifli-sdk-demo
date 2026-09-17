/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_image.h"
#include "vendor/camera_tjpgd/camera_tjpgd.h"

#include <string.h>

#define CAMERA_IMAGE_WORKSPACE_SIZE 8192U

typedef struct
{
    const uint8_t *jpeg;
    size_t jpeg_size;
    size_t input_offset;
    uint8_t *pixels;
    uint32_t capacity;
    uint32_t written_pixels;
    uint16_t width;
    uint16_t height;
} camera_image_context_t;

static union
{
    uint32_t alignment;
    uint8_t bytes[CAMERA_IMAGE_WORKSPACE_SIZE];
} s_camera_image_workspace;

static size_t camera_image_read(CAMERA_JDEC *decoder, uint8_t *data, size_t size)
{
    camera_image_context_t *context = decoder->device;
    size_t available = context->jpeg_size - context->input_offset;

    if (size > available)
        size = available;
    if (data != NULL && size != 0U)
        memcpy(data, context->jpeg + context->input_offset, size);
    context->input_offset += size;
    return size;
}

static int camera_image_prepare_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                                     CAMERA_JDEC *decoder,
                                     camera_image_context_t *context)
{
    if (jpeg == NULL || jpeg_size < 4U ||
        jpeg[0] != 0xFFU || jpeg[1] != 0xD8U ||
        jpeg[jpeg_size - 2U] != 0xFFU || jpeg[jpeg_size - 1U] != 0xD9U)
        return -1;

    context->jpeg = jpeg;
    context->jpeg_size = jpeg_size;
    if (camera_jd_prepare(decoder, camera_image_read,
                          s_camera_image_workspace.bytes,
                          sizeof(s_camera_image_workspace.bytes),
                          context) != JDR_OK)
        return -1;
    if (decoder->width == 0U || decoder->height == 0U ||
        decoder->width > CAMERA_IMAGE_MAX_WIDTH ||
        decoder->height > CAMERA_IMAGE_MAX_HEIGHT)
        return -1;
    return 0;
}

int camera_image_probe_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                            uint16_t *width, uint16_t *height)
{
    CAMERA_JDEC decoder;
    camera_image_context_t context = {0};

    if (width != NULL)
        *width = 0U;
    if (height != NULL)
        *height = 0U;
    if (width == NULL || height == NULL ||
        camera_image_prepare_jpeg(jpeg, jpeg_size, &decoder, &context) != 0)
        return -1;
    *width = decoder.width;
    *height = decoder.height;
    return 0;
}

static int camera_image_output(CAMERA_JDEC *decoder, void *bitmap,
                               CAMERA_JRECT *rectangle)
{
    camera_image_context_t *context = decoder->device;
    const uint16_t *source = bitmap;
    uint32_t row_pixels;
    uint32_t row;

    if (rectangle->left > rectangle->right ||
        rectangle->top > rectangle->bottom ||
        rectangle->right >= context->width ||
        rectangle->bottom >= context->height)
        return 0;
    row_pixels = (uint32_t)rectangle->right - rectangle->left + 1U;
    for (row = rectangle->top; row <= rectangle->bottom; row++)
    {
        uint32_t offset = (row * context->width + rectangle->left) * 2U;
        uint32_t column;

        if (offset > context->capacity ||
            row_pixels > (context->capacity - offset) / 2U)
            return 0;
        for (column = 0; column < row_pixels; column++)
        {
            uint16_t pixel = *source++;
            context->pixels[offset++] = (uint8_t)pixel;
            context->pixels[offset++] = (uint8_t)(pixel >> 8);
        }
        context->written_pixels += row_pixels;
    }
    return 1;
}

int camera_image_decode_jpeg(const uint8_t *jpeg, size_t jpeg_size,
                             uint8_t *pixels, uint32_t capacity, uint8_t scale,
                             uint16_t *width, uint16_t *height)
{
    CAMERA_JDEC decoder;
    camera_image_context_t context = {0};
    uint32_t pixel_count;

    if (width != NULL)
        *width = 0U;
    if (height != NULL)
        *height = 0U;
    if (pixels == NULL || width == NULL || height == NULL || scale > 3U ||
        camera_image_prepare_jpeg(jpeg, jpeg_size, &decoder, &context) != 0)
        return -1;

    context.pixels = pixels;
    context.capacity = capacity;

    context.width = decoder.width >> scale;
    context.height = decoder.height >> scale;
    pixel_count = (uint32_t)context.width * context.height;
    if (pixel_count == 0U || pixel_count > capacity / 2U)
        return -1;
    if (camera_jd_decomp(&decoder, camera_image_output, scale) != JDR_OK ||
        context.written_pixels != pixel_count)
        return -1;

    *width = context.width;
    *height = context.height;
    return 0;
}
