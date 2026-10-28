/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ota_wifi_tran.c
 * @brief TCP relay channel (WiFi variant of the cast transport).
 *
 * Ported from solution2.0 `components/ota/ota_pan/ota_pan_trans.c` (BT PAN
 * version) with the BT sniff / disconnect-callback parts removed. Runs on the
 * device side as a P2P GO hotspot plus TCP server.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <rtthread.h>
#include "rtconfig.h"

#include <wlan_mgnt.h>
#include <board.h>
#include "proto_v3_link.h"
#include "proto_v3.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include "ota_wifi_tran.h"

#define LOG_TAG "WIFI_TRAN"
#include "log.h"

#define IP_MAX_LEN              24
#define PORT_MAX_LEN            8

/* Frame header magic, shared with the phone and the protocol engine */
#define SERIAL_TRANS_MAGIC      0x53454346

#define TCP_BACKLOG                 (5)
#define MAX_PACKET_CACHE_NUMBER     (4)

enum
{
    NET_DATA_STATUS_IDLE = 0,           // slot free
    NET_DATA_STATUS_UPDATING,           // being filled
    NET_DATA_STATUS_UPDATED,            // complete, ready to consume
    NET_DATA_STATUS_USING,              // being consumed
};
typedef uint8_t net_data_status_t;

typedef struct
{
    net_data_status_t status;      // slot state
    uint32_t tick;                 // time of the last update
    uint32_t malloc_size;          // allocated size
    uint32_t size;                 // valid data size
    uint32_t offset;               // data offset
    uint8_t *data;                 // data buffer
} net_packet_t;

enum
{
    OTA_EXT_PAN_STATUS_CLOSED = 0,
    OTA_EXT_PAN_STATUS_OPENED,
};

typedef struct
{
    int32_t             sockfd;
    int32_t             listen_fd;
    rt_thread_t         recv_thread;
    rt_thread_t         assemble_thread;
    rt_thread_t         accept_thread;

    uint32_t            total_recv_frames;
    uint32_t            total_drop_frames;
    uint8_t             status;
    uint8_t             env_init;
    uint8_t             log_on;
    net_packet_t       *current_packet;
    net_packet_t        packets[MAX_PACKET_CACHE_NUMBER];
} ota_wifi_tran_env_t;

static ota_wifi_tran_env_t g_wifi_tran_env;
static struct rt_semaphore g_recv_sem;
static struct rt_mutex     g_tran_mutex;

static char server_ip[IP_MAX_LEN] = "192.168.88.1";
static char server_port[PORT_MAX_LEN] = "2025";

static void ota_ext_pan_recv_data_entry(void *parameter);
static void ota_ext_pan_assemble_data_entry(void *parameter);
static int ota_ext_pan_close_internal(bool close_listener);

static ota_wifi_tran_env_t *wifi_tran_get_env(void)
{
    return &g_wifi_tran_env;
}

/* CRC32/MPEG-2 using the hardware CRC unit (init 0xFFFFFFFF, no final
 * inversion), matching the phone and the protocol engine */
static uint32_t wifi_tran_crc32_mpeg2(const uint8_t *data, uint32_t len)
{
    CRC_HandleTypeDef crc_handle = {0};

    crc_handle.Instance = hwp_crc;
    if (HAL_CRC_Init(&crc_handle) != HAL_OK)
    {
        return 0;
    }
    HAL_CRC_Setmode(&crc_handle, CRC_32_MPEG_2);
    return HAL_CRC_Calculate(&crc_handle, (uint8_t *)data, len);
}

/* Validate and dispatch one complete frame (phone -> device):
 * [magic][u32 len][u8 cate][3B][payload][u32 crc] */
