#include "app_state.h"

app_state_t app_state_next(app_state_t state, app_event_t event)
{
    switch (event)
    {
    case APP_EVENT_START:
        return state == APP_BOOT ? APP_READY_OFFLINE : state;
    case APP_EVENT_PAN_READY:
        return state == APP_READY_OFFLINE || state == APP_RECOVERING ?
               APP_READY : state;
    case APP_EVENT_PAN_DOWN:
        return state == APP_READY ? APP_RECOVERING : state;
    case APP_EVENT_PAN_RETRY_EXHAUSTED:
        return state == APP_RECOVERING ? APP_READY_OFFLINE : state;
    case APP_EVENT_NONE:
    default:
        return state;
    }
}

unsigned int app_retry_next(unsigned int retry_count, unsigned int retry_max)
{
    if (retry_max == 0U || retry_count >= retry_max)
        return 0U;
    return retry_count + 1U;
}
