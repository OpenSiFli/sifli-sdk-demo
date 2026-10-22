# take_photo_to_screen

SF32LB57x 上的摄像头示例：默认只做采集、拍照和写卡；LCD 回显是可选功能，由
`CONFIG_CAMERA_APP_PREVIEW` 控制。

> **默认构建是拍照存储模式**：不打开 LCD，`src/display/*.c` 不参与编译，因此不会
> mux LCD 引脚。按 KEY1 或控制台 `camera_photo` 拍照，照片写到 SD 卡根目录。
>
> 要恢复回显，把 `project/proj.conf` 里的
> `# CONFIG_CAMERA_APP_PREVIEW is not set` 改成 `CONFIG_CAMERA_APP_PREVIEW=y` 后重编。
> 回显路径不再绑定某一块面板：`project/proj.conf` 现在选择和 `ai_camera` 相同的
> 1024×600 HTM H070A20 RGB DPI 屏；换回 390×450 的 CO5300 只需改回 LCD 选项，
> 显示代码不需要改。

预览固定按 **640×480（30 万像素）** 采集，屏幕上怎么显示由面板尺寸决定，不需要改代码：

- **所有面板**：640×480 帧按 1:1 居中贴到整屏黑底上（1024×600 时位置为 (192,60)），
  不做缩放；帧比面板大时只做居中裁剪，仍然不缩放。
- **双整屏缓冲**：两块 1024×600 RGB565 画布放在 PSRAM2 的 `frambuf` 段，CPU 写一块、
  LCDC 扫另一块，和 `ai_camera` 的 LVGL 双画布结构一致。
- RAMLESS 屏（当前 DPI）必须整帧提交；有 GRAM 的屏也走同一套整屏提交，逻辑只有一条。
- 面板尺寸由 LCD 驱动在 `camera_display_open()` 里读取；旋转和 EPIC 缩放已从预览路径
  移除，`camera_preview` 现在只保留字节序开关。

摄像头只通过 `camera_framework` 的 handle 层使用：传感器、通信形式、采集方式和
图像后处理全部在 menuconfig 里选择，工程本身不带传感器驱动。可选组合：

- `GC032A`：`8-bit DVP（并行）` 或 `2-bit SPI（串行）`
- `OV2640`：`8-bit DVP（并行）` 或 `SPI（ArduCam FIFO 模组）`；选后者还要挑 `SPI1`/`SPI2`
- `BF30A2`：固定 RGB565 / 240×320（SPI），菜单里挑 `SPI1`/`SPI2`

软件 ISP 与 JPEG 编解码由框架持有（`camera_framework/software/{isp,jpeg}`，两个核
共用同一份源码），可分别配置运行在 HCPU 或 ACPU 上；本工程只保留 HCPU/ACPU 胶水
（`components/camera_software/example/acpu/`）。
```sh
source /path/to/SiFli-SDK/export.sh
scons -C project --board=dpi-hdk_lb57eybbn6_n16_qspi -j8
```
构建成功只说明固件能生成，采集、拍照和写卡闭环仍需实板验收。

## 配置

`project/proj.conf` 只使能 Camera 1；型号与通信形式改由板级 `Kconfig.board` 的
`Camera model` / `Camera interface` 选择（dpi 板默认 OV2640 + DVP，lcd 板默认
GC032A + DVP）。DVP/2-bit SPI 与
MCLK 的引脚复用仍由开发板自己的 `bsp_pinmux.c`（`BSP_USING_DCMI` 块）完成，
`project/Kconfig.proj` 不再定义引脚菜单：

- 板级 `Camera model`：选择传感器
- 板级 `Camera interface`：通信形式，随传感器变化
  （OV2640/GC032A：`8-bit DVP（并行）` 或 `SPI/2-bit SPI`；BF30A2：`SPI1`/`SPI2`；
  选 ArduCam 时额外挑 `SPI1`/`SPI2`）
- `Camera software ISP` / `Camera software JPEG`：是否使用软件 ISP / 编解码，以及
  各自运行在 HCPU 还是 ACPU
