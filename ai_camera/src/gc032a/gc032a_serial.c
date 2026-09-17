#include "gc032a_serial.h"

#include <bf0_hal.h>
#include <drv_dcmi.h>
#include <drv_io.h>
#include <ipc/completion.h>
#include <mem_section.h>
#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>
#include <string.h>

#include "gc032a_registers.h"
#include "gc032a_serial_settings.h"

#define DBG_TAG "camera.gc032a"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define GC032A_SERIAL_I2C_NAME "i2c1"
#define GC032A_SERIAL_MCLK_DEVICE "pwmt2"
#define GC032A_SERIAL_MCLK_CHANNEL 1
#define GC032A_SERIAL_MCLK_PERIOD_NS 42U
#define GC032A_SERIAL_I2C_ADDR 0x21U
#define GC032A_SERIAL_I2C_MAX_HZ 100000U

static rt_device_t s_dcmi;
static struct rt_device_pwm *s_mclk;
static rt_bool_t s_open;
static rt_bool_t s_dcmi_started;
static struct rt_completion s_done;
static volatile uint32_t s_rx_size;
static volatile int s_rx_error;
static rt_bool_t s_trace_capture;
static struct rt_mutex s_lock;
static rt_bool_t s_lock_initialized;
static struct rt_i2c_bus_device *s_i2c;

#define GC032A_STREAM_EVENT_FRAME (1U << 0)
#define GC032A_STREAM_EVENT_ERROR (1U << 1)
#define GC032A_STREAM_BYTES (2U * GC032A_FRAME_SIZE)
#define GC032A_FRAME_WORDS (GC032A_FRAME_SIZE / sizeof(uint32_t))

static struct rt_event s_stream_events;
static rt_bool_t s_stream_events_initialized;
static uint8_t *s_stream_buffer;
static volatile rt_bool_t s_stream_active;
static volatile rt_bool_t s_stream_stopping;
static volatile rt_bool_t s_stream_has_frame;
static volatile uint32_t s_stream_offset;
static volatile uint32_t s_stream_sequence;
static volatile uint32_t s_stream_interval_ms;
static volatile int s_stream_error;
static rt_tick_t s_stream_last_tick;
static uint32_t s_stream_delivered_sequence;

static int gc032a_serial_stream_stop_locked(void);

/* Serialize device lifetime and transfers without allocating with IRQs masked. */
static int gc032a_serial_lock(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (!s_lock_initialized)
    {
        rt_mutex_init(&s_lock, "gc032a_dcmi", RT_IPC_FLAG_PRIO);
        s_lock_initialized = RT_TRUE;
    }
    rt_hw_interrupt_enable(level);
    return rt_mutex_take(&s_lock, RT_WAITING_FOREVER) == RT_EOK ? 0 : -1;
}

static void gc032a_serial_unlock(void)
{
    (void)rt_mutex_release(&s_lock);
}

static void gc032a_serial_close_i2c(void)
{
    if (s_i2c != RT_NULL)
        (void)rt_device_close((rt_device_t)s_i2c);
    s_i2c = RT_NULL;
}
static int gc032a_serial_i2c_write(uint8_t reg, uint8_t value)
{
    struct rt_i2c_bus_device *bus;
    uint8_t data[2] = {reg, value};
    struct rt_i2c_msg msg = {
        .addr = GC032A_SERIAL_I2C_ADDR,
        .flags = RT_I2C_WR,
        .buf = data,
        .len = sizeof(data),
    };

    bus = rt_i2c_bus_device_find(GC032A_SERIAL_I2C_NAME);
    if (bus == RT_NULL)
        return -1;
    return rt_i2c_transfer(bus, &msg, 1) == 1 ? 0 : -1;
}

static int gc032a_serial_i2c_read(uint8_t reg, uint8_t *value)
{
    struct rt_i2c_bus_device *bus;
    struct rt_i2c_msg msg[2];

    bus = rt_i2c_bus_device_find(GC032A_SERIAL_I2C_NAME);
    if (bus == RT_NULL)
        return -1;
    msg[0].addr = GC032A_SERIAL_I2C_ADDR;
    msg[0].flags = RT_I2C_WR;
    msg[0].buf = &reg;
    msg[0].len = 1;
    msg[1].addr = GC032A_SERIAL_I2C_ADDR;
    msg[1].flags = RT_I2C_RD;
    msg[1].buf = value;
    msg[1].len = 1;
    return rt_i2c_transfer(bus, msg, 2) == 2 ? 0 : -1;
}

