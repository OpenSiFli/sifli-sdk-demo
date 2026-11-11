/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include "usbh_core.h"
#include "usbh_ipheth.h"

#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <netif/ethernetif.h>

#undef USB_DBG_TAG
#define USB_DBG_TAG "ipheth"
#include "usb_log.h"

#if !LWIP_TCPIP_CORE_LOCKING
#error "usbh_ipheth requires LWIP_TCPIP_CORE_LOCKING == 1"
#endif

#define IPHETH_NETIF_NAME "u2"

static struct eth_device g_ipheth_dev;
static uint32_t g_ipheth_start_tick;

static uint32_t ipheth_elapsed_ms(void)
{
    return (uint32_t)(rt_tick_get() - g_ipheth_start_tick) * 1000U / RT_TICK_PER_SECOND;
}

static rt_err_t rt_usbh_ipheth_control(rt_device_t dev, int cmd, void *args)
{
    struct usbh_ipheth *ipheth_class = (struct usbh_ipheth *)dev->user_data;

    switch (cmd)
    {
        case NIOCTL_GADDR:
            /* Read the MAC address: a real MAC is required, otherwise the phone drops DHCP */
            if (args && ipheth_class)
            {
                rt_memcpy(args, ipheth_class->mac, 6);
            }
            else
            {
                return -RT_ERROR;
            }
            break;

        default:
            break;
    }

    return RT_EOK;
}

static rt_err_t rt_usbh_ipheth_eth_tx(rt_device_t dev, struct pbuf *p)
{
    struct pbuf *q;
    uint8_t *buffer;
    int ret;

    (void)dev;

    if (p->tot_len > IPHETH_BUF_SIZE)
    {
        USB_LOG_ERR("ipheth: tx pbuf %u overflow\r\n", (unsigned)p->tot_len);
        return -RT_ERROR;
    }

    buffer = usbh_ipheth_get_eth_txbuf();
    for (q = p; q != NULL; q = q->next)
    {
        usb_memcpy(buffer, q->payload, q->len);
        buffer += q->len;
    }

    ret = usbh_ipheth_eth_output(p->tot_len);
    if (ret < 0)
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}

void usbh_ipheth_eth_input(uint8_t *buf, uint32_t buflen)
{
    struct pbuf *p;
    err_t err;

    if (g_ipheth_dev.netif == NULL)
    {
        return;
    }

    p = pbuf_alloc(PBUF_RAW, buflen, PBUF_POOL);
    if (p == NULL)
    {
        USB_LOG_ERR("ipheth: no memory for pbuf\r\n");
        return;
    }

    if (pbuf_take(p, buf, buflen) != ERR_OK)
    {
        pbuf_free(p);
        return;
    }

    err = g_ipheth_dev.netif->input(p, g_ipheth_dev.netif);
    if (err != ERR_OK)
    {
        pbuf_free(p);
    }
}

void usbh_ipheth_link_change(struct usbh_ipheth *ipheth_class, bool up)
{
    struct netif *netif = g_ipheth_dev.netif;

    (void)ipheth_class;

    USB_LOG_INFO("ipheth: link %s (+%ums since class attach)\r\n", up ? "UP" : "DOWN", ipheth_elapsed_ms());

    if (netif == NULL)
    {
        return;
    }

    LOCK_TCPIP_CORE();
    if (up)
    {
        netif_set_link_up(netif);
    }
    else
    {
        netif_set_link_down(netif);
    }
    UNLOCK_TCPIP_CORE();
}

void usbh_ipheth_run(struct usbh_ipheth *ipheth_class)
{
    g_ipheth_start_tick = rt_tick_get();

    memset(&g_ipheth_dev, 0, sizeof(struct eth_device));

    g_ipheth_dev.parent.control = rt_usbh_ipheth_control;
    g_ipheth_dev.eth_rx = NULL;
    g_ipheth_dev.eth_tx = rt_usbh_ipheth_eth_tx;
    g_ipheth_dev.parent.user_data = ipheth_class;

    eth_device_init(&g_ipheth_dev, IPHETH_NETIF_NAME);

    USB_LOG_INFO("ipheth: netif %s registered, mac %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                 IPHETH_NETIF_NAME,
                 ipheth_class->mac[0], ipheth_class->mac[1], ipheth_class->mac[2],
                 ipheth_class->mac[3], ipheth_class->mac[4], ipheth_class->mac[5]);

    /* The baseline usbh_ipheth_run() only registers the netif; the rx thread is
     * created by this adaptation layer, matching how cherryusb handles the
     * rndis/ecm/ncm usbnet class drivers. */
    usb_osal_thread_create("usbh_ipheth_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1,
                           usbh_ipheth_rx_thread, NULL);
}

void usbh_ipheth_stop(struct usbh_ipheth *ipheth_class)
{
    struct netif *netif = g_ipheth_dev.netif;

    (void)ipheth_class;

    USB_LOG_INFO("ipheth: unregister netif %s\r\n", IPHETH_NETIF_NAME);

    if (netif == NULL)
    {
        return;
    }

    LOCK_TCPIP_CORE();
    eth_device_deinit(&g_ipheth_dev);
    g_ipheth_dev.netif = NULL;
    UNLOCK_TCPIP_CORE();
}
