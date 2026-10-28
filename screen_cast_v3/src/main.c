/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */



#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>

#include "bf0_ble_gap.h"
#include "bf0_sibles.h"
#include "bf0_sibles_advertising.h"
#include "ble_connection_manager.h"
#include "bf0_sibles_serial_trans_service.h"
#include "littlevgl2rtt.h"

#include "screen_cast.h"
#include "screen_cast_ui.h"
#ifdef USING_SCREEN_CAST_WIFI
    #include "ota_wifi_tran.h"
#endif

#define LOG_TAG "screen_cast"
#include "log.h"

#define BLE_APP_TIMEOUT_INTERVAL    5000

/*
 * BLE / WiFi switch: when set to 1 the BLE path is disabled and only the WiFi
 * cast channel runs.
 */
#define SC_WIFI_ONLY_TEST   0

extern void lv_ex_data_pool_init(void);

typedef struct
{
    uint8_t  is_power_on;
    uint8_t  conn_idx;
    uint8_t  is_bg_adv_on;
    uint16_t mtu;
    rt_mailbox_t mb_handle;
} app_env_t;

static app_env_t g_app_env;

static app_env_t *ble_app_get_env(void)
{
    return &g_app_env;
}

SIBLES_ADVERTISING_CONTEXT_DECLAR(g_app_advertising_context);
SIBLES_ADVERTISING_CONTEXT_DECLAR(g_app_advertising_bg_context);

static uint8_t ble_app_background_advertising_event(uint8_t event, void *context, void *data)
{
    switch (event)
    {
    case SIBLES_ADV_EVT_ADV_STARTED:
    {
        sibles_adv_evt_startted_t *evt = (sibles_adv_evt_startted_t *)data;
        LOG_I("bg adv start result %d, mode %d", evt->status, evt->adv_mode);
        break;
    }
    case SIBLES_ADV_EVT_ADV_STOPPED:
    {
        sibles_adv_evt_stopped_t *evt = (sibles_adv_evt_stopped_t *)data;
        LOG_I("bg adv stopped reason %d", evt->reason);
        break;
    }
    default:
        break;
    }
    return 0;
}

static void ble_app_bg_advertising_start(void)
{
    sibles_advertising_para_t para = {0};
    uint8_t ret;
    char local_name[] = "SIFLI_BG_INFO";
    uint8_t manu_additnal_data[] = {0x20, 0xC4, 0x00, 0x91};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;

    para.own_addr_type = GAPM_STATIC_ADDR;
    para.config.adv_mode = SIBLES_ADV_CONNECT_MODE;
    para.config.mode_config.broadcast_config.duration = 0;
    para.config.mode_config.broadcast_config.interval = 0x140;
    para.config.max_tx_pwr = 0x7F;
    para.config.is_auto_restart = 1;

    para.adv_data.completed_name = rt_malloc(rt_strlen(local_name) + sizeof(sibles_adv_type_name_t));
    para.adv_data.completed_name->name_len = rt_strlen(local_name);
    rt_memcpy(para.adv_data.completed_name->name, local_name, para.adv_data.completed_name->name_len);

    para.rsp_data.manufacturer_data = rt_malloc(sizeof(sibles_adv_type_manufacturer_data_t) + sizeof(manu_additnal_data));
    para.rsp_data.manufacturer_data->company_id = manu_company_id;
    para.rsp_data.manufacturer_data->data_len = sizeof(manu_additnal_data);
    rt_memcpy(para.rsp_data.manufacturer_data->additional_data, manu_additnal_data, sizeof(manu_additnal_data));

    para.evt_handler = ble_app_background_advertising_event;

    ret = sibles_advertising_init(g_app_advertising_bg_context, &para);
    if (ret == SIBLES_ADV_NO_ERR)
    {
        sibles_advertising_start(g_app_advertising_bg_context);
    }

    rt_free(para.adv_data.completed_name);
    rt_free(para.rsp_data.manufacturer_data);
}

