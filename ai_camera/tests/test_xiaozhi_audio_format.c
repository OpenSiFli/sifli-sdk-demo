#include <assert.h>
#include <stdint.h>

#include "xiaozhi_audio.h"
#include "xiaozhi_audio_format.h"

int main(void)
{
    uint16_t samples = 0U;

    assert(AI_XIAOZHI_AUDIO_WORKER_STACK_SIZE == 32U * 1024U);
    assert(ai_xiaozhi_audio_should_log_frame(1U));
    assert(!ai_xiaozhi_audio_should_log_frame(2U));
    assert(ai_xiaozhi_audio_should_log_frame(25U));
    assert(!ai_xiaozhi_audio_should_log_frame(26U));
    assert(ai_xiaozhi_audio_worker_can_listen(0, 0) == 0);
    assert(ai_xiaozhi_audio_worker_can_listen(1, 0) == 0);
    assert(ai_xiaozhi_audio_worker_can_listen(0, 1) == 0);
    assert(ai_xiaozhi_audio_worker_can_listen(1, 1) == 1);
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
