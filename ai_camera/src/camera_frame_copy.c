/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_frame_copy.h"

#include <bf0_hal.h>
#include <limits.h>
#include <rtthread.h>
#include <rthw.h>
#include <string.h>

#if !defined(DMA_SUPPORT_DYN_CHANNEL_ALLOC) || !defined(GPDMA1_BASE)
#error Camera frame copy requires dynamically allocated GPDMA1 channels
#endif

#define CAMERA_COPY_CACHE_LINE 32U
#define CAMERA_COPY_IRQ_PRIORITY 1U
#define CAMERA_COPY_TIMEOUT_MS 500U

static DMA_HandleTypeDef s_dma;
static struct rt_semaphore s_done_sem;
static int s_sem_initialized;
static volatile int s_waiting;
static volatile int s_done;
static volatile int s_result;
static volatile rt_tick_t s_done_tick;

static uint32_t camera_copy_ms(rt_tick_t begin, rt_tick_t end)
{
    return (uint32_t)((uint64_t)(rt_tick_t)(end - begin) * 1000U /
                      RT_TICK_PER_SECOND);
}

static int camera_copy_hal_error(HAL_StatusTypeDef status)
{
    if (status == HAL_BUSY)
        return -RT_EBUSY;
    if (status == HAL_TIMEOUT)
        return -RT_ETIMEOUT;
    return -RT_ERROR;
}

static void camera_copy_done(DMA_HandleTypeDef *dma, int result)
{
    if (dma == &s_dma && s_waiting && !s_done)
    {
        s_result = result;
        s_done_tick = rt_tick_get();
        s_done = 1;
        rt_sem_release(&s_done_sem);
    }
}

static void camera_copy_complete(DMA_HandleTypeDef *dma)
{
    camera_copy_done(dma, RT_EOK);
}

static void camera_copy_error(DMA_HandleTypeDef *dma)
{
    camera_copy_done(dma, -RT_ERROR);
}

static int camera_copy_init(void)
{
    HAL_StatusTypeDef status;
    rt_err_t error;

    if (!s_sem_initialized)
    {
        error = rt_sem_init(&s_done_sem, "camcpy", 0, RT_IPC_FLAG_FIFO);
        if (error != RT_EOK)
            return error > RT_EOK ? -error : error;
        s_sem_initialized = 1;
    }

    /* Prefer channel 3; the HAL selects another free channel when needed. */
    memset(&s_dma, 0, sizeof(s_dma));
    s_dma.Instance = DMA1_Channel3;
    s_dma.Init.Request = 0;
    s_dma.Init.Direction = DMA_MEMORY_TO_MEMORY;
    s_dma.Init.Mode = DMA_NORMAL;
    s_dma.Init.Priority = DMA_PRIORITY_LOW;
    s_dma.Init.PeriphInc = DMA_PINC_ENABLE;
    s_dma.Init.MemInc = DMA_MINC_ENABLE;
    s_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    s_dma.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
    s_dma.Init.IrqPrio = CAMERA_COPY_IRQ_PRIORITY;
    status = HAL_DMA_Init(&s_dma);
    if (status != HAL_OK)
        return camera_copy_hal_error(status);
    s_dma.XferCpltCallback = camera_copy_complete;
    s_dma.XferErrorCallback = camera_copy_error;
    return RT_EOK;
}

/* READY follows final completion/error or synchronous abort and releases the
 * dynamic channel. Its old registers can already belong to another DMA user. */
static void camera_copy_stop(void)
{
    for (;;)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        HAL_StatusTypeDef status = HAL_OK;

        if (s_dma.State != HAL_DMA_STATE_READY)
            status = HAL_DMA_Abort(&s_dma);
        __DSB();
        if (status == HAL_OK && s_dma.State == HAL_DMA_STATE_READY)
        {
            rt_hw_interrupt_enable(level);
            return;
        }
        rt_hw_interrupt_enable(level);

        /* Retain both buffers until completion or a successful abort. */
        rt_thread_mdelay(1);
    }
}

