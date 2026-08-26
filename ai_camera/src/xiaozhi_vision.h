#ifndef AI_XIAOZHI_VISION_H
#define AI_XIAOZHI_VISION_H

#include <stddef.h>

#include "xiaozhi_protocol.h"

#define AI_XIAOZHI_VISION_RESPONSE_MAX 384U

int ai_xiaozhi_vision_set_capability(
    const ai_xiaozhi_vision_capability_t *capability);
int ai_xiaozhi_vision_upload_file(const char *path, const char *question,
                                  char *response, size_t response_size);

#endif
