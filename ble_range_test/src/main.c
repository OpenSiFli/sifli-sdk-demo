/**
  ******************************************************************************
  * @file   main.c
  * @author Sifli software development team
  ******************************************************************************
*/

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>

#include "bf0_ble_gap.h"
#include "bf0_sibles.h"
#include "bf0_sibles_internal.h"
#include "bf0_sibles_advertising.h"
#include "ble_connection_manager.h"
#include "range_test.h"

#define LOG_TAG "ble_app"
#include "log.h"

enum ble_app_att_list
{
    BLE_APP_SVC,
    BLE_APP_CHAR,
    BLE_APP_CHAR_VALUE,
    BLE_APP_CLIENT_CHAR_CONFIG_DESCRIPTOR,
    BLE_APP_ATT_NB
};

#define BLE_SPEED_TEST_FLAG (0x1312)

typedef struct
{
    uint16_t attr_hdl;
    uint16_t value_hdl;
    uint8_t prop;
    uint16_t cccd_hdl;
} ble_app_char_t;



#define app_svc_uuid { \
    0x73, 0x69, 0x66, 0x6c, \
    0x69, 0x5f, 0x61, 0x70, \
    0x70, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00 \
};

#define app_chara_uuid { \
    0x73, 0x69, 0x66, 0x6c, \
    0x69, 0x5f, 0x61, 0x70, \
    0x70, 0x01, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00 \
}

#define SERIAL_UUID_16(x) {((uint8_t)(x&0xff)),((uint8_t)(x>>8))}
#define BLE_APP_MAX_CONN_COUNT 4


typedef struct
{
    uint8_t conn_count;
    struct
    {
        uint8_t conn_idx;
        bd_addr_t peer_addr;
        uint16_t hdl_start;
        uint16_t hdl_end;
        ble_app_char_t app_char;
        uint16_t remote_handle;
        uint32_t data;
    } conn[BLE_APP_MAX_CONN_COUNT];
    sibles_hdl srv_handle;
    rt_mailbox_t mb_handle;
} app_env_t;

static app_env_t g_app_env;

static uint8_t g_app_svc[ATT_UUID_128_LEN] = app_svc_uuid;


#define RANGE_TEST_DEV_NAME "SIFLI_RANGE"
#define RANGE_SCAN_WINDOW_10MS 200   /* 2s */

static volatile uint8_t g_tx_connecting = 0;
static ble_gap_addr_t   g_tx_target;
static int8_t           g_tx_best_rssi = -128;
static uint8_t          g_tx_have_candidate = 0;
static rt_timer_t       g_tx_scan_wdt = RT_NULL;

static void range_test_scan_start(void);
static void range_test_connect(ble_gap_addr_t *peer);
static void range_tx_scan_decide(void);
static void range_tx_scan_wdt_start(void);
static void range_tx_scan_wdt_stop(void);

struct attm_desc_128 app_att_db[] =
{
    [BLE_APP_SVC] = {SERIAL_UUID_16(ATT_DECL_PRIMARY_SERVICE), PERM(RD, ENABLE), 0, 0},
    [BLE_APP_CHAR] = {SERIAL_UUID_16(ATT_DECL_CHARACTERISTIC), PERM(RD, ENABLE), 0, 0},
    [BLE_APP_CHAR_VALUE] = {
        app_chara_uuid, PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE) | PERM(WRITE_COMMAND, ENABLE) | PERM(NTF, ENABLE) |
        PERM(WP, NO_AUTH), PERM(UUID_LEN, UUID_128) | PERM(RI, ENABLE), 1024
    },
    [BLE_APP_CLIENT_CHAR_CONFIG_DESCRIPTOR] = {
        SERIAL_UUID_16(ATT_DESC_CLIENT_CHAR_CFG), PERM(RD, ENABLE) | PERM(WRITE_REQ,
                ENABLE) | PERM(WP, NO_AUTH), PERM(RI, ENABLE), 2
    },
};


static uint8_t ble_app_get_dev_by_idx(app_env_t *env, uint8_t conn_idx);

static app_env_t *ble_app_get_env(void)
{
    return &g_app_env;
}



SIBLES_ADVERTISING_CONTEXT_DECLAR(g_app_advertising_context);