static int gc032a_serial_configure_i2c(void)
{
    struct rt_i2c_bus_device *bus;
    struct rt_i2c_configuration configuration = {
        .mode = 0,
        .addr = 0,
        .timeout = 500,
        .max_hz = GC032A_SERIAL_I2C_MAX_HZ,
    };

    bus = rt_i2c_bus_device_find(GC032A_SERIAL_I2C_NAME);
    if (bus == RT_NULL)
    {
        LOG_E("i2c bus %s not found", GC032A_SERIAL_I2C_NAME);
        return -1;
    }
    if (rt_device_open((rt_device_t)bus, RT_DEVICE_FLAG_RDWR | bus->parent.open_flag) != RT_EOK)
    {
        LOG_E("i2c open failed");
        return -1;
    }
    s_i2c = bus;
    if (rt_i2c_configure(bus, &configuration) != RT_EOK)
    {
        gc032a_serial_close_i2c();
        LOG_E("i2c configure failed");
        return -1;
    }
    return 0;
}

static int gc032a_serial_identify_and_reset(void)
{
    uint16_t id = 0U;

    for (unsigned int attempt = 0U; attempt < 2U; attempt++)
    {
        uint8_t high = 0U;
        uint8_t low = 0U;

        if (gc032a_serial_i2c_read(GC032A_REG_CHIP_ID_HIGH, &high) != 0 ||
            gc032a_serial_i2c_read(GC032A_REG_CHIP_ID_LOW, &low) != 0)
            return -1;
        id = (uint16_t)(((uint16_t)high << 8) | low);
        if (id != GC032A_CHIP_ID)
        {
            LOG_E("unexpected chip id 0x%04x", id);
            return -1;
        }
    }
    LOG_I("detected GC032A, ID=0x%04x (two reads)", id);

    /* Reset the sensor before applying its serial-mode register table. */
    if (gc032a_serial_i2c_write(GC032A_REG_PAGE_SELECT,
                               GC032A_SOFTWARE_RESET) != 0)
    {
        LOG_E("sensor software reset failed");
        return -1;
    }
    LOG_I("sensor software reset FE=F0, wait 100 ms");
    rt_thread_mdelay(100U);
    return 0;
}

static int gc032a_serial_load_table(void)
{
    size_t i;

    for (i = 0U; i < sizeof(gc032a_serial_default_regs) /
                        sizeof(gc032a_serial_default_regs[0]); i++)
    {
        if (gc032a_serial_i2c_write(gc032a_serial_default_regs[i][0],
                                    gc032a_serial_default_regs[i][1]) != 0)
            return -1;
    }
    /* Serial output selects RGB565 as 0x86 (the 0x06 low field is the RGB565
     * code, 0x03 is YUV422). */
    if (gc032a_serial_i2c_write(GC032A_REG_PAGE_SELECT, 0x00U) != 0 ||
        gc032a_serial_i2c_write(GC032A_REG_OUTPUT_FORMAT, 0x86U) != 0)
        return -1;
    return 0;
}

