#ifndef AI_XIAOZHI_AUDIO_FORMAT_H
#define AI_XIAOZHI_AUDIO_FORMAT_H

#include <stdint.h>

#define AI_XIAOZHI_AUDIO_DIAGNOSTIC_INTERVAL_FRAMES 25U
#define AI_XIAOZHI_AUDIO_DOWNLINK_PCM_MAX_BYTES 2880U
#define AI_XIAOZHI_AUDIO_SPEAKER_CACHE_BYTES 4096U

typedef enum
{
    AI_XIAOZHI_AUDIO_RESOURCE_RINGBUFFER = 0,
    AI_XIAOZHI_AUDIO_RESOURCE_MESSAGE_QUEUE,
    AI_XIAOZHI_AUDIO_RESOURCE_EVENT,
    AI_XIAOZHI_AUDIO_RESOURCE_THREAD,
    AI_XIAOZHI_AUDIO_RESOURCE_OPUS,
} ai_xiaozhi_audio_resource_t;

int ai_xiaozhi_audio_frame_samples(uint32_t sample_rate,
                                   uint16_t frame_duration_ms,
                                   uint16_t *samples);
uint32_t ai_xiaozhi_audio_speaker_cache_bytes(uint16_t downlink_samples);
uint16_t ai_xiaozhi_audio_pcm_peak(const int16_t *samples, uint16_t count);
int ai_xiaozhi_audio_should_log_frame(uint32_t frame_count);
int ai_xiaozhi_audio_worker_can_listen(int configured, int worker_ready);
const char *ai_xiaozhi_audio_resource_name(ai_xiaozhi_audio_resource_t resource);

#endif
