#include "gc032a_backend.h"

#include <stddef.h>

#include "gc032a_regs.h"
#include "gc032a_serial_settings.h"

int gc032a_probe_twice(gc032a_reg_read_fn_t read_reg, void *context,
                       uint16_t ids[2])
{
    uint8_t high;
    uint8_t low;
    int index;

    if ((read_reg == NULL) || (ids == NULL))
        return GC032A_PROBE_BUS_ERROR;

    for (index = 0; index < 2; index++)
    {
        if ((read_reg(context, GC032A_REG_CHIP_ID_HIGH, &high) != 0) ||
            (read_reg(context, GC032A_REG_CHIP_ID_LOW, &low) != 0))
            return GC032A_PROBE_BUS_ERROR;

        ids[index] = (uint16_t)(((uint16_t)high << 8) | low);
        if (ids[index] != GC032A_CHIP_ID)
            return GC032A_PROBE_ID_MISMATCH;
    }

    return GC032A_PROBE_OK;
}

int gc032a_identify_and_reset(gc032a_reg_read_fn_t read_reg,
                              gc032a_reg_write_fn_t write_reg,
                              void *context, uint16_t ids[2],
                              uint8_t *failed_reg)
{
    int result;

    if ((write_reg == NULL) || (failed_reg == NULL))
        return GC032A_PROBE_BUS_ERROR;
    *failed_reg = 0;

    result = gc032a_probe_twice(read_reg, context, ids);
    if (result != GC032A_PROBE_OK)
        return result;

    if (write_reg(context, GC032A_REG_PAGE_SELECT,
                  GC032A_SOFTWARE_RESET) != 0)
    {
        *failed_reg = GC032A_REG_PAGE_SELECT;
        return GC032A_PROBE_BUS_ERROR;
    }
    return GC032A_PROBE_OK;
}

int gc032a_apply_serial_settings(gc032a_reg_write_fn_t write_reg,
                                 void *context, uint8_t *failed_reg)
{
    size_t index;

    if ((write_reg == NULL) || (failed_reg == NULL))
        return -1;

    for (index = 0;
         index < sizeof(gc032a_serial_default_regs) /
                     sizeof(gc032a_serial_default_regs[0]);
         index++)
    {
        if (write_reg(context, gc032a_serial_default_regs[index][0],
                      gc032a_serial_default_regs[index][1]) != 0)
        {
            *failed_reg = gc032a_serial_default_regs[index][0];
            return -1;
        }
    }

    if (write_reg(context, GC032A_REG_PAGE_SELECT, 0x00) != 0)
    {
        *failed_reg = GC032A_REG_PAGE_SELECT;
        return -1;
    }
    if (write_reg(context, GC032A_REG_OUTPUT_FORMAT, 0x86) != 0)
    {
        *failed_reg = GC032A_REG_OUTPUT_FORMAT;
        return -1;
    }
    return 0;
}

int gc032a_sccb_read_register(gc032a_sccb_send_fn_t send,
                              gc032a_sccb_recv_fn_t recv, void *context,
                              uint8_t reg, uint8_t *value)
{
    if (send(context, GC032A_I2C_ADDRESS, &reg, 1) != 1)
        return -1;
    return recv(context, GC032A_I2C_ADDRESS, value, 1) == 1 ? 0 : -1;
}

void gc032a_get_dcmi_profile(gc032a_dcmi_profile_t *profile)
{
    if (profile == NULL)
        return;

    profile->packet_size = 1280U;
    profile->packet_ids = 0x9d80b6caU;
    profile->sync_word = 0x00ffffffU;
    profile->data_lines = 2U;
    profile->sample_falling_edge = 1U;
}

uint32_t gc032a_mclk_period_ns(void)
{
    return 42U;
}

int gc032a_stream_frame_offset(uint32_t callback_size, uint32_t *offset)
{
    if (offset == NULL)
        return -1;
    if (callback_size == GC032A_FRAME_SIZE)
        *offset = 0U;
    else if (callback_size == 2U * GC032A_FRAME_SIZE)
        *offset = GC032A_FRAME_SIZE;
    else
        return -1;
    return 0;
}

#ifndef CAMERA_GC032A_HOST_TEST

#include <rtdevice.h>
#include <rtthread.h>
#include <drv_dcmi.h>
#include <ipc/completion.h>
#include <rthw.h>