static void wifi_tran_dispatch_frame(net_packet_t *frame)
{
    uint32_t magic, data_len_u32, remote_crc, calc_crc;
    uint16_t data_len;
    uint8_t cate_id;

    memcpy(&magic, frame->data, 4);
    memcpy(&data_len_u32, frame->data + 4, 4);
    cate_id = *(frame->data + 8);
    data_len = (uint16_t)data_len_u32;

    if (magic != SERIAL_TRANS_MAGIC)
    {
        LOG_E("frame magic error 0x%x", magic);
        return;
    }
    memcpy(&remote_crc, frame->data + 12 + data_len, 4);
    calc_crc = wifi_tran_crc32_mpeg2(frame->data + 12, data_len);
    if (calc_crc != remote_crc)
    {
        LOG_E("frame crc error 0x%x, 0x%x", calc_crc, remote_crc);
        proto_v3_rx_error(BLE_SERIAL_TRAN_ERROR_DATA_ERROR);
        return;
    }
    proto_v3_rx_frame(cate_id, frame->data + 12, data_len);
}

static void tran_env_init(ota_wifi_tran_env_t *env)
{
    if (env->env_init)
    {
        return;
    }
    rt_memset(env->packets, 0, sizeof(net_packet_t) * MAX_PACKET_CACHE_NUMBER);

    env->recv_thread = rt_thread_create("wifi_recv", ota_ext_pan_recv_data_entry, env, 2048,
                                        RT_THREAD_PRIORITY_MIDDLE, RT_THREAD_TICK_DEFAULT);
    if (!env->recv_thread)
    {
        LOG_I("create recv thread failed");
        return;
    }
    env->assemble_thread = rt_thread_create("wifi_asm", ota_ext_pan_assemble_data_entry, env, 4096,
                                            RT_THREAD_PRIORITY_MIDDLE, RT_THREAD_TICK_DEFAULT);
    if (!env->assemble_thread)
    {
        LOG_I("create assemble thread failed");
        rt_thread_delete(env->recv_thread);
        env->recv_thread = NULL;
        return;
    }
    env->status = OTA_EXT_PAN_STATUS_OPENED;
    rt_sem_control(&g_recv_sem, RT_IPC_CMD_RESET, RT_NULL);
    rt_thread_startup(env->recv_thread);
    rt_thread_startup(env->assemble_thread);
    env->env_init = 1;
}

static void tran_env_deinit(ota_wifi_tran_env_t *env)
{
    if (env->env_init == 0)
    {
        return;
    }
    for (uint8_t i = 0; i < MAX_PACKET_CACHE_NUMBER; i++)
    {
        if (env->packets[i].data)
        {
            rt_free(env->packets[i].data);
            env->packets[i].data = NULL;
            env->packets[i].status = NET_DATA_STATUS_IDLE;
            env->packets[i].size = 0;
        }
    }
    env->current_packet = NULL;
    env->env_init = 0;
}

static net_packet_t *get_packet_by_status(ota_wifi_tran_env_t *env, net_data_status_t status)
{
    for (int i = 0; i < MAX_PACKET_CACHE_NUMBER; i++)
    {
        if (env->packets[i].status == status)
        {
            return &env->packets[i];
        }
    }
    return NULL;
}

static net_packet_t *get_packet_oldest_updated_packet(ota_wifi_tran_env_t *env)
{
    net_packet_t *frame = NULL;

    for (int i = 0; i < MAX_PACKET_CACHE_NUMBER; i++)
    {
        if (env->packets[i].status != NET_DATA_STATUS_UPDATED)
        {
            continue;
        }
        if (!frame || frame->tick > env->packets[i].tick)
        {
            frame = &env->packets[i];
        }
    }
    return frame;
}

static void dump_packet_status(ota_wifi_tran_env_t *env)
{
    for (int i = 0; i < MAX_PACKET_CACHE_NUMBER; i++)
    {
        LOG_I("frame[%d] %p status:%d", i, &env->packets[i], env->packets[i].status);
    }
}

