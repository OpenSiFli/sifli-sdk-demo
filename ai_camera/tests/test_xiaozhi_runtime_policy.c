#include <assert.h>
#include <string.h>

#include "xiaozhi_activation_policy.h"
#include "xiaozhi_audio_format.h"

int main(void)
{
    assert(ai_xiaozhi_activation_should_skip(0, 0) == 0);
    assert(ai_xiaozhi_activation_should_skip(1, 0) == 1);
    assert(ai_xiaozhi_activation_should_skip(0, 1) == 1);
    assert(ai_xiaozhi_activation_should_skip(1, 1) == 1);

    assert(strcmp(ai_xiaozhi_audio_resource_name(
                     AI_XIAOZHI_AUDIO_RESOURCE_RINGBUFFER),
                 "uplink ringbuffer") == 0);
    assert(strcmp(ai_xiaozhi_audio_resource_name(
                     AI_XIAOZHI_AUDIO_RESOURCE_MESSAGE_QUEUE),
                 "downlink message queue") == 0);
    assert(strcmp(ai_xiaozhi_audio_resource_name(
                     AI_XIAOZHI_AUDIO_RESOURCE_EVENT),
                 "audio event") == 0);
    assert(strcmp(ai_xiaozhi_audio_resource_name(
                     AI_XIAOZHI_AUDIO_RESOURCE_THREAD),
                 "opus worker thread") == 0);
    assert(strcmp(ai_xiaozhi_audio_resource_name(
                     AI_XIAOZHI_AUDIO_RESOURCE_OPUS),
                 "opus codec") == 0);
    assert(strcmp(ai_xiaozhi_audio_resource_name(99), "unknown") == 0);
    return 0;
}
