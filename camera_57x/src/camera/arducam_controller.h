#ifndef ARDUCAM_CONTROLLER_H
#define ARDUCAM_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#define ARDUCAM_REG_TEST1 0x00U
#define ARDUCAM_REG_FIFO_CONTROL 0x04U
#define ARDUCAM_REG_CPLD_RESET 0x07U
#define ARDUCAM_REG_TRIGGER 0x41U
#define ARDUCAM_REG_FIFO_SIZE1 0x42U
#define ARDUCAM_REG_FIFO_SIZE2 0x43U
#define ARDUCAM_REG_FIFO_SIZE3 0x44U

#define ARDUCAM_FIFO_CLEAR_MASK 0x01U
#define ARDUCAM_FIFO_START_MASK 0x02U
#define ARDUCAM_CAPTURE_DONE_MASK 0x08U
#define ARDUCAM_TEST_VALUE 0x55U
#define ARDUCAM_OV2640_BANK_REG 0xFFU
#define ARDUCAM_OV2640_PID_HIGH_REG 0x0AU
#define ARDUCAM_OV2640_PID_LOW_REG 0x0BU
#define ARDUCAM_MAX_FIFO_SIZE 0x00800000U

enum
{
    ARDUCAM_OK = 0,
    ARDUCAM_ERROR_ARGUMENT = -1,
    ARDUCAM_ERROR_IO = -2,
    ARDUCAM_ERROR_ID = -3,
    ARDUCAM_ERROR_TIMEOUT = -4,
    ARDUCAM_ERROR_LENGTH = -5,
    ARDUCAM_ERROR_JPEG = -6,
    ARDUCAM_ERROR_WRITE = -7,
};

typedef enum
{
    ARDUCAM_CAPTURE_PREVIEW,
    ARDUCAM_CAPTURE_STILL,
} arducam_capture_mode_t;

typedef int (*arducam_writer_t)(void *context, const uint8_t *data,
                                size_t size);

typedef struct
{
    int (*chip_write)(void *context, uint8_t reg, uint8_t value);
    int (*chip_read)(void *context, uint8_t reg, uint8_t *value);
    int (*sensor_write)(void *context, uint8_t reg, uint8_t value);
    int (*sensor_read)(void *context, uint8_t reg, uint8_t *value);
    int (*fifo_begin)(void *context);
    int (*fifo_read)(void *context, uint8_t *data, size_t size);
    int (*fifo_end)(void *context);
    int (*set_mode)(void *context, arducam_capture_mode_t mode);
    void (*delay_ms)(void *context, uint32_t milliseconds);
} arducam_controller_ops_t;

int arducam_controller_probe(const arducam_controller_ops_t *ops,
                             void *context, uint16_t *id);
int arducam_controller_capture(const arducam_controller_ops_t *ops,
                               void *context, arducam_capture_mode_t mode,
                               uint8_t *scratch, size_t scratch_size,
                               arducam_writer_t writer, void *writer_context,
                               uint32_t *captured_size, uint32_t timeout_ms);

#endif
