/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_keys.h"

#include <button.h>
#include <errno.h>
#include <rtdevice.h>
#include <rthw.h>

#define CAMERA_KEYS_GROUP       0
#define CAMERA_KEYS_COUNT       3
#define CAMERA_KEYS_QUEUE_SIZE  16
#define CAMERA_KEYS_CONFIRM_KEY 2
/* The driver reports BUTTON_LONG_PRESSED only after
 * BUTTON_ADV_ACTION_CHECK_DELAY, which the board configures as 3000 ms. That
 * is far too long to open the microphone, so talk uses its own hold timer,
 * which also keeps the 3 s threshold untouched for BACK in the gallery. */
#define CAMERA_KEYS_TALK_HOLD_MS 250U

static camera_key_action_t s_actions[CAMERA_KEYS_QUEUE_SIZE];
static unsigned int s_head;
static unsigned int s_tail;
static unsigned int s_count;
static int32_t s_button_id = -1;
static int32_t s_pressed_key = -1;
static rt_bool_t s_confirm_handled;
static rt_bool_t s_enabled;
static rt_bool_t s_starting;
static rt_timer_t s_talk_timer;
static volatile rt_bool_t s_talk_enabled;
static rt_bool_t s_talk_active;

/* The queue and press state are accessed with interrupts disabled. */
static void camera_keys_reset(void)
{
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_pressed_key = -1;
    s_confirm_handled = RT_FALSE;
    s_talk_active = RT_FALSE;
}

static void camera_keys_enqueue(camera_key_action_t action)
{
    if (s_count == CAMERA_KEYS_QUEUE_SIZE)
        return;
    s_actions[s_tail] = action;
    s_tail = (s_tail + 1U) % CAMERA_KEYS_QUEUE_SIZE;
    ++s_count;
}

static void camera_keys_adc_handler(uint8_t group, int32_t key,
                                    button_action_t action)
{
    rt_base_t level;

    if (group != CAMERA_KEYS_GROUP || key < 0 || key >= CAMERA_KEYS_COUNT)
        return;
    level = rt_hw_interrupt_disable();
    if (!s_enabled)
    {
        rt_hw_interrupt_enable(level);
        return;
    }
    switch (action)
    {
    case BUTTON_PRESSED:
        if (s_pressed_key == key)
            break;
        s_pressed_key = key;
        s_confirm_handled = RT_FALSE;
        if (key == 0)
            camera_keys_enqueue(CAMERA_KEY_PREVIOUS);
        else if (key == 1)
            camera_keys_enqueue(CAMERA_KEY_NEXT);
        else if (s_talk_enabled && s_talk_timer != RT_NULL)
            rt_timer_start(s_talk_timer);
        break;
    case BUTTON_CLICKED:
    case BUTTON_LONG_PRESSED:
        if (key == CAMERA_KEYS_CONFIRM_KEY && s_pressed_key == key)
        {
            /* The hold already opened the microphone: this press is a talk, so
             * do not turn it into a confirm or a back as well. */
            if (s_talk_active)
                break;
            if (s_confirm_handled)
                break;
            s_confirm_handled = RT_TRUE;
            if (action == BUTTON_LONG_PRESSED && s_talk_enabled)
            {
                /* Fallback for a hold the talk timer missed. */
                if (s_talk_timer != RT_NULL)
                    rt_timer_stop(s_talk_timer);
                s_talk_active = RT_TRUE;
                camera_keys_enqueue(CAMERA_KEY_TALK_START);
                break;
            }
            camera_keys_enqueue(action == BUTTON_LONG_PRESSED ?
                                CAMERA_KEY_BACK : CAMERA_KEY_CONFIRM);
        }
        break;
    case BUTTON_RELEASED:
        if (s_pressed_key == key)
        {
            s_pressed_key = -1;
            s_confirm_handled = RT_FALSE;
            if (s_talk_timer != RT_NULL)
                rt_timer_stop(s_talk_timer);
            /* Release ends a talk even if the view disabled talk meanwhile. */
            if (s_talk_active)
            {
                s_talk_active = RT_FALSE;
                camera_keys_enqueue(CAMERA_KEY_TALK_STOP);
            }
        }
        break;
    default:
        break;
    }
    rt_hw_interrupt_enable(level);
}

/* Runs in the timer thread: a press held for the talk threshold becomes a talk
 * request, a shorter press is left to the driver's click handling. */
