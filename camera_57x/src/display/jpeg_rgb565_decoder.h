#ifndef JPEG_RGB565_DECODER_H
#define JPEG_RGB565_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint16_t left;
    uint16_t right;
    uint16_t top;
    uint16_t bottom;
} jpeg_rgb565_rect_t;

int jpeg_rgb565_blit(uint8_t *destination, uint16_t width, uint16_t height,
                     const uint8_t *rgb, const jpeg_rgb565_rect_t *rect);
int jpeg_rgb565_swap_write(uint8_t *destination, uint16_t width,
                           uint16_t height, const uint8_t *rgb,
                           const jpeg_rgb565_rect_t *rect);
int jpeg_rgb565_choose_scale(uint16_t width, uint16_t height,
                             uint32_t destination_size);

#ifndef CAMERA_JPEG_DECODER_BLIT_ONLY
int jpeg_rgb565_decode(const uint8_t *jpeg, uint32_t jpeg_size,
                       uint8_t *destination, uint32_t destination_size,
                       void *work, uint32_t work_size,
                       uint16_t *width, uint16_t *height);
#endif

#endif
