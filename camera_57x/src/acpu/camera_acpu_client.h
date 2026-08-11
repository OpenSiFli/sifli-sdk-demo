#ifndef CAMERA_ACPU_CLIENT_H
#define CAMERA_ACPU_CLIENT_H

#include <stdint.h>

int camera_acpu_encode_rgb565_be(uint8_t *pixels, uint16_t width,
                                 uint16_t height, uint32_t pitch,
                                 const uint8_t **jpeg,
                                 uint32_t *jpeg_size,
                                 uint32_t *elapsed_ms);
int camera_acpu_decode_jpeg(const uint8_t *jpeg, uint32_t jpeg_size,
                            uint8_t *destination,
                            uint32_t destination_size,
                            uint16_t *width, uint16_t *height,
                            uint32_t *elapsed_ms);
int camera_acpu_query_cpu_usage(int32_t *usage_x100);

#endif
