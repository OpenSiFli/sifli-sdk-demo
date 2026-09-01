/* SPDX-License-Identifier: MIT */
#include <acpu_ctrl.h>
#include <cpu_usage_profiler.h>
#include <rtthread.h>
#include <string.h>

#include "camera_sw_acpu_protocol.h"
#if defined(CAMERA_SW_ACPU_ISP)
#include "camera_sw_isp_pipeline.h"
#endif
#if defined(CAMERA_SW_ACPU_JPEG_ENCODER)
#include "camera_sw_jpeg_encoder.h"
#endif
#if defined(CAMERA_SW_ACPU_JPEG_DECODER)
#include "camera_sw_jpeg_decoder.h"
#endif

#define CAMERA_SW_ACPU_JPEG_DECODE_WORK_SIZE 16384U
#define CAMERA_SW_ACPU_JPEG_DECODE_MAX_WIDTH 640U

#if defined(CAMERA_SW_ACPU_JPEG_ENCODER)
static uint8_t camera_sw_acpu_jpeg_workspace[
    CAMERA_SW_JPEG_ENCODER_STATIC_WORKSPACE_SIZE] __attribute__((aligned(8)));
static camera_sw_jpeg_encoder_t camera_sw_acpu_jpeg_encoder;
static int camera_sw_acpu_jpeg_encoder_ready;
#endif

#if defined(CAMERA_SW_ACPU_JPEG_DECODER)
typedef struct
{
    const uint8_t *data;
    uint32_t size;
    uint32_t offset;
} camera_sw_acpu_jpeg_reader_t;

static uint8_t camera_sw_acpu_jpeg_decode_work[
    CAMERA_SW_ACPU_JPEG_DECODE_WORK_SIZE];

static size_t camera_sw_acpu_jpeg_read(void *context, uint8_t *buffer,
                                       size_t size)
{
    camera_sw_acpu_jpeg_reader_t *reader = context;
    uint32_t remaining;

    if ((reader == NULL) || (reader->offset > reader->size))
        return 0U;
    remaining = reader->size - reader->offset;
    if (size > remaining)
        size = remaining;
    if ((buffer != NULL) && (size != 0U))
        memcpy(buffer, reader->data + reader->offset, size);
    reader->offset += (uint32_t)size;
    return size;
}
#endif

static uint32_t camera_sw_acpu_now_ms(void)
{
    return (uint32_t)(rt_tick_get() * 1000U / RT_TICK_PER_SECOND);
}

static int camera_sw_acpu_valid_isp_task(uint32_t protocol_version,
                                         uint32_t isp_flags)
{
    return (protocol_version == CAMERA_SW_ACPU_PROTOCOL_VERSION) &&
           ((isp_flags & ~CAMERA_SW_ACPU_ISP_FLAG_ALL) == 0U) &&
           (((isp_flags & CAMERA_SW_ACPU_ISP_FLAG_AWB_APPLY) == 0U) ||
            ((isp_flags & CAMERA_SW_ACPU_ISP_FLAG_AWB_STATS) != 0U));
}

#if defined(CAMERA_SW_ACPU_JPEG_ENCODER)
static int camera_sw_acpu_encode(camera_sw_acpu_jpeg_task_t *task)
{
    camera_sw_jpeg_frame_t frame;

    if ((task == NULL) || (task->input == NULL) || (task->output == NULL) ||
        (task->width == 0U) || (task->height == 0U) ||
        (task->input_pitch < (uint32_t)task->width * 2U) ||
        (task->quality > CAMERA_SW_JPEG_QUALITY_LOW))
        return -1;
    if (!camera_sw_acpu_jpeg_encoder_ready)
    {
        if (camera_sw_jpeg_encoder_init(&camera_sw_acpu_jpeg_encoder,
                                        camera_sw_acpu_jpeg_workspace,
                                        sizeof(camera_sw_acpu_jpeg_workspace)) != 0)
            return -1;
        camera_sw_acpu_jpeg_encoder_ready = 1;
    }
    frame.data = (uint8_t *)task->input;
    frame.width = task->width;
    frame.height = task->height;
    frame.stride = task->input_pitch;
    frame.pixel_format = task->input_big_endian ?
        CAMERA_SW_JPEG_PIXEL_RGB565_BE : CAMERA_SW_JPEG_PIXEL_RGB565;
    return camera_sw_jpeg_encode_to_buffer(
        &camera_sw_acpu_jpeg_encoder, &frame,
        (camera_sw_jpeg_quality_t)task->quality, task->output,
        task->output_capacity, &task->output_size);
}
#endif

