/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include "usbh_core.h"
#include "usbh_ipheth.h"
#include "iph_ld.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "iph_ld"
#include "usb_log.h"

#define MUX_MAGIC        0xfeedfaceu
#define MUX_PROTO_VER    0
#define MUX_PROTO_CTRL   1
#define MUX_PROTO_SETUP  2
#define MUX_PROTO_TCP    6

#define TCP_SYN 0x02
#define TCP_ACK 0x10
#define TCP_RST 0x04

#define LD_CHUNK     1024
#define LD_TX_MAX    (16 + 20 + LD_CHUNK)
#define LD_RX_MAX    4096

static struct usbh_hubport *s_hport;
static struct usb_endpoint_descriptor *s_ep_in;
static struct usb_endpoint_descriptor *s_ep_out;
static uint8_t s_intf = 0xFF;

static uint8_t s_mux_ver;      /* 1 or 2 */
static uint8_t s_mux_hdr;      /* mux header length: 8 or 16 */
static uint16_t s_sport;
static uint16_t s_dport;
static uint16_t s_mux_tx;
static uint16_t s_mux_rx;
static uint32_t s_seq;
static uint32_t s_ack;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t s_tx[LD_TX_MAX];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t s_rx[LD_RX_MAX];

/* Send buffer: 4-byte length prefix + XML (a Pair request carries three PEM certificates, ~4.7 KB) */
static uint8_t s_sendbuf[7168];

/* ------------------------------------------------------------------ */
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int ld_bulk_tx(const uint8_t *buf, uint32_t len)
{
    struct usbh_urb urb;

    usbh_bulk_urb_fill(&urb, s_hport, s_ep_out, (uint8_t *)buf, len, 2000, NULL, NULL);
    return usbh_submit_urb(&urb);
}

static int ld_bulk_rx(uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    struct usbh_urb urb;
    int ret;

    usbh_bulk_urb_fill(&urb, s_hport, s_ep_in, buf, len, timeout_ms, NULL, NULL);
    ret = usbh_submit_urb(&urb);
    if (ret < 0)
    {
        return ret;
    }

    return (int)urb.actual_length;
}

/* ------------------------------------------------------------------ */
/* mux header: length determined by s_mux_hdr (v1 = 8 bytes, v2 = 16 bytes) */
static uint32_t ld_mux_hdr(uint8_t *p, uint32_t proto, uint32_t payload_len)
{
    uint32_t total = (uint32_t)s_mux_hdr + payload_len;

    put_be32(p + 0, proto);
    put_be32(p + 4, total);

    if (s_mux_hdr == 16)
    {
        put_be32(p + 8, MUX_MAGIC);
        put_be16(p + 12, s_mux_tx);
        put_be16(p + 14, s_mux_rx);
        s_mux_tx++;
    }

    return total;
}

static int ld_tcp_tx(uint8_t flags, const uint8_t *payload, uint32_t plen)
{
    uint32_t total = ld_mux_hdr(s_tx, MUX_PROTO_TCP, 20 + plen);
    uint8_t *t = s_tx + s_mux_hdr;

    put_be16(t + 0, s_sport);
    put_be16(t + 2, s_dport);
    put_be32(t + 4, s_seq);
    put_be32(t + 8, s_ack);
    t[12] = 0x50; /* th_off = 5 */
    t[13] = flags;
    put_be16(t + 14, 512);
    /* t+16..19 checksum/urgent stay 0 (a real host does not fill the checksum either) */
    t[16] = 0;
    t[17] = 0;
    t[18] = 0;
    t[19] = 0;

    if (plen != 0)
    {
        usb_memcpy(t + 20, payload, plen);
    }

    return ld_bulk_tx(s_tx, total);
}

