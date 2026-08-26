#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
sdk_dir=${SIFLI_SDK:-/Users/wangmian/SiFli-SDK/main}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ai-camera-tests.XXXXXX")
trap 'rm -r "$build_dir"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
   -I"$project_dir/src" \
   "$test_dir/test_app_state.c" \
   "$project_dir/src/app_state.c" \
   -o "$build_dir/test_app_state"
"$build_dir/test_app_state"

cc -std=c11 -Wall -Wextra -Werror \
   -I"$project_dir/src" \
   "$test_dir/test_audio_loopback.c" \
   "$project_dir/src/audio_loopback.c" \
   -o "$build_dir/test_audio_loopback"
"$build_dir/test_audio_loopback"

cc -std=c11 -Wall -Wextra -Werror \
   -I"$project_dir/src" \
   "$test_dir/test_xiaozhi_audio_format.c" \
   "$project_dir/src/xiaozhi_audio_format.c" \
   -o "$build_dir/test_xiaozhi_audio_format"
"$build_dir/test_xiaozhi_audio_format"

cc -std=c11 -Wall -Wextra -Werror \
   -I"$project_dir/src" \
   -I"$test_dir/include" \
   -I"$sdk_dir/external/cJSON-1.7.17" \
   "$test_dir/test_xiaozhi_protocol.c" \
   "$project_dir/src/xiaozhi_protocol.c" \
   "$sdk_dir/external/cJSON-1.7.17/cJSON.c" \
   -o "$build_dir/test_xiaozhi_protocol"
"$build_dir/test_xiaozhi_protocol"

cc -std=c11 -Wall -Wextra -Werror \
   -I"$project_dir/src" \
   -I"$sdk_dir/middleware/bluetooth/service/bt/bt_finsh" \
   "$test_dir/test_bt_pan_policy.c" \
   "$project_dir/src/bt_pan_policy.c" \
   -o "$build_dir/test_bt_pan_policy"
"$build_dir/test_bt_pan_policy"
