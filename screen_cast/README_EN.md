# BLE Screen Cast Example (screen_cast)

## Supported Platforms
+ sf32lb58 LCD series

> This example relies on the chip's built-in JPEG hardware decoder and is only available on chips that include this module; other series (LB52x/LB55x/LB56x) are not supported for now.

## Overview
This example demonstrates a **BLE screen-cast receiver**: the device advertises as a BLE peripheral. After it connects with the sender (a PC script), the PC encodes images and pushes **JPEG frames in real time** to the device over BLE, frame by frame. The device hardware-decodes each received frame and displays it on the screen in real time via LVGL.

Data path:
```
Sender (PC script)
   └─ Fragmentation per the serial-trans protocol (done on the PC side)
        └─ Packet-by-packet transmission over BLE
             └─ The device's serial-trans service automatically reassembles a complete frame
                  └─ serial-trans callback (category 0x20) receives a complete frame
                       └─ JPEG decoding
                            └─ LVGL image real-time refresh and display
```


## Frame Protocol

The **payload** format of each frame sent by the sender (4-byte header + JPEG):

| Field    | Length | Description                       |
|----------|--------|-----------------------------------|
| magic    | 2B     | 0x53C1 (little-endian), identifies a screen-cast frame |
| type     | 1B     | 0 = full-screen JPEG frame        |
| reserved | 1B     | Reserved (for alignment/extension) |
| JPEG     | N      | JPEG image data                   |

The device also accepts a "headerless raw JPEG". Each full frame is transmitted over BLE serial transmission: the sender fragments it per the protocol, and the device's serial-trans service reassembles it automatically. The maximum size of a single frame is 64KB (the total-length field is a uint16). Just control the JPEG quality/resolution so that a single frame does not exceed this limit.

Transport-layer fragment format (defined by the serial transmission service; see `bf0_sibles_serial_trans_service.h` for details, and the sender must follow it):
- Each BLE packet: `[category(1B)=0x20][flag(1B)][...]`
- flag: 0 = complete packet, 1 = first packet (followed by a 2B total length), 2 = middle packet, 3 = last packet

## Example Usage

### 1. Compilation
Switch to the project directory and run:
```
scons --board=sf32lb58-lcd_n16r32n1_dpi -j8
```

### 2. Flashing
Switch to the example's `project/build_xx` directory, run `uart_download.bat`, and select the port as prompted to start downloading:
```c
$ ./uart_download.bat

     Uart Download

please input the serial port num:5
```
For detailed compilation and download steps, refer to the related instructions in [Quick Start](https://docs.sifli.com/projects/sdk/latest/sf32lb52x/quickstart/index.html).

### 3. Starting the Device
After power-on, the device advertises under the name **SIFLI_SCREENCAST**. The screen is black at first, waiting for the sender to stream.

### 4. PC-Side Streaming (Sender)
The PC script is located at `tools/screen_cast_sender.py` and plays the role of the "mobile APP".

Install dependencies:

Change to the script directory. If this is your first time and the script dependencies are not installed, run the following command to install the dependency libraries:
```
pip install bleak pillow dxcam opencv-python numpy mss
```

Usage examples:
```bash
# Loop through the images in a folder and push them to the device (slideshow/test stream)
python screen_cast_sender.py --source folder --path ./imgs --fps 2 --width 800 --quality 35

# Mirror the PC screen to the device in real time
python screen_cast_sender.py --source screen --fps 10 --width 800 --quality 35
```
The script automatically scans for and connects to the device named `SIFLI_SCREENCAST`; you can also use `--address` to specify the address directly and skip scanning.


## Expected Results
1. The device shows a black screen after power-on and can be discovered via BLE scanning (SIFLI_SCREENCAST).
2. After the PC script connects and starts streaming, the device screen displays the pushed images in real time and changes with the sender's content.
3. The device prints decoding information for each frame via serial log (lv_jpeg_decode: jpg_data x data_size x).


## Revision History
| Version | Date | Release Notes |
|:---|:---|:---|
| 0.0.1 | 08/2026 | Initial version |
