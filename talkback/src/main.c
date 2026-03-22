/*
 * SPDX-FileCopyrightText: 2024-2024 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "bf0_ble_gap.h"
#include "bf0_sibles.h"
#include "bf0_sibles_internal.h"
#include "bf0_sibles_advertising.h"
#include "ble_talk.h"
#include "button.h"
#include "main.h"
#include "ble_talk_network.h"
#include "./peripheral/rgbled.h"
#include "bf0_pm.h"
#include "gui_app_pm.h"
#include "battery_calculator.h"
static app_env_t g_app_env;
static rt_mailbox_t g_app_mb;

static rt_mailbox_t g_rgb_led_mb;
static rt_thread_t g_rgb_led_thread;
static rt_thread_t g_battery_thread;
static uint8_t is_idle = 1;
static uint8_t is_talking = 0;
// Standby idle countdown (60s)
#define STANDBY_IDLE_TIMEOUT_SECONDS 60
struct rt_delayed_work g_standby_idle_work;

app_env_t *ble_app_get_env(void)
{
    return &g_app_env;
}

int ble_app_is_talking(void)
{
    return is_talking;
}

static void send_rgb_led_command(rgb_led_cmd_t cmd, uint32_t color, uint32_t interval, uint32_t count)
{
    led_msg_t *pmsg = (led_msg_t *)rt_malloc(sizeof(led_msg_t));
    if (pmsg == RT_NULL)
    {
        rt_kprintf("RGB LED: alloc failed for message\n");
        return;
    }
    pmsg->target = LED_TARGET_RGB;
    pmsg->cmd = (uint32_t)cmd;
    pmsg->color = color;
    pmsg->blink_interval = interval;
    pmsg->blink_count = count;

    rt_err_t rc = rt_mb_send(g_rgb_led_mb, (rt_uint32_t)pmsg);
    if (rc != RT_EOK)
    {
        rt_kprintf("RGB LED: mailbox send failed %d\n", rc);
        rt_free(pmsg);
    }
}

static void send_nled_command(nled_cmd_t cmd, uint32_t interval, uint32_t count)
{
    led_msg_t *pmsg = (led_msg_t *)rt_malloc(sizeof(led_msg_t));
    if (pmsg == RT_NULL)
    {
        rt_kprintf("NLED: alloc failed for message\n");
        return;
    }
    pmsg->target = LED_TARGET_NLED;
    pmsg->cmd = (uint32_t)cmd;
    pmsg->color = 0;
    pmsg->blink_interval = interval;
    pmsg->blink_count = count;

    rt_err_t rc = rt_mb_send(g_rgb_led_mb, (rt_uint32_t)pmsg);
    if (rc != RT_EOK)
    {
        rt_kprintf("NLED: mailbox send failed %d\n", rc);
        rt_free(pmsg);
    }
}

/* ==================== Standby idle timer ==================== */

static void standby_idle_timer_arm(void)
{
    app_env_t *env = ble_app_get_env();
    if (env->state == DEVICE_STATE_STANDBY)
    {
        rt_work_cancel(&(g_standby_idle_work.work));
        rt_work_submit(&(g_standby_idle_work.work), STANDBY_IDLE_TIMEOUT_SECONDS * 1000);
    }
}

static inline void standby_idle_timer_cancel(void)
{
    rt_work_cancel(&(g_standby_idle_work.work));
}

