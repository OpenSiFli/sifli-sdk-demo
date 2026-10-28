/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#ifndef SC_QR_H
#define SC_QR_H

#include "lvgl.h"

/**
 * Create the QR code view.
 *
 * The view is an `lv_img` backed by an `LV_IMG_CF_ALPHA_8BIT` buffer. 
 *
 * @param parent    parent object the view is created on
 * @param size      QR code square edge length, in pixels
 * @param dark      colour of the modules
 * @param light     colour of the quiet zone
 * @return created object, or NULL when the pixel buffer cannot be allocated
 */
lv_obj_t *sc_qr_create(lv_obj_t *parent, int32_t size, lv_color_t dark, lv_color_t light);

/**
 * Encode @p text and draw it into the QR code view.
 *
 * The A8 image buffer is written in place and the object is invalidated; the
 * new pattern is drawn on the next refresh.
 *
 * @param qr    object returned by @ref sc_qr_create
 * @param text  NUL terminated string to encode
 */
void sc_qr_set_text(lv_obj_t *qr, const char *text);

#endif /* SC_QR_H */
