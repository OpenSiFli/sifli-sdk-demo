/*
 * SPDX-FileCopyrightText: 2024-2024 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MAIN_H_
#define MAIN_H_


#include "bf0_ble_gap.h"

#include "audio_server.h"
#include "log.h"

#ifdef __cplusplus
extern "C" {
#endif
#undef LOG_TAG
#define LOG_TAG "ble_app"


// led
#define NORMAL_LED_ACTIVE_HIGH 1
#define NORMAL_LED_PIN 9  //普通灯，随便给个pin脚，后面板子好了再调整

typedef enum {
    DEVICE_ROLE_SLAVE,
    DEVICE_ROLE_MASTER
} device_role_t;

typedef enum {
    DEVICE_STATE_STANDBY,
    DEVICE_STATE_PAIRING,
    DEVICE_STATE_WAITING_TALK,
    DEVICE_STATE_TALKING
} device_state_t;

typedef enum {
    RGB_LED_CMD_SET_COLOR,
    RGB_LED_CMD_BLINK,
    RGB_LED_CMD_STOP_BLINK
} rgb_led_cmd_t;

typedef struct {
    rgb_led_cmd_t cmd;
    uint32_t color;
    uint32_t blink_interval;  // 闪烁间隔(ms)
    uint32_t blink_count;     // 闪烁次数
} rgb_led_msg_t;

typedef enum { 
    LED_TARGET_RGB = 0, 
    LED_TARGET_NLED = 1 
} led_target_t;

typedef enum { 
    NLED_CMD_SET_ON = 1, 
    NLED_CMD_SET_OFF = 2, 
    NLED_CMD_BLINK = 3, 
    NLED_CMD_STOP_BLINK = 4 
} nled_cmd_t;

typedef struct {
    led_target_t target;        // RGB 或 普通LED
    uint32_t cmd;               // 对 RGB 用 rgb_led_cmd_t；对 NLED 用 nled_cmd_t
    uint32_t color;             // 仅 RGB 使用
    uint32_t blink_interval;    // ms
    uint32_t blink_count;       // 0=无限
} led_msg_t;

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

typedef struct
{
    uint8_t is_power_on;

    uint8_t click;
    // Mbox thread
    rt_mailbox_t mb_handle;
    device_role_t role;           // 设备角色（从组件同步）
    device_state_t state;         // 设备状态（从组件同步）
} app_env_t;

#ifdef __cplusplus
}
#endif
#endif // MAIN_H_

