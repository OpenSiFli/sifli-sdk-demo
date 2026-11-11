/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include "usbh_core.h"
#include "usbh_ipheth.h"
#include "iph_ld.h"
#include "iph_devcert.h"
#include "iph_pair.h"
#include "iph_pair_identity.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "iph_pair"
#include "usb_log.h"

#define PAIR_THREAD_STACK 8192
#define PAIR_THREAD_PRIO  20

/* Maximum time to wait for the "Trust This Computer?" dialog on the phone */
#define PAIR_DIALOG_TIMEOUT_MS 60000

/* Retry cadence while the phone is locked: once every 3 seconds, up to 3 minutes. */
#define PAIR_LOCKED_RETRY_MS 3000
#define PAIR_LOCKED_WAIT_MS  180000

/* plist prefix identical to a real host (iTunes/AMDS):
 * every request carries ProtocolVersion=2, a DOCTYPE, and no Label. */
#define XML_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" \
                 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" " \
                 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" \
                 "<plist version=\"1.0\"><dict>" \
                 "<key>ProtocolVersion</key><string>2</string>"

static char s_b64[2000];
static char s_pem[1500];
static char s_host_elem[1500];
static char s_root_elem[1400];
static char s_dev_elem[2100];
static char s_req[512];
static char s_xml[6144];
static char s_resp[3072];
static uint8_t s_dev_pem[800];
static uint8_t s_dev_cert[IPH_DEVCERT_MAX_SIZE];
static uint8_t s_scratch[256];

static int s_devcert_len;

static volatile bool s_pair_busy;
static volatile bool s_pair_ok;
/* Plug generation: incremented on unplug so that a pairing thread from the
 * previous session cannot set s_pair_ok again */
static volatile uint32_t s_pair_gen;

/* ------------------------------------------------------------------ */
/* Turn a DER certificate into a plist <key>xxx</key><data>...</data> element.
 */
static int pair_cert_element(const uint8_t *der, uint32_t der_len,
                             char *elem, uint32_t elem_size, const char *key)
{
    int n;

    n = iph_der_to_pem(der, der_len, s_pem, sizeof(s_pem));
    if (n <= 0)
    {
        rt_kprintf("[pair] der->pem failed for %s (%d)\n", key, n);
        return -1;
    }

    if (iph_b64_encode((const uint8_t *)s_pem, (uint32_t)n, s_b64, sizeof(s_b64)) <= 0)
    {
        rt_kprintf("[pair] b64 failed for %s\n", key);
        return -1;
    }

    rt_snprintf(elem, elem_size, "<key>%s</key><data>%s</data>", key, s_b64);
    return 0;
}

static int pair_build_xml(void)
{
    int n;

    if (pair_cert_element(iph_root_cert_der, IPH_ROOT_CERT_DER_LEN,
                          s_root_elem, sizeof(s_root_elem), "RootCertificate") != 0)
    {
        return -1;
    }

    if (pair_cert_element(iph_host_cert_der, IPH_HOST_CERT_DER_LEN,
                          s_host_elem, sizeof(s_host_elem), "HostCertificate") != 0)
    {
        return -1;
    }

    if (pair_cert_element(s_dev_cert, (uint32_t)s_devcert_len,
                          s_dev_elem, sizeof(s_dev_elem), "DeviceCertificate") != 0)
    {
        return -1;
    }

    n = rt_snprintf(s_xml, sizeof(s_xml),
                    XML_HEAD
                    "<key>Request</key><string>Pair</string>"
                    "<key>PairRecord</key><dict>"
                    "<key>HostID</key><string>%s</string>"
                    "<key>SystemBUID</key><string>%s</string>"
                    "%s%s%s"
                    "</dict>"
                    "<key>PairingOptions</key><dict>"
                    "<key>ExtendedPairingErrors</key><true/>"
                    "</dict>"
                    "</dict></plist>",
                    IPH_HOST_ID, IPH_SYSTEM_BUID,
                    s_host_elem, s_root_elem, s_dev_elem);

    if ((n <= 0) || (n >= (int)sizeof(s_xml)))
    {
        rt_kprintf("[pair] xml too long (%d)\n", n);
        return -1;
    }

    return n;
}

