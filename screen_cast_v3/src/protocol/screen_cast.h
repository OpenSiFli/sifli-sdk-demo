/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file screen_cast.h
 * @brief Screen / map cast service.
 *
 * The companion phone app (SiFli OTA V3 SDK) can push a live screen cast
 * (map navigation, camera preview, screen mirroring ...) to the device over
 * the BLE serial-transmission channel.
 *
 * This component provides the application facing half of that feature:
 *   - @ref screen_cast_proc.h : frame demultiplexer and per-type ring buffers.
 *   - a thin binding on top of the V3 protocol component
 *     (@ref screen_cast_proto.c).
 *
 * The link, the message layout, the response builders and the supervision
 * timer are provided by `middleware/proto_v3` (`CONFIG_PROTO_V3`). The
 * firmware-update engine is not involved, so this example builds with
 * `CONFIG_BSP_USING_DFU` turned off.
 *
 * Typical usage:
 * @code
 *   screen_cast_init();                       // after the BLE stack is up
 *   sc_frame_t *jpeg = sc_frame_acquire(SC_DATA_TYPE_JPEG);
 *   if (jpeg) { render(jpeg->data, jpeg->size); sc_frame_release(SC_DATA_TYPE_JPEG); }
 * @endcode
 */

#ifndef SCREEN_CAST_H
#define SCREEN_CAST_H

#include "screen_cast_proc.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cast transport types advertised to the phone (the `Tt` field).
 *
 * Bit mask, same encoding as solution2.0's `MAP_CAST_TYPE_*`.
 */
#define SC_CAST_TYPE_BLE        0x01
#define SC_CAST_TYPE_SPP        0x02
#define SC_CAST_TYPE_PAN        0x04
#define SC_CAST_TYPE_WIFI       0x08
#define SC_CAST_TYPE_EC         0x10

/** JPEG quality the phone is asked to encode with (the `Ql` field). */
#define SC_CAST_QR_JPEG_QUALITY "0.2"
/** Navigation info version the phone is asked to send (the `Tp` field). */
#define SC_CAST_NAV_TYPE        1

/** WiFi cast defaults (P2P GO + TCP server, see @ref sc_build_wifi_qr). */
#define SC_WIFI_GO_SSID         "DIRECT-SiFli"
#define SC_WIFI_GO_PWD          "12345678"
#define SC_WIFI_SERVER_IP       "192.168.88.1"
#define SC_WIFI_SERVER_PORT     2025

/**
 * @brief Initialise the screen cast service.
 *
 * Initialises the frame buffers and registers the cast callback with the
 * OTA V3 channel (@ref proto_v3_set_handler). Requires `CONFIG_PROTO_V3`,
 * not the firmware-update engine.
 * Transport independent: call it once at application startup, before any
 * transport (BLE / WiFi) can deliver cast data.
 */
void screen_cast_init(void);

/**
 * @brief Build the connection payload shown as a QR code on the device.
 *
 * The companion phone app has no BLE scanner: it only accepts the device
 * address from a scanned QR code and then connects directly. The payload
 * format is the solution2.0 one, for example
 * `Tt:1;Mac:a0b1c2d3e4f5;Ql:0.2;Sz:840x480;Tp:1`
 * where `Tt` selects the transport, `Mac` is the BLE public address,
 * `Ql` the JPEG quality, `Sz` the requested screen geometry and `Tp` the
 * navigation info type.
 *
 * @param[out] buf       destination buffer.
 * @param[in]  buf_size  size of @p buf.
 * @param[in]  width     requested cast width (multiples of 16).
 * @param[in]  height    requested cast height (multiples of 16).
 * @param[in]  transport SC_CAST_TYPE_* bit mask.
 * @return number of characters written, negative on error (for example when
 *         the BLE stack is not powered on yet and the address is unavailable).
 */
int sc_build_connect_qr(char *buf, size_t buf_size, uint16_t width, uint16_t height, uint8_t transport);

/**
 * @brief Build the WiFi connection payload shown as a QR code on the device.
 *
 * Format:
 * `WIFI:T:WPA;S:<ssid>;P:<pwd>;;|Tt:<type>;Ql:<quality>;Sz:<w>x<h>;Tp:<nav>;Ip:<ip>;Pt:<port>`
 * The `WIFI:` prefix is a standard join-network QR payload: the companion app
 * joins the P2P GO hotspot and then connects to the device TCP server at
 * `Ip:Pt`. Unlike the BLE variant it never needs the BLE address, so it
 * succeeds immediately.
 *
 * @param[out] buf       destination buffer.
 * @param[in]  buf_size  size of @p buf.
 * @param[in]  width     requested cast width.
 * @param[in]  height    requested cast height.
 * @return number of characters written, negative on error.
 */
int sc_build_wifi_qr(char *buf, size_t buf_size, uint16_t width, uint16_t height);

/**
 * @brief Tell the service that the BLE link is gone.
 *
 * Ends the ongoing cast session so that the application can go back to the
 * "scan to connect" page. Call it from the BLE disconnected handler.
 */
void screen_cast_notify_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_CAST_H */
