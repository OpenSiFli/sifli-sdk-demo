#ifndef ARDUCAM_BACKEND_H
#define ARDUCAM_BACKEND_H

#include "camera_source.h"

#define ARDUCAM_PREVIEW_WIDTH 640U
#define ARDUCAM_PREVIEW_HEIGHT 480U
#define ARDUCAM_PREVIEW_SLOT_SIZE (384U * 1024U)

/*
 * The supported OV2640 path is JPEG over the ArduChip SPI FIFO. Direct
 * RGB565 FIFO capture and a DVP/parallel-camera backend are intentionally
 * unavailable until they have board-level validation.
 */
#define ARDUCAM_OV2640_DIRECT_RGB565_AVAILABLE 0

const camera_source_ops_t *arducam_backend_ops(void);

#endif
