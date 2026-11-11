/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"
#include "usbh_ipheth.h"



#if CONFIG_USBHOST_MAX_INTF_ALTSETTINGS < 3
#error "iPhone USB tethering needs CONFIG_USBHOST_MAX_INTF_ALTSETTINGS >= 3 in usb_config.h"
#endif

#undef USB_DBG_TAG
#define USB_DBG_TAG "ipheth"
#include "usb_log.h"

#define DEV_FORMAT "/dev/ipheth"

static struct usbh_ipheth g_ipheth_class;


static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_ipheth_rx_buffer[USB_ALIGN_UP(IPHETH_RX_BUFSIZE, CONFIG_USB_ALIGN_SIZE)];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_ipheth_tx_buffer[USB_ALIGN_UP(IPHETH_BUF_SIZE, CONFIG_USB_ALIGN_SIZE)];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_ipheth_ctrl_buffer[USB_ALIGN_UP(IPHETH_CTRL_BUF_SIZE, CONFIG_USB_ALIGN_SIZE)];


static int usbh_ipheth_ctrl_read_raw(struct usbh_ipheth *ipheth_class, uint8_t cmd, uint16_t index, uint16_t size)
{
    struct usb_setup_packet *setup;
    int ret;

    if (!ipheth_class || !ipheth_class->hport)
    {
        return -USB_ERR_INVAL;
    }

    if (size > IPHETH_CTRL_BUF_SIZE) 
    {
        return -USB_ERR_INVAL;
    }

    setup = ipheth_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_VENDOR | USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = cmd;
    setup->wValue = 0;
    setup->wIndex = index;
    setup->wLength = size;

    ret = usbh_control_transfer(ipheth_class->hport, setup, g_ipheth_ctrl_buffer);

    if (ret < 0) 
    {
        return ret;
    }

    return ret;
}

static int usbh_ipheth_ctrl_read(struct usbh_ipheth *ipheth_class, uint8_t cmd, uint16_t index, void *data, uint16_t size)
{
    int ret;

    ret = usbh_ipheth_ctrl_read_raw(ipheth_class, cmd, index, size);
    if (ret < 0) {
        return ret;
    }

    usb_memcpy(data, g_ipheth_ctrl_buffer, MIN((uint32_t)ret, size));

    return ret;
}

int usbh_ipheth_get_connect_status(struct usbh_ipheth *ipheth_class)
{
    int ret;


    ret = usbh_ipheth_ctrl_read_raw(ipheth_class, IPHETH_CMD_CARRIER_CHECK, ipheth_class->intf, 4);
    if (ret <= 0) {
        return ret < 0 ? ret : -USB_ERR_IO;
    }

    USB_LOG_DBG("ipheth carrier check: len:%d data:%02x %02x %02x %02x\r\n",
                ret,
                g_ipheth_ctrl_buffer[0],
                g_ipheth_ctrl_buffer[1],
                g_ipheth_ctrl_buffer[2],
                g_ipheth_ctrl_buffer[3]);



    if (((ret == 1) && (g_ipheth_ctrl_buffer[0] == IPHETH_CARRIER_ON)) || ((ret >= 2) && (g_ipheth_ctrl_buffer[1] == IPHETH_CARRIER_ON))) 
    {
        ipheth_class->connect_status = true;
    } 
    else 
    {
        ipheth_class->connect_status = false;
    }

    return 0;
}

__WEAK void usbh_ipheth_link_change(struct usbh_ipheth *ipheth_class, bool up)
{
    (void)ipheth_class;
    (void)up;
}


static void usbh_ipheth_carrier_poll(struct usbh_ipheth *ipheth_class)
{
    bool prev = ipheth_class->connect_status;
    int ret;


    if (!ipheth_class->pairing_confirmed)
    {
        return;
    }

    ret = usbh_ipheth_get_connect_status(ipheth_class);

    if (ret < 0) 
    {

        return;
    }

    if (ipheth_class->connect_status == prev) 
    {
        return;
    }

    if (ipheth_class->connect_status) 
    {

        ret = usbh_set_interface(ipheth_class->hport, ipheth_class->intf, IPHETH_ALT_INTFNUM);
        USB_LOG_INFO("ipheth carrier on, re-arm set_interface %s\r\n", ret < 0 ? "FAILED" : "ok");
    } 
    else 
    {
        USB_LOG_WRN("ipheth carrier off\r\n");
    }

    usbh_ipheth_link_change(ipheth_class, ipheth_class->connect_status);
}

