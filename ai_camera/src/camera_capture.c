#include "camera_capture.h"

#include <dfs_posix.h>
#include <dfs_fs.h>
#include <drv_flash.h>
#include <errno.h>
#include <fcntl.h>
#include <mem_section.h>
#include <rtthread.h>
#include <mem_map.h>

#include "camera_handle.h"
#include "device_ui.h"
#include "xiaozhi_vision.h"

#define AI_CAMERA_PHOTO_DIR "/photo"
#define AI_CAMERA_PHOTO_PATH "/photo/latest.jpg"
#define AI_CAMERA_FILESYSTEM_DEVICE "camfs"
#define AI_CAMERA_PHOTO_BUFFER_SIZE (2U * 1024U * 1024U)
#define AI_CAMERA_CAPTURE_STACK_SIZE 3072U
#define AI_CAMERA_CAPTURE_PRIORITY 18U

static rt_sem_t s_capture_sem;
static rt_mutex_t s_capture_lock;
static rt_thread_t s_capture_thread;
static rt_bool_t s_capture_pending;
static rt_bool_t s_capture_initialized;
static rt_bool_t s_filesystem_registered;
static rt_bool_t s_filesystem_ready;
static camera_handler_instance_t *s_camera;
static unsigned int s_photo_index;

static uint8_t s_photo_buffer[AI_CAMERA_PHOTO_BUFFER_SIZE]
    L2_RET_BSS_SECT(s_photo_buffer);

static int ai_camera_save_file(const char *path, const void *buffer,
                               rt_size_t size)
{
    int fd;
    rt_size_t written = 0U;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
        return -1;
    while (written < size)
    {
        int result = write(fd, (const uint8_t *)buffer + written,
                           size - written);

        if (result <= 0)
        {
            close(fd);
            return -1;
        }
        written += (rt_size_t)result;
    }
    if (fsync(fd) != 0 || close(fd) != 0)
        return -1;
    return 0;
}

static int ai_camera_filesystem_init(void)
{
    const char *mounted_path;

    if (s_filesystem_ready)
        return 0;
    if (!s_filesystem_registered)
    {
        register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE,
                            AI_CAMERA_FILESYSTEM_DEVICE);
        s_filesystem_registered = RT_TRUE;
    }
    mounted_path = dfs_filesystem_get_mounted_path(
        rt_device_find(AI_CAMERA_FILESYSTEM_DEVICE));
    if (mounted_path == RT_NULL)
    {
        if (dfs_mount(AI_CAMERA_FILESYSTEM_DEVICE, "/", "elm", 0,
                      RT_NULL) != 0 &&
            (dfs_mkfs("elm", AI_CAMERA_FILESYSTEM_DEVICE) != 0 ||
             dfs_mount(AI_CAMERA_FILESYSTEM_DEVICE, "/", "elm", 0,
                       RT_NULL) != 0))
        {
            rt_kprintf("camera: mount fs_root failed\n");
            return -1;
        }
    }
    s_filesystem_ready = RT_TRUE;
    rt_kprintf("camera: fs_root mounted at /\n");
    return 0;
}

static int ai_camera_prepare(void)
{
    camera_capture_config_t config = {
        .pixformat = PIXFORMAT_JPEG,
        .framesize = FRAMESIZE_UXGA,
        .quality = 10U,
    };
    camera_handle_status_t status;

    if (s_camera != RT_NULL)
        return 0;
    status = camera_handler_instance_init(&s_camera);
    if (status != CAMERA_OK)
    {
        rt_kprintf("camera: init failed %d\n", status);
        return -1;
    }
    status = camera_change_settings(s_camera, &config);
    if (status != CAMERA_OK)
    {
        rt_kprintf("camera: configure UXGA JPEG failed %d\n", status);
        camera_deinit(&s_camera);
        return -1;
    }
    rt_kprintf("camera: Arducam OV2640 still mode 1600x1200 JPEG\n");
    return 0;
}

