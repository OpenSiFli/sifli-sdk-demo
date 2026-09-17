#ifndef AI_CAMERA_DEVICE_UI_H
#define AI_CAMERA_DEVICE_UI_H

#include "device_ui_state.h"

typedef void (*device_ui_action_handler_t)(device_ui_action_t action,
                                           void *context);

int device_ui_start(device_ui_action_handler_t action_handler, void *context);
void device_ui_post(device_ui_event_t event);
int device_ui_pause(void);
void device_ui_resume(void);

#endif
