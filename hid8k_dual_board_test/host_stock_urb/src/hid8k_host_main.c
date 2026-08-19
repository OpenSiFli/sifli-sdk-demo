/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "bf0_hal.h"
#include "hid8k_wire_format.h"
#include "rthw.h"
#include "rtthread.h"
#include "usb_errno.h"
#include "usbh_core.h"
#include "usbh_hid.h"

#define HID8K_HOST_AUTO_START_DELAY_MS 100u
#define HID8K_HOST_AUTO_START_DELAY_TICKS \
    (rt_tick_from_millisecond(HID8K_HOST_AUTO_START_DELAY_MS) + 1u)

/*
 * The stock MUSB HCD advances through an interrupt-IN URB buffer.  This
 * application therefore uses a large finite buffer instead of changing the
 * HCD into a cyclic receiver.  At 8,000 reports/s this buffer lasts 131.072 s.
 * Its contents are intentionally never inspected by the host firmware.
 */
#define HID8K_HOST_RX_BUFFER_BYTES (8u * 1024u * 1024u)

typedef enum
{
    HID8K_HOST_WAIT_DEVICE = 0,
    HID8K_HOST_REJECTED,
    HID8K_HOST_QUIET_WAIT,
    HID8K_HOST_SUBMITTING,
    HID8K_HOST_RUNNING,
    HID8K_HOST_RESUBMIT_PENDING,
    HID8K_HOST_FAULT
} hid8k_host_state_t;

typedef struct
{
    uint16_t vid;
    uint16_t pid;
    uint16_t mps;
    uint8_t speed;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t endpoint;
    uint8_t endpoint_type;
    uint8_t interval;
    uint8_t valid;
} hid8k_host_descriptor_t;

static uint8_t g_hid8k_rx_buffer[HID8K_HOST_RX_BUFFER_BYTES]
    __attribute__((section(".bss.l2_non_ret_bss_hid8k_host_rx"), aligned(4)));

static volatile hid8k_host_state_t g_host_state = HID8K_HOST_WAIT_DEVICE;
static volatile uint8_t g_host_connected;
static volatile uint8_t g_host_descriptor_valid;
static volatile rt_tick_t g_host_arm_tick;
static struct usbh_hid *volatile g_host_hid;
static struct rt_semaphore g_host_submit_sem;
static struct rt_timer g_host_auto_start_timer;

static bool hid8k_host_state_is_active(hid8k_host_state_t state)
{
    return (state == HID8K_HOST_QUIET_WAIT) ||
           (state == HID8K_HOST_SUBMITTING) ||
           (state == HID8K_HOST_RUNNING) ||
           (state == HID8K_HOST_RESUBMIT_PENDING);
}

static hid8k_host_descriptor_t hid8k_host_read_descriptor(
    struct usbh_hid *hid_class)
{
    hid8k_host_descriptor_t descriptor = { 0 };
    struct usb_interface_descriptor *interface_descriptor;
    struct usb_endpoint_descriptor *endpoint;

    if ((hid_class == RT_NULL) || (hid_class->hport == RT_NULL))
    {
        return descriptor;
    }

    descriptor.vid = hid_class->hport->device_desc.idVendor;
    descriptor.pid = hid_class->hport->device_desc.idProduct;
    descriptor.speed = hid_class->hport->speed;
    descriptor.interface_protocol = hid_class->protocol;

    interface_descriptor =
        &hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc;
    descriptor.interface_class = interface_descriptor->bInterfaceClass;
    descriptor.interface_subclass = interface_descriptor->bInterfaceSubClass;

    endpoint = hid_class->intin;
    if (endpoint != RT_NULL)
    {
        descriptor.endpoint = endpoint->bEndpointAddress;
        descriptor.endpoint_type =
            USB_GET_ENDPOINT_TYPE(endpoint->bmAttributes);
        descriptor.mps = USB_GET_MAXPACKETSIZE(endpoint->wMaxPacketSize);
        descriptor.interval = endpoint->bInterval;
    }

    descriptor.valid =
        (descriptor.vid == HID8K_TARGET_VID) &&
        (descriptor.pid == HID8K_TARGET_PID) &&
        (descriptor.speed == USB_SPEED_HIGH) &&
        (descriptor.interface_class == HID8K_INTERFACE_CLASS) &&
        (descriptor.interface_subclass == HID8K_INTERFACE_SUBCLASS) &&
        (descriptor.interface_protocol == HID8K_INTERFACE_PROTOCOL) &&
        (descriptor.endpoint == HID8K_EP_IN) &&
        (descriptor.endpoint_type == USB_ENDPOINT_TYPE_INTERRUPT) &&
        (descriptor.mps == HID8K_REPORT_SIZE) &&
        (descriptor.interval == HID8K_HS_INTERVAL);

    return descriptor;
}

static bool hid8k_host_connection_is_valid_locked(struct usbh_hid *hid_class)
{
    return g_host_connected &&
           g_host_descriptor_valid &&
           (hid_class != RT_NULL) &&
           (hid_class == g_host_hid) &&
           (hid_class->hport != RT_NULL) &&
           hid_class->hport->connected;
}