#define GC032A_I2C_DEVICE_NAME "i2c1"
#define GC032A_MCLK_DEVICE_NAME "pwmt2"
#define GC032A_MCLK_CHANNEL 1
#define GC032A_STREAM_EVENT_FRAME (1U << 0)
#define GC032A_STREAM_EVENT_ERROR (1U << 1)

static struct rt_device_pwm *mclk_device;
static struct rt_completion capture_completion;
static rt_device_t capture_dcmi;
static volatile uint32_t capture_size;
static volatile int capture_error;
static struct rt_event stream_events;
static uint8_t *stream_buffer;
static volatile uint32_t stream_offset;
static volatile uint32_t stream_sequence;

static int gc032a_i2c_send(void *context, uint8_t address,
                           const uint8_t *data, size_t length)
{
    return rt_i2c_master_send((struct rt_i2c_bus_device *)context, address,
                              RT_I2C_WR, data, length);
}

static int gc032a_i2c_recv(void *context, uint8_t address, uint8_t *data,
                           size_t length)
{
    return rt_i2c_master_recv((struct rt_i2c_bus_device *)context, address,
                              RT_I2C_RD, data, length);
}

static int gc032a_mclk_start(void)
{
    rt_err_t result;
    uint32_t period_ns = gc032a_mclk_period_ns();

    mclk_device = (struct rt_device_pwm *)rt_device_find(
        GC032A_MCLK_DEVICE_NAME);
    if (mclk_device == RT_NULL)
        return -RT_ENOSYS;

    result = rt_pwm_set(mclk_device, GC032A_MCLK_CHANNEL,
                        period_ns, period_ns / 2U);
    if (result == RT_EOK)
        result = rt_pwm_enable(mclk_device, GC032A_MCLK_CHANNEL);
    return result;
}

static void gc032a_mclk_stop(void)
{
    if (mclk_device != RT_NULL)
        rt_pwm_disable(mclk_device, GC032A_MCLK_CHANNEL);
    mclk_device = RT_NULL;
}

static int gc032a_i2c_read(void *context, uint8_t reg, uint8_t *value)
{
    return gc032a_sccb_read_register(gc032a_i2c_send, gc032a_i2c_recv,
                                     context, reg, value);
}

static int gc032a_i2c_write(void *context, uint8_t reg, uint8_t value)
{
    struct rt_i2c_bus_device *bus = (struct rt_i2c_bus_device *)context;
    uint8_t data[2] = {reg, value};
    struct rt_i2c_msg message =
    {
        .addr = GC032A_I2C_ADDRESS,
        .flags = RT_I2C_WR,
        .buf = data,
        .len = sizeof(data),
    };

    return rt_i2c_transfer(bus, &message, 1) == 1 ? 0 : -1;
}

static int gc032a_read_page_reg(struct rt_i2c_bus_device *bus, uint8_t page,
                                uint8_t reg, uint8_t *value)
{
    if (gc032a_i2c_write(bus, GC032A_REG_PAGE_SELECT, page) != 0)
        return -1;
    return gc032a_i2c_read(bus, reg, value);
}

static int gc032a_read_serial_status(struct rt_i2c_bus_device *bus,
                                     gc032a_serial_status_t *status)
{
    if ((gc032a_read_page_reg(bus, 0, GC032A_REG_OUTPUT_FORMAT,
                              &status->output_format) != 0) ||
        (gc032a_read_page_reg(bus, 3, 0x51, &status->serial_enable) != 0) ||
        (gc032a_i2c_read(bus, 0x52, &status->serial_mode) != 0) ||
        (gc032a_i2c_read(bus, 0x53, &status->serial_clock) != 0) ||
        (gc032a_i2c_read(bus, 0x64, &status->serial_control) != 0) ||
        (gc032a_i2c_read(bus, 0x60, &status->frame_start) != 0) ||
        (gc032a_i2c_read(bus, 0x61, &status->line_start) != 0) ||
        (gc032a_i2c_read(bus, 0x62, &status->line_end) != 0) ||
        (gc032a_i2c_read(bus, 0x63, &status->frame_end) != 0))
        return -1;

    if ((status->output_format != 0x86) ||
        (status->serial_enable != 0x01) ||
        (status->serial_mode != 0x5a) ||
        (status->serial_clock != 0x64) ||
        (status->serial_control != 0x06) ||
        (status->frame_start != 0xca) ||
        (status->line_start != 0x80) ||
        (status->line_end != 0x9d) ||
        (status->frame_end != 0xb6))
        return -1;
    return gc032a_i2c_write(bus, GC032A_REG_PAGE_SELECT, 0x00);
}

