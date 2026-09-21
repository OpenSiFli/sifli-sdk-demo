#include "xiaozhi_protocol.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "cJSON.h"

#define AI_XIAOZHI_HELLO                                                   \
    "{\"type\":\"hello\",\"version\":3,\"transport\":\"websocket\"," \
    "\"features\":{\"mcp\":true},"                                  \
    "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"   \
    "\"channels\":1,\"frame_duration\":60}}"

static int ai_xiaozhi_session_id_is_safe(const char *session_id)
{
    const unsigned char *value = (const unsigned char *)session_id;

    if (value == NULL || *value == '\0')
        return 0;
    while (*value != '\0')
    {
        if (*value < 0x20U || *value == '"' || *value == '\\')
            return 0;
        ++value;
    }
    return 1;
}

static int ai_xiaozhi_format(char *out, size_t out_size, const char *format,
                             const char *session_id)
{
    int written;

    if (out == NULL || out_size == 0U)
        return -1;
    written = session_id == NULL ? snprintf(out, out_size, "%s", format)
                                 : snprintf(out, out_size, format, session_id);
    return written < 0 || (size_t)written >= out_size ? -1 : written;
}

static int ai_xiaozhi_copy_field(char *out, size_t out_size,
                                 const char *value, size_t value_size)
{
    if (value_size == 0U || value_size >= out_size)
        return -1;
    memcpy(out, value, value_size);
    out[value_size] = '\0';
    return 0;
}

static int ai_xiaozhi_host_is_safe(const char *host, size_t host_size)
{
    size_t index;

    for (index = 0U; index < host_size; ++index)
    {
        unsigned char character = (unsigned char)host[index];

        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '.' || character == '-'))
            return 0;
    }
    return 1;
}

static int ai_xiaozhi_path_is_safe(const char *path)
{
    while (*path != '\0')
    {
        unsigned char character = (unsigned char)*path++;

        if (character <= 0x20U || character == '\\')
            return 0;
    }
    return 1;
}

static int ai_xiaozhi_boundary_is_safe(const char *boundary)
{
    const unsigned char *value = (const unsigned char *)boundary;
    size_t size;

    if (boundary == NULL)
        return 0;
    size = strlen(boundary);
    if (size == 0U || size >= AI_XIAOZHI_MULTIPART_BOUNDARY_MAX)
        return 0;
    while (*value != '\0')
    {
        if (!((*value >= 'a' && *value <= 'z') ||
              (*value >= 'A' && *value <= 'Z') ||
              (*value >= '0' && *value <= '9') || *value == '-' ||
              *value == '_'))
            return 0;
        ++value;
    }
    return 1;
}

static int ai_xiaozhi_text_is_safe(const char *text)
{
    const unsigned char *value = (const unsigned char *)text;

    if (value == NULL || *value == '\0')
        return 0;
    while (*value != '\0')
    {
        if (*value < 0x20U || *value == 0x7fU)
            return 0;
        ++value;
    }
    return 1;
}

static int ai_xiaozhi_json_integer(cJSON *item, uint32_t minimum,
                                   uint32_t maximum, uint32_t *value)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < (double)minimum ||
        item->valuedouble > (double)maximum ||
        item->valuedouble != (double)item->valueint || value == NULL)
        return -1;
    *value = (uint32_t)item->valueint;
    return 0;
}

static char ai_xiaozhi_hex(uint8_t value)
{
    value &= 0x0fU;
    return (char)(value < 10U ? '0' + value : 'a' + value - 10U);
}

int ai_xiaozhi_build_hello(char *out, size_t out_size)
{
    return ai_xiaozhi_format(out, out_size, AI_XIAOZHI_HELLO, NULL);
}

int ai_xiaozhi_build_listen_start(char *out, size_t out_size,
                                  const char *session_id)
{
    static const char format[] =
        "{\"session_id\":\"%s\",\"type\":\"listen\","
        "\"state\":\"start\",\"mode\":\"manual\"}";

    return ai_xiaozhi_session_id_is_safe(session_id)
               ? ai_xiaozhi_format(out, out_size, format, session_id)
               : -1;
}

int ai_xiaozhi_build_listen_stop(char *out, size_t out_size,
                                 const char *session_id)
{
    static const char format[] =
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}";

    return ai_xiaozhi_session_id_is_safe(session_id)
               ? ai_xiaozhi_format(out, out_size, format, session_id)
               : -1;
}

