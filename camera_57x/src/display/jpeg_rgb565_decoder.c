#include "jpeg_rgb565_decoder.h"

#include <string.h>

int jpeg_rgb565_choose_scale(uint16_t width, uint16_t height,
                             uint32_t destination_size)
{
    int scale;

    if ((width == 0U) || (height == 0U))
        return -1;
    for (scale = 0; scale <= 3; scale++)
    {
        uint32_t scaled_width = width >> scale;
        uint32_t scaled_height = height >> scale;

        if ((scaled_width != 0U) && (scaled_height != 0U) &&
            (scaled_width * scaled_height * 2U <= destination_size))
            return scale;
    }
    return -1;
}

int jpeg_rgb565_blit(uint8_t *destination, uint16_t width, uint16_t height,
                     const uint8_t *rgb, const jpeg_rgb565_rect_t *rect)
{
    uint32_t rect_width;
    uint32_t rect_height;
    uint32_t y;

    if ((destination == NULL) || (rgb == NULL) || (rect == NULL) ||
        (rect->left > rect->right) || (rect->top > rect->bottom) ||
        (rect->right >= width) || (rect->bottom >= height))
        return -1;

    rect_width = (uint32_t)rect->right - rect->left + 1U;
    rect_height = (uint32_t)rect->bottom - rect->top + 1U;
    for (y = 0; y < rect_height; y++)
    {
        uint8_t *out = destination +
                       (((uint32_t)rect->top + y) * width + rect->left) * 2U;
        uint32_t x;

        for (x = 0; x < rect_width; x++)
        {
            uint8_t red = *rgb++;
            uint8_t green = *rgb++;
            uint8_t blue = *rgb++;
            uint16_t pixel = (uint16_t)(((uint16_t)(red & 0xf8U) << 8) |
                                        ((uint16_t)(green & 0xfcU) << 3) |
                                        (blue >> 3));

            *out++ = (uint8_t)(pixel >> 8);
            *out++ = (uint8_t)pixel;
        }
    }
    return 0;
}

int jpeg_rgb565_swap_write(uint8_t *destination, uint16_t width,
                           uint16_t height, const uint8_t *rgb,
                           const jpeg_rgb565_rect_t *rect)
{
    uint32_t rect_width;
    uint32_t rect_height;
    uint32_t y;

    if ((destination == NULL) || (rgb == NULL) || (rect == NULL) ||
        (rect->left > rect->right) || (rect->top > rect->bottom) ||
        (rect->right >= width) || (rect->bottom >= height))
        return -1;

    rect_width = (uint32_t)rect->right - rect->left + 1U;
    rect_height = (uint32_t)rect->bottom - rect->top + 1U;
    for (y = 0; y < rect_height; y++)
    {
        const uint8_t *in = rgb + y * rect_width * 2U;
        uint8_t *out = destination +
                       (((uint32_t)rect->top + y) * width + rect->left) * 2U;
        uint32_t x;

        for (x = 0; x < rect_width; x++)
        {
            uint8_t low = *in++;
            uint8_t high = *in++;

            *out++ = high;
            *out++ = low;
        }
    }
    return 0;
}

#ifndef CAMERA_JPEG_DECODER_BLIT_ONLY

#include "tjpgd.h"

typedef struct
{
    const uint8_t *jpeg;
    size_t jpeg_size;
    size_t offset;
    uint8_t *destination;
    uint16_t width;
    uint16_t height;
} jpeg_decode_context_t;

static size_t jpeg_input(JDEC *decoder, uint8_t *buffer, size_t size)
{
    jpeg_decode_context_t *context = decoder->device;
    size_t remaining = context->jpeg_size - context->offset;

    if (size > remaining)
        size = remaining;
    if ((buffer != NULL) && (size != 0U))
        memcpy(buffer, context->jpeg + context->offset, size);
    context->offset += size;
    return size;
}

static int jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    jpeg_decode_context_t *context = decoder->device;
    jpeg_rgb565_rect_t output_rect = {
        .left = rect->left,
        .right = rect->right,
        .top = rect->top,
        .bottom = rect->bottom,
    };

    return jpeg_rgb565_swap_write(context->destination, context->width,
                                  context->height, bitmap,
                                  &output_rect) == 0;
}

int jpeg_rgb565_decode(const uint8_t *jpeg, uint32_t jpeg_size,
                       uint8_t *destination, uint32_t destination_size,
                       void *work, uint32_t work_size,
                       uint16_t *width, uint16_t *height)
{
    jpeg_decode_context_t context;
    JDEC decoder;
    JRESULT result;
    int scale;

    if ((jpeg == NULL) || (jpeg_size < 4U) || (destination == NULL) ||
        (work == NULL) || (width == NULL) || (height == NULL))
        return -1;

    context = (jpeg_decode_context_t){
        .jpeg = jpeg,
        .jpeg_size = jpeg_size,
        .destination = destination,
    };
    result = jd_prepare(&decoder, jpeg_input, work, work_size, &context);
    if (result != JDR_OK)
        return -(int)result;

    scale = jpeg_rgb565_choose_scale(decoder.width, decoder.height,
                                     destination_size);
    if (scale < 0)
        return -1;
    context.width = decoder.width >> scale;
    context.height = decoder.height >> scale;
    result = jd_decomp(&decoder, jpeg_output, (uint8_t)scale);
    if (result != JDR_OK)
        return -(int)result;

    *width = context.width;
    *height = context.height;
    return 0;
}

#endif
