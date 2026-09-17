/* SPDX-License-Identifier: Apache-2.0 */
#include <bf0_hal.h>
#include <drv_io.h>
#include <rtconfig.h>

#if !defined(BSP_USING_BOARD_DPI_HDK_LB575YBBN6_N16_QSPI) || \
    !defined(BOARD_DVP_PINMAP_0) || \
    !defined(LCD_USING_HTM_H070A20_RGB_A01)
#error "camera_board.c requires the HTM DPI board with DCMI pinmap 0"
#endif

#if !defined(TOUCH_IRQ_PIN) || TOUCH_IRQ_PIN != 6
#error "camera_board.c requires the touch IRQ on PA06"
#endif

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
