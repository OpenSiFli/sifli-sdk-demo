/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CherryUSB configuration for the SF32LB58 HID 8 kHz host executor.
 */
#ifndef HID8K_HOST_USB_CONFIG_H
#define HID8K_HOST_USB_CONFIG_H

#include <rtthread.h>

#define CONFIG_USB_PRINTF(...) do { } while (0)
#define CONFIG_USB_DBG_LEVEL USB_DBG_ERROR

#define CONFIG_USB_ALIGN_SIZE 4
#define USB_NOCACHE_RAM_SECTION __attribute__((section(".noncacheable")))

#define CONFIG_USBHOST_MAX_BUS              1
#define CONFIG_USBHOST_MAX_RHPORTS          1
#define CONFIG_USBHOST_MAX_EXTHUBS          0
#define CONFIG_USBHOST_MAX_EHPORTS          1
#define CONFIG_USBHOST_MAX_INTERFACES       2
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 1
#define CONFIG_USBHOST_MAX_ENDPOINTS        2
#define CONFIG_USBHOST_MAX_HID_CLASS        1
#define CONFIG_USBHOST_DEV_NAMELEN          16

#define CONFIG_USBHOST_PSC_PRIO      0
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN 512
#define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT 500

#define CONFIG_USB_MUSB_PIPE_NUM 2
#define CONFIG_USB_MUSB_WITHOUT_MULTIPOINT
#define CONFIG_USB_MUSB_SIFLI

#ifdef SOC_SF32LB58X
#define CONFIG_USB_HS
#endif

#ifndef usb_phyaddr2ramaddr
#define usb_phyaddr2ramaddr(addr) (addr)
#endif

#ifndef usb_ramaddr2phyaddr
#define usb_ramaddr2phyaddr(addr) (addr)
#endif

#endif
