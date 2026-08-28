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

uint32_t ai_xiaozhi_audio_speaker_cache_bytes(uint16_t downlink_samples)
{
    uint32_t frame_bytes = (uint32_t)downlink_samples * 2U;

    return downlink_samples != 0U &&
                   frame_bytes <= AI_XIAOZHI_AUDIO_DOWNLINK_PCM_MAX_BYTES
               ? AI_XIAOZHI_AUDIO_SPEAKER_CACHE_BYTES
               : 0U;
}

uint16_t ai_xiaozhi_audio_pcm_peak(const int16_t *samples, uint16_t count)
{
    uint16_t index;
    uint32_t peak = 0U;

    if (samples == 0)
        return 0U;
    for (index = 0U; index < count; ++index)
    {
        int32_t sample = samples[index];
        uint32_t magnitude = sample < 0 ? (uint32_t)-sample :
                                           (uint32_t)sample;

        if (magnitude > peak)
            peak = magnitude;
    }
    return (uint16_t)peak;
}

int ai_xiaozhi_audio_should_log_frame(uint32_t frame_count)
{
    return frame_count == 1U ||
           frame_count % AI_XIAOZHI_AUDIO_DIAGNOSTIC_INTERVAL_FRAMES == 0U;
}

int ai_xiaozhi_audio_worker_can_listen(int configured, int worker_ready)
{
    return configured != 0 && worker_ready != 0;
}

const char *ai_xiaozhi_audio_resource_name(ai_xiaozhi_audio_resource_t resource)
{
    switch (resource)
    {
    case AI_XIAOZHI_AUDIO_RESOURCE_RINGBUFFER:
        return "uplink ringbuffer";
    case AI_XIAOZHI_AUDIO_RESOURCE_MESSAGE_QUEUE:
        return "downlink message queue";
    case AI_XIAOZHI_AUDIO_RESOURCE_EVENT:
        return "audio event";
    case AI_XIAOZHI_AUDIO_RESOURCE_THREAD:
        return "opus worker thread";
    case AI_XIAOZHI_AUDIO_RESOURCE_OPUS:
        return "opus codec";
    default:
        return "unknown";
    }
}
