/* SPDX-License-Identifier: Apache-2.0 */
#include "learning_camera_status.h"

#include <lwip/ip_addr.h>
#include <netdev.h>
#include <rtthread.h>

#include "camera_service.h"
#include "xiaozhi_websocket.h"

#define LEARNING_CAMERA_STATUS_NETDEV "b0"

/* Written from the main loop and from the XiaoZhi event callback, read from
 * the UI thread: single word stores, so a snapshot copy needs no lock. */
static volatile int s_pan_connected;
static volatile int s_has_ip;
static volatile int s_tts_playing;

void learning_camera_status_set_pan_connected(int connected)
{
    s_pan_connected = connected;
    if (!connected)
        s_has_ip = 0;
}

void learning_camera_status_set_has_ip(int ready)
{
    s_has_ip = ready;
}

void learning_camera_status_set_tts_playing(int playing)
{
    s_tts_playing = playing;
}

void learning_camera_status_get(learning_camera_status_t *status)
{
    if (status == RT_NULL)
        return;
    status->pan_connected = s_pan_connected;
    status->has_ip = s_has_ip;
    /* A connect attempt reports CONNECTING even if a socket object exists. */
    if (ai_xiaozhi_websocket_is_connecting())
        status->ws_state = LEARNING_WS_CONNECTING;
    else if (ai_xiaozhi_websocket_is_active())
        status->ws_state = LEARNING_WS_READY;
    else
        status->ws_state = LEARNING_WS_OFFLINE;
    status->listening = ai_xiaozhi_websocket_is_listening();
    status->tts_playing = s_tts_playing;
    status->camera_capturing =
        camera_service_capture_stage() != CAMERA_CAPTURE_IDLE;
}

static const char *learning_camera_status_yes(int value)
{
    return value ? "yes" : "no";
}

static const char *learning_camera_status_ws_text(learning_ws_state_t state)
{
    switch (state)
    {
    case LEARNING_WS_READY:
        return "ready";
    case LEARNING_WS_CONNECTING:
        return "connecting";
    case LEARNING_WS_OFFLINE:
    default:
        return "offline";
    }
}

/* The IPv4 text is presentation only: has_ip is still decided by main.c's
 * app_ip_ready(), which is the single place that judges PAN readiness. */
static void learning_camera_status_print_ip(void)
{
    struct netdev *netdev = netdev_get_by_name(LEARNING_CAMERA_STATUS_NETDEV);

    if (netdev == RT_NULL || !netdev_is_up(netdev) ||
        !netdev_is_link_up(netdev) || ip_addr_isany(&netdev->ip_addr))
    {
        rt_kprintf("lcs: b0 address none\n");
        return;
    }
    {
        uint32_t address = ip_addr_get_ip4_u32(&netdev->ip_addr);

        rt_kprintf("lcs: b0 address %u.%u.%u.%u\n",
                   (unsigned int)((address >> 24) & 0xFFU),
                   (unsigned int)((address >> 16) & 0xFFU),
                   (unsigned int)((address >> 8) & 0xFFU),
                   (unsigned int)(address & 0xFFU));
    }
}

void learning_camera_status_print(void)
{
    learning_camera_status_t status;
    const char *ws_text;
    const char *address_state;

    learning_camera_status_get(&status);
    ws_text = learning_camera_status_ws_text(status.ws_state);
    address_state = learning_camera_status_yes(status.has_ip);
    rt_kprintf("lcs: pan=%s ip=%s ws=%s\n",
               learning_camera_status_yes(status.pan_connected), address_state,
               ws_text);
    rt_kprintf("lcs: listening=%s tts=%s camera=%s\n",
               learning_camera_status_yes(status.listening),
               learning_camera_status_yes(status.tts_playing),
               status.camera_capturing ? "busy" : "idle");
    learning_camera_status_print_ip();
}

static void learning_camera_status_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: learning_camera_status\n");
        return;
    }
    learning_camera_status_print();
}
MSH_CMD_EXPORT_ALIAS(learning_camera_status_command, learning_camera_status,
                     Show PAN WebSocket voice and camera state);
