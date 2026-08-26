#ifndef AI_XIAOZHI_ACTIVATION_H
#define AI_XIAOZHI_ACTIVATION_H

#include <stddef.h>

#include "xiaozhi_protocol.h"

int ai_xiaozhi_get_identity(char *device_id, size_t device_id_size,
                            char *client_id, size_t client_id_size);
int ai_xiaozhi_fetch_activation(ai_xiaozhi_activation_t *activation);

#endif