/* ------------------------------------------------------------------ */
int iph_ld_open(void)
{
    struct usbh_ipheth *ipheth_class;
    uint8_t num;
    int ret;

    ipheth_class = (struct usbh_ipheth *)usbh_find_class_instance("/dev/ipheth");
    if ((ipheth_class == NULL) || (ipheth_class->hport == NULL))
    {
        USB_LOG_ERR("ld: ipheth not attached\r\n");
        return -USB_ERR_NOTCONN;
    }

    s_hport = ipheth_class->hport;
    s_ep_in = NULL;
    s_ep_out = NULL;
    s_intf = 0xFF;

    num = s_hport->config.config_desc.bNumInterfaces;
    for (uint8_t i = 0; i < num; i++)
    {
        struct usb_interface_descriptor *id = &s_hport->config.intf[i].altsetting[0].intf_desc;

        if ((id->bInterfaceClass != 0xFF) || (id->bInterfaceSubClass != 0xFE) ||
            (id->bInterfaceProtocol != 0x02))
        {
            continue;
        }

        s_intf = id->bInterfaceNumber;
        for (uint8_t e = 0; e < id->bNumEndpoints; e++)
        {
            struct usb_endpoint_descriptor *ep = &s_hport->config.intf[i].altsetting[0].ep[e].ep_desc;

            if (USB_GET_ENDPOINT_TYPE(ep->bmAttributes) != USB_ENDPOINT_TYPE_BULK)
            {
                continue;
            }
            if (ep->bEndpointAddress & 0x80)
            {
                s_ep_in = ep;
            }
            else
            {
                s_ep_out = ep;
            }
        }
        break;
    }

    if ((s_intf == 0xFF) || (s_ep_in == NULL) || (s_ep_out == NULL))
    {
        USB_LOG_ERR("ld: Apple Mobile Device interface not found\r\n");
        return -USB_ERR_NOTSUPP;
    }

    USB_LOG_INFO("ld: intf %u, ep out 0x%02x in 0x%02x\r\n",
                 s_intf, s_ep_out->bEndpointAddress, s_ep_in->bEndpointAddress);

    for (int i = 0; i < 8; i++)
    {
        int drained = ld_bulk_rx(s_rx, LD_RX_MAX, 30);

        if (drained <= 0)
        {
            break;
        }
    }

    /* 1) VERSION: 8-byte header + {major=1, minor=0, padding=0}
     *    A real host requests mux version 1. */
    s_mux_ver = 1;
    s_mux_hdr = 8;
    s_mux_tx = 0;
    s_mux_rx = 0xFFFF;

    put_be32(s_tx + 0, MUX_PROTO_VER);
    put_be32(s_tx + 4, 20);
    put_be32(s_tx + 8, 1);  /* major = 1 */
    put_be32(s_tx + 12, 0); /* minor */
    put_be32(s_tx + 16, 0); /* padding */

    ret = ld_bulk_tx(s_tx, 20);
    if (ret < 0)
    {
        USB_LOG_ERR("ld: version tx failed, ret %d\r\n", ret);
        return ret;
    }

    ret = ld_bulk_rx(s_rx, LD_RX_MAX, 2000);
    if (ret < 0)
    {
        USB_LOG_ERR("ld: version rx failed, ret %d\r\n", ret);
        return ret;
    }
    if (ret < 20)
    {
        USB_LOG_ERR("ld: version reply too short, len %d\r\n", ret);
        return -USB_ERR_IO;
    }

    s_mux_ver = (uint8_t)get_be32(s_rx + 8);
    s_mux_hdr = (s_mux_ver >= 2) ? 16 : 8;
    USB_LOG_INFO("ld: mux version %u (hdr %u)\r\n", s_mux_ver, s_mux_hdr);

    /* 2) SETUP is only needed for version >= 2; version 1 goes straight on */
    if (s_mux_hdr == 16)
    {
        s_mux_tx = 0;
        s_mux_rx = 0xFFFF;
        (void)ld_mux_hdr(s_tx, MUX_PROTO_SETUP, 1);
        s_tx[s_mux_hdr] = 0x07;
        ret = ld_bulk_tx(s_tx, s_mux_hdr + 1);
        if (ret < 0)
        {
            USB_LOG_ERR("ld: setup tx failed, ret %d\r\n", ret);
            return ret;
        }
    }

    rt_thread_mdelay(100);

    return 0;
}

