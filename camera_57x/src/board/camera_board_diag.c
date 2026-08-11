#include "camera_board_diag.h"

#include <dfs_fs.h>
#include <dfs_posix.h>
#include <bf0_hal.h>
#include <drv_touch.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>

#ifdef CAMERA_SENSOR_GC032A
#include "../camera/gc032a_backend.h"
#include "../camera/gc032a_regs.h"
#endif
#include "../app/camera_app.h"
#include "touch_diag.h"

#define CAMERA_LCD_WIDTH 390
#define CAMERA_LCD_HEIGHT 450
#define CAMERA_ADC_CHANNEL 6
#define CAMERA_SD_MOUNT_POINT "/"
#define CAMERA_SD_PROBE_PATH "/.camera_57x_probe"
#define CAMERA_SD_STRESS_PATH "/.camera_57x_sd_stress"
#define CAMERA_SD_STRESS_BLOCK_SIZE 4096U
#define CAMERA_SD_STRESS_BLOCK_COUNT 256U

#ifdef CAMERA_SENSOR_GC032A
static rt_bool_t diag_gc032a_initialized;
static uint16_t diag_gc032a_ids[2];
static gc032a_serial_status_t diag_gc032a_status;
#endif

static rt_bool_t sd_mounted;

static int diag_touch_read(void *context, touch_diag_event_t *event)
{
    struct touch_message message;

    if (rt_device_read((rt_device_t)context, 0, &message, 1) != 1)
        return 0;
    event->x = message.x;
    event->y = message.y;
    event->event = message.event;
    return 1;
}

static void diag_touch_delay(void *context, uint32_t delay_ms)
{
    (void)context;
    rt_thread_mdelay(delay_ms);
}

static int camera_board_diag_touch(void)
{
    rt_device_t touch = rt_device_find("touch");
    touch_diag_event_t event;
    rt_err_t close_result;
    int result;
    const char *event_name;

    if (touch == RT_NULL)
    {
        rt_kprintf("diag display: touch device not found\n");
        return -1;
    }
    result = rt_device_open(touch, RT_DEVICE_FLAG_RDONLY);
    if (result != RT_EOK)
    {
        rt_kprintf("diag display: touch open failed, result=%d\n", result);
        return -1;
    }

    while (diag_touch_read(touch, &event) > 0)
    {
    }
    rt_kprintf("diag display: touch screen within 5 s\n");
    result = touch_diag_wait(diag_touch_read, diag_touch_delay, touch,
                             5000U, 20U, &event);

    close_result = rt_device_close(touch);
    if (close_result != RT_EOK)
    {
        rt_kprintf("diag display: touch close failed, result=%d\n",
                   close_result);
        return -1;
    }
    if (result == TOUCH_DIAG_TIMEOUT)
    {
        rt_kprintf("diag display: touch timeout\n");
        return -1;
    }
    if (result != TOUCH_DIAG_OK)
    {
        rt_kprintf("diag display: touch read failed, result=%d\n", result);
        return -1;
    }

    if (event.event == TOUCH_EVENT_DOWN)
        event_name = "down";
    else if (event.event == TOUCH_EVENT_UP)
        event_name = "up";
    else
        event_name = "other";
    rt_kprintf("diag display: touch %s x=%u y=%u, i2c1 passed\n",
               event_name, event.x, event.y);
    return 0;
}

int camera_board_diag_display(void)
{
    static const rt_uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    rt_device_t lcd;
    struct rt_device_graphic_info info;
    struct rt_device_graphic_ops *ops;
    rt_uint16_t *band;
    rt_uint16_t format = RTGRAPHIC_PIXEL_FORMAT_RGB565;
    rt_uint8_t brightness = 100;
    int band_height = CAMERA_LCD_HEIGHT / 4;
    int band_index;
    int x;
    int y;

    if (camera_app_is_running())
    {
        rt_kprintf("diag display: busy: camera app running\n");
        return -1;
    }

    lcd = rt_device_find("lcd");
    if ((lcd == RT_NULL) || (rt_device_open(lcd, RT_DEVICE_OFLAG_RDWR) != RT_EOK))
        return -1;
    if (rt_device_control(lcd, RTGRAPHIC_CTRL_GET_INFO, &info) != RT_EOK ||
        info.width != CAMERA_LCD_WIDTH || info.height != CAMERA_LCD_HEIGHT)
        return -1;
    if (rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BUF_FORMAT, &format) != RT_EOK)
        return -1;
    if (rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &brightness) != RT_EOK)
        return -1;

    ops = rt_graphix_ops(lcd);
    band = rt_malloc(CAMERA_LCD_WIDTH * band_height * sizeof(*band));
    if ((ops == RT_NULL) || (ops->set_window == RT_NULL) || (ops->draw_rect == RT_NULL) ||
        (band == RT_NULL))
        return -1;

    for (band_index = 0; band_index < 4; band_index++)
    {
        for (y = 0; y < band_height; y++)
            for (x = 0; x < CAMERA_LCD_WIDTH; x++)
                band[y * CAMERA_LCD_WIDTH + x] = colors[band_index];
        mpu_dcache_clean(band, CAMERA_LCD_WIDTH * band_height * sizeof(*band));
        ops->set_window(0, band_index * band_height, CAMERA_LCD_WIDTH - 1,
                        (band_index + 1) * band_height - 1);
        ops->draw_rect((const char *)band, 0, band_index * band_height,
                       CAMERA_LCD_WIDTH - 1, (band_index + 1) * band_height - 1);
        rt_thread_mdelay(5);
    }
    rt_free(band);
    rt_kprintf("diag display: %dx%d color bars sent\n", info.width, info.height);
    return camera_board_diag_touch();
}

