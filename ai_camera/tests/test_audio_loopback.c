#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "audio_loopback.h"

typedef struct
{
    uint8_t data[4];
    uint32_t size;
} sink_t;

static int sink_write(void *context, const uint8_t *data, uint32_t size)
{
    sink_t *sink = context;

    assert(size == 4U);
    memcpy(sink->data, data, size);
    sink->size = size;
    return (int)size;
}

int main(void)
{
    const uint8_t pcm[] = {0x01U, 0x80U, 0xfeU, 0x7fU};
    sink_t sink = {0};

    assert(ai_audio_loopback_forward(sink_write, &sink, pcm, sizeof(pcm)) == 4);
    assert(sink.size == sizeof(pcm));
    assert(memcmp(sink.data, pcm, sizeof(pcm)) == 0);
    assert(ai_audio_loopback_forward(0, &sink, pcm, sizeof(pcm)) < 0);
    assert(ai_audio_loopback_forward(sink_write, &sink, 0, sizeof(pcm)) < 0);
    assert(ai_audio_loopback_forward(sink_write, &sink, pcm, 0U) < 0);
    return 0;
}
