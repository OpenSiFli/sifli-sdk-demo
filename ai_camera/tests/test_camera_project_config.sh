#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
config="$project_dir/project/proj.conf"

grep -qx 'CONFIG_CAMERA_SCCB_I2C_BUS_NAME="i2c1"' "$config"
grep -qx 'CONFIG_CAMERA_SCCB_SCL_PIN=27' "$config"
grep -qx 'CONFIG_CAMERA_SCCB_SDA_PIN=20' "$config"
grep -qx 'CONFIG_CAMERA_ARDUCAM_SPI_BUS2=y' "$config"
