# 投屏数据协议流程

> 一次投屏会话的完整报文流程（40 → 54）：报文格式、hex 示例、设备侧处理路径。
>
> - 协议组件：`middleware/proto_v3/`（分帧、收发、超时监督、响应构造）
> - 应用绑定：`src/protocol/screen_cast_proto.c`（解析消息、组织应答）
> - 帧处理：`src/protocol/screen_cast_proc.c`
> - 传输层：BLE serial-transmission（cateID = `0x01`，外层 GATT 帧 flag=0 表示单包）
>
> 配置依赖：`CONFIG_PROTO_V3`。

## 0. 流程总览

| # | 报文（手机→设备 / 设备→手机） | 设备侧入口 | 说明 |
|---|---|---|---|
| 1 | 40 / 41 | `sc_proto_version_request()` | 版本交换，拿到对端类型 |
| 2 | 42 / —（无应答） | 不处理，走 default 忽略 | 前置准备 |
| 3 | 43 / 44 | `sc_proto_file_init_request()` | 会话初始化，定发送规则 |
| 4 | 45 / 46 | `sc_proto_file_init_completed()` | 确认初始化完成 |
| 5 | 47 / 48 | `sc_proto_file_start_request()` | 数据帧开始，启动监督定时器 |
| 6 | 59 / 50·60·61 | `sc_proto_continual_packet()` | 数据洪流 + 应答 |
| 7 | 51 / 52 | `sc_proto_file_end_request()` | 本路文件结束 |
| 8 | 53 / 54 | `sc_proto_total_end_request()` | 整个会话结束 |
| 异常 | —（监督超时） | `sc_proto_on_sync_timeout()` | 手机不道别时的收尾 |

```text
screen_cast_init()
  ├─ proto_v3_set_handler(&g_sc_proto_handler)      // on_msg / on_close / on_error / on_tx_done
  ├─ proto_v3_set_sink(&g_sc_proto_sink)            // 投屏流程不读取
  └─ proto_v3_sync_set_timeout_cb(sc_proto_on_sync_timeout)

40 → 41 →（42 忽略）→ 43 → 44 → 45 → 46 → 47 → 48 → 59×N → 50×（每 N 包）→ 51 → 52 → 53 → 54
```

> 设备侧没有状态机。会话状态全部收在 `screen_cast_proto.c` 的 `g_sc` 结构里（`type` / `phone_type` /
> `file_aligned` / `remote_version` / `is_lose_check`），加上 `screen_cast_proc.c` 的帧状态。

## 1. 握手：双向版本交换（40 / 41）

### 1.1 手机 → 设备：VERSION_REQUEST（msg 40，0x28）

有效载荷 4 字节：

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 2 | `ota_version` | 手机协议版本 |
| 2 | 2 | `type` | 17 = CONTINUAL_FILE（投屏流） |

手机 v6 举例：`06 00 11 00`（版本 6 + 类型 17）

完整的一条（含外层封装）：

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `01` | cateID | 0x01 | serial-trans 类别 |
| `00` | flag | 0 | 单包（whole） |，
| `08 00` | len | 8 | 本帧总长（layer2） |
| `28 00` | msg_id | 40 | VERSION_REQUEST |
| `04 00` | length | 4 | 载荷长度 |
| `06 00` | ota_version | 6 | 手机协议版本 |
| `11 00` | type | 17 | CONTINUAL_FILE（投屏流） |

### 1.2 设备解析

- 路由：`proto_v3_rx_frame()` → `g_sc_proto_handler.on_msg` → `sc_proto_on_msg()` → `case PROTO_V3_VERSION_REQUEST:` → `sc_proto_version_request()`
- 两个存档（存在 `g_sc` 里）：
  - `g_sc.remote_version = 6` —— 后面决定兼容行为；
  - `g_sc.type = 17` —— 后续按“视频流”处理。
- 先后调 `sc_proc_on_version(6)` 同步给帧处理层，再用 `proto_v3_rsp_version(PROTO_V3_RESULT_OK, 0)` 组装 41 发给手机。
  **没有事件，也没有回调注册**，就是一个直接调用。