int gc032a_backend_probe(uint16_t ids[2])
{
    struct rt_i2c_bus_device *bus;
    struct rt_i2c_configuration configuration =
    {
        .mode = 0,
        .addr = 0,
        .timeout = 500,
        .max_hz = 100000,
    };
    int result;

    result = gc032a_mclk_start();
    if (result != RT_EOK)
        return result;

    rt_thread_mdelay(10);
    bus = rt_i2c_bus_device_find(GC032A_I2C_DEVICE_NAME);
    if (bus == RT_NULL)
    {
        gc032a_mclk_stop();
        return -RT_ENOSYS;
    }
    if (rt_i2c_open(bus, RT_DEVICE_FLAG_RDWR) != RT_EOK)
    {
        gc032a_mclk_stop();
        return -RT_ERROR;
    }

    result = rt_i2c_configure(bus, &configuration);
    if (result == RT_EOK)
        result = gc032a_probe_twice(gc032a_i2c_read, bus, ids);
    rt_i2c_close(bus);

    if (result != GC032A_PROBE_OK)
        gc032a_mclk_stop();
    return result;
}

int gc032a_backend_init(uint16_t ids[2], gc032a_serial_status_t *status,
                        uint8_t *failed_reg)
{
    struct rt_i2c_bus_device *bus;
    struct rt_i2c_configuration configuration =
    {
        .mode = 0,
        .addr = 0,
        .timeout = 500,
        .max_hz = 100000,
    };
    int result;

    if ((ids == RT_NULL) || (status == RT_NULL) || (failed_reg == RT_NULL))
        return -RT_EINVAL;
    *failed_reg = 0;

    result = gc032a_mclk_start();
    if (result != RT_EOK)
        return result;
    rt_thread_mdelay(10);

    bus = rt_i2c_bus_device_find(GC032A_I2C_DEVICE_NAME);
    if ((bus == RT_NULL) ||
        (rt_i2c_open(bus, RT_DEVICE_FLAG_RDWR) != RT_EOK))
    {
        gc032a_mclk_stop();
        return -RT_ENOSYS;
    }

    result = rt_i2c_configure(bus, &configuration);
    if (result == RT_EOK)
    {
        rt_kprintf("diag camera: stage=id\n");
        result = gc032a_identify_and_reset(gc032a_i2c_read,
                                            gc032a_i2c_write, bus, ids,
                                            failed_reg);
        if (result != GC032A_PROBE_OK)
            rt_kprintf("diag camera: failed_stage=%s\n",
                       *failed_reg == GC032A_REG_PAGE_SELECT ?
                       "reset" : "id");
    }
    if (result == GC032A_PROBE_OK)
    {
        rt_kprintf("diag camera: stage=reset passed\n");
        rt_thread_mdelay(100);
        rt_kprintf("diag camera: stage=settings\n");
        result = gc032a_apply_serial_settings(gc032a_i2c_write, bus,
                                               failed_reg);
        if (result != GC032A_PROBE_OK)
            rt_kprintf("diag camera: failed_stage=settings reg=0x%02x\n",
                       *failed_reg);
    }
    if (result == GC032A_PROBE_OK)
    {
        rt_kprintf("diag camera: stage=readback\n");
        result = gc032a_read_serial_status(bus, status);
        if (result != GC032A_PROBE_OK)
            rt_kprintf("diag camera: failed_stage=readback\n");
    }

    rt_i2c_close(bus);
    if (result != GC032A_PROBE_OK)
        gc032a_mclk_stop();
    return result;
}

static rt_err_t gc032a_dcmi_rx(rt_device_t dev, rt_size_t size)
{
    if (size == GC032A_FRAME_SIZE)
    {
        capture_size = size;
        rt_completion_done(&capture_completion);
    }
    else if ((size == 0U) && (rt_hw_dcmi_get_error(dev) != 0))
    {
        capture_error = rt_hw_dcmi_get_error(dev);
        rt_completion_done(&capture_completion);
    }
    return RT_EOK;
}