static int gc032a_serial_verify_output(void)
{
    uint8_t format = 0U;

    if (gc032a_serial_i2c_write(GC032A_REG_PAGE_SELECT, 0x00U) != 0 ||
        gc032a_serial_i2c_read(GC032A_REG_OUTPUT_FORMAT, &format) != 0)
        return -1;
    if (format != 0x86U)
    {
        LOG_E("unexpected output format 0x%02x (want 0x86 RGB565 serial)",
              format);
        return -1;
    }
    LOG_I("serial mode RGB565, 0x44=0x%02x", format);

    /* Read back output-clock and serial controls, then leave the sensor on
     * page 0. Register values do not prove that clock edges reach DCMI. */
    static const uint8_t clock_regs[] = {0xf3, 0xf7, 0xf8, 0xf9, 0xfa};
    static const uint8_t serial_regs[] = {0x51, 0x52, 0x53, 0x64};
    uint8_t clock_values[sizeof(clock_regs)] = {0};
    uint8_t serial_values[sizeof(serial_regs)] = {0};
    int result = 0;
    size_t i;

    for (i = 0; i < sizeof(clock_regs); i++)
    {
        if (gc032a_serial_i2c_read(clock_regs[i], &clock_values[i]) != 0)
            result = -1;
    }
    if (gc032a_serial_i2c_write(GC032A_REG_PAGE_SELECT, 0x03U) != 0)
        result = -1;
    else
    {
        for (i = 0; i < sizeof(serial_regs); i++)
        {
            if (gc032a_serial_i2c_read(serial_regs[i], &serial_values[i]) != 0)
                result = -1;
        }
    }
    if (gc032a_serial_i2c_write(GC032A_REG_PAGE_SELECT, 0x00U) != 0)
    {
        LOG_E("sensor page 0 restore failed");
        return -1;
    }
    if (result != 0)
    {
        LOG_W("sensor clock/serial diagnostic readback failed");
        return 0;
    }
    LOG_I("sensor clock: f3=%02x f7=%02x f8=%02x f9=%02x fa=%02x",
          clock_values[0], clock_values[1], clock_values[2],
          clock_values[3], clock_values[4]);
    LOG_I("sensor page3: 51=%02x 52=%02x 53=%02x 64=%02x",
          serial_values[0], serial_values[1], serial_values[2], serial_values[3]);
    return 0;
}

static void gc032a_serial_trace_pins(void)
{
#if defined(SF32LB57X) && defined(BOARD_DVP_PINMAP_0)
    static const int pads[] = {PAD_PA02, PAD_PA03, PAD_PA04, PAD_PA08};
    static const pin_function expected[] = {GPTIM2_CH1, DCMI_CLK, DCMI_DI0, DCMI_DI1};
    size_t i;
    int matched = 1;

    for (i = 0; i < sizeof(pads) / sizeof(pads[0]); i++)
    {
        pin_function function = 0;
        PIN_ModeTypeDef mode = 0;
        int index = HAL_PIN_Get(pads[i], &function, &mode, 1);

        if (index < 0 || function != expected[i])
        {
            matched = 0;
            LOG_W("camera pin[%u] function=%u expected=%u index=%d",
                  (unsigned int)i, (unsigned int)function,
                  (unsigned int)expected[i], index);
        }
    }
    LOG_I("camera pinmux PA02/03/04/08: %s", matched ? "matched" : "mismatch");
#endif
}

static int gc032a_serial_start_mclk(void)
{
    s_mclk = (struct rt_device_pwm *)rt_device_find(GC032A_SERIAL_MCLK_DEVICE);
    if (s_mclk == RT_NULL)
    {
        LOG_E("%s not found", GC032A_SERIAL_MCLK_DEVICE);
        return -1;
    }
    if (rt_pwm_set(s_mclk, GC032A_SERIAL_MCLK_CHANNEL,
                   GC032A_SERIAL_MCLK_PERIOD_NS,
                   GC032A_SERIAL_MCLK_PERIOD_NS / 2U) != RT_EOK)
        return -1;
    if (rt_pwm_enable(s_mclk, GC032A_SERIAL_MCLK_CHANNEL) != RT_EOK)
        return -1;
    struct rt_pwm_configuration configuration = {0};
    configuration.channel = GC032A_SERIAL_MCLK_CHANNEL;
    if (rt_device_control((rt_device_t)s_mclk, PWM_CMD_GET, &configuration) == RT_EOK)
        LOG_I("MCLK configured: period=%u ns", (unsigned int)configuration.period);
    gc032a_serial_trace_pins();
    rt_thread_mdelay(10U);
    return 0;
}

static void gc032a_serial_stop_mclk(void)
{
    if (s_mclk != RT_NULL)
        (void)rt_pwm_disable(s_mclk, GC032A_SERIAL_MCLK_CHANNEL);
    s_mclk = RT_NULL;
}