static int usbh_ipheth_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usb_endpoint_descriptor *ep_desc;
    struct usbh_ipheth *ipheth_class = &g_ipheth_class;
    uint8_t altsetting = IPHETH_ALT_INTFNUM;
    int ret;


    if (hport->device_desc.idVendor != USB_VENDOR_APPLE) 
    {
        USB_LOG_ERR("ipheth: unsupported idVendor:0x%04x\r\n", hport->device_desc.idVendor);
        return -USB_ERR_NOTSUPP;
    }

    memset(ipheth_class, 0, sizeof(struct usbh_ipheth));

    ipheth_class->hport = hport;
    ipheth_class->intf = intf;

    hport->config.intf[intf].priv = ipheth_class;


    if (intf != IPHETH_INTFNUM) 
    {
        USB_LOG_WRN("ipheth: interface is %u, expected %u, keep going anyway\r\n", intf, IPHETH_INTFNUM);
    }


    if (hport->config.intf[intf].altsetting_num <= altsetting) 
    {
        USB_LOG_ERR("ipheth: altsetting %u not found, altsetting_num:%u\r\n",altsetting, hport->config.intf[intf].altsetting_num);
        return -USB_ERR_NOTSUPP;
    }

    for (uint8_t i = 0; i < hport->config.intf[intf].altsetting[altsetting].intf_desc.bNumEndpoints; i++) 
    {
        ep_desc = &hport->config.intf[intf].altsetting[altsetting].ep[i].ep_desc;

        if (USB_GET_ENDPOINT_TYPE(ep_desc->bmAttributes) != USB_ENDPOINT_TYPE_BULK) {
            continue;
        }

        if (ep_desc->bEndpointAddress & 0x80) 
        {
            USBH_EP_INIT(ipheth_class->bulkin, ep_desc);
        } 
        else 
        {
            USBH_EP_INIT(ipheth_class->bulkout, ep_desc);
        }
    }

    if (!ipheth_class->bulkin || !ipheth_class->bulkout) 
    {
        USB_LOG_ERR("ipheth: no bulk endpoints in altsetting %u\r\n", altsetting);
        return -USB_ERR_NOTSUPP;
    }


    ret = usbh_set_interface(hport, intf, altsetting);

    if (ret < 0) 
    {
        USB_LOG_ERR("ipheth: set interface %u alt %u failed, errorcode:%d\r\n", intf, altsetting, ret);
        return ret;
    }

    ret = usbh_ipheth_ctrl_read(ipheth_class, IPHETH_CMD_GET_MACADDR, ipheth_class->intf, ipheth_class->mac, 6);

    if (ret < 6) 
    {
        USB_LOG_ERR("ipheth: failed to read mac address, errorcode:%d\r\n", ret);
        return ret < 0 ? ret : -USB_ERR_IO;
    }

    USB_LOG_INFO("ipheth MAC address %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                 ipheth_class->mac[0],
                 ipheth_class->mac[1],
                 ipheth_class->mac[2],
                 ipheth_class->mac[3],
                 ipheth_class->mac[4],
                 ipheth_class->mac[5]);


    USB_LOG_INFO("Init ipheth done\r\n");

    strncpy(hport->config.intf[intf].devname, DEV_FORMAT, CONFIG_USBHOST_DEV_NAMELEN);

    USB_LOG_INFO("Register ipheth Class:%s\r\n", hport->config.intf[intf].devname);
    usbh_ipheth_run(ipheth_class);

    return 0;
}

static int usbh_ipheth_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_ipheth *ipheth_class = (struct usbh_ipheth *)hport->config.intf[intf].priv;

    if (ipheth_class) 
    {
        if (ipheth_class->bulkin) 
        {
            usbh_kill_urb(&ipheth_class->bulkin_urb);
        }

        if (ipheth_class->bulkout) 
        {
            usbh_kill_urb(&ipheth_class->bulkout_urb);
        }

        if (hport->config.intf[intf].devname[0] != '\0') 
        {
            usb_osal_thread_schedule_other();
            USB_LOG_INFO("Unregister ipheth Class:%s\r\n", hport->config.intf[intf].devname);
            usbh_ipheth_stop(ipheth_class);
        }

        memset(ipheth_class, 0, sizeof(struct usbh_ipheth));
    }

    return 0;
}

