#include "xiaozhi_protocol.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define AI_XIAOZHI_HELLO                                                   \
    "{\"type\":\"hello\",\"version\":3,\"transport\":\"websocket\"," \
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
        "{\"version\":2,\"mac_address\":\"%s\",\"uuid\":\"%s\","
        "\"chip_model_name\":\"sf32lb573\",\"board\":{"
        "\"type\":\"spi-hdk_lb573ub7n6\",\"mac\":\"%s\"}}";
    int written;

    if (out == NULL || out_size == 0U ||
        !ai_xiaozhi_session_id_is_safe(device_id) ||
        !ai_xiaozhi_session_id_is_safe(client_id))
        return -1;
    written = snprintf(out, out_size, format, device_id, client_id, device_id);
    return written < 0 || (size_t)written >= out_size ? -1 : written;
}

int ai_xiaozhi_parse_websocket_url(const char *url,
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
    if (strncmp(url, "wss://", 6U) == 0)
    {
        authority = url + 6U;
        endpoint->ssl_enabled = 1;
        endpoint->port = 443U;
    }
    else if (strncmp(url, "ws://", 5U) == 0)
    {
        authority = url + 5U;
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