static void ai_camera_capture_worker(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        camera_capture_request_t request;
        camera_handle_status_t status;
        char numbered_path[32];
        char response[AI_XIAOZHI_VISION_RESPONSE_MAX];

        rt_sem_take(s_capture_sem, RT_WAITING_FOREVER);
        if (ai_camera_filesystem_init() < 0)
            goto capture_error;
        if (ai_camera_prepare() < 0)
            goto capture_error;

        request.buffer = s_photo_buffer;
        request.buffer_size = sizeof(s_photo_buffer);
        request.frame_size = 0U;
        status = camera_capture_single(s_camera, &request);
        if (status != CAMERA_OK || request.frame_size == 0U)
        {
            rt_kprintf("camera: capture failed status=%d\n", status);
            goto capture_error;
        }
        if (mkdir(AI_CAMERA_PHOTO_DIR, 0) != 0 && errno != EEXIST)
        {
            rt_kprintf("camera: cannot create %s\n", AI_CAMERA_PHOTO_DIR);
            goto capture_error;
        }
        if (ai_camera_save_file(AI_CAMERA_PHOTO_PATH, s_photo_buffer,
                                request.frame_size) < 0)
        {
            rt_kprintf("camera: save %s failed\n", AI_CAMERA_PHOTO_PATH);
            goto capture_error;
        }
        s_photo_index = (s_photo_index % 999U) + 1U;
        rt_snprintf(numbered_path, sizeof(numbered_path),
                    AI_CAMERA_PHOTO_DIR "/photo_%03u.jpg", s_photo_index);
        (void)ai_camera_save_file(numbered_path, s_photo_buffer,
                                  request.frame_size);
        rt_kprintf("camera: saved %s (%u bytes)\n", AI_CAMERA_PHOTO_PATH,
                   (unsigned int)request.frame_size);
        if (ai_xiaozhi_vision_upload_file(
                AI_CAMERA_PHOTO_PATH, "Please describe this image.", response,
                sizeof(response)) == 0)
            rt_kprintf("camera: XiaoZhi vision response %s\n", response);
        else
            rt_kprintf("camera: XiaoZhi vision upload unavailable\n");
        device_ui_post(DEVICE_UI_EVENT_PHOTO_SAVED);
        goto capture_done;

capture_error:
        device_ui_post(DEVICE_UI_EVENT_PHOTO_ERROR);
capture_done:
        if (rt_mutex_take(s_capture_lock, RT_WAITING_FOREVER) == RT_EOK)
        {
            s_capture_pending = RT_FALSE;
            rt_mutex_release(s_capture_lock);
        }
    }
}

int ai_camera_capture_init(void)
{
    if (s_capture_initialized)
        return 0;
    s_capture_sem = rt_sem_create("cam_cap", 0, RT_IPC_FLAG_FIFO);
    s_capture_lock = rt_mutex_create("cam_cap", RT_IPC_FLAG_PRIO);
    if (s_capture_sem == RT_NULL || s_capture_lock == RT_NULL)
        return -RT_ENOMEM;
    s_capture_thread = rt_thread_create("cam_cap", ai_camera_capture_worker,
                                        RT_NULL, AI_CAMERA_CAPTURE_STACK_SIZE,
                                        AI_CAMERA_CAPTURE_PRIORITY, 10U);
    if (s_capture_thread == RT_NULL)
        return -RT_ENOMEM;
    rt_thread_startup(s_capture_thread);
    s_capture_initialized = RT_TRUE;
    return 0;
}

int ai_camera_capture_start(void)
{
    if (!s_capture_initialized && ai_camera_capture_init() < 0)
        return -RT_ENOMEM;
    if (rt_mutex_take(s_capture_lock, RT_WAITING_FOREVER) != RT_EOK)
        return -RT_ERROR;
    if (s_capture_pending)
    {
        rt_mutex_release(s_capture_lock);
        return -RT_EBUSY;
    }
    s_capture_pending = RT_TRUE;
    rt_mutex_release(s_capture_lock);
    rt_sem_release(s_capture_sem);
    return 0;
}

static void ai_camera_capture_command(void)
{
    int result = ai_camera_capture_start();

    if (result == -RT_EBUSY)
        rt_kprintf("camera: capture already in progress\n");
    else if (result < 0)
        rt_kprintf("camera: capture request failed %d\n", result);
    else
        rt_kprintf("camera: capture queued\n");
}
MSH_CMD_EXPORT_ALIAS(ai_camera_capture_command, ai_camera_capture,
                     Capture Arducam OV2640 JPEG to /photo/latest.jpg);
