#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
ui="$project_dir/src/device_ui.c"

grep -Fq '照片存储失败' "$ui"
grep -Fq '今天的对话' "$ui"
grep -Fq 'lv_obj_set_style_radius' "$ui"
grep -Fq 'DEVICE_UI_STORAGE_ERROR' "$ui"
grep -Fq 'talk_button = device_ui_add_button(toolbar, LV_SYMBOL_AUDIO' "$ui"
grep -Fq 'DEVICE_UI_EVENT_KEY1_RELEASED);' "$ui"
grep -Fq 'device_ui_add_button(toolbar, LV_SYMBOL_IMAGE' "$ui"
grep -Fq 'DEVICE_UI_EVENT_KEY2_PRESSED,' "$ui"
grep -Fq 'device_ui_add_button(toolbar, LV_SYMBOL_PLUS' "$ui"
grep -Fq 'DEVICE_UI_EVENT_KEY3_LONG_PRESSED, LV_EVENT_CLICKED);' "$ui"