static uint8_t ble_app_advertising_event(uint8_t event, void *context, void *data)
{
    app_env_t *env = ble_app_get_env();

    switch (event)
    {
    case SIBLES_ADV_EVT_ADV_STARTED:
    {
        sibles_adv_evt_startted_t *evt = (sibles_adv_evt_startted_t *)data;
        if (!env->is_bg_adv_on)
        {
            env->is_bg_adv_on = 1;
            ble_app_bg_advertising_start();
        }
        LOG_I("adv start result %d, mode %d", evt->status, evt->adv_mode);
        break;
    }
    case SIBLES_ADV_EVT_ADV_STOPPED:
    {
        sibles_adv_evt_stopped_t *evt = (sibles_adv_evt_stopped_t *)data;
        LOG_I("adv stopped reason %d", evt->reason);
        break;
    }
    default:
        break;
    }
    return 0;
}

#define DEFAULT_LOCAL_NAME "SIFLI_APP"
static void ble_app_advertising_start(void)
{
    sibles_advertising_para_t para = {0};
    uint8_t ret;
    char local_name[31] = {0};
    uint8_t manu_additnal_data[] = {0x20, 0xC4, 0x00, 0x91};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;
    bd_addr_t addr;

    ret = ble_get_public_address(&addr);
    if (ret == HL_ERR_NO_ERROR)
    {
        rt_snprintf(local_name, 31, "SIFLI_APP-%x-%x-%x-%x-%x-%x", addr.addr[0], addr.addr[1], addr.addr[2],
                    addr.addr[3], addr.addr[4], addr.addr[5]);
    }
    else
    {
        memcpy(local_name, DEFAULT_LOCAL_NAME, sizeof(DEFAULT_LOCAL_NAME));
    }

    ble_gap_dev_name_t *dev_name = malloc(sizeof(ble_gap_dev_name_t) + strlen(local_name));
    dev_name->len = strlen(local_name);
    memcpy(dev_name->name, local_name, dev_name->len);
    ble_gap_set_dev_name(dev_name);
    free(dev_name);

    para.own_addr_type = GAPM_STATIC_ADDR;
    para.config.adv_mode = SIBLES_ADV_CONNECT_MODE;
    para.config.mode_config.conn_config.duration = 0;
    para.config.mode_config.conn_config.interval = 0x30;
    para.config.max_tx_pwr = 0x7F;
    para.config.is_auto_restart = 1;

    para.rsp_data.completed_name = rt_malloc(rt_strlen(local_name) + sizeof(sibles_adv_type_name_t));
    para.rsp_data.completed_name->name_len = rt_strlen(local_name);
    rt_memcpy(para.rsp_data.completed_name->name, local_name, para.rsp_data.completed_name->name_len);

    para.adv_data.manufacturer_data = rt_malloc(sizeof(sibles_adv_type_manufacturer_data_t) + sizeof(manu_additnal_data));
    para.adv_data.manufacturer_data->company_id = manu_company_id;
    para.adv_data.manufacturer_data->data_len = sizeof(manu_additnal_data);
    rt_memcpy(para.adv_data.manufacturer_data->additional_data, manu_additnal_data, sizeof(manu_additnal_data));

    para.evt_handler = ble_app_advertising_event;

    ret = sibles_advertising_init(g_app_advertising_context, &para);
    if (ret == SIBLES_ADV_NO_ERR)
    {
        sibles_advertising_start(g_app_advertising_context);
    }

    rt_free(para.rsp_data.completed_name);
    rt_free(para.adv_data.manufacturer_data);
}

#ifdef USING_SCREEN_CAST_WIFI
/**
 * @brief Start the WiFi cast channel (hotspot + TCP server).
 *
 * WiFi bring-up (driver load, firmware download) is slow and stack hungry, so
 * it runs in its own thread and does not interfere with the BLE transport. The
 * phone joins via the WiFi QR payload and then connects to the cast server.
 */
static void wifi_cast_start_thread_entry(void *parameter)
{
    // let LVGL / UI settle before WiFi bring-up (firmware download takes a while)
    rt_thread_mdelay(1000);

    if (ota_wifi_tran_start(SC_WIFI_GO_SSID, SC_WIFI_GO_PWD, SC_WIFI_SERVER_PORT) != 0)
    {
        LOG_E("wifi cast start failed");
        return;
    }
    LOG_I("wifi cast ready: ssid %s, tcp server %s:%d",
          SC_WIFI_GO_SSID, SC_WIFI_SERVER_IP, SC_WIFI_SERVER_PORT);
}

