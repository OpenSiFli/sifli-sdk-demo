#ifndef AI_CAMERA_GC032A_CONFIG_H
#define AI_CAMERA_GC032A_CONFIG_H

#include <stdint.h>

#define GC032A_FRAME_WIDTH 640U
#define GC032A_FRAME_HEIGHT 480U
#define GC032A_BYTES_PER_PIXEL 2U
#define GC032A_FRAME_SIZE \
    (GC032A_FRAME_WIDTH * GC032A_FRAME_HEIGHT * GC032A_BYTES_PER_PIXEL)

typedef enum
{
    /* GC032A is read over the 2-line serial interface (DCMI_IF_SPI): it is the
     * only path that has been verified to deliver RGB565 on this board. */
    GC032A_INTERFACE_SERIAL_2BIT = 0,
} gc032a_interface_t;

typedef enum
{
    GC032A_PIXEL_FORMAT_RGB565_BE = 0,
} gc032a_pixel_format_t;

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint8_t bytes_per_pixel;
    gc032a_interface_t interface_type;
    gc032a_pixel_format_t pixel_format;
} gc032a_mode_t;

const gc032a_mode_t *gc032a_fixed_mode(void);
int gc032a_mode_is_supported(const gc032a_mode_t *mode);

#endif