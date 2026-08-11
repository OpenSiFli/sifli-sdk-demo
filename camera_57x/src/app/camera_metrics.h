#ifndef CAMERA_METRICS_H
#define CAMERA_METRICS_H

#include <stdint.h>

uint32_t camera_metrics_average_ms_x10(uint32_t ticks,
                                       uint32_t samples,
                                       uint32_t tick_rate);

#endif