- `sc_proc_on_version()` 的作用：`support_camera_info = (remote_version & 0xff) >= 4`，
  即手机协议版本 ≥ 4 才接受 CAMERA_INFO 帧。

### 1.3 设备 → 手机：VERSION_RESPONSE（msg 41，0x29）

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `29 00` | msg_id | 41 | VERSION_RESPONSE |
| `06 00` | len | 6 | 载荷长度（含 1B 结构体对齐填充） |
| `00 00` | result | 0 | 成功 |
| `06 00` | local_version | 6 | 固件协议版本 |
| `00` | need_reboot | 0 | 投屏无需重启 |
| `00` | （填充） | 0 | `{u16,u16,u8}` 尾部补齐到 2 字节倍数 |


## 2. 前置准备（42）

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `2A 00` | msg_id | 42 | REBOOT_COMMAND |
| `01 00` | len | 1 | 载荷长度 |
| `00` | is_reboot | 0 | 投屏时=0（=1 时设备直接重启） |

- `sc_proto_on_msg()` 的 switch 里没有 `PROTO_V3_REBOOT_COMMAND` 分支，会落到 `default`
  打一行 `ignore message 42` 然后丢弃
- 投屏场景手机固定发 `is_reboot=0`，设备无需回应

## 3. 会话初始化，定发送规则（43 / 44）

> 数据流节奏全在这里定死：每几包要应答（`num_of_rsp_file=3`）、每包最大多少字节（`packet_length_file=4096`）。
> 帧对齐方式（`file_align`）由手机在 43 里宣告，设备只按它选择长度校验公式。

### 3.1 手机 → 设备：FILE_INIT_REQUEST_EXT（msg 43，0x2B）

| 偏移 | 大小 | 字段 | 含义 / 投屏典型值 |
|---|---|---|---|
| 0 | 2 | `ota_version` | 手机协议版本（6） |
| 2 | 2 | `type` | 17 = CONTINUAL_FILE 投屏 |
| 4 | 4 | `file_count` | 会话总文件数 |
| 8 | 4 | `file_length` | 会话总长度 |
| 12 | 16 | `md5` | 会话级 md5（投屏不校验） |
| 28 | 1 | `phone_type` | 1=iOS，2=Android，5=BT_PAN… |
| 29 | 1 | `file_align` | 1=对齐模式（帧尾带 4B CRC + 4 字节对齐） |

### 3.2 设备处理：应答频率决策

`sc_proto_file_init_request()` 里先做手机类型重映射（`sc_proto_remap_phone_type()`：SPP / PAN / WiFi 会改写成对应的
`SC_PHONE_TYPE_*`），然后用四个函数定下本轮参数：

| 字段 | 来源 |
|---|---|
| `num_of_rsp_file` | `sc_proc_on_init_req(phone_type)` |
| `num_of_rsp_bin` | `sc_proto_num_of_rsp_bin(phone_type)` |
| `packet_length_file` | `sc_proto_stream_packet_len(phone_type)` |
| `packet_length_bin` | 固定 `SC_PKT_LEN_BIN`（本流程不使用） |

应答频率 `sc_proc_on_init_req()` 的取值：

| 条件 | 频率 |
|---|---|
| 默认（BLE 直连投屏走这里） | 3（`JPEG_RSP_FREQ`，每 3 个 59 包要一次应答） |
| Android_SPP / BT_PAN 且频率还是默认 | 58x/57x → 8；其他芯片 → 4 |
| Android_SPP | 1 |
| 运行时调整 | shell `sc_cast rsp_freq <n>` 改 `jpeg_rsp_req`，下次会话生效 |

### 3.3 设备 → 手机：FILE_INIT_RESPONSE_EXT（msg 44，0x2C）

| 字段 | 值 | 对后续的意义 |
|---|---|---|
| `result` | 0 | 成功 |
| `local_version` | 6 | 固件协议版本 |
| `num_of_rsp_file` | 3 | 手机每 3 个 59 包置一次 `need_rsp`（bit0）——数据流的"拍子"就定在这 |
| `num_of_rsp_bin` | 20（SPP 链路为 200） | 本流程不使用 |
| `resume_state` / `resume_file_index` / `fs_block` | 0 | 断点续传能力（本流程不用） |
| `packet_length_file` | 4096 | 59 包单包 `data_length` 上限（iOS旧=1024，SPP/PAN=10240，WiFi=20480） |
| `packet_length_bin` | 548 | 本流程不使用 |

