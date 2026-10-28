/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file screen_cast_proto.c
 * @brief Screen cast binding on top of the V3 protocol component.
 *
 * This file is the *cast side* of the V3 channel: it answers the handshake,
 * feeds the continual-file stream into the frame demultiplexer
 * (@ref screen_cast_proc.h) and keeps the supervision timer alive while frames
 * are arriving.
 *
 * It talks to middleware/proto_v3 only - link, message layout, response
 * builders and supervision all live there. Nothing from the firmware-update
 * engine is referenced, which is what lets this example build with
 * CONFIG_BSP_USING_DFU turned off.
 */

#include "screen_cast.h"

#include <rtthread.h>
#include <string.h>

#include "proto_v3.h"
#include "proto_v3_msg.h"
#include "proto_v3_link.h"

#include "bf0_ble_gap.h"

#ifdef BSP_BLE_SERIAL_TRANSMISSION
    #include "bf0_sibles_serial_trans_service.h"
#endif

#define LOG_TAG     "SC_PROTO"
#include "log.h"

/* -------------------------------------------------------------------------
 * Wire parameters
 *
 * Packet sizes and answer cadence offered to the phone. The values differ per
 * transport: a slow phone cannot take 4 KB messages, and SPP / PAN / TCP carry
 * more per packet than a plain BLE notification.
 * ---------------------------------------------------------------------- */

#define SC_PKT_LEN_CONTINUAL            4096
#define SC_PKT_LEN_CONTINUAL_SLOW       1024
#define SC_PKT_LEN_CONTINUAL_SPP        10240
#define SC_PKT_LEN_CONTINUAL_PAN        10240
#define SC_PKT_LEN_CONTINUAL_WIFI       20480

/* Image packets never occur in a cast session; the field is still on the wire
 * and keeps the value a firmware-update peer would expect (512 bytes of data
 * plus a 32 byte hash and a 4 byte index). */
#define SC_PKT_LEN_BIN                  548

#define SC_NUM_OF_RSP_BIN               20
#define SC_NUM_OF_RSP_BIN_SLOW          4
#define SC_NUM_OF_RSP_BIN_SPP_NOR       200

/* Reassembly error reported back to the peer on the V3 channel. */
#define SC_SERIAL_TRANS_ERROR_ASSEMABLE 1

/* Minimum payload lengths, header excluded. */
#define SC_VERSION_REQ_LEN              4
#define SC_FILE_INIT_REQ_LEN            30      /* version 2 + type 2 + count 4
                                                 * + length 4 + md5 16
                                                 * + phone type 1 + align 1 */
#define SC_CONTINUAL_PKT_HEAD_LEN       12      /* file len 4 + index 4
                                                 * + data len 2 + type 1
                                                 * + reserved 1 */

static struct
{
    uint16_t type;              /**< OTA type announced by the phone          */
    uint32_t remote_version;    /**< remote (phone) OTA code version          */
    uint8_t  phone_type;        /**< SC_PHONE_TYPE_*                          */
    uint8_t  file_aligned;      /**< frame is 4-byte aligned + trailing CRC   */
    uint8_t  is_lose_check;     /**< drop stale packets after a link error    */
} g_sc;

/* -------------------------------------------------------------------------
 * Version negotiation
 * ---------------------------------------------------------------------- */

static void sc_proto_version_request(uint8_t *data, uint16_t len)
{
    dfu_version_request_t req;

    if (len < SC_VERSION_REQ_LEN)
    {
        LOG_W("short version request %d", len);
        return;
    }

    memcpy(&req.ota_version, data + 0, 2);
    memcpy(&req.type, data + 2, 2);

    g_sc.type = req.type;
    g_sc.remote_version = req.ota_version;

    LOG_I("version req type %d, remote %d", req.type, req.ota_version);

    sc_proc_on_version(req.ota_version);

    proto_v3_rsp_version(PROTO_V3_RESULT_OK, 0);
}

/* -------------------------------------------------------------------------
 * File (frame) negotiation
 * ---------------------------------------------------------------------- */

/* Size of one stream packet the peer should send us. */
static uint16_t sc_proto_stream_packet_len(uint8_t phone_type)
{
    if (phone_type == SC_PHONE_TYPE_IOS_OLD)
    {
        return SC_PKT_LEN_CONTINUAL_SLOW;
    }

    if (phone_type == SC_PHONE_TYPE_ANDROID_SPP)
    {
        return SC_PKT_LEN_CONTINUAL_SPP;
    }

    if (phone_type == SC_PHONE_TYPE_BT_PAN)
    {

        if (ble_serial_tran_type_get() == SERIAL_TRANS_TYPE_WIFI)
        {
            return SC_PKT_LEN_CONTINUAL_WIFI;
        }

        return SC_PKT_LEN_CONTINUAL_PAN;
    }

    return SC_PKT_LEN_CONTINUAL;
}

