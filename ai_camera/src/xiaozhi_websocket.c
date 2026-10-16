#include <rtthread.h>
#include <rtdevice.h>

#include <stdint.h>
#include <string.h>

#include <lwip/api.h>
#include <lwip/apps/websocket_client.h>

#include "xiaozhi_activation.h"
#include "xiaozhi_audio.h"
#include "xiaozhi_protocol.h"
#include "xiaozhi_vision.h"
#include "xiaozhi_websocket.h"

#define AI_XIAOZHI_WS_HEADER_MAX 1024U
#define AI_XIAOZHI_WS_TEXT_MAX 1024U
#define AI_XIAOZHI_WS_CONNECT_TIMEOUT_MS 15000U
#define AI_XIAOZHI_CONNECT_THREAD_STACK_SIZE 4096*2U
#define AI_XIAOZHI_CONNECT_THREAD_PRIORITY 22U

static wsock_state_t s_socket;
static rt_sem_t s_connect_sem;
static rt_mutex_t s_write_lock;
static volatile int s_connect_status;
static volatile rt_bool_t s_connect_waiting;
static ai_xiaozhi_server_hello_t s_server_hello;
static rt_bool_t s_server_hello_received;
static rt_bool_t s_listening;
static volatile rt_bool_t s_connect_in_progress;
static ai_xiaozhi_event_callback_t s_event_callback;
static void *s_event_context;
/* MCP replies are built on the WebSocket receive path, which runs on a small
 * stack, so the buffers live in BSS instead. */
static char s_mcp_response[AI_XIAOZHI_MCP_TOOL_RESULT_MAX];
static char s_mcp_tool_name[AI_XIAOZHI_MCP_TOOL_NAME_MAX];
static char s_mcp_question[AI_XIAOZHI_MCP_QUESTION_MAX];
static struct rt_work s_audio_disconnect_work;
static rt_bool_t s_audio_disconnect_work_initialized;
/* Owned by the assistant UI: the sidebar panel switches it on and off. */
static volatile int s_session_enabled = 1;

static void ai_xiaozhi_audio_disconnect_work(struct rt_work *work,
                                             void *work_data)
{
    (void)work;
    (void)work_data;
    ai_xiaozhi_audio_disconnect();
}

static void ai_xiaozhi_notify(ai_xiaozhi_event_t event)
{
    if (s_event_callback != RT_NULL)
        s_event_callback(event, s_event_context);
}

/* rt_kprintf truncates long lines, so an unrecognised frame is shown in short
 * chunks and no byte of the protocol is lost. */
static void ai_xiaozhi_log_frame(const char *text, size_t size)
{
    size_t offset;

    rt_kprintf("xiaozhi: unhandled frame %u bytes\n", (unsigned int)size);
    for (offset = 0U; offset < size; offset += 96U)
    {
        char line[112];
        size_t chunk = size - offset;

        if (chunk > 96U)
            chunk = 96U;
        rt_memcpy(line, text + offset, chunk);
        line[chunk] = '\0';
        rt_kprintf("ws| %s\n", line);
    }
}

void ai_xiaozhi_set_event_callback(ai_xiaozhi_event_callback_t callback,
                                   void *context)
{
    s_event_callback = callback;
    s_event_context = context;
    if (!s_audio_disconnect_work_initialized)
    {
        rt_work_init(&s_audio_disconnect_work,
                     ai_xiaozhi_audio_disconnect_work, RT_NULL);
        s_audio_disconnect_work_initialized = RT_TRUE;
    }
}

int ai_xiaozhi_websocket_is_active(void)
{
    return s_socket.pcb != RT_NULL;
}

int ai_xiaozhi_websocket_is_connecting(void)
{
    return s_connect_in_progress != RT_FALSE;
}

int ai_xiaozhi_websocket_is_listening(void)
{
    return s_listening != RT_FALSE;
}

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