void iph_ld_close(void)
{
    s_hport = NULL;
    s_ep_in = NULL;
    s_ep_out = NULL;
}

int iph_ld_connect(uint16_t port)
{
    int ret;
    bool syn_ok = false;

    s_sport = (uint16_t)(0x0800 | (rt_tick_get() & 0xFF));
    s_dport = port;
    s_seq = 0;
    s_ack = 0;

    ret = ld_tcp_tx(TCP_SYN, NULL, 0);
    if (ret < 0)
    {
        USB_LOG_ERR("ld: SYN failed, ret %d\r\n", ret);
        return ret;
    }

    for (int i = 0; i < 5; i++)
    {
        uint32_t proto;
        uint8_t *t;

        ret = ld_bulk_rx(s_rx, LD_RX_MAX, 2000);
        if (ret < 0)
        {
            USB_LOG_ERR("ld: wait SYN-ACK failed, ret %d\r\n", ret);
            return ret;
        }
        if (ret < (int)(s_mux_hdr + 20))
        {
            continue;
        }

        proto = get_be32(s_rx);
        if (proto != MUX_PROTO_TCP)
        {
            USB_LOG_INFO("ld: rx proto %u len %d\r\n", proto, ret);
            continue;
        }

        t = s_rx + s_mux_hdr;

        if ((get_be16(t + 0) != s_dport) || (get_be16(t + 2) != s_sport) ||
            (get_be32(t + 8) != 1))
        {
            continue;
        }

        if ((t[13] & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK))
        {
            if (t[13] & TCP_RST)
            {
                USB_LOG_ERR("ld: port %u refused\r\n", port);
                return -USB_ERR_IO;
            }
            continue;
        }

        s_mux_rx = (s_mux_hdr == 16) ? get_be16(s_rx + 14) : 0xFFFF;
        s_ack = get_be32(t + 4) + 1;
        s_seq = 1;
        syn_ok = true;
        break;
    }

    if (!syn_ok)
    {
        USB_LOG_ERR("ld: no SYN-ACK from port %u\r\n", port);
        return -USB_ERR_TIMEOUT;
    }

    USB_LOG_INFO("ld: tcp %u -> %u established\r\n", s_sport, port);
    return 0;
}

int iph_ld_send(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    int ret;

    while (len > 0)
    {
        uint32_t n = (len > LD_CHUNK) ? LD_CHUNK : len;

        ret = ld_tcp_tx(TCP_ACK, p, n);
        if (ret < 0)
        {
            return ret;
        }
        s_seq += n;
        p += n;
        len -= n;
    }

    return 0;
}

static const uint8_t *ld_find_xml(const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; (i + 4) <= n; i++)
    {
        if ((b[i] == '?') && (b[i + 1] == 'x') && (b[i + 2] == 'm') && (b[i + 3] == 'l'))
        {
            if ((i > 0) && (b[i - 1] == '<'))
            {
                return b + i - 1;
            }

            return b + i;
        }
    }

    return NULL;
}

/* Look for the end marker in a raw byte stream */
static bool ld_has_tail(const uint8_t *b, uint32_t n, const char *tail)
{
    uint32_t m = (uint32_t)strlen(tail);

    for (uint32_t i = 0; (i + m) <= n; i++)
    {
        if (memcmp(b + i, tail, m) == 0)
        {
            return true;
        }
    }

    return false;
}

