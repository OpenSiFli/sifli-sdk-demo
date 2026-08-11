#include "camera_acpu_checksum.h"

uint32_t camera_acpu_checksum(const uint8_t *data, size_t size)
{
    uint32_t checksum = 0x811C9DC5U;
    size_t index;

    for (index = 0U; index < size; index++)
    {
        checksum ^= data[index];
        checksum *= 0x01000193U;
    }
    return checksum;
}
