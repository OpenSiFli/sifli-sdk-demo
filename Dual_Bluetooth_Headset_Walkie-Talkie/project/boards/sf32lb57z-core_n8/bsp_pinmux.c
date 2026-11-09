/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp_board.h"
#include "board_pins.h"

/*
 * Board pin routing.
 *
 * PAD numbers come from Kconfig (BOARD_57Z_*_PIN) so the board can be
 * adapted to the final schematic without touching application code.
 * The 57X pin matrix allows I2S/USART functions on any bonded pad.
 */

void BSP_PIN_Init(void)
{
#ifdef SOC_BF0_HCPU
    /* The 57ZUN3N6 package contains a 1 MB SiP NOR on MPI2.  Its pins are
     * fixed (PAD_SBxx) and are selected by this helper, not by PA60..PA68. */
    HAL_PIN_SetSipFlash2(PIN_MPI_PINMAP_MODE_5);

    /* Console UART1: PA18 RX / PA19 TX by default. */
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_UART_RX_PIN), USART1_RXD, PIN_PULLUP, 1);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_UART_TX_PIN), USART1_TXD, PIN_NOPULL, 1);

    /* I2S1: relay firmware receives on SDI, output firmware transmits on SDO. */
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_MCLK_PIN), I2S1_MCLK, PIN_NOPULL, 1);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_SDO_PIN), I2S1_SDO, PIN_NOPULL, 1);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_SDI_PIN), I2S1_SDI, PIN_PULLDOWN, 1);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_BCK_PIN), I2S1_BCK, PIN_NOPULL, 1);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_LRCK_PIN), I2S1_LRCK, PIN_NOPULL, 1);

    /* Headset status LEDs, driven as plain GPIO by the application. */
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_LED1_PIN), BOARD_GPIO(BOARD_57Z_LED1_PIN), PIN_NOPULL, 1);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_LED2_PIN), BOARD_GPIO(BOARD_57Z_LED2_PIN), PIN_NOPULL, 1);
#endif /* SOC_BF0_HCPU */
}
