#include <acpu_ctrl.h>
#include <finsh.h>
#include <mem_section.h>
#include <rtthread.h>

#include "camera_acpu_client.h"
#include "camera_acpu_checksum.h"
#include "camera_acpu_protocol.h"

#define CAMERA_ACPU_JPEG_WIDTH 390U
#define CAMERA_ACPU_JPEG_HEIGHT 450U
#define CAMERA_ACPU_JPEG_INPUT_SIZE \
    (CAMERA_ACPU_JPEG_WIDTH * CAMERA_ACPU_JPEG_HEIGHT * 2U)
#define CAMERA_ACPU_JPEG_OUTPUT_SIZE (384U * 1024U)

L2_NON_RET_BSS_SECT_BEGIN(camera_acpu_buffers)
L2_NON_RET_BSS_SECT(camera_acpu_buffers,
                    ALIGN(64) static uint8_t camera_acpu_buffer[
                        CAMERA_ACPU_BUFFER_SIZE]);
L2_NON_RET_BSS_SECT(camera_acpu_buffers,
                    ALIGN(64) static uint8_t camera_acpu_jpeg_input[
                        CAMERA_ACPU_JPEG_INPUT_SIZE]);
L2_NON_RET_BSS_SECT(camera_acpu_buffers,
                    ALIGN(64) static uint8_t camera_acpu_jpeg_output[
                        CAMERA_ACPU_JPEG_OUTPUT_SIZE]);
L2_NON_RET_BSS_SECT_END

static int camera_acpu_bench(int argc, char **argv)
{
    camera_acpu_checksum_task_t task;
    uint32_t expected;
    uint32_t index;
    uint8_t error_code = 0U;
    rt_tick_t started;
    rt_tick_t elapsed;

    (void)argc;
    (void)argv;
    for (index = 0U; index < sizeof(camera_acpu_buffer); index++)
        camera_acpu_buffer[index] = (uint8_t)(index * 17U + 3U);

    expected = camera_acpu_checksum(camera_acpu_buffer,
                                    sizeof(camera_acpu_buffer));
    task.input = camera_acpu_buffer;
    task.input_size = sizeof(camera_acpu_buffer);
    task.checksum = 0U;

    started = rt_tick_get();
    (void)acpu_run_task(CAMERA_ACPU_TASK_CHECKSUM, &task, sizeof(task),
                        &error_code);
    elapsed = rt_tick_get() - started;

    rt_kprintf("camera acpu: bytes=%u checksum=%08x expected=%08x "
               "error=%u elapsed=%u ms %s\n",
               task.input_size, task.checksum, expected, error_code,
               (unsigned)(elapsed * 1000U / RT_TICK_PER_SECOND),
               (error_code == 0U) && (task.checksum == expected) ?
                   "passed" : "failed");
    return (error_code == 0U) && (task.checksum == expected) ? 0 : -1;
}
MSH_CMD_EXPORT_ALIAS(camera_acpu_bench, acpu_bench,
                     validate ACPU shared PSRAM access);

static int camera_acpu_jpeg_bench(int argc, char **argv)
{
    camera_acpu_jpeg_task_t task;
    uint32_t x;
    uint32_t y;
    uint8_t error_code = 0U;
    rt_tick_t started;
    rt_tick_t elapsed;
    rt_bool_t valid;

    (void)argc;
    (void)argv;
    for (y = 0U; y < CAMERA_ACPU_JPEG_HEIGHT; y++)
    {
        for (x = 0U; x < CAMERA_ACPU_JPEG_WIDTH; x++)
        {
            uint16_t pixel = (uint16_t)(((x & 0x1FU) << 11) |
                                        ((y & 0x3FU) << 5) |
                                        ((x + y) & 0x1FU));
            uint32_t offset = (y * CAMERA_ACPU_JPEG_WIDTH + x) * 2U;

            camera_acpu_jpeg_input[offset] = (uint8_t)pixel;
            camera_acpu_jpeg_input[offset + 1U] = (uint8_t)(pixel >> 8);
        }
    }
    task.input = camera_acpu_jpeg_input;
    task.output = camera_acpu_jpeg_output;
    task.input_pitch = CAMERA_ACPU_JPEG_WIDTH * 2U;
    task.output_capacity = sizeof(camera_acpu_jpeg_output);
    task.output_size = 0U;
    task.width = CAMERA_ACPU_JPEG_WIDTH;
    task.height = CAMERA_ACPU_JPEG_HEIGHT;
    task.quality = 1U;
    task.input_big_endian = 0U;

    started = rt_tick_get();
    (void)acpu_run_task(CAMERA_ACPU_TASK_JPEG, &task, sizeof(task),
                        &error_code);
    elapsed = rt_tick_get() - started;
    valid = (error_code == 0U) && (task.output_size >= 4U) &&
            (camera_acpu_jpeg_output[0] == 0xFFU) &&
            (camera_acpu_jpeg_output[1] == 0xD8U) &&
            (camera_acpu_jpeg_output[task.output_size - 2U] == 0xFFU) &&
            (camera_acpu_jpeg_output[task.output_size - 1U] == 0xD9U);
    rt_kprintf("camera acpu jpeg: %ux%u size=%u error=%u elapsed=%u ms %s\n",
               task.width, task.height, task.output_size, error_code,
               (unsigned)(elapsed * 1000U / RT_TICK_PER_SECOND),
               valid ? "passed" : "failed");
    return valid ? 0 : -1;
}
MSH_CMD_EXPORT_ALIAS(camera_acpu_jpeg_bench, acpu_jpeg_bench,
                     benchmark ACPU software JPEG encoder);

