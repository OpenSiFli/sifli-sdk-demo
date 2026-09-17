#ifndef AI_CAMERA_GC032A_SERIAL_H
#define AI_CAMERA_GC032A_SERIAL_H

#include <stdint.h>

#include "gc032a_config.h"

/* GC032A 2-line serial (CCIR656) front end, read through the SoC DCMI in
 * SPI 2-line mode.  The sensor is configured over SCCB directly and frames are
 * moved by the DCMI DMA into caller-owned buffers, with no SDK camera
 * component involved.  Ported from the camera_57x reference project. */

#define GC032A_SERIAL_PACKET_SIZE 1280U
#define GC032A_SERIAL_PACKET_ID 0x9d80b6caU
#define GC032A_SERIAL_SYNC_WORD 0x00ffffffU

/* Power up the camera rail, start MCLK, open SCCB and load the serial register
 * table (ending with 0x44 = 0x86, RGB565 for the serial output). */
int gc032a_serial_open(void);
void gc032a_serial_close(void);
int gc032a_serial_is_open(void);

/* Capture exactly one VGA frame into dst (must hold GC032A_FRAME_SIZE bytes).
 * Blocking, returns 0 on success; returns busy while continuous capture runs. */
int gc032a_serial_capture(uint8_t *dst, uint32_t size, uint32_t timeout_ms,
                          uint32_t *captured_size);

typedef struct
{
    const uint8_t *data;
    uint32_t sequence;
    uint32_t interval_ms;
} gc032a_serial_stream_frame_t;

/* Start continuous capture after open(). The caller owns a 64-byte-aligned
 * buffer of 2 * GC032A_FRAME_SIZE bytes until stream_stop() returns. */
int gc032a_serial_stream_start(uint8_t *buffer, uint32_t size);
/* Wait for the latest completed half. The frame remains borrowed from DMA:
 * check frame_valid() before and after copying to a separate stable buffer. */
int gc032a_serial_stream_wait(gc032a_serial_stream_frame_t *frame,
                              uint32_t timeout_ms);
int gc032a_serial_stream_frame_valid(const gc032a_serial_stream_frame_t *frame);
int gc032a_serial_stream_stop(void);

#endif /* AI_CAMERA_GC032A_SERIAL_H */
