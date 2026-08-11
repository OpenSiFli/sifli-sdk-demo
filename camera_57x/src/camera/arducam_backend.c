#include "arducam_backend.h"

#include <bf0_hal.h>
#include <drv_spi.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>

#include "arducam_controller.h"
#include "arducam_ov2640.h"
#include "arducam_protocol.h"

#define ARDUCAM_SPI_BUS_NAME "spi2"
#define ARDUCAM_SPI_DEVICE_NAME "arducam"
#define ARDUCAM_I2C_BUS_NAME "i2c1"
#define ARDUCAM_I2C_ADDRESS 0x30U
#define ARDUCAM_SPI_CLOCK_HZ 8000000U
#define ARDUCAM_FIFO_BURST_COMMAND 0x3CU
#define ARDUCAM_CAPTURE_TIMEOUT_MS 3000U
#define ARDUCAM_SCRATCH_SIZE 8192U

typedef struct
{
    struct rt_spi_device *spi;
    struct rt_i2c_bus_device *i2c;
    uint8_t *stream_buffer;
    uint32_t stream_sequence;
    uint8_t stream_slot;
    rt_bool_t burst_open;
    rt_bool_t mode_valid;
    arducam_capture_mode_t mode;
} arducam_backend_t;

typedef struct
{
    uint8_t *data;
    uint32_t capacity;
    uint32_t size;
} arducam_memory_writer_t;

static arducam_backend_t backend;
static uint8_t fifo_scratch[ARDUCAM_SCRATCH_SIZE]
    __attribute__((aligned(64)));

static int arducam_spi_write(void *context, uint8_t reg, uint8_t value)
{
    arducam_backend_t *camera = context;
    uint8_t data[2] = {arducam_register_write_command(reg), value};

    return rt_spi_send(camera->spi, data, sizeof(data)) == sizeof(data) ? 0 : -1;
}

static int arducam_spi_read(void *context, uint8_t reg, uint8_t *value)
{
    arducam_backend_t *camera = context;
    uint8_t command = arducam_register_read_command(reg);

    return rt_spi_send_then_recv(camera->spi, &command, sizeof(command),
                                 value, 1U) == RT_EOK ? 0 : -1;
}

static int arducam_sensor_write(void *context, uint8_t reg, uint8_t value)
{
    arducam_backend_t *camera = context;
    uint8_t data[2] = {reg, value};
    struct rt_i2c_msg message =
    {
        .addr = ARDUCAM_I2C_ADDRESS,
        .flags = RT_I2C_WR,
        .buf = data,
        .len = sizeof(data),
    };

    return rt_i2c_transfer(camera->i2c, &message, 1U) == 1 ? 0 : -1;
}

static int arducam_sensor_read(void *context, uint8_t reg, uint8_t *value)
{
    arducam_backend_t *camera = context;

    if (rt_i2c_master_send(camera->i2c, ARDUCAM_I2C_ADDRESS, RT_I2C_WR,
                           &reg, 1U) != 1)
        return -1;
    return rt_i2c_master_recv(camera->i2c, ARDUCAM_I2C_ADDRESS, RT_I2C_RD,
                              value, 1U) == 1 ? 0 : -1;
}

static void arducam_delay(void *context, uint32_t milliseconds)
{
    (void)context;
    rt_thread_mdelay(milliseconds);
}

static int arducam_fifo_begin(void *context)
{
    arducam_backend_t *camera = context;
    uint8_t command = ARDUCAM_FIFO_BURST_COMMAND;
    struct rt_spi_message message =
    {
        .send_buf = &command,
        .length = 1U,
    };

    if (rt_spi_take_bus(camera->spi) != RT_EOK)
        return -1;
    if (rt_spi_take(camera->spi) != RT_EOK)
    {
        rt_spi_release_bus(camera->spi);
        return -1;
    }
    if (camera->spi->bus->ops->xfer(camera->spi, &message) != 1U)
    {
        rt_spi_release(camera->spi);
        rt_spi_release_bus(camera->spi);
        return -1;
    }
    camera->burst_open = RT_TRUE;
    return 0;
}

