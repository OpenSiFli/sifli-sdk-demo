#include "device_ui.h"

#include <rtthread.h>

#include <littlevgl2rtt.h>
#include <lvgl.h>

#define DEVICE_UI_QUEUE_DEPTH 16U
#define DEVICE_UI_THREAD_STACK_SIZE 4096U
#define DEVICE_UI_THREAD_PRIORITY 20U
#define DEVICE_UI_MAX_WAIT_MS 100U

typedef struct
{
    device_ui_event_t event;
} device_ui_message_t;

static rt_mq_t s_queue;
static rt_thread_t s_thread;
static device_ui_action_handler_t s_action_handler;
static void *s_action_context;

static void device_ui_add_label(lv_obj_t *parent, const char *text,
                                const lv_font_t *font, lv_align_t align,
                                lv_coord_t x, lv_coord_t y,
                                lv_coord_t width, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(label, align, x, y);
}

static void device_ui_content_for_state(device_ui_state_t state,
                                        const char **headline,
                                        const char **detail,
                                        lv_color_t *accent)
{
    *headline = "OFFLINE";
    *detail = "Connect phone PAN\nto use XiaoZhi";
    *accent = lv_color_hex(0xe94f37);

    switch (state)
    {
    case DEVICE_UI_CONNECTING:
        *headline = "CONNECTING";
        *detail = "Connecting XiaoZhi";
        *accent = lv_color_hex(0xf5a524);
        break;
    case DEVICE_UI_HOME:
        *headline = "READY";
        *detail = "Hold KEY1 to talk";
        *accent = lv_color_hex(0x2bb673);
        break;
    case DEVICE_UI_LISTENING:
        *headline = "LISTENING";
        *detail = "Release KEY1\nwhen finished";
        *accent = lv_color_hex(0x3296dc);
        break;
    case DEVICE_UI_THINKING:
        *headline = "THINKING";
        *detail = "XiaoZhi is working";
        *accent = lv_color_hex(0x8e67d8);
        break;
    case DEVICE_UI_SPEAKING:
        *headline = "SPEAKING";
        *detail = "XiaoZhi reply";
        *accent = lv_color_hex(0x2bb673);
        break;
    case DEVICE_UI_CAMERA_UNAVAILABLE:
        *headline = "CAMERA";
        *detail = "OV2640 DVP unavailable\nCapture path reserved";
        *accent = lv_color_hex(0xf5a524);
        break;
    case DEVICE_UI_CONNECTION:
        *headline = "PAN";
        *detail = "Bluetooth connection\nKEY3: return home";
        *accent = lv_color_hex(0x3296dc);
        break;
    case DEVICE_UI_ERROR:
        *headline = "NETWORK ERROR";
        *detail = "Reconnect PAN\nand try again";
        *accent = lv_color_hex(0xe94f37);
        break;
    case DEVICE_UI_OFFLINE:
    default:
        break;
    }
}

static void device_ui_render(device_ui_state_t state)
{
    const char *headline;
    const char *detail;
    lv_color_t accent;
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *status;

    device_ui_content_for_state(state, &headline, &detail, &accent);
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x13202b), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    device_ui_add_label(screen, "AI CAMERA", &lv_font_montserrat_28,
                        LV_ALIGN_TOP_MID, 0, 30, 350,
                        lv_color_hex(0xf5f7fa));
    status = lv_obj_create(screen);
    lv_obj_set_size(status, 300, 110);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(status, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(status, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(status, 6, LV_PART_MAIN);
    device_ui_add_label(status, headline, &lv_font_montserrat_20,
                        LV_ALIGN_TOP_MID, 0, 16, 270,
                        lv_color_hex(0xffffff));
    device_ui_add_label(status, detail, LV_FONT_DEFAULT,
                        LV_ALIGN_BOTTOM_MID, 0, -14, 270,
                        lv_color_hex(0xffffff));
    device_ui_add_label(screen, "KEY1 TALK    KEY2 CAMERA    KEY3 HOME",
                        LV_FONT_DEFAULT, LV_ALIGN_BOTTOM_MID, 0, -32, 350,
                        lv_color_hex(0xb9c5d1));
}

static void device_ui_thread(void *parameter)
{
    device_ui_state_t state = DEVICE_UI_OFFLINE;
    rt_err_t result;

    (void)parameter;
    result = littlevgl2rtt_init("lcd");
    if (result != RT_EOK)
    {
        rt_kprintf("ui: LVGL initialization failed %d\n", result);
        return;
    }
    device_ui_render(state);
    rt_kprintf("ui: CO5300 ready\n");

    for (;;)
    {
        device_ui_message_t message;
        rt_uint32_t wait_ms = lv_task_handler();

        if (wait_ms > DEVICE_UI_MAX_WAIT_MS)
            wait_ms = DEVICE_UI_MAX_WAIT_MS;
        if (rt_mq_recv(s_queue, &message, sizeof(message),
                       rt_tick_from_millisecond(wait_ms)) == RT_EOK)
        {
            device_ui_transition_t transition =
                device_ui_reduce(state, message.event);

            if (transition.state != state)
            {
                state = transition.state;
                device_ui_render(state);
            }
            if (transition.action != DEVICE_UI_ACTION_NONE &&
                s_action_handler != RT_NULL)
                s_action_handler(transition.action, s_action_context);
        }
    }
}

int device_ui_start(device_ui_action_handler_t action_handler, void *context)
{
    if (s_queue != RT_NULL)
        return 0;
    s_queue = rt_mq_create("ai_ui", sizeof(device_ui_message_t),
                           DEVICE_UI_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    if (s_queue == RT_NULL)
        return -RT_ENOMEM;
    s_action_handler = action_handler;
    s_action_context = context;
    s_thread = rt_thread_create("ai_ui", device_ui_thread, RT_NULL,
                                DEVICE_UI_THREAD_STACK_SIZE,
                                DEVICE_UI_THREAD_PRIORITY,
                                RT_THREAD_TICK_DEFAULT);
    if (s_thread == RT_NULL)
        return -RT_ENOMEM;
    rt_thread_startup(s_thread);
    return 0;
}

void device_ui_post(device_ui_event_t event)
{
    device_ui_message_t message = {.event = event};

    if (s_queue != RT_NULL && event != DEVICE_UI_EVENT_NONE)
        (void)rt_mq_send(s_queue, &message, sizeof(message));
}