static int camera_acpu_cpu_usage(int argc, char **argv)
{
    int32_t usage_x100 = -1;

    (void)argc;
    (void)argv;
    if (camera_acpu_query_cpu_usage(&usage_x100) != 0)
    {
        rt_kprintf("camera acpu: cpu usage unavailable; retry after 2 s\n");
        return -1;
    }
    rt_kprintf("camera acpu: cpu usage=%d.%02d%%\n",
               usage_x100 / 100, usage_x100 % 100);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(camera_acpu_cpu_usage, acpu_cpu,
                     show ACPU CPU usage);

int camera_acpu_query_cpu_usage(int32_t *usage_x100)
{
    camera_acpu_cpu_usage_task_t task = {.usage_x100 = -1};
    uint8_t error_code = 0U;

    if (usage_x100 == RT_NULL)
        return -1;
    (void)acpu_run_task(CAMERA_ACPU_TASK_CPU_USAGE, &task, sizeof(task),
                        &error_code);
    if ((error_code != 0U) || (task.usage_x100 < 0))
        return -1;
    *usage_x100 = task.usage_x100;
    return 0;
}

int camera_acpu_encode_rgb565_be(uint8_t *pixels, uint16_t width,
                                 uint16_t height, uint32_t pitch,
                                 const uint8_t **jpeg,
                                 uint32_t *jpeg_size,
                                 uint32_t *elapsed_ms)
{
    camera_acpu_jpeg_task_t task;
    uint8_t error_code = 0U;
    rt_tick_t started;
    rt_tick_t elapsed;

    if ((pixels == RT_NULL) || (width == 0U) || (height == 0U) ||
        (pitch < (uint32_t)width * 2U) || (jpeg == RT_NULL) ||
        (jpeg_size == RT_NULL) || (elapsed_ms == RT_NULL))
        return -1;

    task.input = pixels;
    task.output = camera_acpu_jpeg_output;
    task.input_pitch = pitch;
    task.output_capacity = sizeof(camera_acpu_jpeg_output);
    task.output_size = 0U;
    task.width = width;
    task.height = height;
    task.quality = 1U;
    task.input_big_endian = 1U;

    started = rt_tick_get();
    (void)acpu_run_task(CAMERA_ACPU_TASK_JPEG, &task, sizeof(task),
                        &error_code);
    elapsed = rt_tick_get() - started;
    *elapsed_ms = (uint32_t)(elapsed * 1000U / RT_TICK_PER_SECOND);
    if ((error_code != 0U) || (task.output_size < 4U) ||
        (camera_acpu_jpeg_output[0] != 0xFFU) ||
        (camera_acpu_jpeg_output[1] != 0xD8U) ||
        (camera_acpu_jpeg_output[task.output_size - 2U] != 0xFFU) ||
        (camera_acpu_jpeg_output[task.output_size - 1U] != 0xD9U))
        return -1;

    *jpeg = camera_acpu_jpeg_output;
    *jpeg_size = task.output_size;
    return 0;
}

int camera_acpu_decode_jpeg(const uint8_t *jpeg, uint32_t jpeg_size,
                            uint8_t *destination,
                            uint32_t destination_size,
                            uint16_t *width, uint16_t *height,
                            uint32_t *elapsed_ms)
{
    camera_acpu_jpeg_decode_task_t task;
    uint8_t error_code = 0U;
    rt_tick_t started;
    rt_tick_t elapsed;

    if ((jpeg == RT_NULL) || (jpeg_size < 4U) ||
        (destination == RT_NULL) || (destination_size == 0U) ||
        (width == RT_NULL) || (height == RT_NULL) ||
        (elapsed_ms == RT_NULL))
        return -1;

    task.input = jpeg;
    task.output = destination;
    task.input_size = jpeg_size;
    task.output_capacity = destination_size;
    task.width = 0U;
    task.height = 0U;

    started = rt_tick_get();
    (void)acpu_run_task(CAMERA_ACPU_TASK_JPEG_DECODE, &task, sizeof(task),
                        &error_code);
    elapsed = rt_tick_get() - started;
    *elapsed_ms = (uint32_t)(elapsed * 1000U / RT_TICK_PER_SECOND);
    if ((error_code != 0U) || (task.width == 0U) || (task.height == 0U) ||
        ((uint32_t)task.width * task.height * 2U > destination_size))
        return -1;

    *width = task.width;
    *height = task.height;
    return 0;
}
