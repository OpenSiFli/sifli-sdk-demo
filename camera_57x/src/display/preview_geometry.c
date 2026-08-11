#include "preview_geometry.h"

#include <stddef.h>

#define PREVIEW_EPIC_SCALE_NONE 1024U

int preview_geometry_calculate(uint16_t source_width, uint16_t source_height,
                               uint16_t display_width, uint16_t display_height,
                               preview_geometry_t *geometry)
{
    uint32_t scaled_width;
    uint32_t scaled_height;
    uint16_t rotated_width = source_height;
    uint16_t rotated_height = source_width;

    if ((geometry == NULL) || (source_width == 0U) || (source_height == 0U) ||
        (display_width == 0U) || (display_height == 0U))
        return -1;

    if ((uint64_t)display_width * rotated_height >=
        (uint64_t)display_height * rotated_width)
    {
        scaled_width = display_width;
        scaled_height = ((uint64_t)rotated_height * display_width +
                         rotated_width / 2U) / rotated_width;
    }
    else
    {
        scaled_height = display_height;
        scaled_width = ((uint64_t)rotated_width * display_height +
                        rotated_height / 2U) / rotated_height;
    }
    if ((scaled_width > UINT16_MAX) || (scaled_height > UINT16_MAX))
        return -1;

    geometry->rotated_width = rotated_width;
    geometry->rotated_height = rotated_height;
    geometry->scaled_width = (uint16_t)scaled_width;
    geometry->scaled_height = (uint16_t)scaled_height;
    geometry->crop_x = (uint16_t)((scaled_width - display_width) / 2U);
    geometry->crop_y = (uint16_t)((scaled_height - display_height) / 2U);
    geometry->epic_scale = (uint16_t)(((uint32_t)rotated_width *
                                       PREVIEW_EPIC_SCALE_NONE) / scaled_width);
    geometry->angle_tenths = 900;
    return 0;
}
