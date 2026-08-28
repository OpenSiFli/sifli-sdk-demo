#include <assert.h>

#include "device_ui_state.h"

int main(void)
{
    device_ui_transition_t transition;

    transition = device_ui_reduce(DEVICE_UI_OFFLINE,
                                  DEVICE_UI_EVENT_PAN_READY);
    assert(transition.state == DEVICE_UI_CONNECTING);
    assert(transition.action == DEVICE_UI_ACTION_CONNECT_XIAOZHI);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_XIAOZHI_READY);
    assert(transition.state == DEVICE_UI_HOME);
    assert(transition.action == DEVICE_UI_ACTION_NONE);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_KEY1_PRESSED);
    assert(transition.state == DEVICE_UI_LISTENING);
    assert(transition.action == DEVICE_UI_ACTION_LISTEN_START);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_KEY1_RELEASED);
    assert(transition.state == DEVICE_UI_THINKING);
    assert(transition.action == DEVICE_UI_ACTION_LISTEN_STOP);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_XIAOZHI_TTS_START);
    assert(transition.state == DEVICE_UI_SPEAKING);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_XIAOZHI_TTS_STOP);
    assert(transition.state == DEVICE_UI_HOME);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_KEY2_PRESSED);
    assert(transition.state == DEVICE_UI_CAMERA_CAPTURING);
    assert(transition.action == DEVICE_UI_ACTION_CAPTURE_PHOTO);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_KEY3_PRESSED);
    assert(transition.state == DEVICE_UI_HOME);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_KEY3_LONG_PRESSED);
    assert(transition.state == DEVICE_UI_CONNECTION);

    transition = device_ui_reduce(transition.state,
                                  DEVICE_UI_EVENT_PAN_DOWN);
    assert(transition.state == DEVICE_UI_OFFLINE);

    transition = device_ui_reduce(DEVICE_UI_OFFLINE,
                                  DEVICE_UI_EVENT_KEY1_PRESSED);
    assert(transition.state == DEVICE_UI_OFFLINE);
    assert(transition.action == DEVICE_UI_ACTION_NONE);
    return 0;
}
