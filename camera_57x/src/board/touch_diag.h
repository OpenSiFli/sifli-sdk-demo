#ifndef TOUCH_DIAG_H
#define TOUCH_DIAG_H

#include <stdint.h>

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint8_t event;
} touch_diag_event_t;

typedef int (*touch_diag_read_fn_t)(void *context, touch_diag_event_t *event);
typedef void (*touch_diag_delay_fn_t)(void *context, uint32_t delay_ms);

typedef enum
{
    TOUCH_DIAG_OK = 0,
    TOUCH_DIAG_TIMEOUT = -1,
    TOUCH_DIAG_READ_ERROR = -2,
    TOUCH_DIAG_INVALID = -3,
} touch_diag_result_t;

int touch_diag_wait(touch_diag_read_fn_t read_event,
                    touch_diag_delay_fn_t delay, void *context,
                    uint32_t timeout_ms, uint32_t poll_ms,
                    touch_diag_event_t *event);

#endif