int ai_xiaozhi_format_device_id(char *out, size_t out_size,
                                const uint8_t address[6])
{
    size_t index;
    size_t offset = 0U;

    if (out == NULL || address == NULL || out_size < AI_XIAOZHI_DEVICE_ID_MAX)
        return -1;
    for (index = 0U; index < 6U; ++index)
    {
        out[offset++] = ai_xiaozhi_hex(address[index] >> 4U);
        out[offset++] = ai_xiaozhi_hex(address[index]);
        if (index != 5U)
            out[offset++] = ':';
    }
    out[offset] = '\0';
    return (int)offset;
}

int ai_xiaozhi_format_pan_device_id(char *out, size_t out_size,
                                    const uint8_t pan_address[6])
{
    uint8_t address[6];

    if (pan_address == NULL)
        return -1;
    address[0] = pan_address[1];
    address[1] = pan_address[0];
    address[2] = pan_address[3];
    address[3] = pan_address[2];
    address[4] = pan_address[5];
    address[5] = pan_address[4];
    return ai_xiaozhi_format_device_id(out, out_size, address);
}

int ai_xiaozhi_format_client_id(char *out, size_t out_size,
                                const uint8_t digest[16])
{
    size_t index;
    size_t offset = 0U;

    if (out == NULL || digest == NULL || out_size < AI_XIAOZHI_CLIENT_ID_MAX)
        return -1;
    for (index = 0U; index < 16U; ++index)
    {
        out[offset++] = ai_xiaozhi_hex(digest[index] >> 4U);
        out[offset++] = ai_xiaozhi_hex(digest[index]);
        if (index == 3U || index == 5U || index == 7U || index == 9U)
            out[offset++] = '-';
    }
    out[offset] = '\0';
    return (int)offset;
}

int ai_xiaozhi_build_activation_request(char *out, size_t out_size,
                                        const char *device_id,
                                        const char *client_id)
{
    static const char format[] =
        "{\"version\":2,\"flash_size\":4194304,\"psram_size\":0,"
        "\"minimum_free_heap_size\":123456,\"mac_address\":\"%s\","
        "\"uuid\":\"%s\",\"chip_model_name\":\"sf32lb573\","
        "\"chip_info\":{\"model\":1,\"cores\":2,\"revision\":0,"
        "\"features\":0},\"application\":{\"name\":\"ai_camera\","
        "\"version\":\"1.0.0\",\"compile_time\":\"2021-01-01T00:00:00Z\","
        "\"idf_version\":\"SiFli-SDK\",\"elf_sha256\":\"\"},"
        "\"partition_table\":[{\"label\":\"app\",\"type\":1,"
        "\"subtype\":2,\"address\":10000,\"size\":100000}],"
        "\"ota\":{\"label\":\"ota_0\"},\"board\":{"
        "\"type\":\"spi-hdk_lb573ub7n6\",\"mac\":\"%s\"}}";
    int written;

    if (out == NULL || out_size == 0U ||
        !ai_xiaozhi_session_id_is_safe(device_id) ||
        !ai_xiaozhi_session_id_is_safe(client_id))
        return -1;
    written = snprintf(out, out_size, format, device_id, client_id, device_id);
    return written < 0 || (size_t)written >= out_size ? -1 : written;
}

int ai_xiaozhi_format_content_length_header(char *out, size_t out_size,
                                            size_t content_length)
{
    int written;

    if (out == NULL || out_size == 0U)
        return -1;
    written = snprintf(out, out_size, "Content-Length: %lu\r\n",
                       (unsigned long)content_length);
    return written < 0 || (size_t)written >= out_size ? -1 : written;
}

int ai_xiaozhi_activation_http_status_ok(int status)
{
    return status == 200 ? 0 : -1;
}

