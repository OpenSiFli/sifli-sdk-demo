/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GC032A_REGS_H
#define GC032A_REGS_H

#define GC032A_I2C_ADDRESS       0x21U
#define GC032A_REG_CHIP_ID_HIGH  0xf0U
#define GC032A_REG_CHIP_ID_LOW   0xf1U
#define GC032A_REG_PAGE_SELECT   0xfeU
#define GC032A_REG_OUTPUT_FORMAT 0x44U
#define GC032A_SOFTWARE_RESET    0xf0U
#define GC032A_CHIP_ID           0x232aU
#define GC032A_FRAME_SIZE        (640U * 480U * 2U)

#endif
