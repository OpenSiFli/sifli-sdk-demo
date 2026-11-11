/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBH_IPHETH_H
#define USBH_IPHETH_H

#include "usb_util.h"   /* __WEAK */

#define IPHETH_INTFNUM     2
#define IPHETH_ALT_INTFNUM 1

/* 厂商控制请求 */
#define IPHETH_CMD_GET_MACADDR    0x00
#define IPHETH_CMD_CARRIER_CHECK  0x45
#define IPHETH_CARRIER_ON         0x04
#define IPHETH_CTRL_BUF_SIZE      0x40
#define IPHETH_CTRL_TIMEOUT       500


#define IPHETH_BUF_SIZE 1514


#define IPHETH_IP_ALIGN 2

#define IPHETH_RX_BUFSIZE (IPHETH_BUF_SIZE + IPHETH_IP_ALIGN)



#define IPHETH_CARRIER_POLL_MS 1000

struct usbh_ipheth 
{
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *bulkin;  /* Bulk IN endpoint */
    struct usb_endpoint_descriptor *bulkout; /* Bulk OUT endpoint */
    struct usbh_urb bulkout_urb;
    struct usbh_urb bulkin_urb;

    uint8_t intf;

    uint8_t mac[6];
    bool connect_status;
    bool pairing_confirmed;   

    void *user_data;
};

#ifdef __cplusplus
extern "C" {
#endif

int usbh_ipheth_get_connect_status(struct usbh_ipheth *ipheth_class);


void usbh_ipheth_link_change(struct usbh_ipheth *ipheth_class, bool up);

void usbh_ipheth_run(struct usbh_ipheth *ipheth_class);
void usbh_ipheth_stop(struct usbh_ipheth *ipheth_class);

uint8_t *usbh_ipheth_get_eth_txbuf(void);
int usbh_ipheth_eth_output(uint32_t buflen);
void usbh_ipheth_eth_input(uint8_t *buf, uint32_t buflen);
void usbh_ipheth_rx_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV);

#ifdef __cplusplus
}
#endif

#endif /* USBH_IPHETH_H */