static rt_err_t gc032a_serial_rx(rt_device_t dev, rt_size_t size)
{
    if (s_stream_stopping)
        return RT_EOK;
    if (s_stream_active)
    {
        struct rt_dcmi_device *dcmi = (struct rt_dcmi_device *)dev;
        int error = rt_hw_dcmi_get_error(dev);

        if (error != 0 || dcmi->hdma.ErrorCode != HAL_DMA_ERROR_NONE)
        {
            s_stream_error = -RT_ERROR;
            rt_event_send(&s_stream_events, GC032A_STREAM_EVENT_ERROR);
        }
        /* DCMI also reports normal frame-boundary interrupts with size 0.
         * Only DMA half/full notifications identify a completed buffer. */
        else if (s_stream_error == 0 &&
                 (size == GC032A_FRAME_SIZE || size == GC032A_STREAM_BYTES))
        {
            rt_tick_t now = rt_tick_get();

            s_stream_interval_ms = s_stream_has_frame ?
                (uint32_t)((uint64_t)(now - s_stream_last_tick) * 1000U /
                           RT_TICK_PER_SECOND) : 0U;
            s_stream_last_tick = now;
            s_stream_offset = size == GC032A_FRAME_SIZE ? 0U : GC032A_FRAME_SIZE;
            ++s_stream_sequence;
            s_stream_has_frame = RT_TRUE;
            rt_event_send(&s_stream_events, GC032A_STREAM_EVENT_FRAME);
        }
        return RT_EOK;
    }

    s_rx_error = rt_hw_dcmi_get_error(dev);
    if (size == GC032A_FRAME_SIZE || (size == 0U && s_rx_error != 0))
    {
        s_rx_size = (uint32_t)size;
        rt_completion_done(&s_done);
    }
    return RT_EOK;
}

int gc032a_serial_is_open(void)
{
    return s_open ? 1 : 0;
}

static int gc032a_serial_open_locked(void)
{
    int result;

    if (s_open)
        return 0;

    BSP_CAMERA_PowerUp();
    if (gc032a_serial_start_mclk() != 0)
    {
        gc032a_serial_stop_mclk();
        BSP_CAMERA_PowerDown();
        return -1;
    }
    if (gc032a_serial_configure_i2c() != 0)
    {
        gc032a_serial_close_i2c();
        gc032a_serial_stop_mclk();
        BSP_CAMERA_PowerDown();
        return -1;
    }
    if (gc032a_serial_identify_and_reset() != 0)
    {
        gc032a_serial_close_i2c();
        gc032a_serial_stop_mclk();
        BSP_CAMERA_PowerDown();
        return -1;
    }
    if (gc032a_serial_load_table() != 0 || gc032a_serial_verify_output() != 0)
    {
        gc032a_serial_close_i2c();
        gc032a_serial_stop_mclk();
        BSP_CAMERA_PowerDown();
        return -1;
    }
    s_dcmi = rt_device_find("dcmi");
    if (s_dcmi == RT_NULL)
    {
        LOG_E("dcmi device not found");
        gc032a_serial_close_i2c();
        gc032a_serial_stop_mclk();
        BSP_CAMERA_PowerDown();
        return -1;
    }
    result = rt_device_open(s_dcmi, RT_DEVICE_FLAG_RDWR);
    if (result != RT_EOK)
    {
        LOG_E("dcmi open failed %d", result);
        s_dcmi = RT_NULL;
        gc032a_serial_close_i2c();
        gc032a_serial_stop_mclk();
        BSP_CAMERA_PowerDown();
        return -1;
    }
    rt_device_set_rx_indicate(s_dcmi, gc032a_serial_rx);
    rt_completion_init(&s_done);
    s_trace_capture = RT_TRUE;
    s_open = RT_TRUE;
    LOG_I("serial front end ready (DCMI SPI 2-line)");
    return 0;
}

int gc032a_serial_open(void)
{
    int result;

    if (gc032a_serial_lock() != 0)
        return -1;
    result = gc032a_serial_open_locked();
    gc032a_serial_unlock();
    return result;
}

static void gc032a_serial_close_locked(void)
{
    if (!s_open)
        return;
    if (s_dcmi != RT_NULL)
    {
        if (s_stream_active || s_stream_stopping)
            (void)gc032a_serial_stream_stop_locked();
        else if (s_dcmi_started)
            (void)rt_device_control(s_dcmi, RT_DEVICE_CTRL_STOP, RT_NULL);
        s_dcmi_started = RT_FALSE;
        rt_device_set_rx_indicate(s_dcmi, RT_NULL);
        rt_device_close(s_dcmi);
        s_dcmi = RT_NULL;
    }
    gc032a_serial_close_i2c();
    gc032a_serial_stop_mclk();
    BSP_CAMERA_PowerDown();
    s_open = RT_FALSE;
}