/* ==================== State switch event callback ==================== */
static void app_on_phase_changed(ble_talk_phase_t old_phase, ble_talk_phase_t new_phase)
{
    app_env_t *env = ble_app_get_env();
    ble_talk_network_role_t role = ble_talk_network_get_role();

    switch (new_phase)
    {
    case BLE_TALK_PHASE_STANDBY:      env->state = DEVICE_STATE_STANDBY;      break;
    case BLE_TALK_PHASE_PAIRING:      env->state = DEVICE_STATE_PAIRING;      break;
    case BLE_TALK_PHASE_WAITING_TALK: env->state = DEVICE_STATE_WAITING_TALK; break;
    case BLE_TALK_PHASE_TALKING:      env->state = DEVICE_STATE_TALKING;      break;
    }
    env->role = (role == BLE_TALK_NETWORK_INITIATOR_ROLE) ? DEVICE_ROLE_MASTER : DEVICE_ROLE_SLAVE;

    /* LED 指示 */
    switch (new_phase)
    {
    case BLE_TALK_PHASE_STANDBY:
        rt_kprintf("in to standby\n");
        if (role == BLE_TALK_NETWORK_INITIATOR_ROLE)
            send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0x0000ff, 0, 0);
        else
            send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0xff0000, 0, 0);
        standby_idle_timer_arm();
        break;
    case BLE_TALK_PHASE_PAIRING:
        rt_kprintf("in to pairing\n");
        if (role == BLE_TALK_NETWORK_INITIATOR_ROLE)
            send_rgb_led_command(RGB_LED_CMD_BLINK, 0x0000ff, 1000, 0);
        else
            send_rgb_led_command(RGB_LED_CMD_BLINK, 0xff0000, 1000, 0);
        standby_idle_timer_cancel();
        break;
    case BLE_TALK_PHASE_WAITING_TALK:
        rt_kprintf("in to waiting talk\n");
        if (role == BLE_TALK_NETWORK_INITIATOR_ROLE)
            send_rgb_led_command(RGB_LED_CMD_BLINK, 0x0000ff, 1000, 0);
        else
            send_rgb_led_command(RGB_LED_CMD_BLINK, 0xff0000, 1000, 0);
        standby_idle_timer_cancel();
        break;
    case BLE_TALK_PHASE_TALKING:
        rt_kprintf("in to talking\n");
        send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0x00ff00, 0, 0);
        standby_idle_timer_cancel();
        break;
    }
}

static void app_on_pairing_timeout(void)
{
    app_env_t *env = ble_app_get_env();
    env->state = DEVICE_STATE_STANDBY;
    env->role = (ble_talk_network_get_role() == BLE_TALK_NETWORK_INITIATOR_ROLE)
                    ? DEVICE_ROLE_MASTER : DEVICE_ROLE_SLAVE;
    LOG_I("Pairing timeout, returned to standby");
}

static void app_on_reconnect_timeout(void)
{
    app_env_t *env = ble_app_get_env();
    env->state = DEVICE_STATE_STANDBY;
    LOG_I("Reconnect timeout, returned to standby");
    send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0xff0000, 0, 0);
    standby_idle_timer_arm();
}

static void app_on_room_full(void)
{
    app_env_t *env = ble_app_get_env();
    env->state = DEVICE_STATE_STANDBY;
    send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0xff0000, 0, 0);
    standby_idle_timer_arm();
    rt_kprintf("Room full, returned to standby\n");
}


static void app_on_scan_state_changed(uint8_t is_scanning)
{
    (void)is_scanning;
}

static void app_on_receiver_synced(uint8_t synced_num)
{
    (void)synced_num;
    send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0xffd500, 0, 0);
}

static void app_on_receiver_sync_stopped(uint8_t synced_num)
{
    if (synced_num == 0)
    {
        send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0x00ff00, 0, 0);
    }
}

/* ==================== low power ==================== */

static void standby_idle_timeout_handler(struct rt_work *work, void *work_data)
{
    app_env_t *env = ble_app_get_env();
    rt_kprintf("Sleep triggered\n");
    if (env->state == DEVICE_STATE_STANDBY)
    {
        rt_kprintf("Entering sleep mode\n");

        ble_talk_network_enter_idle();

        BSP_LCD_PowerDown();

        send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0x000000, 0, 0);

        env->role = DEVICE_ROLE_SLAVE;

        rt_pm_release(PM_SLEEP_MODE_IDLE);
        is_idle = 0;
    }
}

/* ==================== battery ==================== */

