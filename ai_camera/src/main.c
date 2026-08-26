#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/ip_addr.h>
#include <netdev.h>

#include "audio_loopback.h"
#include "audio_server.h"
#include "app_state.h"
#include "bf0_sibles.h"
#include "bt_connection_manager.h"
#include "bts2_app_inc.h"

#define APP_QUEUE_DEPTH 16U
#define APP_IP_POLL_MS 200U

typedef enum
{
    APP_MESSAGE_STACK_READY = 0,
    APP_MESSAGE_BOND_READY,
    APP_MESSAGE_CONNECT_PAN,
    APP_MESSAGE_PAN_UP,
    APP_MESSAGE_PAN_DOWN,
    APP_MESSAGE_IP_POLL,
    APP_MESSAGE_RETRY_PAN,
} app_message_type_t;

typedef struct
{
    app_message_type_t type;
} app_message_t;

static rt_mq_t s_queue;
static rt_timer_t s_connect_timer;
static rt_timer_t s_ip_timer;
static rt_timer_t s_retry_timer;
static bt_notify_device_mac_t s_peer;
static rt_bool_t s_peer_valid;
static unsigned int s_retry_count;
static unsigned int s_ip_poll_count;
static app_state_t s_state = APP_BOOT;

static int app_audio_write(void *context, const uint8_t *data,
                           uint32_t size)
{
    return audio_write((audio_client_t)context, (uint8_t *)data, size);
}

static int app_audio_loopback_callback(audio_server_callback_cmt_t command,
                                       void *context, uint32_t reserved)
{
    audio_client_t *client = context;

    if (command == as_callback_cmd_data_coming && client != RT_NULL &&
        *client != RT_NULL)
    {
        audio_server_coming_data_t *incoming =
            (audio_server_coming_data_t *)reserved;

        return ai_audio_loopback_forward(app_audio_write, *client,
                                         incoming->data, incoming->data_len);
    }
    return 0;
}

static void ai_audio_loopback(int argc, char **argv)
{
    audio_parameter_t parameter = {0};
    audio_client_t client = RT_NULL;
    unsigned long seconds = 10U;

    if (argc == 2)
        seconds = strtoul(argv[1], RT_NULL, 10);
    if (argc > 2 || seconds == 0U || seconds > 60U)
    {
        rt_kprintf("usage: ai_audio_loopback [1-60 seconds]\n");
        return;
    }

    parameter.write_bits_per_sample = 16U;
    parameter.write_channnel_num = 1U;
    parameter.write_samplerate = 16000U;
    parameter.write_cache_size = 2048U;
    parameter.read_bits_per_sample = 16U;
    parameter.read_channnel_num = 1U;
    parameter.read_samplerate = 16000U;
    parameter.read_cache_size = 0U;
    parameter.read_which_mic = AUDIO_MIC0_ONLY;

    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, 5U);
    client = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TXRX, &parameter,
                        app_audio_loopback_callback, &client);
    if (client == RT_NULL)
    {
        rt_kprintf("audio: loopback open failed\n");
        return;
    }

    rt_kprintf("audio: loopback %lu s, mic0 -> speaker\n", seconds);
    rt_thread_mdelay((rt_int32_t)(seconds * 1000U));
    audio_close(client);
    rt_kprintf("audio: loopback stopped\n");
}
MSH_CMD_EXPORT(ai_audio_loopback, analog mic to speaker loopback);

static void app_post(app_message_type_t type)
{
    app_message_t message = {.type = type};

    if (s_queue != RT_NULL)
        rt_mq_send(s_queue, &message, sizeof(message));
}

static void app_connect_timer(void *parameter)
{
    (void)parameter;
    app_post(APP_MESSAGE_CONNECT_PAN);
}

static void app_ip_timer(void *parameter)
{
    (void)parameter;
    app_post(APP_MESSAGE_IP_POLL);
}

static void app_retry_timer(void *parameter)
{
    (void)parameter;
    app_post(APP_MESSAGE_RETRY_PAN);
}

static int app_ip_ready(void)
{
    struct netdev *netdev = netdev_get_by_name("b0");

    return netdev != RT_NULL && netdev_is_up(netdev) &&
           netdev_is_link_up(netdev) && !ip_addr_isany(&netdev->ip_addr);
}

static void app_start_retry(void)
{
    unsigned int next_retry;

    if (!s_peer_valid)
        return;
    next_retry = app_retry_next(s_retry_count, LEARNING_CAMERA_PAN_RETRY_MAX);
    if (next_retry == 0U)
    {
        s_state = app_state_next(s_state, APP_EVENT_PAN_RETRY_EXHAUSTED);
        rt_kprintf("learning: PAN retry exhausted\n");
        return;
    }
    s_retry_count = next_retry;
    rt_timer_start(s_retry_timer);
    rt_kprintf("learning: PAN retry %u/%u\n", s_retry_count,
               LEARNING_CAMERA_PAN_RETRY_MAX);
}

