#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "bts2_app_interface_type.h"
#include "bt_pan_policy.h"

int main(void)
{
    assert(app_bt_message_for_event(BT_NOTIFY_HID,
                                    BT_NOTIFY_HID_PROFILE_CONNECTED) ==
           APP_MESSAGE_CONNECT_PAN);
    assert(app_bt_message_for_event(BT_NOTIFY_PAN,
                                    BT_NOTIFY_PAN_PROFILE_CONNECTED) ==
           APP_MESSAGE_PAN_UP);
    assert(app_bt_message_for_event(BT_NOTIFY_PAN,
                                    BT_NOTIFY_PAN_PROFILE_DISCONNECTED) ==
           APP_MESSAGE_PAN_DOWN);
    assert(app_bt_message_for_event(BT_NOTIFY_COMMON,
                                    BT_NOTIFY_COMMON_LOCAL_NAME_RSP) ==
           APP_MESSAGE_NONE);
    return 0;
}
