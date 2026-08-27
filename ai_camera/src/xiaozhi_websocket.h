#ifndef AI_CAMERA_XIAOZHI_WEBSOCKET_H
#define AI_CAMERA_XIAOZHI_WEBSOCKET_H

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
int ai_xiaozhi_connect_async(void);
int ai_xiaozhi_listen_start_request(void);
int ai_xiaozhi_listen_stop_request(void);

#endif
