#ifndef AI_CAMERA_BT_PAN_POLICY_H
#define AI_CAMERA_BT_PAN_POLICY_H

#include <stdint.h>

typedef enum
{
    APP_MESSAGE_NONE = 0,
    APP_MESSAGE_STACK_READY,
    APP_MESSAGE_BOND_READY,
    APP_MESSAGE_CONNECT_PAN,
    APP_MESSAGE_PAN_UP,
    APP_MESSAGE_PAN_DOWN,
    APP_MESSAGE_IP_POLL,
    APP_MESSAGE_RETRY_PAN,
    APP_MESSAGE_XIAOZHI_UP,
    APP_MESSAGE_XIAOZHI_DOWN,
    APP_MESSAGE_XIAOZHI_RETRY,
} app_message_type_t;

app_message_type_t app_bt_message_for_event(uint16_t type, uint16_t event_id);

#endif