static int ai_xiaozhi_parse_url(const char *url, const char *secure_scheme,
                                const char *plain_scheme,
                                ai_xiaozhi_endpoint_t *endpoint)
{
    const char *authority;
    const char *authority_end;
    const char *port_separator;
    size_t host_size;
    uint32_t port;

    if (url == NULL || endpoint == NULL)
        return -1;
    memset(endpoint, 0, sizeof(*endpoint));
    if (strncmp(url, secure_scheme, strlen(secure_scheme)) == 0)
    {
        authority = url + strlen(secure_scheme);
        endpoint->ssl_enabled = 1;
        endpoint->port = 443U;
    }
    else if (strncmp(url, plain_scheme, strlen(plain_scheme)) == 0)
    {
        authority = url + strlen(plain_scheme);
        endpoint->ssl_enabled = 0;
        endpoint->port = 80U;
    }
    else
    {
        return -1;
    }
    if (*authority == '\0' || strchr(authority, '#') != NULL)
        return -1;

    authority_end = strpbrk(authority, "/?");
    if (authority_end == NULL)
        authority_end = authority + strlen(authority);
    port_separator = memchr(authority, ':', (size_t)(authority_end - authority));
    host_size = port_separator == NULL
                    ? (size_t)(authority_end - authority)
                    : (size_t)(port_separator - authority);
    if (!ai_xiaozhi_host_is_safe(authority, host_size) ||
        ai_xiaozhi_copy_field(endpoint->host, sizeof(endpoint->host), authority,
                              host_size) < 0)
        return -1;

    if (port_separator != NULL)
    {
        const char *port_text = port_separator + 1U;

        port = 0U;
        if (port_text == authority_end)
            return -1;
        while (port_text < authority_end)
        {
            if (*port_text < '0' || *port_text > '9')
                return -1;
            port = port * 10U + (uint32_t)(*port_text++ - '0');
            if (port > 65535U)
                return -1;
        }
        if (port == 0U)
            return -1;
        endpoint->port = (uint16_t)port;
    }

    if (*authority_end == '\0')
        return ai_xiaozhi_copy_field(endpoint->path, sizeof(endpoint->path), "/",
                                     1U);
    if (!ai_xiaozhi_path_is_safe(authority_end))
        return -1;
    if (*authority_end == '/')
        return ai_xiaozhi_copy_field(endpoint->path, sizeof(endpoint->path),
                                     authority_end, strlen(authority_end));
    if (strlen(authority_end) + 1U >= sizeof(endpoint->path))
        return -1;
    endpoint->path[0] = '/';
    memcpy(endpoint->path + 1U, authority_end, strlen(authority_end) + 1U);
    return 0;
}

int ai_xiaozhi_parse_websocket_url(const char *url,
                                   ai_xiaozhi_endpoint_t *endpoint)
{
    return ai_xiaozhi_parse_url(url, "wss://", "ws://", endpoint);
}

int ai_xiaozhi_parse_http_url(const char *url,
                              ai_xiaozhi_endpoint_t *endpoint)
{
    return ai_xiaozhi_parse_url(url, "https://", "http://", endpoint);
}

int ai_xiaozhi_parse_activation_response(const char *response,
                                         ai_xiaozhi_activation_t *activation)
{
    ai_xiaozhi_activation_t parsed = {0};
    cJSON *root;
    cJSON *websocket;
    cJSON *url;
    cJSON *token;
    cJSON *activation_object;
    cJSON *code;
    int result = -1;

    if (response == NULL || activation == NULL)
        return -1;
    root = cJSON_Parse(response);
    if (root == NULL)
        return -1;

    websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    url = websocket == NULL ? NULL :
          cJSON_GetObjectItemCaseSensitive(websocket, "url");
    token = websocket == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(websocket, "token");
    if (!cJSON_IsObject(websocket) || !cJSON_IsString(url) ||
        !cJSON_IsString(token) ||
        ai_xiaozhi_parse_websocket_url(url->valuestring, &parsed.endpoint) < 0 ||
        !ai_xiaozhi_text_is_safe(token->valuestring) ||
        ai_xiaozhi_copy_field(parsed.token, sizeof(parsed.token),
                              token->valuestring,
                              strlen(token->valuestring)) < 0)
        goto done;

    activation_object = cJSON_GetObjectItemCaseSensitive(root, "activation");
    if (activation_object != NULL)
    {
        code = cJSON_GetObjectItemCaseSensitive(activation_object, "code");
        if (!cJSON_IsObject(activation_object) || !cJSON_IsString(code) ||
            !ai_xiaozhi_text_is_safe(code->valuestring) ||
            ai_xiaozhi_copy_field(parsed.activation_code,
                                  sizeof(parsed.activation_code),
                                  code->valuestring,
                                  strlen(code->valuestring)) < 0)
            goto done;
        parsed.activation_pending = 1;
    }

    *activation = parsed;
    result = 0;
done:
    cJSON_Delete(root);
    return result;
}

