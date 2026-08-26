#include <assert.h>
#include <stdint.h>

#include "xiaozhi_audio_format.h"

int main(void)
{
    uint16_t samples = 0U;

    assert(ai_xiaozhi_audio_frame_samples(16000U, 60U, &samples) == 0);
    assert(samples == 960U);
    assert(ai_xiaozhi_audio_frame_samples(24000U, 60U, &samples) == 0);
    assert(samples == 1440U);
    assert(ai_xiaozhi_audio_frame_samples(16000U, 20U, &samples) == 0);
    assert(samples == 320U);
    assert(ai_xiaozhi_audio_frame_samples(16000U, 15U, &samples) < 0);
    assert(ai_xiaozhi_audio_frame_samples(16000U, 7U, &samples) < 0);
    assert(ai_xiaozhi_audio_frame_samples(0U, 60U, &samples) < 0);
    assert(ai_xiaozhi_audio_frame_samples(16000U, 60U, 0) < 0);
    return 0;
}