int camera_board_diag_adc(void)
{
    rt_adc_device_t adc = (rt_adc_device_t)rt_device_find("bat1");
    rt_uint32_t min = 0xffffffffU;
    rt_uint32_t max = 0;
    rt_uint32_t sum = 0;
    int i;

    if (adc == RT_NULL)
    {
        rt_kprintf("diag adc: device bat1 not found\n");
        return -1;
    }
    if (rt_adc_enable(adc, CAMERA_ADC_CHANNEL) != RT_EOK)
    {
        rt_kprintf("diag adc: enable bat1 ch%d failed\n", CAMERA_ADC_CHANNEL);
        return -1;
    }
    for (i = 0; i < 10; i++)
    {
        rt_uint32_t value = rt_adc_read(adc, CAMERA_ADC_CHANNEL);
        if (value < min) min = value;
        if (value > max) max = value;
        sum += value;
    }
    rt_adc_disable(adc, CAMERA_ADC_CHANNEL);
    rt_kprintf("diag adc: ch%d min=%u.%u avg=%u.%u max=%u.%u mV\n",
               CAMERA_ADC_CHANNEL, min / 10, min % 10, (sum / 10) / 10,
               (sum / 10) % 10, max / 10, max % 10);
    return 0;
}

int camera_board_diag_sd(void)
{
    static const char payload[] = "camera_57x sd probe\n";
    char readback[sizeof(payload)];
    rt_device_t sd_device;
    const char *mounted_path;
    int fd;
    int i;

    for (i = 0; (i < 30) && (rt_device_find("sd1") == RT_NULL); i++)
        rt_thread_mdelay(100);
    sd_device = rt_device_find("sd1");
    if (sd_device == RT_NULL)
    {
        rt_kprintf("diag sd: device sd1 not found after 3 s\n");
        return -1;
    }

    if (!sd_mounted)
    {
        mounted_path = dfs_filesystem_get_mounted_path(sd_device);
        if ((mounted_path != RT_NULL) &&
            (strcmp(mounted_path, CAMERA_SD_MOUNT_POINT) == 0))
        {
            sd_mounted = RT_TRUE;
        }
        else if (dfs_mount("sd1", CAMERA_SD_MOUNT_POINT, "elm", 0, 0) != 0)
        {
            rt_kprintf("diag sd: mount sd1 at / failed, errno=%d\n", rt_get_errno());
            return -1;
        }
        sd_mounted = RT_TRUE;
    }

    fd = open(CAMERA_SD_PROBE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
    {
        rt_kprintf("diag sd: open write failed, errno=%d\n", rt_get_errno());
        return -1;
    }
    if (write(fd, payload, sizeof(payload) - 1) != sizeof(payload) - 1)
    {
        rt_kprintf("diag sd: write failed, errno=%d\n", rt_get_errno());
        close(fd);
        return -1;
    }
    if (close(fd) != 0)
    {
        rt_kprintf("diag sd: close write failed, errno=%d\n", rt_get_errno());
        return -1;
    }
    fd = open(CAMERA_SD_PROBE_PATH, O_RDONLY, 0);
    if (fd < 0)
    {
        rt_kprintf("diag sd: open read failed, errno=%d\n", rt_get_errno());
        return -1;
    }
    if (read(fd, readback, sizeof(payload) - 1) != sizeof(payload) - 1)
    {
        rt_kprintf("diag sd: read failed, errno=%d\n", rt_get_errno());
        close(fd);
        return -1;
    }
    if (close(fd) != 0)
    {
        rt_kprintf("diag sd: close read failed, errno=%d\n", rt_get_errno());
        return -1;
    }
    if (memcmp(payload, readback, sizeof(payload) - 1) != 0)
    {
        rt_kprintf("diag sd: payload mismatch\n");
        return -1;
    }
    if (unlink(CAMERA_SD_PROBE_PATH) != 0)
    {
        rt_kprintf("diag sd: remove probe failed, errno=%d\n", rt_get_errno());
        return -1;
    }

    rt_kprintf("diag sd: sd1 probe passed\n");
    return 0;
}

static void diag_sd_fill(rt_uint8_t *buffer, rt_uint32_t block)
{
    rt_uint32_t i;

    for (i = 0; i < CAMERA_SD_STRESS_BLOCK_SIZE; i++)
        buffer[i] = (rt_uint8_t)(block + i);
}

static int diag_sd_stress(void)
{
    rt_uint8_t *write_buffer;
    rt_uint8_t *read_buffer;
    rt_uint32_t block;
    int fd;
    int rc = -1;

    if (camera_app_is_saving())
    {
        rt_kprintf("diag sd_stress: busy: photo save in progress\n");
        return -1;
    }

    if (camera_board_diag_sd() != 0)
        return -1;

    write_buffer = rt_malloc(CAMERA_SD_STRESS_BLOCK_SIZE);
    read_buffer = rt_malloc(CAMERA_SD_STRESS_BLOCK_SIZE);
    if ((write_buffer == RT_NULL) || (read_buffer == RT_NULL))
        goto out;

    fd = open(CAMERA_SD_STRESS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
        goto out;
    for (block = 0; block < CAMERA_SD_STRESS_BLOCK_COUNT; block++)
    {
        diag_sd_fill(write_buffer, block);
        if (write(fd, write_buffer, CAMERA_SD_STRESS_BLOCK_SIZE) != CAMERA_SD_STRESS_BLOCK_SIZE)
        {
            close(fd);
            goto out;
        }
    }
    if (close(fd) != 0)
        goto out;

    fd = open(CAMERA_SD_STRESS_PATH, O_RDONLY, 0);
    if (fd < 0)
        goto out;
    for (block = 0; block < CAMERA_SD_STRESS_BLOCK_COUNT; block++)
    {
        diag_sd_fill(write_buffer, block);
        if ((read(fd, read_buffer, CAMERA_SD_STRESS_BLOCK_SIZE) != CAMERA_SD_STRESS_BLOCK_SIZE) ||
            (memcmp(write_buffer, read_buffer, CAMERA_SD_STRESS_BLOCK_SIZE) != 0))
        {
            close(fd);
            goto out;
        }
    }
    if (close(fd) != 0)
        goto out;

    rt_kprintf("diag sd_stress: %u KiB write/read/verify passed\n",
               (CAMERA_SD_STRESS_BLOCK_SIZE * CAMERA_SD_STRESS_BLOCK_COUNT) / 1024U);
    rc = 0;
out:
    unlink(CAMERA_SD_STRESS_PATH);
    if (write_buffer != RT_NULL) rt_free(write_buffer);
    if (read_buffer != RT_NULL) rt_free(read_buffer);
    if (rc != 0) rt_kprintf("diag sd_stress: failed, errno=%d\n", rt_get_errno());
    return rc;
}

static int diag(int argc, char *argv[])
{
    int rc = 0;

    if (argc != 2)
    {
        rt_kprintf("usage: diag display|adc|sd|sd_stress|camera|all\n");
        return -1;
    }
    if ((strcmp(argv[1], "display") == 0) || (strcmp(argv[1], "all") == 0))
        rc |= camera_board_diag_display();
    if ((strcmp(argv[1], "adc") == 0) || (strcmp(argv[1], "all") == 0))
        rc |= camera_board_diag_adc();
    if ((strcmp(argv[1], "sd") == 0) || (strcmp(argv[1], "all") == 0))
        rc |= camera_board_diag_sd();
    if (strcmp(argv[1], "sd_stress") == 0)
        rc |= diag_sd_stress();
    if (strcmp(argv[1], "camera") == 0)
    {
        if (camera_app_is_running())
        {
            rt_kprintf("diag camera: busy: camera app running\n");
            return -1;
        }
#ifdef CAMERA_SENSOR_GC032A
        uint8_t failed_reg = 0;

        if (!diag_gc032a_initialized)
        {
            rc = gc032a_backend_init(diag_gc032a_ids, &diag_gc032a_status,
                                     &failed_reg);
            if (rc == 0)
                diag_gc032a_initialized = RT_TRUE;
        }
        else
        {
            rt_kprintf("diag camera: stage=reuse\n");
            rc = 0;
        }
        rt_kprintf("diag camera: init=%d id1=0x%04x id2=0x%04x failed_reg=0x%02x\n",
                   rc, diag_gc032a_ids[0], diag_gc032a_ids[1], failed_reg);
        if (rc != 0)
            return rc;

        rt_kprintf("diag camera: fmt=0x%02x serial=%02x/%02x/%02x/%02x markers=%02x/%02x/%02x/%02x\n",
                   diag_gc032a_status.output_format,
                   diag_gc032a_status.serial_enable,
                   diag_gc032a_status.serial_mode,
                   diag_gc032a_status.serial_clock,
                   diag_gc032a_status.serial_control,
                   diag_gc032a_status.frame_start,
                   diag_gc032a_status.line_start,
                   diag_gc032a_status.line_end,
                   diag_gc032a_status.frame_end);
#endif
    }
    return rc;
}
MSH_CMD_EXPORT_ALIAS(diag, diag, camera board diagnostics)