static int arducam_fifo_read(void *context, uint8_t *data, size_t size)
{
    arducam_backend_t *camera = context;
    struct rt_spi_message message =
    {
        .recv_buf = data,
        .length = size,
    };

    if (!camera->burst_open)
        return -1;
    return camera->spi->bus->ops->xfer(camera->spi, &message) == size ? 0 : -1;
}

static int arducam_fifo_end(void *context)
{
    arducam_backend_t *camera = context;
    int result = 0;

    if (!camera->burst_open)
        return -1;
    if (rt_spi_release(camera->spi) != RT_EOK)
        result = -1;
    if (rt_spi_release_bus(camera->spi) != RT_EOK)
        result = -1;
    camera->burst_open = RT_FALSE;
    return result;
}

static int arducam_set_mode(void *context, arducam_capture_mode_t mode)
{
    arducam_backend_t *camera = context;

    if (camera->mode_valid && (camera->mode == mode))
        return 0;
    if (arducam_ov2640_set_mode(arducam_sensor_write, arducam_delay,
                                camera, mode) != 0)
        return -1;
    camera->mode = mode;
    camera->mode_valid = RT_TRUE;
    return 0;
}

static const arducam_controller_ops_t controller_ops =
{
    .chip_write = arducam_spi_write,
    .chip_read = arducam_spi_read,
    .sensor_write = arducam_sensor_write,
    .sensor_read = arducam_sensor_read,
    .fifo_begin = arducam_fifo_begin,
    .fifo_read = arducam_fifo_read,
    .fifo_end = arducam_fifo_end,
    .set_mode = arducam_set_mode,
    .delay_ms = arducam_delay,
};

static int arducam_memory_write(void *context, const uint8_t *data, size_t size)
{
    arducam_memory_writer_t *writer = context;

    if ((writer->size > writer->capacity) ||
        (size > writer->capacity - writer->size))
        return -1;
    memcpy(writer->data + writer->size, data, size);
    writer->size += (uint32_t)size;
    return 0;
}

static int arducam_open(void)
{
    struct rt_spi_configuration spi_config =
    {
        .mode = RT_SPI_MODE_0 | RT_SPI_MSB | RT_SPI_MASTER,
        .data_width = 8U,
        .max_hz = ARDUCAM_SPI_CLOCK_HZ,
        .frameMode = RT_SPI_MOTO,
    };
    struct rt_i2c_configuration i2c_config =
    {
        .mode = 0,
        .addr = 0,
        .timeout = 5000,
        .max_hz = 100000U,
    };
    uint16_t id;

    memset(&backend, 0, sizeof(backend));
    HAL_PIN_Set(PAD_PA37, SPI2_DIO, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA38, SPI2_DI, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA39, SPI2_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA40, SPI2_CS, PIN_PULLUP, 1);

    if ((rt_device_find(ARDUCAM_SPI_DEVICE_NAME) == RT_NULL) &&
        (rt_hw_spi_device_attach(ARDUCAM_SPI_BUS_NAME,
                                 ARDUCAM_SPI_DEVICE_NAME) != RT_EOK))
        return -RT_ENOSYS;
    backend.spi = (struct rt_spi_device *)rt_device_find(
        ARDUCAM_SPI_DEVICE_NAME);
    if ((backend.spi == RT_NULL) ||
        (rt_device_open((rt_device_t)backend.spi,
                        RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_DMA_RX |
                        RT_DEVICE_FLAG_DMA_TX) != RT_EOK) ||
        (rt_spi_configure(backend.spi, &spi_config) != RT_EOK))
        goto error;

    backend.i2c = rt_i2c_bus_device_find(ARDUCAM_I2C_BUS_NAME);
    if ((backend.i2c == RT_NULL) ||
        (rt_i2c_open(backend.i2c, RT_DEVICE_FLAG_RDWR) != RT_EOK) ||
        (rt_i2c_configure(backend.i2c, &i2c_config) != RT_EOK))
        goto error;

    if (arducam_controller_probe(&controller_ops, &backend, &id) != 0)
        goto error;
    if (arducam_ov2640_initialize(arducam_sensor_write, arducam_delay,
                                  &backend) != 0)
        goto error;
    backend.mode = ARDUCAM_CAPTURE_PREVIEW;
    backend.mode_valid = RT_TRUE;
    rt_kprintf("camera: Arducam OV2640 id=0x%04x SPI=%u Hz\n",
               id, ARDUCAM_SPI_CLOCK_HZ);
    return 0;

error:
    if (backend.i2c != RT_NULL)
        rt_i2c_close(backend.i2c);
    if (backend.spi != RT_NULL)
        rt_device_close((rt_device_t)backend.spi);
    memset(&backend, 0, sizeof(backend));
    return -RT_ERROR;
}

