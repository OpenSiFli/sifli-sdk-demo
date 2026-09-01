# Camera software JPEG example

This standalone SiFli SDK example verifies both public JPEG APIs without a
sensor, display, filesystem, PSRAM dependency. Its HCPU
`main.c` encodes a deterministic 32x24 RGB565_BE fixture to a caller-owned
buffer, decodes the result into a separate caller-owned buffer, then verifies
the JPEG markers and decoded dimensions.

`menuconfig` includes the regular **Camera software JPEG** component menu.
This HCPU example defaults to HCPU with both encoder and decoder enabled. It
prints an actionable error instead of silently ignoring a disabled codec or an
ACPU core selection.

## Source code path

`components/camera_software/jpeg/example/src/main.c`

## Supported boards

The checked build uses `spi-hdk_lb573ub7n6_hcpu`. Other SF32LB57X HCPU boards
can be used when their board name and memory map are compatible.

## Hardware required

Only the development board and a serial connection are required. The example
uses a generated 32x24 frame and caller-owned RAM buffers; no camera, display,
filesystem, SD card, or PSRAM input is required.

## Build

```sh
source /path/to/SiFli-SDK/export.sh
cd components/camera_software/jpeg/example/project
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

The checked-in defaults enable both codecs on HCPU. To inspect the component
menu, run:

```sh
scons --board=spi-hdk_lb573ub7n6_hcpu --menuconfig
```

Keep **Camera software JPEG**, **Encoder**, **Decoder**, and **HCPU** enabled
for this verification example.

## Example output

```text
camera_sw_jpeg example: jpeg_size=NNN decoded=32x24 checksum=xxxxxxxx status=PASS
```

The JPEG size and checksum are target-build dependent. `status=PASS` confirms
that encoding, JPEG marker validation, decoding, and geometry validation all
completed.

## Troubleshooting

- If the log reports a disabled codec or ACPU selection, restore both codec
  options and HCPU in menuconfig, then rebuild.
- If no log appears, check the board serial port and reset the board after
  flashing `main.bin` with the SDK download tool.
