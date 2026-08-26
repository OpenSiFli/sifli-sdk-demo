#include "xiaozhi_vision.h"

#include <dfs_posix.h>
#include <finsh.h>
#include <lwip/tcpip.h>
#include <rtthread.h>
#include <webclient.h>

#include <string.h>

#define AI_XIAOZHI_VISION_HEADER_MAX 1024U
#define AI_XIAOZHI_VISION_URL_MAX 512U
#define AI_XIAOZHI_VISION_STREAM_SIZE 2048U

extern int webclient_handle_response(struct webclient_session *session);

static ai_xiaozhi_vision_capability_t s_capability;
static rt_mutex_t s_capability_lock;

static int ai_xiaozhi_vision_write(struct webclient_session *session,
                                   const void *data, size_t size)
{
    return webclient_write(session, data, size) == (int)size ? 0 : -1;
}

static int ai_xiaozhi_vision_build_url(const ai_xiaozhi_endpoint_t *endpoint,
                                       char *url, size_t url_size)
{
    int written;

    if (endpoint == RT_NULL || url == RT_NULL || url_size == 0U)
        return -1;
    written = rt_snprintf(url, url_size, "%s://%s:%u%s",
                          endpoint->ssl_enabled ? "https" : "http",
                          endpoint->host, endpoint->port, endpoint->path);
    return written < 0 || (size_t)written >= url_size ? -1 : 0;
}

static int ai_xiaozhi_vision_response(struct webclient_session *session,
                                      char *response, size_t response_size)
{
    char buffer[AI_XIAOZHI_VISION_RESPONSE_MAX];
    int remaining;
    size_t copied = 0U;

    if (webclient_handle_response(session) != 200)
        return -1;
    remaining = webclient_content_length_get(session);
    if (response == RT_NULL || response_size == 0U)
        return remaining == 0 ? 0 : -1;
    response[0] = '\0';
    while (remaining > 0)
    {
        size_t request_size = (size_t)remaining;
        int received;

        if (request_size > sizeof(buffer))
            request_size = sizeof(buffer);
        received = webclient_read(session, buffer, request_size);
        if (received <= 0)
            return -1;
        if (copied < response_size - 1U)
        {
            size_t available = response_size - 1U - copied;
            size_t size = (size_t)received > available ? available :
                          (size_t)received;

            rt_memcpy(response + copied, buffer, size);
            copied += size;
        }
        remaining -= received;
    }
    response[copied] = '\0';
    return 0;
}

int ai_xiaozhi_vision_set_capability(
    const ai_xiaozhi_vision_capability_t *capability)
{
    if (capability == RT_NULL || capability->endpoint.host[0] == '\0' ||
        capability->endpoint.path[0] == '\0')
        return -1;
    if (s_capability_lock == RT_NULL)
        s_capability_lock = rt_mutex_create("xz_vis", RT_IPC_FLAG_PRIO);
    if (s_capability_lock == RT_NULL ||
        rt_mutex_take(s_capability_lock, RT_WAITING_FOREVER) != RT_EOK)
        return -1;
    s_capability = *capability;
    rt_mutex_release(s_capability_lock);
    return 0;
}

int ai_xiaozhi_vision_upload_file(const char *path, const char *question,
                                  char *response, size_t response_size)
{
    ai_xiaozhi_vision_capability_t capability;
    struct webclient_session *session = RT_NULL;
    struct stat info;
    char url[AI_XIAOZHI_VISION_URL_MAX];
    char boundary[AI_XIAOZHI_MULTIPART_BOUNDARY_MAX];
    char prefix[512];
    char suffix[AI_XIAOZHI_MULTIPART_BOUNDARY_MAX + 10U];
    char header[AI_XIAOZHI_VISION_HEADER_MAX];
    char authorization[AI_XIAOZHI_TOKEN_MAX + 32U];
    uint8_t buffer[AI_XIAOZHI_VISION_STREAM_SIZE];
    int fd = -1;
    int prefix_size;
    int suffix_size;
    int content_length;
    int header_size;
    int result = -1;

    if (path == RT_NULL || question == RT_NULL ||
        s_capability_lock == RT_NULL ||
        rt_mutex_take(s_capability_lock, RT_WAITING_FOREVER) != RT_EOK)
        return -1;
    capability = s_capability;
    rt_mutex_release(s_capability_lock);
    if (stat(path, &info) != 0 || info.st_size <= 0 ||
        ai_xiaozhi_vision_build_url(&capability.endpoint, url,
                                    sizeof(url)) < 0)
        return -1;
    rt_snprintf(boundary, sizeof(boundary), "ai-camera-%lu",
                (unsigned long)rt_tick_get());
    prefix_size = ai_xiaozhi_build_multipart_prefix(prefix, sizeof(prefix),
                                                    boundary, question);
    suffix_size = ai_xiaozhi_build_multipart_suffix(suffix, sizeof(suffix),
                                                    boundary);
    content_length = ai_xiaozhi_multipart_length(question, (size_t)info.st_size,
                                                  boundary);
    if (prefix_size < 0 || suffix_size < 0 || content_length < 0)
        return -1;
    authorization[0] = '\0';
    if (capability.token[0] != '\0')
    {
        int authorization_size = rt_snprintf(
            authorization, sizeof(authorization), "Authorization: Bearer %s\r\n",
            capability.token);

        if (authorization_size < 0 ||
            (size_t)authorization_size >= sizeof(authorization))
            return -1;
    }
    header_size = rt_snprintf(
        header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: RT-Thread HTTP Agent\r\n"
        "Accept: */*\r\n"
        "%s"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %d\r\n\r\n",
        capability.endpoint.path, capability.endpoint.host, authorization,
        boundary, content_length);
    if (header_size < 0 || (size_t)header_size >= sizeof(header))
        return -1;
    fd = open(path, O_RDONLY, 0);
    session = webclient_session_create(AI_XIAOZHI_VISION_HEADER_MAX);
    if (fd < 0 || session == RT_NULL ||
        webclient_send_usermethod(session, url, header, (size_t)header_size) <
            0 ||
        ai_xiaozhi_vision_write(session, prefix, (size_t)prefix_size) < 0)
        goto done;
    for (;;)
    {
        int read_size = read(fd, buffer, sizeof(buffer));

        if (read_size < 0)
            goto done;
        if (read_size == 0)
            break;
        if (ai_xiaozhi_vision_write(session, buffer, (size_t)read_size) < 0)
            goto done;
    }
    if (ai_xiaozhi_vision_write(session, suffix, (size_t)suffix_size) < 0)
        goto done;
    result = ai_xiaozhi_vision_response(session, response, response_size);
done:
    if (fd >= 0)
        close(fd);
    if (session != RT_NULL)
    {
        LOCK_TCPIP_CORE();
        webclient_close(session);
        UNLOCK_TCPIP_CORE();
    }
    return result;
}

static void ai_xiaozhi_vision_upload(int argc, char **argv)
{
    char response[AI_XIAOZHI_VISION_RESPONSE_MAX];
    const char *question = "Please describe this image.";

    if (argc == 3)
        question = argv[2];
    if (argc < 2 || argc > 3)
    {
        rt_kprintf("usage: ai_xiaozhi_vision <jpeg-path> [question]\n");
        return;
    }
    if (ai_xiaozhi_vision_upload_file(argv[1], question, response,
                                      sizeof(response)) < 0)
    {
        rt_kprintf("xiaozhi: vision upload failed\n");
        return;
    }
    rt_kprintf("xiaozhi: vision response %s\n", response);
}
MSH_CMD_EXPORT(ai_xiaozhi_vision_upload, upload JPEG for XiaoZhi recognition);