void battery_thread_entry(void *parameter)
{
    rt_device_t battery_device = rt_device_find("bat1");
    rt_adc_cmd_read_arg_t read_arg;
    read_arg.channel = 7;
    uint8_t percentage = 0;
    battery_calculator_t battery_calc;
    battery_calculator_config_t calc_config = {
        .charging_table = charging_curve_table,
        .charging_table_size = charging_curve_table_size,
        .discharging_table = discharge_curve_table,
        .discharging_table_size = discharge_curve_table_size,
        .charge_filter_threshold = 50,
        .discharge_filter_threshold = 30,
        .filter_count = 3,
        .secondary_filter_enabled = true,
        .secondary_filter_weight_pre = 90,
        .secondary_filter_weight_cur = 10
    };

    battery_calculator_init(&battery_calc, &calc_config);

    while (1)
    {
        rt_adc_enable((rt_adc_device_t)battery_device, read_arg.channel);
        rt_thread_mdelay(300);
        rt_uint32_t voltage = rt_adc_read((rt_adc_device_t)battery_device, read_arg.channel);
        rt_adc_disable((rt_adc_device_t)battery_device, read_arg.channel);
        if (!is_idle)
        {
#ifdef BSP_USING_BOARD_SF32LB52_LCHSPI_ULP
            BSP_GPIO_Set(26, 1, 1);
            HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_PULLUP, 1);
            HAL_PIN_Set(PAD_PA10, I2C2_SCL, PIN_PULLUP, 1);
            HAL_PIN_Set(PAD_PA11, I2C2_SDA, PIN_PULLUP, 1);
            rt_thread_mdelay(10);
#endif
            percentage = battery_calculator_get_percent(&battery_calc, voltage);
#ifdef BSP_USING_BOARD_SF32LB52_LCHSPI_ULP
            BSP_GPIO_Set(26, 0, 1);
            HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_NOPULL, 1);
            HAL_PIN_Set(PAD_PA10, GPIO_A10, PIN_PULLDOWN, 1);
            HAL_PIN_Set(PAD_PA11, GPIO_A11, PIN_PULLDOWN, 1);
#endif
        }
        else
        {
            percentage = battery_calculator_get_percent(&battery_calc, voltage);
        }
        rt_kprintf("Battery Voltage: %u mV, Percentage: %u%%\n", voltage, percentage);
        if (percentage < 20)
        {
            send_nled_command(NLED_CMD_BLINK, 1000, 0);
        }
        else if (percentage >= 20)
        {
            send_nled_command(NLED_CMD_STOP_BLINK, 0, 0);
        }

        rt_thread_mdelay(10000);
    }
}

/* ==================== LED 线程 ==================== */

static inline void nled_set_level(int on)
{
    BSP_GPIO_Set(NORMAL_LED_PIN,
                 (NORMAL_LED_ACTIVE_HIGH ? (on ? PIN_HIGH : PIN_LOW)
                                         : (on ? PIN_LOW : PIN_HIGH)), 1);
}

