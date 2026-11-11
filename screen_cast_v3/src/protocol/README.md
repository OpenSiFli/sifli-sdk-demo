# screen_cast — 投屏接收服务

`screen_cast` 是本例程自带的**投屏接收组件**，位于 `src/protocol/`：
把手机 App 推送的实时画面流（地图导航、相机预览、屏幕镜像等）解协议、解复用为
可消费的帧，供应用层渲染。

它不依赖任何具体 App 或界面，只依赖 `middleware/framed_proto` 与 LVGL 的 `app_mem`
内存接口（`CONFIG_PKG_USING_LITTLEVGL2RTT`）。

## 分层

```
手机 App
        │  BLE serial-transmission / SPP / PAN / WiFi，category 0x01
        ▼
middleware/framed_proto/     ← 协议组件：分帧、收发、超时监督、响应构造
        │                   framed_proto_handler_t::on_msg
        ▼
screen_cast_proto.c      ← 消息分发与应答（40/43/45/47/51/53/59/36）
        ▼
screen_cast_proc.c       ← 帧解复用 + 每类型环形缓冲（JPEG / MP3 / EZIP / NAV_INFO / CAMERA_INFO）
        ▼
src/screen_cast_ui.c     ← 应用层，只负责渲染
```

协议组件不在本组件内：`middleware/framed_proto` 只负责把消息搬进搬出，不理解业务含义；
本组件负责解析具体消息并组织应答。配置上依赖 `CONFIG_FRAMED_PROTO`。

| 文件 | 职责 | 是否可复用 |
|------|------|-----------|
| `screen_cast_proc.c/.h`  | 帧解复用、环形缓冲、帧获取/释放、会话状态 | 是（与传输无关） |
| `screen_cast_proto.c`    | 消息分发与应答 + 连接二维码载荷生成 | 是（默认绑定） |
| `screen_cast.h`          | 统一对外头文件 | — |

## 协议说明

传输帧（serial-trans payload 内部）：

```
[u16 message_id (LE)][u16 length (LE)][data ...]
```

涉及的 OTA-V3 报文（`framed_proto_msg_id_t` 子集）：

| id | 报文 | 方向 |
|----|------|------|
| 40 / 41 | VERSION_REQUEST / RESPONSE | 手机 → 设备 / 应答 |
| 43 / 44 | FILE_INIT_REQUEST_EXT / RESPONSE_EXT | 握手 |
| 45 / 46 | FILE_INIT_COMPLETED_REQUEST / RESPONSE | 握手 |
| 47 / 48 | FILE_START_REQUEST_EXT / RESPONSE_EXT | 每帧开始 |
| 59 | FILE_CONTINUAL_PACKET | 帧数据流（核心） |
| 60 | FILE_CONTINUAL_PACKET_ERROR | 错误 |
| 61 | FILE_CONTINUAL_PACKET_MODE_CHANGE | 应用反向请求（拍照/录像等） |
| 50 | FILE_PACKET_RESPONSE_EXT | 应答 |
| 51 / 52 | FILE_END_REQUEST_EXT / RESPONSE_EXT | 每帧结束 |
| 53 / 54 | FILE_TOTAL_END_REQUEST_EXT / RESPONSE_EXT | 会话结束 |
| 56 / 57 | CANCEL_REQ / RSP | 取消 |

`FILE_CONTINUAL_PACKET` 载荷：

```
[u32 file_length][u32 index][u16 data_length][u8 type][u8 reserved][data ...]
```

- `type`：`0=WHOLE, 1=START, 2=MIDDLE, 3=ENDING`
- `reserved`：`bit0=need_rsp`，`bit1=new_packet`
- `index` 每帧从 1 开始；`file_length` 为该帧总长度

帧数据本身有两条路径（自动判别）：

1. **带协议**：前 10 字节 `[u16 type][u32 timestamp][u32 total_len]`；
2. **不带协议**：按魔数判别 JPEG（`FF D8 FF`）/ MP3（`ID3` / `RIFF`），否则视为 `NAV_INFO`。

