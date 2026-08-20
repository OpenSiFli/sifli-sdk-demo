# camera_57x

`camera_57x` is a camera demo for the SF32LB57x SPI HDK. It uses an
Arducam OV2640 module with an on-board FIFO, reads JPEG frames through SPI2,
uses PA34 ADC keys for camera controls, and saves photos through J4
(SDMMC2 / `sd1`).

Image post-processing is provided by the in-project
`components/camera_software` package. ISP, JPEG encoding/decoding, and the
optional HCPU/ACPU adapter can be configured and verified independently. The
default application currently uses the OV2640 FIFO-SPI JPEG preview path.
There is no OV2640 DVP/parallel-camera backend or direct RGB565 preview from
the ArduChip FIFO. Those unfinished, unvalidated paths, along with input
comparison and tuning branches, are not part of the default build.

Target board: `sf32lb57-spi-hdk_n16r4`.

The project is pinned to this local SiFli SDK revision:
`e72ab9e761caebe707e95a06e613f1872301e9df`.

The SDK build target is `spi-hdk_lb573ub7n6`, which corresponds to the
`sf32lb57-spi-hdk_n16r4` hardware configuration used by this project.

## Build

```sh
source /path/to/SiFli-SDK/export.sh
scons -C camera_57x/project --board=spi-hdk_lb573ub7n6 -j8
```

A successful build only proves that the firmware image was generated. Camera
capture, SD-card access, and display operation still require on-board testing.

## Preview and photo capture

At startup, the firmware initializes the CO5300 display, Arducam OV2640, and
PA34 ADC keys, then starts continuous preview. The OV2640 produces 640x480
JPEG frames. SPI2 reads each frame from the module FIFO at 8 MHz in chunks;
the software JPEG backend decodes the frame to RGB565, and EPIC rotates,
scales, and center-crops it to fill the 390x450 display.

Connect the module as follows: PA37/MOSI, PA38/MISO, PA39/SCLK, and PA40/CS.
SCCB uses I2C1 on PA20/SDA and PA27/SCL.

Preview uses two FIFO receive buffers and two stable JPEG buffers. While the
display owns one stable frame, capture can continue into the other. When both
stable buffers are unavailable, the oldest ready frame is dropped. The log
prints capture period, frame-copy, and display timing every five seconds:

```text
camera: captured=144 displayed=143 dropped=0 failed=0 fps=6.5 period=140.1 copy=26.1 display=152.7 ms
```

`period` sets the capture-rate limit; `copy` and `display` help identify time
spent in the FIFO, PSRAM, JPEG decode, or LCD path. Actual values depend on the
target board.

Press KEY1 to pause preview, switch the OV2640 to 1600x1200 JPEG, and save the
FIFO data to the SD-card root as `/IMG_0001.JPG`, `/IMG_0002.JPG`, and so on.
The save path validates JPEG SOI and EOI markers and always restores 640x480
preview after completion or failure. KEY2 toggles the album, and KEY3 shows the
next photo. The album starts with the highest-numbered `IMG_*.JPG`, pauses
capture, and shows the filename at the bottom of the screen. The software
decoder supports common baseline JPEG 4:2:2, 4:2:0, and 4:4:4 sampling; it
shows `UNSUPPORTED` for progressive, corrupted, or files larger than 768 KiB.

SDMMC2 remains in the verified one-bit configuration. The DAT1-DAT3 CRC issue
seen in four-bit mode is tracked as a separate hardware issue.

## Verify the software components independently

The component examples use deterministic in-memory frames, so ISP, JPEG, and
the dual-core adapter can be checked without a camera, display, or SD card.
After activating the SDK environment, build the examples from the repository
root:

```sh
source /path/to/SiFli-SDK/export.sh
scons -C camera_57x/components/camera_software/isp/example/project \
      --board=spi-hdk_lb573ub7n6_hcpu
scons -C camera_57x/components/camera_software/jpeg/example/project \
      --board=spi-hdk_lb573ub7n6_hcpu
scons -C camera_57x/components/camera_software/example/acpu/project/hcpu \
      --board=spi-hdk_lb573ub7n6_hcpu
```

The ISP and JPEG examples print `status=PASS` after flashing. The ACPU example
requires a dual-core board and produces both HCPU and ACPU images. See
`camera_57x/components/camera_software/README.md` for the public APIs, Kconfig
options, and shared-buffer requirements.

### RGB565 and OV2640 input paths

`RGB565_BE` is the 16-bit pixel format used by post-processing and display.
The default OV2640 path is "JPEG in the SPI FIFO -> software decode ->
RGB565_BE -> ISP/EPIC/LCD". It is not direct RGB565 output from the OV2640.

The deferred direct path would configure the OV2640 for VGA RGB565 and read a
fixed-size frame through the ArduChip SPI FIFO. That is also not DVP. DVP is a
parallel sensor-to-SoC DCMI interface, and this project does not provide an
OV2640 backend for it. Menuconfig displays the unavailable state but does not
offer either path as a selectable configuration.