/* Number of image packets between two answers. A cast session never sends
 * images, the field only has to match what the peer expects. */
static uint8_t sc_proto_num_of_rsp_bin(uint8_t phone_type)
{
    uint8_t num = SC_NUM_OF_RSP_BIN;

    if (phone_type == SC_PHONE_TYPE_IOS_OLD || phone_type == SC_PHONE_TYPE_HARMONY)
    {
        num = SC_NUM_OF_RSP_BIN_SLOW;
    }

#ifdef BSP_BLE_SERIAL_TRANSMISSION
    if (ble_serial_tran_type_get() == SERIAL_TRANS_TYPE_SPP)
    {
        num = SC_NUM_OF_RSP_BIN_SPP_NOR;
    }
#endif

    return num;
}

/* The phone announces a bare BLE / iOS / Android phone type; what actually
 * arrives tells us whether it is really SPP or PAN. */
static uint8_t sc_proto_remap_phone_type(uint8_t phone_type)
{
#ifdef BSP_BLE_SERIAL_TRANSMISSION
    uint8_t tran_type = ble_serial_tran_type_get();

    if ((phone_type == SC_PHONE_TYPE_ANDROID || phone_type == SC_PHONE_TYPE_DEVICE)
            && tran_type == SERIAL_TRANS_TYPE_SPP)
    {
        LOG_I("using spp");
        return SC_PHONE_TYPE_ANDROID_SPP;
    }

    if (tran_type == SERIAL_TRANS_TYPE_PAN || tran_type == SERIAL_TRANS_TYPE_WIFI)
    {
        LOG_I("using pan");
        return SC_PHONE_TYPE_BT_PAN;
    }
#endif

    return phone_type;
}

static void sc_proto_file_init_request(uint8_t *data, uint16_t len)
{
    proto_v3_rsp_file_init_t rsp;
    uint8_t phone_type;
    uint8_t file_align;

    if (len < SC_FILE_INIT_REQ_LEN)
    {
        LOG_W("short file init request %d", len);
        return;
    }

    memcpy(&phone_type, data + 28, 1);
    memcpy(&file_align, data + 29, 1);

    phone_type = sc_proto_remap_phone_type(phone_type);

    g_sc.phone_type = phone_type;
    g_sc.file_aligned = file_align;

    memset(&rsp, 0, sizeof(rsp));
    rsp.result = PROTO_V3_RESULT_OK;
    rsp.resume_state = 0;
    rsp.resume_index = 0;
    rsp.fs_block = 0;
    rsp.num_of_rsp_file = (uint8_t)sc_proc_on_init_req(phone_type);
    rsp.num_of_rsp_bin = sc_proto_num_of_rsp_bin(phone_type);
    rsp.packet_length_file = sc_proto_stream_packet_len(phone_type);
    rsp.packet_length_bin = SC_PKT_LEN_BIN;

    LOG_I("file init phone %d, rsp file %d, pkt %d", phone_type,
          rsp.num_of_rsp_file, rsp.packet_length_file);

    proto_v3_rsp_file_init(&rsp);
}

static void sc_proto_file_init_completed(void)
{
    g_sc.is_lose_check = 0;
    proto_v3_rsp_file_init_completed(PROTO_V3_RESULT_OK, 0);
}

/* -------------------------------------------------------------------------
 * Frame transfer
 * ---------------------------------------------------------------------- */

static void sc_proto_file_start_request(uint8_t *data, uint16_t len)
{
    uint16_t file_type;

    if (len < 2)
    {
        LOG_W("short file start request %d", len);
        return;
    }

    memcpy(&file_type, data, 2);

    /* A cast session only ever carries resource files; anything else belongs
     * to a firmware update and is not this application's business. */
    if (file_type != SC_FILE_TYPE_RES && file_type != SC_FILE_TYPE_RES_WITHOUT_PATH)
    {
        LOG_W("unexpected file type %d", file_type);
        return;
    }

    sc_proc_file_start();


    proto_v3_sync_start(PROTO_V3_SYNC_CONTINUAL_FILE);

    proto_v3_rsp_file_start(PROTO_V3_RESULT_OK);
}

