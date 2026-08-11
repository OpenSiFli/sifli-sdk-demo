#include <acpu_ctrl.h>
#include <rtconfig.h>
#include <cpu_usage_profiler.h>
#include <stdint.h>

#include "camera_acpu_checksum.h"
#include "camera_acpu_protocol.h"
#include "camera_jpeg_encoder.h"
#include "jpeg_rgb565_decoder.h"

#define CAMERA_ACPU_JPEG_DECODE_WORK_SIZE 16384U

static uint8_t camera_acpu_jpeg_decode_work[
    CAMERA_ACPU_JPEG_DECODE_WORK_SIZE];

void acpu_main(uint8_t task_name, void *param)
{
    if (task_name == CAMERA_ACPU_TASK_CPU_USAGE)
    {
        camera_acpu_cpu_usage_task_t *task = param;
        float usage;

        if (task == NULL)
        {
            acpu_send_result(1U, 0U);
            return;
        }
        usage = cpu_get_usage();
        task->usage_x100 = usage < 0.0f ? -1 : (int32_t)(usage * 100.0f);
        acpu_send_result(0U, 0U);
        return;
    }

    if (task_name == CAMERA_ACPU_TASK_JPEG_DECODE)
    {
        camera_acpu_jpeg_decode_task_t *task = param;
        int result;

        if (task == NULL)
        {
            acpu_send_result(1U, 0U);
            return;
        }
        result = jpeg_rgb565_decode(
            task->input, task->input_size, task->output,
            task->output_capacity, camera_acpu_jpeg_decode_work,
            sizeof(camera_acpu_jpeg_decode_work), &task->width,
            &task->height);
        acpu_send_result(result == 0 ? 0U : 1U, 0U);
        return;
    }

    if (task_name == CAMERA_ACPU_TASK_JPEG)
    {
        camera_acpu_jpeg_task_t *task = param;
        int result;

        if (task == NULL)
        {
            acpu_send_result(1U, 0U);
            return;
        }
        if (task->input_big_endian != 0U)
            result = camera_jpeg_encode_rgb565_be(
                task->input, task->width, task->height, task->input_pitch,
                (camera_jpeg_quality_t)task->quality, task->output,
                task->output_capacity, &task->output_size);
        else
            result = camera_jpeg_encode_rgb565(
                task->input, task->width, task->height, task->input_pitch,
                (camera_jpeg_quality_t)task->quality, task->output,
                task->output_capacity, &task->output_size);
        acpu_send_result(result == 0 ? 0U : 1U, task->output_size);
        return;
    }

    camera_acpu_checksum_task_t *task = param;

    if ((task_name != CAMERA_ACPU_TASK_CHECKSUM) || (task == NULL) ||
        (task->input == NULL) || (task->input_size == 0U))
    {
        acpu_send_result(1U, 0U);
        return;
    }

    task->checksum = camera_acpu_checksum(task->input, task->input_size);
    acpu_send_result(0U, task->checksum);
}
