#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$test_dir/.." && pwd)
config="$project_dir/project/proj.conf"
generated_config="$project_dir/project/build_spi-hdk_lb573ub7n6_hcpu/.config"

config_has()
{
    grep -qx "$1" "$config" ||
        { test -f "$generated_config" && grep -qx "$1" "$generated_config"; }
}

config_has 'CONFIG_CAMERA_SCCB_I2C_BUS_NAME="i2c1"'
config_has 'CONFIG_CAMERA_SCCB_SCL_PIN=27'
config_has 'CONFIG_CAMERA_SCCB_SDA_PIN=20'
config_has 'CONFIG_CAMERA_ARDUCAM_SPI_BUS2=y'
config_has 'CONFIG_CAMERA_ARDUCAM_FIFO_SCRATCH_SIZE=4096'
grep -qx 'CONFIG_RT_DFS_ELM_MAX_SECTOR_SIZE=4096' "$config"
grep -qx 'CONFIG_LV_FONT_DEFAULT_MONTSERRAT_16=y' "$config"
! grep -qx 'CONFIG_LV_FONT_DEFAULT_SIMSUN_16_CJK=y' "$config"
config_has 'CONFIG_PKG_USING_MBEDTLS_USE_ALL_CERTS=n'
config_has 'CONFIG_PKG_USING_MBEDTLS_USER_CERTS=n'
config_has 'CONFIG_PKG_USING_MBEDTLS_DIGICERT_GLOBAL_ROOT_G2=y'