static uint8_t ble_app_advertising_event(uint8_t event, void *context, void *data)
{
    app_env_t *env = ble_app_get_env();

    switch (event)
    {
    case SIBLES_ADV_EVT_ADV_STARTED:
    {
        sibles_adv_evt_startted_t *evt = (sibles_adv_evt_startted_t *)data;
        LOG_I("ADV start resutl %d, mode %d\r\n", evt->status, evt->adv_mode);
        break;
    }
    case SIBLES_ADV_EVT_ADV_STOPPED:
    {
        sibles_adv_evt_stopped_t *evt = (sibles_adv_evt_stopped_t *)data;
        LOG_I("ADV stopped reason %d, mode %d\r\n", evt->reason, evt->adv_mode);
        break;
    }
    default:
        break;
    }
    return 0;
}


static void ble_app_advertising_start(void)
{
    sibles_advertising_para_t para = {0};
    uint8_t ret;
    char local_name[31] = {0};
    uint8_t manu_additnal_data[] = {0x20, 0xC4, 0x00, 0x91};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;

    rt_snprintf(local_name, 31, "%s", RANGE_TEST_DEV_NAME);

    ble_gap_dev_name_t *dev_name = malloc(sizeof(ble_gap_dev_name_t) + strlen(local_name));
    dev_name->len = strlen(local_name);
    memcpy(dev_name->name, local_name, dev_name->len);
    ble_gap_set_dev_name(dev_name);
    free(dev_name);

    para.own_addr_type = GAPM_STATIC_ADDR;
    para.config.adv_mode = SIBLES_ADV_CONNECT_MODE;
    para.config.mode_config.conn_config.duration = 0x0;
    para.config.mode_config.conn_config.interval = 0x30;
    para.config.max_tx_pwr = 0x7F;
    para.config.is_auto_restart = 1;

    /* 名字超过 adv 长度,放到 scan rsp */
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
        sibles_advertising_start(g_app_advertising_context);

    rt_free(para.rsp_data.completed_name);
    rt_free(para.adv_data.manufacturer_data);
}

uint8_t *ble_app_gatts_get_cbk(uint8_t conn_idx, uint8_t idx, uint16_t *len)
{
    uint8_t *ret_val = NULL;
    app_env_t *env = ble_app_get_env();
    uint8_t dev_idx = ble_app_get_dev_by_idx(env, conn_idx);
    *len = 0;
    switch (idx)
    {
    case BLE_APP_CHAR_VALUE:
    {
        ret_val = (uint8_t *)&env->conn[dev_idx].data;
        *len = 4;
        break;
    }
    default:
        break;
    }
    return ret_val;
}

uint8_t ble_app_gatts_set_cbk(uint8_t conn_idx, sibles_set_cbk_t *para)
{
    (void)conn_idx;
    (void)para;
    return 0;
}

static void ble_app_service_init(void)
{
    app_env_t *env = ble_app_get_env();
    sibles_register_svc_128_t svc;

    svc.att_db = (struct attm_desc_128 *)&app_att_db;
    svc.num_entry = BLE_APP_ATT_NB;
    svc.sec_lvl = PERM(SVC_AUTH, NO_AUTH) | PERM(SVC_UUID_LEN, UUID_128) | PERM(SVC_MI, ENABLE);
    svc.uuid = g_app_svc;
    env->srv_handle = sibles_register_svc_128(&svc);
    if (env->srv_handle)
        sibles_register_cbk(env->srv_handle, ble_app_gatts_get_cbk, ble_app_gatts_set_cbk);
}


#ifdef ULOG_USING_FILTER
static void app_log_filter_set(void)
{
    ulog_tag_lvl_filter_set("BLE_GAP", LOG_LVL_WARNING);

    ulog_tag_lvl_filter_set("sibles", LOG_LVL_WARNING);
}
#endif

int main(void)
{
    app_env_t *env = ble_app_get_env();

    /* 必须在 sifli_ble_enable() 之前:发射功率经弱函数在 BT 初始化时下发 */
    range_test_io_init();

    env->mb_handle = rt_mb_create("app", 8, RT_IPC_FLAG_FIFO);
    sifli_ble_enable();
#ifdef ULOG_USING_FILTER
    app_log_filter_set();
#endif

    while (1)
    {
        uint32_t value;
        rt_mb_recv(env->mb_handle, (rt_uint32_t *)&value, RT_WAITING_FOREVER);
        if (value == BLE_POWER_ON_IND)
        {
            for (uint32_t i = 0; i < BLE_APP_MAX_CONN_COUNT; i++)
                env->conn[i].conn_idx = 0xFF;

            if (range_test_get_role() == RANGE_TEST_ROLE_RX)
            {
                ble_app_service_init();
                ble_app_advertising_start();
                LOG_I("RX: advertising as \"%s\"", RANGE_TEST_DEV_NAME);
            }
            else
            {
                g_tx_connecting = 0;
                range_test_scan_start();
            }
        }
    }
    return RT_EOK;
}