### 3.4 hex 对照

**43 请求：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `2B 00` | msg_id | 43 | FILE_INIT_REQUEST_EXT |
| `1E 00` | len | 30 | 载荷长度 |
| `06 00` | ota_version | 6 | 手机协议版本 |
| `11 00` | type | 17 | CONTINUAL_FILE |
| `<4B>` | file_count | — | 会话总文件数 |
| `<4B>` | file_length | — | 会话总长度 |
| `<16B>` | md5 | — | 投屏不校验 |
| `02` | phone_type | 2 | Android |
| `00` | file_align | 0 | 非对齐 |

**44 应答：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `2C 00` | msg_id | 44 | FILE_INIT_RESPONSE_EXT |
| `14 00` | len | 20 | 载荷长度（含 2B 结构体对齐填充） |
| `00 00` | result | 0 | 成功 |
| `06 00` | local_version | 6 | 固件协议版本 |
| `03` | num_of_rsp_file | 3 | 每 3 包应答一次 |
| `14` | num_of_rsp_bin | 20 | 本流程不使用 |
| `00 00` | resume_state | 0 | |
| `00 00 00 00` | resume_file_index | 0 | |
| `00 00` | fs_block | 0 | |
| `00 10` | packet_length_file | 4096 | 单包 data_length 上限 |
| `24 02` | packet_length_bin | 548 | 本流程不使用 |
| `00 00` | （填充） | 0 | 含 `u32` 的结构体按 4B 对齐，尾部补齐 18→20 |


## 4. 初始化完成确认（45 / 46）

### 4.1 手机 → 设备：FILE_INIT_COMPLETED_REQUEST（msg 45，0x2D）

| 偏移 | 大小 | 字段 | 投屏典型值 |
|---|---|---|---|
| 0 | 1 | `resume_result` | 0（0=全新开始；1=断点续传，升级路径才用） |
| 1 | 1 | `res_file_with_path` | 0 |
| 2 | 2 | `file_name_len` | 0 |
| 4 | 4 | `block_size` | 0（手机给的对齐块大小） |
| 8 | N | `file_name` | 无（投屏不带文件名） |

### 4.2 设备处理 + 46 应答

直接调 `sc_proto_file_init_completed()`：把 `g_sc.is_lose_check` 清零，然后
`proto_v3_rsp_file_init_completed(PROTO_V3_RESULT_OK, 0)` 回 OK。没有其它副作用。

**45 请求：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `2D 00` | msg_id | 45 | FILE_INIT_COMPLETED_REQUEST |
| `08 00` | len | 8 | 载荷长度 |
| `00` | resume_result | 0 | 全新开始 |
| `00` | res_file_with_path | 0 | |
| `00 00` | file_name_len | 0 | |
| `00 00 00 00` | block_size | 0 | |

**46 应答：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `2E 00` | msg_id | 46 | FILE_INIT_COMPLETED_RESPONSE |
| `08 00` | len | 8 | 载荷长度 |
| `00 00` | result | 0 | 成功 |
| `00 00` | reserved | 0 | |
| `00 00 00 00` | left_block | 0 | |

### 4.3 这个环节的作用

1. 清掉丢包标志：`g_sc.is_lose_check = 0`
2. 回 46，没有其它副作用（不启动任何线程、不碰存储）

## 5. 数据帧开始（47 / 48）

### 5.1 手机 → 设备：FILE_START_REQUEST_EXT（msg 47，0x2F）

