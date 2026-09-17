#include "camera_capture.h"

#include <dfs_posix.h>
#include <dfs_fs.h>
#include <drv_flash.h>
#include <errno.h>
#include <fcntl.h>
#include <mem_section.h>
#include <rtthread.h>
#include <mem_map.h>
#include <string.h>

#include "camera_sw_jpeg_encoder.h"
#include "device_ui.h"
#include "gc032a_camera.h"
#include "gc032a_preview.h"
#include "xiaozhi_vision.h"

#define AI_CAMERA_PHOTO_DIR "/photo"
#define AI_CAMERA_PHOTO_PATH "/photo/latest.jpg"
#define AI_CAMERA_FILESYSTEM_DEVICE "camfs"
#define AI_CAMERA_PHOTO_BUFFER_SIZE (512U * 1024U)
#define AI_CAMERA_RAW_WIDTH GC032A_FRAME_WIDTH
#define AI_CAMERA_RAW_HEIGHT GC032A_FRAME_HEIGHT
#define AI_CAMERA_RAW_BUFFER_SIZE GC032A_FRAME_SIZE
#define AI_CAMERA_CAPTURE_STACK_SIZE 8192U
#define AI_CAMERA_CAPTURE_PRIORITY 18U
/* The preview hands over its next complete frame, and one frame currently costs
 * ~2.4 s of capture plus ~0.4 s of render, so the wait has to cover a period. */
#define AI_CAMERA_FRAME_TIMEOUT_MS 5000U

static rt_sem_t s_capture_sem;
static rt_mutex_t s_capture_lock;
static rt_thread_t s_capture_thread;
static rt_bool_t s_capture_pending;
static rt_bool_t s_capture_initialized;
static rt_bool_t s_filesystem_registered;
static rt_bool_t s_filesystem_ready;
static volatile rt_bool_t s_format_running;
static unsigned int s_photo_index;

static uint8_t s_photo_buffer[AI_CAMERA_PHOTO_BUFFER_SIZE]
    L2_RET_BSS_SECT(s_photo_buffer);
static uint8_t s_raw_buffer[AI_CAMERA_RAW_BUFFER_SIZE]
    L2_RET_BSS_SECT(s_raw_buffer);
static uint8_t s_jpeg_workspace[CAMERA_SW_JPEG_ENCODER_STATIC_WORKSPACE_SIZE]
    __attribute__((aligned(8)));

static int ai_camera_save_file(const char *path, const void *buffer,
                               rt_size_t size)
{
    int fd;
    rt_size_t written = 0U;

    rt_kprintf("camera: writing %s (%u bytes)\n", path, (unsigned int)size);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
    {
        rt_kprintf("camera: open %s failed (errno %d)\n", path, errno);
        if (errno == -EIO)
            rt_kprintf("camera: volume I/O error, \"ai_camera_format "
                       "confirm\" rebuilds it\n");
        return -1;
    }
    rt_kprintf("camera: %s opened\n", path);
    while (written < size)
    {
        int result = write(fd, (const uint8_t *)buffer + written,
                           size - written);

        if (result <= 0)
        {
            rt_kprintf("camera: write %s failed at %u/%u (errno %d)\n", path,
                       (unsigned int)written, (unsigned int)size, errno);
            close(fd);
            return -1;
        }
        written += (rt_size_t)result;
    }
    if (fsync(fd) != 0)
    {
        rt_kprintf("camera: fsync %s failed (errno %d)\n", path, errno);
        close(fd);
        return -1;
    }
    if (close(fd) != 0)
    {
        rt_kprintf("camera: close %s failed (errno %d)\n", path, errno);
        return -1;
    }
    return 0;
}

/* DFS hands the ELM result to rt_set_errno() without negating it, so an
 * already existing directory shows up as errno == -EEXIST (not +EEXIST) and the
 * usual errno check cannot be used.  Confirm the path instead. */
static int ai_camera_ensure_photo_dir(void)
{
    struct stat info;

    if (stat(AI_CAMERA_PHOTO_DIR, &info) == 0 &&
        (info.st_mode & S_IFDIR) != 0)
        return 0;
    rt_kprintf("camera: creating %s\n", AI_CAMERA_PHOTO_DIR);
    if (mkdir(AI_CAMERA_PHOTO_DIR, 0) == 0)
        return 0;
    if (stat(AI_CAMERA_PHOTO_DIR, &info) == 0 &&
        (info.st_mode & S_IFDIR) != 0)
        return 0;
    rt_kprintf("camera: cannot create %s (errno %d)\n", AI_CAMERA_PHOTO_DIR,
               errno);
    return -1;
}

