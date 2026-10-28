/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */



#include "sc_qr.h"

#if LV_USE_QRCODE

#include <string.h>


#include "qrcode/qrcodegen.h"

#define LOG_TAG     "SC_QR"
#include "log.h"


typedef struct
{
    lv_img_dsc_t dsc;
    uint8_t     *buf;
    int32_t      size;
} sc_qr_t;

static sc_qr_t s_qr;

lv_obj_t *sc_qr_create(lv_obj_t *parent, int32_t size, lv_color_t dark, lv_color_t light)
{
    uint32_t buf_size = (uint32_t)size * (uint32_t)size;
    lv_obj_t *img;

    if (s_qr.buf != NULL && s_qr.size != size)
    {
        lv_mem_free(s_qr.buf);
        s_qr.buf = NULL;
    }

    if (s_qr.buf == NULL)
    {
        s_qr.buf = lv_mem_alloc(buf_size);
        if (s_qr.buf == NULL)
        {
            LOG_E("qr buffer (%u bytes) alloc failed", (unsigned)buf_size);
            return NULL;
        }
        s_qr.size = size;
    }
    lv_memset_00(s_qr.buf, buf_size);

    s_qr.dsc.header.always_zero = 0;
    s_qr.dsc.header.cf          = LV_IMG_CF_ALPHA_8BIT;
    s_qr.dsc.header.w           = size;
    s_qr.dsc.header.h           = size;
    s_qr.dsc.data_size          = buf_size;
    s_qr.dsc.data               = s_qr.buf;

    img = lv_img_create(parent);
    lv_img_set_src(img, &s_qr.dsc);

    /* Light modules and quiet zone: reuse the object background colour */
    lv_obj_set_style_bg_color(img, light, 0);
    lv_obj_set_style_bg_opa(img, LV_OPA_100, 0);

    /* Dark modules: opaque pixels in the A8 image, tinted by recolor */
    lv_obj_set_style_img_recolor(img, dark, 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_100, 0);

    return img;
}

void sc_qr_set_text(lv_obj_t *qr, const char *text)
{
    int32_t version;
    int32_t qr_size;
    int32_t scale;
    int32_t scaled;
    int32_t margin;
    int32_t remain;
    uint32_t version_extend;
    uint32_t data_len;
    uint8_t *qr0 = NULL;
    uint8_t *data_tmp = NULL;

    if (qr == NULL || s_qr.buf == NULL || text == NULL)
    {
        return;
    }

    data_len = (uint32_t)strlen(text);
    if (data_len > qrcodegen_BUFFER_LEN_MAX)
    {
        return;
    }

    version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, data_len);
    if (version <= 0)
    {
        return;
    }
    qr_size = qrcodegen_version2size(version);
    if (qr_size <= 0)
    {
        return;
    }
    scale = s_qr.size / qr_size;
    if (scale <= 0)
    {
        return;
    }
    /* When the canvas size is not a multiple of the module size, bump the QR
     * version so the code fills as much of the available area as possible
     * (same heuristic as the LVGL implementation). */
    remain = s_qr.size % qr_size;
    version_extend = (uint32_t)(remain / (scale << 2));
    if (version_extend != 0 && version < qrcodegen_VERSION_MAX)
    {
        version = (version + (int32_t)version_extend > qrcodegen_VERSION_MAX)
                  ? qrcodegen_VERSION_MAX : version + (int32_t)version_extend;
    }

    qr0 = lv_mem_alloc(qrcodegen_BUFFER_LEN_FOR_VERSION(version));
    data_tmp = lv_mem_alloc(qrcodegen_BUFFER_LEN_FOR_VERSION(version));
    if (qr0 == NULL || data_tmp == NULL)
    {
        LOG_E("qr encode buffer alloc failed");
        goto out;
    }
    memcpy(data_tmp, text, data_len);

    if (!qrcodegen_encodeBinary(data_tmp, data_len, qr0, qrcodegen_Ecc_MEDIUM,
                                version, version, qrcodegen_Mask_AUTO, true))
    {
        LOG_E("qr encode failed (len %u, version %d)", (unsigned)data_len, (int)version);
        goto out;
    }

    /* Draw the modules: 0x00 = transparent (light), 0xff = opaque (dark) */
    lv_memset_00(s_qr.buf, (uint32_t)s_qr.size * (uint32_t)s_qr.size);

    qr_size = qrcodegen_getSize(qr0);
    scale = s_qr.size / qr_size;
    scaled = qr_size * scale;
    margin = (s_qr.size - scaled) / 2;

    for (int32_t y = margin; y < scaled + margin; y += scale)
    {
        for (int32_t x = margin; x < scaled + margin; x++)
        {
            if (qrcodegen_getModule(qr0, (x - margin) / scale, (y - margin) / scale))
            {
                s_qr.buf[y * s_qr.size + x] = 0xff;
            }
        }

      
        const uint8_t *row_src = s_qr.buf + y * s_qr.size;
        for (int32_t s = 1; s < scale; s++)
        {
            memcpy(s_qr.buf + (y + s) * s_qr.size, row_src, s_qr.size);
        }
    }


    lv_obj_invalidate(qr);

out:
    if (qr0 != NULL)
    {
        lv_mem_free(qr0);
    }
    if (data_tmp != NULL)
    {
        lv_mem_free(data_tmp);
    }
}

#endif /* LV_USE_QRCODE */
