/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include "iph_devcert.h"
#include "iph_pair_identity.h"
#include "iph_ld.h"           /* iph_b64_decode */

#include <string.h>

#include "usbh_core.h"         

#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/asn1write.h>

#undef USB_DBG_TAG
#define USB_DBG_TAG "iph_dc"
#include "usb_log.h"

#define DEVCERT_TMP_SIZE 1200
#define DEVPUB_PEM_MAX   800

static int iph_pair_rng(void *p_rng, unsigned char *out, size_t len)
{
    static uint32_t s = 0x9E3779B9u;
    size_t i;

    (void)p_rng;

    for (i = 0; i < len; i++)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        out[i] = (unsigned char)(s >> 24);
    }

    return 0;
}

static int iph_pem_to_spki(const char *pem, uint32_t pem_len,
                           uint8_t *out, uint32_t out_max, uint32_t *out_len)
{
    /* SEQUENCE(0x122) { SEQUENCE{ OID rsaEncryption, NULL }, BIT STRING(0x10F) } */
    static const uint8_t rsa2048_spki_prefix[] = {
        0x30, 0x82, 0x01, 0x22,
        0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01, 0x05, 0x00,
        0x03, 0x82, 0x01, 0x0F, 0x00
    };
    char b64[DEVPUB_PEM_MAX];
    uint8_t der[400];
    uint32_t i = 0;
    uint32_t n = 0;
    int dlen;

    /* Skip the "-----BEGIN ...-----" line */
    while ((i < pem_len) && (pem[i] != '\n'))
    {
        i++;
    }
    i++;

    /* Collect base64 characters; a '-' marks the "-----END" line */
    while ((i < pem_len) && (pem[i] != '-'))
    {
        char c = pem[i++];

        if ((c != '\r') && (c != '\n') && (c != ' ') && (c != '\t'))
        {
            if ((n + 1) >= sizeof(b64))
            {
                return -1;
            }
            b64[n++] = c;
        }
    }

    dlen = iph_b64_decode(b64, n, der, sizeof(der));
    if (dlen <= 0)
    {
        USB_LOG_ERR("devcert: base64 decode failed (%d)\r\n", dlen);
        return -1;
    }

    if ((dlen >= 5) && (der[0] == 0x30) && (der[4] == 0x02))
    {
        /* PKCS#1 RSAPublicKey: wrap it into SPKI */
        if (dlen != 270)
        {
            USB_LOG_ERR("devcert: unexpected rsa pubkey len %d (expect 270)\r\n", dlen);
            return -1;
        }
        if ((sizeof(rsa2048_spki_prefix) + 270u) > out_max)
        {
            return -1;
        }

        memcpy(out, rsa2048_spki_prefix, sizeof(rsa2048_spki_prefix));
        memcpy(out + sizeof(rsa2048_spki_prefix), der, 270);
        *out_len = (uint32_t)(sizeof(rsa2048_spki_prefix) + 270);
    }
    else if ((dlen >= 5) && (der[0] == 0x30) && (der[4] == 0x30))
    {
        /* Already a SubjectPublicKeyInfo */
        if ((uint32_t)dlen > out_max)
        {
            return -1;
        }

        memcpy(out, der, (size_t)dlen);
        *out_len = (uint32_t)dlen;
    }
    else
    {
        USB_LOG_ERR("devcert: unknown pubkey der (len %d, first %02x %02x)\r\n",
                    dlen, der[0], der[4]);
        return -1;
    }

    USB_LOG_INFO("devcert: device pubkey der %d -> spki %u bytes\r\n", dlen, (unsigned)*out_len);

    return 0;
}