static void ble_app_device_connected(app_env_t *env, connection_manager_connect_ind_t *ind)
{
    uint32_t i;
    for (i = 0; i < BLE_APP_MAX_CONN_COUNT; i++)
        if (env->conn[i].conn_idx == 0xFF)
            break;

    if (i == BLE_APP_MAX_CONN_COUNT)
        RT_ASSERT(0);

    env->conn_count++;
    env->conn[i].conn_idx = ind->conn_idx;
    env->conn[i].peer_addr = ind->peer_addr;

    LOG_I("Peer device(role:%d) (%x-%x-%x-%x-%x-%x) connected as deivce %d", ind->role, env->conn[i].peer_addr.addr[5],
          env->conn[i].peer_addr.addr[4],
          env->conn[i].peer_addr.addr[3],
          env->conn[i].peer_addr.addr[2],
          env->conn[i].peer_addr.addr[1],
          env->conn[i].peer_addr.addr[0],
          i);
}

static void ble_app_deivce_disconnected(app_env_t *env, uint8_t idx, uint8_t reason)
{
    RT_ASSERT((idx < BLE_APP_MAX_CONN_COUNT) && (env->conn[idx].conn_idx != 0xFF));
    env->conn[idx].conn_idx = 0xFF;
    env->conn_count--;

    LOG_I("Device %d (%x-%x-%x-%x-%x-%x) disconnected(reason %d)", idx, env->conn[idx].peer_addr.addr[5],
          env->conn[idx].peer_addr.addr[4],
          env->conn[idx].peer_addr.addr[3],
          env->conn[idx].peer_addr.addr[2],
          env->conn[idx].peer_addr.addr[1],
          env->conn[idx].peer_addr.addr[0],
          reason);
}

static uint8_t ble_app_get_dev_by_idx(app_env_t *env, uint8_t conn_idx)
{
    uint32_t i;
    for (i = 0; i < BLE_APP_MAX_CONN_COUNT; i++)
        if (env->conn[i].conn_idx == conn_idx)
            break;

    return i == BLE_APP_MAX_CONN_COUNT ? 0xFF : (uint8_t)i;
}

static int8_t send_speed_test_packet(uint8_t *data, uint16_t len, uint8_t conn_idx)
{
    app_env_t *env = ble_app_get_env();
    sibles_write_remote_value_t value;
    uint8_t write_data[64];
    uint16_t command_flag = BLE_SPEED_TEST_FLAG;
    uint8_t idx = ble_app_get_dev_by_idx(env, conn_idx);

    if (idx == 0xFF || (uint32_t)len + 4 > sizeof(write_data))
        return -1;

    memcpy(write_data, &command_flag, 2);
    memcpy(write_data + 2, &len, 2);
    memcpy(write_data + 4, data, len);

    value.handle = env->conn[idx].app_char.value_hdl;
    value.write_type = SIBLES_WRITE_WITHOUT_RSP;
    value.len = len + 4;
    value.value = write_data;
    return sibles_write_remote_value(env->conn[idx].remote_handle, conn_idx, &value);
}

static rt_thread_t       g_tx_thread = RT_NULL;
static volatile uint8_t  g_tx_sending = 0;
static volatile uint8_t  g_tx_send_conn_idx = 0;

static void range_tx_send_entry(void *param)
{
    uint8_t payload[12] = {0};
    (void)param;

    while (1)
    {
        if (g_tx_sending)
        {
            int8_t ret = send_speed_test_packet(payload, sizeof(payload), g_tx_send_conn_idx);
            rt_thread_mdelay(ret == SIBLES_WIRTE_TX_FLOWCTRL_ERR ? 5 : 2);
        }
        else
        {
            rt_thread_mdelay(20);
        }
    }
}