static void rgb_led_thread_entry(void *parameter)
{
    led_msg_t *msg = RT_NULL;

    enum { LED_MODE_STEADY = 0, LED_MODE_BLINK = 1 } led_mode_rgb = LED_MODE_STEADY;
    uint32_t steady_color = 0x000000;
    uint32_t blink_color = 0x000000;
    uint32_t blink_interval_rgb = 1000;
    uint32_t blink_remaining_rgb = 0;
    int on_phase_rgb = 0;
    rt_int32_t next_rgb_ticks = -1;

    enum { NLED_MODE_STEADY = 0, NLED_MODE_BLINK = 1 } led_mode_nled = NLED_MODE_STEADY;
    uint32_t blink_interval_nled = 1000;
    uint32_t blink_remaining_nled = 0;
    int on_phase_nled = 0;
    rt_int32_t next_nled_ticks = -1;

    rgb_led_init();
    rgb_led_set_color(steady_color);
    nled_set_level(0);

    while (1)
    {
        rt_int32_t wait_ticks = RT_WAITING_FOREVER;
        if (led_mode_rgb == LED_MODE_BLINK)
        {
            if (next_rgb_ticks < 0)
                next_rgb_ticks = rt_tick_from_millisecond((blink_interval_rgb == 0 ? 1000 : blink_interval_rgb) / 2);
            wait_ticks = next_rgb_ticks;
        }
        if (led_mode_nled == NLED_MODE_BLINK)
        {
            if (next_nled_ticks < 0)
                next_nled_ticks = rt_tick_from_millisecond((blink_interval_nled == 0 ? 1000 : blink_interval_nled) / 2);
            if (wait_ticks == RT_WAITING_FOREVER || next_nled_ticks < wait_ticks)
                wait_ticks = next_nled_ticks;
        }

        msg = RT_NULL;
        rt_err_t rc = rt_mb_recv(g_rgb_led_mb, (rt_uint32_t *)&msg, wait_ticks);
        if (rc == RT_EOK && msg != RT_NULL)
        {
            if (msg->target == LED_TARGET_RGB)
            {
                switch ((rgb_led_cmd_t)msg->cmd)
                {
                case RGB_LED_CMD_SET_COLOR:
                    led_mode_rgb = LED_MODE_STEADY;
                    steady_color = msg->color;
                    rgb_led_set_color(steady_color);
                    next_rgb_ticks = -1;
                    break;
                case RGB_LED_CMD_BLINK:
                    led_mode_rgb = LED_MODE_BLINK;
                    blink_color = msg->color;
                    blink_interval_rgb = (msg->blink_interval == 0) ? 1000 : msg->blink_interval;
                    blink_remaining_rgb = msg->blink_count;
                    on_phase_rgb = 1;
                    rgb_led_set_color(blink_color);
                    next_rgb_ticks = rt_tick_from_millisecond(blink_interval_rgb / 2);
                    break;
                case RGB_LED_CMD_STOP_BLINK:
                    led_mode_rgb = LED_MODE_STEADY;
                    steady_color = 0x000000;
                    rgb_led_set_color(steady_color);
                    next_rgb_ticks = -1;
                    break;
                default:
                    break;
                }
            }
            else
            {
                switch ((nled_cmd_t)msg->cmd)
                {
                case NLED_CMD_SET_ON:
                    led_mode_nled = NLED_MODE_STEADY;
                    on_phase_nled = 1;
                    nled_set_level(1);
                    rt_kprintf("NLED: ON\n");
                    next_nled_ticks = -1;
                    break;
                case NLED_CMD_SET_OFF:
                    led_mode_nled = NLED_MODE_STEADY;
                    on_phase_nled = 0;
                    nled_set_level(0);
                    rt_kprintf("NLED: OFF\n");
                    next_nled_ticks = -1;
                    break;
                case NLED_CMD_BLINK:
                    led_mode_nled = NLED_MODE_BLINK;
                    blink_interval_nled = (msg->blink_interval == 0) ? 1000 : msg->blink_interval;
                    blink_remaining_nled = msg->blink_count;
                    on_phase_nled = 1;
                    nled_set_level(1);
                    next_nled_ticks = rt_tick_from_millisecond(blink_interval_nled / 2);
                    break;
                case NLED_CMD_STOP_BLINK:
                    led_mode_nled = NLED_MODE_STEADY;
                    on_phase_nled = 0;
                    nled_set_level(0);
                    rt_kprintf("NLED: STOP_BLINK -> OFF\n");
                    next_nled_ticks = -1;
                    break;
                default:
                    break;
                }
            }
            rt_free(msg);
            continue;
        }

        if (rc == -RT_ETIMEOUT)
        {
            rt_int32_t elapsed = wait_ticks;
            if (led_mode_rgb == LED_MODE_BLINK && next_rgb_ticks >= 0)
            {
                next_rgb_ticks -= elapsed;
                if (next_rgb_ticks <= 0)
                {
                    on_phase_rgb = !on_phase_rgb;
                    rgb_led_set_color(on_phase_rgb ? blink_color : 0x000000);
                    if (blink_remaining_rgb > 0 && on_phase_rgb == 0)
                    {
                        if (--blink_remaining_rgb == 0)
                        {
                            led_mode_rgb = LED_MODE_STEADY;
                            steady_color = 0x000000;
                            rgb_led_set_color(steady_color);
                            next_rgb_ticks = -1;
                        }
                        else
                        {
                            next_rgb_ticks = rt_tick_from_millisecond(blink_interval_rgb / 2);
                        }
                    }
                    else if (led_mode_rgb == LED_MODE_BLINK)
                    {
                        next_rgb_ticks = rt_tick_from_millisecond(blink_interval_rgb / 2);
                    }
                }
            }

            if (led_mode_nled == NLED_MODE_BLINK && next_nled_ticks >= 0)
            {
                next_nled_ticks -= elapsed;
                if (next_nled_ticks <= 0)
                {
                    on_phase_nled = !on_phase_nled;
                    nled_set_level(on_phase_nled);
                    if (blink_remaining_nled > 0 && on_phase_nled == 0)
                    {
                        if (--blink_remaining_nled == 0)
                        {
                            led_mode_nled = NLED_MODE_STEADY;
                            on_phase_nled = 0;
                            nled_set_level(0);
                            next_nled_ticks = -1;
                        }
                        else
                        {
                            next_nled_ticks = rt_tick_from_millisecond(blink_interval_nled / 2);
                        }
                    }
                    else if (led_mode_nled == NLED_MODE_BLINK)
                    {
                        next_nled_ticks = rt_tick_from_millisecond(blink_interval_nled / 2);
                    }
                }
            }
        }
    }
}