static void wifi_cast_start_thread(void)
{
    rt_thread_t tid = rt_thread_create("wifi_cast", wifi_cast_start_thread_entry, NULL, 8192,
                                       RT_THREAD_PRIORITY_MIDDLE, 10);
    if (tid)
    {
        rt_thread_startup(tid);
    }
}
#endif

static void ble_app_on_power_on(void)
{
    app_env_t *env = ble_app_get_env();
    env->is_power_on = 1;
    env->mtu = 23;

    // register the serial-transmission GATT service (BLE cast data channel)
    ble_serial_tran_init();

    ble_app_advertising_start();
    LOG_I("BLE powered on, advertising started");
}

static void ble_app_thread_entry(void *parameter)
{
    app_env_t *env = ble_app_get_env();
    rt_uint32_t value;

    sifli_ble_enable();

    while (1)
    {
        if (rt_mb_recv(env->mb_handle, &value, RT_WAITING_FOREVER) == RT_EOK)
        {
            if (value == BLE_POWER_ON_IND)
            {
                ble_app_on_power_on();
            }
        }
    }
}

static void ble_app_start_thread(void)
{
    rt_thread_t tid = rt_thread_create("ble_app", ble_app_thread_entry, NULL, 2048, 20, 10);
    if (tid)
    {
        rt_thread_startup(tid);
    }
}

int ble_app_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    app_env_t *env = ble_app_get_env();

    switch (event_id)
    {
    case BLE_POWER_ON_IND:
    {
        if (env->mb_handle)
        {
            rt_mb_send(env->mb_handle, BLE_POWER_ON_IND);
        }
        break;
    }
    case BLE_GAP_CONNECTED_IND:
    {
        ble_gap_connect_ind_t *ind = (ble_gap_connect_ind_t *)data;
        env->conn_idx = ind->conn_idx;
        LOG_I("connected, conn_idx %d", ind->conn_idx);
        break;
    }
    case SIBLES_MTU_EXCHANGE_IND:
    {
        sibles_mtu_exchange_ind_t *ind = (sibles_mtu_exchange_ind_t *)data;
        env->mtu = ind->mtu;
        LOG_I("Exchanged MTU size: %d", ind->mtu);
        break;
    }
    case BLE_GAP_DISCONNECTED_IND:
    {
        ble_gap_disconnected_ind_t *ind = (ble_gap_disconnected_ind_t *)data;
        LOG_I("disconnected reason %d", ind->reason);
        /* End the cast session; the UI returns to the "scan to connect" page */
        screen_cast_notify_disconnect();
        break;
    }
    default:
        break;
    }
    return 0;
}
BLE_EVENT_REGISTER(ble_app_event_handler, NULL);

/**
 * @brief Application entry point.
 */
int main(void)
{
    rt_err_t ret;

    rt_kprintf("SiFli BLE map cast (screen_cast) example\n");

#if !SC_WIFI_ONLY_TEST
    /* Start the BLE event thread */
    ble_app_get_env()->mb_handle = rt_mb_create("app", 8, RT_IPC_FLAG_FIFO);
    ble_app_start_thread();
#endif

    /* Initialise LVGL and the cast UI */
    ret = littlevgl2rtt_init("lcd");
    if (ret != RT_EOK)
    {
        rt_kprintf("littlevgl2rtt_init failed: %d\n", ret);
        return ret;
    }

    lv_ex_data_pool_init();

    // build the UI, QR code page included
    screen_cast_ui_init();

    /* Cast service bring-up: frame buffers plus the protocol handler binding.
     * Transport independent, shared by the BLE and WiFi links, run once. */
    screen_cast_init();

#ifdef USING_SCREEN_CAST_WIFI
    /* Start the WiFi transport (hotspot + TCP server), independent of BLE */
    wifi_cast_start_thread();
#endif

    while (1)
    {
        uint32_t ms = lv_task_handler();
        rt_thread_mdelay(ms);
    }

    return 0;
}
