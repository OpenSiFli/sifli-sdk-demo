/* SPDX-License-Identifier: Apache-2.0
 *
 * Read-only runtime status of the learning camera.
 *
 * This module owns no policy: every field is either pushed in by the thread
 * that already knows the answer (PAN/IP from main.c, TTS from the XiaoZhi
 * event callback) or pulled from the module that owns it (WebSocket and
 * listening from xiaozhi_websocket.c, capture from camera_service.c).  It is
 * meant to be queried by the UI, never to drive behaviour.
 */
#ifndef LEARNING_CAMERA_STATUS_H
#define LEARNING_CAMERA_STATUS_H

typedef enum
{
    /* No WebSocket socket at all. */
    LEARNING_WS_OFFLINE = 0,
    /* A connect attempt is still running. */
    LEARNING_WS_CONNECTING,
    /* The WebSocket socket is established. */
    LEARNING_WS_READY,
} learning_ws_state_t;

typedef struct
{
    int pan_connected;
    int has_ip;
    learning_ws_state_t ws_state;
    int listening;
    int tts_playing;
    int camera_capturing;
} learning_camera_status_t;

/* Push-style updates. Only the owners of the information call these. */
void learning_camera_status_set_pan_connected(int connected);
void learning_camera_status_set_has_ip(int ready);
void learning_camera_status_set_tts_playing(int playing);

/* Snapshot for the UI. Reads the WebSocket, microphone and camera modules, so
 * it must not be called from an interrupt context. */
void learning_camera_status_get(learning_camera_status_t *status);

/* Human readable snapshot, exported as the MSH command
 * "learning_camera_status". */
void learning_camera_status_print(void);

#endif
