#include "screen.h"
#include "math.h"
#include "bf0_hal.h"
#include "lvgl.h"
#include <stdio.h>
#include "button.h"
#include <rtthread.h>
#include "agif.h"
#include "lvsf_input.h"


#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 128
#define GIF_SPEED 100 // Unit: milliseconds, control the GIF playback speed
// GIF resources
LV_IMG_DECLARE(gif1);
LV_IMG_DECLARE(gif2);
LV_IMG_DECLARE(gif3);
LV_IMG_DECLARE(gif4);
LV_IMG_DECLARE(gif5);
LV_IMG_DECLARE(gif6);


typedef enum
{
    KEYPAD_KEY_HOME = 2,
} keypad_key_code_t;
typedef enum
{
    KEYPAD_KEY_STATE_REL,
    KEYPAD_KEY_STATE_PRESSED,
} keypad_key_state_t;

typedef struct
{
    keypad_key_code_t last_key;
    keypad_key_state_t last_key_state;
} keypad_status_t;


static int active_count = 6;
static int32_t key1_button_handle = -1;
static keypad_status_t keypad_status;

typedef struct {
    int16_t x;
    int16_t y;
    const lv_img_dsc_t *gif_src; // GIF Resources
    lv_obj_t *gif_obj;           // Standalone LVGL GIF controls
    lv_timer_t *refresh_timer;   // Independent refresh timer 
    bool active;                 // Activated (play animation)
} ScreenLayout;

// Define the layout of 6 screens, each with a GIF control
static ScreenLayout screens[6] = {
    {0, 0, &gif1, NULL, NULL, false},
    {SCREEN_WIDTH, 0, &gif2, NULL, NULL, false},
    {SCREEN_WIDTH*2, 0, &gif3, NULL, NULL, false},
    {0, SCREEN_HEIGHT, &gif4, NULL, NULL, false},
    {SCREEN_WIDTH, SCREEN_HEIGHT, &gif5, NULL, NULL, false},
    {SCREEN_WIDTH*2, SCREEN_HEIGHT, &gif6, NULL, NULL, false}
};

/**
 * GIF refresh timer callback function  
 */
static void gif_refresh_timer_cb(lv_timer_t *timer)
{
    ScreenLayout *screen = (ScreenLayout *)timer->user_data;
    if (screen && screen->gif_obj) {
        int ret = lv_gif_dec_next_frame(screen->gif_obj);
        if (ret == 0) {
            lv_gif_dec_restart(screen->gif_obj);
        }
    }
}

/**
 * Create a single GIF control (manage independently) 
 */
static lv_obj_t *create_gif_control(lv_obj_t *parent, const lv_img_dsc_t *src, int16_t x, int16_t y, int index)
{
    lv_color_t bg = lv_color_white();
    lv_obj_t *gif = lv_gif_dec_create(parent, src, &bg, LV_COLOR_DEPTH);
    if (gif == NULL) {
        rt_kprintf("Failed to create GIF control, resource or memory issue!\n");
        return NULL;
    }
    
    lv_obj_set_size(gif, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_pos(gif, x, y);
    lv_obj_set_style_border_width(gif, 0, 0);
    
    // Initialize to the paused state
    lv_gif_dec_task_pause(gif, 1);
    
    // Create an independent refresh timer
    screens[index].refresh_timer = lv_timer_create(gif_refresh_timer_cb, GIF_SPEED, &screens[index]);
    if (screens[index].refresh_timer) {
        lv_timer_pause(screens[index].refresh_timer);
    }
    return gif;
}

/**
 * Update the screen activation status (control the corresponding control to play/pause)
 */
static void update_screen_active(int count)
{
    for (int i = 0; i < 6; i++) {
        bool new_active = (i < count);
        
        if (new_active != screens[i].active) {
            screens[i].active = new_active;
            
            if (screens[i].refresh_timer) {
                if (new_active) {
                    // Activate screen: Start playing from the beginning
                    lv_gif_dec_restart(screens[i].gif_obj);
                    lv_timer_resume(screens[i].refresh_timer);;
                } else {
                    // Disable screen: Pause at the current frame
                    lv_timer_pause(screens[i].refresh_timer);
                }
            }
        }
    }
}

static void button_event_handler(int32_t pin, button_action_t action)
{
    lv_disp_trig_activity(NULL);

    switch (action)
    {
    case BUTTON_CLICKED:
    {
        keypad_status.last_key = KEYPAD_KEY_HOME;
        keypad_status.last_key_state = KEYPAD_KEY_STATE_PRESSED;
        break;
    }
    default:
        break;
    }
}

void button_key_read(uint32_t *last_key, lv_indev_state_t *state)
{
    if (last_key && state)
    {
        *last_key = keypad_status.last_key;
        if (KEYPAD_KEY_STATE_REL == keypad_status.last_key_state)
        {
            *state = LV_INDEV_STATE_REL;
        }
        else
        {
            *state = LV_INDEV_STATE_PR;
            keypad_status.last_key_state = KEYPAD_KEY_STATE_REL;
        }
    }
}

static int32_t default_keypad_handler(lv_key_t key, lv_indev_state_t event)
{
    static lv_indev_state_t last_event = LV_INDEV_STATE_REL;

    if (last_event != event) 
    {
        last_event = event;

        if ((LV_INDEV_STATE_PR == event) && (LV_KEY_HOME == key))
        {
            active_count = (active_count % 6) + 1;
            update_screen_active(active_count);
        }
    }
    return LV_BLOCK_EVENT;
}

void key_button_init(void)
{
    button_cfg_t cfg;

    cfg.pin = BSP_KEY1_PIN;
    cfg.mode = PIN_MODE_INPUT;
#ifdef BSP_KEY1_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
#else
    cfg.active_state = BUTTON_ACTIVE_LOW;
#endif
    cfg.button_handler = button_event_handler;
    
    int32_t id = button_init(&cfg);
    if (id >= 0) {
        RT_ASSERT(SF_EOK == button_enable(id));
        key1_button_handle = id;
    } else {
        rt_kprintf("Button initialization failed\n");
    }
}

void create_multi_screen_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        rt_kprintf("Failed to obtain the main screen of LVGL!\n");
        return;
    }
    // Register a keystroke function
    keypad_default_handler_register(default_keypad_handler);

    // Create 6 GIF controls
    for (int i = 0; i < 6; i++) {
        screens[i].gif_obj = create_gif_control(
            scr, 
            screens[i].gif_src, 
            screens[i].x, 
            screens[i].y,
            i
        );
    }
    // Initial activation of 6 screens
    update_screen_active(6);
}