#include <assert.h>

extern int ai_xiaozhi_audio_worker_can_listen(int configured,
                                              int worker_ready);

int main(void)
{
    assert(ai_xiaozhi_audio_worker_can_listen(0, 0) == 0);
    assert(ai_xiaozhi_audio_worker_can_listen(1, 0) == 0);
    assert(ai_xiaozhi_audio_worker_can_listen(0, 1) == 0);
    assert(ai_xiaozhi_audio_worker_can_listen(1, 1) == 1);
    return 0;
}
