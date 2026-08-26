#include <rtthread.h>

#include <lwip/api.h>
#include <lwip/apps/websocket_client.h>

#include "xiaozhi_activation.h"
#include "xiaozhi_protocol.h"

#define AI_XIAOZHI_WS_HEADER_MAX 1024U
#define AI_XIAOZHI_WS_CONNECT_TIMEOUT_MS 15000U

static wsock_state_t s_socket;
static rt_sem_t s_connect_sem;
static volatile int s_connect_status;
static volatile rt_bool_t s_connect_waiting;

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
        if (s_connect_waiting)
            rt_sem_release(s_connect_sem);
    }
    else if (code == WS_TEXT)
    {
        rt_kprintf("xiaozhi: text %.*s\n", (int)size, buffer);
    }
    else if (code == WS_DATA)
    {
        rt_kprintf("xiaozhi: audio packet %u bytes\n", (unsigned int)size);
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
    if (s_connect_sem == RT_NULL)
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
            wsock_write(&s_socket, hello, (u16_t)hello_length, OPCODE_TEXT);
    if (error != ERR_OK)
    {
        rt_kprintf("xiaozhi: hello send failed %d\n", error);
        wsock_close(&s_socket, WSOCK_RESULT_ERR_CONNECT, error);
        return;
    }
    rt_kprintf("xiaozhi: WebSocket connected\n");
}
MSH_CMD_EXPORT(ai_xiaozhi_connect, connect XiaoZhi WebSocket);