- `BSP_USING_CAMERA1` 只表示 Camera 1 座子有相机，不再 `select` SDK 自带的
  GC032A 驱动；`camera_framework` 由 `customer/peripherals/camera/sf-pkg.yaml`
  在 `BSP_USING_CAMERA1=y` 时自动拉取

### 回显开关与拍照存储

`project/proj.conf` 目前是拍照存储构建：

```conf
CONFIG_BSP_USING_SDMMC1=y
CONFIG_SDMMC1_BUS_WIDTH_1_ONLY=y
# CONFIG_CAMERA_APP_PREVIEW is not set
CONFIG_CAMERA_KEYS_USE_ADC_KEYS=y
```

- `CAMERA_APP_PREVIEW=n`：不打开 LCD，`src/display/*.c` 不参与编译，`camera_app_start()`
  不会调用 `camera_display_open()`，因此不会重新 mux LCD 的 DPI 引脚。拍照走
  `camera_app_save_still_photo()`（OV2640 传感器 JPEG，UXGA 1600×1200），或
  `camera_app_save_photo()`（无传感器 JPEG 的型号，编码当前帧）。
- 要恢复回显：把 `# CONFIG_CAMERA_APP_PREVIEW is not set` 改成
  `CONFIG_CAMERA_APP_PREVIEW=y`，重新编译。`src/SConscript` 会用同一个符号决定是否
  把 `display/*.c` 加回构建。
- 面板在 `project/proj.conf` 里选：当前是
  `CONFIG_LCD_USING_HTM_H070A20_RGB_A01=y`（1024×600 RGB DPI，与 `ai_camera` 相同），
  同时关闭 `CONFIG_LCD_USING_TFT_CO5300`。显示层只从 LCD 驱动读取面板尺寸和格式，
  不在代码里写死分辨率。
- `CAMERA_KEYS_USE_ADC_KEYS=y`：这块 `sf32lb57-lcd_n16r8` 板的 KEY1..KEY3 是 PA34 上的
  电阻分压 ADC 按键；设成 0 时使用普通 GPIO 按键（只适合按键直接接 GPIO 的载板）。
- TF 卡走 SDMMC1（`sd0`），所以打开 `CONFIG_BSP_USING_SDMMC1=y` 和
  `CONFIG_SDMMC1_BUS_WIDTH_1_ONLY=y`；如果载板没有 TF 卡，`camera_app_start()` 会打印
  `no sd card`，拍照请求会被拒绝。

如果实板是 GC032A 2-bit SPI，把 `project/proj.conf` 里的三行换成：

```conf
CONFIG_BSP_USING_CAMERA1=y
CONFIG_SENSOR_USING_GC032A=y
CONFIG_CAMERA_GC032A_INTERFACE_SERIAL_2BIT=y
```

## 接线（`spi-hdk_lb575ybbn6_n16`）

GC032A 2-bit SPI（DCMI SPI 包模式；引脚由开发板 `bsp_pinmux.c` 的 `BSP_USING_DCMI` 块复用）：

| GC032A 模块引脚 | 开发板 | 说明 |
| --- | --- | --- |
| IIC_SDA | PA20 | SCCB / I2C1 SDA |
| IIC_SCL | PA27 | SCCB / I2C1 SCL |
| MCLK | PA39 | 24 MHz XCLK（GPTIM2_CH1） |
| SPI_CLK | PA40 | DCMI_CLK，串行时钟 |
| SPI_D0 | PA41 | DCMI_DI0 |
| SPI_D1 | PA43 | DCMI_DI1 |

2-bit 串行的包格式（每行字节数、包标识、同步字、采样沿、数据线顺序）是传感器协议，
现在写在 `core/driver/gc032a/gc032a.c` 的 `s_serial_2bit` 常量里，运行时下发给 DCMI
后端；接线顺序不一致时改那个常量，不再有菜单项。

