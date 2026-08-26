#include "bt_pan_policy.h"

#include <stdbool.h>

#include "bts2_app_interface_type.h"

app_message_type_t app_bt_message_for_event(uint16_t type, uint16_t event_id)
{
    if (type == BT_NOTIFY_HID &&
        event_id == BT_NOTIFY_HID_PROFILE_CONNECTED)
        return APP_MESSAGE_CONNECT_PAN;
    if (type == BT_NOTIFY_PAN)
    {
        if (event_id == BT_NOTIFY_PAN_PROFILE_CONNECTED)
            return APP_MESSAGE_PAN_UP;
        if (event_id == BT_NOTIFY_PAN_PROFILE_DISCONNECTED)
            return APP_MESSAGE_PAN_DOWN;
    }
    return APP_MESSAGE_NONE;
}