static int arducam_close(void)
{
    if (backend.burst_open)
        arducam_fifo_end(&backend);
    if (backend.i2c != RT_NULL)
        rt_i2c_close(backend.i2c);
    if (backend.spi != RT_NULL)
        rt_device_close((rt_device_t)backend.spi);
    memset(&backend, 0, sizeof(backend));
    return 0;
}

static int arducam_stream_start(uint8_t *buffer, uint32_t size)
{
    if ((buffer == RT_NULL) || (size != 2U * ARDUCAM_PREVIEW_SLOT_SIZE))
        return -RT_EINVAL;
    backend.stream_buffer = buffer;
    backend.stream_slot = 0U;
    return 0;
}

static int arducam_stream_wait(camera_stream_frame_t *frame,
                               uint32_t timeout_ms)
{
    arducam_memory_writer_t writer;
    uint32_t captured_size;
    int result;

    if ((frame == RT_NULL) || (backend.stream_buffer == RT_NULL))
        return -RT_EINVAL;
    writer = (arducam_memory_writer_t)
    {
        .data = backend.stream_buffer +
                backend.stream_slot * ARDUCAM_PREVIEW_SLOT_SIZE,
        .capacity = ARDUCAM_PREVIEW_SLOT_SIZE,
    };
    result = arducam_controller_capture(&controller_ops, &backend,
                                        ARDUCAM_CAPTURE_PREVIEW,
                                        fifo_scratch, sizeof(fifo_scratch),
                                        arducam_memory_write, &writer,
                                        &captured_size, timeout_ms);
    if (result != 0)
        return result;
    backend.stream_sequence++;
    frame->data = writer.data;
    frame->size = captured_size;
    frame->sequence = backend.stream_sequence;
    backend.stream_slot ^= 1U;
    return 0;
}

static int arducam_stream_stop(void)
{
    backend.stream_buffer = RT_NULL;
    return 0;
}

static int arducam_capture_still(camera_source_writer_t writer,
                                 void *context, uint32_t *captured_size)
{
    int result;

    if ((writer == RT_NULL) || (captured_size == RT_NULL))
        return -RT_EINVAL;
    result = arducam_controller_capture(&controller_ops, &backend,
                                        ARDUCAM_CAPTURE_STILL,
                                        fifo_scratch, sizeof(fifo_scratch),
                                        writer, context, captured_size,
                                        ARDUCAM_CAPTURE_TIMEOUT_MS);
    if (result != ARDUCAM_OK)
        rt_kprintf("camera: still capture error=%d\n", result);
    if (arducam_set_mode(&backend, ARDUCAM_CAPTURE_PREVIEW) != 0)
        return -RT_ERROR;
    return result;
}

static const camera_source_ops_t arducam_ops =
{
    .open = arducam_open,
    .close = arducam_close,
    .stream_start = arducam_stream_start,
    .stream_wait = arducam_stream_wait,
    .stream_stop = arducam_stream_stop,
    .capture_still = arducam_capture_still,
};

const camera_source_ops_t *arducam_backend_ops(void)
{
    return &arducam_ops;
}