static net_packet_t *get_packet_to_write(ota_wifi_tran_env_t *env, uint32_t w_size)
{
    net_packet_t *frame = NULL;
    uint8_t retry = 255;

    rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
    do
    {
        frame = get_packet_by_status(env, NET_DATA_STATUS_IDLE);
        if (!frame)
        {
            frame = get_packet_by_status(env, NET_DATA_STATUS_UPDATED);
        }
        if (!frame)
        {
            dump_packet_status(env);
            rt_mutex_release(&g_tran_mutex);
            LOG_E("no packet cache available");
            return NULL;
        }
        else if (frame->status == NET_DATA_STATUS_IDLE)
        {
            break;
        }
        else if (frame->status == NET_DATA_STATUS_USING || frame->status == NET_DATA_STATUS_UPDATED)
        {
            rt_mutex_release(&g_tran_mutex);
            LOG_I("recv thread yield");
            rt_thread_yield();
            rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
        }
    }
    while (retry--);

    frame = get_packet_by_status(env, NET_DATA_STATUS_IDLE);
    if (!frame)
    {
        frame = get_packet_oldest_updated_packet(env);
    }
    if (!frame)
    {
        LOG_E("no frame or all frames are using");
        dump_packet_status(env);
        rt_mutex_release(&g_tran_mutex);
        return NULL;
    }
    if (frame->status == NET_DATA_STATUS_UPDATED)
    {
        env->total_drop_frames++;
        LOG_I("recv thread drop frame, total_drop %d, total_recv %d", env->total_drop_frames, env->total_recv_frames);
    }

    frame->status = NET_DATA_STATUS_UPDATING;
    frame->offset = 0;

    if (!frame->data || frame->malloc_size < w_size)
    {
        if (frame->data)
        {
            rt_free(frame->data);
        }
        frame->data = rt_malloc(w_size);
        if (!frame->data)
        {
            frame->status = NET_DATA_STATUS_IDLE;
            rt_mutex_release(&g_tran_mutex);
            LOG_E("malloc frame %d failed", w_size);
            return NULL;
        }
        frame->malloc_size = w_size;
    }
    frame->size = w_size;
    rt_mutex_release(&g_tran_mutex);
    return frame;
}

static bool packet_set_done(ota_wifi_tran_env_t *env, net_packet_t *frame)
{
    rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
    frame->status = NET_DATA_STATUS_UPDATED;
    env->total_recv_frames++;
    frame->tick = env->total_recv_frames;
    rt_mutex_release(&g_tran_mutex);
    return true;
}

/* Drop len bytes (used to resync on the next frame header when the cache is full) */
static void wifi_tran_drain(int32_t sockfd, uint32_t len)
{
    uint8_t tmp[64];

    while (len > 0)
    {
        int rd = (len > sizeof(tmp)) ? (int)sizeof(tmp) : (int)len;
        int ret = recv(sockfd, (char *)tmp, rd, 0);
        if (ret <= 0)
        {
            break;
        }
        len -= ret;
    }
}

static void ota_ext_pan_assemble_data_entry(void *parameter)
{
    LOG_I("ota_ext_pan_assemble_data_entry");
    ota_wifi_tran_env_t *env = (ota_wifi_tran_env_t *)parameter;
    net_packet_t *frame;

    while (1)
    {
        rt_sem_take(&g_recv_sem, RT_WAITING_FOREVER);
        rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
        frame = get_packet_oldest_updated_packet(env);
        if (frame)
        {
            frame->status = NET_DATA_STATUS_USING;
            uint32_t tick = rt_tick_get_millisecond();
            wifi_tran_dispatch_frame(frame);
            if (env->log_on)
            {
                LOG_I("assemble data size:%d, spend %d ms", frame->size, rt_tick_get_millisecond() - tick);
            }
            frame->status = NET_DATA_STATUS_IDLE;
        }
        rt_mutex_release(&g_tran_mutex);
    }
}

