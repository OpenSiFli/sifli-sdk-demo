#include <rtthread.h>

#include <stdint.h>
#include <string.h>

#include <lwip/api.h>
#include <lwip/apps/websocket_client.h>

#include "xiaozhi_activation.h"
#include "xiaozhi_audio.h"
#include "xiaozhi_protocol.h"
#include "xiaozhi_vision.h"

#define AI_XIAOZHI_WS_HEADER_MAX 1024U
#define AI_XIAOZHI_WS_TEXT_MAX 1024U
#define AI_XIAOZHI_WS_CONNECT_TIMEOUT_MS 15000U

static wsock_state_t s_socket;
static rt_sem_t s_connect_sem;
static rt_mutex_t s_write_lock;
static volatile int s_connect_status;
static volatile rt_bool_t s_connect_waiting;
static ai_xiaozhi_server_hello_t s_server_hello;
static rt_bool_t s_server_hello_received;
static rt_bool_t s_listening;

static err_t ai_xiaozhi_websocket_write(const void *data, size_t size,
                                        uint8_t opcode)
{
    err_t error;

    if (data == RT_NULL || size == 0U || size > UINT16_MAX ||
        s_socket.pcb == RT_NULL || s_write_lock == RT_NULL)
        return ERR_VAL;
    if (rt_mutex_take(s_write_lock, RT_WAITING_FOREVER) != RT_EOK)
        return ERR_MEM;
    error = wsock_write(&s_socket, (const char *)data, (u16_t)size, opcode);
    rt_mutex_release(s_write_lock);
    return error;
}

static int ai_xiaozhi_websocket_send_audio(void *context,
                                           const uint8_t *data, size_t size)
{
    (void)context;
    return ai_xiaozhi_websocket_write(data, size, OPCODE_BINARY) == ERR_OK ?
           0 : -1;
}

static err_t ai_xiaozhi_websocket_callback(int code, char *buffer, size_t size)
{
    if (code == WS_CONNECT)
    {
        s_connect_status = (int)(uintptr_t)buffer;
        if (s_connect_waiting)
            rt_sem_release(s_connect_sem);
    }
    else if (code == WS_DISCONNECT)
    {
        s_connect_status = -1;
        s_server_hello_received = RT_FALSE;
        s_listening = RT_FALSE;
        ai_xiaozhi_audio_disconnect();
        if (s_connect_waiting)
            rt_sem_release(s_connect_sem);
    }
    else if (code == WS_TEXT)
    {
        char text[AI_XIAOZHI_WS_TEXT_MAX];
        char mcp_session_id[AI_XIAOZHI_SESSION_ID_MAX];
        char mcp_response[384];
        ai_xiaozhi_vision_capability_t vision;
        int mcp_request_id;

        if (buffer == RT_NULL || size >= sizeof(text))
        {
            rt_kprintf("xiaozhi: text frame too large (%u bytes)\n",
                       (unsigned int)size);
            return ERR_OK;
        }
        rt_memcpy(text, buffer, size);
        text[size] = '\0';
        if (ai_xiaozhi_parse_server_hello(text, &s_server_hello) == 0)
        {
            s_server_hello_received = RT_TRUE;
            rt_kprintf("xiaozhi: server hello v%u %s %u Hz ch%u %u ms",
                       s_server_hello.protocol_version,
                       s_server_hello.audio_format,
                       (unsigned int)s_server_hello.sample_rate,
                       s_server_hello.channels,
                       s_server_hello.frame_duration);
            if (s_server_hello.session_id[0] != '\0')
                rt_kprintf(" session=%s", s_server_hello.session_id);
            rt_kprintf("\n");
            if (ai_xiaozhi_audio_configure(&s_server_hello) < 0)
                rt_kprintf("xiaozhi: unsupported audio parameters\n");
        }
        else if (ai_xiaozhi_parse_mcp_vision_initialize_request(
                     text, &vision, mcp_session_id, sizeof(mcp_session_id),
                     &mcp_request_id) == 0)
        {
            if (ai_xiaozhi_vision_set_capability(&vision) == 0)
            {
                int response_size = ai_xiaozhi_build_mcp_initialize_response(
                    mcp_response, sizeof(mcp_response), mcp_session_id,
                    mcp_request_id);

                if (response_size < 0 ||
                    ai_xiaozhi_websocket_write(mcp_response,
                                                (size_t)response_size,
                                                OPCODE_TEXT) != ERR_OK)
                    rt_kprintf("xiaozhi: MCP initialize response failed\n");
                else
                    rt_kprintf("xiaozhi: vision upload capability ready\n");
            }
            else
                rt_kprintf("xiaozhi: vision capability rejected\n");
        }
        else
        {
            rt_kprintf("xiaozhi: unhandled text frame (%u bytes)\n",
                       (unsigned int)size);
        }
    }
    else if (code == WS_DATA)
    {
        if (ai_xiaozhi_audio_receive((const uint8_t *)buffer, size) < 0)
            rt_kprintf("xiaozhi: audio packet dropped (%u bytes)\n",
                       (unsigned int)size);
    }
    return ERR_OK;
}

