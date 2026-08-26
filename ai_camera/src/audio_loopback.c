#include "audio_loopback.h"

int ai_audio_loopback_forward(ai_audio_write_fn writer, void *context,
                              const uint8_t *data, uint32_t size)
{
    if (writer == 0 || data == 0 || size == 0U)
        return -1;
    return writer(context, data, size);
}