int ai_xiaozhi_parse_server_hello(const char *response,
                                  ai_xiaozhi_server_hello_t *hello)
{
    ai_xiaozhi_server_hello_t parsed = {0};
    cJSON *root;
    cJSON *type;
    cJSON *version;
    cJSON *transport;
    cJSON *session_id;
    cJSON *audio_params;
    cJSON *format;
    cJSON *sample_rate;
    cJSON *channels;
    cJSON *frame_duration;
    uint32_t value;
    uint32_t version_value;
    uint32_t channels_value;
    int result = -1;

    if (response == NULL || hello == NULL)
        return -1;
    root = cJSON_Parse(response);
    if (root == NULL)
        return -1;

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    audio_params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
    format = audio_params == NULL
                 ? NULL
                 : cJSON_GetObjectItemCaseSensitive(audio_params, "format");
    sample_rate = audio_params == NULL
                      ? NULL
                      : cJSON_GetObjectItemCaseSensitive(audio_params,
                                                         "sample_rate");
    channels = audio_params == NULL
                   ? NULL
                   : cJSON_GetObjectItemCaseSensitive(audio_params, "channels");
    frame_duration = audio_params == NULL
                         ? NULL
                         : cJSON_GetObjectItemCaseSensitive(audio_params,
                                                            "frame_duration");

    if (!cJSON_IsString(type) || strcmp(type->valuestring, "hello") != 0 ||
        ai_xiaozhi_json_integer(version, 1U, 3U, &version_value) < 0 ||
        !cJSON_IsString(transport) ||
        strcmp(transport->valuestring, "websocket") != 0 ||
        !cJSON_IsString(format) || strcmp(format->valuestring, "opus") != 0 ||
        ai_xiaozhi_copy_field(parsed.audio_format,
                              sizeof(parsed.audio_format), format->valuestring,
                              strlen(format->valuestring)) < 0 ||
        ai_xiaozhi_json_integer(sample_rate, 8000U, 48000U, &parsed.sample_rate) <
            0 ||
        ai_xiaozhi_json_integer(channels, 1U, 2U, &channels_value) < 0)
        goto done;
    parsed.protocol_version = (uint8_t)version_value;
    parsed.channels = (uint8_t)channels_value;
    if (ai_xiaozhi_json_integer(frame_duration, 10U, 120U, &value) < 0)
        goto done;
    parsed.frame_duration = (uint16_t)value;
    if (session_id != NULL)
    {
        if (!cJSON_IsString(session_id) ||
            !ai_xiaozhi_session_id_is_safe(session_id->valuestring) ||
            ai_xiaozhi_copy_field(parsed.session_id, sizeof(parsed.session_id),
                                  session_id->valuestring,
                                  strlen(session_id->valuestring)) < 0)
            goto done;
    }

    *hello = parsed;
    result = 0;
done:
    cJSON_Delete(root);
    return result;
}

ai_xiaozhi_server_event_t ai_xiaozhi_parse_server_event(
    const char *response)
{
    cJSON *root;
    cJSON *type;
    cJSON *state;
    ai_xiaozhi_server_event_t event = AI_XIAOZHI_SERVER_EVENT_NONE;

    if (response == NULL)
        return event;
    root = cJSON_Parse(response);
    if (root == NULL)
        return event;
    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(type) && cJSON_IsString(state) &&
        strcmp(type->valuestring, "tts") == 0)
    {
        if (strcmp(state->valuestring, "start") == 0)
            event = AI_XIAOZHI_SERVER_EVENT_TTS_START;
        else if (strcmp(state->valuestring, "stop") == 0)
            event = AI_XIAOZHI_SERVER_EVENT_TTS_STOP;
    }
    cJSON_Delete(root);
    return event;
}

