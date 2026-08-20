# Camera software ISP

This component accepts caller-owned RGB565 big-endian buffers and provides
statistics, AWB, CCM, gamma, tone and filtering primitives. It uses standard C
types only and has no RT-Thread, IPC, camera, display or storage dependency.

The OpenMV-derived subset is MIT licensed in `LICENSES/MIT.txt`; the preserved
upstream record is `UPSTREAM-OPENMV.md`. Ordered dithering runs before final
RGB565 quantization and cannot recover sensor precision already discarded by
an RGB565 sensor output.