int camera_frame_copy_half(uint8_t *dst, const uint8_t *src,
                           uint16_t width, uint16_t height, unsigned int half,
                           camera_frame_copy_timing_t *timing)
{
    camera_frame_copy_timing_t measured = {0};
    uint32_t half_bytes;
    uint32_t frame_bytes;
    uint32_t offset;
    uintptr_t source = (uintptr_t)src;
    uintptr_t destination = (uintptr_t)dst;
    rt_tick_t begin;
    rt_tick_t submit_begin;
    rt_tick_t submit_end;
    rt_tick_t callback_tick;
    rt_tick_t wake_tick;
    rt_tick_t finish_begin;
    rt_base_t level;
    rt_err_t wait_status;
    HAL_StatusTypeDef dma_status;
    int completed;
    int result;

    if (timing != RT_NULL)
        memset(timing, 0, sizeof(*timing));
    if (dst == RT_NULL || src == RT_NULL || width == 0U || height == 0U ||
        (height & 1U) != 0U || half > 1U || width > (uint32_t)INT16_MAX + 1U)
        return -RT_EINVAL;

    half_bytes = (uint32_t)width * (height / 2U) * 2U;
    frame_bytes = half_bytes * 2U;
    if (((source | destination | half_bytes) & (CAMERA_COPY_CACHE_LINE - 1U)) != 0U ||
        source > UINTPTR_MAX - frame_bytes ||
        destination > UINTPTR_MAX - frame_bytes ||
        (source < destination + frame_bytes && destination < source + frame_bytes))
        return -RT_EINVAL;

    offset = half * half_bytes;
    begin = rt_tick_get();
    result = camera_copy_init();
    if (result != RT_EOK)
        goto done;

    s_waiting = 0;
    while (rt_sem_take(&s_done_sem, 0) == RT_EOK)
    {
    }
    s_done = 0;
    s_result = -RT_ERROR;
    s_done_tick = 0;

    /* DCMI wrote the source. Discard stale CPU lines before HAL source clean
     * can write them back; prepare only the destination half we exclusively own. */
    SCB_InvalidateDCache_by_Addr((void *)(src + offset), (int32_t)half_bytes);
    SCB_CleanInvalidateDCache_by_Addr((void *)(dst + offset), (int32_t)half_bytes);
    submit_begin = rt_tick_get();
    measured.prepare_ms = camera_copy_ms(begin, submit_begin);
    s_waiting = 1;
    dma_status = HAL_DMA_Start_IT(&s_dma, (uint32_t)(source + offset),
                                 (uint32_t)(destination + offset), half_bytes / 4U);
    submit_end = rt_tick_get();
    measured.submit_ms = camera_copy_ms(submit_begin, submit_end);
    if (dma_status != HAL_OK)
    {
        s_waiting = 0;
        camera_copy_stop();
        result = camera_copy_hal_error(dma_status);
        goto invalidate;
    }

    wait_status = rt_sem_take(&s_done_sem,
                              rt_tick_from_millisecond(CAMERA_COPY_TIMEOUT_MS));
    level = rt_hw_interrupt_disable();
    wake_tick = rt_tick_get();
    completed = s_done;
    callback_tick = s_done_tick;
    result = s_result;
    s_waiting = 0;
    rt_hw_interrupt_enable(level);

    if (completed)
    {
        if (result == RT_EOK)
            measured.resume_ms = camera_copy_ms(callback_tick, wake_tick);
    }
    else
    {
        result = wait_status == -RT_ETIMEOUT ? -RT_ETIMEOUT : -RT_ERROR;
    }
    camera_copy_stop();

invalidate:
    finish_begin = rt_tick_get();
    SCB_InvalidateDCache_by_Addr((void *)(dst + offset), (int32_t)half_bytes);
    measured.finish_ms = camera_copy_ms(finish_begin, rt_tick_get());
done:
    measured.total_ms = camera_copy_ms(begin, rt_tick_get());
    if (timing != RT_NULL)
        *timing = measured;
    return result;
}
