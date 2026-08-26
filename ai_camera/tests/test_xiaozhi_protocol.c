#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "xiaozhi_protocol.h"

int main(void)
{
    static const char hello[] =
        "{\"type\":\"hello\",\"version\":3,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60}}";
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
    static const char activation_request[] =
        "{\"version\":2,\"mac_address\":\"00:11:22:33:44:55\","
        "\"uuid\":\"00010203-0405-0607-0809-0a0b0c0d0e0f\","
        "\"chip_model_name\":\"sf32lb573\",\"board\":{"
        "\"type\":\"spi-hdk_lb573ub7n6\",\"mac\":\"00:11:22:33:44:55\"}}";
    static const uint8_t address[] = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
    static const uint8_t digest[] = {0x00U, 0x01U, 0x02U, 0x03U,
                                     0x04U, 0x05U, 0x06U, 0x07U,
                                     0x08U, 0x09U, 0x0aU, 0x0bU,
                                     0x0cU, 0x0dU, 0x0eU, 0x0fU};
    ai_xiaozhi_endpoint_t endpoint;
    ai_xiaozhi_activation_t activation;
    char message[sizeof(hello)];
    char request[AI_XIAOZHI_ACTIVATION_REQUEST_MAX];

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
    assert(ai_xiaozhi_format_client_id(message, sizeof(message), digest) == 36);
    assert(strcmp(message, "00010203-0405-0607-0809-0a0b0c0d0e0f") == 0);
    assert(ai_xiaozhi_format_client_id(message, 36U, digest) < 0);
    assert(ai_xiaozhi_build_activation_request(
               request, sizeof(request), "00:11:22:33:44:55",
               "00010203-0405-0607-0809-0a0b0c0d0e0f") ==
           (int)strlen(activation_request));
    assert(strcmp(request, activation_request) == 0);
    assert(ai_xiaozhi_build_activation_request(
               request, sizeof(request), "bad\"mac",
               "00010203-0405-0607-0809-0a0b0c0d0e0f") < 0);
    assert(ai_xiaozhi_build_activation_request(
               request, sizeof(activation_request) - 1U, "00:11:22:33:44:55",
               "00010203-0405-0607-0809-0a0b0c0d0e0f") < 0);

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
    return 0;
}
