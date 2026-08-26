# SF32LB573 AI Camera

SF32LB573 拍学机原型工程。当前阶段包含 RT-Thread 应用协调、蓝牙 PAN、模拟麦克风环回，以及小智的激活、WebSocket、Opus 音频和 MCP 视觉上传协议；摄像头、DVP、SD 卡和外置 Wi-Fi 尚未接入。

## Build

```sh
source /Users/wangmian/SiFli-SDK/main/export.sh
scons -C ai_camera/project --board=spi-hdk_lb573ub7n6 -j8
```

## M1 board acceptance

1. 烧录 `project/build_spi-hdk_lb573ub7n6_hcpu/sftool_param.json` 生成的镜像集。
2. 在 Android 手机开启“蓝牙网络共享”，并完成与开发板配对。
3. 串口依次确认以下日志：

   ```text
   learning: Bluetooth stack ready
   learning: pairing complete, PAN connect scheduled
   learning: PAN profile connected, waiting for DHCP
   learning: PAN ready, b0 has IPv4
   ```

4. 执行 `ifconfig`，确认 `b0` 已分配非零 IPv4 地址。
5. 关闭手机蓝牙网络共享，确认日志出现有限次数的 `learning: PAN retry n/5`；重新开启共享后，确认再次出现 `PAN ready`，不需要重启开发板。

构建成功只证明目标固件生成；配对、PAN、DHCP 和断线恢复必须以上述实板日志验收。

## M2 audio loopback

本板使用专用模拟麦克风输入和 PA51 控制的板载功放。烧录后，在串口执行：

```text
ai_audio_loopback
```

该命令以 16 kHz、16-bit、单声道运行 10 秒，将默认 MIC0 的音频回送至扬声器；可传 `1`～`60` 指定秒数。验收时记录是否听到连续人声、是否有爆音或啸叫，以及串口中 `sifli_aw8155_start` / `sifli_aw8155_stop` 的功放控制日志。

## M3 XiaoZhi activation and WebSocket

M1 已确认 `b0` 获得 IPv4 后，执行：

```text
ai_xiaozhi_activation
```

该命令以本机 BLE 公共地址生成 Device-ID、以 SHA-256 派生 Client-ID，向小智激活接口请求动态 WebSocket URL 和 token。若显示绑定码，按小智平台完成绑定后重新执行命令；token 不会输出到串口。

绑定完成后执行：

```text
ai_xiaozhi_connect
```

验收成功日志为 `xiaozhi: WebSocket connected`，随后可看到服务端的文本帧和
`xiaozhi: server hello ...`。服务器协商为单声道 Opus 后，固件会建立 16 kHz、
60 ms 的麦克风上行编码通道，以及按服务端参数解码的扬声器下行播放通道。

收到 `xiaozhi: server hello ... session=...` 后，可用以下命令验证会话监听控制：

```text
ai_xiaozhi_listen_start
ai_xiaozhi_listen_stop
```

`ai_xiaozhi_listen_start` 先发送小智协议的 `listen` 文本帧，再启动 MIC0 的
Opus 上行；`ai_xiaozhi_listen_stop` 停止采集。需要通过实板确认麦克风上行与
扬声器下行的实际音频质量。

服务端发来 MCP `initialize` 视觉能力后，串口显示
`xiaozhi: vision upload capability ready`。若已有 JPEG 文件，可用：

```text
ai_xiaozhi_vision_upload <jpeg-path> [question]
```

该命令以服务端提供的 HTTP(S) URL 上传 `question` 和 `file` 两个 multipart
字段；仅当服务端提供 token 时才发送 Bearer 认证。URL 和 token 只保存在 RAM，
不会打印到串口或持久化。

## 摄像头边界

项目只预留 OV2640 的 DVP 接口，不启用 SPI-FIFO 或其他摄像头后端。当前
SF32LB573 平台的 DVP 采集尚不可用，因此拍摄预览、快门保存和照片识别上传的
硬件触发必须等待 DVP 支持及实际引脚定义完成；固件不会把该路径报告为已支持。
