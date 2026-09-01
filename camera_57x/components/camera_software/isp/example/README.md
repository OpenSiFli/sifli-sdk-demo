# Camera software ISP example

This is a standalone SiFli SDK example for the portable ISP component. It
does not require a sensor, display, filesystem, PSRAM, or a `camera_57x/src`
dependency. Its HCPU `main.c` builds a deterministic 32x24 RGB565_BE fixture,
runs the color ISP path, and prints the input/output checksum and applied
flags. A zero return value means the pipeline accepted the frame and applied
all requested stages.

`menuconfig` includes the regular **Camera software ISP** component menu.
This HCPU example defaults to **Software ISP** and **HCPU**. Gamma, AWB, tone,
vivid, and dithering selections determine the fixture pipeline flags.

## Source code path

`components/camera_software/isp/example/src/main.c`

## Supported boards

The checked build uses `spi-hdk_lb573ub7n6_hcpu`. Other SF32LB57X HCPU boards
can be used when their board name and memory map are compatible.

## Hardware required

Only the development board and a serial connection are required. The fixture
is generated in RAM; no camera, display, filesystem, SD card, or PSRAM input
is required.

## Build

```sh
source /path/to/SiFli-SDK/export.sh
cd components/camera_software/isp/example/project
scons --board=spi-hdk_lb573ub7n6_hcpu
```

The generated HCPU image is
`build_spi-hdk_lb573ub7n6_hcpu/main.bin`.

To flash the generated package, run the SDK-provided script from the project
directory and then open the board serial console:

```sh
./build_spi-hdk_lb573ub7n6_hcpu/uart_download.sh
```

## Configure the project

The checked-in defaults are enough for the first build. To inspect the
component menu, run:

```sh
scons --board=spi-hdk_lb573ub7n6_hcpu --menuconfig
```

Keep **Software ISP** and **HCPU** selected. The example intentionally keeps
its deterministic fixture and does not connect to a sensor.

## Example output

```text
camera_sw_isp example: input_checksum=xxxxxxxx output_checksum=xxxxxxxx flags=xxxxxxxx status=PASS
```

The checksums and flags depend on the selected configuration. `status=PASS`
confirms that the fixture was accepted and the requested pipeline completed.

## Troubleshooting

- If the log says to select Software ISP and HCPU, restore those two
  menuconfig choices and rebuild.
- If no log appears, check the board serial port and reset the board after
  flashing `main.bin` with the SDK download tool.
