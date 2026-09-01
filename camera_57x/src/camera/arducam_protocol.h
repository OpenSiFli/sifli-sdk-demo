#ifndef ARDUCAM_PROTOCOL_H
#define ARDUCAM_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint8_t first[2];
    uint8_t last[2];
    uint32_t size;
} arducam_jpeg_validator_t;

typedef struct
{
    uint32_t size;
    uint8_t started;
    uint8_t finished;
    uint8_t saw_ff;
} arducam_jpeg_extractor_t;

uint8_t arducam_register_read_command(uint8_t address);
uint8_t arducam_register_write_command(uint8_t address);
uint32_t arducam_fifo_length(uint8_t low, uint8_t middle, uint8_t high);
void arducam_jpeg_validator_init(arducam_jpeg_validator_t *validator);
void arducam_jpeg_validator_feed(arducam_jpeg_validator_t *validator,
                                 const uint8_t *data, size_t size);
int arducam_jpeg_validator_finish(const arducam_jpeg_validator_t *validator);
void arducam_jpeg_extractor_init(arducam_jpeg_extractor_t *extractor);
size_t arducam_jpeg_extractor_feed(arducam_jpeg_extractor_t *extractor,
                                   const uint8_t *input, size_t input_size,
                                   uint8_t *output, size_t output_capacity);
int arducam_jpeg_extractor_finish(const arducam_jpeg_extractor_t *extractor);

#endif
