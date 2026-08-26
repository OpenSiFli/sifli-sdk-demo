# SF32LB573 AI Camera

SF32LB573 拍学机原型工程。当前阶段验证 RT-Thread 应用协调、蓝牙 PAN、模拟麦克风环回，以及小智的激活和 WebSocket 信令；摄像头、PDM、DVP、SD 卡和外置 Wi-Fi 尚未接入。

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

验收成功日志为 `xiaozhi: WebSocket connected`，随后可看到服务端的文本帧。当前阶段仅完成激活和 WebSocket `hello` 信令，尚未发送 Opus 音频或播放服务端音频。