static void app_handle_message(const app_message_t *message)
{
    switch (message->type)
    {
    case APP_MESSAGE_STACK_READY:
        bt_interface_set_local_name(strlen(LEARNING_CAMERA_BT_NAME),
                                    (void *)LEARNING_CAMERA_BT_NAME);
        rt_kprintf("learning: Bluetooth stack ready\n");
        break;
    case APP_MESSAGE_BOND_READY:
        s_retry_count = 0U;
        rt_timer_start(s_connect_timer);
        rt_kprintf("learning: pairing complete, PAN connect scheduled\n");
        break;
    case APP_MESSAGE_CONNECT_PAN:
    case APP_MESSAGE_RETRY_PAN:
        if (s_peer_valid)
            bt_interface_conn_ext(s_peer.addr, BT_PROFILE_PAN);
        break;
    case APP_MESSAGE_PAN_UP:
        s_ip_poll_count = 0U;
        rt_timer_start(s_ip_timer);
        rt_kprintf("learning: PAN profile connected, waiting for DHCP\n");
        break;
    case APP_MESSAGE_IP_POLL:
        if (app_ip_ready())
        {
            rt_timer_stop(s_ip_timer);
            s_retry_count = 0U;
            s_state = app_state_next(s_state, APP_EVENT_PAN_READY);
            rt_kprintf("learning: PAN ready, b0 has IPv4\n");
        }
        else if (++s_ip_poll_count >=
                 (15000U / APP_IP_POLL_MS))
        {
            rt_timer_stop(s_ip_timer);
            app_post(APP_MESSAGE_PAN_DOWN);
        }
        break;
    case APP_MESSAGE_PAN_DOWN:
        rt_timer_stop(s_connect_timer);
        rt_timer_stop(s_ip_timer);
        s_state = app_state_next(s_state, APP_EVENT_PAN_DOWN);
        app_start_retry();
        break;
    default:
        break;
    }
}

static int app_bt_notify(uint16_t type, uint16_t event_id,
                         uint8_t *data, uint16_t data_len)
{
    if (type == BT_NOTIFY_COMMON)
    {
        if (event_id == BT_NOTIFY_COMMON_BT_STACK_READY)
            app_post(APP_MESSAGE_STACK_READY);
        else if (event_id == BT_NOTIFY_COMMON_ENCRYPTION &&
                 data_len >= sizeof(bt_notify_device_mac_t))
        {
            memcpy(&s_peer, data, sizeof(s_peer));
            s_peer_valid = RT_TRUE;
            app_post(APP_MESSAGE_BOND_READY);
        }
        else if (event_id == BT_NOTIFY_COMMON_PAIR_IND &&
                 data_len >= sizeof(bt_notify_device_base_info_t))
        {
            bt_notify_device_base_info_t *info =
                (bt_notify_device_base_info_t *)data;

            if (info->res == BTS2_SUCC)
            {
                s_peer = info->mac;
                s_peer_valid = RT_TRUE;
                app_post(APP_MESSAGE_BOND_READY);
            }
        }
        else if (event_id == BT_NOTIFY_COMMON_ACL_DISCONNECTED)
            app_post(APP_MESSAGE_PAN_DOWN);
        else if (event_id == BT_NOTIFY_COMMON_KEY_MISSING &&
                 data_len >= sizeof(bt_notify_device_base_info_t))
        {
            bt_notify_device_base_info_t *info =
                (bt_notify_device_base_info_t *)data;

            bt_cm_delete_bonded_devs_and_linkkey(info->mac.addr);
            s_peer_valid = RT_FALSE;
            app_post(APP_MESSAGE_PAN_DOWN);
        }
    }
    else if (type == BT_NOTIFY_PAN)
    {
        if (event_id == BT_NOTIFY_PAN_PROFILE_CONNECTED)
            app_post(APP_MESSAGE_PAN_UP);
        else if (event_id == BT_NOTIFY_PAN_PROFILE_DISCONNECTED)
            app_post(APP_MESSAGE_PAN_DOWN);
    }
    return 0;
}

uint32_t bt_get_class_of_device(void)
{
    return (uint32_t)BT_SRVCLS_NETWORK | BT_DEVCLS_LAP | BT_LAP_FULLY;
}

int main(void)
{
    app_message_t message;

    s_queue = rt_mq_create("learn_evt", sizeof(message), APP_QUEUE_DEPTH,
                           RT_IPC_FLAG_FIFO);
    if (s_queue == RT_NULL)
        return -RT_ENOMEM;
    s_connect_timer = rt_timer_create("learn_conn", app_connect_timer,
                                      RT_NULL,
                                      rt_tick_from_millisecond(
                                          LEARNING_CAMERA_PAN_CONNECT_DELAY_MS),
                                      RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    s_ip_timer = rt_timer_create("learn_ip", app_ip_timer, RT_NULL,
                                 rt_tick_from_millisecond(APP_IP_POLL_MS),
                                 RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    s_retry_timer = rt_timer_create("learn_retry", app_retry_timer, RT_NULL,
                                    rt_tick_from_millisecond(
                                        LEARNING_CAMERA_PAN_RETRY_DELAY_MS),
                                    RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (s_connect_timer == RT_NULL || s_ip_timer == RT_NULL ||
        s_retry_timer == RT_NULL)
        return -RT_ENOMEM;

    s_state = app_state_next(s_state, APP_EVENT_START);
    rt_kprintf("learning_camera_573 board=spi-hdk_lb573ub7n6 state=%d\n",
               s_state);
#ifdef BSP_BT_CONNECTION_MANAGER
    bt_cm_set_profile_target(BT_CM_PAN, BT_LINK_PHONE, 0);
#endif
    bt_interface_register_bt_event_notify_callback(app_bt_notify);
    sifli_ble_enable();

    while (rt_mq_recv(s_queue, &message, sizeof(message),
                      RT_WAITING_FOREVER) == RT_EOK)
        app_handle_message(&message);

    return 0;
}