OV2640（DVP，硬件 DCMI）默认引脚：VSYNC=PA37、HSYNC=PA38、PCLK=PA40、
D0..D7=PA41/43/44/42/48/47/46/45、XCLK=PA39，SCCB 同上。OV2640 的 ArduCam FIFO 与
BF30A2 走 SPI，SPI 实例在 `Data bus settings` 里选，引脚由板级复用。
```text
camera: captured=.. displayed=.. dropped=.. failed=.. fps=.. period=.. copy=.. display=.. ms
camera: isp n=.. acpu=.. hcpu=.. ipc=.. ms
camera: stage n=.. dclean=.. blend=.. wait=.. push=.. ms
```
`captured/displayed/dropped/failed` 每 5 秒统计一次；`period` 是采集周期上限，`copy`
是每帧复制耗时，`display` 是刷屏耗时（软件 ISP 下 `display == isp + write`，`write`
为画布拷贝加面板提交）。`stage` 行把刷屏拆成 dclean / blend / wait / push：
`blend` 是 CPU 行拷贝时间，`wait` 是等空闲画布的时间，`dclean` 是整屏缓存写回，
`push` 是异步提交本身；整屏交换在另一块画布上进行，不阻塞下一帧。`camera: isp n=..` 把一次 ISP 的 HCPU 墙钟、ACPU 侧耗时和 IPC 差值分开给出。

## 按键与命令

- KEY1（PA34，ADC 分压）：拍照
- `camera_photo`：排队拍一张；相机未运行时打印 `photo ignored, camera is not running`
- `camera_stats [window_ms]`：按窗口统计帧数、ISP/拍照耗时与双核占用
- `camera_preview [rotate 0|1] [background 0|1] [swap 0|1]`：仅在
  `CONFIG_CAMERA_APP_PREVIEW=y` 时可用；CPU 拷贝路径只保留第 4 个参数 `swap`，
  `rotate`/`background` 不再参与预览

### 关于 ADC 按键

`button_bind_adc_button()` 会把绑定按钮自己的 GPIO 回调替换成 ADC 组的回调，所以
一条按键不能同时走 GPIO 和 ADC 两条路。`sf32lb57-lcd_n16r8` 板的 KEY1..KEY3 是
PA34 上的电阻分压按键，因此 `project/proj.conf` 默认：

```conf
CONFIG_CAMERA_KEYS_USE_ADC_KEYS=y
```

如果载板的 KEY1 是直接接 GPIO 的普通按键，把这个选项关掉（或设成 n），
`camera_keys.c` 就会走 GPIO 路径。不要把电池检测通道当成按键分压的载板设成 y，
否则会读到电池电压而不是按键。

## 拍照与文件

- 传感器自带 JPEG 编码器（OV2640）：拍照时把传感器切到它支持的最大 JPEG 分辨率
  （**UXGA 1600×1200**，全 2 MP），存的就是传感器自己压出来的那张 JPEG。
- 保留的那一帧来自**一次连续接收**：先收 9 帧预热丢掉，第 10 帧留下写卡，所以拍照缓冲
  要能放下 10 帧。DCMI 一置位就开始搬数据，缓冲区开头可能是置位前就在传的那一帧的尾巴，
  因此后端不是从 offset 0 数帧，而是在整段接收数据里找**最后一个完整的 JPEG**（SOI…EOI
  能完整解析的那一张），把它搬到缓冲区开头再返回。
- 因为拍照缓冲借用了预览的双 DMA 缓冲，**拍照前必须把 stream 和 capture 两条通道都停掉**
  （`camera_stop_stream()` + `camera_stop_capture()` 都返回成功），否则 DCMI 可能仍按上次
  预览的 DMA 目标写入：看起来收满了、字节数也正常，但缓冲里其实还是预览像素。
- 只有 RGB565 的传感器（GC032A、BF30A2）：编码当前预览帧，分辨率与预览相同。

照片写到 SD 卡根目录：`/IMG_0001.JPG`、`/IMG_0002.JPG`……（SDMMC2 枚举为 `sd1`，
挂载在 `/`），写卡前后校验 JPEG 的 SOI/EOI 标记。每张照片打印两行，软件编码路径的
格式为：

```text
camera: photo /IMG_0001.JPG <bytes> B <w>x<h> encode=<ms> write=<ms> ms
camera: photo cpu hcpu=<%> acpu=<%> codec=<hcpu|acpu|sensor>
```

