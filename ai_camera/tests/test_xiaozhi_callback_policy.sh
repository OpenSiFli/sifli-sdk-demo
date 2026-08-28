#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
websocket="$project_dir/src/xiaozhi_websocket.c"
audio="$project_dir/src/xiaozhi_audio.c"

callback_body=$(sed -n '/^static err_t ai_xiaozhi_websocket_callback/,/^static int ai_xiaozhi_connect_request/p' "$websocket")
receive_body=$(sed -n '/^int ai_xiaozhi_audio_receive/,/^void ai_xiaozhi_audio_disconnect/p' "$audio")

if printf '%s\n' "$callback_body" | grep -Fq 'rt_kprintf('; then
    echo 'WebSocket callback must not log from tcpip context' >&2
    exit 1
fi
if printf '%s\n' "$callback_body" | grep -Fq 'ai_xiaozhi_audio_configure('; then
    echo 'WebSocket callback must not configure audio in tcpip context' >&2
    exit 1
fi
if printf '%s\n' "$callback_body" | grep -Fq 'ai_xiaozhi_audio_disconnect('; then
    echo 'WebSocket callback must not stop audio in tcpip context' >&2
    exit 1
fi
if printf '%s\n' "$receive_body" | grep -Fq 'rt_kprintf('; then
    echo 'WebSocket audio receive path must not log from tcpip context' >&2
    exit 1
fi
