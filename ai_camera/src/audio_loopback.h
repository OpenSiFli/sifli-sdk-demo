#ifndef AI_CAMERA_AUDIO_LOOPBACK_H
#define AI_CAMERA_AUDIO_LOOPBACK_H

#include <stdint.h>

typedef int (*ai_audio_write_fn)(void *context, const uint8_t *data,
                                 uint32_t size);

int ai_audio_loopback_forward(ai_audio_write_fn writer, void *context,
                              const uint8_t *data, uint32_t size);

#endif
