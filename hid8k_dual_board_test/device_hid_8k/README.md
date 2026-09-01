# SF32LB58 USB HID 8 kHz 设备板

本工程将 SF32LB58 NAND 开发板配置为 USB 2.0 High-Speed HID 设备。

## USB 属性

- VID/PID：`38F4:FFFF`
- HID subclass/protocol：`0/0`
- Interrupt IN：`EP 0x81`
- 最大包长：8 字节
- High-Speed `bInterval=1`
- 报告：4 字节鼠标字段 + 32 位小端递增序号

## 执行路径

`SET_CONFIGURATION` 完成后，设备调用一次 `usbd_ep_start_write()` 装入首包。每次 EP1 IN 成功完成后，端点完成回调立即调用同一接口装入下一包。发送节奏由主机 IN 令牌决定。


## 编译和烧录

```powershell
scons --board=sf32lb58-lcd_a128r32n1_dsi_hcpu -j8
```
