/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bf0_hal.h"
#include "usbd_core.h"
#include "usbd_hid.h"

#define HID8K_BUS_ID           0u
#define HID8K_IN_EP            0x81u
#define HID8K_REPORT_SIZE      8u
#define HID8K_CONFIG_DESC_SIZE 34u
#define HID8K_HS_INTERVAL      1u
#define HID8K_FS_INTERVAL      1u
#define HID8K_VID              0x38f4u
#define HID8K_PID              0xffffu
#define HID8K_BCD_DEVICE       0x0801u
#define HID8K_MAX_POWER_MA     100u

/*
 * Report bytes:
 *   0 buttons, 1 X, 2 Y, 3 wheel, 4..7 uint32 little-endian sequence.
 * The sequence is present only for protocol-analyzer continuity checks.
 */
static const uint8_t g_hid_report_descriptor[] =
{
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01,
    0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xc0,
    0x06, 0x00, 0xff, 0x09, 0x01, 0x15, 0x00,
    0x27, 0xff, 0xff, 0xff, 0xff, 0x75, 0x20, 0x95, 0x01,
    0x81, 0x02, 0xc0
};

static const uint8_t g_device_descriptor[] =
{
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00,
                               HID8K_VID, HID8K_PID,
                               HID8K_BCD_DEVICE, 0x01)
};

#define HID8K_INTERFACE_AND_ENDPOINT(interval)                                  \
    0x09, USB_DESCRIPTOR_TYPE_INTERFACE,                                        \
    0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,                                 \
    0x09, HID_DESCRIPTOR_TYPE_HID,                                               \
    0x11, 0x01, 0x00, 0x01, HID_DESCRIPTOR_TYPE_HID_REPORT,                     \
    WBVAL(sizeof(g_hid_report_descriptor)),                                      \
    0x07, USB_DESCRIPTOR_TYPE_ENDPOINT, HID8K_IN_EP, 0x03,                       \
    WBVAL(HID8K_REPORT_SIZE), (interval)

static const uint8_t g_config_descriptor_hs[] =
{
    USB_CONFIG_DESCRIPTOR_INIT(HID8K_CONFIG_DESC_SIZE, 0x01, 0x01,
                               USB_CONFIG_BUS_POWERED, HID8K_MAX_POWER_MA),
    HID8K_INTERFACE_AND_ENDPOINT(HID8K_HS_INTERVAL)
};

static const uint8_t g_config_descriptor_fs[] =
{
    USB_CONFIG_DESCRIPTOR_INIT(HID8K_CONFIG_DESC_SIZE, 0x01, 0x01,
                               USB_CONFIG_BUS_POWERED, HID8K_MAX_POWER_MA),
    HID8K_INTERFACE_AND_ENDPOINT(HID8K_FS_INTERVAL)
};

static const uint8_t g_other_speed_descriptor_hs[] =
{
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(HID8K_CONFIG_DESC_SIZE, 0x01, 0x01,
                                           USB_CONFIG_BUS_POWERED,
                                           HID8K_MAX_POWER_MA),
    HID8K_INTERFACE_AND_ENDPOINT(HID8K_FS_INTERVAL)
};

static const uint8_t g_other_speed_descriptor_fs[] =
{
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(HID8K_CONFIG_DESC_SIZE, 0x01, 0x01,
                                           USB_CONFIG_BUS_POWERED,
                                           HID8K_MAX_POWER_MA),
    HID8K_INTERFACE_AND_ENDPOINT(HID8K_HS_INTERVAL)
};

static const uint8_t g_device_qualifier_descriptor[] =
{
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, 0x01)
};

static const char *g_string_descriptors[] =
{
    (const char[]){0x09, 0x04},
    "SiFli",
    "SF32LB58 HID 8K Source",
    "LB58HID8K01"
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return g_device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    return (speed == USB_SPEED_HIGH) ? g_config_descriptor_hs
                                     : g_config_descriptor_fs;
}

static const uint8_t *device_qualifier_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return g_device_qualifier_descriptor;
}

static const uint8_t *other_speed_descriptor_callback(uint8_t speed)
{
    return (speed == USB_SPEED_HIGH) ? g_other_speed_descriptor_hs
                                     : g_other_speed_descriptor_fs;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index >= (sizeof(g_string_descriptors) /
                  sizeof(g_string_descriptors[0])))
    {
        return NULL;
    }
    return g_string_descriptors[index];
}

static const struct usb_descriptor g_usb_descriptor =
{
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback =
        device_qualifier_descriptor_callback,
    .other_speed_descriptor_callback = other_speed_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback
};

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_tx_report[HID8K_REPORT_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_control_report[HID8K_REPORT_SIZE];
static struct usbd_interface g_hid_interface;
static volatile bool g_device_configured;
static volatile uint32_t g_next_sequence;

static void fill_report(uint8_t *report, uint32_t sequence)
{
    report[0] = 0u;
    report[1] = 0u;
    report[2] = 0u;
    report[3] = 0u;
    report[4] = (uint8_t)(sequence >> 0);
    report[5] = (uint8_t)(sequence >> 8);
    report[6] = (uint8_t)(sequence >> 16);
    report[7] = (uint8_t)(sequence >> 24);
}

static int queue_next_report(void)
{
    uint32_t sequence = g_next_sequence;
    int result;

    fill_report(g_tx_report, sequence);
    result = usbd_ep_start_write(HID8K_BUS_ID, HID8K_IN_EP,
                                 g_tx_report, HID8K_REPORT_SIZE);
    if (result == 0)
    {
        memcpy(g_control_report, g_tx_report, HID8K_REPORT_SIZE);
        g_next_sequence = sequence + 1u;
    }
    else
    {
        /* Do not hide a submit failure with a software retry loop. */
        g_device_configured = false;
    }
    return result;
}

static void hid8k_ep_in_callback(uint8_t busid, uint8_t ep,
                                 uint32_t nbytes)
{
    if ((ep != HID8K_IN_EP) || !g_device_configured)
    {
        return;
    }

    if (!usb_device_is_configured(busid) ||
        (nbytes != HID8K_REPORT_SIZE))
    {
        g_device_configured = false;
        return;
    }

    (void)queue_next_report();
}

static struct usbd_endpoint g_hid_in_endpoint =
{
    .ep_cb = hid8k_ep_in_callback,
    .ep_addr = HID8K_IN_EP
};

static void usb_event_handler(uint8_t busid, uint8_t event)
{
    switch (event)
    {
    case USBD_EVENT_ERROR:
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        g_device_configured = false;
        g_next_sequence = 0u;
        break;
    case USBD_EVENT_CONFIGURED:
        if (usbd_get_port_speed(busid) == USB_SPEED_HIGH)
        {
            g_next_sequence = 0u;
            g_device_configured = true;
            (void)queue_next_report();
        }
        else
        {
            g_device_configured = false;
        }
        break;
    default:
        break;
    }
}

void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t **data, uint32_t *len)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    (void)report_type;
    *data = g_control_report;
    *len = HID8K_REPORT_SIZE;
}

int main(void)
{
    usbd_desc_register(HID8K_BUS_ID, &g_usb_descriptor);
    usbd_add_interface(HID8K_BUS_ID,
                       usbd_hid_init_intf(HID8K_BUS_ID, &g_hid_interface,
                                          g_hid_report_descriptor,
                                          sizeof(g_hid_report_descriptor)));
    usbd_add_endpoint(HID8K_BUS_ID, &g_hid_in_endpoint);
    usbd_initialize(HID8K_BUS_ID, (uintptr_t)USBC_BASE, usb_event_handler);
    return 0;
}
