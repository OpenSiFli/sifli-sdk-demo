#include "xiaozhi_vision.h"

#include <dfs_posix.h>
#include <finsh.h>
#include <lwip/tcpip.h>
#include <rtthread.h>
#include <webclient.h>

#include <string.h>

#include "photo_storage/photo_store.h"
#include "xiaozhi_websocket.h"

#define AI_XIAOZHI_VISION_HEADER_MAX 1024U
#define AI_XIAOZHI_VISION_URL_MAX 512U
#define AI_XIAOZHI_VISION_STREAM_SIZE 2048U
/* The upload frame alone (header + multipart prefix + stream buffer) is several
 * kilobytes, and an https endpoint adds a TLS handshake on top, so it must not
 * run on the 4 KB shell thread. Callers hand the work to this thread instead. */
#define AI_XIAOZHI_VISION_STACK_SIZE (16U * 1024U)
#define AI_XIAOZHI_VISION_THREAD_PRIORITY 20U
#define AI_XIAOZHI_VISION_PATH_MAX 96U
#define AI_XIAOZHI_VISION_QUESTION_MAX 128U
#define AI_XIAOZHI_VISION_DEFAULT_QUESTION "请描述这张图片。"

extern int webclient_handle_response(struct webclient_session *session);

static ai_xiaozhi_vision_capability_t s_capability;
static rt_mutex_t s_capability_lock;
/* Cleared when the session that handed the capability out ends. */
static volatile int s_capability_valid;

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
    if (remaining <= 0)
        rt_kprintf("vision: content_length %d\n", remaining);
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
    s_capability_valid = 1;
    rt_mutex_release(s_capability_lock);
    /* Diagnostic: proves the session handed out a usable endpoint. */
    rt_kprintf("vision: endpoint %s://%s:%u%s\n",
               capability->endpoint.ssl_enabled ? "https" : "http",
               capability->endpoint.host, (unsigned int)capability->endpoint.port,
               capability->endpoint.path);
    rt_kprintf("vision: token_len=%u\n",
               (unsigned int)strlen(capability->token));
    return 0;
}

void ai_xiaozhi_vision_clear_capability(void)
{
    if (s_capability_lock == RT_NULL)
    {
        s_capability_valid = 0;
        return;
    }
    if (rt_mutex_take(s_capability_lock, RT_WAITING_FOREVER) != RT_EOK)
        return;
    memset(&s_capability, 0, sizeof(s_capability));
    s_capability_valid = 0;
    rt_mutex_release(s_capability_lock);
    rt_kprintf("vision: capability cleared\n");
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
    rt_tick_t begin = 0U;

    if (path == RT_NULL || question == RT_NULL)
        return -1;
    if (s_capability_lock == RT_NULL || !s_capability_valid)
    {
        /* No usable capability: the session has not handed one out (or the one
         * it handed out belonged to a session that already ended). */
        rt_kprintf("vision: no capability yet\n");
        return -1;
    }
    if (rt_mutex_take(s_capability_lock, RT_WAITING_FOREVER) != RT_EOK)
        return -1;
    capability = s_capability;
    rt_mutex_release(s_capability_lock);
    begin = rt_tick_get();
    if (stat(path, &info) != 0 || info.st_size <= 0 ||
        ai_xiaozhi_vision_build_url(&capability.endpoint, url,
                                    sizeof(url)) < 0)
    {
        rt_kprintf("vision: cannot read %s\n", path);
        return -1;
    }
    rt_kprintf("vision: post %s (%u bytes)\n", capability.endpoint.path,
               (unsigned int)info.st_size);
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
    rt_kprintf("vision: upload %s elapsed_ms=%lu\n",
               result == 0 ? "ok" : "FAILED",
               (unsigned long)((rt_tick_get() - begin) * 1000U /
                               RT_TICK_PER_SECOND));
    return result;
}

/* rt_kprintf truncates long lines, so the body is printed in short chunks. */
static void ai_xiaozhi_vision_print_response(const char *response)
{
    size_t length = strlen(response);
    size_t offset;

    rt_kprintf("vision: response %u bytes\n", (unsigned int)length);
    for (offset = 0U; offset < length; offset += 96U)
    {
        char line[112];
        size_t size = length - offset;

        if (size > 96U)
            size = 96U;
        rt_memcpy(line, response + offset, size);
        line[size] = '\0';
        rt_kprintf("v| %s\n", line);
    }
}

typedef struct
{
    /* 1 for an MCP camera tool call, 0 for the console test command. */
    int tool_call;
    char session_id[AI_XIAOZHI_SESSION_ID_MAX];
    int request_id;
    char path[AI_XIAOZHI_VISION_PATH_MAX];
    char question[AI_XIAOZHI_VISION_QUESTION_MAX];
} ai_xiaozhi_vision_job_t;

static ai_xiaozhi_vision_job_t s_job;
static volatile int s_job_busy;
static ai_xiaozhi_vision_photo_provider_t s_photo_provider;

void ai_xiaozhi_vision_set_photo_provider(
    ai_xiaozhi_vision_photo_provider_t provider)
{
    s_photo_provider = provider;
}

/* The photo a question is about: the one the user just took, otherwise the
 * newest one in the album (the model asked for a picture either way). */
static int ai_xiaozhi_vision_pick_photo(char *path, size_t size)
{
    photo_store_info_t items[1];
    size_t written = 0U;
    uint32_t total = 0U;

    if (s_photo_provider != RT_NULL && s_photo_provider(path, size) == 0)
    {
        rt_kprintf("vision: photo pending in the UI\n");
        return 0;
    }
    if (photo_store_list(0U, items, 1U, &written, &total) == 0 &&
        written == 1U)
    {
        rt_kprintf("vision: no pending photo, newest in the album\n");
        rt_strncpy(path, items[0].path, size - 1U);
        path[size - 1U] = '\0';
        return 0;
    }
    return -1;
}

