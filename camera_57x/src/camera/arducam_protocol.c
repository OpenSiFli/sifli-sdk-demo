#include "arducam_protocol.h"

#include <string.h>

uint8_t arducam_register_read_command(uint8_t address)
{
    return address & 0x7FU;
}

uint8_t arducam_register_write_command(uint8_t address)
{
    return address | 0x80U;
}

uint32_t arducam_fifo_length(uint8_t low, uint8_t middle, uint8_t high)
{
    return ((uint32_t)(high & 0x7FU) << 16) |
           ((uint32_t)middle << 8) | low;
}

void arducam_jpeg_validator_init(arducam_jpeg_validator_t *validator)
{
    if (validator != NULL)
        memset(validator, 0, sizeof(*validator));
}

void arducam_jpeg_validator_feed(arducam_jpeg_validator_t *validator,
                                 const uint8_t *data, size_t size)
{
    size_t index;

    if ((validator == NULL) || ((data == NULL) && (size != 0U)))
        return;

    for (index = 0; index < size; index++)
    {
        if (validator->size < 2U)
            validator->first[validator->size] = data[index];
        validator->last[0] = validator->last[1];
        validator->last[1] = data[index];
        validator->size++;
    }
}

int arducam_jpeg_validator_finish(const arducam_jpeg_validator_t *validator)
{
    if ((validator == NULL) || (validator->size < 4U))
        return -1;
    return (validator->first[0] == 0xFFU) &&
           (validator->first[1] == 0xD8U) &&
           (validator->last[0] == 0xFFU) &&
           (validator->last[1] == 0xD9U) ? 0 : -1;
}

void arducam_jpeg_extractor_init(arducam_jpeg_extractor_t *extractor)
{
    if (extractor != NULL)
        memset(extractor, 0, sizeof(*extractor));
}

size_t arducam_jpeg_extractor_feed(arducam_jpeg_extractor_t *extractor,
                                   const uint8_t *input, size_t input_size,
                                   uint8_t *output, size_t output_capacity)
{
    size_t input_index;
    size_t output_size = 0U;

    if ((extractor == NULL) || (input == NULL) || (output == NULL) ||
        extractor->finished)
        return 0U;

    for (input_index = 0U; input_index < input_size; input_index++)
    {
        uint8_t value = input[input_index];

        if (!extractor->started)
        {
            if (extractor->saw_ff && (value == 0xD8U))
            {
                if (output_capacity - output_size < 2U)
                    break;
                output[output_size++] = 0xFFU;
                output[output_size++] = 0xD8U;
                extractor->size += 2U;
                extractor->started = 1U;
                extractor->saw_ff = 0U;
            }
            else
            {
                extractor->saw_ff = value == 0xFFU;
            }
            continue;
        }

        if (output_size == output_capacity)
            break;
        output[output_size++] = value;
        extractor->size++;
        if (extractor->saw_ff && (value == 0xD9U))
        {
            extractor->finished = 1U;
            extractor->saw_ff = 0U;
            break;
        }
        extractor->saw_ff = value == 0xFFU;
    }
    return output_size;
}

int arducam_jpeg_extractor_finish(const arducam_jpeg_extractor_t *extractor)
{
    return (extractor != NULL) && extractor->started &&
           extractor->finished && (extractor->size >= 4U) ? 0 : -1;
}
