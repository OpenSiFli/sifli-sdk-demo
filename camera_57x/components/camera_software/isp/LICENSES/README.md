# ISP Component License Materials

The vendored OpenMV portable subset is MIT licensed. The exact license text is
in `MIT.txt` and also at `camera_57x/openmv/LICENSE`.

Rules:

1. The original copyright and license headers in the vendored
   `camera_sw_isp_isp.c` must be preserved verbatim in every copy.
2. `camera_sw_isp.c` and the public headers must keep the SPDX MIT identifier
   and OpenMV attribution because they wrap the adapted RGB565 routines.
3. On migration to `SiFli-SDK-Peripherals`, ship `MIT.txt`, the vendored
   sources with their headers, and this directory. No upstream NOTICE file
   exists for this subset.
