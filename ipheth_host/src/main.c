/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include "rtthread.h"
#include "bf0_hal.h"
#include "usbh_core.h"
#include "usbh_ipheth.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "iph_pair.h"
#ifdef RT_USING_NETDEV
    #include <netdev.h>
#endif
#include <stdint.h>
#include <string.h>

#if defined(BSP_USING_SPI_NAND) && defined(RT_USING_DFS)
    #include "dfs_file.h"
    #include "dfs_posix.h"
    #include "drv_flash.h"
#endif

#define IPHETH_NETIF_NAME "u2"
#define DHCP_TIMEOUT_MS 30000U
#define DHCP_POLL_INTERVAL_MS 500U
#define IPHETH_LINK_SETTLE_MS 1000U

#define IPHETH_INTERFACE_NUM 2

struct usb_netif_status
{
    uint8_t present;
    uint8_t link_up;
    uint8_t dhcp_supplied;
    char ip[IP4ADDR_STRLEN_MAX];
    char netmask[IP4ADDR_STRLEN_MAX];
    char gateway[IP4ADDR_STRLEN_MAX];
};

static volatile uint32_t s_usb_connected_tick;
static volatile uint32_t s_usb_configured_tick;

static uint32_t elapsed_ms_since(uint32_t tick)
{
    if (tick == 0U)
    {
        return 0U;
    }
    return (uint32_t)(rt_tick_get() - tick) * 1000U / RT_TICK_PER_SECOND;
}

static void usb_host_event_handler(uint8_t busid, uint8_t hub_index, uint8_t hub_port,
                                   uint8_t intf, uint8_t event)
{
    (void)busid;
    (void)hub_index;
    (void)hub_port;

    switch (event)
    {
        case USBH_EVENT_DEVICE_CONNECTED:
            s_usb_connected_tick = rt_tick_get();
            s_usb_configured_tick = 0U;
            rt_kprintf("[USB] +0ms device connected\n");
            break;

        case USBH_EVENT_DEVICE_CONFIGURED:
            s_usb_configured_tick = rt_tick_get();
            rt_kprintf("[USB] +%ums configured (SET_CONFIGURATION done)\n",
                       elapsed_ms_since(s_usb_connected_tick));
            break;

        case USBH_EVENT_INTERFACE_START:
            rt_kprintf("[USB] +%ums interface %u class driver started\n",
                       elapsed_ms_since(s_usb_connected_tick), intf);
            if (intf == IPHETH_INTERFACE_NUM)
            {
                iph_pair_auto_start();
            }
            break;

        case USBH_EVENT_INTERFACE_UNSUPPORTED:
            rt_kprintf("[USB] +%ums interface %u unsupported class\n",
                       elapsed_ms_since(s_usb_connected_tick), intf);
            break;

        case USBH_EVENT_DEVICE_DISCONNECTED:
            rt_kprintf("[USB] device disconnected\n");
            s_usb_connected_tick = 0U;
            s_usb_configured_tick = 0U;
            iph_pair_reset();
            break;

        default:
            break;
    }
}

static struct netif *find_ipheth_netif_locked(void)
{
    struct netif *netif;

    for (netif = netif_list; netif != RT_NULL; netif = netif->next)
    {
        if (netif->name[0] == IPHETH_NETIF_NAME[0] &&
                netif->name[1] == IPHETH_NETIF_NAME[1])
        {
            return netif;
        }
    }

    return RT_NULL;
}

static void get_usb_netif_status(struct usb_netif_status *status)
{
    struct netif *netif;
    struct dhcp *dhcp;

    memset(status, 0, sizeof(*status));
    LOCK_TCPIP_CORE();
    netif = find_ipheth_netif_locked();
    if (netif != RT_NULL)
    {
        status->present = 1U;
        status->link_up = netif_is_link_up(netif) ? 1U : 0U;
        dhcp = netif_dhcp_data(netif);
        status->dhcp_supplied = (dhcp != RT_NULL &&
                                 dhcp_supplied_address(netif)) ? 1U : 0U;
        ipaddr_ntoa_r(&netif->ip_addr, status->ip, sizeof(status->ip));
        ipaddr_ntoa_r(&netif->netmask, status->netmask,
                      sizeof(status->netmask));
        ipaddr_ntoa_r(&netif->gw, status->gateway,
                      sizeof(status->gateway));
    }
    UNLOCK_TCPIP_CORE();
}

