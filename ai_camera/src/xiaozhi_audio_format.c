#include "xiaozhi_audio_format.h"

#include <limits.h>

int ai_xiaozhi_audio_frame_samples(uint32_t sample_rate,
                                   uint16_t frame_duration_ms,
                                   uint16_t *samples)
{
    uint32_t value;

    if (samples == 0 || sample_rate == 0U ||
        (frame_duration_ms != 5U && frame_duration_ms != 10U &&
         frame_duration_ms != 20U && frame_duration_ms != 40U &&
         frame_duration_ms != 60U) ||
        sample_rate > UINT32_MAX / frame_duration_ms)
        return -1;
    value = sample_rate * frame_duration_ms;
    if (value % 1000U != 0U || value / 1000U > UINT16_MAX)
        return -1;
    *samples = (uint16_t)(value / 1000U);
    return 0;
}

int ai_xiaozhi_audio_should_log_frame(uint32_t frame_count)
{
    return frame_count == 1U ||
           frame_count % AI_XIAOZHI_AUDIO_DIAGNOSTIC_INTERVAL_FRAMES == 0U;
}