int iph_ld_recv(void *buf, uint32_t maxlen, uint32_t timeout_ms)
{
    int ret;
    uint32_t proto;
    uint32_t plen;
    uint8_t *t;

    for (int i = 0; i < 8; i++)
    {
        ret = ld_bulk_rx(s_rx, LD_RX_MAX, timeout_ms);
        if (ret < 0)
        {
            return ret;
        }
        if (ret < (int)s_mux_hdr)
        {
            continue;
        }

        proto = get_be32(s_rx);

        if (proto == MUX_PROTO_CTRL)
        {
            if (ret > (int)(s_mux_hdr + 1))
            {
                const char *type = (s_rx[s_mux_hdr] == 3) ? "ERROR" :
                                  ((s_rx[s_mux_hdr] == 5) ? "WARNING" : "INFO");

                USB_LOG_WRN("ld: device %s: %.*s\r\n", type,
                            ret - s_mux_hdr - 1, (const char *)(s_rx + s_mux_hdr + 1));
            }
            continue;
        }
        if (proto != MUX_PROTO_TCP)
        {
            USB_LOG_INFO("ld: rx proto %u len %d\r\n", proto, ret);
            continue;
        }
        if (ret < (int)(s_mux_hdr + 20))
        {
            continue;
        }

        if (s_mux_hdr == 16)
        {
            s_mux_rx = get_be16(s_rx + 14);
        }

        t = s_rx + s_mux_hdr;
        plen = (uint32_t)ret - s_mux_hdr - 20;
        if (plen == 0)
        {
            continue;
        }

        s_ack = get_be32(t + 4) + plen;
        (void)ld_tcp_tx(TCP_ACK, NULL, 0);

        if (plen > maxlen)
        {
            plen = maxlen;
        }
        usb_memcpy(buf, t + 20, plen);

        return (int)plen;
    }

    return 0;
}

int iph_ld_request(const char *req, char *resp, uint32_t resp_max, uint32_t timeout_ms)
{
    uint32_t total = 0;
    uint32_t len = (uint32_t)strlen(req);
    uint32_t deadline = rt_tick_get() + rt_tick_from_millisecond(timeout_ms);
    uint32_t xlen;
    uint8_t *buf = (uint8_t *)resp;
    const uint8_t *x;
    int ret;

    /* A real host prefixes the XML with a 4-byte big-endian length
     * (measured: QueryType = 00 00 01 1c = 284). Without it the device treats
     * the first 4 bytes of "<?xm" as the length and resets the connection. */
    if ((len == 0) || (len + 4 > sizeof(s_sendbuf)))
    {
        return -USB_ERR_INVAL;
    }
    put_be32(s_sendbuf, len);
    memcpy(s_sendbuf + 4, req, len);

    ret = iph_ld_send(s_sendbuf, len + 4);
    if (ret < 0)
    {
        return ret;
    }

    resp[0] = '\0';
    for (;;)
    {
        if (total + 1 >= resp_max)
        {
            break;
        }

        ret = iph_ld_recv(buf + total, resp_max - total - 1, 1000);
        if (ret < 0)
        {
            /* Timeout: if data was already received, fall through to the parsing
             * check below; any other error is returned directly */
            if ((total > 0) && (ret == -USB_ERR_TIMEOUT))
            {
                break;
            }
            return ret;
        }
        if (ret > 0)
        {
            total += (uint32_t)ret;
        }

        /* The device reply may carry leading 0x00 or a 4-byte length prefix, so it
         * must not be searched with strstr as a C string */
        x = ld_find_xml(buf, total);
        if ((x != NULL) && ld_has_tail(x, (uint32_t)(buf + total - x), "</plist>"))
        {
            xlen = (uint32_t)(buf + total - x);
            memmove(resp, x, xlen);
            resp[xlen] = '\0';
            return (int)xlen;
        }

        if ((int32_t)(rt_tick_get() - deadline) >= 0)
        {
            break;
        }
    }

    /* On timeout: return whatever XML was collected, so a missing trailing byte
     * does not turn a valid reply into a failure */
    x = ld_find_xml(buf, total);
    if (x != NULL)
    {
        xlen = (uint32_t)(buf + total - x);
        memmove(resp, x, xlen);
        resp[xlen] = '\0';
        return (int)xlen;
    }

    /* Everything received was garbage (e.g. the debug string the device attaches
     * to an RST); never return it as a valid reply */
    resp[0] = '\0';
    return -USB_ERR_IO;
}

