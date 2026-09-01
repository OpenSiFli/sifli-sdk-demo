/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bf0_hal.h"
#include "board.h"
#include "rtdevice.h"
#include "rtthread.h"
#include "usbd_core.h"
#include "usbd_hid.h"

#include <stdbool.h>
#include <stdint.h>

#include "hid8k_circle_lut.h"

#define HID8K_BUS_ID                  0u
#define HID8K_IN_EP                   0x81u
#define HID8K_REPORT_SIZE             8u
#define HID8K_CONFIG_DESC_SIZE        34u
#define HID8K_HS_INTERVAL             1u
#define HID8K_FS_INTERVAL             1u
#define HID8K_TARGET_RATE             8000u
#define HID8K_VID                     0x38f4u
#define HID8K_PID                     0xffffu
#define HID8K_BCD_DEVICE              0x0801u
#define HID8K_MAX_POWER_MA            100u
#define HID8K_KEY_DEBOUNCE_MASK       0x07u

#if (HID8K_CIRCLE_SAMPLE_COUNT != 4096u) || \
        (HID8K_CIRCLE_QUARTER_SAMPLE_COUNT != 1024u) || \
        (HID8K_CIRCLE_RADIUS_COUNTS != 1024u)
#error "Unexpected HID8K continuous-circle geometry"
#endif

/* Report: buttons, X, Y, wheel, sequence uint32 little-endian. No Report ID. */
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
    "SF32LB58 HID Rate Test",
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

static volatile bool g_device_configured;
static volatile bool g_configured_report_pending;
static volatile bool g_submit_error_pending;
static volatile uint8_t g_port_speed;
static volatile uint8_t g_next_buffer;
static volatile uint32_t g_next_sequence;
static volatile uint32_t g_circle_enabled;
static volatile int32_t g_last_submit_result;