int ai_xiaozhi_parse_mcp_vision_initialize_request(
    const char *response, ai_xiaozhi_vision_capability_t *vision,
    char *session_id, size_t session_id_size, int *request_id)
{
    ai_xiaozhi_vision_capability_t parsed = {0};
    cJSON *root;
    cJSON *type;
    cJSON *envelope_session_id;
    cJSON *payload;
    cJSON *jsonrpc;
    cJSON *method;
    cJSON *id;
    cJSON *params;
    cJSON *capabilities;
    cJSON *vision_object;
    cJSON *url;
    cJSON *token;
    uint32_t parsed_request_id;
    int result = -1;

    if (response == NULL || vision == NULL ||
        (session_id != NULL && session_id_size == 0U) ||
        (session_id == NULL && session_id_size != 0U))
        return -1;
    root = cJSON_Parse(response);
    if (root == NULL)
        return -1;
    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    envelope_session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    jsonrpc = payload == NULL ? NULL :
              cJSON_GetObjectItemCaseSensitive(payload, "jsonrpc");
    method = payload == NULL ? NULL :
             cJSON_GetObjectItemCaseSensitive(payload, "method");
    id = payload == NULL ? NULL :
         cJSON_GetObjectItemCaseSensitive(payload, "id");
    params = payload == NULL ? NULL :
             cJSON_GetObjectItemCaseSensitive(payload, "params");
    capabilities = params == NULL ? NULL :
                   cJSON_GetObjectItemCaseSensitive(params, "capabilities");
    vision_object = capabilities == NULL ? NULL :
                    cJSON_GetObjectItemCaseSensitive(capabilities, "vision");
    url = vision_object == NULL ? NULL :
          cJSON_GetObjectItemCaseSensitive(vision_object, "url");
    token = vision_object == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(vision_object, "token");

    if (!cJSON_IsString(type) || strcmp(type->valuestring, "mcp") != 0 ||
        !cJSON_IsString(envelope_session_id) ||
        !ai_xiaozhi_session_id_is_safe(envelope_session_id->valuestring) ||
        !cJSON_IsObject(payload) || !cJSON_IsString(jsonrpc) ||
        strcmp(jsonrpc->valuestring, "2.0") != 0 || !cJSON_IsString(method) ||
        strcmp(method->valuestring, "initialize") != 0 ||
        ai_xiaozhi_json_integer(id, 0U, (uint32_t)INT_MAX,
                                &parsed_request_id) < 0 ||
        !cJSON_IsObject(params) || !cJSON_IsObject(capabilities) ||
        !cJSON_IsObject(vision_object) || !cJSON_IsString(url) ||
        ai_xiaozhi_parse_http_url(url->valuestring, &parsed.endpoint) < 0 ||
        (token != NULL &&
         (!cJSON_IsString(token) ||
          (token->valuestring[0] != '\0' &&
           (!ai_xiaozhi_text_is_safe(token->valuestring) ||
            ai_xiaozhi_copy_field(parsed.token, sizeof(parsed.token),
                                  token->valuestring,
                                  strlen(token->valuestring)) < 0)))))
        goto done;
    if (session_id != NULL && ai_xiaozhi_copy_field(
                                  session_id, session_id_size,
                                  envelope_session_id->valuestring,
                                  strlen(envelope_session_id->valuestring)) < 0)
        goto done;
    if (request_id != NULL)
        *request_id = (int)parsed_request_id;
    *vision = parsed;
    result = 0;
done:
    cJSON_Delete(root);
    return result;
}

int ai_xiaozhi_parse_mcp_vision_initialize(
    const char *response, ai_xiaozhi_vision_capability_t *vision)
{
    return ai_xiaozhi_parse_mcp_vision_initialize_request(
        response, vision, NULL, 0U, NULL);
}

int ai_xiaozhi_build_mcp_initialize_response(
    char *out, size_t out_size, const char *session_id, int request_id)
{
    static const char format[] =
        "{\"session_id\":\"%s\",\"type\":\"mcp\","
        "\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{"
        "\"protocolVersion\":\"2024-11-05\","
        /* Declaring the tools capability is what makes the server ask for
         * tools/list; without it the model never learns about the camera. */
        "\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"ai_camera\","
        "\"version\":\"1.0.0\"}}}}";
    int written;

    if (out == NULL || out_size == 0U ||
        !ai_xiaozhi_session_id_is_safe(session_id) || request_id < 0)
        return -1;
    written = snprintf(out, out_size, format, session_id, request_id);
    return written < 0 || (size_t)written >= out_size ? -1 : written;
}

/* Shared envelope check for the MCP requests the server sends us: type "mcp",
 * a safe session id, JSON-RPC 2.0, the expected method and an integer id. */