/* ==================== button==================== */

/*
 * 按键1（KEY1）处理：
 *   - 对讲中短按 → 退出房间 (leave_room)
 *   - 待机 + 回连待处理 → 回连 (reconnect)
 */
static void ble_app_button_event_handler(int32_t pin, button_action_t action)
{
    app_env_t *env = ble_app_get_env();
    LOG_I("button(%d) %d, click(%d)", pin, action, env->click);

    /* 任意按键动作：若处于待机，则重置倒计时 */
    if (env->state == DEVICE_STATE_STANDBY)
    {
        standby_idle_timer_arm();
        rt_kprintf("Reset sleep countdown\n");
    }
    if (!is_idle)
    {
        BSP_LCD_PowerUp();
        is_idle = 1;
        rt_pm_request(PM_SLEEP_MODE_IDLE);
    }

    if (action == BUTTON_CLICKED)
    {
        if (env->state == DEVICE_STATE_TALKING)
        {
            /* 发言中不允许退出房间 */
            if (is_talking)
            {
                LOG_I("Cannot leave room while speaking");
                return;
            }
            /* 退出房间：驱动自动区分 Slave/Master 并执行对应逻辑 */
            ble_talk_network_leave_room();
            is_talking = 0;
            LOG_I("Left room");
        }
        else if (env->state == DEVICE_STATE_STANDBY &&
                 ble_talk_network_is_reconnect_pending())
        {
            /* Slave 回连：驱动自动恢复房间/token/JOIN */
            LOG_I("Slave reconnecting");
            send_rgb_led_command(RGB_LED_CMD_BLINK, 0xff0000, 1000, 0);
            ble_talk_network_reconnect();
        }
    }
}

/*
 * 按键2（KEY2）处理：
 *   - 对讲中按住 → PTT 发言
 *   - 待机短按   → 切换角色
 *   - 待机长按   → 进入配对（Master 创建房间 / Slave 扫描房间）
 *   - 待对讲长按 → Master 确认开始对讲
 */
