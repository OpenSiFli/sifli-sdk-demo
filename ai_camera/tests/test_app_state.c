#include <assert.h>

#include "app_state.h"

int main(void)
{
    app_state_t state = APP_BOOT;

    state = app_state_next(state, APP_EVENT_START);
    assert(state == APP_READY_OFFLINE);

    state = app_state_next(state, APP_EVENT_PAN_READY);
    assert(state == APP_READY);

    state = app_state_next(state, APP_EVENT_PAN_DOWN);
    assert(state == APP_RECOVERING);

    state = app_state_next(state, APP_EVENT_PAN_RETRY_EXHAUSTED);
    assert(state == APP_READY_OFFLINE);

    state = app_state_next(APP_READY, APP_EVENT_NONE);
    assert(state == APP_READY);

    assert(app_retry_next(0U, 5U) == 1U);
    assert(app_retry_next(4U, 5U) == 5U);
    assert(app_retry_next(5U, 5U) == 0U);
    assert(app_retry_next(0U, 0U) == 0U);
    return 0;
}
