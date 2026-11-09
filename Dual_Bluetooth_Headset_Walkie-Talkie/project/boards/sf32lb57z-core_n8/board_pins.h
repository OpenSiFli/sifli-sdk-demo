/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "bf0_hal.h"

/*
 * Kconfig stores pin numbers as plain integers (2..57).  PAD enum names are
 * zero padded (PAD_PA02), so a small table is used instead of token pasting.
 */
static inline pin_pad board_pad(int number)
{
    static const pin_pad pads[] =
    {
        PAD_PA00,
        PAD_PA01,
        PAD_PA02,
        PAD_PA03,
        PAD_PA04,
        PAD_PA05,
        PAD_PA06,
        PAD_PA07,
        PAD_PA08,
        PAD_PA09,
        PAD_PA10,
        PAD_PA11,
        PAD_PA12,
        PAD_PA13,
        PAD_PA14,
        PAD_PA15,
        PAD_PA16,
        PAD_PA17,
        PAD_PA18,
        PAD_PA19,
        PAD_PA20,
        PAD_PA21,
        PAD_PA22,
        PAD_PA23,
        PAD_PA24,
        PAD_PA25,
        PAD_PA26,
        PAD_PA27,
        PAD_PA28,
        PAD_PA29,
        PAD_PA30,
        PAD_PA31,
        PAD_PA32,
        PAD_PA33,
        PAD_PA34,
        PAD_PA35,
        PAD_PA36,
        PAD_PA37,
        PAD_PA38,
        PAD_PA39,
        PAD_PA40,
        PAD_PA41,
        PAD_PA42,
        PAD_PA43,
        PAD_PA44,
        PAD_PA45,
        PAD_PA46,
        PAD_PA47,
        PAD_PA48,
        PAD_PA49,
        PAD_PA50,
        PAD_PA51,
        PAD_PA52,
        PAD_PA53,
        PAD_PA54,
        PAD_PA55,
        PAD_PA56,
        PAD_PA57
    };

    return pads[number];
}

#define BOARD_PAD(n)     board_pad((n))
#define _BOARD_GPIO(n)   GPIO_A##n
#define BOARD_GPIO(n)    _BOARD_GPIO(n)

#endif /* BOARD_PINS_H */