static void range_tx_start_send(uint8_t conn_idx)
{
    g_tx_send_conn_idx = conn_idx;
    g_tx_sending = 1;
    if (g_tx_thread == RT_NULL)
    {
        g_tx_thread = rt_thread_create("rt_tx", range_tx_send_entry, RT_NULL,
                                       1024, RT_THREAD_PRIORITY_LOW, 10);
        if (g_tx_thread)
            rt_thread_startup(g_tx_thread);
    }
}

static int ble_app_gattc_event_handler(uint16_t event_id, uint8_t *data, uint16_t len)
{
    switch (event_id)
    {
    case SIBLES_REGISTER_REMOTE_SVC_RSP:
    {
        sibles_register_remote_svc_rsp_t *rsp = (sibles_register_remote_svc_rsp_t *)data;
        if (rsp->status == HL_ERR_NO_ERROR)
            range_tx_start_send(rsp->conn_idx);
        break;
    }
    default:
        break;
    }
    return 0;
}

void start_speed_test_search(uint8_t conn_idx)
{
    uint8_t app_svc[] = app_svc_uuid;
    sibles_search_service(conn_idx, ATT_UUID_128_LEN, (uint8_t *)app_svc);
}

/* 在 adv/scan-rsp 里匹配完整名(0x09)/简写名(0x08) */
static int range_adv_match_name(const uint8_t *adv, uint16_t len, const char *name)
{
    uint16_t i = 0;
    uint16_t nlen = (uint16_t)strlen(name);

    while ((uint16_t)(i + 1) < len)
    {
        uint8_t ad_len = adv[i];
        uint8_t ad_type;

        if (ad_len == 0)
            break;
        if ((uint16_t)(i + 1 + ad_len) > len)
            break;
        ad_type = adv[i + 1];
        if ((ad_type == 0x09 || ad_type == 0x08) &&
                (uint16_t)(ad_len - 1) >= nlen &&
                memcmp(&adv[i + 2], name, nlen) == 0)
            return 1;
        i = (uint16_t)(i + ad_len + 1);
    }
    return 0;
}

static void range_test_scan_start(void)
{
    ble_gap_scan_start_t scan_param = {0};

    g_tx_connecting     = 0;
    g_tx_have_candidate = 0;
    g_tx_best_rssi      = -128;

    scan_param.own_addr_type = GAPM_STATIC_ADDR;
    scan_param.type = GAPM_SCAN_TYPE_OBSERVER;
    scan_param.dup_filt_pol = 0;
    scan_param.scan_param_1m.scan_intv = 0x60;
    scan_param.scan_param_1m.scan_wd = 0x30;
    scan_param.duration = RANGE_SCAN_WINDOW_10MS;
    scan_param.period = 0;
    ble_gap_scan_start(&scan_param);
    range_tx_scan_wdt_start();
    LOG_I("TX: scanning %dms for \"%s\" (pick strongest RSSI)...",
          RANGE_SCAN_WINDOW_10MS * 10, RANGE_TEST_DEV_NAME);
}

static void range_test_connect(ble_gap_addr_t *peer)
{
    ble_gap_connection_create_param_t cp = {0};
    cp.own_addr_type = GAPM_STATIC_ADDR;
    cp.conn_to = 500;
    cp.type = GAPM_INIT_TYPE_DIRECT_CONN_EST;
    cp.conn_param_1m.scan_intv = 0x60;
    cp.conn_param_1m.scan_wd = 0x30;
    cp.conn_param_1m.conn_intv_max = 0x18;   /* 24 * 1.25ms = 30ms */
    cp.conn_param_1m.conn_intv_min = 0x18;
    cp.conn_param_1m.conn_latency = 0;
    cp.conn_param_1m.supervision_to = 500;   /* 5s */
    cp.conn_param_1m.ce_len_max = 0x10;
    cp.conn_param_1m.ce_len_min = 0x10;
    cp.peer_addr.addr_type = peer->addr_type;
    memcpy(cp.peer_addr.addr.addr, peer->addr.addr, BD_ADDR_LEN);
    ble_gap_create_connection(&cp);
}

static void range_tx_scan_decide(void)
{
    if (g_tx_connecting)
        return;
    if (g_tx_have_candidate)
    {
        g_tx_connecting = 1;
        LOG_I("TX: connecting to strongest \"%s\" (rssi %d)...", RANGE_TEST_DEV_NAME, g_tx_best_rssi);
        range_test_connect(&g_tx_target);
    }
    else
    {
        range_test_scan_start();
    }
}