static void ble_app_key2_event_handler(int32_t pin, button_action_t action)
{
    app_env_t *env = ble_app_get_env();

    if (env->state == DEVICE_STATE_STANDBY)
    {
        standby_idle_timer_arm();
        rt_kprintf("Reset sleep countdown\n");
    }
    if (!is_idle)
    {
        BSP_LCD_PowerUp();
        is_idle = 1;
        rt_pm_request(PM_SLEEP_MODE_IDLE);
    }

    /* PTT 按住发言 */
    if (action == BUTTON_PRESSED)
    {
        rt_kprintf("Button pressed\n");
        if (env->state != DEVICE_STATE_TALKING)
        {
            LOG_I("Not in talking state, cannot start\n");
            return;
        }
        if (!(ble_app_receiver_get_synced_num() > 0 || strlen(ble_talk_network_get_room_id()) > 0))
        {
            LOG_I("No device in room, cannot start\n");
            return;
        }
        if (!is_talking)
        {
            rt_kprintf("Start speaking\n");
            uint8_t ret = ble_app_sender_trigger();
            if (ret == 0)
                is_talking = 1;
        }
    }
    else if (action == BUTTON_RELEASED)
    {
        if (is_talking)
        {
            rt_kprintf("Stop speaking\n");
            ble_app_sender_stop();
            is_talking = 0;
        }
    }

    /* 短按切换角色 */
    if (action == BUTTON_CLICKED)
    {
        if (env->state == DEVICE_STATE_STANDBY)
        {
            ble_talk_network_role_t new_role = ble_talk_network_switch_role();
            env->role = (new_role == BLE_TALK_NETWORK_INITIATOR_ROLE)
                            ? DEVICE_ROLE_MASTER : DEVICE_ROLE_SLAVE;
            if (new_role == BLE_TALK_NETWORK_INITIATOR_ROLE)
            {
                send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0x0000ff, 0, 0);
                LOG_I("Switched to master");
            }
            else
            {
                send_rgb_led_command(RGB_LED_CMD_SET_COLOR, 0xff0000, 0, 0);
                LOG_I("Switched to slave");
            }
        }
    }
    /* 长按：配对 / 确认对讲 */
    else if (action == BUTTON_LONG_PRESSED)
    {
        if (env->state == DEVICE_STATE_STANDBY)
        {
            LOG_I("Entering pairing mode");
            if (env->role == DEVICE_ROLE_MASTER)
            {
                /* Master 创建房间*/
                ble_talk_network_create_room();
                LOG_I("Room created, entering pairing state");
            }
            else
            {
                /* Slave 扫描房间*/
                ble_talk_network_scan_rooms();
                LOG_I("Slave scanning for rooms");
            }
        }
        else if (env->state == DEVICE_STATE_WAITING_TALK)
        {
            if (env->role == DEVICE_ROLE_MASTER)
            {
                /* Master 确认开始对讲，发送 SYNC */
                LOG_I("Master starting talk, sending confirm");
                ble_talk_network_confirm_talking();
            }
            else
            {
                LOG_I("Waiting for master, in waiting-talk state");
            }
        }
    }
}

/* ==================== 开关机按键 ==================== */

HAL_RAM_RET_CODE_SECT(PowerDownCustom, void PowerDownCustom(void))
{
    rt_kprintf("PowerDownCustom\n");

    HAL_PMU_SelectWakeupPin(0, 19); // PA43
    HAL_PMU_EnablePinWakeup(0, 0);

    HAL_PIN_Set(PAD_PA24, GPIO_A24, PIN_PULLDOWN, 1);
    for (uint32_t i = PAD_PA28; i <= PAD_PA44; i++)
    {
        HAL_PIN_Set(i, (pin_function)(i - PAD_PA28 + GPIO_A28), PIN_PULLDOWN, 1);
    }
    hwp_pmuc->PERI_LDO &= ~(PMUC_PERI_LDO_EN_LDO18 | PMUC_PERI_LDO_EN_VDD33_LDO2 | PMUC_PERI_LDO_EN_VDD33_LDO3);
    hwp_pmuc->WKUP_CNT = 0x000F000F;

    rt_hw_interrupt_disable();
    rt_kprintf("PowerDownCustom2\n");
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO2_3V3, false, false);
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, false, false);
    HAL_PMU_EnterHibernate();
}

static void ble_app_button3_event_handler(int32_t pin, button_action_t action)
{
    if (action == BUTTON_LONG_PRESSED)
    {
        rt_kprintf("long pressed\n");
        PowerDownCustom();
    }
}

/* ==================== 按键初始化 ==================== */

static void ble_app_button_init(void)
{
    button_cfg_t cfg;

    cfg.pin = BSP_KEY1_PIN;
    cfg.active_state = BSP_KEY1_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT;
    cfg.button_handler = ble_app_button_event_handler;
    int32_t id = button_init(&cfg);
    RT_ASSERT(id >= 0);
    RT_ASSERT(SF_EOK == button_enable(id));

    cfg.pin = BSP_KEY2_PIN;
    cfg.active_state = BSP_KEY2_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT;
    cfg.button_handler = ble_app_key2_event_handler;
    int32_t id2 = button_init(&cfg);
    RT_ASSERT(id2 >= 0);
    RT_ASSERT(SF_EOK == button_enable(id2));
}

