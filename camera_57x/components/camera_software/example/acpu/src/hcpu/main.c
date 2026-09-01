/* SPDX-License-Identifier: MIT */
#include <mem_section.h>
#include <rtthread.h>

#include "camera_sw_acpu_client.h"

#define CAMERA_SW_EXAMPLE_WIDTH 32U
#define CAMERA_SW_EXAMPLE_HEIGHT 24U
#define CAMERA_SW_EXAMPLE_JPEG_CAPACITY 4096U
#define CAMERA_SW_EXAMPLE_DECODE_STRIDE 640U

L2_NON_RET_BSS_SECT_BEGIN(camera_sw_acpu_example_buffers)
L2_NON_RET_BSS_SECT(camera_sw_acpu_example_buffers,
                    ALIGN(64) static uint8_t camera_sw_example_pixels[
                        CAMERA_SW_EXAMPLE_WIDTH * CAMERA_SW_EXAMPLE_HEIGHT * 2U]);
L2_NON_RET_BSS_SECT(camera_sw_acpu_example_buffers,
                    ALIGN(64) static uint8_t camera_sw_example_jpeg[
                        CAMERA_SW_EXAMPLE_JPEG_CAPACITY]);
L2_NON_RET_BSS_SECT(camera_sw_acpu_example_buffers,
                    ALIGN(64) static uint8_t camera_sw_example_decoded[
                        CAMERA_SW_EXAMPLE_DECODE_STRIDE * CAMERA_SW_EXAMPLE_HEIGHT * 2U]);
L2_NON_RET_BSS_SECT_END

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

int main(void)
{
    camera_sw_acpu_metrics_t isp_metrics;
    camera_sw_acpu_metrics_t decode_metrics;
    uint32_t jpeg_size = 0U;
    uint32_t encode_ms = 0U;
    uint16_t decoded_width = 0U;
    uint16_t decoded_height = 0U;
    uint32_t flags = CAMERA_SW_ACPU_ISP_FLAGS_COLOR;
    uint32_t checksum;
    int status;

    camera_sw_example_fill_fixture();
    status = camera_sw_acpu_process_rgb565_be(
        camera_sw_example_pixels, CAMERA_SW_EXAMPLE_WIDTH,
        CAMERA_SW_EXAMPLE_HEIGHT, CAMERA_SW_EXAMPLE_WIDTH * 2U, flags,
        &isp_metrics);
    if (status == 0)
        status = camera_sw_acpu_encode_rgb565_be(
            camera_sw_example_pixels, CAMERA_SW_EXAMPLE_WIDTH,
            CAMERA_SW_EXAMPLE_HEIGHT, CAMERA_SW_EXAMPLE_WIDTH * 2U,
            camera_sw_example_jpeg, sizeof(camera_sw_example_jpeg),
            &jpeg_size, &encode_ms);
    if (status == 0)
        status = camera_sw_acpu_decode_jpeg(
            camera_sw_example_jpeg, jpeg_size, camera_sw_example_decoded,
            sizeof(camera_sw_example_decoded), &decoded_width, &decoded_height,
            0U, &decode_metrics);
    checksum = camera_sw_example_checksum(
        camera_sw_example_decoded,
        (uint32_t)decoded_width * decoded_height * 2U);
    rt_kprintf("camera_sw_acpu example: jpeg_size=%u decoded=%ux%u "
               "checksum=%08x isp_flags=%08x encode_ms=%u status=%s\n",
               jpeg_size, decoded_width, decoded_height, checksum,
               isp_metrics.isp.applied_flags, encode_ms,
               (status == 0) && (decoded_width == CAMERA_SW_EXAMPLE_WIDTH) &&
               (decoded_height == CAMERA_SW_EXAMPLE_HEIGHT) &&
               (isp_metrics.isp.applied_flags == flags) ? "PASS" : "FAIL");
    return (status == 0) && (decoded_width == CAMERA_SW_EXAMPLE_WIDTH) &&
           (decoded_height == CAMERA_SW_EXAMPLE_HEIGHT) &&
           (isp_metrics.isp.applied_flags == flags) ? 0 : -1;
}
