/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#ifndef IPH_DEVCERT_H
#define IPH_DEVCERT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPH_DEVCERT_MAX_SIZE 1024

/**
 * @brief Build the DeviceCertificate
 *
 * @param dev_pub_pem  PEM text returned by GetValue(DevicePublicKey)
 *                     ("BEGIN RSA PUBLIC KEY" or "BEGIN PUBLIC KEY")
 * @param pem_len      Length of the PEM text (trailing NUL is optional)
 * @param out_der      Output buffer for the DER-encoded certificate
 * @param out_max      Size of the output buffer, IPH_DEVCERT_MAX_SIZE is recommended
 *
 * @return >0 actual DER length; <0 on failure (low bits of the mbedtls error code)
 */
int iph_devcert_build(const char *dev_pub_pem, uint32_t pem_len,
                      uint8_t *out_der, uint32_t out_max);

#ifdef __cplusplus
}
#endif

#endif /* IPH_DEVCERT_H */