void gc032a_serial_close(void)
{
    if (gc032a_serial_lock() != 0)
        return;
    gc032a_serial_close_locked();
    gc032a_serial_unlock();
}

static int gc032a_serial_capture_locked(uint8_t *dst, uint32_t size,
                                        uint32_t timeout_ms,
                                        uint32_t *captured_size)
{
    struct rt_dcmi_config config;
    rt_err_t result;
    rt_err_t stop_result;
    uint32_t received;
    int receive_error;

    if (captured_size != RT_NULL)
        *captured_size = 0U;
    if (s_stream_active || s_stream_stopping)
        return -RT_EBUSY;
    if (!s_open || dst == RT_NULL || size != GC032A_FRAME_SIZE ||
        ((uintptr_t)dst & 63U) != 0U)
        return -1;

    memset(&config, 0, sizeof(config));
    config.dcmi_cfg = DCMI_ALL_CFG;
    config.dcmi_if = DCMI_IF_SPI;
    config.dcmi_spi.pkt_size = GC032A_SERIAL_PACKET_SIZE;
    config.dcmi_spi.pkt_id = GC032A_SERIAL_PACKET_ID;
    config.dcmi_spi.spi_dataline = DCMI_SPI_2LINE;
    config.dcmi_spi.spi_clkeg = DCMI_SPI_CLK_N;
    config.dcmi_spi.spi_sync = GC032A_SERIAL_SYNC_WORD;
    config.dcmi_cm = DCMI_CPT_SIG;
    config.yuv2rgb_en = DCMI_YUV2RGB_DIS;
    config.frame_buffer = dst;
    config.buffer_size = size;

    if (s_trace_capture)
        LOG_I("first capture: prepare buffer=%p bytes=%u", dst, (unsigned int)size);
    /* Maintain only this frame: whole-cache invalidation can discard dirty
     * JPEG or UI data produced by the other threads. */
    SCB_CleanInvalidateDCache_by_Addr((uint32_t *)dst, (int32_t)size);
    s_rx_size = 0U;
    s_rx_error = 0;
    rt_completion_init(&s_done);

    result = rt_device_control(s_dcmi, RT_DEVICE_CTRL_CONFIG, &config);
    if (result == RT_EOK)
    {
        s_dcmi_started = RT_TRUE;
        result = rt_device_control(s_dcmi, RT_DEVICE_CTRL_START, RT_NULL);
    }
    if (result == RT_EOK)
    {
        if (s_trace_capture)
        {
            LOG_I("first capture: DMA started, frame timeout=%u ms",
                  (unsigned int)timeout_ms);
            s_trace_capture = RT_FALSE;
        }
        result = rt_completion_wait(&s_done,
                                    rt_tick_from_millisecond(timeout_ms));
        if (result != RT_EOK)
            LOG_W("frame wait failed %d, stopping capture", result);
    }
    else
        LOG_W("capture configuration/start failed %d", result);

    /* STOP may invoke an abort callback; retain the completed frame outcome. */
    received = s_rx_size;
    receive_error = s_rx_error;
    stop_result = rt_device_control(s_dcmi, RT_DEVICE_CTRL_STOP, RT_NULL);
    if (stop_result != RT_EOK)
    {
        LOG_W("capture stop failed %d", stop_result);
        return stop_result;
    }
    s_dcmi_started = RT_FALSE;
    if (result != RT_EOK)
        return result;
    if (receive_error != 0 || received != size)
    {
        LOG_W("capture bad frame error=0x%x size=%u", receive_error,
              (unsigned int)received);
        return -1;
    }
    SCB_InvalidateDCache_by_Addr(dst, (int32_t)size);
    if (captured_size != RT_NULL)
        *captured_size = size;
    return 0;
}
int gc032a_serial_capture(uint8_t *dst, uint32_t size, uint32_t timeout_ms,
                          uint32_t *captured_size)
{
    int result;

    if (gc032a_serial_lock() != 0)
        return -1;
    result = gc032a_serial_capture_locked(dst, size, timeout_ms,
                                          captured_size);
    gc032a_serial_unlock();
    return result;
}

