#include <assert.h>

#include "device_input_policy.h"

int main(void)
{
    assert(device_input_event_for_adc(0U, 0, DEVICE_INPUT_PRESSED) ==
           DEVICE_UI_EVENT_KEY1_PRESSED);
    assert(device_input_event_for_adc(0U, 0, DEVICE_INPUT_RELEASED) ==
           DEVICE_UI_EVENT_KEY1_RELEASED);
    assert(device_input_event_for_adc(0U, 1, DEVICE_INPUT_CLICKED) ==
           DEVICE_UI_EVENT_KEY2_PRESSED);
    assert(device_input_event_for_adc(0U, 2, DEVICE_INPUT_CLICKED) ==
           DEVICE_UI_EVENT_KEY3_PRESSED);
    assert(device_input_event_for_adc(0U, 2, DEVICE_INPUT_LONG_PRESSED) ==
           DEVICE_UI_EVENT_KEY3_LONG_PRESSED);
    assert(device_input_event_for_adc(1U, 0, DEVICE_INPUT_PRESSED) ==
           DEVICE_UI_EVENT_NONE);
    assert(device_input_event_for_adc(0U, 1, DEVICE_INPUT_RELEASED) ==
           DEVICE_UI_EVENT_NONE);
    assert(device_input_event_for_adc(0U, 3, DEVICE_INPUT_CLICKED) ==
           DEVICE_UI_EVENT_NONE);
    return 0;
}