传感器 JPEG 路径把中间的耗时项换成 `setup=` / `capture=` / `frames=` / `write=`，
`frames=` 是这次连续接收的帧数（预热 + 保留），`capture=` 是整段接收的时长。整条路径
失败时打印 `camera: still capture failed status=… bytes=…`。
```sh
source /path/to/SiFli-SDK/export.sh
scons -C camera_framework/examples/isp/project \
      --board=spi-hdk_lb573ub7n6_hcpu
scons -C camera_framework/examples/jpeg/project \
      --board=spi-hdk_lb573ub7n6_hcpu
scons -C camera_framework/examples/acpu/project/hcpu \
      --board=spi-hdk_lb573ub7n6_hcpu
```
ISP 和 JPEG 例程烧录后分别输出 `status=PASS`；ACPU 例程需要双核板，并同时生成 HCPU
与 ACPU 镜像。详细 API、Kconfig 选项和内存约束见
`camera_framework/software/isp/README.md` 与
`camera_framework/software/jpeg/README.md`。

## 颜色与方向

`RGB565_BE` 是后处理与显示使用的 16-bit 像素格式。每个摄像头的字节序、显示交换与
EPIC 旋转/背景层集中在 `src/camera/camera_profile.h`：OV2640 低字节在前（面板原生），
GC032A 与 2-bit SPI 高字节在前（软件 ISP 的 gamma 曲线也按它标定），旋转与背景层默认
关闭。预览路径已经改为 CPU 行拷贝，不再使用 EPIC 旋转/双层混合；换摄像头后先用
`camera_preview 0 0 1` 在实板上确认颜色，再把结论记回该头文件。

## 屏幕尺寸

采集仍按"一帧 VGA"配置，显示缓冲由 `project/proj.conf` 选中的面板决定
（`LCD_HOR_RES_MAX` / `LCD_VER_RES_MAX`）。`camera_display_open()` 还会从 LCD 驱动
读回实际尺寸和格式。

| 项目 | 行为 |
| --- | --- |
| 几何 | 640×480 原尺寸居中，1024×600 时位于 (192,60)；帧比面板大时居中裁剪，不缩放 |
| 画布 | 2 块整屏 RGB565，位于 PSRAM2 的 `.bss.l2_non_ret_bss_frambuf` |
| 合成 | CPU 按行 `memcpy` 到当前画布，不经过 EPIC |
| 提交 | `set_window()` 整屏 + `draw_rect_async()` |
| 换帧 | `tx_complete` 释放画布；CPU 可以同时写另一块画布 |

这版直接采用 `ai_camera` 的关键结构：**双整屏 + CPU 拷贝 + 异步整层提交**，不再每帧
跑 EPIC 缩放。EPIC 的 1024×600 缩放开销正是之前 `wait=41ms` 的来源。

启动日志：

```text
camera: panel 1024x600 fmt=rgb888 align=1, buffer 1024x600 (1228800 bytes x 2)
camera: cfg 640x480 rgb565 ...
camera: preview started 640x480
```

预览画布和 LCD 层始终是 RGB565：即使面板本身是 RGB888，也只通过
`RTGRAPHIC_CTRL_SET_BUF_FORMAT` 告诉 LCDC 按 RGB565 读取这一层，由 LCDC 转换成面板
输出格式。

### 内存约束

- PSRAM1 的 `.RW_PSRAM_NON_RET`：2,457,600 B 相机帧（DCMI 双帧 + 稳定双帧）+
  393,216 B 图像处理 = `0x2EE000`。
- PSRAM2 的 `frambuf`：2 × 1,228,800 B 预览画布 = `0x258000`。

原来 640×480 的 JPEG 解码缓冲已经移除：当前 OV2640 路径输出 RGB565，预览不经过
JPEG 解码。若以后要接 JPEG-only 传感器，需要单独增加解码缓冲；PSRAM1 现在还有空间，
但不要把解码缓冲再塞回 `camera_display.c` 的 `frambuf` 段。