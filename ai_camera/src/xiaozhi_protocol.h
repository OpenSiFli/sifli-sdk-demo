#ifndef AI_XIAOZHI_PROTOCOL_H
#define AI_XIAOZHI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define AI_XIAOZHI_ENDPOINT_HOST_MAX 128U
#define AI_XIAOZHI_ENDPOINT_PATH_MAX 256U
#define AI_XIAOZHI_TOKEN_MAX 256U
#define AI_XIAOZHI_MULTIPART_BOUNDARY_MAX 64U
#define AI_XIAOZHI_ACTIVATION_CODE_MAX 64U
#define AI_XIAOZHI_SESSION_ID_MAX 64U
#define AI_XIAOZHI_AUDIO_FORMAT_MAX 16U
#define AI_XIAOZHI_DEVICE_ID_MAX 18U
#define AI_XIAOZHI_CLIENT_ID_MAX 37U
#define AI_XIAOZHI_CONTENT_LENGTH_HEADER_MAX 48U
#define AI_XIAOZHI_ACTIVATION_REQUEST_MAX 1024U
#define AI_XIAOZHI_MCP_TOOL_NAME_MAX 64U
#define AI_XIAOZHI_MCP_QUESTION_MAX 192U
#define AI_XIAOZHI_MCP_TOOLS_RESPONSE_MAX 768U
#define AI_XIAOZHI_MCP_TOOL_RESULT_MAX 1024U
/* The explain answer is JSON ({...,"text":"..."}); a full Chinese sentence
 * does not fit AI_XIAOZHI_VISION_RESPONSE_MAX. */
#define AI_XIAOZHI_VISION_TEXT_MAX 512U
#define AI_XIAOZHI_VISION_EXPLAIN_MAX 768U

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

typedef struct
{
    char session_id[AI_XIAOZHI_SESSION_ID_MAX];
    char audio_format[AI_XIAOZHI_AUDIO_FORMAT_MAX];
    uint32_t sample_rate;
    uint8_t channels;
    uint16_t frame_duration;
    uint8_t protocol_version;
} ai_xiaozhi_server_hello_t;

typedef struct
{
    ai_xiaozhi_endpoint_t endpoint;
    char token[AI_XIAOZHI_TOKEN_MAX];
} ai_xiaozhi_vision_capability_t;

typedef enum
{
    AI_XIAOZHI_SERVER_EVENT_NONE = 0,
    AI_XIAOZHI_SERVER_EVENT_TTS_START,
    AI_XIAOZHI_SERVER_EVENT_TTS_STOP,
} ai_xiaozhi_server_event_t;

int ai_xiaozhi_build_hello(char *out, size_t out_size);
int ai_xiaozhi_build_listen_start(char *out, size_t out_size,
                                  const char *session_id);
int ai_xiaozhi_build_listen_stop(char *out, size_t out_size,
                                 const char *session_id);
int ai_xiaozhi_format_device_id(char *out, size_t out_size,
                                const uint8_t address[6]);
int ai_xiaozhi_format_pan_device_id(char *out, size_t out_size,
                                    const uint8_t pan_address[6]);
int ai_xiaozhi_format_client_id(char *out, size_t out_size,
                                const uint8_t digest[16]);
int ai_xiaozhi_build_activation_request(char *out, size_t out_size,
                                        const char *device_id,
                                        const char *client_id);
int ai_xiaozhi_format_content_length_header(char *out, size_t out_size,
                                            size_t content_length);
int ai_xiaozhi_activation_http_status_ok(int status);
int ai_xiaozhi_parse_websocket_url(const char *url,
                                   ai_xiaozhi_endpoint_t *endpoint);
int ai_xiaozhi_parse_http_url(const char *url,
                              ai_xiaozhi_endpoint_t *endpoint);
int ai_xiaozhi_parse_activation_response(const char *response,
                                         ai_xiaozhi_activation_t *activation);
int ai_xiaozhi_parse_server_hello(const char *response,
                                  ai_xiaozhi_server_hello_t *hello);
ai_xiaozhi_server_event_t ai_xiaozhi_parse_server_event(
    const char *response);
int ai_xiaozhi_parse_mcp_vision_initialize(
    const char *response, ai_xiaozhi_vision_capability_t *vision);
int ai_xiaozhi_parse_mcp_vision_initialize_request(
    const char *response, ai_xiaozhi_vision_capability_t *vision,
    char *session_id, size_t session_id_size, int *request_id);
int ai_xiaozhi_build_mcp_initialize_response(
    char *out, size_t out_size, const char *session_id, int request_id);
/* MCP tool channel: the server (MCP client) enumerates the device tools with
 * tools/list and invokes them with tools/call. */
int ai_xiaozhi_parse_mcp_tools_list_request(
    const char *response, char *session_id, size_t session_id_size,
    int *request_id);
int ai_xiaozhi_parse_mcp_tools_call_request(
    const char *response, char *session_id, size_t session_id_size,
    int *request_id, char *tool_name, size_t tool_name_size, char *question,
    size_t question_size);
int ai_xiaozhi_build_mcp_tools_list_response(
    char *out, size_t out_size, const char *session_id, int request_id);
/* text is JSON escaped; is_error marks the result as a failed call. */
int ai_xiaozhi_build_mcp_tool_result(char *out, size_t out_size,
                                     const char *session_id, int request_id,
                                     const char *text, int is_error);
/* Pulls the human readable answer out of the vision explain response. */
int ai_xiaozhi_parse_vision_explain_response(const char *response, char *text,
                                             size_t text_size);
int ai_xiaozhi_multipart_length(const char *question, size_t jpeg_size,
                                const char *boundary);
int ai_xiaozhi_build_multipart_prefix(char *out, size_t out_size,
                                      const char *boundary,
                                      const char *question);
int ai_xiaozhi_build_multipart_suffix(char *out, size_t out_size,
                                      const char *boundary);

#endif
