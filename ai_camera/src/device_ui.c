#include "device_ui.h"
#include "device_ui_font.h"

#include <rtthread.h>

#include <littlevgl2rtt.h>
#include <lv_ex_data.h>
#include <lvgl.h>

#include <stdint.h>

#define DEVICE_UI_QUEUE_DEPTH 16U
#define DEVICE_UI_THREAD_STACK_SIZE 4096U
#define DEVICE_UI_THREAD_PRIORITY 20U
#define DEVICE_UI_MAX_WAIT_MS 100U
#define DEVICE_UI_MARGIN 20
#define DEVICE_UI_STATUS_TOP 58
#define DEVICE_UI_STATUS_HEIGHT 68
#define DEVICE_UI_PHOTO_WIDTH 144
#define DEVICE_UI_PHOTO_HEIGHT 112
#define DEVICE_UI_PHOTO_RIGHT_MARGIN 41
#define DEVICE_UI_VOICE_LEFT 23
#define DEVICE_UI_VOICE_TOP 171
#define DEVICE_UI_VOICE_WIDTH 147
#define DEVICE_UI_VOICE_HEIGHT 60
#define DEVICE_UI_DETAIL_TOP 270
#define DEVICE_UI_TOOLBAR_HEIGHT 90
#define DEVICE_UI_TEXT_LINE_HEIGHT 20
#define DEVICE_UI_TEXT_TWO_LINES (DEVICE_UI_TEXT_LINE_HEIGHT * 2 + 2)

typedef struct
{
    device_ui_event_t event;
} device_ui_message_t;

static rt_mq_t s_queue;
static rt_thread_t s_thread;
static device_ui_action_handler_t s_action_handler;
static void *s_action_context;
static rt_sem_t s_pause_ack;
static rt_sem_t s_resume_sem;
static volatile rt_bool_t s_pause_requested;
static volatile rt_bool_t s_paused;

static void device_ui_add_label(lv_obj_t *parent, const char *text,
                                const lv_font_t *font, lv_align_t align,
                                lv_coord_t x, lv_coord_t y,
                                lv_coord_t width, lv_coord_t height,
                                lv_color_t color,
                                lv_text_align_t text_align)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, text_align, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, 2, LV_PART_MAIN);
    lv_obj_align(label, align, x, y);
}

static lv_obj_t *device_ui_add_panel(lv_obj_t *parent, lv_coord_t x,
                                     lv_coord_t y, lv_coord_t width,
                                     lv_coord_t height, lv_color_t color,
                                     lv_coord_t radius)
{
    lv_obj_t *panel = lv_obj_create(parent);

    lv_obj_set_size(panel, width, height);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_bg_color(panel, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void device_ui_content_for_state(device_ui_state_t state,
                                        const char **headline,
                                        const char **detail,
                                        lv_color_t *accent)
{
    *headline = "等待网络";
    *detail = "连接手机蓝牙网络后就可以聊天";
    *accent = lv_color_hex(0xe66f61);

    switch (state)
    {
    case DEVICE_UI_CONNECTING:
        *headline = "正在连接";
        *detail = "正在连接小智服务";
        *accent = lv_color_hex(0xf1bb55);
        break;
    case DEVICE_UI_HOME:
        *headline = "随时准备好";
        *detail = "拍下眼前的事物，我来认识它";
        *accent = lv_color_hex(0x16d4cf);
        break;
    case DEVICE_UI_LISTENING:
        *headline = "我在听";
        *detail = "松开后会发送你的声音";
        *accent = lv_color_hex(0x5ba9ee);
        break;
    case DEVICE_UI_THINKING:
        *headline = "正在思考";
        *detail = "正在整理刚才听到的话";
        *accent = lv_color_hex(0xa18aec);
        break;
    case DEVICE_UI_SPEAKING:
        *headline = "正在回复";
        *detail = "可以继续和我说话";
        *accent = lv_color_hex(0x16d4cf);
        break;
    case DEVICE_UI_CAMERA_CAPTURING:
        *headline = "正在拍照";
        *detail = "正在保存照片，请稍候";
        *accent = lv_color_hex(0xf1bb55);
        break;
    case DEVICE_UI_CAMERA_UNAVAILABLE:
        *headline = "相机不可用";
        *detail = "请检查 OV2640 连接";
        *accent = lv_color_hex(0xf1bb55);
        break;
    case DEVICE_UI_CONNECTION:
        *headline = "蓝牙网络";
        *detail = "请开启手机蓝牙网络共享";
        *accent = lv_color_hex(0x5ba9ee);
        break;
    case DEVICE_UI_ERROR:
        *headline = "网络连接失败";
        *detail = "检查蓝牙网络后再试";
        *accent = lv_color_hex(0xe66f61);
        break;
    case DEVICE_UI_STORAGE_ERROR:
        *headline = "照片存储失败";
        *detail = "存储分区不可用，请重试";
        *accent = lv_color_hex(0xe66f61);
        break;
    case DEVICE_UI_OFFLINE:
    default:
        break;
    }
}

static void device_ui_button_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    device_ui_event_t ui_event = (device_ui_event_t)(uintptr_t)
        lv_event_get_user_data(event);

    if (ui_event == DEVICE_UI_EVENT_KEY1_PRESSED &&
        code != LV_EVENT_PRESSED)
        return;
    if (ui_event == DEVICE_UI_EVENT_KEY1_RELEASED &&
        code != LV_EVENT_RELEASED)
        return;
    if (ui_event != DEVICE_UI_EVENT_KEY1_PRESSED &&
        ui_event != DEVICE_UI_EVENT_KEY1_RELEASED &&
        code != LV_EVENT_CLICKED)
        return;
    device_ui_post(ui_event);
}

static lv_obj_t *device_ui_add_button(lv_obj_t *parent, const char *text,
                                      lv_coord_t x, lv_coord_t y,
                                      lv_coord_t width, lv_coord_t height,
                                      lv_color_t color,
                                      device_ui_event_t event,
                                      lv_event_code_t event_code)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_t *label = lv_label_create(button);

    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, height / 2, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, device_ui_button_event, event_code,
                        (void *)(uintptr_t)event);
    return button;
}