/* ==================== 开机检测 ==================== */

void check_poweron_reason(void)
{
    switch (SystemPowerOnModeGet())
    {
    case PM_REBOOT_BOOT:
    case PM_COLD_BOOT:
    {
        break;
    }
    case PM_HIBERNATE_BOOT:
    case PM_SHUTDOWN_BOOT:
    {
        if (PMUC_WSR_RTC & pm_get_wakeup_src())
        {
            NVIC_EnableIRQ(RTC_IRQn);
        }
        else if (PMUC_WSR_PIN_ALL & pm_get_wakeup_src())
        {
            rt_thread_mdelay(1000);
            int val = rt_pin_read(BSP_KEY2_PIN);
            rt_kprintf("Power key level after 1s: %d\n", val);
            if (val != 1)
            {
                rt_kprintf("Not long press, shutdown now.\n");
                PowerDownCustom();
                while (1) {};
            }
            else
            {
                rt_kprintf("Long press detected, power on as normal.\n");
            }
        }
        else if (0 == pm_get_wakeup_src())
        {
            RT_ASSERT(0);
        }
        break;
    }
    default:
    {
        RT_ASSERT(0);
    }
    }
}

/* ==================== 主函数 ==================== */

int main(void)
{
    check_poweron_reason();
    app_env_t *env = ble_app_get_env();

    /* 创建邮箱和 LED 线程 */
    g_rgb_led_mb = rt_mb_create("rgb_mb", 8, RT_IPC_FLAG_FIFO);
    g_rgb_led_thread = rt_thread_create("rgb_led", rgb_led_thread_entry, RT_NULL,
                                        1024, 6, 10);
    if (g_rgb_led_thread)
    {
        rt_thread_startup(g_rgb_led_thread);
    }

    /* 初始化设备状态 */
    env->role = DEVICE_ROLE_SLAVE;
    env->state = DEVICE_STATE_STANDBY;
    rgb_led_set_color(0xff0000);
    env->mb_handle = rt_mb_create("app", 8, RT_IPC_FLAG_FIFO);

    /* 初始化待机空闲倒计时 */
    rt_delayed_work_init(&g_standby_idle_work, standby_idle_timeout_handler, NULL);
    standby_idle_timer_arm();

    /* 初始化组网（内部管理配对/回连超时定时器） */
    ble_talk_network_init();

    /* 注册回调 — 仅做 UI/LED 更新 */
    ble_talk_network_callbacks_t cbs = {
        .on_phase_changed    = app_on_phase_changed,
        .on_pairing_timeout  = app_on_pairing_timeout,
        .on_reconnect_timeout = app_on_reconnect_timeout,
        .on_room_full        = app_on_room_full,
    };
    ble_talk_network_register_callbacks(&cbs);

    /* 注册底层操作接口 */
    ble_talk_network_ops_t ops = {
        .scan_enable  = (void (*)(void))ble_app_scan_enable,
        .scan_stop    = (void (*)(void))ble_app_scan_stop,
        .is_speaking  = (uint8_t (*)(void))ble_app_is_talking,
        .sender_stop  = (void (*)(void))ble_app_sender_stop,
    };
    ble_talk_network_set_ops(&ops);

    /* 注册对讲回调 */
    ble_talk_callbacks_t talk_cbs = {
        .on_scan_state_changed   = app_on_scan_state_changed,
        .on_receiver_synced      = app_on_receiver_synced,
        .on_receiver_sync_stopped = app_on_receiver_sync_stopped,
    };
    ble_talk_register_callbacks(&talk_cbs);

    /* 初始化按键 */
    ble_app_button_init();
    /* 启用 BLE */
    sifli_ble_enable();
    /* 初始化音频 */
    ble_app_sender_init();
    ble_app_receviver_init();

    /* 电池监测线程 */
    g_battery_thread = rt_thread_create("battery", battery_thread_entry, RT_NULL,
                                        1024, 5, 10);
    if (g_battery_thread)
    {
        rt_thread_startup(g_battery_thread);
    }

    /* 主循环 */
    while (1)
    {
        uint32_t value;
        rt_mb_recv(env->mb_handle, (rt_uint32_t *)&value, RT_WAITING_FOREVER);
    }
    return RT_EOK;
}

