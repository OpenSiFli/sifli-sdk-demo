/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file screen_cast_ui.h
 * @brief LVGL rendering of the cast stream (application view layer).
 */

#ifndef SCREEN_CAST_UI_H
#define SCREEN_CAST_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the cast UI objects and start the refresh timer.
 *
 * Must be called after LVGL is initialised and after @ref screen_cast_init.
 */
void screen_cast_ui_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_CAST_UI_H */
