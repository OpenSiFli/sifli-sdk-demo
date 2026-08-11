#ifndef ARDUCAM_OV2640_H
#define ARDUCAM_OV2640_H

#include <stdint.h>

#include "arducam_controller.h"

typedef int (*arducam_sensor_write_t)(void *context, uint8_t reg,
                                      uint8_t value);
typedef void (*arducam_sensor_delay_t)(void *context, uint32_t milliseconds);

int arducam_ov2640_initialize(arducam_sensor_write_t write,
                              arducam_sensor_delay_t delay, void *context);
int arducam_ov2640_set_mode(arducam_sensor_write_t write,
                            arducam_sensor_delay_t delay, void *context,
                            arducam_capture_mode_t mode);

#endif