| 偏移 | 大小 | 字段 | 投屏典型值 |
|---|---|---|---|
| 0 | 2 | `file_type` | 1 = RES_WITHOUT_PATH（不带路径的资源流）/ 0 = RES |
| 2 | 2 | `file_index` | 文件序号（投屏一般为 0） |
| 4 | 4 | `file_length` | 文件总长（手机给） |
| 8 | 4 | `file_packet_count` | 总包数（手机给） |
| 12 | 4 | `already_send_length` | 已传长度（断点续传用；投屏=0） |
| 16 | 2 | `file_name_length` | 0（投屏不带文件名） |
| 18 | N | `file_name` | 无（video stream 不读、不 malloc，直接置 NULL） |

> 本流程只接受 `file_type` 为 0 或 1，其余取值会被 `sc_proto_file_start_request()` 直接拒绝。

### 5.2 设备处理

`sc_proto_on_msg()` → `sc_proto_file_start_request()`：

1. 只读载荷前 2 字节 `file_type`，不是 `RES` / `RES_WITHOUT_PATH` 就报 `unexpected file type` 并返回；
2. `sc_proc_file_start()` 进入投屏模式（内部置 `g_casting = true` 并初始化收帧缓冲）；
3. 启动监督定时器：`proto_v3_sync_start(PROTO_V3_SYNC_CONTINUAL_FILE)`（所有链路一致）；
4. `proto_v3_rsp_file_start(PROTO_V3_RESULT_OK)` 发送 48。

> 48 只是纯应答，不重置任何计数。连接提速接口 `proto_v3_request_connection_priority()` 目前没有调用。
> 监督定时器在 47 启动，每个 59 包到达时重启（见 6.2），51 停止。

### 5.3 hex 举例

**47 请求**（type=RES_WITHOUT_PATH、index=0、file_length=0x00300000、packet_count=0x00000C00、already=0）：

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `2F 00` | msg_id | 47 | FILE_START_REQUEST_EXT |
| `12 00` | len | 18 | 载荷长度 |
| `01 00` | file_type | 1 | RES_WITHOUT_PATH |
| `00 00` | file_index | 0 | |
| `00 00 30 00` | file_length | 0x00300000 | 文件总长（示例） |
| `00 0C 00 00` | file_packet_count | 0x00000C00 | 总包数（示例） |
| `00 00 00 00` | already_send_length | 0 | |
| `00 00` | file_name_length | 0 | |

**外层 GATT 封装**（与 47 请求合成完整帧）：

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `01` | cateID | 1 | |
| `00` | flag | 0 | 单包 |
| `16 00` | len | 22 | 4 + 18（layer2 共 22 字节） |

**48 应答**（result=0）：

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `30 00` | msg_id | 48 | FILE_START_RESPONSE_EXT |
| `02 00` | len | 2 | 载荷长度 |
| `00 00` | result | 0 | 成功 |

> 完整 GATT 帧 = `01 00 | 06 00`（cate=1 / flag=0 / len=6）+ 上述 6 字节。

## 6. 数据传输（59 / 50 / 60 / 61）

### 6.1 手机 → 设备：FILE_CONTINUAL_PACKET（msg 59，0x3B）

12 字节头 + 数据：

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | `file_length` | 本帧总长（含 10B 业务头；对齐时含 CRC+填充） |
| 4 | 4 | `index` | 包在本帧内的序号，从 1 开始（index==1 = 新一帧的第一包） |
| 8 | 2 | `data_length` | 本包数据长度，≤ 44 报文协商值（BLE 4096 / iOS旧 1024 / SPP·PAN 10240 / WiFi 20480） |
| 10 | 1 | `type` | 0=WHOLE 整帧一包 / 1=START / 2=MIDDLE / 3=ENDING |
| 11 | 1 | `reserved` | bit0=`need_rsp`（要 50 应答）；bit1=`new_packet`（丢包恢复标志） |
| 12 | N | `data` | 帧数据切片（帧第一个包的 data 以 10B 业务头开始，见下） |

> `index` 是每帧重新从 1 数的，不是整个会话的包号——这是组帧的关键设计。

**`data` 区开头：10B 业务头**（协议模式下、仅每帧第一个包（index==1）携带；后续包直接是数据段）：