/* ==================== BLE 地址与事件 ==================== */

#ifndef NVDS_AUTO_UPDATE_MAC_ADDRESS_ENABLE
ble_common_update_type_t ble_request_public_address(bd_addr_t *addr)
{
    int ret = bt_mac_addr_generate_via_uid_v2(addr);
    if (ret != 0)
    {
        LOG_I("generate mac addres failed %d", ret);
        return BLE_UPDATE_NO_UPDATE;
    }
    return BLE_UPDATE_ONCE;
}
#endif

int ble_app_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    app_env_t *env = ble_app_get_env();
    int ret = 0;
    switch (event_id)
    {
    case BLE_POWER_ON_IND:
    {
        env->is_power_on = 1;
        ble_app_scan_init();
        ble_app_peri_advertising_init();
        ble_talk_network_advertising_init();
        LOG_I("receive BLE power on!\r\n");
        break;
    }
    case BLE_GAP_SCAN_START_CNF:
    case BLE_GAP_SCAN_STOP_CNF:
    case BLE_GAP_SCAN_STOPPED_IND:
    case BLE_GAP_EXT_ADV_REPORT_IND:
    {
        /* 转发给网络 */
        ble_talk_network_event_handler(event_id, data, len, context);
        /* 继续处理音频接收 */
        ret = ble_app_receiver_event_handler(event_id, data, len, context);
        break;
    }
    case BLE_GAP_CREATE_PERIODIC_ADV_SYNC_CNF:
    case BLE_GAP_START_PERIODIC_ADV_SYNC_CNF:
    case BLE_GAP_DELETE_PERIODIC_ADV_SYNC_CNF:
    case BLE_GAP_PERIODIC_ADV_SYNC_CREATED_IND:
    case BLE_GAP_PERIODIC_ADV_SYNC_STOPPED_IND:
    case BLE_GAP_PERIODIC_ADV_SYNC_ESTABLISHED_IND:
    {
        ret = ble_app_receiver_event_handler(event_id, data, len, context);
    }
    default:
        break;
    }
    return ret;
}
BLE_EVENT_REGISTER(ble_app_event_handler, NULL);

#ifdef SF32LB52X_58
uint16_t g_em_offset[HAL_LCPU_CONFIG_EM_BUF_MAX_NUM] =
{
    0x178, 0x178, 0x740, 0x7A0, 0x810, 0x880, 0xA00, 0xBB0, 0xD48,
    0x133C, 0x13A4, 0x19BC, 0x21BC, 0x21BC, 0x21BC, 0x21BC, 0x21BC, 0x21BC,
    0x21BC, 0x21BC, 0x263C, 0x265C, 0x2734, 0x2784, 0x28D4, 0x28E8, 0x28FC,
    0x29EC, 0x29FC, 0x2BBC, 0x2BD8, 0x3BE8, 0x5804, 0x5804, 0x5804
};

void lcpu_rom_config(void)
{
    hal_lcpu_bluetooth_em_config_t em_offset;
    memcpy((void *)em_offset.em_buf, (void *)g_em_offset, HAL_LCPU_CONFIG_EM_BUF_MAX_NUM * 2);
    em_offset.is_valid = 1;
    HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_EM_BUF, &em_offset, sizeof(hal_lcpu_bluetooth_em_config_t));

    hal_lcpu_bluetooth_act_configt_t act_cfg;
    act_cfg.ble_max_act = 6;
    act_cfg.ble_max_iso = 0;
    act_cfg.ble_max_ral = 3;
    act_cfg.bt_max_acl = 7;
    act_cfg.bt_max_sco = 0;
    act_cfg.bit_valid = CO_BIT(0) | CO_BIT(1) | CO_BIT(2) | CO_BIT(3) | CO_BIT(4);
    HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_ACT_CFG, &act_cfg, sizeof(hal_lcpu_bluetooth_act_configt_t));
}
#endif

