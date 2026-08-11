/*
 * ACPU early clock setup.
 *
 * The SDK leaves the ACPU system clock at 48 MHz by default. Doubling it to
 * 96 MHz roughly halves JPEG encode/decode time. The switch happens in
 * hw_preinit0() before RT-Thread starts; the SDK clock query does not know
 * DBL96, so SysTick is re-configured with SystemCoreClock after RT-Thread
 * board init to keep the tick rate at 1 ms.
 */
#include <bf0_hal_cortex.h>
#include <bf0_hal_rcc.h>
#include <rtconfig.h>
#include <rtthread.h>
#include <stdint.h>

extern uint32_t SystemCoreClock;

void hw_preinit0(void)
{
    HAL_RCC_LCPU_ClockSelectDBL96();
    SystemCoreClock = 96000000U;
}

static int camera_acpu_tick_reconfigure(void)
{
    HAL_SYSTICK_Config(SystemCoreClock / RT_TICK_PER_SECOND);
    return 0;
}
INIT_APP_EXPORT(camera_acpu_tick_reconfigure);
