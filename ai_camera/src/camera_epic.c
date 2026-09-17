/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_epic.h"

#include <drv_epic.h>
#include <limits.h>
#include <rtthread.h>

#define CAMERA_EPIC_CACHE_LINE 32U

static struct rt_semaphore s_copy_done;
static int s_copy_done_initialized;
static volatile rt_tick_t s_copy_done_tick;

static void camera_epic_copy_done(EPIC_HandleTypeDef *epic)
{
    (void)epic;
    s_copy_done_tick = rt_tick_get();
    rt_sem_release(&s_copy_done);
}

static uint32_t camera_epic_ticks_to_ms(rt_tick_t ticks)
{
    return (uint32_t)(((uint64_t)ticks * 1000U) / RT_TICK_PER_SECOND);
}

int camera_epic_render_frame(uint8_t *dst, const uint8_t *src,
                             uint16_t width, uint16_t height,
                             camera_epic_timing_t *timing)
{
    EPIC_AreaTypeDef area;
    uint32_t frame_bytes;
    uintptr_t source = (uintptr_t)src;
    uintptr_t destination = (uintptr_t)dst;
    rt_tick_t begin;
    rt_tick_t submit_begin;
    rt_tick_t submit_end;
    rt_tick_t resumed;
    rt_tick_t end;
    rt_err_t wait_status;
    int result;
    int completion_received = 0;

    if (timing != RT_NULL)
    {
        timing->total_ms = 0U;
        timing->submit_ms = 0U;
        timing->resume_ms = 0U;
    }
    if (dst == RT_NULL || src == RT_NULL || width == 0U || height == 0U ||
        width > (uint32_t)INT16_MAX + 1U || height > (uint32_t)INT16_MAX + 1U)
        return -RT_EINVAL;

    frame_bytes = (uint32_t)width * height * 2U;
    if (frame_bytes > INT32_MAX ||
        ((source | destination | frame_bytes) & (CAMERA_EPIC_CACHE_LINE - 1U)) != 0U ||
        source > UINTPTR_MAX - frame_bytes ||
        destination > UINTPTR_MAX - frame_bytes ||
        (source < destination + frame_bytes && destination < source + frame_bytes))
        return -RT_EINVAL;

    if (!s_copy_done_initialized)
    {
        result = rt_sem_init(&s_copy_done, "cam_epic", 0, RT_IPC_FLAG_FIFO);
        if (result != RT_EOK)
            return result > RT_EOK ? -result : result;
        s_copy_done_initialized = 1;
    }
    /* Calls are serialized; the previous transfer has already retired. */
    while (rt_sem_take(&s_copy_done, 0) == RT_EOK)
    {
    }

    area.x0 = 0;
    area.y0 = 0;
    area.x1 = (int16_t)(width - 1U);
    area.y1 = (int16_t)(height - 1U);

    begin = rt_tick_get();
    /* The source is written by DCMI; discard stale CPU cache lines before
     * the EPIC driver performs its source cache maintenance. */
    SCB_InvalidateDCache_by_Addr((void *)src, (int32_t)frame_bytes);
    submit_begin = rt_tick_get();
    result = drv_epic_copy(src, dst, &area, &area, &area,
                           EPIC_COLOR_RGB565, EPIC_COLOR_RGB565,
                           camera_epic_copy_done);
    submit_end = rt_tick_get();
    if (result > RT_EOK)
        result = -result;

    if (result == RT_EOK)
    {
        /* This callback follows the last split and destination cache update.
         * A later LVGL operation can own EPIC without extending this wait. */
        wait_status = rt_sem_take(&s_copy_done,
                                  rt_tick_from_millisecond(DRV_EPIC_TIMEOUT_MS));
        if (wait_status != RT_EOK)
            result = wait_status > RT_EOK ? -wait_status : wait_status;
        else
            completion_received = 1;
    }
    resumed = rt_tick_get();

    if (result != RT_EOK)
    {
        /* Keep both buffers borrowed through error recovery. The shared
         * driver may still own them after a failed submission or wait. */
        do
        {
            wait_status = drv_epic_wait_done();
        }
        while (wait_status != RT_EOK);
        /* The single-operation driver registers this callback before HAL
         * submission, including failed starts. Recovery releases its shared
         * semaphore before invoking the callback, so also retire that callback
         * before the next call can reuse this private completion semaphore. */
        if (!completion_received)
        {
            while (rt_sem_take(&s_copy_done,
                               rt_tick_from_millisecond(DRV_EPIC_TIMEOUT_MS)) != RT_EOK)
            {
            }
        }
    }

    end = rt_tick_get();
    /* The shared driver can invoke completion after timeout recovery. */
    if (result == RT_EOK && (rt_tick_t)(end - begin) >=
        rt_tick_from_millisecond(DRV_EPIC_TIMEOUT_MS))
        result = -RT_ETIMEOUT;

    if (timing != RT_NULL)
    {
        timing->total_ms = camera_epic_ticks_to_ms((rt_tick_t)(end - begin));
        timing->submit_ms = camera_epic_ticks_to_ms((rt_tick_t)(submit_end - submit_begin));
        if (completion_received)
            timing->resume_ms = camera_epic_ticks_to_ms((rt_tick_t)(resumed - s_copy_done_tick));
    }
    return result;
}
