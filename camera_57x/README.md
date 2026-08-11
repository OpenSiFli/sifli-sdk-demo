# camera_57x

`camera_57x` 是 SF32LB57x SPI HDK 的摄像头 Demo。当前使用带 FIFO 的
Arducam OV2640，通过 SPI2 读取 JPEG、PA34 ADC 快门控制，并通过
J4（SDMMC2 / `sd1`）保存照片。

目标板：`sf32lb57-spi-hdk_n16r4`。

本工程当前固定在本机完整 SiFli SDK 提交：
`e72ab9e761caebe707e95a06e613f1872301e9df`。

SDK 中的可编译板名为 `spi-hdk_lb573ub7n6`，它对应本项目所称的
`sf32lb57-spi-hdk_n16r4` 硬件配置。

## 构建

```sh
source /Users/wangmian/SiFli-SDK/main/export.sh
scons -C camera_57x/project --board=spi-hdk_lb573ub7n6 -j8
```

构建成功仅表示固件可生成；摄像头采集、SD 卡和显示闭环仍必须在实板验收。

## 自动预览与拍照

固件启动后会自动初始化 CO5300、Arducam OV2640 和 PA34 ADC 按键，并开始
连续预览。OV2640 输出 640×480 JPEG，SPI2 以 8 MHz 从模组 FIFO 分块读取；
当前软件 JPEG 后端解码为 RGB565，再由 EPIC 顺时针旋转、等比缩放并居中裁剪，
铺满 390×450 屏幕。解码入口保留后端接口，后续可替换为硬件 JPEGD 后端。

飞线连接为：PA37/MOSI、PA38/MISO、PA39/SCLK、PA40/CS；SCCB 使用
I2C1 的 PA20/SDA 和 PA27/SCL。

预览使用两块 FIFO 接收缓冲和两块稳定 JPEG 缓冲。显示持有一块稳定帧时，
采集线程可继续使用另一块；两个稳定槽都不可写时丢弃旧帧。
每 5 秒的统计日志同时给出帧周期、稳定帧复制和显示耗时：

```text
camera: captured=144 displayed=143 dropped=0 failed=0 fps=6.5 period=140.1 copy=26.1 display=152.7 ms
```

其中 `period` 决定采集帧率上限，`copy` 和 `display` 用于定位 FIFO、PSRAM、
JPEGD 或 LCD 路径的耗时；实板数值以实际日志为准。

短按 KEY1 会暂停预览，将 OV2640 切换到 1600×1200 JPEG，并把 FIFO 数据
分块写入 SD 卡根目录：`/IMG_0001.JPG`、`/IMG_0002.JPG`……写入前后会校验
JPEG 的 SOI/EOI 标记，保存成功或失败后都恢复 640×480 预览。KEY2/KEY3
用于浏览 SD 卡照片：KEY2 在预览和相册之间切换，进入相册时显示编号最大的
`IMG_*.JPG`；KEY3 显示下一张，末尾回到第一张。相册模式暂停摄像头采集，
屏幕底部显示文件名。当前软件后端支持基线 JPEG 的常见 4:2:2、4:2:0 和 4:4:4
采样；渐进式、损坏或超过 768 KiB 的文件显示 `UNSUPPORTED`。

当前 SDMMC2 保持实板已通过的一线模式。四线模式的 DAT1–DAT3 CRC 问题仍作为
独立硬件问题处理，不在本阶段恢复。

## 板级诊断

`diag` 只用于硬件测试。自动预览运行时，`diag camera` 和 `diag display` 会返回
`busy: camera app running`，避免与生产路径同时操作 DCMI 或 LCD。

```text
diag display
```

预期：红、绿、蓝、白四个色条覆盖 390×450 屏幕。

```text
diag adc
```

预期：日志显示 PA34/`bat1` 通道 6 的十次采样的最小、平均、最大值。分别按住
KEY1/KEY2/KEY3 重复执行，采样簇应接近 2970/2580/2190 mV，且校准窗口不重叠。

```text
diag sd
```

预期：`sd1` 挂载到 `/`，写读 `/.camera_57x_probe` 并校验内容，然后只删除该探测
文件。该板的启动文件系统只有 `/dev`，因此不能把 TF 挂到不存在的 `/sd`；该命令
绝不格式化 SD 卡。

`diag sd_stress` 会用临时文件完成 1 MiB 顺序写入、读回和逐块校验，结束时自动删除。

当前实板在 SDMMC2 四线写入时出现数据 CRC 错误，一线模式写读已通过，因此启用
`CONFIG_SDMMC2_BUS_WIDTH_1_ONLY=y`。后续单独恢复四线并检查 PA33/DAT1、
PA28/DAT2、PA29/DAT3；该问题不阻塞摄像头控制链路开发。

```text
diag camera
```

当前 `proj.conf` 选择 Arducam OV2640 后端。正式流程由开机自动预览启动，
不再通过 `diag camera` 启动。
