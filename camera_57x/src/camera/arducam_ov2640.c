#include "arducam_ov2640.h"

#include <stddef.h>

#include "arducam_ov2640_settings.h"

static int arducam_ov2640_write_table(arducam_sensor_write_t write,
                                      void *context,
                                      const arducam_sensor_reg_t *table)
{
    size_t index;

    for (index = 0U; !((table[index].reg == 0xFFU) &&
                       (table[index].value == 0xFFU)); index++)
    {
        if (write(context, table[index].reg, table[index].value) != 0)
            return -1;
    }
    return 0;
}

int arducam_ov2640_set_mode(arducam_sensor_write_t write,
                            arducam_sensor_delay_t delay, void *context,
                            arducam_capture_mode_t mode)
{
    const arducam_sensor_reg_t *table;

    if ((write == NULL) || (delay == NULL))
        return -1;
    table = mode == ARDUCAM_CAPTURE_STILL ?
            OV2640_1600x1200_JPEG : OV2640_640x480_JPEG;
    if (arducam_ov2640_write_table(write, context, table) != 0)
        return -1;
    delay(context, 100U);
    return 0;
}

int arducam_ov2640_initialize(arducam_sensor_write_t write,
                              arducam_sensor_delay_t delay, void *context)
{
    if ((write == NULL) || (delay == NULL) ||
        (write(context, 0xFFU, 0x01U) != 0) ||
        (write(context, 0x12U, 0x80U) != 0))
        return -1;
    delay(context, 100U);
    if ((arducam_ov2640_write_table(write, context, OV2640_JPEG_INIT) != 0) ||
        (arducam_ov2640_write_table(write, context, OV2640_YUV422) != 0) ||
        (arducam_ov2640_write_table(write, context, OV2640_JPEG) != 0))
        return -1;
    return arducam_ov2640_set_mode(write, delay, context,
                                    ARDUCAM_CAPTURE_PREVIEW);
}
