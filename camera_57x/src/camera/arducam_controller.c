#include "arducam_controller.h"

#include "arducam_protocol.h"

static int arducam_ops_valid(const arducam_controller_ops_t *ops)
{
    return (ops != NULL) && (ops->chip_write != NULL) &&
           (ops->chip_read != NULL) && (ops->sensor_write != NULL) &&
           (ops->sensor_read != NULL) && (ops->fifo_begin != NULL) &&
           (ops->fifo_read != NULL) && (ops->fifo_end != NULL) &&
           (ops->set_mode != NULL) && (ops->delay_ms != NULL);
}

int arducam_controller_probe(const arducam_controller_ops_t *ops,
                             void *context, uint16_t *id)
{
    uint8_t value;
    uint8_t high;
    uint8_t low;

    if (!arducam_ops_valid(ops) || (id == NULL))
        return ARDUCAM_ERROR_ARGUMENT;
    *id = 0U;

    if (ops->chip_write(context, ARDUCAM_REG_CPLD_RESET, 0x80U) != 0)
        return ARDUCAM_ERROR_IO;
    ops->delay_ms(context, 100U);
    if (ops->chip_write(context, ARDUCAM_REG_CPLD_RESET, 0x00U) != 0)
        return ARDUCAM_ERROR_IO;
    ops->delay_ms(context, 100U);

    if ((ops->chip_write(context, ARDUCAM_REG_TEST1,
                         ARDUCAM_TEST_VALUE) != 0) ||
        (ops->chip_read(context, ARDUCAM_REG_TEST1, &value) != 0) ||
        (value != ARDUCAM_TEST_VALUE))
        return ARDUCAM_ERROR_IO;

    if ((ops->sensor_write(context, ARDUCAM_OV2640_BANK_REG, 0x01U) != 0) ||
        (ops->sensor_read(context, ARDUCAM_OV2640_PID_HIGH_REG, &high) != 0) ||
        (ops->sensor_read(context, ARDUCAM_OV2640_PID_LOW_REG, &low) != 0))
        return ARDUCAM_ERROR_IO;

    *id = ((uint16_t)high << 8) | low;
    return (high == 0x26U) && ((low == 0x41U) || (low == 0x42U)) ?
           ARDUCAM_OK : ARDUCAM_ERROR_ID;
}

static int arducam_wait_capture(const arducam_controller_ops_t *ops,
                                void *context, uint32_t timeout_ms)
{
    uint32_t elapsed;
    uint8_t trigger;

    for (elapsed = 0U; elapsed <= timeout_ms; elapsed++)
    {
        if (ops->chip_read(context, ARDUCAM_REG_TRIGGER, &trigger) != 0)
            return ARDUCAM_ERROR_IO;
        if ((trigger & ARDUCAM_CAPTURE_DONE_MASK) != 0U)
            return ARDUCAM_OK;
        if (elapsed != timeout_ms)
            ops->delay_ms(context, 1U);
    }
    return ARDUCAM_ERROR_TIMEOUT;
}

static int arducam_read_fifo_size(const arducam_controller_ops_t *ops,
                                  void *context, uint32_t *size)
{
    uint8_t low;
    uint8_t middle;
    uint8_t high;

    if ((ops->chip_read(context, ARDUCAM_REG_FIFO_SIZE1, &low) != 0) ||
        (ops->chip_read(context, ARDUCAM_REG_FIFO_SIZE2, &middle) != 0) ||
        (ops->chip_read(context, ARDUCAM_REG_FIFO_SIZE3, &high) != 0))
        return ARDUCAM_ERROR_IO;
    *size = arducam_fifo_length(low, middle, high);
    return ((*size > 0U) && (*size <= ARDUCAM_MAX_FIFO_SIZE)) ?
           ARDUCAM_OK : ARDUCAM_ERROR_LENGTH;
}

int arducam_controller_capture(const arducam_controller_ops_t *ops,
                               void *context, arducam_capture_mode_t mode,
                               uint8_t *scratch, size_t scratch_size,
                               arducam_writer_t writer, void *writer_context,
                               uint32_t *captured_size, uint32_t timeout_ms)
{
    arducam_jpeg_extractor_t extractor;
    uint32_t fifo_size;
    uint32_t fifo_offset = 0U;
    size_t read_capacity;
    uint8_t *jpeg_buffer;
    size_t jpeg_capacity;
    int result;
    int end_result;

    if (!arducam_ops_valid(ops) || (scratch == NULL) ||
        (scratch_size < 4U) || (writer == NULL) ||
        (captured_size == NULL))
        return ARDUCAM_ERROR_ARGUMENT;
    *captured_size = 0U;

    result = ops->set_mode(context, mode);
    if (result != 0)
        return ARDUCAM_ERROR_IO;
    if ((ops->chip_write(context, ARDUCAM_REG_FIFO_CONTROL,
                         ARDUCAM_FIFO_CLEAR_MASK) != 0) ||
        (ops->chip_write(context, ARDUCAM_REG_FIFO_CONTROL,
                         ARDUCAM_FIFO_START_MASK) != 0))
        return ARDUCAM_ERROR_IO;

    result = arducam_wait_capture(ops, context, timeout_ms);
    if (result != ARDUCAM_OK)
        return result;
    result = arducam_read_fifo_size(ops, context, &fifo_size);
    if (result != ARDUCAM_OK)
        return result;
    if (ops->fifo_begin(context) != 0)
        return ARDUCAM_ERROR_IO;

    read_capacity = (scratch_size - 1U) / 2U;
    jpeg_buffer = scratch + read_capacity;
    jpeg_capacity = scratch_size - read_capacity;
    arducam_jpeg_extractor_init(&extractor);
    result = ARDUCAM_OK;
    while ((fifo_offset < fifo_size) && !extractor.finished)
    {
        size_t chunk = fifo_size - fifo_offset;
        size_t jpeg_chunk;

        if (chunk > read_capacity)
            chunk = read_capacity;
        if (ops->fifo_read(context, scratch, chunk) != 0)
        {
            result = ARDUCAM_ERROR_IO;
            break;
        }
        fifo_offset += (uint32_t)chunk;
        jpeg_chunk = arducam_jpeg_extractor_feed(&extractor, scratch, chunk,
                                                  jpeg_buffer,
                                                  jpeg_capacity);
        if ((jpeg_chunk > 0U) &&
            (writer(writer_context, jpeg_buffer, jpeg_chunk) != 0))
        {
            result = ARDUCAM_ERROR_WRITE;
            break;
        }
    }

    end_result = ops->fifo_end(context);
    if ((result == ARDUCAM_OK) && (end_result != 0))
        result = ARDUCAM_ERROR_IO;
    if ((result == ARDUCAM_OK) &&
        (arducam_jpeg_extractor_finish(&extractor) != 0))
        result = ARDUCAM_ERROR_JPEG;
    if ((result == ARDUCAM_OK) &&
        (ops->chip_write(context, ARDUCAM_REG_FIFO_CONTROL,
                         ARDUCAM_FIFO_CLEAR_MASK) != 0))
        result = ARDUCAM_ERROR_IO;
    if (result == ARDUCAM_OK)
        *captured_size = extractor.size;
    return result;
}
