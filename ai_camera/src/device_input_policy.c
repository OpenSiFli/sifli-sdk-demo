#include "device_input_policy.h"

device_ui_event_t device_input_event_for_adc(uint8_t group_index,
                                             int32_t button_index,
                                             device_input_action_t action)
{
    if (group_index != 0U)
        return DEVICE_UI_EVENT_NONE;

    if (button_index == 0)
    {
        if (action == DEVICE_INPUT_PRESSED)
            return DEVICE_UI_EVENT_KEY1_PRESSED;
        if (action == DEVICE_INPUT_RELEASED)
            return DEVICE_UI_EVENT_KEY1_RELEASED;
    }
    else if (button_index == 1 && action == DEVICE_INPUT_CLICKED)
        return DEVICE_UI_EVENT_KEY2_PRESSED;
    else if (button_index == 2)
    {
        if (action == DEVICE_INPUT_CLICKED)
            return DEVICE_UI_EVENT_KEY3_PRESSED;
        if (action == DEVICE_INPUT_LONG_PRESSED)
            return DEVICE_UI_EVENT_KEY3_LONG_PRESSED;
    }
    return DEVICE_UI_EVENT_NONE;
}
