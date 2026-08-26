#ifndef LEARNING_CAMERA_APP_STATE_H
#define LEARNING_CAMERA_APP_STATE_H

typedef enum
{
    APP_BOOT = 0,
    APP_READY_OFFLINE,
    APP_READY,
    APP_RECOVERING,
} app_state_t;

typedef enum
{
    APP_EVENT_NONE = 0,
    APP_EVENT_START,
    APP_EVENT_PAN_READY,
    APP_EVENT_PAN_DOWN,
    APP_EVENT_PAN_RETRY_EXHAUSTED,
} app_event_t;

app_state_t app_state_next(app_state_t state, app_event_t event);
unsigned int app_retry_next(unsigned int retry_count, unsigned int retry_max);

#endif