static void sc_proto_continual_packet(uint8_t *data, uint16_t len)
{
    sc_recv_t recv;
    uint32_t file_len;
    uint32_t index;
    uint16_t data_len;
    uint8_t packet_type;
    uint8_t reserved;
    uint8_t need_rsp;
    uint8_t new_packet;

    if (len < SC_CONTINUAL_PKT_HEAD_LEN)
    {
        LOG_W("short continual packet %d", len);
        return;
    }

    memcpy(&file_len, data + 0, 4);
    memcpy(&index, data + 4, 4);
    memcpy(&data_len, data + 8, 2);
    packet_type = data[10];
    reserved = data[11];

    need_rsp = reserved & 1;
    new_packet = (reserved & 2) >> 1;

    if (g_sc.is_lose_check)
    {
        if (new_packet == 0)
        {
            /* The reassembly buffer was dropped; whatever was already queued
             * belongs to the broken frame. */
            return;
        }

        LOG_I("new packet, rsp %d", need_rsp);
        g_sc.is_lose_check = 0;
    }

    if ((uint32_t)SC_CONTINUAL_PKT_HEAD_LEN + data_len > len)
    {
        LOG_W("truncated continual packet %d + %d > %d",
              SC_CONTINUAL_PKT_HEAD_LEN, data_len, len);
        return;
    }

    /* A frame arrives every few tens of milliseconds; anything longer than the
     * supervision timeout means the phone went away. */
    proto_v3_sync_end();
    proto_v3_sync_start(PROTO_V3_SYNC_CONTINUAL_FILE);

    recv.packet_data = data + SC_CONTINUAL_PKT_HEAD_LEN;
    recv.packet_len = data_len;
    recv.index = index;
    recv.packet_type = packet_type;
    recv.file_size = file_len;
    recv.file_aligned = g_sc.file_aligned;
    recv.phone_type = g_sc.phone_type;
    recv.remote_version = g_sc.remote_version;

    sc_proc_on_packet(&recv);

    if (need_rsp)
    {
        uint8_t mode = sc_get_switch_mode();

        if (mode != 0)
        {
            proto_v3_rsp_continual_mode(mode, 0, 0);
            sc_set_switch_mode(0);
        }
        else
        {
            proto_v3_rsp_file_packet(PROTO_V3_RESULT_OK, 0);
        }
    }
}

static void sc_proto_file_end_request(uint8_t *data, uint16_t len)
{
    dfu_file_end_request_ext_t req;

    if (len < sizeof(req))
    {
        LOG_W("short file end request %d", len);
        return;
    }

    memcpy(&req, data, sizeof(req));

    if (req.file_type != SC_FILE_TYPE_RES && req.file_type != SC_FILE_TYPE_RES_WITHOUT_PATH)
    {
        return;
    }

    proto_v3_sync_end();

    sc_proc_file_end();

    proto_v3_rsp_file_end(PROTO_V3_RESULT_OK);
}

static void sc_proto_total_end_request(void)
{
    sc_proc_session_end();

    proto_v3_rsp_file_total_end(PROTO_V3_RESULT_OK);
}

/* -------------------------------------------------------------------------
 * Link callbacks
 * ---------------------------------------------------------------------- */

static void sc_proto_on_msg(proto_v3_msg_t *msg, uint16_t buf_len)
{
    (void)buf_len;

    switch (msg->message_id)
    {
    case PROTO_V3_VERSION_REQUEST:
    {
        sc_proto_version_request(msg->data, msg->length);
        break;
    }
    case PROTO_V3_FILE_INIT_REQUEST:
    {
        sc_proto_file_init_request(msg->data, msg->length);
        break;
    }
    case PROTO_V3_FILE_INIT_COMPLETED_REQUEST:
    {
        sc_proto_file_init_completed();
        break;
    }
    case PROTO_V3_FILE_START_REQUEST:
    {
        sc_proto_file_start_request(msg->data, msg->length);
        break;
    }
    case PROTO_V3_FILE_CONTINUAL_PACKET:
    {
        sc_proto_continual_packet(msg->data, msg->length);
        break;
    }
    case PROTO_V3_FILE_END_REQUEST:
    {
        sc_proto_file_end_request(msg->data, msg->length);
        break;
    }
    case PROTO_V3_FILE_TOTAL_END_REQUEST:
    {
        sc_proto_total_end_request();
        break;
    }
    case PROTO_V3_LINK_LOSE_CHECK_RSP:
    {
        /* The peer answered our supervision request; the link is alive. */
        g_sc.is_lose_check = 0;
        proto_v3_sync_end();
        break;
    }
    default:
    {
        LOG_I("ignore message %d", msg->message_id);
        break;
    }
    }
}

static void sc_proto_on_close(void)
{
    proto_v3_sync_end();
    g_sc.is_lose_check = 0;
    sc_proc_session_end();
}

static void sc_proto_on_error(uint8_t error)
{
    proto_v3_sync_end();

#ifdef BSP_BLE_SERIAL_TRANSMISSION
    if (error == BLE_SERIAL_TRAN_ERROR_ASSEMBLE_ERROR
            || error == BLE_SERIAL_TRAN_ERROR_ASSEMBLE_OVER_LEN)
    {
        g_sc.is_lose_check = 1;
        proto_v3_rsp_continual_error(SC_SERIAL_TRANS_ERROR_ASSEMABLE);
    }
#else
    (void)error;
#endif

    sc_proc_on_packet_error();
}

