# `ai_camera` 交接文档

更新时间：2026-08-28

本文档面向下一位继续维护 SF32LB573 拍学机原型的工程师。代码目录是
`ai_camera/`，目标板是 `spi-hdk_lb573ub7n6`。

## 当前 Git 状态

- 收敛后的修复提交：`9a7f009 fix(camera): harden UI storage and voice runtime`
- 当前工作分支正在改名为：`ai-camera`
- 目标分支：`main`
- Draft MR：`https://gitlab.sifli.com/sw/sifli-sdk-demo/-/merge_requests/11`
  （源分支 `ai-camera`，目标分支 `main`）
- 旧 MR !10 已关闭；改名前的远端分支 `codex/camera-runtime-stability` 已删除。
- 工作区中 `camera_57x/`、`findings.md`、`progress.md`、`task_plan.md` 等有其他未提交改动；不要在整理本分支时加入或回退这些文件。

## 已完成内容

### 拍照和存储

- `fs_root` 使用实际块设备几何信息并打印扇区大小、挂载和格式化结果。
- FatFs 最大扇区大小改为 4096，解决原日志中的
  `The sector size of device is greater than the sector size of FAT`、
  `format error, result=1` 和 `mount fs_root failed`。
- 拍照失败显示为“照片存储失败”，不再误报为网络错误。
- 照片保存到 `/photo/latest.jpg`，并生成递增编号照片。
- Arducam OV2640 SPI FIFO 路径保持为当前静态 JPEG 验证后端。

### UI 和中文字库

- UI 改为按实际屏幕宽高计算位置，避免固定坐标导致标题、状态区和底部按钮偏移。
- 关闭 SDK 默认稀疏 SimSun CJK 字库，加入 `device_ui_font.c` 的中文子集和
  FontAwesome `U+F001` 图标，覆盖当前页面所需字符。
- 清理旧的点阵对象，避免第二次按键后 LVGL 内存池耗尽。

### XiaoZhi 音频和 WebSocket

- Opus 工作线程、环形缓冲区、消息队列和事件对象的生命周期集中管理。
- 下行播放缓存调整为 4096，避免 `audio_client_init` 的 `ring_pool` 断言。
- WebSocket/TCPIP 回调不再直接做音频资源释放或日志输出；断线清理由系统
  workqueue 执行，规避 `ulog.output_lock` 断言和回调上下文中的并发释放。
- 增加下行 PCM 峰值日志，例如：
  `xiaozhi: downlink frame=25 packet=206B pcm=2880B peak=...`。
- 本地音乐通道私有音量设为 15。

## 使用方法

### 首次烧录和开机

- 使用 `ai_camera/project/build_spi-hdk_lb573ub7n6_hcpu/sftool_param.json`
  生成的镜像集烧录，然后上电；不要只烧录 `main.bin`。
- 屏幕初始显示“等待网络”。

### 手机配网

- 手机开启“蓝牙网络共享”，在蓝牙设置中与开发板配对。
- 配对成功且 `b0` 拿到 IPv4 后，开发板会自动连接小智服务，屏幕依次显示
  “正在连接”→“随时准备好”。
- 如果一直停留在“等待网络”或“蓝牙网络”，先确认手机共享已开启、PAN 已连接。

### 语音对话（按住说话）

- 在“随时准备好”页面，按住语音气泡（“你好，小智”）或实体 KEY1 说话，
  屏幕显示“我在听”。
- 松开手指后发送语音，屏幕先显示“正在思考”；服务端回复时显示“正在回复”，
  播报完成后回到“随时准备好”。
- 只有首页状态支持按住说话，其他状态不响应录音。

### 拍照

- 点击 PHOTO 卡片或实体 KEY2 拍照：屏幕显示“正在拍照”，完成后照片保存到
  `/photo/latest.jpg`，同时生成递增编号照片。
- 若已协商到小智视觉能力，拍照后会通过服务端提供的 URL 自动上传识别。
- 保存失败时屏幕显示“照片存储失败”，可重试；只有首页状态支持拍照。