static int pair_request(const char *request, const char *key, char *resp,
                        uint32_t resp_max, uint32_t timeout_ms)
{
    if (key != NULL)
    {
        rt_snprintf(s_req, sizeof(s_req),
                    XML_HEAD
                    "<key>Request</key><string>%s</string>"
                    "<key>Key</key><string>%s</string>"
                    "</dict></plist>", request, key);
    }
    else
    {
        rt_snprintf(s_req, sizeof(s_req),
                    XML_HEAD
                    "<key>Request</key><string>%s</string>"
                    "</dict></plist>", request);
    }

    return iph_ld_request(s_req, resp, resp_max, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* Result of a single pairing attempt */
typedef enum
{
    PAIR_TRY_OK = 0,    /* host is trusted */
    PAIR_TRY_LOCKED,    /* phone is locked (PasswordProtected); retry after unlock */
    PAIR_TRY_FAIL,      /* any other failure; retrying is pointless */
} pair_try_result_t;

/* Whether the reply carries a lock-screen error. */
static bool pair_resp_locked(const char *resp)
{
    char err[96] = {0};

    if (iph_plist_get_string(resp, "Error", err, sizeof(err)) < 0)
    {
        return false;
    }

    return (strcmp(err, "PasswordProtected") == 0);
}

static pair_try_result_t pair_try_once(uint32_t gen)
{
    char type[64] = {0};
    char err[96] = {0};
    uint32_t deadline;
    bool locked = false;
    int n;
    int der_len;

    if (iph_ld_open() < 0)
    {
        return PAIR_TRY_FAIL;
    }
    if (iph_ld_connect(IPH_LD_LOCKDOWN_PORT) < 0)
    {
        iph_ld_close();
        return PAIR_TRY_FAIL;
    }

    /* 1) QueryType */
    n = pair_request("QueryType", NULL, s_resp, sizeof(s_resp), 5000);
    if (n <= 0)
    {
        rt_kprintf("[pair] QueryType failed, resp %d\n", n);
        iph_ld_close();
        return PAIR_TRY_FAIL;
    }
    iph_plist_get_string(s_resp, "Type", type, sizeof(type));
    rt_kprintf("[pair] QueryType -> '%s' (resp %d)\n", type, n);
    if (pair_resp_locked(s_resp))
    {
        rt_kprintf("[pair] iPhone is locked (PasswordProtected)\n");
        iph_ld_close();
        return PAIR_TRY_LOCKED;
    }
    if (strcmp(type, "com.apple.mobile.lockdown") != 0)
    {
        iph_plist_get_string(s_resp, "Error", err, sizeof(err));
        rt_kprintf("[pair] not lockdownd, Error=%s\n", err);
        iph_ld_close();
        return PAIR_TRY_FAIL;
    }

    /* 2) Device public key */
    n = pair_request("GetValue", "DevicePublicKey", s_resp, sizeof(s_resp), 5000);
    if (n <= 0)
    {
        rt_kprintf("[pair] GetValue failed, resp %d\n", n);
        iph_ld_close();
        return PAIR_TRY_FAIL;
    }
    n = iph_plist_get_data(s_resp, "Value", s_dev_pem, sizeof(s_dev_pem));
    rt_kprintf("[pair] DevicePublicKey %d bytes\n", n);
    if (n <= 0)
    {
        if (pair_resp_locked(s_resp))
        {
            rt_kprintf("[pair] iPhone is locked (PasswordProtected)\n");
            iph_ld_close();
            return PAIR_TRY_LOCKED;
        }
        iph_ld_close();
        return PAIR_TRY_FAIL;
    }

    /* 3) Sign the DeviceCertificate over the device public key with the root key */
    der_len = iph_devcert_build((const char *)s_dev_pem, (uint32_t)n,
                               s_dev_cert, sizeof(s_dev_cert));
    if (der_len <= 0)
    {
        rt_kprintf("[pair] build DeviceCertificate FAILED (%d)\n", der_len);
        iph_ld_close();
        return PAIR_TRY_FAIL;
    }
    s_devcert_len = der_len;

    /* 4) Build and send the Pair request */
    n = pair_build_xml();
    if (n <= 0)
    {
        iph_ld_close();
        return PAIR_TRY_FAIL;
    }
    rt_kprintf("[pair] sending Pair (%d bytes), tap TRUST on the phone\n", n);

    deadline = rt_tick_get() + rt_tick_from_millisecond(PAIR_DIALOG_TIMEOUT_MS);
    for (;;)
    {
        n = iph_ld_request(s_xml, s_resp, sizeof(s_resp), 60000);
        if (n <= 0)
        {
            rt_kprintf("[pair] Pair request failed, resp %d\n", n);
            break;
        }

        err[0] = '\0';
        if ((s_resp[0] == '\0') ||
                (iph_plist_get_string(s_resp, "Error", err, sizeof(err)) < 0))
        {
            break;      /* no Error means pairing succeeded */
        }

        if (strcmp(err, "PasswordProtected") == 0)
        {
            rt_kprintf("[pair] iPhone is locked (PasswordProtected)\n");
            locked = true;
            break;
        }

        if (strcmp(err, "PairingDialogResponsePending") != 0)
        {
            rt_kprintf("[pair] device error: %s\n", err);
            break;
        }

        if ((int32_t)(rt_tick_get() - deadline) >= 0)
        {
            rt_kprintf("[pair] dialog timeout, user did not confirm\n");
            break;
        }

        rt_thread_mdelay(1000);
    }

    iph_ld_close();

    if (locked)
    {
        return PAIR_TRY_LOCKED;
    }

    if ((n > 0) && (s_resp[0] != '\0') &&
            ((iph_plist_get_string(s_resp, "Error", err, sizeof(err)) < 0) ||
             (strcmp(err, "AlreadyPaired") == 0)))
    {
        if (gen == s_pair_gen)
        {
            s_pair_ok = true;
        }
        n = iph_plist_get_data(s_resp, "EscrowBag", s_scratch, sizeof(s_scratch));
        rt_kprintf("[pair] SUCCESS: host trusted (EscrowBag %d bytes)\n", (n > 0) ? n : 0);
        return PAIR_TRY_OK;
    }

    if (n > 0)
    {
        rt_kprintf("[pair] FAILED: %s\n", err);
    }

    return PAIR_TRY_FAIL;
}


static void pair_do_all(void *arg)
{
    uint32_t gen;
    uint32_t deadline;

    (void)arg;

    gen = s_pair_gen;

    rt_kprintf("[pair] ==== start, host id %s ====\n", IPH_HOST_ID);

    deadline = rt_tick_get() + rt_tick_from_millisecond(PAIR_LOCKED_WAIT_MS);

    for (;;)
    {
        pair_try_result_t r = pair_try_once(gen);

        if (r != PAIR_TRY_LOCKED)
        {
            break;
        }

        if ((gen != s_pair_gen) || ((int32_t)(rt_tick_get() - deadline) >= 0))
        {
            rt_kprintf("[pair] give up, unlock the iPhone and re-plug to retry\n");
            break;
        }

        rt_kprintf("[pair] waiting for unlock, retry in %dms\n", PAIR_LOCKED_RETRY_MS);
        rt_thread_mdelay(PAIR_LOCKED_RETRY_MS);
    }

    rt_kprintf("[pair] ==== done, ok=%d ====\n", s_pair_ok ? 1 : 0);
    s_pair_busy = false;
}

/* Called by main.c once the ipheth interface is ready on the USB side */
void iph_pair_auto_start(void)
{
    if (s_pair_ok || s_pair_busy)
    {
        return;
    }

    s_pair_busy = true;
    if (usb_osal_thread_create("iph_pair", PAIR_THREAD_STACK, PAIR_THREAD_PRIO,
                               pair_do_all, NULL) == NULL)
    {
        s_pair_busy = false;
        rt_kprintf("[pair] create thread failed\n");
    }
}

/* Called by main.c on USB disconnect so the next plug runs a fresh lockdown Pair. */

void iph_pair_reset(void)
{
    s_pair_gen++;

    if (s_pair_ok)
    {
        rt_kprintf("[pair] reset, will pair again on next plug\n");
    }

    s_pair_ok = false;
}

static void iph_pair(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (s_pair_busy)
    {
        rt_kprintf("[pair] busy\n");
        return;
    }

    s_pair_busy = true;
    if (usb_osal_thread_create("iph_pair", PAIR_THREAD_STACK, PAIR_THREAD_PRIO,
                               pair_do_all, NULL) == NULL)
    {
        s_pair_busy = false;
        rt_kprintf("[pair] create thread failed\n");
    }
}
MSH_CMD_EXPORT(iph_pair, iPhone lockdown Pair (needs TRUST tap on the phone))