void usbh_ipheth_rx_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV)
{
    uint32_t rx_length;
    int ret;

    (void)CONFIG_USB_OSAL_THREAD_GET_ARGV;
    USB_LOG_INFO("Create ipheth rx thread\r\n");
    // clang-format off
find_class:
    // clang-format on
    g_ipheth_class.connect_status = false;
    g_ipheth_class.pairing_confirmed = false;

    if (usbh_find_class_instance("/dev/ipheth") == NULL)
    {
        goto delete;
    }

    while (1)
    {

        usbh_bulk_urb_fill(&g_ipheth_class.bulkin_urb, g_ipheth_class.hport, g_ipheth_class.bulkin,
                           g_ipheth_rx_buffer, IPHETH_RX_BUFSIZE, IPHETH_CARRIER_POLL_MS, NULL, NULL);

        ret = usbh_submit_urb(&g_ipheth_class.bulkin_urb);

        /* 静默超时不是错误，是"该查载波了" */
        if (ret == -USB_ERR_TIMEOUT) 
        {
            usbh_ipheth_carrier_poll(&g_ipheth_class);
            continue;
        }

        if (ret < 0) 
        {
            goto find_class;
        }

        rx_length = g_ipheth_class.bulkin_urb.actual_length;

        /* 只有帧头没有载荷，丢掉继续收 */
        if (rx_length <= IPHETH_IP_ALIGN)
        {
            USB_LOG_DBG("ipheth drop empty rx urb\r\n");
            continue;
        }


        if (g_ipheth_class.pairing_confirmed == false)
        {
            g_ipheth_class.pairing_confirmed = true;
            USB_LOG_INFO("ipheth: first rx frame -> pairing confirmed, tx enabled\r\n");


            usbh_ipheth_carrier_poll(&g_ipheth_class);
        }

        USB_LOG_DBG("rxlen:%d\r\n", rx_length);
        usbh_ipheth_eth_input(&g_ipheth_rx_buffer[IPHETH_IP_ALIGN], rx_length - IPHETH_IP_ALIGN);
    }
    // clang-format off
delete:
    USB_LOG_INFO("Delete ipheth rx thread\r\n");

}

uint8_t *usbh_ipheth_get_eth_txbuf(void)
{
    return g_ipheth_tx_buffer;
}

int usbh_ipheth_eth_output(uint32_t buflen)
{
    int ret;


    if (g_ipheth_class.connect_status == false || g_ipheth_class.pairing_confirmed == false)
    {
        return -USB_ERR_NOTCONN;
    }

    if (buflen > IPHETH_BUF_SIZE) 
    {
        USB_LOG_ERR("ipheth: tx frame %u is overflow\r\n", buflen);
        return -USB_ERR_INVAL;
    }


        uint32_t len = buflen;

        if ((len % g_ipheth_class.bulkout->wMaxPacketSize) == 0)
        {
            g_ipheth_tx_buffer[len] = 0;  
            len += 1;
        }

        USB_LOG_DBG("txlen:%u\r\n", (unsigned)len);
        usbh_bulk_urb_fill(&g_ipheth_class.bulkout_urb, g_ipheth_class.hport, g_ipheth_class.bulkout,
                           g_ipheth_tx_buffer, len, USB_OSAL_WAITING_FOREVER, NULL, NULL);
    
    ret = usbh_submit_urb(&g_ipheth_class.bulkout_urb);


    if (ret < 0)
    {
        g_ipheth_class.connect_status = false;
    }

    return ret;
}

static const struct usbh_class_driver ipheth_class_driver = 
{
    .driver_name = "ipheth",
    .connect = usbh_ipheth_connect,
    .disconnect = usbh_ipheth_disconnect
};


CLASS_INFO_DEFINE const struct usbh_class_info ipheth_class_info = 
{
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
    .bInterfaceClass = USB_DEVICE_CLASS_VEND_SPECIFIC,
    .bInterfaceSubClass = USB_DEVICE_CLASS_APPLE_ETH_SUBCLASS,
    .bInterfaceProtocol = USB_DEVICE_CLASS_APPLE_ETH_PROTOCOL,
    .id_table = NULL,
    .class_driver = &ipheth_class_driver
};
