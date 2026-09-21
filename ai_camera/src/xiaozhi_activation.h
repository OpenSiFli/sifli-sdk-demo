#ifndef AI_XIAOZHI_ACTIVATION_H
#define AI_XIAOZHI_ACTIVATION_H

#include <stddef.h>

#include "xiaozhi_protocol.h"

int ai_xiaozhi_get_identity(char *device_id, size_t device_id_size,
                            char *client_id, size_t client_id_size);
int ai_xiaozhi_fetch_activation(ai_xiaozhi_activation_t *activation);
/* Code the user types in the app while this device is not bound yet. It is kept
 * from the last activation answer so the UI can show it on screen instead of
 * leaving it to the console log. Returns 1 while a code is known, else 0. */
int ai_xiaozhi_activation_bind_code(char *buffer, size_t size);

#endif
