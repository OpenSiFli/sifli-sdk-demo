/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ota_wifi_tran.h
 * @brief TCP relay channel (WiFi variant of the cast transport).
 *
 * Ported from solution2.0 `components/ota/ota_pan/ota_pan_trans.c` (BT PAN
 * version) with the BT specific parts removed: the device runs a TCP server on
 * a P2P GO hotspot, and once the phone connects both directions use the same
 * magic-tagged frame format:
 *
 *   [0x53454346][u32 len][u8 cateID][3B reserve][len bytes data][u32 crc]
 *
 * Received frames are handed to the framed_proto transport layer:
 *   framed_proto_rx_frame(cate_id, payload, len)   (middleware/framed_proto)
 * The transmit direction calls back through the weak hook
 * framed_proto_pan_send_data().
 */

#ifndef OTA_WIFI_TRAN_H
#define OTA_WIFI_TRAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the WiFi cast channel: switch AP to AP mode -> P2P GO hotspot -> TCP server.
 *
 * Includes the full WiFi bring-up (driver load + firmware download). Must be
 * called from a worker thread with at least 8KB of stack.
 *
 * @param ssid  P2P GO hotspot name the phone joins.
 * @param pwd   hotspot password (>= 8 characters).
 * @param port  TCP server port.
 * @return 0 on success, negative on failure (the BLE channel is unaffected).
 */
int ota_wifi_tran_start(const char *ssid, const char *pwd, uint16_t port);

/** @brief Start only the TCP server (called by ota_wifi_tran_start, usable on its own). */
int ota_ext_start_listen(const char *ip, const char *port);

/** @brief Send one already framed block; called back by the serial-transmission service. */
int framed_proto_pan_send_data(const void *data, uint32_t data_len);

/** @brief Close the current TCP connection, keeping the listener for a reconnect. */
int ota_ext_pan_close(void);

/** @brief Close both the TCP connection and the listener. */
int ota_ext_pan_server_close(void);

/** @brief Debug helper: read the configured server ip / port. */
const char *ota_ext_get_server_ip(void);
const char *ota_ext_get_server_port(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_WIFI_TRAN_H */
