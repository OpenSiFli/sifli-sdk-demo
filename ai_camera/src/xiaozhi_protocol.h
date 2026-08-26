#ifndef AI_XIAOZHI_PROTOCOL_H
#define AI_XIAOZHI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define AI_XIAOZHI_ENDPOINT_HOST_MAX 128U
#define AI_XIAOZHI_ENDPOINT_PATH_MAX 256U
#define AI_XIAOZHI_TOKEN_MAX 256U
#define AI_XIAOZHI_ACTIVATION_CODE_MAX 64U
#define AI_XIAOZHI_DEVICE_ID_MAX 18U
#define AI_XIAOZHI_CLIENT_ID_MAX 37U
#define AI_XIAOZHI_ACTIVATION_REQUEST_MAX 256U

typedef struct
{
    char host[AI_XIAOZHI_ENDPOINT_HOST_MAX];
    char path[AI_XIAOZHI_ENDPOINT_PATH_MAX];
    uint16_t port;
    int ssl_enabled;
} ai_xiaozhi_endpoint_t;

typedef struct
{
    ai_xiaozhi_endpoint_t endpoint;
    char token[AI_XIAOZHI_TOKEN_MAX];
    char activation_code[AI_XIAOZHI_ACTIVATION_CODE_MAX];
    int activation_pending;
} ai_xiaozhi_activation_t;

int ai_xiaozhi_build_hello(char *out, size_t out_size);
int ai_xiaozhi_build_listen_start(char *out, size_t out_size,
                                  const char *session_id);
int ai_xiaozhi_build_listen_stop(char *out, size_t out_size,
                                 const char *session_id);
int ai_xiaozhi_format_device_id(char *out, size_t out_size,
                                const uint8_t address[6]);
int ai_xiaozhi_format_client_id(char *out, size_t out_size,
                                const uint8_t digest[16]);
int ai_xiaozhi_build_activation_request(char *out, size_t out_size,
                                        const char *device_id,
                                        const char *client_id);
int ai_xiaozhi_parse_websocket_url(const char *url,
                                   ai_xiaozhi_endpoint_t *endpoint);
int ai_xiaozhi_parse_activation_response(const char *response,
                                         ai_xiaozhi_activation_t *activation);

#endif
