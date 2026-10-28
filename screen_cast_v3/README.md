# 地图导航投屏 (screen_cast)

## 支持的平台

* sf32lb58 lcd系列

本例程依赖芯片内置 JPEG 硬件解码器，仅在带该模块的芯片上可用；其他系列（LB52x/LB55x/LB56x）暂不支持

## 概述

本示例演示如何接收手机 App 推送的**地图导航投屏**并显示到 LCD：

- 设备作为 BLE 外设广播，通过 serial-transmission 服务接收数据；
- 传输可选 BLE / WiFi，WiFi 通道见 [`src/wifi_tran`](src/wifi_tran)；
- 本示例默认使用ble传输方式。

投屏数据按帧下发，帧类型包括 JPEG 画面、MP3 音频、导航信息（JSON + 转向图标 ezip）。
报文格式与完整交互流程见 [`src/protocol/protocol_flow.md`](src/protocol/protocol_flow.md)。

## 目录结构

```text
screen_cast/
├─ project/              构建工程（SConstruct / proj.conf / Kconfig.proj）
└─ src/
   ├─ protocol/          协议处理：消息分发与应答、帧解复用
   ├─ wifi_tran/         WiFi TCP 传输（P2P GO / SoftAP）
   ├─ resource/fonts/    字体资源
   ├─ main.c
   ├─ screen_cast_ui.c   投屏画面渲染
   └─ sc_qr.c            二维码显示
```


## 例程的使用

### menuconfig 配置

| 配置项 | 作用 |
|---|---|
| `CONFIG_PROTO_V3` | 协议组件（分帧、收发、超时监督） |
| `CONFIG_BLUETOOTH` | 蓝牙总开关 |
| `CONFIG_BSP_BLE_SERIAL_TRANSMISSION` | BLE 串行传输（投屏数据通道） |
| `CONFIG_USING_SCREEN_CAST` | 投屏服务（定义于 `src/protocol/Kconfig`） |
| `CONFIG_PKG_USING_LITTLEVGL2RTT` / `CONFIG_LVGL_V8` | LVGL |
| `CONFIG_PKG_USING_CJSON` | 解析导航 JSON |
| `CONFIG_LV_USE_QRCODE` | 显示“扫码连接”二维码 |

以上已包含在 `project/proj.conf` 中。

可选的 WiFi 通道（需 WiFi 驱动就位）：

```
CONFIG_RT_USING_WIFI=y
CONFIG_RT_USING_LWIP=y
CONFIG_USING_SCREEN_CAST_WIFI=y      # 必须：否则 src/wifi_tran 不参与编译
CONFIG_USING_SCREEN_CAST_WIFI_AP=y   # 可选：用 SoftAP 代替 P2P GO
```

### 编译和烧录

```bash
cd project
scons --board=sf32lb58-lcd_n16r32n1_dpi -j8
```

产物位于 `project/build_sf32lb58-lcd_n16r32n1_dpi_hcpu/`，运行 `uart_download.bat` 烧录。

## 使用流程

1. 上电后设备广播（名称 `SIFLI_APP-xx-xx-xx-xx-xx-xx`），LCD 显示“扫码连接”页：
   二维码 + 提示文字 `Scan with the phone app to connect`。
2. 用手机 App 扫描该二维码。二维码内容形如：

   ```
   Tt:1;Mac:a0b1c2d3e4f5;Ql:0.2;Sz:840x480;Tp:1
   ```

   App 从 `Mac` 字段取出设备 BLE 地址后直接回连，无需扫描。
3. 连接成功后进入地图导航并开始投屏。**第一帧到达时二维码页自动隐藏**，进入导航模式后叠加转向图标、路况与剩余行程信息。
4. BLE 断开时自动结束后台会话，二维码页重新出现，可再次扫码连接。


## 预期结果

- 未连接时：黑底 + 二维码 + 英文提示。
- 连接并投屏后：LCD 上出现手机端地图画面，并随导航实时刷新。
- 导航模式下显示：`X米进入YY路, 速度:Zkm/h`、剩余时间/距离、红绿灯个数与转向图标。
- 断开连接后回到二维码页。

## 调试命令

设备串口 shell 中可用（`MSH_CMD_EXPORT(sc_cast, screen cast cmd)`）：

```
sc_cast log <0|1>            # 帧率统计日志开关（JPEG/MP3 fps、丢帧、码率）
sc_cast dump <0|1>           # 帧 dump：每帧输出大小与 CRC32
sc_cast raw [type] [count]   # 把接下来 count 个指定类型原始帧以 hex 打印
                             #   type: 0=JPEG 1=MP3 2=EZIP 3=NAV_INFO 4=CAMERA_INFO
                             #   例: sc_cast raw / sc_cast raw 0 2 / sc_cast raw 3 1
sc_cast rsp_freq <n>         # 调整应答频率（下次会话初始化时生效）
sc_cast mode <n>             # 反向请求手机拍照/录像：设值后在下一个需应答的
                             # 数据包时机以 61 报文发出（与 50 应答二选一）
```




