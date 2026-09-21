/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>
#include <string.h>
#include <ulog.h>

#include <lwip/ip_addr.h>
#include <netdev.h>

#include "app_state.h"
#include "audio_loopback.h"
#include "audio_server.h"
#include "bf0_sibles.h"
#include "bt_connection_manager.h"
#include "bt_pan_policy.h"
#include "bts2_app_inc.h"
#include "camera_service.h"
#include "camera_ui.h"
#include "learning_camera_status.h"
#include "xiaozhi_websocket.h"

// 376057

#define APP_QUEUE_DEPTH 16U
#define APP_IP_POLL_MS 200U
#define APP_BT_READY_TIMEOUT_MS 30000U
#define APP_XIAOZHI_RETRY_MS 2000U
#define APP_XIAOZHI_RETRY_MAX_MS 30000U
#define APP_XIAOZHI_RETRY_SHIFT_MAX 4U

typedef struct
{
    app_message_type_t type;
} app_message_t;

static rt_mq_t s_queue;
static rt_sem_t s_bt_ready;
static rt_timer_t s_connect_timer;
static rt_timer_t s_ip_timer;
static rt_timer_t s_retry_timer;
static rt_timer_t s_xiaozhi_timer;
static bt_notify_device_mac_t s_peer;
static rt_bool_t s_peer_valid;
static unsigned int s_retry_count;
static unsigned int s_ip_poll_count;
static unsigned int s_xiaozhi_retry;
static app_state_t s_state = APP_BOOT;

static void app_post(app_message_type_t type);

static void app_xiaozhi_event(ai_xiaozhi_event_t event, void *context)
{
    (void)context;
    switch (event)
    {
    case AI_XIAOZHI_EVENT_READY:
        rt_kprintf("xiaozhi: session ready\n");
        app_post(APP_MESSAGE_XIAOZHI_UP);
        break;
    case AI_XIAOZHI_EVENT_DISCONNECTED:
        learning_camera_status_set_tts_playing(0);
        rt_kprintf("xiaozhi: session disconnected\n");
        app_post(APP_MESSAGE_XIAOZHI_DOWN);
        break;
    case AI_XIAOZHI_EVENT_ERROR:
        rt_kprintf("xiaozhi: session error\n");

        if (!ai_xiaozhi_websocket_is_active())
            app_post(APP_MESSAGE_XIAOZHI_DOWN);
        break;
    case AI_XIAOZHI_EVENT_TTS_START:
        learning_camera_status_set_tts_playing(1);
        rt_kprintf("xiaozhi: TTS start\n");
        break;
    case AI_XIAOZHI_EVENT_TTS_STOP:
        learning_camera_status_set_tts_playing(0);
        rt_kprintf("xiaozhi: TTS stop\n");
        break;
    default:
        break;
    }
}

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

static void app_xiaozhi_timer(void *parameter)
{
    (void)parameter;
    app_post(APP_MESSAGE_XIAOZHI_RETRY);
}

static void app_schedule_xiaozhi_retry(void)
{
    learning_camera_status_t status;
    unsigned int shift = s_xiaozhi_retry < APP_XIAOZHI_RETRY_SHIFT_MAX
                             ? s_xiaozhi_retry : APP_XIAOZHI_RETRY_SHIFT_MAX;
    uint32_t delay = APP_XIAOZHI_RETRY_MS << shift;
    rt_tick_t ticks;

    learning_camera_status_get(&status);
    if (!ai_xiaozhi_session_enabled())
    {
        /* The user switched the session off from the sidebar panel. */
        rt_kprintf("xiaozhi: session off, retry disabled\n");
        return;
    }
    if (!status.pan_connected || !status.has_ip)
    {
        /* The PAN path owns its own retry; there is nothing to talk to yet. */
        rt_kprintf("xiaozhi: session down, network not ready\n");
        return;
    }
    if (delay > APP_XIAOZHI_RETRY_MAX_MS)
        delay = APP_XIAOZHI_RETRY_MAX_MS;
    if (s_xiaozhi_retry < APP_XIAOZHI_RETRY_SHIFT_MAX)
        ++s_xiaozhi_retry;
    ticks = rt_tick_from_millisecond(delay);
    rt_timer_stop(s_xiaozhi_timer);
    rt_timer_control(s_xiaozhi_timer, RT_TIMER_CTRL_SET_TIME, &ticks);
    rt_timer_start(s_xiaozhi_timer);
    rt_kprintf("xiaozhi: reconnect in %u ms\n", (unsigned int)delay);
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
        rt_kprintf("xiaozhi: PAN retry exhausted\n");
        return;
    }
    s_retry_count = next_retry;
    rt_timer_start(s_retry_timer);
    rt_kprintf("xiaozhi: PAN retry %u/%u\n", s_retry_count,
               LEARNING_CAMERA_PAN_RETRY_MAX);
}

