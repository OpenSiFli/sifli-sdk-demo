#include "device_input.h"

#include <rtthread.h>

#include <button.h>
#include <sf_type.h>

#include "device_input_policy.h"
#include "device_ui.h"

#define DEVICE_INPUT_ADC_GROUP 0U

static void device_input_dummy_handler(int32_t pin, button_action_t action)
{
    (void)pin;
    (void)action;
}

static device_input_action_t device_input_action_from_button(
    button_action_t action)
{
    switch (action)
    {
    case BUTTON_PRESSED:
        return DEVICE_INPUT_PRESSED;
    case BUTTON_RELEASED:
        return DEVICE_INPUT_RELEASED;
    case BUTTON_LONG_PRESSED:
        return DEVICE_INPUT_LONG_PRESSED;
    case BUTTON_CLICKED:
    default:
        return DEVICE_INPUT_CLICKED;
    }
}

static void device_input_adc_handler(uint8_t group_index, int32_t button_index,
                                     button_action_t action)
{
    device_ui_event_t event = device_input_event_for_adc(
        group_index, button_index, device_input_action_from_button(action));

    if (event != DEVICE_UI_EVENT_NONE)
    {
        rt_kprintf("input: key %d action %d\n", (int)(button_index + 1),
                   (int)action);
        device_ui_post(event);
    }
}

int device_input_start(void)
{
    button_cfg_t config = {0};
    adc_button_handler_t handlers[ADC_BUTTON_GROUP1_MAX_NUM];
    int32_t button_id;
    uint8_t index;

    if (rt_device_find(ADC_BUTTON_ADC_DEV_NAME) == RT_NULL)
    {
        rt_kprintf("input: ADC device %s unavailable\n",
                   ADC_BUTTON_ADC_DEV_NAME);
        return -RT_ERROR;
    }
    config.pin = BSP_KEY1_PIN;
    config.active_state = BUTTON_ACTIVE_HIGH;
    config.mode = PIN_MODE_INPUT;
    config.button_handler = device_input_dummy_handler;
    button_id = button_init(&config);
    if (button_id < 0)
    {
        rt_kprintf("input: ADC button initialization failed %d\n", button_id);
        return -RT_ERROR;
    }
    for (index = 0U; index < ADC_BUTTON_GROUP1_MAX_NUM; ++index)
        handlers[index] = device_input_adc_handler;
    if (button_bind_adc_button(button_id, DEVICE_INPUT_ADC_GROUP,
                               ADC_BUTTON_GROUP1_MAX_NUM, handlers) != 0 ||
        button_enable(button_id) != SF_EOK)
    {
        rt_kprintf("input: ADC button binding failed\n");
        return -RT_ERROR;
    }
    rt_kprintf("input: ADC keys ready\n");
    return 0;
}