static void ota_ext_pan_recv_data_entry(void *parameter)
{
    LOG_I("ota_ext_pan_recv_data_entry");
    ota_wifi_tran_env_t *env = (ota_wifi_tran_env_t *)parameter;

    uint8_t header[12];
    uint16_t data_len;
    uint32_t magic, data_len_u32;
    uint32_t total_packet_len, recv_idx;

    while (1)
    {
        if (NULL == env->current_packet)
        {
            data_len = 0;
            do
            {
                uint16_t get_len = sizeof(header) - data_len;
                int ret = recv(env->sockfd, (char *)header + data_len, get_len, 0);
                if (ret > 0)
                {
                    data_len += ret;
                }
                else if (ret == 0)
                {
                    LOG_I("server disconnect");
                    break;
                }
                else
                {
                    LOG_I("recv error %d", ret);
                    break;
                }
            }
            while (data_len < sizeof(header));

            if (data_len != sizeof(header))
            {
                LOG_I("recv header error length %d", data_len);
                break;
            }

            memcpy(&magic, header, 4);
            if (magic != SERIAL_TRANS_MAGIC)
            {
                LOG_I("skip for magic 0x%x", magic);
                continue;
            }
            memcpy(&data_len_u32, header + 4, 4);
            if (data_len_u32 > 0xFFFF)
            {
                LOG_I("skip for length %d", data_len_u32);
                continue;
            }
            data_len = (uint16_t)data_len_u32;

            total_packet_len = sizeof(header) + data_len + 4;
            env->current_packet = get_packet_to_write(env, total_packet_len);
            if (env->current_packet == NULL)
            {
                /* All 4 frame slots are in use (should not happen): drop this
                 * frame and resync */
                LOG_E("no packet buffer, drop one frame");
                wifi_tran_drain(env->sockfd, total_packet_len - sizeof(header));
                continue;
            }
            rt_memcpy(env->current_packet->data, header, sizeof(header));

            recv_idx = sizeof(header);
        }

        if (recv_idx < total_packet_len)
        {
            int ret = recv(env->sockfd, (char *)env->current_packet->data + recv_idx,
                           total_packet_len - recv_idx, 0);
            if (ret > 0)
            {
                recv_idx += ret;
            }
            else if (ret == 0)
            {
                LOG_I("server disconnect");
                break;
            }
            else
            {
                LOG_I("recv error %d", ret);
                break;
            }
        }
        if (recv_idx == total_packet_len)
        {
            packet_set_done(env, env->current_packet);
            rt_sem_release(&g_recv_sem);
            env->current_packet = NULL;
        }
        else if (recv_idx > total_packet_len)
        {
            LOG_I("recv_idx %d, total_packet_len %d", recv_idx, total_packet_len);
            break;
        }
    }
    LOG_I("exit recv data thread!");
    ota_ext_pan_close_internal(false);
}

static void ota_ext_pan_accept_entry(void *parameter)
{
    ota_wifi_tran_env_t *env = wifi_tran_get_env();
    LOG_I("enter accept thread!");

    while (1)
    {
        int32_t listen_fd;
        int32_t client_fd = -1;

        rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
        listen_fd = env->listen_fd;
        rt_mutex_release(&g_tran_mutex);

        if (listen_fd < 0)
        {
            break;
        }

        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0)
        {
            LOG_E("socket accept fail, fd %d", client_fd);
            rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
            if (env->listen_fd == listen_fd)
            {
                env->listen_fd = -1;
            }
            env->accept_thread = NULL;
            rt_mutex_release(&g_tran_mutex);
            close(listen_fd);
            return;
        }

        int tcp_nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay, sizeof(tcp_nodelay));

        rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
        if (env->sockfd >= 0)
        {
            LOG_E("reject accepted fd %d, active fd %d already exists", client_fd, env->sockfd);
            rt_mutex_release(&g_tran_mutex);
            close(client_fd);
            continue;
        }
        env->sockfd = client_fd;
        rt_mutex_release(&g_tran_mutex);

        LOG_I("socket accept success, fd %d", client_fd);
        tran_env_init(env);
    }

    rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
    env->accept_thread = NULL;
    rt_mutex_release(&g_tran_mutex);
    LOG_I("exit accept thread!");
}

/* Used by the protocol engine to tell whether the TCP channel is up
 * (see middleware/proto_v3/proto_v3_link.c) */
uint8_t ota_ext_pan_is_active(void)
{
    return (g_wifi_tran_env.sockfd >= 0) ? 1 : 0;
}


uint8_t ota_ext_is_wifi(void)
{
    return 1;
}

