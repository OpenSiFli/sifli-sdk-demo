#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)

if sed -n '/^int main(void)/,/^}/p' "$project_dir/src/main.c" |
    grep -Fq 'ai_camera_capture_init('; then
    echo 'camera capture must not allocate its worker at boot' >&2
    exit 1
fi

grep -Fq 'if (!s_capture_initialized && ai_camera_capture_init() < 0)' \
    "$project_dir/src/camera_capture.c"