static void camera_keys_talk_timer(void *parameter)
{
    rt_base_t level;

    (void)parameter;
    level = rt_hw_interrupt_disable();
    if (s_enabled && s_talk_enabled && !s_talk_active &&
        s_pressed_key == CAMERA_KEYS_CONFIRM_KEY)
    {
        s_talk_active = RT_TRUE;
        camera_keys_enqueue(CAMERA_KEY_TALK_START);
    }
    rt_hw_interrupt_enable(level);
}

/* button_init requires a handler before the ADC group replaces it. */
static void camera_keys_gpio_handler(int32_t pin, button_action_t action)
{
    (void)pin;
    (void)action;
}

int camera_keys_start(void)
{
    adc_button_handler_t handlers[CAMERA_KEYS_COUNT] =
    {
        camera_keys_adc_handler, camera_keys_adc_handler,
        camera_keys_adc_handler,
    };
    button_cfg_t config = {0};
    rt_base_t level;
    int result;

    level = rt_hw_interrupt_disable();
    if (s_starting)
    {
        rt_hw_interrupt_enable(level);
        return -EBUSY;
    }
    if (s_enabled)
    {
        rt_hw_interrupt_enable(level);
        return 0;
    }
    s_starting = RT_TRUE;
    camera_keys_reset();
    rt_hw_interrupt_enable(level);

    if (ADC_BUTTON_GROUP_NUM <= CAMERA_KEYS_GROUP ||
        ADC_BUTTON_GROUP1_MAX_NUM < CAMERA_KEYS_COUNT)
    {
        result = -EINVAL;
        goto failed;
    }
    if (s_talk_timer == RT_NULL)
    {
        s_talk_timer = rt_timer_create("cam_talk", camera_keys_talk_timer,
                                       RT_NULL,
                                       rt_tick_from_millisecond(
                                           CAMERA_KEYS_TALK_HOLD_MS),
                                       RT_TIMER_FLAG_ONE_SHOT |
                                           RT_TIMER_FLAG_SOFT_TIMER);
        if (s_talk_timer == RT_NULL)
        {
            result = -ENOMEM;
            goto failed;
        }
    }
    if (rt_device_find(ADC_BUTTON_ADC_DEV_NAME) == RT_NULL)
    {
        result = -ENODEV;
        goto failed;
    }
    if (s_button_id < 0)
    {
        config.pin = BSP_KEY1_PIN;
#ifdef BSP_KEY1_ACTIVE_HIGH
        config.active_state = BUTTON_ACTIVE_HIGH;
#else
        config.active_state = BUTTON_ACTIVE_LOW;
#endif
        config.mode = PIN_MODE_INPUT;
        config.button_handler = camera_keys_gpio_handler;
        config.debounce_time = 20;
        config.name = "camera_keys";
        result = button_init(&config);
        if (result < 0)
            goto failed;
        s_button_id = result;
    }
    result = button_bind_adc_button(s_button_id, CAMERA_KEYS_GROUP,
                                   CAMERA_KEYS_COUNT, handlers);
    if (result != SF_EOK)
        goto failed;

    level = rt_hw_interrupt_disable();
    s_enabled = RT_TRUE;
    rt_hw_interrupt_enable(level);
    result = button_enable(s_button_id);
    if (result != SF_EOK)
        goto failed;

    level = rt_hw_interrupt_disable();
    s_starting = RT_FALSE;
    rt_hw_interrupt_enable(level);
    return 0;

failed:
    level = rt_hw_interrupt_disable();
    s_enabled = RT_FALSE;
    rt_hw_interrupt_enable(level);
    if (s_button_id >= 0)
        button_disable(s_button_id);
    level = rt_hw_interrupt_disable();
    camera_keys_reset();
    s_starting = RT_FALSE;
    rt_hw_interrupt_enable(level);
    return result;
}

void camera_keys_set_talk_enabled(int enabled)
{
    rt_base_t level = rt_hw_interrupt_disable();

    s_talk_enabled = enabled ? RT_TRUE : RT_FALSE;
    /* Only stops a hold from starting a talk; a running talk still ends on
     * release so the microphone cannot be left open. */
    if (!s_talk_enabled && s_talk_timer != RT_NULL)
        rt_timer_stop(s_talk_timer);
    rt_hw_interrupt_enable(level);
}

int camera_keys_poll(camera_key_action_t *action)
{
    rt_base_t level;

    if (action == RT_NULL)
        return -EINVAL;
    level = rt_hw_interrupt_disable();
    if (s_count == 0)
    {
        rt_hw_interrupt_enable(level);
        return 0;
    }
    *action = s_actions[s_head];
    s_head = (s_head + 1U) % CAMERA_KEYS_QUEUE_SIZE;
    --s_count;
    rt_hw_interrupt_enable(level);
    return 1;
}
