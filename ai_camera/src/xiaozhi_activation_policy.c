#include "xiaozhi_activation_policy.h"

int ai_xiaozhi_activation_should_skip(int websocket_active,
                                      int connect_in_progress)
{
    return websocket_active != 0 || connect_in_progress != 0;
}
