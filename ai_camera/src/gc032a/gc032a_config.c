#include "gc032a_config.h"

static const gc032a_mode_t s_fixed_mode = {
    .width = GC032A_FRAME_WIDTH,
    .height = GC032A_FRAME_HEIGHT,
    .bytes_per_pixel = GC032A_BYTES_PER_PIXEL,
    .interface_type = GC032A_INTERFACE_SERIAL_2BIT,
    .pixel_format = GC032A_PIXEL_FORMAT_RGB565_BE,
};

const gc032a_mode_t *gc032a_fixed_mode(void)
{
    return &s_fixed_mode;
}

int gc032a_mode_is_supported(const gc032a_mode_t *mode)
{
    return mode != 0 && mode->width == GC032A_FRAME_WIDTH &&
           mode->height == GC032A_FRAME_HEIGHT &&
           mode->bytes_per_pixel == GC032A_BYTES_PER_PIXEL &&
           mode->interface_type == GC032A_INTERFACE_SERIAL_2BIT &&
           mode->pixel_format == GC032A_PIXEL_FORMAT_RGB565_BE;
}