/* Keep the DMA ring private to the stream. Its completed half is borrowed only
 * until DMA wraps to that half; callers publish a separate stable copy. */
int gc032a_serial_stream_start(uint8_t *buffer, uint32_t size)
{
    struct rt_dcmi_config config = {0};
    struct rt_dcmi_device *dcmi;
    rt_base_t level;
    int result;

    if (buffer == RT_NULL || size != GC032A_STREAM_BYTES ||
        ((uintptr_t)buffer & 63U) != 0U)
        return -RT_EINVAL;
    if (gc032a_serial_lock() != 0)
        return -RT_ERROR;
    if (!s_open || s_dcmi == RT_NULL)
    {
        result = -RT_ERROR;
        goto done;
    }
    if (s_stream_active || s_stream_stopping)
    {
        result = -RT_EBUSY;
        goto done;
    }
    if (!s_stream_events_initialized)
    {
        result = rt_event_init(&s_stream_events, "gc032a_stream", RT_IPC_FLAG_PRIO);
        if (result != RT_EOK)
            goto done;
        s_stream_events_initialized = RT_TRUE;
    }
    rt_event_control(&s_stream_events, RT_IPC_CMD_RESET, RT_NULL);
    SCB_CleanInvalidateDCache_by_Addr((uint32_t *)buffer, (int32_t)size);

    config.dcmi_cfg = DCMI_ALL_CFG;
    config.dcmi_if = DCMI_IF_SPI;
    config.dcmi_spi.pkt_size = GC032A_SERIAL_PACKET_SIZE;
    config.dcmi_spi.pkt_id = GC032A_SERIAL_PACKET_ID;
    config.dcmi_spi.spi_dataline = DCMI_SPI_2LINE;
    config.dcmi_spi.spi_clkeg = DCMI_SPI_CLK_N;
    config.dcmi_spi.spi_sync = GC032A_SERIAL_SYNC_WORD;
    config.dcmi_cm = DCMI_CPT_CON;
    config.yuv2rgb_en = DCMI_YUV2RGB_DIS;
    config.frame_buffer = buffer;
    config.buffer_size = size;
    result = rt_device_control(s_dcmi, RT_DEVICE_CTRL_CONFIG, &config);
    if (result != RT_EOK)
        goto done;

    dcmi = (struct rt_dcmi_device *)s_dcmi;
    level = rt_hw_interrupt_disable();
    dcmi->error = 0;
    dcmi->hdma.ErrorCode = HAL_DMA_ERROR_NONE;
    s_stream_buffer = buffer;
    s_stream_offset = 0U;
    /* Preserve the sequence across restarts so an old borrowed frame cannot
     * become valid again after the first interrupt of a new stream. */
    ++s_stream_sequence;
    s_stream_delivered_sequence = s_stream_sequence;
    s_stream_interval_ms = 0U;
    s_stream_last_tick = 0;
    s_stream_error = 0;
    s_stream_has_frame = RT_FALSE;
    s_stream_stopping = RT_FALSE;
    s_stream_active = RT_TRUE;
    rt_hw_interrupt_enable(level);

    s_dcmi_started = RT_TRUE;
    result = rt_device_control(s_dcmi, RT_DEVICE_CTRL_START, RT_NULL);
    if (result != RT_EOK)
        (void)gc032a_serial_stream_stop_locked();
    else
        LOG_I("continuous capture started, two VGA DMA buffers");
done:
    gc032a_serial_unlock();
    return result;
}

