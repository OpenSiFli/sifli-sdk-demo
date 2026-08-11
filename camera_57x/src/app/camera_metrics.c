#include "camera_metrics.h"

uint32_t camera_metrics_average_ms_x10(uint32_t ticks,
                                       uint32_t samples,
                                       uint32_t tick_rate)
{
    if ((samples == 0U) || (tick_rate == 0U))
    {
        return 0U;
    }

    return (uint32_t)(((uint64_t)ticks * 10000U) /
                      ((uint64_t)samples * tick_rate));
}
