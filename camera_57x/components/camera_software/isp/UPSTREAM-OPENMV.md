# OpenMV-derived camera routines

This directory records the provenance of the small OpenMV subset adapted for
`camera_57x`.

The reusable standard-C implementation is kept in `camera_software/isp/src`.
Its public headers depend only on standard C types; the sources additionally use `libm`.
RT-Thread, SiFli ACPU IPC, sensor, display, and storage integration remain in
the project source tree.

- Upstream: https://github.com/openmv/openmv
- Pinned commit: `7d4dbf7ab2f00e7684e57dff7bd13812fd9210d7`
- License for the selected source files: MIT

Selected upstream routines:

- `lib/imlib/isp.c`: `imlib_awb_rgb_avg`, `imlib_awb`, `imlib_ccm`, and
  `imlib_gamma` RGB565 paths.
- `lib/imlib/filter.c`: RGB565 mean and 3x3 morph neighborhood logic.
- `drivers/sensors/ov2640.c`: OV2640 built-in AEC/AGC/AWB control and raw
  register readback.

Local adaptations under `camera_software/isp` use explicit RGB565 big-endian byte access, caller-owned
scratch storage, and project-native sensor callbacks. They do not import
OpenMV's `image_t`, allocator, MicroPython bindings, filesystem, or scripting
runtime.

The portable subset also contains project-local composition around those
primitives: a 64-bin percentile Tone LUT and a fixed mild-saturation Vivid
profile using the adapted CCM/gamma paths. These profiles are not claimed as
verbatim OpenMV algorithms. RT-Thread timing, ACPU IPC, mode selection, sensor
AWB transactions, storage, and same-input comparison remain outside this
directory.

AGAST, LSD, ZBAR, proprietary code, and non-commercial-only components are not
included.
