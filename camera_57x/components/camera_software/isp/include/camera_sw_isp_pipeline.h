/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_SW_ISP_PIPELINE_H
#define CAMERA_SW_ISP_PIPELINE_H

#include <stdint.h>
#include "camera_sw_isp.h"

#define CAMERA_SW_ISP_FLAG_AWB_STATS (1U << 0)
#define CAMERA_SW_ISP_FLAG_AWB_APPLY (1U << 1)
#define CAMERA_SW_ISP_FLAG_CCM (1U << 2)
#define CAMERA_SW_ISP_FLAG_GAMMA (1U << 3)
#define CAMERA_SW_ISP_FLAG_FILTER (1U << 4)
#define CAMERA_SW_ISP_FLAG_TONE (1U << 5)
#define CAMERA_SW_ISP_FLAG_VIVID (1U << 6)
#define CAMERA_SW_ISP_FLAG_DITHER (1U << 7)
#define CAMERA_SW_ISP_FLAG_ALL 0xffU

typedef struct
{
    camera_sw_isp_stats_t stats;
    uint32_t decode_ms;
    uint32_t awb_stats_ms;
    uint32_t awb_apply_ms;
    uint32_t ccm_ms;
    uint32_t gamma_ms;
    uint32_t filter_ms;
    uint32_t tone_ms;
    uint32_t applied_flags;
} camera_sw_isp_result_t;

typedef uint32_t (*camera_sw_isp_now_ms_t)(void);

int camera_sw_isp_process_rgb565_be(uint8_t *pixels, uint16_t width,
                                    uint16_t height, uint32_t stride,
                                    uint32_t flags,
                                    camera_sw_isp_result_t *result,
                                    camera_sw_isp_now_ms_t now_ms);
#endif
