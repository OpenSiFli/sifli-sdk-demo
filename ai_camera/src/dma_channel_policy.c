/* SPDX-License-Identifier: Apache-2.0 */
#include "dma_channel_policy.h"

#include <bf0_hal.h>
#include <rtthread.h>

/* Alias the linker creates for the original function because of
 * '-Wl,--wrap=HAL_DMA_Init' (see SConscript). */
extern HAL_StatusTypeDef __real_HAL_DMA_Init(DMA_HandleTypeDef *hdma);

static int s_audio_tx_channel_logged;

HAL_StatusTypeDef __wrap_HAL_DMA_Init(DMA_HandleTypeDef *hdma)
{
    if (hdma != RT_NULL && hdma->Init.Request == AI_CAMERA_AUDIO_TX_DMA_REQUEST &&
        hdma->Instance != AI_CAMERA_AUDIO_TX_DMA_INSTANCE)
    {
        /* Must happen before the real call: HAL_DMA_Init() computes
         * ChannelIndex, OrgChannelIndex, DmaBaseAddress and IsGPDMA from
         * Instance. */
        hdma->Instance = AI_CAMERA_AUDIO_TX_DMA_INSTANCE;
        if (!s_audio_tx_channel_logged)
        {
            s_audio_tx_channel_logged = 1;
            rt_kprintf("dma: audio tx pinned to ch5\n");
        }
    }
    return __real_HAL_DMA_Init(hdma);
}