static int ai_xiaozhi_parse_mcp_request(const char *response, int tools_call,
                                        char *session_id, size_t session_id_size,
                                        int *request_id, char *tool_name,
                                        size_t tool_name_size, char *question,
                                        size_t question_size)
{
    cJSON *root;
    cJSON *type;
    cJSON *envelope_session_id;
    cJSON *payload;
    cJSON *jsonrpc;
    cJSON *method;
    cJSON *id;
    cJSON *params;
    uint32_t parsed_id;
    int result = -1;

    if (response == NULL)
        return -1;
    root = cJSON_Parse(response);
    if (root == NULL)
        return -1;
    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    envelope_session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    jsonrpc = payload == NULL ? NULL :
              cJSON_GetObjectItemCaseSensitive(payload, "jsonrpc");
    method = payload == NULL ? NULL :
             cJSON_GetObjectItemCaseSensitive(payload, "method");
    id = payload == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(payload, "id");
    params = payload == NULL ? NULL :
             cJSON_GetObjectItemCaseSensitive(payload, "params");

    if (!cJSON_IsString(type) || strcmp(type->valuestring, "mcp") != 0 ||
        !cJSON_IsString(envelope_session_id) ||
        !ai_xiaozhi_session_id_is_safe(envelope_session_id->valuestring) ||
        !cJSON_IsObject(payload) || !cJSON_IsString(jsonrpc) ||
        strcmp(jsonrpc->valuestring, "2.0") != 0 || !cJSON_IsString(method) ||
        strcmp(method->valuestring,
               tools_call ? "tools/call" : "tools/list") != 0 ||
        ai_xiaozhi_json_integer(id, 0U, (uint32_t)INT_MAX, &parsed_id) < 0)
        goto done;
    if (session_id != NULL &&
        ai_xiaozhi_copy_field(session_id, session_id_size,
                              envelope_session_id->valuestring,
                              strlen(envelope_session_id->valuestring)) < 0)
        goto done;
    if (request_id != NULL)
        *request_id = (int)parsed_id;
    if (tools_call)
    {
        cJSON *name;
        cJSON *arguments;
        cJSON *question_item;

        if (!cJSON_IsObject(params))
            goto done;
        name = cJSON_GetObjectItemCaseSensitive(params, "name");
        arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (!cJSON_IsString(name) ||
            !ai_xiaozhi_text_is_safe(name->valuestring))
            goto done;
        if (tool_name != NULL &&
            ai_xiaozhi_copy_field(tool_name, tool_name_size, name->valuestring,
                                  strlen(name->valuestring)) < 0)
            goto done;
        if (question != NULL)
        {
            question_item = cJSON_IsObject(arguments) ?
                            cJSON_GetObjectItemCaseSensitive(arguments,
                                                             "question") :
                            NULL;
            /* The model may omit the question: fall back to the default. */
            if (question_item == NULL)
                question[0] = '\0';
            else if (!cJSON_IsString(question_item) ||
                     !ai_xiaozhi_text_is_safe(question_item->valuestring) ||
                     ai_xiaozhi_copy_field(question, question_size,
                                           question_item->valuestring,
                                           strlen(question_item->valuestring)) <
                         0)
                goto done;
        }
    }
    result = 0;
done:
    cJSON_Delete(root);
    return result;
}

int ai_xiaozhi_parse_mcp_tools_list_request(const char *response,
                                           char *session_id,
                                           size_t session_id_size,
                                           int *request_id)
{
    if (session_id == NULL || session_id_size == 0U)
        return -1;
    return ai_xiaozhi_parse_mcp_request(response, 0, session_id,
                                        session_id_size, request_id, NULL, 0U,
                                        NULL, 0U);
}

int ai_xiaozhi_parse_mcp_tools_call_request(const char *response,
                                           char *session_id,
                                           size_t session_id_size,
                                           int *request_id, char *tool_name,
                                           size_t tool_name_size,
                                           char *question,
                                           size_t question_size)
{
    if (session_id == NULL || session_id_size == 0U || tool_name == NULL ||
        tool_name_size == 0U || question == NULL || question_size == 0U)
        return -1;
    return ai_xiaozhi_parse_mcp_request(response, 1, session_id,
                                        session_id_size, request_id, tool_name,
                                        tool_name_size, question,
                                        question_size);
}