int ota_ext_pan_send_data(const void *data, uint32_t data_len)
{
    ota_wifi_tran_env_t *env = wifi_tran_get_env();
    int ret;

    if (!data || !data_len)
    {
        return -1;
    }
    if (env->sockfd < 0)
    {
        return -1;
    }

    rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
    ret = send(env->sockfd, data, data_len, 0);
    rt_mutex_release(&g_tran_mutex);

    if (env->log_on)
    {
        LOG_I("socket send data len %d", ret);
    }
    return ret;
}

static int ota_ext_pan_close_internal(bool close_listener)
{
    ota_wifi_tran_env_t *env = wifi_tran_get_env();
    rt_thread_t self = rt_thread_self();

    rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
    if (env->recv_thread)
    {
        if (env->recv_thread == self)
        {
            env->recv_thread = NULL;
        }
        else
        {
            rt_thread_delete(env->recv_thread);
            env->recv_thread = NULL;
        }
    }
    if (env->assemble_thread)
    {
        if (env->assemble_thread == self)
        {
            env->assemble_thread = NULL;
        }
        else
        {
            rt_thread_delete(env->assemble_thread);
            env->assemble_thread = NULL;
        }
    }
    if (close_listener && env->listen_fd >= 0)
    {
        close(env->listen_fd);
        env->listen_fd = -1;
    }
    if (close_listener && env->accept_thread)
    {
        if (env->accept_thread == self)
        {
            env->accept_thread = NULL;
        }
        else
        {
            rt_thread_delete(env->accept_thread);
            env->accept_thread = NULL;
        }
    }
    env->status = OTA_EXT_PAN_STATUS_CLOSED;
    if (env->sockfd >= 0)
    {
        LOG_I("close sockfd ret:%d", close(env->sockfd));
        env->sockfd = -1;
    }
    env->total_drop_frames = 0;
    env->total_recv_frames = 0;
    tran_env_deinit(env);
    rt_mutex_release(&g_tran_mutex);

    /* Notify the upper layer outside the lock (the protocol callback may enter
     * the transmit path) */
    proto_v3_rx_closed();
    LOG_I("close_listener %d", close_listener);
    return 0;
}

int ota_ext_pan_close(void)
{
    return ota_ext_pan_close_internal(false);
}

int ota_ext_pan_server_close(void)
{
    return ota_ext_pan_close_internal(true);
}

