#ifndef RGBLED_H
#define RGBLED_H

#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"

void rgb_led_init();
void rgb_led_set_color(uint32_t color);

#endif // RGBLED_H