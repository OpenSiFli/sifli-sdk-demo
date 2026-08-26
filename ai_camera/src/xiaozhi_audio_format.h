#ifndef AI_XIAOZHI_AUDIO_FORMAT_H
#define AI_XIAOZHI_AUDIO_FORMAT_H

#include <stdint.h>

#define AI_XIAOZHI_AUDIO_DIAGNOSTIC_INTERVAL_FRAMES 25U

int ai_xiaozhi_audio_frame_samples(uint32_t sample_rate,
                                   uint16_t frame_duration_ms,
                                   uint16_t *samples);
int ai_xiaozhi_audio_should_log_frame(uint32_t frame_count);

#endif