/* ------------------------------------------------------------------ */
int iph_b64_encode(const uint8_t *in, uint32_t in_len, char *out, uint32_t out_max)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t o = 0;
    uint32_t col = 0;

    for (uint32_t i = 0; i < in_len; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        uint32_t n = 1;

        if (i + 1 < in_len)
        {
            v |= (uint32_t)in[i + 1] << 8;
            n = 2;
        }
        if (i + 2 < in_len)
        {
            v |= in[i + 2];
            n = 3;
        }

        if (o + 5 >= out_max)
        {
            return -1;
        }

        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (n > 1) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = (n > 2) ? tbl[v & 0x3F] : '=';

        col += 4;
        if (col >= 64)
        {
            col = 0;
            out[o++] = '\n';
        }
    }

    if ((o > 0) && (out[o - 1] != '\n'))
    {
        out[o++] = '\n';
    }
    out[o] = '\0';

    return (int)o;
}

int iph_der_to_pem(const uint8_t *der, uint32_t der_len, char *out, uint32_t out_max)
{
    static const char pem_head[] = "-----BEGIN CERTIFICATE-----\n";
    static const char pem_tail[] = "-----END CERTIFICATE-----\n";
    uint32_t o;
    int n;

    if ((der == NULL) || (out == NULL))
    {
        return -1;
    }
    if (out_max <= (sizeof(pem_head) + sizeof(pem_tail)))
    {
        return -1;
    }

    memcpy(out, pem_head, sizeof(pem_head) - 1);
    o = sizeof(pem_head) - 1;

    /* iph_b64_encode wraps at 64 columns and appends a trailing '\n', which is
     * exactly the body format of a PEM block */
    n = iph_b64_encode(der, der_len, out + o, out_max - o - (uint32_t)(sizeof(pem_tail) - 1) - 1);
    if (n <= 0)
    {
        return -1;
    }
    o += (uint32_t)n;

    memcpy(out + o, pem_tail, sizeof(pem_tail) - 1);
    o += sizeof(pem_tail) - 1;
    out[o] = '\0';

    return (int)o;
}

static int b64_val(char c)
{
    if ((c >= 'A') && (c <= 'Z')) return c - 'A';
    if ((c >= 'a') && (c <= 'z')) return c - 'a' + 26;
    if ((c >= '0') && (c <= '9')) return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int iph_b64_decode(const char *in, uint32_t in_len, uint8_t *out, uint32_t out_max)
{
    uint32_t o = 0;
    uint32_t acc = 0;
    uint32_t bits = 0;

    for (uint32_t i = 0; (i < in_len) && (in[i] != '='); i++)
    {
        int v = b64_val(in[i]);

        if (v < 0)
        {
            continue;
        }

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (o < out_max)
            {
                out[o++] = (uint8_t)((acc >> bits) & 0xFF);
            }
        }
    }

    return (int)o;
}

static const char *plist_find_key(const char *xml, const char *key)
{
    char pat[64];
    int n = rt_snprintf(pat, sizeof(pat), "<key>%s</key>", key);

    if ((n <= 0) || (n >= (int)sizeof(pat)))
    {
        return NULL;
    }

    return strstr(xml, pat);
}

int iph_plist_get_string(const char *xml, const char *key, char *out, uint32_t out_max)
{
    const char *p = plist_find_key(xml, key);
    const char *s;
    const char *e;
    uint32_t n;

    if (p == NULL)
    {
        return -1;
    }

    s = strstr(p, "<string>");
    if (s == NULL)
    {
        return -1;
    }
    s += 8;
    e = strstr(s, "</string>");
    if (e == NULL)
    {
        return -1;
    }

    n = (uint32_t)(e - s);
    if (n >= out_max)
    {
        n = out_max - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';

    return (int)n;
}

int iph_plist_get_data(const char *xml, const char *key, uint8_t *out, uint32_t out_max)
{
    const char *p = plist_find_key(xml, key);
    const char *s;
    const char *e;

    if (p == NULL)
    {
        return -1;
    }

    s = strstr(p, "<data>");
    if (s == NULL)
    {
        return -1;
    }
    s += 6;
    e = strstr(s, "</data>");
    if (e == NULL)
    {
        return -1;
    }

    return iph_b64_decode(s, (uint32_t)(e - s), out, out_max);
}
