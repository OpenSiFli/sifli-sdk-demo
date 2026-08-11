#ifndef ARDUCAM_BACKEND_H
#define ARDUCAM_BACKEND_H

#include "camera_source.h"

#define ARDUCAM_PREVIEW_WIDTH 640U
#define ARDUCAM_PREVIEW_HEIGHT 480U
#define ARDUCAM_PREVIEW_SLOT_SIZE (384U * 1024U)

const camera_source_ops_t *arducam_backend_ops(void);

#endif