### 其他按键

- 左上角首页图标（KEY3）：单击回到首页。
- 长按 KEY3：显示“蓝牙网络”状态页，用于检查网络状态。

## 历史问题（已解决）

用户现场日志已经证明服务端下行 Opus 包能够收到并解码为 2880 字节 PCM，
曾出现的“回复时没有声音”问题已在实板闭环确认解决。若后续回归，不要只看
WebSocket 包计数，按下面顺序记录完整日志：

1. `xiaozhi: speaker opened ... Hz` 是否出现；若没有，检查 `audio_open()` 返回值、
   音频参数和音频服务初始化。
2. `downlink ... pcm=... peak=...` 的 `peak` 是否大于 0；若为 0，问题在服务端
   音频内容、Opus 协商或解码参数。
3. 是否出现 `speaker write short`；出现时检查播放缓存、I2S/DAC 时钟和音频线程
   调度。
4. 确认功放使能脚 PA51、扬声器连接和板级音频路由；可先运行已有的
   `ai_audio_loopback` 验证本地播放链路。
5. 连续按键验证：第一次按住 KEY1 录音并松开，等待 TTS 完成后再次操作；确认不再
   出现 `ring_pool`、`output_lock` 或线程 fatal error。

如果需要进一步定位，优先在 `ai_camera/src/xiaozhi_audio.c` 的
`ai_xiaozhi_audio_open_speaker()` 和 `ai_xiaozhi_audio_write_speaker()` 增加一次性
参数/返回值日志，不要把日志写回 WebSocket 或音频回调。

## 构建和测试

先加载 SDK 环境：

```sh
source /Users/wangmian/SiFli-SDK/main/export.sh
```

主机测试：

```sh
sh ai_camera/tests/run_host_tests.sh
```

板级构建：

```sh
python3 /Users/wangmian/SiFli-SDK/main/tools/sdk.py \
  -C ai_camera/project build --board=spi-hdk_lb573ub7n6
```

当前构建产物：

`/Users/wangmian/Fixes/sifli-sdk-demo/ai_camera/project/build_spi-hdk_lb573ub7n6_hcpu/main.bin`

最近一次产物大小为 1,591,376 字节。烧录时使用同目录生成的
`sftool_param.json` 镜像集合，不要只单独烧录 `main.bin`。

## 常用串口命令

```text
ai_audio_loopback 10
ai_xiaozhi_activation
ai_xiaozhi_connect
ai_xiaozhi_listen_start
ai_xiaozhi_listen_stop
ai_camera_capture
```

验收顺序建议为：手机蓝牙网络共享和 PAN/DHCP -> XiaoZhi 激活与连接 -> 本地
环回播放 -> XiaoZhi 录音/回复 -> Arducam 拍照和 `/photo` 文件检查。

## 代码入口

- `ai_camera/src/device_ui.c`：屏幕布局、状态文案和按钮事件。
- `ai_camera/src/device_ui_font.c`：当前 UI 中文和图标字库；授权说明见同目录
  `device_ui_font.LICENSE.txt`。
- `ai_camera/src/camera_capture.c`：文件系统挂载、拍照保存和视觉上传触发。
- `ai_camera/src/xiaozhi_audio.c`：Opus 编解码、音频客户端和播放写入。
- `ai_camera/src/xiaozhi_websocket.c`：连接、文本帧、下行音频入队和断线 workqueue。
- `ai_camera/tests/`：策略测试、状态机测试、字库覆盖测试和音频格式测试。

## 交接注意事项

- 只提交 `ai_camera/` 和本交接文档范围内的改动，除非明确接手
  `camera_57x` 的另一项工作。
- 修改 UI 文案时同步检查 `test_device_ui_font.c`，确保新增中文字符有字形。
- 修改音频资源释放路径时必须重新跑主机测试和板级构建，并在第二次录音/播放
  循环后观察线程、互斥锁和内存信息。
- MR !11 目标保持 `main`，当前为 Draft；分支名使用精确的 `ai-camera`。