static lv_obj_t *device_ui_add_bubble(lv_obj_t *parent, const char *text,
                                      lv_coord_t x, lv_coord_t y,
                                      lv_coord_t width, lv_coord_t height,
                                      lv_color_t color,
                                      lv_text_align_t text_align)
{
    lv_obj_t *bubble = device_ui_add_panel(parent, x, y, width, height,
                                            color, 12);

    if (text_align == LV_TEXT_ALIGN_CENTER)
    {
        device_ui_add_label(bubble, text, &device_ui_font,
                            LV_ALIGN_CENTER, 0, 0, width - 24,
                            DEVICE_UI_TEXT_LINE_HEIGHT,
                            lv_color_hex(0xf4fbff), text_align);
    }
    else
    {
        device_ui_add_label(bubble, text, &device_ui_font,
                            LV_ALIGN_TOP_LEFT, 14, 14, width - 28,
                            height - 28, lv_color_hex(0xf4fbff), text_align);
    }
    return bubble;
}

static void device_ui_render(device_ui_state_t state)
{
    const char *headline;
    const char *detail;
    lv_color_t accent;
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *status;
    lv_obj_t *photo_card;
    lv_obj_t *voice_bubble;
    lv_obj_t *toolbar;
    lv_obj_t *talk_button;
    lv_coord_t screen_width;
    lv_coord_t screen_height;
    lv_coord_t content_width;
    lv_coord_t photo_x;
    lv_coord_t toolbar_y;
    lv_coord_t detail_height;

    device_ui_content_for_state(state, &headline, &detail, &accent);
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x09152a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    screen_width = lv_obj_get_width(screen);
    screen_height = lv_obj_get_height(screen);
    content_width = screen_width - DEVICE_UI_MARGIN * 2;
    photo_x = screen_width - DEVICE_UI_PHOTO_RIGHT_MARGIN -
              DEVICE_UI_PHOTO_WIDTH;
    toolbar_y = screen_height - DEVICE_UI_TOOLBAR_HEIGHT;
    detail_height = toolbar_y - DEVICE_UI_DETAIL_TOP - 12;
    device_ui_add_button(screen, LV_SYMBOL_HOME, 18, 12, 34, 34,
                         lv_color_hex(0xe4d653), DEVICE_UI_EVENT_KEY3_PRESSED,
                         LV_EVENT_CLICKED);
    device_ui_add_label(screen, "今天的对话", &device_ui_font,
                        LV_ALIGN_TOP_MID, 0, 16, 180,
                        DEVICE_UI_TEXT_LINE_HEIGHT,
                        lv_color_hex(0xf4fbff), LV_TEXT_ALIGN_CENTER);
    device_ui_add_label(screen, "AI CAMERA", &lv_font_montserrat_16,
                        LV_ALIGN_TOP_RIGHT, -19, 20, 120,
                        DEVICE_UI_TEXT_LINE_HEIGHT,
                        lv_color_hex(0x81a9dd), LV_TEXT_ALIGN_RIGHT);

    status = device_ui_add_panel(screen, DEVICE_UI_MARGIN,
                                 DEVICE_UI_STATUS_TOP, content_width,
                                 DEVICE_UI_STATUS_HEIGHT,
                                 lv_color_hex(0x234a77), 10);
    lv_obj_set_style_bg_opa(status, LV_OPA_90, LV_PART_MAIN);
    device_ui_add_label(status, headline, &device_ui_font,
                        LV_ALIGN_TOP_LEFT, 12, 10, 112,
                        DEVICE_UI_TEXT_LINE_HEIGHT, accent,
                        LV_TEXT_ALIGN_LEFT);
    device_ui_add_label(status, detail, &device_ui_font,
                        LV_ALIGN_TOP_LEFT, 128, 9, content_width - 142,
                        DEVICE_UI_TEXT_TWO_LINES,
                        lv_color_hex(0xe8f4ff), LV_TEXT_ALIGN_LEFT);

    photo_card = device_ui_add_panel(screen, photo_x, 140,
                                     DEVICE_UI_PHOTO_WIDTH,
                                     DEVICE_UI_PHOTO_HEIGHT,
                                     lv_color_hex(0x1b466d), 12);
    lv_obj_set_style_border_width(photo_card, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(photo_card, accent, LV_PART_MAIN);
    device_ui_add_label(photo_card, LV_SYMBOL_IMAGE, &lv_font_montserrat_28,
                        LV_ALIGN_TOP_MID, 0, 16, 110, 28,
                        lv_color_hex(0xd8ffff), LV_TEXT_ALIGN_CENTER);
    device_ui_add_label(photo_card, "PHOTO", &lv_font_montserrat_16,
                        LV_ALIGN_BOTTOM_MID, 0, -16, 110,
                        DEVICE_UI_TEXT_LINE_HEIGHT,
                        lv_color_hex(0xd8ffff), LV_TEXT_ALIGN_CENTER);

    voice_bubble = device_ui_add_bubble(screen, LV_SYMBOL_AUDIO " 你好，小智",
                                        DEVICE_UI_VOICE_LEFT,
                                        DEVICE_UI_VOICE_TOP,
                                        DEVICE_UI_VOICE_WIDTH,
                                        DEVICE_UI_VOICE_HEIGHT,
                                        lv_color_hex(0x4d9cd5),
                                        LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(voice_bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(voice_bubble, device_ui_button_event, LV_EVENT_PRESSED,
                        (void *)(uintptr_t)DEVICE_UI_EVENT_KEY1_PRESSED);
    lv_obj_add_event_cb(voice_bubble, device_ui_button_event, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)DEVICE_UI_EVENT_KEY1_RELEASED);
    device_ui_add_bubble(screen, detail, DEVICE_UI_MARGIN,
                         DEVICE_UI_DETAIL_TOP, content_width, detail_height,
                         lv_color_hex(0x245a91), LV_TEXT_ALIGN_LEFT);

    toolbar = device_ui_add_panel(screen, 0, toolbar_y, screen_width,
                                  DEVICE_UI_TOOLBAR_HEIGHT,
                                  lv_color_hex(0x102a4c), 0);
    lv_obj_set_style_border_width(toolbar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(toolbar, lv_color_hex(0x315a85),
                                  LV_PART_MAIN);
    talk_button = device_ui_add_button(toolbar, LV_SYMBOL_AUDIO, 19, 13, 58,
                                       58, lv_color_hex(0xe4d653),
                                       DEVICE_UI_EVENT_KEY1_PRESSED,
                                       LV_EVENT_PRESSED);
    lv_obj_add_event_cb(talk_button, device_ui_button_event, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)DEVICE_UI_EVENT_KEY1_RELEASED);
    device_ui_add_button(toolbar, LV_SYMBOL_IMAGE,
                         (screen_width - 174) / 2, 13, 174, 58,
                         lv_color_hex(0x16d4cf), DEVICE_UI_EVENT_KEY2_PRESSED,
                         LV_EVENT_CLICKED);
    device_ui_add_button(toolbar, LV_SYMBOL_PLUS, screen_width - 77, 13,
                         58, 58,
                         lv_color_hex(0x6d91e9),
                         DEVICE_UI_EVENT_KEY3_LONG_PRESSED, LV_EVENT_CLICKED);
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
    lv_ex_data_pool_init();
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
        if (s_pause_requested)
        {
            s_paused = RT_TRUE;
            rt_sem_release(s_pause_ack);
            rt_sem_take(s_resume_sem, RT_WAITING_FOREVER);
            s_paused = RT_FALSE;
            s_pause_requested = RT_FALSE;
            device_ui_render(state);
        }
    }
}

int device_ui_start(device_ui_action_handler_t action_handler, void *context)
{
    if (s_queue != RT_NULL)
        return 0;
    s_queue = rt_mq_create("ai_ui", sizeof(device_ui_message_t),
                           DEVICE_UI_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    s_pause_ack = rt_sem_create("ui_pause", 0, RT_IPC_FLAG_FIFO);
    s_resume_sem = rt_sem_create("ui_resume", 0, RT_IPC_FLAG_FIFO);
    if (s_queue == RT_NULL || s_pause_ack == RT_NULL ||
        s_resume_sem == RT_NULL)
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

int device_ui_pause(void)
{
    device_ui_message_t message = {.event = DEVICE_UI_EVENT_NONE};

    if (s_queue == RT_NULL || s_pause_ack == RT_NULL)
        return -RT_ERROR;
    if (s_paused)
        return 0;
    s_pause_requested = RT_TRUE;
    if (rt_mq_send(s_queue, &message, sizeof(message)) != RT_EOK ||
        rt_sem_take(s_pause_ack, rt_tick_from_millisecond(1000U)) != RT_EOK)
    {
        s_pause_requested = RT_FALSE;
        return -RT_ETIMEOUT;
    }
    return 0;
}

void device_ui_resume(void)
{
    if (s_paused && s_resume_sem != RT_NULL)
        rt_sem_release(s_resume_sem);
}
