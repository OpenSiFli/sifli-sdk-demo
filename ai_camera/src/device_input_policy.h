#ifndef AI_CAMERA_DEVICE_INPUT_POLICY_H
#define AI_CAMERA_DEVICE_INPUT_POLICY_H

#include <stdint.h>

#include "device_ui_state.h"

typedef enum
{
    DEVICE_INPUT_PRESSED = 0,
    DEVICE_INPUT_RELEASED,
    DEVICE_INPUT_LONG_PRESSED,
    DEVICE_INPUT_CLICKED,
} device_input_action_t;

device_ui_event_t device_input_event_for_adc(uint8_t group_index,
                                             int32_t button_index,
                                             device_input_action_t action);

#endif
