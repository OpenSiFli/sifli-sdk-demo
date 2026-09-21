#ifndef AI_XIAOZHI_VISION_H
#define AI_XIAOZHI_VISION_H

#include <stddef.h>

#include "xiaozhi_protocol.h"

#define AI_XIAOZHI_VISION_RESPONSE_MAX 384U

int ai_xiaozhi_vision_set_capability(
    const ai_xiaozhi_vision_capability_t *capability);
/* The endpoint and its token belong to one session: drop them when that session
 * ends, so an upload can never reuse a stale token (the server hands out a new
 * capability at the start of the next session). */
void ai_xiaozhi_vision_clear_capability(void);
int ai_xiaozhi_vision_upload_file(const char *path, const char *question,
                                  char *response, size_t response_size);

/* The UI owns the photo the user is asking about: it fills path and returns 0,
 * or returns -1 when there is none. */
typedef int (*ai_xiaozhi_vision_photo_provider_t)(char *path, size_t size);
void ai_xiaozhi_vision_set_photo_provider(
    ai_xiaozhi_vision_photo_provider_t provider);

int ai_xiaozhi_vision_request_tool_call(const char *session_id, int request_id,
                                        const char *question);

#endif