static int ai_camera_filesystem_init(void)
{
    rt_device_t fs_device;
    struct rt_device_blk_geometry geometry = {0};
    const char *mounted_path;
    int mount_result;
    int mkfs_result;

    if (s_filesystem_ready)
        return 0;
    if (!s_filesystem_registered)
    {
        register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE,
                            AI_CAMERA_FILESYSTEM_DEVICE);
        s_filesystem_registered = RT_TRUE;
    }
    fs_device = rt_device_find(AI_CAMERA_FILESYSTEM_DEVICE);
    if (fs_device == RT_NULL)
    {
        rt_kprintf("camera: filesystem device %s unavailable\n",
                   AI_CAMERA_FILESYSTEM_DEVICE);
        return -1;
    }
    if (rt_device_control(fs_device, RT_DEVICE_CTRL_BLK_GETGEOME,
                          &geometry) == RT_EOK)
    {
        rt_kprintf("camera: fs geometry sector=%u block=%u count=%u\n",
                   (unsigned int)geometry.bytes_per_sector,
                   (unsigned int)geometry.block_size,
                   (unsigned int)geometry.sector_count);
    }
    else
    {
        rt_kprintf("camera: fs geometry unavailable\n");
    }
    mounted_path = dfs_filesystem_get_mounted_path(fs_device);
    if (mounted_path == RT_NULL)
    {
        mount_result = dfs_mount(AI_CAMERA_FILESYSTEM_DEVICE, "/", "elm", 0,
                                 RT_NULL);
        rt_kprintf("camera: fs mount result=%d\n", mount_result);
        if (mount_result != 0)
        {
            mkfs_result = dfs_mkfs("elm", AI_CAMERA_FILESYSTEM_DEVICE);
            rt_kprintf("camera: fs format result=%d\n", mkfs_result);
            if (mkfs_result != 0)
                return -1;
            mount_result = dfs_mount(AI_CAMERA_FILESYSTEM_DEVICE, "/", "elm",
                                     0, RT_NULL);
            rt_kprintf("camera: fs mount after format result=%d\n",
                       mount_result);
            if (mount_result != 0)
                return -1;
        }
    }
    s_filesystem_ready = RT_TRUE;
    rt_kprintf("camera: fs_root mounted at /\n");
    return 0;
}

static int ai_camera_prepare(void)
{
    int status = gc032a_camera_init();

    if (status != GC032A_CAMERA_OK)
    {
        rt_kprintf("camera: init failed %d\n", status);
        return -1;
    }
    rt_kprintf("camera: GC032A 2-line serial 640x480 RGB565\n");
    return 0;
}

/* raw points at one 640x480 RGB565 frame: either the one-shot capture buffer or
 * the frame frozen by the live preview. */
static int ai_camera_encode_jpeg(uint8_t *raw, rt_size_t raw_size,
                                 uint32_t *jpeg_size)
{
    camera_sw_jpeg_encoder_t encoder;
    camera_sw_jpeg_frame_t frame = {
        .data = raw,
        .width = AI_CAMERA_RAW_WIDTH,
        .height = AI_CAMERA_RAW_HEIGHT,
        .stride = AI_CAMERA_RAW_WIDTH * 2U,
        .pixel_format = CAMERA_SW_JPEG_PIXEL_RGB565_BE,
    };

    if (raw == RT_NULL || raw_size != sizeof(s_raw_buffer) ||
        jpeg_size == RT_NULL)
        return -1;
    if (camera_sw_jpeg_encoder_init(&encoder, s_jpeg_workspace,
                                    sizeof(s_jpeg_workspace)) != 0)
        return -1;
    return camera_sw_jpeg_encode_to_buffer(
        &encoder, &frame, CAMERA_SW_JPEG_QUALITY_HIGH, s_photo_buffer,
        sizeof(s_photo_buffer), jpeg_size);
}