static void app_handle_message(const app_message_t *message)
{
    switch (message->type)
    {
    case APP_MESSAGE_STACK_READY:
        bt_interface_set_local_name(strlen(LEARNING_CAMERA_BT_NAME),
                                    (void *)LEARNING_CAMERA_BT_NAME);
        rt_kprintf("xiaozhi: Bluetooth stack ready\n");
        break;
    case APP_MESSAGE_BOND_READY:
        s_retry_count = 0U;
        rt_timer_start(s_connect_timer);
        rt_kprintf("xiaozhi: pairing complete, PAN connect scheduled\n");
        break;
    case APP_MESSAGE_CONNECT_PAN:
    case APP_MESSAGE_RETRY_PAN:
        if (s_peer_valid)
        {
            rt_timer_stop(s_connect_timer);
            bt_interface_conn_ext(s_peer.addr, BT_PROFILE_PAN);
        }
        break;
    case APP_MESSAGE_PAN_UP:
        s_ip_poll_count = 0U;
        rt_timer_start(s_ip_timer);
        learning_camera_status_set_pan_connected(1);
        rt_kprintf("xiaozhi: PAN profile connected, waiting for DHCP\n");
        break;
    case APP_MESSAGE_IP_POLL:
        if (app_ip_ready())
        {
            rt_timer_stop(s_ip_timer);
            s_retry_count = 0U;
            s_state = app_state_next(s_state, APP_EVENT_PAN_READY);
            learning_camera_status_set_pan_connected(1);
            learning_camera_status_set_has_ip(1);
            rt_kprintf("xiaozhi: PAN ready, b0 has IPv4\n");
            /* The reference build starts the session from its assistant UI.
             * Without that UI the request is issued as soon as the network
             * is usable; listening is still controlled from the console. */
            if (ai_xiaozhi_session_enabled() && ai_xiaozhi_connect_async() < 0)
                rt_kprintf("xiaozhi: connect request failed\n");
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
        learning_camera_status_set_pan_connected(0);
        s_state = app_state_next(s_state, APP_EVENT_PAN_DOWN);
        app_start_retry();
        break;
    case APP_MESSAGE_XIAOZHI_UP:
        s_xiaozhi_retry = 0U;
        break;
    case APP_MESSAGE_XIAOZHI_DOWN:
        if (!ai_xiaozhi_websocket_is_active())
            app_schedule_xiaozhi_retry();
        break;
    case APP_MESSAGE_XIAOZHI_RETRY:
        if (!ai_xiaozhi_session_enabled())
            break;
        rt_kprintf("xiaozhi: reconnect attempt %u\n", s_xiaozhi_retry);
        if (ai_xiaozhi_connect_async() < 0)
            rt_kprintf("xiaozhi: reconnect request failed\n");
        break;
    default:
        break;
    }
}

static int app_bt_notify(uint16_t type, uint16_t event_id,
                         uint8_t *data, uint16_t data_len)
{
    app_message_type_t message = app_bt_message_for_event(type, event_id);

    if (message != APP_MESSAGE_NONE)
        app_post(message);
    if (type == BT_NOTIFY_COMMON)
    {
        if (event_id == BT_NOTIFY_COMMON_BT_STACK_READY)
        {
            app_post(APP_MESSAGE_STACK_READY);
            if (s_bt_ready != RT_NULL)
                rt_sem_release(s_bt_ready);
        }
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
    return 0;
}

uint32_t bt_get_class_of_device(void)
{
    return (uint32_t)BT_SRVCLS_NETWORK | BT_DEVCLS_LAP | BT_LAP_FULLY;
}

int main(void)
{
    app_message_t message;
    int result;

#ifdef ULOG_USING_FILTER
    /* The Bluetooth PAN glue logs one debug line per received TCP segment, which
     * drowns the console while an image is uploading. */
    ulog_tag_lvl_filter_set("BT.lwip", LOG_LVL_INFO);
#endif

    s_queue = rt_mq_create("xz_event", sizeof(message), APP_QUEUE_DEPTH,
                           RT_IPC_FLAG_FIFO);
    if (s_queue == RT_NULL)
        return -RT_ENOMEM;
    s_bt_ready = rt_sem_create("xz_bt", 0, RT_IPC_FLAG_FIFO);
    if (s_bt_ready == RT_NULL)
        return -RT_ENOMEM;
    s_connect_timer = rt_timer_create("xz_conn", app_connect_timer,
                                      RT_NULL,
                                      rt_tick_from_millisecond(
                                          LEARNING_CAMERA_PAN_CONNECT_DELAY_MS),
                                      RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    s_ip_timer = rt_timer_create("xz_ip", app_ip_timer, RT_NULL,
                                 rt_tick_from_millisecond(APP_IP_POLL_MS),
                                 RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    s_retry_timer = rt_timer_create("xz_retry", app_retry_timer, RT_NULL,
                                    rt_tick_from_millisecond(
                                        LEARNING_CAMERA_PAN_RETRY_DELAY_MS),
                                    RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    s_xiaozhi_timer = rt_timer_create("xz_ws", app_xiaozhi_timer, RT_NULL,
                                      rt_tick_from_millisecond(APP_XIAOZHI_RETRY_MS),
                                      RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (s_connect_timer == RT_NULL || s_ip_timer == RT_NULL ||
        s_retry_timer == RT_NULL || s_xiaozhi_timer == RT_NULL)
        return -RT_ENOMEM;

    ai_xiaozhi_set_event_callback(app_xiaozhi_event, RT_NULL);
    s_state = app_state_next(s_state, APP_EVENT_START);
    rt_kprintf("xiaozhi: app ready board=dpi-hdk_lb575ybbn6_n16_qspi_hcpu state=%d\n",
               s_state);
#ifdef BSP_BT_CONNECTION_MANAGER
    bt_cm_set_profile_target(BT_CM_HID, BT_LINK_PHONE, 1);
#endif
    bt_interface_register_bt_event_notify_callback(app_bt_notify);
    
    rt_kprintf("xiaozhi: enabling BT stack\n");
    sifli_ble_enable();
    rt_kprintf("xiaozhi: BT stack enable returned\n");
    if (s_bt_ready != RT_NULL &&
        rt_sem_take(s_bt_ready,
                    rt_tick_from_millisecond(APP_BT_READY_TIMEOUT_MS)) != RT_EOK)
        rt_kprintf("xiaozhi: BT stack not ready after %u ms, starting camera anyway\n",
                   APP_BT_READY_TIMEOUT_MS);
    else
        rt_kprintf("xiaozhi: BT stack ready, starting camera\n");

    result = camera_service_start();
    if (result != RT_EOK)
    {
        rt_kprintf("camera: service startup failed %d\n", result);
        return result;
    }
    result = camera_ui_start();
    if (result != RT_EOK)
        rt_kprintf("camera: UI startup failed %d\n", result);

    while (rt_mq_recv(s_queue, &message, sizeof(message),
                      RT_WAITING_FOREVER) == RT_EOK)
        app_handle_message(&message);

    return 0;
}

// ai_xiaozhi_vision_upload /photo/photo_0000000012.jpg