# Camera software ACPU example

This directory is a minimal synchronous SiFli dual-core adapter and a directly
buildable SiFli SDK example for the sibling `isp` and `jpeg` components. It is
not a sensor, display, file-system, or frame-buffer implementation.

## Source code path

The HCPU verification entry is
`components/camera_software/example/acpu/src/hcpu/main.c`. The ACPU worker is
`components/camera_software/example/acpu/acpu/camera_sw_acpu_worker.c`.

## Supported boards

The checked build uses `spi-hdk_lb573ub7n6_hcpu` on an SF32LB57X dual-core
board with ACPU support. Single-core boards cannot run this example.

## Hardware required

Use a dual-core development board with shared L2/PSRAM visible to both cores,
plus a serial connection. No camera, display, filesystem, or SD card is
required because the example creates its fixture in shared memory.

## Standalone build

The executable HCPU entry is `src/hcpu/main.c`. It creates a deterministic
32x24 RGB565_BE fixture in shared L2/PSRAM memory, runs ISP, JPEG encode, and
JPEG decode through the public `camera_sw_acpu_*` API, and prints `status=PASS`
only when all operations succeed. The SDK ACPU framework owns the actual ACPU
`main()`; this package supplies its application entry at
`acpu/camera_sw_acpu_worker.c:acpu_main()`.

```sh
source /path/to/SiFli-SDK/export.sh
cd components/camera_software/example/acpu/project/hcpu
scons --board=spi-hdk_lb573ub7n6_hcpu
```

This produces the HCPU `main.bin` and an ACPU child image. Flash the generated
download package, then reset the board. The serial log begins with
`camera_sw_acpu example:` and reports JPEG size, `32x24` decoded geometry,
applied ISP flags, and `status=PASS`.

The generated download script is
`build_spi-hdk_lb573ub7n6_hcpu/uart_download.sh`. Run it from the HCPU project
directory, flash the complete package, reset the board, and open the serial
console.

## Configure the project

This is a fixed all-ACPU verification project. Its HCPU SConstruct enables ISP,
JPEG encoding, and JPEG decoding on ACPU so the two images can be built and
verified together. It does not expose a standalone menuconfig choice for moving
these components between cores.

## Example output

```text
camera_sw_acpu example: jpeg_size=NNN decoded=32x24 checksum=xxxxxxxx isp_flags=xxxxxxxx encode_ms=NN status=PASS
```

The timing, checksum, JPEG size, and flags are target-build dependent.
`status=PASS` confirms the HCPU client, ACPU worker, shared buffers, ISP, JPEG
encoder, and JPEG decoder completed successfully.

## Troubleshooting

- If the build does not create `acpu/acpu.bin`, verify that the selected board
  has ACPU support and rebuild from `project/hcpu`.
- If the example reports failure, verify that the HCPU and ACPU images were
  flashed as one download package and reset the board.
- If the serial output is absent, check the board serial port and baud rate.

## Kconfig

Source `components/camera_software/Kconfig` from the consumer project's
`Kconfig.proj`.
The selected ISP/JPEG execution-core options determine which example side is
compiled. ACPU placement requires `BSP_USING_ACPU`.

## SConscript

When at least one software component runs on ACPU, the HCPU SConscript loads
`example/acpu/hcpu/SConscript`; the ACPU image loads
`example/acpu/acpu/SConscript`. Both scripts derive the component root from
their own location and only include public component headers.

## PSRAM and Cache

The caller owns every input, output and destination buffer passed to
`camera_sw_acpu_*`. IPC pointers must refer to memory visible to both cores,
normally shared PSRAM. `acpu_run_task()` performs cache clean/invalidate before
the request; callers must not reuse a buffer until the synchronous call returns.

`camera_sw_acpu_encode_rgb565_be()` accepts the RGB565_BE input, a caller-owned
JPEG destination and its capacity. `camera_sw_acpu_decode_jpeg()` and
`camera_sw_acpu_process_rgb565_be()` likewise operate in-place on
caller-provided shared buffers. The client never returns a pointer to static
JPEG storage.

## acpu_main and Errors

The worker exports `acpu_main()` and must be linked into the ACPU image. It
handles JPEG encode, JPEG decode, ISP and CPU-usage task IDs. All APIs return
`0` on success and `-1` on invalid geometry, insufficient capacity, protocol
mismatch, missing selected codec, or ACPU task failure. JPEG decode supports a
maximum source/output row width of 640 pixels and selects decoder scaling to
fit the destination capacity.