static void sc_proto_on_tx_done(void)
{
    /* A cast frame is dropped rather than retried, nothing to release here. */
}

/* The supervision timer fired: the phone stopped in the middle of a frame. */
static void sc_proto_on_sync_timeout(uint8_t sync_type)
{
    if (sync_type == PROTO_V3_SYNC_CONTINUAL_FILE)
    {
        LOG_W("frame stream went quiet");

        proto_v3_sync_end();
        g_sc.is_lose_check = 1;

        proto_v3_rsp_continual_error(SC_SERIAL_TRANS_ERROR_ASSEMABLE);
        sc_proc_on_packet_error();

        /* Give the phone a longer window to pick the stream back up. */
        proto_v3_sync_start(PROTO_V3_SYNC_CONTINUAL_FILE_LOST);
        return;
    }

    if (sync_type == PROTO_V3_SYNC_CONTINUAL_FILE_LOST)
    {
        LOG_W("frame stream remote lost");

        proto_v3_sync_end();
        g_sc.is_lose_check = 0;
        sc_proc_err();
    }
}

static const proto_v3_handler_t g_sc_proto_handler =
{
    .on_msg     = sc_proto_on_msg,
    .on_close   = sc_proto_on_close,
    .on_error   = sc_proto_on_error,
    .on_tx_done = sc_proto_on_tx_done,
};

/* Nothing is ever installed: the payload is a picture, not a firmware image. */
static int sc_proto_on_file_data(uint8_t img_id, uint32_t offset, uint8_t *data, uint32_t len)
{
    (void)img_id;
    (void)offset;
    (void)data;
    (void)len;
    return 0;
}

static int sc_proto_on_install(void)
{
    return 0;
}

static uint8_t sc_proto_is_stream(void)
{
    return 1;
}

static const proto_v3_sink_t g_sc_proto_sink =
{
    .on_file_data = sc_proto_on_file_data,
    .on_install = sc_proto_on_install,
    .is_stream = sc_proto_is_stream,
};

/* -------------------------------------------------------------------------
 * Application entry points
 * ---------------------------------------------------------------------- */

void screen_cast_init(void)
{
    rt_memset(&g_sc, 0, sizeof(g_sc));
    sc_proc_init();

    proto_v3_set_handler(&g_sc_proto_handler);
    proto_v3_set_sink(&g_sc_proto_sink);
    proto_v3_sync_set_timeout_cb(sc_proto_on_sync_timeout);

    LOG_I("screen cast bound to the V3 protocol component");
}

void screen_cast_notify_disconnect(void)
{
    sc_proc_session_end();
}

int sc_build_connect_qr(char *buf, size_t buf_size, uint16_t width, uint16_t height, uint8_t transport)
{
    bd_addr_t addr;
    char mac[13];

    if (buf == NULL || buf_size == 0)
    {
        return -1;
    }

    /* The public address is only valid once the BLE stack has been powered on;
     * the caller is expected to retry until this succeeds. */
    if (ble_get_public_address(&addr) != HL_ERR_NO_ERROR)
    {
        LOG_I("public address not available yet");
        return -1;
    }

    rt_snprintf(mac, sizeof(mac), "%02x%02x%02x%02x%02x%02x",
                addr.addr[0], addr.addr[1], addr.addr[2],
                addr.addr[3], addr.addr[4], addr.addr[5]);

    return rt_snprintf(buf, buf_size, "Tt:%d;Mac:%s;Ql:%s;Sz:%dx%d;Tp:%d",
                       transport, mac, SC_CAST_QR_JPEG_QUALITY, width, height, SC_CAST_NAV_TYPE);
}

/* WiFi connection QR code: the "WIFI:" prefix is the standard join-network
 * payload understood by phone cameras; the part after "|" carries the cast
 * parameters (Tt=8 selects the WiFi transport) and Ip/Pt give the device TCP
 * server address. */
int sc_build_wifi_qr(char *buf, size_t buf_size, uint16_t width, uint16_t height)
{
    if (buf == NULL || buf_size == 0)
    {
        return -1;
    }

    return rt_snprintf(buf, buf_size, "WIFI:T:WPA;S:%s;P:%s;;|Tt:%d;Ql:%s;Sz:%dx%d;Tp:%d;Ip:%s;Pt:%d",
                       SC_WIFI_GO_SSID, SC_WIFI_GO_PWD,
                       SC_CAST_TYPE_WIFI, SC_CAST_QR_JPEG_QUALITY, width, height,
                       SC_CAST_NAV_TYPE, SC_WIFI_SERVER_IP, SC_WIFI_SERVER_PORT);
}
