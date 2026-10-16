/* SPDX-License-Identifier: Apache-2.0 */
#include <bf0_hal.h>
#include <drv_io.h>
#include <rtconfig.h>

#if !defined(BSP_USING_BOARD_DPI_HDK_LB57EYBBN6_N16_QSPI) || \
    !defined(BOARD_DVP_PINMAP_0) || \
    !defined(LCD_USING_HTM_H070A20_RGB_A01)
#error "camera_board.c requires the HTM DPI board with DCMI pinmap 0"
#endif

#if !defined(TOUCH_IRQ_PIN) || TOUCH_IRQ_PIN != 6
#error "camera_board.c requires the touch IRQ on PA06"
#endif

/* The camera framework owns DCMI hardware and calls HAL_DCMI_Init(), whose
 * weak MspInit hook is the only place the DVP pinmux is configured once the
 * SDK's DCMI device driver is disabled. XCLK is configured separately by the
 * camera framework. */
void HAL_DCMI_MspInit(DCMI_HandleTypeDef *hdcmi)
{
    (void)hdcmi;
    HAL_PIN_Set(PAD_PA00, DCMI_VSYNC, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA01, DCMI_HSYNC, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA03, DCMI_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA04, DCMI_DI0, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA05, DCMI_DI3, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA07, DCMI_DI5, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA08, DCMI_DI1, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA09, DCMI_DI2, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA10, DCMI_DI4, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA35, DCMI_DI7, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA36, DCMI_DI6, PIN_NOPULL, 1);
}

/* The HTM touch reset line is not connected on this board. PA07 carries
 * DCMI_D5, so the touch lifecycle hooks only configure the PA06 IRQ pin. */
void __wrap_BSP_TP_PowerUp(void)
{
    HAL_PIN_Set(PAD_PA00 + TOUCH_IRQ_PIN, GPIO_A0 + TOUCH_IRQ_PIN,
                PIN_PULLUP, 1);
}

void __wrap_BSP_TP_PowerDown(void)
{
    HAL_PIN_Set(PAD_PA00 + TOUCH_IRQ_PIN, GPIO_A0 + TOUCH_IRQ_PIN,
                PIN_NOPULL, 1);
}

void __wrap_BSP_TP_Reset(uint8_t high1_low0)
{
    (void)high1_low0;
}
