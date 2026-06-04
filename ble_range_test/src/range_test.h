/**
  ******************************************************************************
  * @file   range_test.h
  * @brief  Core-3p3 蓝牙拉距测试:开机跳线读取、发射功率档位、PA32 LED 指示。
  ******************************************************************************
  */
#ifndef __RANGE_TEST_H__
#define __RANGE_TEST_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 蓝牙角色,由 PA27 跳线在开机时决定 */
typedef enum
{
    RANGE_TEST_ROLE_TX = 0, /* PA27 低/无跳线:发射模式(central,主动扫描连接并持续发数据) */
    RANGE_TEST_ROLE_RX = 1, /* PA27 高:接收模式(peripheral,广播并接收数据) */
} range_test_role_t;

typedef enum
{
    RANGE_TEST_LED_SEARCHING = 0, /* 未扫到候选:TX=每秒双闪 / RX=1Hz 单次慢闪 */
    RANGE_TEST_LED_CONNECTING,    /* TX 已扫到候选、正在 connect:呼吸渐变 (1Hz 三角波) */
    RANGE_TEST_LED_CONNECTED,     /* 已连接:常亮 */
} range_test_led_state_t;

/**
 * @brief 开机读取跳线(PA27 角色 + PA26/25/24 功率档)并初始化 PA32 LED。
 *        必须在 sifli_ble_enable() 之前调用——发射功率经弱函数在 BT 初始化时下发。
 */
void range_test_io_init(void);

range_test_role_t range_test_get_role(void);
int8_t            range_test_get_tx_power(void); /* 返回 10/13/16/19 dBm */

void range_test_led_set_state(range_test_led_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* __RANGE_TEST_H__ */