static void ai_camera_capture_worker(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        int status;
        size_t captured;
        uint8_t *raw_frame = RT_NULL;
        uint32_t raw_size;
        uint32_t jpeg_size;
        char numbered_path[32];
        char response[AI_XIAOZHI_VISION_RESPONSE_MAX];

        rt_sem_take(s_capture_sem, RT_WAITING_FOREVER);
        if (s_format_running)
        {
            rt_kprintf("camera: format in progress, try again\n");
            goto capture_error;
        }
        if (ai_camera_filesystem_init() < 0)
            goto capture_error;
        if (gc032a_preview_is_running())
        {
            /* The preview thread hands over its next complete frame, so the
             * JPEG is the picture on screen and the DCMI is never driven
             * behind the preview thread's back. */
            status = gc032a_preview_deliver_frame(s_raw_buffer,
                                                  sizeof(s_raw_buffer),
                                                  AI_CAMERA_FRAME_TIMEOUT_MS);
            if (status != GC032A_CAMERA_OK)
            {
                rt_kprintf("camera: preview frame handover failed (%d)\n",
                           status);
                goto capture_error;
            }
            raw_frame = s_raw_buffer;
            raw_size = GC032A_FRAME_SIZE;
        }
        else
        {
            if (ai_camera_prepare() < 0)
                goto capture_error;
            captured = 0U;
            status = gc032a_camera_capture_one(s_raw_buffer,
                                               sizeof(s_raw_buffer),
                                               &captured);
            if (status != GC032A_CAMERA_OK || captured == 0U)
            {
                rt_kprintf("camera: capture failed status=%d\n", status);
                (void)gc032a_camera_recover();
                goto capture_error;
            }
            raw_frame = s_raw_buffer;
            raw_size = (uint32_t)captured;
        }

        {
            if (ai_camera_encode_jpeg(raw_frame, raw_size, &jpeg_size) != 0)
            {
                rt_kprintf("camera: RGB565 to JPEG encode failed\n");
                goto capture_error;
            }
        }
        if (ai_camera_ensure_photo_dir() < 0)
            goto capture_error;
        if (ai_camera_save_file(AI_CAMERA_PHOTO_PATH, s_photo_buffer,
                                jpeg_size) < 0)
        {
            rt_kprintf("camera: save %s failed\n", AI_CAMERA_PHOTO_PATH);
            goto capture_error;
        }
        s_photo_index = (s_photo_index % 999U) + 1U;
        rt_snprintf(numbered_path, sizeof(numbered_path),
                    AI_CAMERA_PHOTO_DIR "/photo_%03u.jpg", s_photo_index);
        (void)ai_camera_save_file(numbered_path, s_photo_buffer,
                                  jpeg_size);
        rt_kprintf("camera: saved %s (%u bytes)\n", AI_CAMERA_PHOTO_PATH,
                   (unsigned int)jpeg_size);
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
                     Capture GC032A DVP frame to /photo/latest.jpg);

/* Rebuilding the volume drops every photo, so it takes an explicit argument.
 * The work happens on its own thread with a log line per step: erasing and
 * rewriting the volume can take seconds and must neither block the shell nor
 * hide where it stopped. */
static void ai_camera_format_thread(void *parameter)
{
    rt_device_t fs_device;
    int result;

    (void)parameter;
    if (!s_filesystem_registered)
    {
        rt_kprintf("camera: format: registering %s\n",
                   AI_CAMERA_FILESYSTEM_DEVICE);
        register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE,
                            AI_CAMERA_FILESYSTEM_DEVICE);
        s_filesystem_registered = RT_TRUE;
    }
    fs_device = rt_device_find(AI_CAMERA_FILESYSTEM_DEVICE);
    if (fs_device == RT_NULL)
    {
        rt_kprintf("camera: format: %s not found\n",
                   AI_CAMERA_FILESYSTEM_DEVICE);
        s_format_running = RT_FALSE;
        return;
    }
    if (dfs_filesystem_get_mounted_path(fs_device) != RT_NULL)
    {
        rt_kprintf("camera: format: unmounting /\n");
        result = dfs_unmount("/");
        rt_kprintf("camera: format: unmount result=%d\n", result);
    }
    rt_kprintf("camera: format: mkfs start\n");
    result = dfs_mkfs("elm", AI_CAMERA_FILESYSTEM_DEVICE);
    rt_kprintf("camera: format: mkfs result=%d, mount on next capture\n",
               result);
    s_filesystem_ready = RT_FALSE;
    s_format_running = RT_FALSE;
}

static void ai_camera_format_command(int argc, char **argv)
{
    rt_thread_t thread;

    if (argc < 2 || strcmp(argv[1], "confirm") != 0)
    {
        rt_kprintf("camera: this erases every photo; run "
                   "\"ai_camera_format confirm\" to do it\n");
        return;
    }
    if (s_format_running)
    {
        rt_kprintf("camera: format already running\n");
        return;
    }
    if (s_capture_pending)
    {
        rt_kprintf("camera: capture in progress, try again\n");
        return;
    }
    thread = rt_thread_create("cam_fmt", ai_camera_format_thread, RT_NULL,
                              2048U, AI_CAMERA_CAPTURE_PRIORITY, 10U);
    if (thread == RT_NULL)
    {
        rt_kprintf("camera: cannot start the format thread\n");
        return;
    }
    s_format_running = RT_TRUE;
    rt_thread_startup(thread);
    rt_kprintf("camera: format started, watch the log\n");
}
MSH_CMD_EXPORT_ALIAS(ai_camera_format_command, ai_camera_format,
                     Rebuild the photo volume);
