/* SPDX-License-Identifier: MIT */
#include <rtthread.h>

#include "camera_sw_isp_pipeline.h"

#if defined(CAMERA_SW_ISP_EXAMPLE_ENABLED)

#define CAMERA_SW_EXAMPLE_WIDTH 32U
#define CAMERA_SW_EXAMPLE_HEIGHT 24U

static uint8_t camera_sw_example_pixels[
    CAMERA_SW_EXAMPLE_WIDTH * CAMERA_SW_EXAMPLE_HEIGHT * 2U];

static uint32_t camera_sw_example_now_ms(void)
{
    return (uint32_t)((uint64_t)rt_tick_get() * 1000U / RT_TICK_PER_SECOND);
}

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
    camera_sw_isp_result_t result;
    uint32_t input_checksum;
    uint32_t output_checksum;
    uint32_t flags = 0U;
    int status;

#if defined(CAMERA_SW_ISP_VIVID)
    flags = CAMERA_SW_ISP_FLAG_AWB_STATS |
            CAMERA_SW_ISP_FLAG_AWB_APPLY |
            CAMERA_SW_ISP_FLAG_GAMMA |
            CAMERA_SW_ISP_FLAG_TONE |
            CAMERA_SW_ISP_FLAG_VIVID;
#if defined(CAMERA_SW_ISP_DITHER)
    flags |= CAMERA_SW_ISP_FLAG_DITHER;
#endif
#else
#if defined(CAMERA_SW_ISP_AWB)
    flags |= CAMERA_SW_ISP_FLAG_AWB_STATS | CAMERA_SW_ISP_FLAG_AWB_APPLY;
#endif
#if defined(CAMERA_SW_ISP_GAMMA)
    flags |= CAMERA_SW_ISP_FLAG_GAMMA;
#endif
#if defined(CAMERA_SW_ISP_TONE)
    flags |= CAMERA_SW_ISP_FLAG_AWB_STATS | CAMERA_SW_ISP_FLAG_TONE;
#endif
#if defined(CAMERA_SW_ISP_DITHER)
    if ((flags & CAMERA_SW_ISP_FLAG_GAMMA) != 0U)
        flags |= CAMERA_SW_ISP_FLAG_DITHER;
#endif
#endif
    camera_sw_example_fill_fixture();
    input_checksum = camera_sw_example_checksum(
        camera_sw_example_pixels, sizeof(camera_sw_example_pixels));
    status = camera_sw_isp_process_rgb565_be(
        camera_sw_example_pixels, CAMERA_SW_EXAMPLE_WIDTH,
        CAMERA_SW_EXAMPLE_HEIGHT, CAMERA_SW_EXAMPLE_WIDTH * 2U, flags,
        &result, camera_sw_example_now_ms);
    output_checksum = camera_sw_example_checksum(
        camera_sw_example_pixels, sizeof(camera_sw_example_pixels));
    rt_kprintf("camera_sw_isp example: input_checksum=%08x "
               "output_checksum=%08x flags=%08x status=%s\n",
               input_checksum, output_checksum, result.applied_flags,
               (status == 0) && (result.applied_flags == flags) ? "PASS" : "FAIL");
    return (status == 0) && (result.applied_flags == flags) ? 0 : -1;
}

#else

int main(void)
{
    rt_kprintf("camera_sw_isp example: select Software ISP and HCPU in menuconfig\n");
    return -1;
}

#endif