| 帧内偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 2 | `type` | 帧内容类型：**0=JPEG** / 1=MP3 / 2=EZIP / 3=NAV_INFO / 4=CAMERA_INFO（与 59 包头偏移 10 的分片 `type` 同名但无关） |
| 2 | 4 | `timestamp` | 手机给的帧时间戳 |
| 6 | 4 | `total_len` | 净数据长度（业务头之外的真实数据字节数） |
| 10 | N | 数据 | 真实帧字节（JPEG 图像等） |

老手机（无协议）没有业务头——设备直接按魔数（`FF D8`）判断是否 JPEG。

### 6.2 设备处理与组帧

`sc_proto_continual_packet()` 的处理顺序：

```c
1. 长度校验（< 12 字节直接返回）
2. 拆出 file_len / index / data_len / packet_type / reserved
   need_rsp = reserved & 1;  new_packet = (reserved & 2) >> 1;
3. 丢包恢复：g_sc.is_lose_check 为真时，new_packet==0 的包全部丢弃
4. 越界检查：12 + data_len > len 则报错返回
5. 复位监督定时器：proto_v3_sync_end(); proto_v3_sync_start(CONTINUAL_FILE);
6. 填 sc_recv_t（带 file_aligned / phone_type / remote_version）
7. sc_proc_on_packet(&recv)
8. need_rsp 时：若 sc_get_switch_mode() 非 0 则发 61 并清标志，否则发 50
```

> 注意第 8 步：**61 是“措着应答时机”发出去的**，不是独立触发。shell 只负责
> `sc_set_switch_mode(n)`，真正的 61 要等到下一个 `need_rsp=1` 的 59 包。
> 且 61 与 50 是**二选一**，发 61 的那个包不回 50。

**组帧（每帧第一包，index==1）做 4 件事：**

1. 会话首次：`judge_protocol()` 判一次协议——读业务头前 10 字节 `[type u16][timestamp u32][total_len u32]`，
   同时满足 `type < SC_DATA_TYPE_MAX` 和下面两条长度公式之一才算协议帧：
   不对齐时 `total_len + 10 == 首包数据长度`；对齐时 `(total_len + 14 + 3) / 4 * 4 == 首包数据长度`。
   不满足则判定为裸数据（老手机）；
2. 协议模式：提取 type/timestamp，`total_len = file_size - 10`，数据指针跳过 10B 业务头；
3. 依类型切 UI 模式：NAV → 导航页；JPEG/CAMERA → 全屏页；
4. 上一帧还没组完 → 标记丢弃（防错位）；然后取新帧缓冲（JPEG 用 64 字节对齐的 PSRAM 分配器）。

**每个包**：memcpy 追加到 `frame->data[offset]`；`type==WHOLE` 或 `ENDING` → `buffer_set_frame_done()` → 帧状态置 UPDATED；UI 的 20ms 定时器通过 `sc_frame_acquire(SC_DATA_TYPE_JPEG)` 取走显示。

### 6.3 心跳（2s / 6s）

每个 59 包到达 → `proto_v3_sync_end()` + `proto_v3_sync_start(CONTINUAL_FILE)` 重启 2s 定时器。
超时回调是 `sc_proto_on_sync_timeout()`：

```text
2s 没包 → sc_proto_on_sync_timeout(PROTO_V3_SYNC_CONTINUAL_FILE):
    proto_v3_sync_end()
    g_sc.is_lose_check = 1
    proto_v3_rsp_continual_error(SC_SERIAL_TRANS_ERROR_ASSEMABLE)   // 发 60，error_type=1
    sc_proc_on_packet_error()                                       // 丢弃正在组装的帧
    proto_v3_sync_start(PROTO_V3_SYNC_CONTINUAL_FILE_LOST)          // 换 6s 超时

6s 还没包 → sc_proto_on_sync_timeout(PROTO_V3_SYNC_CONTINUAL_FILE_LOST):
    proto_v3_sync_end()
    g_sc.is_lose_check = 0
    sc_proc_err()          // 判定对端掉线，会话收尾
```

> 两级超时：2s 是「可能卡了一下」，只报错并丢半帧；6s 是「对端真的没了」，直接结束会话。
> 两条路径最后都落到 `sc_proc_session_end()` / `sc_proc_err()`，效果一致：`g_casting` 复位、
> 二维码页自动回来。