#if defined(CAMERA_SW_ACPU_JPEG_DECODER)
static int camera_sw_acpu_decode(camera_sw_acpu_jpeg_decode_task_t *task)
{
    camera_sw_acpu_jpeg_reader_t reader;
    camera_sw_jpeg_frame_t output;
    uint32_t started;
    uint32_t decode_ms;
    uint32_t rows;

    if ((task == NULL) || (task->input == NULL) || (task->input_size < 4U) ||
        (task->output == NULL) ||
        !camera_sw_acpu_valid_isp_task(task->protocol_version, task->isp_flags))
        return -1;
    rows = task->output_capacity / (CAMERA_SW_ACPU_JPEG_DECODE_MAX_WIDTH * 2U);
    if ((rows == 0U) || (rows > UINT16_MAX))
        return -1;
    reader.data = task->input;
    reader.size = task->input_size;
    reader.offset = 0U;
    output.data = task->output;
    output.width = 0U;
    output.height = (uint16_t)rows;
    output.stride = CAMERA_SW_ACPU_JPEG_DECODE_MAX_WIDTH * 2U;
    output.pixel_format = CAMERA_SW_JPEG_PIXEL_RGB565_BE;
    memset(&task->isp_result, 0, sizeof(task->isp_result));
    started = camera_sw_acpu_now_ms();
    if (camera_sw_jpeg_decode(camera_sw_acpu_jpeg_read, &reader,
                              camera_sw_acpu_jpeg_decode_work,
                              sizeof(camera_sw_acpu_jpeg_decode_work),
                              &output) != 0)
        return -1;
    decode_ms = camera_sw_acpu_now_ms() - started;
    task->width = output.width;
    task->height = output.height;
#if defined(CAMERA_SW_ACPU_ISP)
    if (camera_sw_isp_process_rgb565_be(
        task->output, task->width, task->height, output.stride,
        task->isp_flags, &task->isp_result, camera_sw_acpu_now_ms) != 0)
        return -1;
    task->isp_result.decode_ms = decode_ms;
    return 0;
#else
    task->isp_result.decode_ms = decode_ms;
    return task->isp_flags == 0U ? 0 : -1;
#endif
}
#endif

void acpu_main(uint8_t task_name, void *param)
{
    if (task_name == CAMERA_SW_ACPU_TASK_CPU_USAGE)
    {
        camera_sw_acpu_cpu_usage_task_t *task = param;
        float usage;

        if (task == NULL)
        {
            acpu_send_result(ACPU_ERR_ASSERT, 0U);
            return;
        }
        usage = cpu_get_usage();
        task->usage_x100 = usage < 0.0f ? -1 : (int32_t)(usage * 100.0f);
        task->status = usage < 0.0f ? -1 : 0;
        acpu_send_result(ACPU_ERR_OK, 0U);
        return;
    }

#if defined(CAMERA_SW_ACPU_JPEG_DECODER)
    if (task_name == CAMERA_SW_ACPU_TASK_JPEG_DECODE)
    {
        camera_sw_acpu_jpeg_decode_task_t *task = param;
        int result = camera_sw_acpu_decode(task);

        if (task == NULL)
        {
            acpu_send_result(ACPU_ERR_ASSERT, 0U);
            return;
        }
        task->status = result;
        acpu_send_result(ACPU_ERR_OK, 0U);
        return;
    }
#endif

#if defined(CAMERA_SW_ACPU_ISP)
    if (task_name == CAMERA_SW_ACPU_TASK_ISP)
    {
        camera_sw_acpu_isp_task_t *task = param;
        int result;

        if ((task == NULL) ||
            !camera_sw_acpu_valid_isp_task(task->protocol_version,
                                            task->isp_flags) ||
            (task->height == 0U) ||
            (task->stride < (uint32_t)task->width * 2U) ||
            (task->stride > task->capacity / task->height))
        {
            if (task == NULL)
            {
                acpu_send_result(ACPU_ERR_ASSERT, 0U);
                return;
            }
            task->status = -1;
            acpu_send_result(ACPU_ERR_OK, 0U);
            return;
        }
        result = camera_sw_isp_process_rgb565_be(
            task->pixels, task->width, task->height, task->stride,
            task->isp_flags, &task->isp_result, camera_sw_acpu_now_ms);
        task->status = result;
        acpu_send_result(ACPU_ERR_OK, 0U);
        return;
    }
#endif

#if defined(CAMERA_SW_ACPU_JPEG_ENCODER)
    if (task_name == CAMERA_SW_ACPU_TASK_JPEG)
    {
        camera_sw_acpu_jpeg_task_t *task = param;
        int result = camera_sw_acpu_encode(task);

        if (task == NULL)
        {
            acpu_send_result(ACPU_ERR_ASSERT, 0U);
            return;
        }
        task->status = result;
        acpu_send_result(ACPU_ERR_OK,
                         result == 0 ? task->output_size : 0U);
        return;
    }
#endif

    acpu_send_result(ACPU_ERR_ASSERT, 0U);
}
