/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include "usbh_core.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "apple_cfg"
#include "usb_log.h"

#define APPLE_VID 0x05AC

/* ipheth  class/subclass/protocol */
#define APPLE_IPHETH_CLASS    0xFF
#define APPLE_IPHETH_SUBCLASS 0xFD
#define APPLE_IPHETH_PROTOCOL 0x01

#define APPLE_CFG_BUF_SIZE 512

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_apple_cfg_buf[APPLE_CFG_BUF_SIZE];


static int apple_get_config_desc(struct usbh_hubport *hport, uint8_t index,
                                 uint8_t *buf, uint16_t len)
{
    struct usbh_urb urb;
    struct usb_setup_packet setup;
    int ret;

    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
    setup.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_CONFIGURATION << 8) | index);
    setup.wIndex = 0;
    setup.wLength = len;


    usb_osal_mutex_take(hport->mutex);
    memset(&urb, 0, sizeof(urb));
    usbh_control_urb_fill(&urb, hport, &setup, buf, len, 1000, NULL, NULL);
    ret = usbh_submit_urb(&urb);
    if (ret == 0)
    {
        ret = (int)urb.actual_length;
    }
    usb_osal_mutex_give(hport->mutex);

    return ret;
}

static bool apple_cfg_has_ipheth(uint16_t total)
{
    uint16_t offset = 0;

    while ((offset + 2U) <= total)
    {
        uint8_t dlen = g_apple_cfg_buf[offset];
        uint8_t dtype = g_apple_cfg_buf[offset + 1U];

        if ((dlen < 2U) || ((offset + dlen) > total))
        {
            break;
        }

        if ((dtype == USB_DESCRIPTOR_TYPE_INTERFACE) && (dlen >= 9U))
        {
            struct usb_interface_descriptor *id =
                (struct usb_interface_descriptor *)(g_apple_cfg_buf + offset);

            if ((id->bInterfaceClass == APPLE_IPHETH_CLASS) &&
                    (id->bInterfaceSubClass == APPLE_IPHETH_SUBCLASS) &&
                    (id->bInterfaceProtocol == APPLE_IPHETH_PROTOCOL))
            {
                return true;
            }
        }

        offset += dlen;
    }

    return false;
}

uint8_t usbh_get_hport_active_config_index(struct usbh_hubport *hport)
{
    uint8_t count;

    if ((hport == NULL) || (hport->device_desc.idVendor != APPLE_VID))
    {
        return 0;
    }

    count = hport->device_desc.bNumConfigurations;
    if (count <= 1U)
    {
        return 0;
    }

    for (uint8_t ci = 0; ci < count; ci++)
    {
        struct usb_configuration_descriptor *cfg;
        uint16_t total;
        int ret;

        ret = apple_get_config_desc(hport, ci, g_apple_cfg_buf, USB_SIZEOF_CONFIG_DESC);
        if (ret < USB_SIZEOF_CONFIG_DESC)
        {
            continue;
        }

        cfg = (struct usb_configuration_descriptor *)g_apple_cfg_buf;
        total = cfg->wTotalLength;
        if ((total < USB_SIZEOF_CONFIG_DESC) || (total > APPLE_CFG_BUF_SIZE))
        {
            continue;
        }

        ret = apple_get_config_desc(hport, ci, g_apple_cfg_buf, total);
        if (ret < USB_SIZEOF_CONFIG_DESC)
        {
            continue;
        }

        if (apple_cfg_has_ipheth(total))
        {
            USB_LOG_INFO("[apple] use cfg%u (bConfigurationValue %u) with ipheth\r\n",
                         ci, cfg->bConfigurationValue);
            return ci;
        }
    }

    USB_LOG_WRN("[apple] no configuration with ipheth, keep config 0\r\n");
    return 0;
}