static void tx_scan_wdt_cb(void *param)
{
    (void)param;
    if (range_test_get_role() == RANGE_TEST_ROLE_TX && !g_tx_connecting)
    {
        LOG_I("[wdt] TX: SCAN_STOPPED missing, forcing scan decision");
        ble_gap_scan_stop();
        range_tx_scan_decide();
    }
}

static void range_tx_scan_wdt_start(void)
{
    if (g_tx_scan_wdt == RT_NULL)
        g_tx_scan_wdt = rt_timer_create("tx_swdt", tx_scan_wdt_cb, RT_NULL,
                                        rt_tick_from_millisecond(RANGE_SCAN_WINDOW_10MS * 10 + 1000),
                                        RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (g_tx_scan_wdt)
    {
        rt_timer_stop(g_tx_scan_wdt);
        rt_timer_start(g_tx_scan_wdt);
    }
}

static void range_tx_scan_wdt_stop(void)
{
    if (g_tx_scan_wdt)
        rt_timer_stop(g_tx_scan_wdt);
}


int ble_app_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    app_env_t *env = ble_app_get_env();
    switch (event_id)
    {
    case BLE_POWER_ON_IND:
    {
        /* Handle in own thread to avoid conflict */
        if (env->mb_handle)
            rt_mb_send(env->mb_handle, BLE_POWER_ON_IND);
        break;
    }
    case BLE_GAP_CREATE_CONNECTION_CNF:
    {
        ble_gap_create_connection_cnf_t *cnf = (ble_gap_create_connection_cnf_t *)data;
        if (cnf->status != HL_ERR_NO_ERROR)
        {
            LOG_E("Create connection failed %d!", cnf->status);
            if (range_test_get_role() == RANGE_TEST_ROLE_TX)
            {
                g_tx_connecting = 0;
                range_test_scan_start();
            }
        }
        break;
    }
    case BLE_GAP_EXT_ADV_REPORT_IND:
    {
        ble_gap_ext_adv_report_ind_t *ind = (ble_gap_ext_adv_report_ind_t *)data;
        if (range_test_get_role() != RANGE_TEST_ROLE_TX || g_tx_connecting)
            break;
        if (range_adv_match_name(ind->data, ind->length, RANGE_TEST_DEV_NAME))
        {
            if (!g_tx_have_candidate || ind->rssi > g_tx_best_rssi)
            {
                g_tx_best_rssi = ind->rssi;
                g_tx_target = ind->addr;
                g_tx_have_candidate = 1;
                LOG_I("TX: candidate \"%s\" rssi %d", RANGE_TEST_DEV_NAME, ind->rssi);
            }
        }
        break;
    }
    case BLE_GAP_SCAN_STOPPED_IND:
    {
        if (range_test_get_role() == RANGE_TEST_ROLE_TX)
        {
            range_tx_scan_wdt_stop();
            range_tx_scan_decide();
        }
        break;
    }
    case BLE_GAP_CANCEL_CREATE_CONNECTION_CNF:
    {
        ble_gap_create_connection_cnf_t *cnf = (ble_gap_create_connection_cnf_t *)data;
        LOG_I("Create connection cancel status: %d", cnf->status);
        break;
    }
    case CONNECTION_MANAGER_CONNCTED_IND:
    {
        connection_manager_connect_ind_t *ind = (connection_manager_connect_ind_t *)data;
        if (env->conn_count == BLE_APP_MAX_CONN_COUNT)
        {
            LOG_E("Exceed maximum link number(%d)!", BLE_APP_MAX_CONN_COUNT);
            ble_gap_disconnect_t dis_conn;
            dis_conn.conn_idx = ind->conn_idx;
            dis_conn.reason = CO_ERROR_REMOTE_USER_TERM_CON;
            ble_gap_disconnect(&dis_conn);
            break;
        }

        ble_app_device_connected(env, ind);
        range_test_led_set_state(RANGE_TEST_LED_CONNECTED);
        if (range_test_get_role() == RANGE_TEST_ROLE_TX)
            range_tx_scan_wdt_stop();
        sibles_exchange_mtu(ind->conn_idx);
        break;
    }
    case BLE_GAP_UPDATE_CONN_PARAM_IND:
    {
        ble_gap_update_conn_param_ind_t *ind = (ble_gap_update_conn_param_ind_t *)data;
        uint8_t idx = ble_app_get_dev_by_idx(env, ind->conn_idx);
        if (idx == 0xFF)
            break;
        LOG_I("Updated device %d connection interval :%d", idx, ind->con_interval);
        break;
    }
    case SIBLES_MTU_EXCHANGE_IND:
    {
        sibles_mtu_exchange_ind_t *ind = (sibles_mtu_exchange_ind_t *)data;
        uint8_t idx = ble_app_get_dev_by_idx(env, ind->conn_idx);
        if (idx == 0xFF)
            break;

        LOG_I("Exchanged device %d MTU size: %d", idx, ind->mtu);

        if (range_test_get_role() == RANGE_TEST_ROLE_TX)
            start_speed_test_search(ind->conn_idx);
        break;
    }
    case BLE_GAP_DISCONNECTED_IND:
    {
        ble_gap_disconnected_ind_t *ind = (ble_gap_disconnected_ind_t *)data;
        uint8_t idx = ble_app_get_dev_by_idx(env, ind->conn_idx);
        if (idx == 0xFF)
            break;

        ble_app_deivce_disconnected(env, idx, ind->reason);
        sibles_unregister_remote_svc(ind->conn_idx, env->conn[idx].hdl_start, env->conn[idx].hdl_end, ble_app_gattc_event_handler);

        g_tx_sending = 0;
        range_test_led_set_state(RANGE_TEST_LED_SEARCHING);
        if (range_test_get_role() == RANGE_TEST_ROLE_TX)
        {
            g_tx_connecting = 0;
            range_test_scan_start();
        }
        break;
    }
    case SIBLES_WRITE_VALUE_RSP:
    {
        sibles_write_value_rsp_t *rsp = (sibles_write_value_rsp_t *)data;
        LOG_I("SIBLES_WRITE_VALUE_RSP %d", rsp->result);
        break;
    }
    case SIBLES_SEARCH_SVC_RSP:
    {
        sibles_svc_search_rsp_t *rsp = (sibles_svc_search_rsp_t *)data;
        uint8_t idx = ble_app_get_dev_by_idx(env, rsp->conn_idx);

        uint8_t app_svc[] = app_svc_uuid;
        uint8_t app_char[] = app_chara_uuid;

        if (idx == 0xFF)
            break;
        /* rsp->svc 在错误时可能为 NULL,UUID 不匹配先排除 */
        if (memcmp(rsp->search_uuid, app_svc, rsp->search_svc_len) != 0)
            break;
        if (rsp->result != HL_ERR_NO_ERROR)
            break;

        env->conn[idx].hdl_start = rsp->svc->hdl_start;
        env->conn[idx].hdl_end = rsp->svc->hdl_end;

        uint32_t i;
        uint8_t ready = 0;
        uint16_t offset = 0;
        sibles_svc_search_char_t *chara = (sibles_svc_search_char_t *)rsp->svc->att_db;
        for (i = 0; i < rsp->svc->char_count; i++)
        {
            if (!memcmp(chara->uuid, app_char, chara->uuid_len))
            {
                LOG_I("noti_uuid received, att handle(%x), des handle(%x)", chara->attr_hdl, chara->desc[0].attr_hdl);
                if (chara->desc_count != 1)
                    break;
                env->conn[idx].app_char.attr_hdl = chara->attr_hdl;
                env->conn[idx].app_char.value_hdl = chara->pointer_hdl;
                env->conn[idx].app_char.prop = chara->prop;
                env->conn[idx].app_char.cccd_hdl = chara->desc[0].attr_hdl;
                ready = 1;
            }
            offset = sizeof(sibles_svc_search_char_t) + chara->desc_count * sizeof(struct sibles_disc_char_desc_ind);
            chara = (sibles_svc_search_char_t *)((uint8_t *)chara + offset);
        }
        if (ready != 1)
            break;
        env->conn[idx].remote_handle = sibles_register_remote_svc(rsp->conn_idx, env->conn[idx].hdl_start, env->conn[idx].hdl_end, ble_app_gattc_event_handler);
        break;
    }

    default:
        break;
    }
    return 0;
}
BLE_EVENT_REGISTER(ble_app_event_handler, NULL);
/****END OF FILE****/

