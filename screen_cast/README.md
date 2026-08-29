# BLE 投屏示例 (screen_cast)

## 支持的平台
+ sf32lb58 lcd系列

> 本例程依赖芯片内置 JPEG 硬件解码器，仅在带该模块的芯片上可用；其他系列（LB52x/LB55x/LB56x）暂不支持。

## 概述
本例程演示一个**BLE 投屏接收端**：设备作为 BLE 从设备（peripheral）广播，与
发送端（PC 脚本）连接后，PC把图片编码再通过 BLE 把一帧一帧的 **JPEG 图像实时推送**给设备，
设备收到后硬件解码并通过 LVGL 实时显示在屏幕上。

数据通路：
```
发送端(PC 脚本)
   └─ 按 serial-trans 协议分片 (PC 端 fragment 完成)
        └─ BLE 逐包传输
             └─ 设备端 serial-trans 服务自动重组成完整一帧
                  └─ serial-trans 回调 (category 0x20) 收到完整一帧
                       └─ JPEG 解码
                            └─ LVGL image 实时刷新显示
```


## 帧协议

发送端每帧的**负载**格式（4 字节头 + JPEG）：

| 字段     | 长度 | 说明                          |
|----------|------|-------------------------------|
| magic    | 2B   | 0x53C1（小端），标识投屏帧    |
| type     | 1B   | 0 = JPEG 整屏帧               |
| reserved | 1B   | 预留（对齐/扩展用）           |
| JPEG     | N    | JPEG 图像数据                 |

设备端也接受"无头的裸 JPEG"，整帧经 BLE serial transmission 传输：
发送端按协议分片、设备端 serial-trans 服务自动重组，单帧上限 64KB（总长字段为 uint16）。
控制 JPEG 质量/分辨率让单帧不超过此上限即可。

传输层分片格式（由 serial transmission service 定义，详情可见`bf0_sibles_serial_trans_service.h`,发送端需遵循）：
- 每个 BLE 包：`[category(1B)=0x20][flag(1B)][...]`
- flag：0=完整包，1=首包(后跟2B总长)，2=中间包，3=尾包

## 例程的使用

### 1. 编译
切换到 project 目录，运行：
```
scons --board=sf32lb58-lcd_n16r32n1_dpi -j8
```

### 2. 烧录
切换到例程`project/build_xx`目录，运行`uart_download.bat`，按提示选择端口即可进行下载：
```c
$ ./uart_download.bat

     Uart Download

please input the serial port num:5
```
关于编译、下载的详细步骤，请参考[快速入门](https://docs.sifli.com/projects/sdk/latest/sf32lb52x/quickstart/index.html)的相关介绍。

### 3. 启动设备
上电后设备以 **SIFLI_SCREENCAST** 名称广播，屏幕先为黑屏，等待发送端推流。

### 4. PC 端推流（发送端）
PC 脚本见 `tools/screen_cast_sender.py`，扮演"手机 APP"的角色。

安装依赖：

cd 到脚本目录，如果第一次使用没安装脚本依赖的话执行以下命令安装依赖库
```
pip install bleak pillow dxcam opencv-python numpy mss
```

用法示例：
```bash
# 把一个文件夹里的图片循环推给设备（幻灯片/测试流）
python screen_cast_sender.py --source folder --path ./imgs --fps 2 --width 800 --quality 35

# 把 PC 屏幕实时镜像到设备
python screen_cast_sender.py --source screen --fps 10 --width 800 --quality 35
```
脚本会自动扫描名为 `SIFLI_SCREENCAST` 的设备并连接；也可用 `--address` 直接指定地址跳过扫描。


## 例程的预期结果
1. 设备上电黑屏，可被 BLE 扫描到（SIFLI_SCREENCAST）。
2. PC 脚本连接后开始推流，设备屏幕实时显示推送的图像并随发送端内容变化。
3. 设备串口 log 打印每帧解码信息（`lv_jpeg_decode: jpg_data x data_size x`）。


## 更新记录
| 版本 | 日期 | 发布说明 |
|:---|:---|:---|
| 0.0.1 | 08/2026 | 初始版本 |