int ai_xiaozhi_build_mcp_tools_list_response(char *out, size_t out_size,
                                            const char *session_id,
                                            int request_id)
{
    static const char format[] =
        "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\","
        "\"id\":%d,\"result\":{\"tools\":[{\"name\":\"self.camera.take_photo\","
        "\"description\":\"Take a photo with the device camera and return a "
        "description of what it shows. Call this whenever the user asks about a "
        "picture or about what the camera can see.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"question\":{"
        "\"type\":\"string\",\"description\":\"The question to answer about the "
        "photo\"}},\"required\":[\"question\"]}}]}}}";
    int written;

    if (out == NULL || out_size == 0U ||
        !ai_xiaozhi_session_id_is_safe(session_id) || request_id < 0)
        return -1;
    written = snprintf(out, out_size, format, session_id, request_id);
    return written < 0 || (size_t)written >= out_size ? -1 : written;
}

int ai_xiaozhi_build_mcp_tool_result(char *out, size_t out_size,
                                     const char *session_id, int request_id,
                                     const char *text, int is_error)
{
    cJSON *root;
    cJSON *payload;
    cJSON *result;
    cJSON *content;
    cJSON *item;
    char *printed;
    size_t length;
    int status = -1;

    if (out == NULL || out_size == 0U ||
        !ai_xiaozhi_session_id_is_safe(session_id) || request_id < 0 ||
        text == NULL)
        return -1;
    root = cJSON_CreateObject();
    payload = cJSON_CreateObject();
    result = cJSON_CreateObject();
    content = cJSON_CreateArray();
    item = cJSON_CreateObject();
    if (root == NULL || payload == NULL || result == NULL || content == NULL ||
        item == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(payload);
        cJSON_Delete(result);
        cJSON_Delete(content);
        cJSON_Delete(item);
        return -1;
    }
    /* cJSON escapes the text, so quotes and newlines cannot break the frame. */
    cJSON_AddStringToObject(root, "session_id", session_id);
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(payload, "id", (double)request_id);
    cJSON_AddItemToObject(payload, "result", result);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", is_error != 0);
    printed = cJSON_PrintUnformatted(root);
    if (printed != NULL)
    {
        length = strlen(printed);
        if (length < out_size)
        {
            memcpy(out, printed, length + 1U);
            status = (int)length;
        }
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return status;
}

int ai_xiaozhi_parse_vision_explain_response(const char *response, char *text,
                                             size_t text_size)
{
    cJSON *root;
    cJSON *item;
    int result = -1;

    if (response == NULL || text == NULL || text_size == 0U)
        return -1;
    text[0] = '\0';
    root = cJSON_Parse(response);
    if (root == NULL)
        return -1;
    item = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(item) && item->valuestring[0] != '\0' &&
        ai_xiaozhi_copy_field(text, text_size, item->valuestring,
                              strlen(item->valuestring)) == 0)
        result = 0;
    cJSON_Delete(root);
    return result;
}

int ai_xiaozhi_build_multipart_prefix(char *out, size_t out_size,
                                      const char *boundary,
                                      const char *question)
{
    static const char format[] =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"question\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; "
        "filename=\"camera.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";
    int written;

    if (out == NULL || out_size == 0U || !ai_xiaozhi_boundary_is_safe(boundary) ||
        !ai_xiaozhi_text_is_safe(question))
        return -1;
    written = snprintf(out, out_size, format, boundary, question, boundary);
    return written < 0 || (size_t)written >= out_size ? -1 : written;
}

int ai_xiaozhi_build_multipart_suffix(char *out, size_t out_size,
                                      const char *boundary)
{
    int written;

    if (out == NULL || out_size == 0U || !ai_xiaozhi_boundary_is_safe(boundary))
        return -1;
    written = snprintf(out, out_size, "\r\n--%s--\r\n", boundary);
    return written < 0 || (size_t)written >= out_size ? -1 : written;
}

int ai_xiaozhi_multipart_length(const char *question, size_t jpeg_size,
                                const char *boundary)
{
    char prefix[512];
    char suffix[AI_XIAOZHI_MULTIPART_BOUNDARY_MAX + 10U];
    int prefix_size;
    int suffix_size;

    prefix_size = ai_xiaozhi_build_multipart_prefix(prefix, sizeof(prefix),
                                                    boundary, question);
    suffix_size = ai_xiaozhi_build_multipart_suffix(suffix, sizeof(suffix),
                                                    boundary);
    if (prefix_size < 0 || suffix_size < 0 ||
        jpeg_size > (size_t)-1 - (size_t)prefix_size - (size_t)suffix_size)
        return -1;
    return jpeg_size + (size_t)prefix_size + (size_t)suffix_size > (size_t)INT_MAX
               ? -1
               : (int)(jpeg_size + (size_t)prefix_size + (size_t)suffix_size);
}
