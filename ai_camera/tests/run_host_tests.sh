#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
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
