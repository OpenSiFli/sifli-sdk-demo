/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <string.h>

#include "camera_sw_jpeg_decoder.h"
#include "camera_sw_jpeg_encoder.h"

#if defined(CAMERA_SW_JPEG_EXAMPLE_ENCODER) && \
    defined(CAMERA_SW_JPEG_EXAMPLE_DECODER)

#define CAMERA_SW_EXAMPLE_WIDTH 32U
#define CAMERA_SW_EXAMPLE_HEIGHT 24U
#define CAMERA_SW_EXAMPLE_JPEG_CAPACITY 4096U
#define CAMERA_SW_EXAMPLE_JPEG_WORK_SIZE 16384U

typedef struct
{
    const uint8_t *data;
    uint32_t size;
    uint32_t offset;
} camera_sw_example_reader_t;

static uint8_t camera_sw_example_pixels[
    CAMERA_SW_EXAMPLE_WIDTH * CAMERA_SW_EXAMPLE_HEIGHT * 2U];
static uint8_t camera_sw_example_jpeg[CAMERA_SW_EXAMPLE_JPEG_CAPACITY];
static uint8_t camera_sw_example_decoded[
    CAMERA_SW_EXAMPLE_WIDTH * CAMERA_SW_EXAMPLE_HEIGHT * 2U];
static uint8_t camera_sw_example_decoder_work[CAMERA_SW_EXAMPLE_JPEG_WORK_SIZE];
static uint8_t camera_sw_example_encoder_work[
    CAMERA_SW_JPEG_ENCODER_STATIC_WORKSPACE_SIZE] __attribute__((aligned(8)));

static uint32_t camera_sw_example_checksum(const uint8_t *data, uint32_t size)
{
    uint32_t value = 2166136261U;

    while (size-- != 0U)
        value = (value ^ *data++) * 16777619U;
    return value;
}

static void camera_sw_example_fill_fixture(void)
{
    uint32_t y;

    for (y = 0U; y < CAMERA_SW_EXAMPLE_HEIGHT; y++)
    {
        uint32_t x;

        for (x = 0U; x < CAMERA_SW_EXAMPLE_WIDTH; x++)
        {
            uint16_t pixel = (uint16_t)(((x & 0x1fU) << 11) |
                                        (((x + y) & 0x3fU) << 5) |
                                        (y & 0x1fU));
            uint32_t offset = (y * CAMERA_SW_EXAMPLE_WIDTH + x) * 2U;

            camera_sw_example_pixels[offset] = (uint8_t)(pixel >> 8);
            camera_sw_example_pixels[offset + 1U] = (uint8_t)pixel;
        }
    }
}

static size_t camera_sw_example_read(void *context, uint8_t *buffer,
                                     size_t size)
{
    camera_sw_example_reader_t *reader = context;
    uint32_t available;

    if ((reader == RT_NULL) || (reader->offset > reader->size))
        return 0U;
    available = reader->size - reader->offset;
    if (size > available)
        size = available;
    if ((buffer != RT_NULL) && (size != 0U))
        memcpy(buffer, reader->data + reader->offset, size);
    reader->offset += (uint32_t)size;
    return size;
}

int main(void)
{
    camera_sw_jpeg_encoder_t encoder;
    camera_sw_jpeg_frame_t input;
    camera_sw_jpeg_frame_t output;
    camera_sw_example_reader_t reader;
    uint32_t jpeg_size = 0U;
    uint32_t checksum;
    int status;

    camera_sw_example_fill_fixture();
    input.data = camera_sw_example_pixels;
    input.width = CAMERA_SW_EXAMPLE_WIDTH;
    input.height = CAMERA_SW_EXAMPLE_HEIGHT;
    input.stride = CAMERA_SW_EXAMPLE_WIDTH * 2U;
    input.pixel_format = CAMERA_SW_JPEG_PIXEL_RGB565_BE;
    status = camera_sw_jpeg_encoder_init(&encoder, camera_sw_example_encoder_work,
                                         sizeof(camera_sw_example_encoder_work));
    if (status == 0)
        status = camera_sw_jpeg_encode_to_buffer(
            &encoder, &input, CAMERA_SW_JPEG_QUALITY_HIGH,
            camera_sw_example_jpeg, sizeof(camera_sw_example_jpeg), &jpeg_size);

    reader.data = camera_sw_example_jpeg;
    reader.size = jpeg_size;
    reader.offset = 0U;
    output.data = camera_sw_example_decoded;
    output.width = 0U;
    output.height = CAMERA_SW_EXAMPLE_HEIGHT;
    output.stride = CAMERA_SW_EXAMPLE_WIDTH * 2U;
    output.pixel_format = CAMERA_SW_JPEG_PIXEL_RGB565_BE;
    if ((status == 0) && ((jpeg_size < 4U) ||
                          (camera_sw_example_jpeg[0] != 0xffU) ||
                          (camera_sw_example_jpeg[1] != 0xd8U) ||
                          (camera_sw_example_jpeg[jpeg_size - 2U] != 0xffU) ||
                          (camera_sw_example_jpeg[jpeg_size - 1U] != 0xd9U)))
        status = -1;
    if (status == 0)
        status = camera_sw_jpeg_decode(camera_sw_example_read, &reader,
                                       camera_sw_example_decoder_work,
                                       sizeof(camera_sw_example_decoder_work),
                                       &output);
    checksum = camera_sw_example_checksum(camera_sw_example_decoded,
                                          sizeof(camera_sw_example_decoded));
    rt_kprintf("camera_sw_jpeg example: jpeg_size=%u decoded=%ux%u "
               "checksum=%08x status=%s\n", jpeg_size, output.width,
               output.height, checksum,
               (status == 0) && (output.width == CAMERA_SW_EXAMPLE_WIDTH) &&
               (output.height == CAMERA_SW_EXAMPLE_HEIGHT) ? "PASS" : "FAIL");
    return (status == 0) && (output.width == CAMERA_SW_EXAMPLE_WIDTH) &&
           (output.height == CAMERA_SW_EXAMPLE_HEIGHT) ? 0 : -1;
}

#else

int main(void)
{
    rt_kprintf("camera_sw_jpeg example: select encoder, decoder, and HCPU in menuconfig\n");
    return -1;
}

#endif
