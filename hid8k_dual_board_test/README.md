# SF32LB58 USB HID 8 kHz 双板测试

## 目的

本目录包含两块同型号 SF32LB58 NAND 开发板的测试工程：一块作为 USB 2.0 高速 HID 设备，一块作为 USB 主机。

测试以串接在两块板之间的 USB 协议分析仪记录为准。

## 工程

| 目录 | 板端角色 | 功能 |
|---|---|---|
| `device_hid_8k` | USB Device | 枚举为高速 HID，持续提供 8 字节 EP1 IN 报告，KEY1 可切换画圈 |
| `host_stock_urb` | USB Host | 枚举并校验目标设备，自动提交标准长中断 IN URB |

两个工程的目标板均为：

```text
sf32lb58-lcd_a128r32n1_dsi
```


## 设备板行为

设备枚举属性固定为：

- USB 2.0 High-Speed；
- VID/PID `38F4:FFFF`；
- HID 接口，subclass 0，protocol 0；
- 端点 `0x81`，Interrupt IN；
- `wMaxPacketSize=8`；
- 高速 `bInterval=1`，即每个 125 微秒微帧可调度一次。

主机完成 `SET_CONFIGURATION` 后，设备通过标准 `usbd_ep_start_write()` 装入第一份报告。每次 EP1 IN 发送完成，CherryUSB 调用端点完成回调；回调立即用同一公开接口装入下一份报告。设备端没有 125 微秒软件定时器。

报告格式为：

```text
byte 0    buttons = 0
byte 1    X：画圈关闭时为0，画圈开启时为圆周轨迹X增量
byte 2    Y：画圈关闭时为0，画圈开启时为圆周轨迹Y增量
byte 3    wheel = 0
byte 4..7 32位小端递增序号
```

序号只供分析仪检查已经捕获的数据是否连续，设备固件本身不统计或判断。

## 主机板行为和限制

主机只接受上述 VID/PID、速度、接口和端点属性全部匹配的设备。有效枚举后等待不少于 100 毫秒，再自动提交一个标准 CherryUSB 中断 IN URB；不需要串口命令。

SDK 的公开 MUSB HCD 接口没有循环 URB 或“永久覆盖同一个 8 字节缓冲区”的模式。为了不修改协议栈，本工程使用 8 MiB PSRAM 缓冲区承载一个标准长 URB，并且不读取、不解析该缓冲区。理论上在 8,000 份/秒、每份 8 字节时，一个 URB 可持续：

```text
8 MiB / (8000 × 8 B/s) = 131.072 s
```

只要目标设备仍保持有效连接，当前 URB 无论因缓冲区填满、短传输或传输错误而结束，应用线程都会提交下一笔长 URB，但不在本地分类或统计结束原因。正常填满边界不是无缝循环接口，可能产生额外间隔。正式抓包窗口应短于 120 秒，并避开该边界。



## 提交时的SDK信息
SDK branch: main
SDK commit: d0f103b75e4ad397f9b82b1d3d9bdd2bdb1b61d6
SDK version: v2.5.0
Build target: sf32lb58-lcd_a128r32n1_dsi_hcpu