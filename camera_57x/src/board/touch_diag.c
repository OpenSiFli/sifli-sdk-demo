#include "touch_diag.h"

#include <stddef.h>

int touch_diag_wait(touch_diag_read_fn_t read_event,
                    touch_diag_delay_fn_t delay, void *context,
                    uint32_t timeout_ms, uint32_t poll_ms,
                    touch_diag_event_t *event)
{
    uint32_t elapsed_ms;

    if ((read_event == NULL) || (delay == NULL) || (event == NULL) ||
        (poll_ms == 0U))
        return TOUCH_DIAG_INVALID;

    for (elapsed_ms = 0; elapsed_ms < timeout_ms; elapsed_ms += poll_ms)
    {
        int result = read_event(context, event);

        if (result > 0)
            return TOUCH_DIAG_OK;
        if (result < 0)
            return TOUCH_DIAG_READ_ERROR;
        delay(context, poll_ms);
    }
    return TOUCH_DIAG_TIMEOUT;
}
