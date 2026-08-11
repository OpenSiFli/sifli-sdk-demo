#ifndef GC032A_BACKEND_H
#define GC032A_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "camera_source.h"

typedef int (*gc032a_reg_read_fn_t)(void *context, uint8_t reg,
                                    uint8_t *value);
typedef int (*gc032a_reg_write_fn_t)(void *context, uint8_t reg,
                                     uint8_t value);
typedef int (*gc032a_sccb_send_fn_t)(void *context, uint8_t address,
                                     const uint8_t *data, size_t length);
typedef int (*gc032a_sccb_recv_fn_t)(void *context, uint8_t address,
                                     uint8_t *data, size_t length);

typedef struct
{
    uint32_t packet_size;
    uint32_t packet_ids;
    uint32_t sync_word;
    uint8_t data_lines;
    uint8_t sample_falling_edge;
} gc032a_dcmi_profile_t;

typedef struct
{
    uint8_t output_format;
    uint8_t serial_enable;
    uint8_t serial_mode;
    uint8_t serial_clock;
    uint8_t serial_control;
    uint8_t frame_start;
    uint8_t line_start;
    uint8_t line_end;
    uint8_t frame_end;
} gc032a_serial_status_t;

typedef enum
{
    GC032A_PROBE_OK = 0,
    GC032A_PROBE_BUS_ERROR = -1,
    GC032A_PROBE_ID_MISMATCH = -2,
} gc032a_probe_result_t;

int gc032a_probe_twice(gc032a_reg_read_fn_t read_reg, void *context,
                       uint16_t ids[2]);
int gc032a_identify_and_reset(gc032a_reg_read_fn_t read_reg,
                              gc032a_reg_write_fn_t write_reg,
                              void *context, uint16_t ids[2],
                              uint8_t *failed_reg);
int gc032a_apply_serial_settings(gc032a_reg_write_fn_t write_reg,
                                 void *context, uint8_t *failed_reg);
int gc032a_sccb_read_register(gc032a_sccb_send_fn_t send,
                              gc032a_sccb_recv_fn_t recv, void *context,
                              uint8_t reg, uint8_t *value);
void gc032a_get_dcmi_profile(gc032a_dcmi_profile_t *profile);
uint32_t gc032a_mclk_period_ns(void);
int gc032a_stream_frame_offset(uint32_t callback_size, uint32_t *offset);

#ifndef CAMERA_GC032A_HOST_TEST
int gc032a_backend_probe(uint16_t ids[2]);
int gc032a_backend_init(uint16_t ids[2], gc032a_serial_status_t *status,
                        uint8_t *failed_reg);
int gc032a_backend_capture_one(uint8_t *dst, uint32_t size,
                               uint32_t timeout_ms, uint32_t *captured_size,
                               int *dcmi_error);
const camera_source_ops_t *gc032a_backend_ops(void);
#endif

#endif