static void ai_xiaozhi_vision_send_tool_result(const char *text, int is_error)
{
    char result[AI_XIAOZHI_MCP_TOOL_RESULT_MAX];
    int size = ai_xiaozhi_build_mcp_tool_result(result, sizeof(result),
                                                s_job.session_id,
                                                s_job.request_id, text,
                                                is_error);

    if (size < 0)
    {
        rt_kprintf("vision: tool result build failed\n");
        return;
    }
    if (ai_xiaozhi_websocket_send_text(result, (size_t)size) != 0)
        rt_kprintf("vision: tool result send failed\n");
    else
        rt_kprintf("vision: tool result sent\n");
}

static void ai_xiaozhi_vision_worker(void *parameter)
{
    char response[AI_XIAOZHI_VISION_EXPLAIN_MAX];
    char text[AI_XIAOZHI_VISION_TEXT_MAX];
    const char *question = s_job.question[0] != '\0'
                               ? s_job.question
                               : AI_XIAOZHI_VISION_DEFAULT_QUESTION;

    (void)parameter;
    if (s_job.tool_call)
    {
        if (ai_xiaozhi_vision_pick_photo(s_job.path, sizeof(s_job.path)) < 0)
        {
            rt_kprintf("vision: no photo available\n");
            ai_xiaozhi_vision_send_tool_result("设备里还没有照片。", 1);
            goto done;
        }
        rt_kprintf("vision: tool call photo %s\n", s_job.path);
    }
    if (ai_xiaozhi_vision_upload_file(s_job.path, question, response,
                                      sizeof(response)) < 0)
    {
        rt_kprintf("vision: upload failed\n");
        if (s_job.tool_call)
            ai_xiaozhi_vision_send_tool_result("图片上传失败，请稍后再试。", 1);
        goto done;
    }
    if (!s_job.tool_call)
    {
        ai_xiaozhi_vision_print_response(response);
        goto done;
    }
    if (ai_xiaozhi_parse_vision_explain_response(response, text,
                                                sizeof(text)) < 0)
    {
        /* Keep the model informed even if the body is not valid JSON. */
        rt_strncpy(text, response, sizeof(text) - 1U);
        text[sizeof(text) - 1U] = '\0';
    }
    rt_kprintf("vision: tool result %u bytes\n", (unsigned int)strlen(text));
    ai_xiaozhi_vision_send_tool_result(text, 0);
done:
    s_job_busy = 0;
    /* Just return: rt_thread_exit() runs after the entry returns and does the
     * teardown (timer detach, defunct list, stack release by the idle thread).
     * Calling rt_thread_delete(rt_thread_self()) as well detaches the thread
     * timer twice and trips the assert in rt_timer_detach(). */
}

static int ai_xiaozhi_vision_start_worker(void)
{
    rt_thread_t thread = rt_thread_create(
        "vision", ai_xiaozhi_vision_worker, RT_NULL,
        AI_XIAOZHI_VISION_STACK_SIZE, AI_XIAOZHI_VISION_THREAD_PRIORITY, 10U);

    if (thread == RT_NULL)
        return -1;
    rt_thread_startup(thread);
    return 0;
}

int ai_xiaozhi_vision_request_tool_call(const char *session_id, int request_id,
                                        const char *question)
{
    if (session_id == RT_NULL || question == RT_NULL || request_id < 0)
        return -1;
    if (s_job_busy)
        return -1;
    s_job_busy = 1;
    s_job.tool_call = 1;
    s_job.request_id = request_id;
    s_job.path[0] = '\0';
    rt_strncpy(s_job.session_id, session_id, sizeof(s_job.session_id) - 1U);
    s_job.session_id[sizeof(s_job.session_id) - 1U] = '\0';
    rt_strncpy(s_job.question, question, sizeof(s_job.question) - 1U);
    s_job.question[sizeof(s_job.question) - 1U] = '\0';
    if (ai_xiaozhi_vision_start_worker() != 0)
    {
        s_job_busy = 0;
        return -1;
    }
    return 0;
}

static void ai_xiaozhi_vision_upload(int argc, char **argv)
{
    const char *question = AI_XIAOZHI_VISION_DEFAULT_QUESTION;

    if (argc < 2 || argc > 3)
    {
        rt_kprintf("usage: ai_xiaozhi_vision_upload <jpeg-path> [question]\n");
        return;
    }
    if (argc == 3)
        question = argv[2];
    if (s_job_busy)
    {
        rt_kprintf("vision: another upload is running\n");
        return;
    }
    s_job_busy = 1;
    s_job.tool_call = 0;
    s_job.session_id[0] = '\0';
    s_job.request_id = 0;
    rt_strncpy(s_job.path, argv[1], sizeof(s_job.path) - 1U);
    s_job.path[sizeof(s_job.path) - 1U] = '\0';
    rt_strncpy(s_job.question, question, sizeof(s_job.question) - 1U);
    s_job.question[sizeof(s_job.question) - 1U] = '\0';
    if (ai_xiaozhi_vision_start_worker() != 0)
    {
        s_job_busy = 0;
        rt_kprintf("vision: cannot start the upload thread\n");
        return;
    }
    rt_kprintf("vision: uploading %s\n", s_job.path);
}
MSH_CMD_EXPORT(ai_xiaozhi_vision_upload, upload JPEG for XiaoZhi recognition);
