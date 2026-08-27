#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <button.h>
#include <rtthread.h>
#include <sf_type.h>

#include "device_input.h"
#include "device_ui_state.h"

static button_cfg_t s_config;
static int s_enable_id;
static int s_bind_id;
static int s_bind_group;
static int s_bind_handler_count;
static int s_button_init_count;
static int s_button_bind_count;
static int s_button_enable_count;
static int s_adc_available;

int rt_kprintf(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    va_end(arguments);
    return 0;
}

int32_t button_init(button_cfg_t *config)
{
    s_config = *config;
    s_button_init_count++;
    return 0;
}

int32_t button_bind_adc_button(int32_t id, int8_t group_index,
                               uint8_t handler_count,
                               adc_button_handler_t *handlers)
{
    s_button_bind_count++;
    s_bind_id = id;
    s_bind_group = group_index;
    s_bind_handler_count = handler_count;
    assert(handlers[0] != NULL);
    assert(handlers[1] != NULL);
    assert(handlers[2] != NULL);
    return 0;
}

int button_enable(int32_t id)
{
    s_button_enable_count++;
    s_enable_id = id;
    return SF_EOK;
}

rt_device_t rt_device_find(const char *name)
{
    if (s_adc_available && strcmp(name, ADC_BUTTON_ADC_DEV_NAME) == 0)
        return (rt_device_t)&s_adc_available;
    return RT_NULL;
}

void device_ui_post(device_ui_event_t event)
{
    (void)event;
}

int main(void)
{
    s_adc_available = 1;
    assert(device_input_start() == 0);
    assert(s_config.pin == BSP_KEY1_PIN);
    assert(s_config.active_state == BUTTON_ACTIVE_HIGH);
    assert(s_config.mode == PIN_MODE_INPUT);
    assert(s_config.button_handler != NULL);
    assert(s_config.debounce_time == 0U);
    assert(s_config.name == NULL);
    assert(s_bind_id == 0);
    assert(s_bind_group == 0);
    assert(s_bind_handler_count == 3);
    assert(s_enable_id == 0);

    s_adc_available = 0;
    s_button_init_count = 0;
    s_button_bind_count = 0;
    s_button_enable_count = 0;
    assert(device_input_start() == -RT_ERROR);
    assert(s_button_init_count == 0);
    assert(s_button_bind_count == 0);
    assert(s_button_enable_count == 0);
    return 0;
}
