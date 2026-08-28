#include "device_ui_state.h"

device_ui_transition_t device_ui_reduce(device_ui_state_t state,
                                        device_ui_event_t event)
{
    device_ui_transition_t transition = {
        .state = state,
        .action = DEVICE_UI_ACTION_NONE,
    };

    switch (event)
    {
    case DEVICE_UI_EVENT_PAN_READY:
        transition.state = DEVICE_UI_CONNECTING;
        transition.action = DEVICE_UI_ACTION_CONNECT_XIAOZHI;
        break;
    case DEVICE_UI_EVENT_PAN_DOWN:
    case DEVICE_UI_EVENT_XIAOZHI_DISCONNECTED:
        transition.state = DEVICE_UI_OFFLINE;
        break;
    case DEVICE_UI_EVENT_XIAOZHI_READY:
        transition.state = DEVICE_UI_HOME;
        break;
    case DEVICE_UI_EVENT_XIAOZHI_ERROR:
        transition.state = DEVICE_UI_ERROR;
        break;
    case DEVICE_UI_EVENT_XIAOZHI_TTS_START:
        transition.state = DEVICE_UI_SPEAKING;
        break;
    case DEVICE_UI_EVENT_XIAOZHI_TTS_STOP:
        transition.state = DEVICE_UI_HOME;
        break;
    case DEVICE_UI_EVENT_PHOTO_SAVED:
        transition.state = DEVICE_UI_HOME;
        break;
    case DEVICE_UI_EVENT_PHOTO_ERROR:
        transition.state = DEVICE_UI_STORAGE_ERROR;
        break;
    case DEVICE_UI_EVENT_KEY1_PRESSED:
        if (state == DEVICE_UI_HOME)
        {
            transition.state = DEVICE_UI_LISTENING;
            transition.action = DEVICE_UI_ACTION_LISTEN_START;
        }
        break;
    case DEVICE_UI_EVENT_KEY1_RELEASED:
        if (state == DEVICE_UI_LISTENING)
        {
            transition.state = DEVICE_UI_THINKING;
            transition.action = DEVICE_UI_ACTION_LISTEN_STOP;
        }
        break;
    case DEVICE_UI_EVENT_KEY2_PRESSED:
        if (state == DEVICE_UI_HOME)
        {
            transition.state = DEVICE_UI_CAMERA_CAPTURING;
            transition.action = DEVICE_UI_ACTION_CAPTURE_PHOTO;
        }
        break;
    case DEVICE_UI_EVENT_KEY3_PRESSED:
        transition.state = DEVICE_UI_HOME;
        break;
    case DEVICE_UI_EVENT_KEY3_LONG_PRESSED:
        transition.state = DEVICE_UI_CONNECTION;
        break;
    case DEVICE_UI_EVENT_NONE:
    default:
        break;
    }
    return transition;
}
