/**
  ******************************************************************************
  * @file   range_test.c
  * @brief  Core-3p3 蓝牙拉距测试:跳线读取、发射功率下发、PA32 LED 状态机。
  *
  * 跳线真值表(IO 默认低电平,拉高=接 3.3V):
  *   PA27  低/无 = 发射模式(TX) ; 高 = 接收模式(RX)
  *   PA26  拉高 = 10dBm
  *   PA25  拉高 = 13dBm
  *   PA24  拉高 = 16dBm
  *   三者都不接 = 19dBm(最大/默认)
  *   PA32  LED(高=亮)
  ******************************************************************************
  */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "bf0_hal_lcpu_config.h"
#include "lcpu_config_type.h"
#include "mem_map.h"
#include "range_test.h"

#define LOG_TAG "range_io"
#include "log.h"

/* 功率打包宏:max/min/init 各 8bit + bqb 标志 */
#define RT_RF_PWR_PARA(max, min, init, is_bqb) \
    ((uint32_t)(((uint32_t)(is_bqb) << 24) | ((uint32_t)(uint8_t)(init) << 16) | \
                ((uint32_t)(uint8_t)(min) << 8) | (uint8_t)(int8_t)(max)))

/* 引脚(端口 A,使用 hwp_gpio1;Pin 号即 PAxx 编号,与 board.conf LED1_PIN=32 一致) */
#define PIN_ROLE     27
#define PIN_PWR_10   26
#define PIN_PWR_13   25
#define PIN_PWR_16   24
#define PIN_LED      32

#define LED_TICK_MS    20
#define TICKS_PER_SEC  (1000 / LED_TICK_MS) /* 50 */

static range_test_role_t                g_role       = RANGE_TEST_ROLE_TX;
static int8_t                           g_tx_power   = 19;
static volatile range_test_led_state_t  g_led_state  = RANGE_TEST_LED_SEARCHING;
static rt_timer_t                       g_led_timer  = RT_NULL;
static uint32_t                         g_led_tick   = 0;

/* 复用为 GPIO 输入下拉并读电平 */
static int jumper_read(int pad, int func, int pin)
{
    GPIO_InitTypeDef init = {0};

    HAL_PIN_Set(pad, func, PIN_PULLDOWN, 1);
    init.Pin  = pin;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(hwp_gpio1, &init);
    HAL_Delay_us(20);
    return (HAL_GPIO_ReadPin(hwp_gpio1, pin) == GPIO_PIN_SET) ? 1 : 0;
}

static void led_write(int on)
{
    HAL_GPIO_WritePin(hwp_gpio1, PIN_LED, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void led_timer_cb(void *param)
{
    int on = 0;
    uint32_t t = g_led_tick % TICKS_PER_SEC;   /* 0..49 */
    (void)param;

    switch (g_led_state)
    {
    case RANGE_TEST_LED_CONNECTED:
        on = 1;
        break;

    case RANGE_TEST_LED_SEARCHING:
    default:
        if (g_role == RANGE_TEST_ROLE_RX)
            on = (t < 5);                       /* RX: 1Hz 单闪 100ms */
        else
            on = (t < 5) || (t >= 10 && t < 15);/* TX: 双闪 100/100/100/700 */
        break;
    }

    led_write(on);
    g_led_tick++;
}

void range_test_led_set_state(range_test_led_state_t state)
{
    g_led_state = state;
}

range_test_role_t range_test_get_role(void)    { return g_role; }
int8_t            range_test_get_tx_power(void) { return g_tx_power; }

void range_test_io_init(void)
{
    GPIO_InitTypeDef init = {0};

    g_role = jumper_read(PAD_PA27, GPIO_A27, PIN_ROLE) ? RANGE_TEST_ROLE_RX
                                                       : RANGE_TEST_ROLE_TX;

    /* 优先级 PA26(10) > PA25(13) > PA24(16);都不接 = 19dBm */
    if      (jumper_read(PAD_PA26, GPIO_A26, PIN_PWR_10)) g_tx_power = 10;
    else if (jumper_read(PAD_PA25, GPIO_A25, PIN_PWR_13)) g_tx_power = 13;
    else if (jumper_read(PAD_PA24, GPIO_A24, PIN_PWR_16)) g_tx_power = 16;
    else                                                  g_tx_power = 19;

    LOG_I("Range test boot: role=%s, tx_power=%ddBm",
          (g_role == RANGE_TEST_ROLE_RX) ? "RX(recv)" : "TX(send)", g_tx_power);

    HAL_PIN_Set(PAD_PA32, GPIO_A32, PIN_NOPULL, 1);
    init.Pin  = PIN_LED;
    init.Mode = GPIO_MODE_OUTPUT;
    init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(hwp_gpio1, &init);
    led_write(0);
    g_led_state = RANGE_TEST_LED_SEARCHING;

    g_led_timer = rt_timer_create("rt_led", led_timer_cb, RT_NULL,
                                  rt_tick_from_millisecond(LED_TICK_MS),
                                  RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    if (g_led_timer)
        rt_timer_start(g_led_timer);
}

/* 重写 __WEAK 的 HAL_LCPU_CONFIG_set_core() 拦截 BT_TX_PWR 写入,
 * 配合 proj.conf BT_TX_POWER_VAL_MAX=19 钉死发射功率 */

/* 与 HAL_LCPU_CONIFG_init() 一致地推算 LCPU 共享配置区基址 */
static uint8_t *rt_lcpu_cfg_base(void)
{
#ifdef SF32LB52X
    if (__HAL_SYSCFG_GET_REVID() >= HAL_CHIP_REV_ID_A4)
        return (uint8_t *)LCPU2HCPU_MB_CH2_BUF_REV_B_START_ADDR;
#endif
    return (uint8_t *)LCPU_CONFIG_START_ADDR;
}

HAL_StatusTypeDef HAL_LCPU_CONFIG_set_core(uint8_t config_type, void *value, uint16_t length)
{
    uint32_t pwr;

    if (config_type == HAL_LCPU_CONFIG_BT_TX_PWR)
    {
        int8_t p = g_tx_power;
        pwr = RT_RF_PWR_PARA(p, p, p, 0);
        value = &pwr;
        length = sizeof(pwr);
        LOG_I("Override BT TX power -> %ddBm", p);
    }

    return (LCPU_CONFIG_set(rt_lcpu_cfg_base(), config_type, (uint8_t *)value, length) == 0)
           ? HAL_OK : HAL_ERROR;
}
