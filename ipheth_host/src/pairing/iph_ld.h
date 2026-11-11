/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#ifndef IPH_LD_H
#define IPH_LD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPH_LD_LOCKDOWN_PORT 62078

/**
 * @brief Open the usbmux (Apple Mobile Device) interface and negotiate mux version 1
 * @return 0 on success, <0 on failure
 */
int iph_ld_open(void);

/** @brief Tear down the mux session (nothing needs to be sent; re-enumeration restores it) */
void iph_ld_close(void);

/**
 * @brief Open a TCP connection over mux (SYN / SYN-ACK / ACK handled internally)
 */
int iph_ld_connect(uint16_t port);

/**
 * @brief Send a block of data (split automatically according to the mux MTU)
 */
int iph_ld_send(const void *data, uint32_t len);

/**
 * @brief Receive data (control packets and pure ACKs are skipped automatically)
 * @param timeout_ms timeout for a single wait
 * @return >0 bytes received; 0 on timeout; <0 on error
 */
int iph_ld_recv(void *buf, uint32_t maxlen, uint32_t timeout_ms);

/**
 * @brief Send a plist request and receive the response (blocking, with retries)
 * @param req   request XML (may be assembled by the caller from several parts)
 * @param resp  response buffer
 * @return >0 response length; <0 on error
 */
int iph_ld_request(const char *req, char *resp, uint32_t resp_max, uint32_t timeout_ms);

/* ---------------- minimal plist / base64 ---------------- */

/** Find the <string> value following <key>key</key>; returns 0 on success */
int iph_plist_get_string(const char *xml, const char *key, char *out, uint32_t out_max);

/** Find the <data> following <key>key</key> and base64-decode it into out; returns decoded length */
int iph_plist_get_data(const char *xml, const char *key, uint8_t *out, uint32_t out_max);

/** base64 encode (with line breaks, standard alphabet); returns written length */
int iph_b64_encode(const uint8_t *in, uint32_t in_len, char *out, uint32_t out_max);

/** base64 decode; returns decoded length */
int iph_b64_decode(const char *in, uint32_t in_len, uint8_t *out, uint32_t out_max);

/**
 * @brief DER certificate -> PEM text ("-----BEGIN CERTIFICATE-----" + 64-column base64 + "-----END")
 *
 * In a lockdown PairRecord all three certificates are stored as **PEM text**
 * (verified against a real host record); sending raw DER makes the device
 * reject the request with InvalidPairRecord.
 *
 * @return >0 PEM length; <0 on failure
 */
int iph_der_to_pem(const uint8_t *der, uint32_t der_len, char *out, uint32_t out_max);

#ifdef __cplusplus
}
#endif

#endif /* IPH_LD_H */