static void ai_xiaozhi_connect(int argc, char **argv)
{
    ai_xiaozhi_activation_t activation;
    char device_id[AI_XIAOZHI_DEVICE_ID_MAX];
    char client_id[AI_XIAOZHI_CLIENT_ID_MAX];
    char hello[192];
    int hello_length;
    err_t error;

    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: ai_xiaozhi_connect\n");
        return;
    }
    if (s_socket.pcb != RT_NULL)
    {
        rt_kprintf("xiaozhi: WebSocket is already active\n");
        return;
    }
    if (ai_xiaozhi_fetch_activation(&activation) < 0)
    {
        rt_kprintf("xiaozhi: activation request failed\n");
        return;
    }
    if (activation.activation_pending)
    {
        rt_kprintf("xiaozhi: bind device with code %s\n",
                   activation.activation_code);
        return;
    }
    if (ai_xiaozhi_get_identity(device_id, sizeof(device_id), client_id,
                                sizeof(client_id)) < 0)
    {
        rt_kprintf("xiaozhi: local identity unavailable\n");
        return;
    }
    if (s_connect_sem == RT_NULL)
        s_connect_sem = rt_sem_create("xz_conn", 0, RT_IPC_FLAG_FIFO);
    if (s_write_lock == RT_NULL)
        s_write_lock = rt_mutex_create("xz_write", RT_IPC_FLAG_PRIO);
    if (s_connect_sem == RT_NULL || s_write_lock == RT_NULL)
    {
        rt_kprintf("xiaozhi: no connection semaphore\n");
        return;
    }

    error = wsock_init(&s_socket, activation.endpoint.ssl_enabled, 1,
                       ai_xiaozhi_websocket_callback);
    if (error != ERR_OK)
    {
        rt_kprintf("xiaozhi: WebSocket init failed %d\n", error);
        return;
    }
    s_connect_status = 0;
    s_server_hello_received = RT_FALSE;
    s_listening = RT_FALSE;
    rt_memset(&s_server_hello, 0, sizeof(s_server_hello));
    s_connect_waiting = RT_TRUE;
    error = wsock_connect(&s_socket, AI_XIAOZHI_WS_HEADER_MAX,
                          activation.endpoint.host, activation.endpoint.path,
                          activation.endpoint.port, activation.token, RT_NULL,
                          "Protocol-Version: 1\r\nDevice-Id: %s\r\n"
                          "Client-Id: %s\r\n",
                          device_id, client_id);
    if (error != ERR_OK ||
        rt_sem_take(s_connect_sem,
                    rt_tick_from_millisecond(AI_XIAOZHI_WS_CONNECT_TIMEOUT_MS)) !=
            RT_EOK ||
        s_connect_status != 101)
    {
        s_connect_waiting = RT_FALSE;
        rt_kprintf("xiaozhi: WebSocket connect failed %d status %d\n", error,
                   s_connect_status);
        if (s_socket.pcb != RT_NULL)
            wsock_close(&s_socket, WSOCK_RESULT_ERR_CONNECT, error);
        return;
    }
    s_connect_waiting = RT_FALSE;
    hello_length = ai_xiaozhi_build_hello(hello, sizeof(hello));
    error = hello_length < 0 ? ERR_VAL :
            ai_xiaozhi_websocket_write(hello, (size_t)hello_length,
                                       OPCODE_TEXT);
    if (error != ERR_OK)
    {
        rt_kprintf("xiaozhi: hello send failed %d\n", error);
        wsock_close(&s_socket, WSOCK_RESULT_ERR_CONNECT, error);
        return;
    }
    rt_kprintf("xiaozhi: WebSocket connected\n");
}
MSH_CMD_EXPORT(ai_xiaozhi_connect, connect XiaoZhi WebSocket);