int ai_xiaozhi_websocket_send_text(const char *text, size_t size)
{
    if (text == RT_NULL || size == 0U)
        return -1;
    return ai_xiaozhi_websocket_write(text, size, OPCODE_TEXT) == ERR_OK ?
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
        rt_bool_t was_ready = s_server_hello_received;

        int reason = (int)(uintptr_t)buffer;

        s_connect_status = -1;
        s_server_hello_received = RT_FALSE;
        s_listening = RT_FALSE;
        if (s_audio_disconnect_work_initialized)
            (void)rt_work_submit(&s_audio_disconnect_work, 0);
        if (was_ready)
            ai_xiaozhi_notify(AI_XIAOZHI_EVENT_DISCONNECTED);
        rt_kprintf("xiaozhi: socket closed reason=%d ready=%d\n", reason,
                   (int)was_ready);
        if (s_connect_waiting)
            rt_sem_release(s_connect_sem);
    }
    else if (code == WS_TEXT)
    {
        char text[AI_XIAOZHI_WS_TEXT_MAX];
        char mcp_session_id[AI_XIAOZHI_SESSION_ID_MAX];
        char mcp_response[384];
        ai_xiaozhi_vision_capability_t vision;
        ai_xiaozhi_server_event_t server_event;
        int mcp_request_id;

        if (buffer == RT_NULL || size >= sizeof(text))
            return ERR_OK;
        rt_memcpy(text, buffer, size);
        text[size] = '\0';
        if (ai_xiaozhi_parse_server_hello(text, &s_server_hello) == 0)
        {
            s_server_hello_received = RT_TRUE;
            ai_xiaozhi_notify(AI_XIAOZHI_EVENT_READY);
        }
        else if ((server_event = ai_xiaozhi_parse_server_event(text)) !=
                 AI_XIAOZHI_SERVER_EVENT_NONE)
        {
            ai_xiaozhi_notify(
                server_event == AI_XIAOZHI_SERVER_EVENT_TTS_START
                    ? AI_XIAOZHI_EVENT_TTS_START
                    : AI_XIAOZHI_EVENT_TTS_STOP);
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

                if (response_size >= 0)
                    (void)ai_xiaozhi_websocket_write(mcp_response,
                                                     (size_t)response_size,
                                                     OPCODE_TEXT);
            }
        }
        else if (ai_xiaozhi_parse_mcp_tools_list_request(
                     text, mcp_session_id, sizeof(mcp_session_id),
                     &mcp_request_id) == 0)
        {
            /* Announcing a tool is what lets the model ask for a photo. */
            int response_size = ai_xiaozhi_build_mcp_tools_list_response(
                s_mcp_response, sizeof(s_mcp_response), mcp_session_id,
                mcp_request_id);

            rt_kprintf("xiaozhi: tools/list -> announce camera\n");
            if (response_size >= 0)
                (void)ai_xiaozhi_websocket_write(s_mcp_response,
                                                 (size_t)response_size,
                                                 OPCODE_TEXT);
        }
        else if (ai_xiaozhi_parse_mcp_tools_call_request(
                     text, mcp_session_id, sizeof(mcp_session_id),
                     &mcp_request_id, s_mcp_tool_name, sizeof(s_mcp_tool_name),
                     s_mcp_question, sizeof(s_mcp_question)) == 0)
        {
            rt_kprintf("xiaozhi: tools/call %s q=%s\n", s_mcp_tool_name,
                       s_mcp_question);
            if (ai_xiaozhi_vision_request_tool_call(
                    mcp_session_id, mcp_request_id, s_mcp_question) != 0)
            {
                /* Another photo is still in flight: answer this call anyway so
                 * the turn does not stall. */
                int response_size = ai_xiaozhi_build_mcp_tool_result(
                    s_mcp_response, sizeof(s_mcp_response), mcp_session_id,
                    mcp_request_id, "设备正在处理上一张照片，请稍后再试。", 1);

                if (response_size >= 0)
                    (void)ai_xiaozhi_websocket_write(s_mcp_response,
                                                     (size_t)response_size,
                                                     OPCODE_TEXT);
            }
        }
        else
            ai_xiaozhi_log_frame(text, size);
    }
    else if (code == WS_DATA)
    {
        (void)ai_xiaozhi_audio_receive((const uint8_t *)buffer, size);
    }
    return ERR_OK;
}

static int ai_xiaozhi_connect_request(void)
{
    ai_xiaozhi_activation_t activation;
    char device_id[AI_XIAOZHI_DEVICE_ID_MAX];
    char client_id[AI_XIAOZHI_CLIENT_ID_MAX];
    char hello[192];
    int hello_length;
    err_t error;

    if (s_socket.pcb != RT_NULL)
    {
        rt_kprintf("xiaozhi: WebSocket is already active\n");
        return 0;
    }
    if (s_audio_disconnect_work_initialized)
        (void)rt_work_cancel_sync(&s_audio_disconnect_work);
    ai_xiaozhi_audio_disconnect();
    if (ai_xiaozhi_fetch_activation(&activation) < 0)
    {
        rt_kprintf("xiaozhi: activation request failed\n");
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
        return -1;
    }

    if (activation.activation_pending)
        rt_kprintf("xiaozhi: unbound device, bind code %s\n",
                   activation.activation_code);

    if (ai_xiaozhi_get_identity(device_id, sizeof(device_id), client_id,
                                sizeof(client_id)) < 0)
    {
        rt_kprintf("xiaozhi: local identity unavailable\n");
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
        return -1;
    }

    if (s_connect_sem == RT_NULL)
        s_connect_sem = rt_sem_create("xz_conn", 0, RT_IPC_FLAG_FIFO);
    if (s_write_lock == RT_NULL)
        s_write_lock = rt_mutex_create("xz_write", RT_IPC_FLAG_PRIO);
    if (s_connect_sem == RT_NULL || s_write_lock == RT_NULL)
    {
        rt_kprintf("xiaozhi: no connection semaphore\n");
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
        return -1;
    }

    error = wsock_init(&s_socket, activation.endpoint.ssl_enabled, 1,
                       ai_xiaozhi_websocket_callback);

    if (error != ERR_OK)
    {
        rt_kprintf("xiaozhi: WebSocket init failed %d\n", error);
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
        return -1;
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
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
        return -1;
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
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
        return -1;
    }
    rt_kprintf("xiaozhi: WebSocket connected\n");
    return 0;
}