static int dev_set_basic_constraints_ca_false(mbedtls_x509write_cert *ctx)
{
    unsigned char buf[8];
    unsigned char *c = buf + sizeof(buf);
    size_t len = 0;
    int ret;

    memset(buf, 0, sizeof(buf));

    /* BasicConstraints ::= SEQUENCE { }  -- an empty sequence means CA:FALSE */
    ret = mbedtls_asn1_write_len(&c, buf, len);
    if (ret < 0)
    {
        return ret;
    }
    len += (size_t)ret;

    ret = mbedtls_asn1_write_tag(&c, buf, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    if (ret < 0)
    {
        return ret;
    }
    len += (size_t)ret;

    return mbedtls_x509write_crt_set_extension(ctx, MBEDTLS_OID_BASIC_CONSTRAINTS,
                                               MBEDTLS_OID_SIZE(MBEDTLS_OID_BASIC_CONSTRAINTS),
                                               1 /* critical */,
                                               buf + sizeof(buf) - len, len);
}

int iph_devcert_build(const char *dev_pub_pem, uint32_t pem_len,
                      uint8_t *out_der, uint32_t out_max)
{
    mbedtls_pk_context dev_pk;
    mbedtls_pk_context root_pk;
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;
    uint8_t spki[400];
    uint32_t spki_len = 0;
    uint8_t tmp[DEVCERT_TMP_SIZE];
    int ret = -1;
    int n;

    if ((dev_pub_pem == NULL) || (out_der == NULL) || (pem_len == 0) || (pem_len >= DEVPUB_PEM_MAX))
    {
        return -1;
    }

    mbedtls_pk_init(&dev_pk);
    mbedtls_pk_init(&root_pk);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);

    /* 1) Device public key: PEM -> SPKI DER */
    if (iph_pem_to_spki(dev_pub_pem, pem_len, spki, sizeof(spki), &spki_len) != 0)
    {
        USB_LOG_ERR("devcert: decode device pubkey failed\r\n");
        goto cleanup;
    }

    n = mbedtls_pk_parse_public_key(&dev_pk, spki, spki_len);
    if (n != 0)
    {
        USB_LOG_ERR("devcert: parse device pubkey failed -0x%04x\r\n", (unsigned)-n);
        goto cleanup;
    }

    /* 2) Built-in root private key (PKCS#1 RSAPrivateKey DER) */
    n = mbedtls_pk_parse_key(&root_pk, iph_root_key_der, IPH_ROOT_KEY_DER_LEN, NULL, 0);
    if (n != 0)
    {
        USB_LOG_ERR("devcert: parse root key failed -0x%04x\r\n", (unsigned)-n);
        goto cleanup;
    }

    /* 3) Build the certificate */
    n = mbedtls_mpi_lset(&serial, 1);
    if (n != 0)
    {
        goto cleanup;
    }

    mbedtls_x509write_crt_set_version(&crt, 2);   /* 2 = v3 */
    mbedtls_x509write_crt_set_serial(&crt, &serial);
    mbedtls_x509write_crt_set_subject_key(&crt, &dev_pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &root_pk);
    mbedtls_x509write_crt_set_subject_key_identifier(&crt);
    n = dev_set_basic_constraints_ca_false(&crt);
    if (n != 0)
    {
        USB_LOG_ERR("devcert: set basicConstraints failed -0x%04x\r\n", (unsigned)-n);
        goto cleanup;
    }
    mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                        MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20440101000000");
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    n = mbedtls_x509write_crt_der(&crt, tmp, sizeof(tmp), iph_pair_rng, NULL);
    if (n <= 0)
    {
        USB_LOG_ERR("devcert: write der failed -0x%04x\r\n", (unsigned)-n);
        goto cleanup;
    }
    if ((uint32_t)n > out_max)
    {
        USB_LOG_ERR("devcert: der %d bytes > out_max %u\r\n", n, (unsigned)out_max);
        goto cleanup;
    }

    /* mbedtls writes the DER at the end of the buffer, so the data starts at tmp + sizeof(tmp) - n */
    memcpy(out_der, tmp + sizeof(tmp) - n, (size_t)n);
    USB_LOG_INFO("devcert: built %d bytes (device pubkey %u pem bytes)\r\n",
                 n, (unsigned)pem_len);
    ret = n;

cleanup:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_mpi_free(&serial);
    mbedtls_pk_free(&root_pk);
    mbedtls_pk_free(&dev_pk);

    return ret;
}
