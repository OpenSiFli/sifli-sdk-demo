/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AI_CAMERA_KEYS_H
#define AI_CAMERA_KEYS_H

typedef enum
{
    CAMERA_KEY_PREVIOUS,
    CAMERA_KEY_NEXT,
    CAMERA_KEY_CONFIRM,
    CAMERA_KEY_BACK,
} camera_key_action_t;

/* Enable the board's three ADC keys. Returns 0 on success, or a negative error. */
int camera_keys_start(void);
/* Nonblocking: returns 1 for an action, 0 for an empty queue, or -EINVAL. */
int camera_keys_poll(camera_key_action_t *action);

#endif