static int ai_xiaozhi_send_listen(rt_bool_t start)
{
    char message[192];
    int message_length;
    err_t error;

    if (s_socket.pcb == RT_NULL || !s_server_hello_received ||
        s_server_hello.session_id[0] == '\0')
    {
        rt_kprintf("xiaozhi: session is not ready\n");
        return -1;
    }
    if (start && s_listening)
    {
        rt_kprintf("xiaozhi: listen already started\n");
        return 0;
    }
    if (!start && !s_listening)
    {
        rt_kprintf("xiaozhi: listen is not active\n");
        return 0;
    }

    message_length = start
                         ? ai_xiaozhi_build_listen_start(
                               message, sizeof(message),
                               s_server_hello.session_id)
                         : ai_xiaozhi_build_listen_stop(
                               message, sizeof(message),
                               s_server_hello.session_id);
    if (message_length < 0)
    {
        rt_kprintf("xiaozhi: listen message build failed\n");
        return -1;
    }
    if (start && ai_xiaozhi_audio_configure(&s_server_hello) < 0)
    {
        rt_kprintf("xiaozhi: server audio parameters are unsupported\n");
        return -1;
    }
    if (!start)
        ai_xiaozhi_audio_listen_stop();
    error = ai_xiaozhi_websocket_write(message, (size_t)message_length,
                                       OPCODE_TEXT);
    if (error != ERR_OK)
    {
        rt_kprintf("xiaozhi: listen %s failed %d\n",
                   start ? "start" : "stop", error);
        return -1;
    }
    if (start && ai_xiaozhi_audio_listen_start(
                     ai_xiaozhi_websocket_send_audio, RT_NULL) < 0)
    {
        char stop_message[192];
        int stop_length = ai_xiaozhi_build_listen_stop(
            stop_message, sizeof(stop_message), s_server_hello.session_id);

        if (stop_length > 0)
            (void)ai_xiaozhi_websocket_write(stop_message,
                                             (size_t)stop_length,
                                             OPCODE_TEXT);
        rt_kprintf("xiaozhi: microphone start failed\n");
        return -1;
    }
    s_listening = start;
    rt_kprintf("xiaozhi: listen %s\n", start ? "start" : "stop");
    return 0;
}

static void ai_xiaozhi_listen_start(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: ai_xiaozhi_listen_start\n");
        return;
    }
    (void)ai_xiaozhi_send_listen(RT_TRUE);
}
MSH_CMD_EXPORT(ai_xiaozhi_listen_start, start XiaoZhi listening session);

static void ai_xiaozhi_listen_stop(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: ai_xiaozhi_listen_stop\n");
        return;
    }
    (void)ai_xiaozhi_send_listen(RT_FALSE);
}
MSH_CMD_EXPORT(ai_xiaozhi_listen_stop, stop XiaoZhi listening session);
