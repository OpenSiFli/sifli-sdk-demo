# Map Navigation Screen Cast (screen_cast)

## Supported Platforms

* SF32LB58 LCD series

This example depends on the on-chip JPEG hardware decoder and only runs on chips that provide it. Other series (LB52x/LB55x/LB56x) are not supported.

## Overview

This example shows how to receive a **map navigation cast** pushed by a phone app and display it on the LCD:

- The device advertises as a BLE peripheral and receives data through the serial-transmission service;
- The transport can be BLE or WiFi; see [`src/wifi_tran`](src/wifi_tran) for the WiFi channel;
- This example uses BLE by default.

Cast data is delivered frame by frame. Frame types include JPEG pictures, MP3 audio and navigation info (JSON + turn icon in ezip format).
See [`src/protocol/protocol_flow.md`](src/protocol/protocol_flow.md) for the message format and the complete interaction flow.

## Directory Structure

```text
screen_cast_v3/
├─ project/              Build project (SConstruct / proj.conf / Kconfig.proj)
└─ src/
   ├─ protocol/          Protocol handling: message dispatch and responses, frame demultiplexing
   ├─ wifi_tran/         WiFi TCP transport (P2P GO / SoftAP)
   ├─ resource/fonts/    Font resources
   ├─ main.c
   ├─ screen_cast_ui.c   Cast rendering
   └─ sc_qr.c            QR code display
```


## Example Usage

### menuconfig Configuration

| Option | Purpose |
|---|---|
| `CONFIG_FRAMED_PROTO` | Protocol component (framing, TX/RX, supervision) |
| `CONFIG_BLUETOOTH` | Bluetooth master switch |
| `CONFIG_BSP_BLE_SERIAL_TRANSMISSION` | BLE serial transmission (cast data channel) |
| `CONFIG_USING_SCREEN_CAST` | Cast service (defined in `src/protocol/Kconfig`) |
| `CONFIG_PKG_USING_LITTLEVGL2RTT` / `CONFIG_LVGL_V8` | LVGL |
| `CONFIG_PKG_USING_CJSON` | Navigation JSON parsing |
| `CONFIG_LV_USE_QRCODE` | "Scan to connect" QR code |

All of the above are already included in `project/proj.conf`.

Optional WiFi channel (a working WiFi driver is required):

```
CONFIG_RT_USING_WIFI=y
CONFIG_RT_USING_LWIP=y
CONFIG_USING_SCREEN_CAST_WIFI=y      # Required: otherwise src/wifi_tran is not compiled
CONFIG_USING_SCREEN_CAST_WIFI_AP=y   # Optional: use SoftAP instead of P2P GO
```

### Build and Flash

```bash
cd project
scons --board=sf32lb58-lcd_n16r32n1_dpi -j8
```

The artifacts are generated under `project/build_sf32lb58-lcd_n16r32n1_dpi_hcpu/`. Run `uart_download.bat` to flash them.

## Usage Steps

1. After power-on the device advertises as `SIFLI_APP-xx-xx-xx-xx-xx-xx`, and the LCD shows the "scan to connect" page: a QR code plus the hint `Scan with the phone app to connect`.
2. Scan the QR code with the phone app. The payload looks like:

   ```
   Tt:1;Mac:a0b1c2d3e4f5;Ql:0.2;Sz:840x480;Tp:1
   ```

   The app takes the device BLE address from the `Mac` field and connects back directly; no scanning is needed.
3. Once connected, map navigation starts and casting begins. **The QR page is hidden as soon as the first frame arrives**; in navigation mode the turn icon, road information and remaining trip information are overlaid.
4. When BLE disconnects, the session is ended automatically, the QR page reappears and you can scan again to connect.


## Expected Results

- Not connected: black background + QR code + English hint.
- Connected and casting: the phone's map is shown on the LCD and refreshes in real time with navigation.
- In navigation mode the LCD shows `X米进入YY路, 速度:Zkm/h` (X m then turn into YY road, speed Z km/h), remaining time/distance, the traffic light count and the turn icon.
- After disconnect the QR page comes back.

## Debug Commands

Available in the device serial shell (`MSH_CMD_EXPORT(sc_cast, screen cast cmd)`):

```
sc_cast log <0|1>            # frame-rate statistics switch (JPEG/MP3 fps, dropped frames, bitrate)
sc_cast dump <0|1>           # frame dump: print size and CRC32 of every frame
sc_cast raw [type] [count]   # print the next count raw frames of the given type as hex
                             #   type: 0=JPEG 1=MP3 2=EZIP 3=NAV_INFO 4=CAMERA_INFO
                             #   e.g. sc_cast raw / sc_cast raw 0 2 / sc_cast raw 3 1
sc_cast rsp_freq <n>         # adjust the response frequency (effective on the next session init)
sc_cast mode <n>             # reverse request to take a photo / record video: sent as
                             #   message 61 with the next packet that needs a response (instead of 50)
```