static rt_err_t gc032a_dcmi_stream_rx(rt_device_t dev, rt_size_t size)
{
    uint32_t offset;

    if (gc032a_stream_frame_offset(size, &offset) == 0)
    {
        stream_offset = offset;
        stream_sequence++;
        rt_event_send(&stream_events, GC032A_STREAM_EVENT_FRAME);
    }
    else if ((size == 0U) && (rt_hw_dcmi_get_error(dev) != 0))
    {
        rt_event_send(&stream_events, GC032A_STREAM_EVENT_ERROR);
    }
    return RT_EOK;
}

int gc032a_backend_capture_one(uint8_t *dst, uint32_t size,
                               uint32_t timeout_ms, uint32_t *captured_size,
                               int *dcmi_error)
{
    gc032a_dcmi_profile_t profile;
    struct rt_dcmi_config config = {0};
    rt_err_t result;

    if ((dst == RT_NULL) || (size != GC032A_FRAME_SIZE) ||
        (captured_size == RT_NULL) || (dcmi_error == RT_NULL))
        return -RT_EINVAL;

    capture_dcmi = rt_device_find("dcmi");
    if (capture_dcmi == RT_NULL)
        return -RT_ENOSYS;
    result = rt_device_open(capture_dcmi, RT_DEVICE_FLAG_RDWR);
    if (result != RT_EOK)
        return result;

    gc032a_get_dcmi_profile(&profile);
    config.dcmi_cfg = DCMI_ALL_CFG;
    config.dcmi_if = DCMI_IF_SPI;
    config.dcmi_spi.pkt_size = profile.packet_size;
    config.dcmi_spi.pkt_id = profile.packet_ids;
    config.dcmi_spi.spi_dataline = DCMI_SPI_2LINE;
    config.dcmi_spi.spi_clkeg = profile.sample_falling_edge ?
                                DCMI_SPI_CLK_N : DCMI_SPI_CLK_P;
    config.dcmi_spi.spi_sync = profile.sync_word;
    config.dcmi_cm = DCMI_CPT_SIG;
    config.yuv2rgb_en = DCMI_YUV2RGB_DIS;
    config.frame_buffer = dst;
    config.buffer_size = size;

    capture_size = 0;
    capture_error = 0;
    rt_completion_init(&capture_completion);
    rt_device_set_rx_indicate(capture_dcmi, gc032a_dcmi_rx);
    result = rt_device_control(capture_dcmi, RT_DEVICE_CTRL_CONFIG, &config);
    if (result == RT_EOK)
        result = rt_device_control(capture_dcmi, RT_DEVICE_CTRL_START, RT_NULL);
    if (result == RT_EOK)
        result = rt_completion_wait(&capture_completion,
                                    rt_tick_from_millisecond(timeout_ms));

    rt_device_control(capture_dcmi, RT_DEVICE_CTRL_STOP, RT_NULL);
    rt_device_set_rx_indicate(capture_dcmi, RT_NULL);
    rt_device_close(capture_dcmi);
    capture_dcmi = RT_NULL;

    *captured_size = capture_size;
    *dcmi_error = capture_error;
    if (result != RT_EOK)
        return result;
    if (capture_error != 0)
        return -RT_ERROR;
    if (capture_size != size)
        return -RT_ERROR;

    mpu_dcache_invalidate(dst, size);
    return RT_EOK;
}

static int gc032a_open(void)
{
    uint16_t ids[2];
    gc032a_serial_status_t status;
    uint8_t failed_reg;

    int result = gc032a_backend_init(ids, &status, &failed_reg);

    if (result == 0)
        rt_event_init(&stream_events, "gc_stream", RT_IPC_FLAG_PRIO);
    return result;
}

static int gc032a_close(void)
{
    if (capture_dcmi != RT_NULL)
    {
        rt_device_set_rx_indicate(capture_dcmi, RT_NULL);
        rt_device_control(capture_dcmi, RT_DEVICE_CTRL_STOP, RT_NULL);
        rt_device_close(capture_dcmi);
        capture_dcmi = RT_NULL;
    }
    stream_buffer = RT_NULL;
    gc032a_mclk_stop();
    return 0;
}

static int gc032a_capture(uint8_t *dst, uint32_t size,
                          camera_source_frame_cb_t callback, void *context)
{
    uint32_t captured_size;
    int dcmi_error;
    int result = gc032a_backend_capture_one(dst, size, 2000,
                                            &captured_size, &dcmi_error);

    if (callback != RT_NULL)
        callback(context, result);
    return result;
}

