# camera_57x

`camera_57x` 是 SF32LB57x SPI HDK 的摄像头 Demo。当前使用带 FIFO 的
Arducam OV2640，通过 SPI2 读取 JPEG、PA34 ADC 快门控制，并通过
J4（SDMMC2 / `sd1`）保存照片。

图像后处理使用工程内的 `components/camera_software` 组件包：ISP、JPEG
编解码和可选的 HCPU/ACPU 适配器可以独立配置和验证。当前默认应用仍走
OV2640 FIFO-SPI JPEG 预览路径。当前没有 OV2640 DVP 并行采集后端，也没有
ArduChip FIFO 直读 RGB565 预览；这两条未完成、未实板验证的路径，以及输入对比
和调参分支均不纳入默认工程。

目标板：`sf32lb57-spi-hdk_n16r4`。

本工程当前固定在本机完整 SiFli SDK 提交：
`e72ab9e761caebe707e95a06e613f1872301e9df`。

SDK 中的可编译板名为 `spi-hdk_lb573ub7n6`，它对应本项目所称的
`sf32lb57-spi-hdk_n16r4` 硬件配置。

## 构建

```sh
source /path/to/SiFli-SDK/export.sh
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

## 软件组件独立验证

组件例程不依赖摄像头、显示屏或 SD 卡输入，使用内存中的确定性图像快速验证
ISP、JPEG 和双核适配器。加载 SDK 环境后，在对应目录执行：

```sh
source /path/to/SiFli-SDK/export.sh
scons -C camera_57x/components/camera_software/isp/example/project \
      --board=spi-hdk_lb573ub7n6_hcpu
scons -C camera_57x/components/camera_software/jpeg/example/project \
      --board=spi-hdk_lb573ub7n6_hcpu
scons -C camera_57x/components/camera_software/example/acpu/project/hcpu \
      --board=spi-hdk_lb573ub7n6_hcpu
```

ISP 和 JPEG 例程烧录后分别输出 `status=PASS`；ACPU 例程需要双核板，并同时
生成 HCPU 与 ACPU 镜像。详细 API、Kconfig 选项和内存约束见
`camera_57x/components/camera_software/README.md`。

### RGB565 与 OV2640 输入路径

此工程中的 `RGB565_BE` 是后处理与显示使用的 16-bit 像素格式。当前默认 OV2640
路径为“SPI FIFO 中的 JPEG -> 软件解码 -> RGB565_BE -> ISP/EPIC/LCD”。它不等同于
OV2640 直接输出 RGB565。

后者原计划由 OV2640 输出 VGA RGB565，再通过 ArduChip 的 SPI FIFO 固定长度读取；
它也不是 DVP。真正的 DVP 是传感器到 SoC DCMI 的并行数据接口，本工程尚未提供
OV2640 的该接口后端。menuconfig 仅显示不可用状态，不能选择这些路径。
