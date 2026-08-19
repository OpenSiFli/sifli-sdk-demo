# SF32LB58 USB HID 8 kHz 标准长 URB 主机板

本工程将 SF32LB58 NAND 开发板配置为 USB 2.0 High-Speed Host。

## 自动执行

主机枚举 HID 设备后严格校验：

- VID/PID `38F4:FFFF`；
- High-Speed；
- HID class 3、protocol 0；
- EP `0x81`、Interrupt IN、MPS 8、`bInterval=1`。

属性匹配后，主机等待不少于 100 毫秒并自动提交一个标准异步中断 IN URB。工程没有 `hid8k_host_start`、`hid8k_host_stop` 或 `hid8k_host_status` 命令，也不解析、统计或打印报告。

## 长 URB

标准 MUSB HCD会推进 URB 的缓冲区指针。为了不修改 HCD，本工程在 PSRAM2 中分配 8 MiB 缓冲区；内容从不被应用读取。按 8,000 份/秒、每份 8 字节计算，单笔 URB 理论持续 131.072 秒。只要设备仍保持有效连接，任何结束的 URB 都由主线程重新提交，不在本地分类或统计结束原因。正常填满边界可能产生额外间隔，因此正式抓包应小于 120 秒并避开边界。


## 编译和烧录
```powershell
scons --board=sf32lb58-lcd_a128r32n1_dsi_hcpu -j8
```
## 验收

开始采集后再接入设备板。主机在有效枚举后自动工作，不执行串口命令。稳态窗口内，分析仪必须看到每个连续高速微帧恰好一组目标 `EP1 IN -> DATA0/DATA1（8字节） -> ACK`，且缺IN、NAK、STALL、Incomplete、重试和CRC错误均为0。

编译成功或URB提交成功都不等于8 kHz通过。
