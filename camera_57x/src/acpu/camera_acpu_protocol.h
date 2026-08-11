#ifndef CAMERA_ACPU_PROTOCOL_H
#define CAMERA_ACPU_PROTOCOL_H

#include <stdint.h>

#define CAMERA_ACPU_TASK_CHECKSUM 1U
#define CAMERA_ACPU_TASK_JPEG 2U
#define CAMERA_ACPU_TASK_JPEG_DECODE 3U
#define CAMERA_ACPU_TASK_CPU_USAGE 4U
#define CAMERA_ACPU_BUFFER_SIZE 4096U

typedef struct
{
    const uint8_t *input;
    uint32_t input_size;
    uint32_t checksum;
} camera_acpu_checksum_task_t;

typedef struct
{
    uint8_t *input;
    uint8_t *output;
    uint32_t input_pitch;
    uint32_t output_capacity;
    uint32_t output_size;
    uint16_t width;
    uint16_t height;
    uint8_t quality;
    uint8_t input_big_endian;
} camera_acpu_jpeg_task_t;

typedef struct
{
    const uint8_t *input;
    uint8_t *output;
    uint32_t input_size;
    uint32_t output_capacity;
    uint16_t width;
    uint16_t height;
} camera_acpu_jpeg_decode_task_t;

typedef struct
{
    int32_t usage_x100;
} camera_acpu_cpu_usage_task_t;

#endif