static int wait_for_ipheth_link(void)
{
    struct usb_netif_status status;

    while (1)
    {
        get_usb_netif_status(&status);
        if (status.present && status.link_up)
        {
            return 1;
        }

        rt_thread_mdelay(DHCP_POLL_INTERVAL_MS);
    }
}

static int wait_for_dhcp(void)
{
    struct usb_netif_status status;
    uint32_t elapsed = 0U;

    while (elapsed < DHCP_TIMEOUT_MS)
    {
        get_usb_netif_status(&status);
        if (!status.present || !status.link_up)
        {
            return 0;
        }

        if (status.dhcp_supplied)
        {
            rt_kprintf("[USB] DHCP bound IP=%s mask=%s gw=%s (T+%ums after plugin)\n",
                       status.ip, status.netmask, status.gateway,
                       elapsed_ms_since(s_usb_connected_tick));
            return 1;
        }

        rt_thread_mdelay(DHCP_POLL_INTERVAL_MS);
        elapsed += DHCP_POLL_INTERVAL_MS;
    }

    rt_kprintf("[USB] DHCP timeout after %ums: link is up but no lease\n",
               (unsigned)DHCP_TIMEOUT_MS);
    return 0;
}

#ifdef RT_USING_NETDEV
static void set_ipheth_default_netdev(void)
{
    struct netdev *ipheth_netdev;

    ipheth_netdev = netdev_get_by_name(IPHETH_NETIF_NAME);
    if (ipheth_netdev == RT_NULL)
    {
        return;
    }

    LOCK_TCPIP_CORE();
    netdev_set_default(ipheth_netdev);
    UNLOCK_TCPIP_CORE();
}
#endif /* RT_USING_NETDEV */

static void ipheth_monitor_thread(void *parameter)
{
    (void)parameter;
    rt_kprintf("[USB] Waiting for ipheth netif '%s'\n", IPHETH_NETIF_NAME);

    while (1)
    {
        struct usb_netif_status status;

        wait_for_ipheth_link();
        rt_thread_mdelay(IPHETH_LINK_SETTLE_MS);

        if (!wait_for_dhcp())
        {
            rt_thread_mdelay(DHCP_POLL_INTERVAL_MS);
            continue;
        }

#ifdef RT_USING_NETDEV
        set_ipheth_default_netdev();
#endif

        while (1)
        {
            rt_thread_mdelay(DHCP_POLL_INTERVAL_MS);
            get_usb_netif_status(&status);
            if (!status.present || !status.link_up || !status.dhcp_supplied)
            {
                rt_kprintf("[USB] ipheth network lost\n");
                break;
            }
        }
    }
}

#if defined(BSP_USING_SPI_NAND) && defined(RT_USING_DFS)
#define NAND_MTD_NAME "root"
static int fs_mount_init(void)
{
    register_nand_device(FS_REGION_START_ADDR & (0xFC000000),
                         FS_REGION_START_ADDR - (FS_REGION_START_ADDR & (0xFC000000)),
                         FS_REGION_SIZE, NAND_MTD_NAME);
    if (dfs_mount(NAND_MTD_NAME, "/", "elm", 0, 0) == 0)
    {
        rt_kprintf("[FS] mount fs on flash to root success\n");
    }
    else
    {
        rt_kprintf("[FS] mount fs on flash to root fail, mkfs first\n");
        if (dfs_mkfs("elm", NAND_MTD_NAME) == 0)
        {
            rt_kprintf("[FS] make elm fs on flash success, mount again\n");
            if (dfs_mount(NAND_MTD_NAME, "/", "elm", 0, 0) == 0)
            {
                rt_kprintf("[FS] mount fs on flash success\n");
            }
            else
            {
                rt_kprintf("[FS] mount to fs on flash fail\n");
            }
        }
        else
        {
            rt_kprintf("[FS] dfs_mkfs elm flash fail\n");
        }
    }
    return RT_EOK;
}
INIT_ENV_EXPORT(fs_mount_init);
#endif

int main(void)
{
    rt_thread_t thread;
    int ret;

    ret = usbh_initialize(0, (uintptr_t)USBC_BASE, usb_host_event_handler);
    if (ret < 0)
    {
        rt_kprintf("[USB] Host init failed ret=%d\n", ret);
        return ret;
    }

    thread = rt_thread_create("ipheth_mon", ipheth_monitor_thread, RT_NULL,
                              2048, 20, 10);
    if (thread == RT_NULL)
    {
        rt_kprintf("[USB] ipheth monitor create failed\n");
        return -RT_ERROR;
    }

    rt_thread_startup(thread);
    return 0;
}