int gc032a_serial_stream_wait(gc032a_serial_stream_frame_t *frame,
                              uint32_t timeout_ms)
{
    rt_tick_t begin;
    rt_tick_t timeout;
    int result = RT_EOK;

    if (frame == RT_NULL)
        return -RT_EINVAL;
    memset(frame, 0, sizeof(*frame));
    if (gc032a_serial_lock() != 0)
        return -RT_ERROR;
    if (!s_stream_active || s_stream_stopping)
    {
        result = -RT_ERROR;
        goto done;
    }
    begin = rt_tick_get();
    timeout = rt_tick_from_millisecond(timeout_ms);
    for (;;)
    {
        rt_uint32_t events = 0U;
        rt_tick_t elapsed = rt_tick_get() - begin;
        rt_tick_t remaining = elapsed < timeout ? timeout - elapsed : 0;
        rt_base_t level;

        result = rt_event_recv(&s_stream_events,
                               GC032A_STREAM_EVENT_FRAME | GC032A_STREAM_EVENT_ERROR,
                               RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                               remaining, &events);
        if (result != RT_EOK)
            break;
        level = rt_hw_interrupt_disable();
        if (s_stream_error != 0 || (events & GC032A_STREAM_EVENT_ERROR) != 0U)
            result = -RT_ERROR;
        else if (s_stream_has_frame &&
                 s_stream_sequence != s_stream_delivered_sequence)
        {
            frame->data = s_stream_buffer + s_stream_offset;
            frame->sequence = s_stream_sequence;
            frame->interval_ms = s_stream_interval_ms;
            s_stream_delivered_sequence = s_stream_sequence;
        }
        rt_hw_interrupt_enable(level);
        if (result != RT_EOK || frame->data != RT_NULL)
            break;
        if (rt_tick_get() - begin >= timeout)
        {
            result = -RT_ETIMEOUT;
            break;
        }
    }
    if (result == RT_EOK)
        SCB_InvalidateDCache_by_Addr((void *)frame->data, (int32_t)GC032A_FRAME_SIZE);
done:
    gc032a_serial_unlock();
    return result;
}

int gc032a_serial_stream_frame_valid(const gc032a_serial_stream_frame_t *frame)
{
    rt_base_t level;
    int valid = 0;

    if (frame == RT_NULL)
        return 0;
    level = rt_hw_interrupt_disable();
    if (s_stream_active && !s_stream_stopping && s_stream_has_frame &&
        s_stream_error == 0 && frame->sequence == s_stream_sequence &&
        frame->data == s_stream_buffer + s_stream_offset)
    {
        struct rt_dcmi_device *dcmi = (struct rt_dcmi_device *)s_dcmi;
        uint32_t reads_left = __HAL_DMA_GET_COUNTER(&dcmi->hdma);
        uint32_t writes_left = reads_left;

#if defined(SF32LB57X)
        /* CNDTR counts source reads; CSR.DSTNDT counts completed destination
         * writes. Check both because the half interrupt follows source reads. */
        writes_left = ((GPDMA_Channel_TypeDef *)dcmi->hdma.Instance)->CSR &
                      GPDMA_CSR1_DSTNDT_Msk;
#endif
        if (dcmi->hdma.State == HAL_DMA_STATE_BUSY &&
            dcmi->hdma.ErrorCode == HAL_DMA_ERROR_NONE)
        {
            if (s_stream_offset == 0U)
                valid = reads_left > 0U && reads_left <= GC032A_FRAME_WORDS &&
                        writes_left > 0U && writes_left <= GC032A_FRAME_WORDS;
            else
                valid = reads_left > GC032A_FRAME_WORDS &&
                        reads_left <= 2U * GC032A_FRAME_WORDS &&
                        writes_left > GC032A_FRAME_WORDS &&
                        writes_left <= 2U * GC032A_FRAME_WORDS;
        }
    }
    rt_hw_interrupt_enable(level);
    return valid;
}

static int gc032a_serial_stream_stop_locked(void)
{
    rt_base_t level;
    int result;

    if (!s_stream_active)
        return RT_EOK;
    level = rt_hw_interrupt_disable();
    s_stream_stopping = RT_TRUE;
    s_stream_active = RT_FALSE;
    s_stream_has_frame = RT_FALSE;
    ++s_stream_sequence;
    rt_hw_interrupt_enable(level);

    result = rt_device_control(s_dcmi, RT_DEVICE_CTRL_STOP, RT_NULL);
    if (result == RT_EOK)
        s_dcmi_started = RT_FALSE;
    level = rt_hw_interrupt_disable();
    s_stream_buffer = RT_NULL;
    s_stream_stopping = RT_FALSE;
    rt_hw_interrupt_enable(level);
    rt_event_control(&s_stream_events, RT_IPC_CMD_RESET, RT_NULL);
    return result;
}

int gc032a_serial_stream_stop(void)
{
    int result;

    if (gc032a_serial_lock() != 0)
        return -RT_ERROR;
    result = gc032a_serial_stream_stop_locked();
    gc032a_serial_unlock();
    return result;
}