### 6.4 设备应答：50 / 60 / 61

| 报文 | 字节 | 字段 | 值 | 说明 |
|---|---|---|---|---|
| 50 | `32 00` | msg_id | 50 | FILE_PACKET_RESPONSE_EXT |
| 50 | `08 00` | len | 8 | 含 2B 结构体对齐填充 |
| 50 | `00 00` | result | 0 | 正常包应答流程，每 3 包一次 |
| 50 | `00 00` | （填充） | 0 | `u32` 对齐填充 |
| 50 | `00 00 00 00` | new_index | 0 | |
| 60 | `3C 00` | msg_id | 60 | FILE_CONTINUAL_PACKET_ERROR |
| 60 | `06 00` | len | 6 | |
| 60 | `01 00` | error_type | 1 | 组装错误（`SC_SERIAL_TRANS_ERROR_ASSEMABLE`），连续流超时时发 |
| 60 | `00 00` | reserved_1 | 0 | |
| 60 | `00 00` | reserved_2 | 0 | |
| 61 | `3D 00` | msg_id | 61 | FILE_CONTINUAL_PACKET_MODE_CHANGE |
| 61 | `06 00` | len | 6 | |
| 61 | `03 00` | mode | 3 | 请求拍照（3=拍照/4=开始录像/5=停止录像）；由 `sc_set_switch_mode()` 设值，措下个 `need_rsp` 包发出 |
| 61 | `00 00` | reserved_1 | 0 | |
| 61 | `00 00` | reserved_2 | 0 | |


### 6.5 59 包 hex 举例

一帧 JPEG：业务头 10B + 负载 5000B = 5010B（0x1392），拆两包（4096 + 914）：

**包 1（START, need_rsp=1）：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `3B 00` | msg_id | 59 | FILE_CONTINUAL_PACKET |
| `0C 10` | len | 4108 | 12 + 4096 |
| `92 13 00 00` | file_length | 5010 | 0x1392，本帧总长 |
| `01 00 00 00` | index | 1 | 本帧第 1 包 |
| `00 10` | data_length | 4096 | |
| `01` | type | 1 | START |
| `01` | reserved | 1 | need_rsp=1 |
| `…` | data | — | 业务头 10B + JPEG 第 1 段（4086B） |

**包 2（ENDING, need_rsp=0）：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `3B 00` | msg_id | 59 | FILE_CONTINUAL_PACKET |
| `9E 03` | len | 926 | 12 + 914 |
| `92 13 00 00` | file_length | 5010 | 同帧 |
| `02 00 00 00` | index | 2 | 本帧第 2 包 |
| `92 03` | data_length | 914 | |
| `03` | type | 3 | ENDING |
| `00` | reserved | 0 | need_rsp=0 |
| `…` | data | — | 帧数据第 2 段（914B） |

## 7. 本路结束（51 / 52）

### 7.1 手机 → 设备：FILE_END_REQUEST_EXT（msg 51，0x33）

| 偏移 | 大小 | 字段 | 投屏典型值 |
|---|---|---|---|
| 0 | 1 | `more_image` | 0（还有没有下一个文件；投屏无后续） |
| 1 | 1 | `file_type` | 1 = RES_WITHOUT_PATH |
| 2 | 2 | `image_id_or_file_index` | 文件序号（投屏=0） |

### 7.2 设备处理 + 52 应答

`sc_proto_file_end_request()`：先校验 `file_type` 必须是 `RES` / `RES_WITHOUT_PATH` → 停监督定时器
→ `sc_proc_file_end()` 释放收帧缓冲 → `proto_v3_rsp_file_end(PROTO_V3_RESULT_OK)` 回 52。

> 注意：此时 `g_casting` 仍为 true——二维码页要到 53/54 或异常收尾才恢复显示。

**51 请求：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `33 00` | msg_id | 51 | FILE_END_REQUEST_EXT |
| `04 00` | len | 4 | 载荷长度 |
| `00` | more_image | 0 | 无后续文件 |
| `01` | file_type | 1 | RES_WITHOUT_PATH |
| `00 00` | image_id_or_file_index | 0 | |