static void ai_xiaozhi_connect(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: ai_xiaozhi_connect\n");
        return;
    }
    /* Asking for a session from the console switches the retry back on. */
    ai_xiaozhi_session_set_enabled(1);
    (void)ai_xiaozhi_connect_request();
}
MSH_CMD_EXPORT(ai_xiaozhi_connect, connect XiaoZhi WebSocket);

static void ai_xiaozhi_disconnect(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: ai_xiaozhi_disconnect\n");
        return;
    }
    ai_xiaozhi_session_set_enabled(0);
    ai_xiaozhi_session_close();
}
MSH_CMD_EXPORT(ai_xiaozhi_disconnect, close the XiaoZhi WebSocket session);

static void ai_xiaozhi_connect_worker(void *parameter)
{
    (void)parameter;
    (void)ai_xiaozhi_connect_request();
    s_connect_in_progress = RT_FALSE;
}

int ai_xiaozhi_connect_async(void)
{
    rt_thread_t thread;

    if (s_socket.pcb != RT_NULL || s_connect_in_progress)
        return 0;
    thread = rt_thread_create("xz_conn", ai_xiaozhi_connect_worker, RT_NULL,
                              AI_XIAOZHI_CONNECT_THREAD_STACK_SIZE,
                              AI_XIAOZHI_CONNECT_THREAD_PRIORITY,
                              RT_THREAD_TICK_DEFAULT);
    if (thread == RT_NULL)
        return -1;
    s_connect_in_progress = RT_TRUE;
    rt_thread_startup(thread);
    return 0;
}

void ai_xiaozhi_session_set_enabled(int enabled)
{
    s_session_enabled = enabled ? 1 : 0;
}

int ai_xiaozhi_session_enabled(void)
{
    return s_session_enabled != 0;
}

/* Local close. The session is torn down the same way a server close is, only the
 * Bluetooth PAN link is left alone; the app keeps its own retry policy. */
void ai_xiaozhi_session_close(void)
{
    if (s_socket.pcb == RT_NULL)
        return;
    s_listening = RT_FALSE;
    s_server_hello_received = RT_FALSE;
    if (s_audio_disconnect_work_initialized)
        (void)rt_work_submit(&s_audio_disconnect_work, 0);
    if (wsock_close(&s_socket, WSOCK_RESULT_LOCAL_ABORT, ERR_OK) != ERR_OK)
        rt_kprintf("xiaozhi: session close failed\n");
    else
        rt_kprintf("xiaozhi: session closed locally\n");
}

static int ai_xiaozhi_send_listen(rt_bool_t start)
{
    char message[192];
    int message_length;
    err_t error;

    if (s_socket.pcb == RT_NULL || !s_server_hello_received ||
        s_server_hello.session_id[0] == '\0')
    {
        rt_kprintf("xiaozhi: session is not ready\n");
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
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
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
        return -1;
    }
    if (start && ai_xiaozhi_audio_configure(&s_server_hello) < 0)
    {
        rt_kprintf("xiaozhi: server audio parameters are unsupported\n");
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
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
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
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
        ai_xiaozhi_notify(AI_XIAOZHI_EVENT_ERROR);
        return -1;
    }
    s_listening = start;
    rt_kprintf("xiaozhi: listen %s\n", start ? "start" : "stop");
    return 0;
}

int ai_xiaozhi_listen_start_request(void)
{
    return ai_xiaozhi_send_listen(RT_TRUE);
}

int ai_xiaozhi_listen_stop_request(void)
{
    return ai_xiaozhi_send_listen(RT_FALSE);
}

static void ai_xiaozhi_listen_start(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: ai_xiaozhi_listen_start\n");
        return;
    }
    (void)ai_xiaozhi_listen_start_request();
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
    (void)ai_xiaozhi_listen_stop_request();
}
MSH_CMD_EXPORT(ai_xiaozhi_listen_stop, stop XiaoZhi listening session);
