#ifndef AI_XIAOZHI_AUDIO_H
#define AI_XIAOZHI_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "xiaozhi_protocol.h"

#define AI_XIAOZHI_AUDIO_WORKER_STACK_SIZE (32U * 1024U)

typedef int (*ai_xiaozhi_audio_send_t)(void *context, const uint8_t *data,
                                       size_t size);

int ai_xiaozhi_audio_configure(const ai_xiaozhi_server_hello_t *hello);
int ai_xiaozhi_audio_listen_start(ai_xiaozhi_audio_send_t send,
                                  void *send_context);
void ai_xiaozhi_audio_listen_stop(void);
int ai_xiaozhi_audio_receive(const uint8_t *data, size_t size);
void ai_xiaozhi_audio_disconnect(void);

#endif
