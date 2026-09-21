#ifndef AI_CAMERA_XIAOZHI_WEBSOCKET_H
#define AI_CAMERA_XIAOZHI_WEBSOCKET_H

#include <stddef.h>

typedef enum
{
    AI_XIAOZHI_EVENT_READY = 0,
    AI_XIAOZHI_EVENT_DISCONNECTED,
    AI_XIAOZHI_EVENT_ERROR,
    AI_XIAOZHI_EVENT_TTS_START,
    AI_XIAOZHI_EVENT_TTS_STOP,
} ai_xiaozhi_event_t;

typedef void (*ai_xiaozhi_event_callback_t)(ai_xiaozhi_event_t event,
                                            void *context);

void ai_xiaozhi_set_event_callback(ai_xiaozhi_event_callback_t callback,
                                   void *context);
int ai_xiaozhi_websocket_is_active(void);
int ai_xiaozhi_websocket_is_connecting(void);
int ai_xiaozhi_websocket_is_listening(void);
int ai_xiaozhi_connect_async(void);
int ai_xiaozhi_listen_start_request(void);
int ai_xiaozhi_listen_stop_request(void);
/* Sends one JSON text frame on the active session. Thread safe. */
int ai_xiaozhi_websocket_send_text(const char *text, size_t size);

/* Session switch, owned by the assistant UI. Enabled (the default) lets the app
 * open a session and reopen it when the server closes an idle one; disabled
 * leaves the session closed until it is enabled again. Closing a session never
 * touches the Bluetooth PAN link. */
void ai_xiaozhi_session_set_enabled(int enabled);
int ai_xiaozhi_session_enabled(void);
void ai_xiaozhi_session_close(void);

#endif
