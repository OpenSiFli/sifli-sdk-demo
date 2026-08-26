#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "xiaozhi_protocol.h"

extern int ai_xiaozhi_build_mcp_initialize_response(
    char *out, size_t out_size, const char *session_id, int request_id);
extern int ai_xiaozhi_parse_mcp_vision_initialize_request(
    const char *response, ai_xiaozhi_vision_capability_t *vision,
    char *session_id, size_t session_id_size, int *request_id);

static void assert_json_string(cJSON *object, const char *key,
                               const char *expected)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    assert(cJSON_IsString(item));
    assert(strcmp(item->valuestring, expected) == 0);
}

int main(void)
{
    static const char hello[] =
        "{\"type\":\"hello\",\"version\":3,\"transport\":\"websocket\","
        "\"features\":{\"mcp\":true},"
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60}}";
    static const char mcp_initialize[] =
        "{\"session_id\":\"session-42\",\"type\":\"mcp\","
        "\"payload\":{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{\"vision\":{"
        "\"url\":\"https://vision.example.com/upload\","
        "\"token\":\"vision-token\"}}},\"id\":1}}";
    static const char mcp_initialize_without_token[] =
        "{\"session_id\":\"session-42\",\"type\":\"mcp\","
        "\"payload\":{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{\"vision\":{"
        "\"url\":\"https://vision.example.com/upload\"}}},\"id\":1}}";
    static const char listen_start[] =
        "{\"session_id\":\"session-42\",\"type\":\"listen\","
        "\"state\":\"start\",\"mode\":\"manual\"}";
    static const char listen_stop[] =
        "{\"session_id\":\"session-42\",\"type\":\"listen\","
        "\"state\":\"stop\"}";
    static const char activation_response[] =
        "{\"websocket\":{\"url\":\"wss://voice.example.com/xiaozhi/v1/\","
        "\"token\":\"dynamic-token\"},\"activation\":{\"code\":\"ABCD-1234\"}}";
    static const char activated_response[] =
        "{\"websocket\":{\"url\":\"ws://192.0.2.1:9000/ws\","
        "\"token\":\"second-token\"}}";
    static const char server_hello[] =
        "{\"type\":\"hello\",\"version\":1,"
        "\"transport\":\"websocket\",\"session_id\":\"session-42\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":24000,"
        "\"channels\":1,\"frame_duration\":60}}";
    static const char server_hello_v2[] =
        "{\"type\":\"hello\",\"version\":2,"
        "\"transport\":\"websocket\",\"audio_params\":{"
        "\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":2,\"frame_duration\":20}}";
    static const uint8_t address[] = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
    static const uint8_t pan_address[] = {0xcdU, 0xabU, 0x78U,
                                          0x56U, 0x34U, 0x12U};
    static const uint8_t digest[] = {0x00U, 0x01U, 0x02U, 0x03U,
                                     0x04U, 0x05U, 0x06U, 0x07U,
                                     0x08U, 0x09U, 0x0aU, 0x0bU,
                                     0x0cU, 0x0dU, 0x0eU, 0x0fU};
    ai_xiaozhi_endpoint_t endpoint;
    ai_xiaozhi_activation_t activation;
    ai_xiaozhi_server_hello_t server;
    ai_xiaozhi_vision_capability_t vision;
    int request_id;
    char message[sizeof(hello)];
    char mcp_response[384];
    char request[AI_XIAOZHI_ACTIVATION_REQUEST_MAX];
    char multipart[512];
    cJSON *request_json;

    assert(ai_xiaozhi_build_hello(message, sizeof(message)) ==
           (int)strlen(hello));
    assert(strcmp(message, hello) == 0);
    assert(ai_xiaozhi_build_hello(message, sizeof(hello) - 1U) < 0);

    assert(ai_xiaozhi_build_listen_start(message, sizeof(message),
                                         "session-42") ==
           (int)strlen(listen_start));
    assert(strcmp(message, listen_start) == 0);
    assert(ai_xiaozhi_build_listen_stop(message, sizeof(message),
                                        "session-42") ==
           (int)strlen(listen_stop));
    assert(strcmp(message, listen_stop) == 0);
    assert(ai_xiaozhi_build_listen_start(message, sizeof(message),
                                         "bad\"session") < 0);
    assert(ai_xiaozhi_build_listen_stop(message, sizeof(message), 0) < 0);
    assert(ai_xiaozhi_format_device_id(message, sizeof(message), address) == 17);
    assert(strcmp(message, "00:11:22:33:44:55") == 0);
    assert(ai_xiaozhi_format_device_id(message, 17U, address) < 0);
    assert(ai_xiaozhi_format_pan_device_id(message, sizeof(message),
                                            pan_address) == 17);
    assert(strcmp(message, "ab:cd:56:78:12:34") == 0);
    assert(ai_xiaozhi_format_pan_device_id(message, 17U, pan_address) < 0);
    assert(ai_xiaozhi_format_client_id(message, sizeof(message), digest) == 36);
    assert(strcmp(message, "00010203-0405-0607-0809-0a0b0c0d0e0f") == 0);
    assert(ai_xiaozhi_format_client_id(message, 36U, digest) < 0);
    assert(ai_xiaozhi_build_activation_request(
               request, sizeof(request), "00:11:22:33:44:55",
               "00010203-0405-0607-0809-0a0b0c0d0e0f") > 500);
    request_json = cJSON_Parse(request);
    assert(cJSON_IsObject(request_json));
    assert(cJSON_GetObjectItemCaseSensitive(request_json, "version")->valueint == 2);
    assert(cJSON_GetObjectItemCaseSensitive(request_json, "flash_size")->valueint ==
           4194304);
    assert(cJSON_GetObjectItemCaseSensitive(request_json, "psram_size")->valueint ==
           0);
    assert_json_string(request_json, "mac_address", "00:11:22:33:44:55");
    assert_json_string(request_json, "uuid", "00010203-0405-0607-0809-0a0b0c0d0e0f");
    assert_json_string(request_json, "chip_model_name", "sf32lb573");
    assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(request_json,
                                                            "chip_info")));
    assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(request_json,
                                                            "application")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(request_json,
                                                                "partition_table")) ==
           1);
    assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(request_json, "ota")));
    assert_json_string(cJSON_GetObjectItemCaseSensitive(request_json, "board"),
                       "type", "spi-hdk_lb573ub7n6");
    assert_json_string(cJSON_GetObjectItemCaseSensitive(request_json, "board"),
                       "mac", "00:11:22:33:44:55");
    cJSON_Delete(request_json);
    assert(ai_xiaozhi_format_content_length_header(
               message, sizeof(message), 559U) == 21);
    assert(strcmp(message, "Content-Length: 559\r\n") == 0);
    assert(ai_xiaozhi_format_content_length_header(
               message, 21U, 559U) < 0);
    assert(ai_xiaozhi_build_activation_request(
               request, sizeof(request), "bad\"mac",
               "00010203-0405-0607-0809-0a0b0c0d0e0f") < 0);
    assert(ai_xiaozhi_build_activation_request(
               request, 64U, "00:11:22:33:44:55",
               "00010203-0405-0607-0809-0a0b0c0d0e0f") < 0);
    assert(ai_xiaozhi_activation_http_status_ok(200) == 0);
    assert(ai_xiaozhi_activation_http_status_ok(400) < 0);
    assert(ai_xiaozhi_activation_http_status_ok(0) < 0);

    assert(ai_xiaozhi_parse_websocket_url(
               "wss://voice.example.com/xiaozhi/v1/?device=573", &endpoint) ==
           0);
    assert(strcmp(endpoint.host, "voice.example.com") == 0);
    assert(strcmp(endpoint.path, "/xiaozhi/v1/?device=573") == 0);
    assert(endpoint.port == 443U);
    assert(endpoint.ssl_enabled == 1);

    assert(ai_xiaozhi_parse_websocket_url("ws://192.0.2.1:9000/ws", &endpoint) ==
           0);
    assert(strcmp(endpoint.host, "192.0.2.1") == 0);
    assert(strcmp(endpoint.path, "/ws") == 0);
    assert(endpoint.port == 9000U);
    assert(endpoint.ssl_enabled == 0);
    assert(ai_xiaozhi_parse_websocket_url("https://voice.example.com/ws",
                                          &endpoint) < 0);
    assert(ai_xiaozhi_parse_websocket_url("wss://voice.example.com:0/ws",
                                          &endpoint) < 0);

    assert(ai_xiaozhi_parse_activation_response(activation_response,
                                                &activation) == 0);
    assert(strcmp(activation.endpoint.host, "voice.example.com") == 0);
    assert(strcmp(activation.endpoint.path, "/xiaozhi/v1/") == 0);
    assert(activation.endpoint.port == 443U);
    assert(activation.endpoint.ssl_enabled == 1);
    assert(strcmp(activation.token, "dynamic-token") == 0);
    assert(strcmp(activation.activation_code, "ABCD-1234") == 0);
    assert(activation.activation_pending == 1);

    assert(ai_xiaozhi_parse_activation_response(activated_response,
                                                &activation) == 0);
    assert(strcmp(activation.endpoint.host, "192.0.2.1") == 0);
    assert(activation.endpoint.port == 9000U);
    assert(strcmp(activation.token, "second-token") == 0);
    assert(activation.activation_code[0] == '\0');
    assert(activation.activation_pending == 0);
    assert(ai_xiaozhi_parse_activation_response(
               "{\"websocket\":{\"url\":\"wss://voice.example.com/ws\","
               "\"token\":\"bad\\r\\ntoken\"}}",
               &activation) < 0);
    assert(ai_xiaozhi_parse_activation_response(
               "{\"websocket\":{\"url\":\"https://voice.example.com/ws\","
               "\"token\":\"dynamic-token\"}}",
               &activation) < 0);
    assert(ai_xiaozhi_parse_server_hello(server_hello, &server) == 0);
    assert(server.protocol_version == 1U);
    assert(strcmp(server.session_id, "session-42") == 0);
    assert(strcmp(server.audio_format, "opus") == 0);
    assert(server.sample_rate == 24000U);
    assert(server.channels == 1U);
    assert(server.frame_duration == 60U);
    assert(ai_xiaozhi_parse_server_hello(server_hello_v2, &server) == 0);
    assert(server.protocol_version == 2U);
    assert(server.sample_rate == 16000U);
    assert(server.channels == 2U);
    assert(server.frame_duration == 20U);
    assert(ai_xiaozhi_parse_server_hello(
               "{\"type\":\"hello\",\"version\":1,"
               "\"transport\":\"websocket\",\"audio_params\":{"
               "\"format\":\"opus\",\"sample_rate\":\"24000\","
               "\"channels\":1,\"frame_duration\":60}}",
               &server) < 0);
    assert(ai_xiaozhi_parse_server_hello(
               "{\"type\":\"hello\",\"version\":1,"
               "\"transport\":\"websocket\",\"audio_params\":{"
               "\"format\":\"pcm\",\"sample_rate\":24000,"
               "\"channels\":1,\"frame_duration\":60}}",
               &server) < 0);
    assert(ai_xiaozhi_parse_mcp_vision_initialize(mcp_initialize, &vision) ==
           0);
    assert(strcmp(vision.endpoint.host, "vision.example.com") == 0);
    assert(strcmp(vision.endpoint.path, "/upload") == 0);
    assert(vision.endpoint.port == 443U);
    assert(vision.endpoint.ssl_enabled == 1);
    assert(strcmp(vision.token, "vision-token") == 0);
    assert(ai_xiaozhi_parse_mcp_vision_initialize_request(
               mcp_initialize, &vision, message, sizeof(message),
               &request_id) == 0);
    assert(strcmp(message, "session-42") == 0);
    assert(request_id == 1);
    assert(ai_xiaozhi_parse_mcp_vision_initialize(
               mcp_initialize_without_token, &vision) == 0);
    assert(vision.token[0] == '\0');
    assert(ai_xiaozhi_parse_mcp_vision_initialize_request(
               mcp_initialize_without_token, &vision, message, 8U,
               &request_id) < 0);
    assert(ai_xiaozhi_parse_mcp_vision_initialize_request(
               mcp_initialize_without_token, &vision, message,
               sizeof(message), &request_id) == 0);
    assert(strcmp(message, "session-42") == 0);
    assert(request_id == 1);
    assert(ai_xiaozhi_build_mcp_initialize_response(
               mcp_response, sizeof(mcp_response), "session-42", 1) > 0);
    assert(strcmp(
               mcp_response,
               "{\"session_id\":\"session-42\",\"type\":\"mcp\","
               "\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
               "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
               "\"serverInfo\":{\"name\":\"ai_camera\","
               "\"version\":\"1.0.0\"}}}}") == 0);
    assert(ai_xiaozhi_parse_mcp_vision_initialize(
               "{\"type\":\"mcp\",\"payload\":{\"method\":\"tools/list\"}}",
               &vision) < 0);
    assert(ai_xiaozhi_parse_mcp_vision_initialize(
               "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\","
               "\"method\":\"initialize\",\"params\":{\"capabilities\":{"
               "\"vision\":{\"url\":\"ws://bad.example/upload\","
               "\"token\":\"x\"}}}}}",
               &vision) < 0);
    assert(ai_xiaozhi_multipart_length("what is this", 1024U,
                                       "boundary-42") > 1024);
    assert(ai_xiaozhi_build_multipart_prefix(
               multipart, sizeof(multipart), "boundary-42", "what is this") >
           0);
    assert(strstr(multipart, "name=\"question\"") != NULL);
    assert(strstr(multipart, "what is this") != NULL);
    assert(strstr(multipart, "name=\"file\"; filename=\"camera.jpg\"") !=
           NULL);
    assert(ai_xiaozhi_build_multipart_suffix(
               multipart, sizeof(multipart), "boundary-42") > 0);
    assert(strcmp(multipart, "\r\n--boundary-42--\r\n") == 0);
    assert(ai_xiaozhi_build_multipart_prefix(
               multipart, sizeof(multipart), "bad\r\nboundary", "question") <
           0);
    assert(ai_xiaozhi_build_multipart_prefix(
               multipart, sizeof(multipart), "boundary", "bad\r\nquestion") <
           0);
    return 0;
}