**52 应答：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `34 00` | msg_id | 52 | FILE_END_RESPONSE_EXT |
| `02 00` | len | 2 | 载荷长度 |
| `00 00` | result | 0 | 成功 |

## 8. 整个会话结束（53 / 54）

手机发 msg53 → `sc_proto_total_end_request()`：**载荷完全不解析**，只做两件事：

1. `sc_proc_session_end()` —— 结束投屏会话（`g_casting` 复位、释放缓冲、二维码页回来）
2. `proto_v3_rsp_file_total_end(PROTO_V3_RESULT_OK)` —— 发送 54

**53 请求**（示例值：设备端投屏分支不解析载荷，长度/内容以手机实际发送为准）：

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `35 00` | msg_id | 53 | FILE_TOTAL_END_REQUEST_EXT |
| `00 00` | len | 0 | 载荷不解析 |

**54 应答：**

| 字节 | 字段 | 值 | 说明 |
|---|---|---|---|
| `36 00` | msg_id | 54 | FILE_TOTAL_END_RESPONSE_EXT |
| `02 00` | len | 2 | 载荷长度 |
| `00 00` | result | 0 | 成功 |

## 9. 补充：异常收尾（手机不道别）

手机 App 被杀/走远，没有 51/53，走监督超时线：

```text
2s 无包 → sc_proto_on_sync_timeout(CONTINUAL_FILE):
            发 60（error_type=1），丢弃半帧，换 6s 超时继续等

链路断开 → sc_proto_on_close():
            proto_v3_sync_end()
            g_sc.is_lose_check = 0
            sc_proc_session_end()      ← 和正常 53 收尾走同一个出口

传输报错 → sc_proto_on_error(error):
            proto_v3_sync_end()
            （其余处理见下一行）
```

> 三条路径最后都落到 `sc_proc_session_end()` / `sc_proc_err()`，效果一致：`g_casting` 复位、
> 二维码页自动回来。

## 附录：报文速查表

| msg | 名称 | 方向 | 载荷长 | 说明 |
|---|---|---|---|---|
| 40 | VERSION_REQUEST | 手机→设备 | 4 | 版本 + 类型（17=投屏） |
| 41 | VERSION_RESPONSE | 设备→手机 | 6 | result + 版本 + need_reboot（含 1B 填充） |
| 42 | REBOOT_COMMAND | 手机→设备 | 1 | is_reboot（投屏=0，设备侧忽略） |
| 43 | FILE_INIT_REQUEST_EXT | 手机→设备 | 30 | 初始化 + 发送规则声明 |
| 44 | FILE_INIT_RESPONSE_EXT | 设备→手机 | 20 | 应答频率 + 单包上限（含 2B 填充） |
| 45 | FILE_INIT_COMPLETED_REQUEST | 手机→设备 | 8 | 全新开始（投屏） |
| 46 | FILE_INIT_COMPLETED_RESPONSE | 设备→手机 | 8 | result + left_block |
| 47 | FILE_START_REQUEST_EXT | 手机→设备 | 18 | 本路开始 |
| 48 | FILE_START_RESPONSE_EXT | 设备→手机 | 2 | result |
| 59 | FILE_CONTINUAL_PACKET | 手机→设备 | 12+N | 数据切片 |
| 50 | FILE_PACKET_RESPONSE_EXT | 设备→手机 | 8 | result + new_index（含 2B 填充） |
| 60 | FILE_CONTINUAL_PACKET_ERROR | 设备→手机 | 6 | error_type（1=组装错误） |
| 61 | FILE_CONTINUAL_PACKET_MODE_CHANGE | 设备→手机 | 6 | mode（3=拍照） |
| 51 | FILE_END_REQUEST_EXT | 手机→设备 | 4 | 本路结束 |
| 52 | FILE_END_RESPONSE_EXT | 设备→手机 | 2 | result |
| 53 | FILE_TOTAL_END_REQUEST_EXT | 手机→设备 | — | 会话结束（载荷不解析） |
| 54 | FILE_TOTAL_END_RESPONSE_EXT | 设备→手机 | 2 | result |