#if defined(BSP_USING_KEY1) && defined(BSP_KEY1_PIN)
static uint8_t g_key1_history;
static bool g_key1_stable_pressed;
#endif

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_tx_report[2][HID8K_REPORT_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_control_report[HID8K_REPORT_SIZE];
static struct usbd_interface g_hid_interface;

static const char *speed_name(uint8_t speed)
{
    if (speed == USB_SPEED_HIGH)
    {
        return "HS";
    }
    if (speed == USB_SPEED_FULL)
    {
        return "FS";
    }
    return "UNKNOWN";
}

#if defined(BSP_USING_KEY1) && defined(BSP_KEY1_PIN)
static bool key1_is_pressed(void)
{
    int value = rt_pin_read(BSP_KEY1_PIN);

#ifdef BSP_KEY1_ACTIVE_HIGH
    return value != 0;
#else
    return value == 0;
#endif
}

static void circle_key_init(void)
{
    bool pressed;

#if BSP_KEY1_PIN == 150
    /* KEY1 is PB54; correct the board's PB54 pin function for this test. */
    (void)HAL_PIN_Set(PAD_PB54, GPIO_B54, PIN_PULLDOWN, 0);
#endif
    rt_pin_mode(BSP_KEY1_PIN, PIN_MODE_INPUT);
    pressed = key1_is_pressed();
    g_key1_history = pressed ? HID8K_KEY_DEBOUNCE_MASK : 0u;
    g_key1_stable_pressed = pressed;
    g_circle_enabled = 0u;
    rt_kprintf("KEY1 circle control pin=%u initial=OFF; press to toggle\n",
               (uint32_t)BSP_KEY1_PIN);
}

static void circle_key_poll(void)
{
    bool pressed = key1_is_pressed();

    g_key1_history = (uint8_t)(((uint32_t)g_key1_history << 1u) |
                               (pressed ? 1u : 0u));
    g_key1_history &= HID8K_KEY_DEBOUNCE_MASK;
    if ((g_key1_history == HID8K_KEY_DEBOUNCE_MASK) &&
            !g_key1_stable_pressed)
    {
        g_key1_stable_pressed = true;
        g_circle_enabled = g_circle_enabled ? 0u : 1u;
        rt_kprintf("HID8K CIRCLE %s\n",
                   g_circle_enabled ? "ON" : "OFF");
    }
    else if (g_key1_history == 0u)
    {
        g_key1_stable_pressed = false;
    }
}
#else
static void circle_key_init(void)
{
    g_circle_enabled = 0u;
    rt_kprintf("KEY1 circle control unavailable on this board\n");
}

static void circle_key_poll(void)
{
}
#endif

static void circle_delta_for_sequence(uint32_t sequence,
                                      int8_t *x, int8_t *y)
{
    uint32_t phase = sequence & HID8K_CIRCLE_SAMPLE_MASK;
    uint32_t quadrant = phase >> HID8K_CIRCLE_QUADRANT_SHIFT;
    uint32_t index = phase & HID8K_CIRCLE_QUARTER_MASK;
    int8_t dx = g_hid8k_circle_quarter_delta[index][0];
    int8_t dy = g_hid8k_circle_quarter_delta[index][1];

    switch (quadrant)
    {
    case 0u:
        *x = dx;
        *y = dy;
        break;
    case 1u:
        *x = (int8_t)-dy;
        *y = dx;
        break;
    case 2u:
        *x = (int8_t)-dx;
        *y = (int8_t)-dy;
        break;
    default:
        *x = dy;
        *y = (int8_t)-dx;
        break;
    }
}

static void fill_report(uint8_t *report, uint32_t sequence)
{
    int8_t x = 0;
    int8_t y = 0;

    if (g_circle_enabled != 0u)
    {
        circle_delta_for_sequence(sequence, &x, &y);
    }
    report[0] = 0u;
    report[1] = (uint8_t)x;
    report[2] = (uint8_t)y;
    report[3] = 0u;
    report[4] = (uint8_t)(sequence >> 0);
    report[5] = (uint8_t)(sequence >> 8);
    report[6] = (uint8_t)(sequence >> 16);
    report[7] = (uint8_t)(sequence >> 24);
}

static int queue_next_report(void)
{
    uint8_t buffer_index = g_next_buffer;
    uint32_t sequence = g_next_sequence;
    uint8_t *report = g_tx_report[buffer_index];
    int result;
    uint32_t i;

    fill_report(report, sequence);
    result = usbd_ep_start_write(HID8K_BUS_ID, HID8K_IN_EP, report,
                                 HID8K_REPORT_SIZE);
    if (result == 0)
    {
        for (i = 0u; i < HID8K_REPORT_SIZE; i++)
        {
            g_control_report[i] = report[i];
        }
        g_next_sequence = sequence + 1u;
        g_next_buffer = buffer_index ^ 1u;
    }
    else
    {
        g_last_submit_result = result;
        g_submit_error_pending = true;
    }
    return result;
}

static void hid8k_ep_in_callback(uint8_t busid, uint8_t ep,
                                 uint32_t nbytes)
{
    (void)busid;
    (void)nbytes;
    if ((ep == HID8K_IN_EP) && g_device_configured)
    {
        (void)queue_next_report();
    }
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
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        g_device_configured = false;
        break;
    case USBD_EVENT_CONFIGURED:
        g_port_speed = usbd_get_port_speed(busid);
        g_device_configured = true;
        g_configured_report_pending = true;
        (void)queue_next_report();
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

static int hid8k_usb_init(void)
{
    usbd_desc_register(HID8K_BUS_ID, &g_usb_descriptor);
    usbd_add_interface(HID8K_BUS_ID,
                       usbd_hid_init_intf(HID8K_BUS_ID, &g_hid_interface,
                                          g_hid_report_descriptor,
                                          sizeof(g_hid_report_descriptor)));
    usbd_add_endpoint(HID8K_BUS_ID, &g_hid_in_endpoint);
    return usbd_initialize(HID8K_BUS_ID, (uintptr_t)USBC_BASE,
                           usb_event_handler);
}

static void print_deferred_usb_status(void)
{
    bool print_configured;
    bool print_submit_error;
    uint8_t speed;
    int32_t submit_result;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    print_configured = g_configured_report_pending;
    print_submit_error = g_submit_error_pending;
    speed = g_port_speed;
    submit_result = g_last_submit_result;
    g_configured_report_pending = false;
    g_submit_error_pending = false;
    rt_hw_interrupt_enable(level);

    if (print_configured)
    {
        rt_kprintf("HID8K USB CONFIGURED speed=%s ep=0x%02x mps=%u "
                   "interval=%u\n",
                   speed_name(speed), HID8K_IN_EP, HID8K_REPORT_SIZE,
                   (speed == USB_SPEED_HIGH) ? HID8K_HS_INTERVAL :
                                               HID8K_FS_INTERVAL);
    }
    if (print_submit_error)
    {
        rt_kprintf("HID8K USB SUBMIT ERROR result=%d\n", submit_result);
    }
}

int main(void)
{
    int result;

    rt_kprintf("========================================\n");
    rt_kprintf("SF32LB58 USB HID 8K device, v4e functional path\n");
    rt_kprintf("target_rate=%u hs_bInterval=%u report_size=%u\n",
               HID8K_TARGET_RATE, HID8K_HS_INTERVAL, HID8K_REPORT_SIZE);
    rt_kprintf("device timing probes and local statistics are disabled\n");
    rt_kprintf("use the external USB protocol analyzer for test results\n");
    rt_kprintf("press KEY1 to start or stop continuous circle motion\n");
    rt_kprintf("========================================\n");

    circle_key_init();
    result = hid8k_usb_init();
    if (result != 0)
    {
        rt_kprintf("HID8K USB INIT ERROR result=%d\n", result);
    }

    while (1)
    {
        circle_key_poll();
        print_deferred_usb_status();
        rt_thread_mdelay(10);
    }
}
