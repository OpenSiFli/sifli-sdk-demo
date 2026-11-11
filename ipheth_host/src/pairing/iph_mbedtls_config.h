/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#ifndef IPH_MBEDTLS_CONFIG_H
#define IPH_MBEDTLS_CONFIG_H

#include "mbedtls/config.h"

#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_WRITE_C

/* The subjectKeyIdentifier extension relies on SHA1 */
#define MBEDTLS_SHA1_C

#endif /* IPH_MBEDTLS_CONFIG_H */