int ota_ext_start_listen(const char *ip, const char *port)
{
    ota_wifi_tran_env_t *env = wifi_tran_get_env();
    struct addrinfo hints, *server_res = NULL;
    int32_t listen_fd = -1;
    int ret = -1;
    const char *listen_ip = (ip && ip[0]) ? ip : NULL;
    const char *listen_port = (port && port[0]) ? port : server_port;

    LOG_I("start listen %s:%s", listen_ip ? listen_ip : "0.0.0.0", listen_port);

    rt_mutex_take(&g_tran_mutex, RT_WAITING_FOREVER);
    if (env->sockfd >= 0)
    {
        LOG_E("already has active connection fd %d", env->sockfd);
        goto exit;
    }
    if (env->accept_thread)
    {
        LOG_E("accept_thread already exist");
        goto exit;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    ret = getaddrinfo(listen_ip, listen_port, &hints, &server_res);
    if (ret != 0)
    {
        LOG_E("TCP listen unresolved addr %d", ret);
        ret = -1;
        goto exit;
    }
    listen_fd = socket(server_res->ai_family, server_res->ai_socktype, server_res->ai_protocol);
    if (listen_fd < 0)
    {
        LOG_E("listen socket create error, %d", listen_fd);
        ret = -1;
        goto exit;
    }
    int reuse_addr = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

    ret = bind(listen_fd, (struct sockaddr *)server_res->ai_addr, server_res->ai_addrlen);
    if (ret < 0)
    {
        LOG_E("socket bind fail, ret %d", ret);
        goto exit;
    }
    freeaddrinfo(server_res);
    server_res = NULL;

    ret = listen(listen_fd, TCP_BACKLOG);
    if (ret < 0)
    {
        LOG_E("socket listen fail, ret %d", ret);
        goto exit;
    }
    if (env->accept_thread)
    {
        LOG_E("accept_thread already exist");
        ret = -1;
        goto exit;
    }
    env->accept_thread = rt_thread_create("wifi_acc", ota_ext_pan_accept_entry, env, 2048,
                                          RT_THREAD_PRIORITY_MIDDLE, RT_THREAD_TICK_DEFAULT);
    if (!env->accept_thread)
    {
        LOG_I("create accept thread failed");
        ret = -1;
        goto exit;
    }
    env->listen_fd = listen_fd;
    listen_fd = -1;
    rt_thread_startup(env->accept_thread);
    rt_mutex_release(&g_tran_mutex);
    LOG_I("start listen finish");
    return 0;

exit:
    if (server_res)
    {
        freeaddrinfo(server_res);
    }
    if (listen_fd >= 0)
    {
        close(listen_fd);
    }
    rt_mutex_release(&g_tran_mutex);
    return -1;
}

const char *ota_ext_get_server_ip(void)
{
    return server_ip;
}

const char *ota_ext_get_server_port(void)
{
    return server_port;
}

int ota_wifi_tran_start(const char *ssid, const char *pwd, uint16_t port)
{
    rt_err_t err;
    char port_str[PORT_MAX_LEN];

    if (!ssid || !pwd)
    {
        return -1;
    }

#ifdef USING_SCREEN_CAST_WIFI_AP
    /* SoftAP hotspot mode */
    err = rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP);
    if (err != RT_EOK)
    {
        LOG_E("set AP mode failed %d", err);
        return -2;
    }
    err = rt_wlan_start_ap(ssid, pwd);
    if (err != RT_EOK)
    {
        LOG_E("start ap failed %d", err);
        return -3;
    }
    LOG_I("softap up: ssid=%s", ssid);
#else
    /* P2P mode */
    if (!rt_wlan_p2p_go_is_active())
    {
        /* The AP device has to be in the AP role first, then P2P GO can be
         * started on top of it */
        err = rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP);
        if (err != RT_EOK)
        {
            LOG_E("set AP mode failed %d", err);
            return -2;
        }
        err = rt_wlan_p2p_go_start(ssid, pwd);
        if (err != RT_EOK)
        {
            LOG_E("p2p go start failed %d", err);
            return -3;
        }
    }
    LOG_I("p2p go up: ssid=%s", ssid);
#endif

    rt_snprintf(port_str, sizeof(port_str), "%d", port);
    if (ota_ext_start_listen(NULL, port_str) != 0)
    {
        LOG_E("tcp listen failed");
        return -4;
    }
    return 0;
}

#ifdef RT_USING_FINSH
int wifi_tran(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage:\n");
        rt_kprintf("  wifi_tran log <0/1>       - tx/rx log switch\n");
        rt_kprintf("  wifi_tran srv <ip> <port> - start TCP server\n");
        rt_kprintf("  wifi_tran close           - close TCP server and connection\n");
        return -1;
    }
    if (!strcmp(argv[1], "log") && argc >= 3)
    {
        g_wifi_tran_env.log_on = atoi(argv[2]);
        rt_kprintf("wifi_tran log %d\n", g_wifi_tran_env.log_on);
    }
    else if (!strcmp(argv[1], "srv") && argc >= 4)
    {
        ota_ext_start_listen(argv[2], argv[3]);
    }
    else if (!strcmp(argv[1], "close"))
    {
        ota_ext_pan_server_close();
    }
    return 0;
}
MSH_CMD_EXPORT(wifi_tran, wifi cast transport: log/srv/close);
#endif

static int wifi_tran_init(void)
{
    g_wifi_tran_env.sockfd = -1;
    g_wifi_tran_env.listen_fd = -1;
    rt_mutex_init(&g_tran_mutex, "wifi_tran", RT_IPC_FLAG_FIFO);
    rt_sem_init(&g_recv_sem, "wifi_recv", 0, RT_IPC_FLAG_FIFO);
    return 0;
}
INIT_APP_EXPORT(wifi_tran_init);