static int gc032a_stream_start(uint8_t *buffer, uint32_t size)
{
    gc032a_dcmi_profile_t profile;
    struct rt_dcmi_config config = {0};
    rt_err_t result;

    if ((buffer == RT_NULL) || (size != 2U * GC032A_FRAME_SIZE) ||
        (capture_dcmi != RT_NULL))
        return -RT_EINVAL;

    capture_dcmi = rt_device_find("dcmi");
    if (capture_dcmi == RT_NULL)
        return -RT_ENOSYS;
    result = rt_device_open(capture_dcmi, RT_DEVICE_FLAG_RDWR);
    if (result != RT_EOK)
    {
        capture_dcmi = RT_NULL;
        return result;
    }

    gc032a_get_dcmi_profile(&profile);
    config.dcmi_cfg = DCMI_ALL_CFG;
    config.dcmi_if = DCMI_IF_SPI;
    config.dcmi_spi.pkt_size = profile.packet_size;
    config.dcmi_spi.pkt_id = profile.packet_ids;
    config.dcmi_spi.spi_dataline = DCMI_SPI_2LINE;
    config.dcmi_spi.spi_clkeg = profile.sample_falling_edge ?
                                DCMI_SPI_CLK_N : DCMI_SPI_CLK_P;
    config.dcmi_spi.spi_sync = profile.sync_word;
    config.dcmi_cm = DCMI_CPT_CON;
    config.yuv2rgb_en = DCMI_YUV2RGB_DIS;
    config.frame_buffer = buffer;
    config.buffer_size = size;

    stream_buffer = buffer;
    rt_event_control(&stream_events, RT_IPC_CMD_RESET, RT_NULL);
    rt_device_set_rx_indicate(capture_dcmi, gc032a_dcmi_stream_rx);
    result = rt_device_control(capture_dcmi, RT_DEVICE_CTRL_CONFIG, &config);
    if (result == RT_EOK)
        result = rt_device_control(capture_dcmi, RT_DEVICE_CTRL_START, RT_NULL);
    if (result == RT_EOK)
        return 0;

    if (capture_dcmi != RT_NULL)
    {
        rt_device_set_rx_indicate(capture_dcmi, RT_NULL);
        rt_device_close(capture_dcmi);
        capture_dcmi = RT_NULL;
    }
    stream_buffer = RT_NULL;
    return result;
}

static int gc032a_stream_wait(camera_stream_frame_t *frame,
                              uint32_t timeout_ms)
{
    rt_uint32_t events;
    rt_base_t level;
    uint32_t offset;
    uint32_t sequence;
    rt_err_t result;

    if ((frame == RT_NULL) || (capture_dcmi == RT_NULL) ||
        (stream_buffer == RT_NULL))
        return -RT_EINVAL;

    result = rt_event_recv(&stream_events,
                           GC032A_STREAM_EVENT_FRAME |
                           GC032A_STREAM_EVENT_ERROR,
                           RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                           rt_tick_from_millisecond(timeout_ms), &events);
    if (result != RT_EOK)
        return result;
    if ((events & GC032A_STREAM_EVENT_ERROR) != 0U)
        return -RT_ERROR;

    level = rt_hw_interrupt_disable();
    offset = stream_offset;
    sequence = stream_sequence;
    rt_hw_interrupt_enable(level);
    frame->data = stream_buffer + offset;
    frame->size = GC032A_FRAME_SIZE;
    frame->sequence = sequence;
    mpu_dcache_invalidate(frame->data, frame->size);
    return 0;
}

static int gc032a_stream_stop(void)
{
    if (capture_dcmi == RT_NULL)
        return 0;

    rt_device_set_rx_indicate(capture_dcmi, RT_NULL);
    rt_device_control(capture_dcmi, RT_DEVICE_CTRL_STOP, RT_NULL);
    rt_device_close(capture_dcmi);
    capture_dcmi = RT_NULL;
    stream_buffer = RT_NULL;
    return 0;
}

static const camera_source_ops_t gc032a_ops =
{
    .open = gc032a_open,
    .close = gc032a_close,
    .capture = gc032a_capture,
    .stream_start = gc032a_stream_start,
    .stream_wait = gc032a_stream_wait,
    .stream_stop = gc032a_stream_stop,
};

const camera_source_ops_t *gc032a_backend_ops(void)
{
    return &gc032a_ops;
}

#endif
