#include "gc032a_preview_geometry.h"

#include <stddef.h>

#define GC032A_EPIC_SCALE_NONE 1024U

int gc032a_preview_geometry_calculate(uint16_t source_width,
                                      uint16_t source_height,
                                      uint16_t display_width,
                                      uint16_t display_height,
                                      gc032a_preview_geometry_t *geometry)
{
    if (geometry == NULL || source_width == 0U || source_height == 0U ||
        display_width == 0U || display_height == 0U)
        return -1;

    geometry->scaled_width = source_width;
    geometry->scaled_height = source_height;
    geometry->crop_x = (source_width <= display_width)
                           ? (uint16_t)((display_width - source_width) / 2U)
                           : 0U;
    geometry->crop_y = (source_height <= display_height)
                           ? (uint16_t)((display_height - source_height) / 2U)
                           : 0U;
    geometry->epic_scale = GC032A_EPIC_SCALE_NONE;
    geometry->angle_tenths = 0;
    return 0;
}