`NAV_INFO` 帧内容：`[u32 json_len][json][u32 pic_len][pic(ezip)]`，
JSON 字段：`roadName / distance / speed / remainTime / remainDistance / remainLights / turnIconChanged / turnIconName`。

## 配置

```
CONFIG_FRAMED_PROTO=y                # 必选：分帧、收发、超时监督、响应构造
CONFIG_USING_SCREEN_CAST=y           # 投屏组件（depends on FRAMED_PROTO && PKG_USING_LITTLEVGL2RTT）
CONFIG_USING_SCREEN_CAST_WIFI=y      # 可选：WiFi（P2P GO + TCP）通道
```

以上开关默认已在 `project/proj.conf` 中配置，WiFi 相关开关默认注释掉。

## 应用接口

```c
#include "screen_cast.h"

screen_cast_init();                                    // 应用启动时调用一次

sc_frame_t *jpg = sc_frame_acquire(SC_DATA_TYPE_JPEG); // 取一帧（无则返回 NULL）
if (jpg) {
    render(jpg->data, jpg->size);
    sc_frame_release(SC_DATA_TYPE_JPEG);               // 释放该类型当前使用中的帧
}

sc_map_type_t mode = sc_get_map_type();                // SC_MAP_TYPE_FULL_MODE / NAV_MODE
sc_set_switch_mode(SC_MAP_TYPE_TAKE_PICTURE);          // 反向请求手机拍照
```

> `sc_frame_acquire()` / `sc_frame_release()` 按类型配对使用：每个类型同一时刻只有一个
> 帧处于 `USING` 状态。

### 手机连接（二维码）

手机 App 没有搜索/连接蓝牙的能力，只能由设备把 BLE 地址做成二维码显示在屏幕上，
App 扫码后直接按地址回连：

```c
char payload[128];
sc_build_connect_qr(payload, sizeof(payload), 832, 480, SC_CAST_TYPE_BLE);
// -> "Tt:1;Mac:a0b1c2d3e4f5;Ql:0.2;Sz:832x480;Tp:1"
sc_qr_set_text(qr_obj, payload);   // qr_obj 由 src/sc_qr.c 的 sc_qr_create() 创建
```

| 字段 | 含义 |
|------|------|
| `Tt` | 传输类型位掩码，`SC_CAST_TYPE_*`（`1` = BLE） |
| `Mac` | 设备 BLE 公网地址，12 位小写无分隔 |
| `Ql` | 请求手机编码的 JPEG 质量 |
| `Sz` | 请求的分辨率（建议对齐到 16） |
| `Tp` | 导航信息版本 |

### 会话状态

```c
if (!sc_is_casting()) { /* 显示扫码页 */ }

screen_cast_notify_disconnect();   // BLE 断开时调用，结束后回到扫码页
```

`sc_is_casting()` 在 `sc_proc_file_start()` 置位，`sc_proc_session_end()` / `sc_proc_err()`
清零。

## 命令行

```
sc_cast log <0|1>            # 帧率统计日志开关
sc_cast dump <0|1>           # 每帧输出大小与 CRC32
sc_cast raw [type] [count]   # 导出原始帧（0=JPEG 1=MP3 2=EZIP 3=NAV_INFO 4=CAMERA_INFO）
sc_cast rsp_freq <n>         # 调整应答频率（下次会话初始化时生效）
sc_cast mode <n>             # 反向请求模式：3=拍照 4=开始录像 5=停止录像
```

## 可移植性

- 换渲染：直接使用 `screen_cast_proc.h` 的帧 API，或替换 `src/screen_cast_ui.c`。
- 换传输（SPP / PAN / WiFi）：上层接口不变，只需在 `screen_cast_proto.c` 中按需应答
  新的 OTA-V3 事件，并把二维码的 `Tt` 字段改成对应位掩码。WiFi 通道的实现见
  `src/wifi_tran/ota_wifi_tran.c`。
