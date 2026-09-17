TJpgDec R0.03 by ChaN (2021).

Source: SiFli-SDK external/lvgl_v8/src/extra/libs/sjpg/tjpgd.c and tjpgd.h.
The license and copyright notice are preserved in camera_tjpgd.c.

This copy removes the LVGL dependency, prefixes exported symbols with camera_,
uses RGB565 output with scaling, and validates SOF0, SOS and DRI segment sizes.