static void hid8k_host_complete(void *arg, int nbytes)
{
    struct usbh_hid *hid_class = (struct usbh_hid *)arg;
    bool resubmit = false;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    if ((hid_class == RT_NULL) || (hid_class != g_host_hid))
    {
        rt_hw_interrupt_enable(level);
        return;
    }

    if (hid8k_host_connection_is_valid_locked(hid_class) &&
        (nbytes != -USB_ERR_SHUTDOWN) &&
        (nbytes != -USB_ERR_NOTCONN))
    {
        g_host_state = HID8K_HOST_RESUBMIT_PENDING;
        resubmit = true;
    }
    else
    {
        g_host_state = HID8K_HOST_WAIT_DEVICE;
    }
    rt_hw_interrupt_enable(level);

    if (resubmit)
    {
        rt_sem_release(&g_host_submit_sem);
    }
}

static void hid8k_host_submit_long_urb(void)
{
    struct usbh_hid *hid_class;
    hid8k_host_state_t state;
    rt_tick_t now;
    int ret;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    state = g_host_state;
    if ((state != HID8K_HOST_QUIET_WAIT) &&
        (state != HID8K_HOST_RESUBMIT_PENDING))
    {
        rt_hw_interrupt_enable(level);
        return;
    }

    if (state == HID8K_HOST_QUIET_WAIT)
    {
        now = rt_tick_get();
        if ((rt_tick_t)(now - g_host_arm_tick) <
            HID8K_HOST_AUTO_START_DELAY_TICKS)
        {
            rt_hw_interrupt_enable(level);
            return;
        }
    }

    hid_class = (struct usbh_hid *)g_host_hid;
    if (!hid8k_host_connection_is_valid_locked(hid_class))
    {
        g_host_state = HID8K_HOST_WAIT_DEVICE;
        rt_hw_interrupt_enable(level);
        return;
    }

    g_host_state = HID8K_HOST_SUBMITTING;
    usbh_int_urb_fill(&hid_class->intin_urb,
                      hid_class->hport,
                      hid_class->intin,
                      g_hid8k_rx_buffer,
                      sizeof(g_hid8k_rx_buffer),
                      0u,
                      hid8k_host_complete,
                      hid_class);
    ret = usbh_submit_urb(&hid_class->intin_urb);

    if (ret < 0)
    {
        if ((ret == -USB_ERR_NOTCONN) ||
            !hid8k_host_connection_is_valid_locked(hid_class))
        {
            g_host_state = HID8K_HOST_WAIT_DEVICE;
        }
        else
        {
            g_host_state = HID8K_HOST_FAULT;
        }
    }
    else
    {
        g_host_state = HID8K_HOST_RUNNING;
    }
    rt_hw_interrupt_enable(level);
}

static void hid8k_host_auto_start_timeout(void *parameter)
{
    (void)parameter;
    rt_sem_release(&g_host_submit_sem);
}

void usbh_hid_run(struct usbh_hid *hid_class)
{
    hid8k_host_descriptor_t descriptor =
        hid8k_host_read_descriptor(hid_class);
    bool auto_start = false;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    if (!hid8k_host_state_is_active(g_host_state))
    {
        g_host_hid = hid_class;
        g_host_connected = 1u;
        g_host_descriptor_valid = descriptor.valid;
        if (descriptor.valid)
        {
            g_host_arm_tick = rt_tick_get();
            g_host_state = HID8K_HOST_QUIET_WAIT;
            auto_start = true;
        }
        else
        {
            g_host_state = HID8K_HOST_REJECTED;
        }
    }
    rt_hw_interrupt_enable(level);

    if (auto_start)
    {
        rt_timer_stop(&g_host_auto_start_timer);
        if (rt_timer_start(&g_host_auto_start_timer) != RT_EOK)
        {
            level = rt_hw_interrupt_disable();
            if ((g_host_hid == hid_class) &&
                (g_host_state == HID8K_HOST_QUIET_WAIT))
            {
                g_host_state = HID8K_HOST_FAULT;
            }
            rt_hw_interrupt_enable(level);
        }
    }
}

void usbh_hid_stop(struct usbh_hid *hid_class)
{
    bool owns_connection = false;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    if (g_host_hid == hid_class)
    {
        owns_connection = true;
        g_host_hid = RT_NULL;
        g_host_connected = 0u;
        g_host_descriptor_valid = 0u;
        g_host_state = HID8K_HOST_WAIT_DEVICE;
    }
    rt_hw_interrupt_enable(level);

    if (owns_connection)
    {
        rt_timer_stop(&g_host_auto_start_timer);
    }
}

int main(void)
{
    int ret;

    ret = rt_sem_init(&g_host_submit_sem, "hid8kgo", 0,
                      RT_IPC_FLAG_FIFO);
    if (ret != RT_EOK)
    {
        return ret;
    }

    rt_timer_init(&g_host_auto_start_timer, "hid8kas",
                  hid8k_host_auto_start_timeout, RT_NULL,
                  HID8K_HOST_AUTO_START_DELAY_TICKS,
                  RT_TIMER_FLAG_ONE_SHOT);

    ret = usbh_initialize(0u, (uintptr_t)USBC_BASE, RT_NULL);
    if (ret < 0)
    {
        return ret;
    }

    while (1)
    {
        rt_sem_take(&g_host_submit_sem, RT_WAITING_FOREVER);
        hid8k_host_submit_long_urb();
    }
}
