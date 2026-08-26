#ifndef AI_CAMERA_TEST_BUTTON_H
#define AI_CAMERA_TEST_BUTTON_H

#include <stdint.h>

typedef enum
{
    BUTTON_PRESSED = 0,
    BUTTON_RELEASED,
    BUTTON_LONG_PRESSED,
    BUTTON_CLICKED,
} button_action_t;

typedef enum
{
    BUTTON_ACTIVE_LOW = 0,
    BUTTON_ACTIVE_HIGH,
} button_active_state_t;

typedef void (*button_handler_t)(int32_t pin, button_action_t action);
typedef void (*adc_button_handler_t)(uint8_t group_index, int32_t button_index,
                                     button_action_t action);

typedef struct
{
    int32_t pin;
    button_active_state_t active_state;
    uint32_t mode;
    button_handler_t button_handler;
    uint16_t debounce_time;
    const char *name;
} button_cfg_t;

#define BSP_KEY1_PIN 34
#define PIN_MODE_INPUT 0U

int32_t button_init(button_cfg_t *config);
int32_t button_bind_adc_button(int32_t id, int8_t group_index,
                               uint8_t handler_count,
                               adc_button_handler_t *handlers);
int button_enable(int32_t id);

#endif
