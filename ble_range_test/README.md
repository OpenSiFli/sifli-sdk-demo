# BLE 拉距测试 Demo (ble_range_test)

## SiFli-SDK 版本
SDK release/v2.4 分支。

## 支持的开发板
- sf32lb52-core_n4

## 概述
本 Demo 通过两块 SF32LB52 板进行 BLE 拉距测试,一块作为 TX(central),另一块作为 RX(peripheral)。TX 端持续发送 write-without-response 包,RX 端持续接收并丢弃包,以此来测试最大可达距离和连接稳定性。

## 跳线真值表

PA24~PA27 默认低电平,**拉高接 3.3V 视为"接通"**。

### PA27 — 角色

| 跳线 | 角色 |
|:---:|:---|
| 不接 / 低 | TX(central,主动扫描+发送) |
| 拉高 | RX(peripheral,广播+接收) |

### PA26/PA25/PA24 — 发射功率档(优先级 PA26 > PA25 > PA24)

| PA26 | PA25 | PA24 | 发射功率 |
|:---:|:---:|:---:|:---:|
| 拉高 | * | * | 10 dBm |
| 低 | 拉高 | * | 13 dBm |
| 低 | 低 | 拉高 | 16 dBm |
| 低 | 低 | 低 | 19 dBm(默认/最大) |

> 注意:开机后跳线变化无效,**改档位必须重新上电**。

## LED 指示(PA32)

| 状态 | TX 板 | RX 板 |
|---|---|---|
| 未连接(搜索中) | 双闪(亮 100 / 灭 100 / 亮 100 / 灭 700 ms) | 单闪(每秒亮 100 ms) |
| 已连接 | 常亮 | 常亮 |

LED 状态由 BLE 协议栈事件驱动:`CONNECTION_MANAGER_CONNCTED_IND` → 常亮;`BLE_GAP_DISCONNECTED_IND` → 闪烁。

## 工作流程

### TX 端
1. 开机读取跳线 → 设置发射功率 → LED 进入搜索态(双闪)
2. 开 2 秒扫描窗口,窗口内累计名字匹配 `SIFLI_RANGE` 的最强 RSSI 候选
3. 窗口结束时:有候选 → 向最强者发起连接;无候选 → 重开扫描窗口
4. 连接建立 → MTU 交换 → 服务发现 → 注册远端服务 → LED 转常亮 → **发送线程以约 2ms/包持续写**
5. 链路断开(supervision timeout 5s 或对端断)→ LED 回搜索态 → 重扫重连

### RX 端
1. 开机读取跳线 → 设置发射功率 → LED 进入搜索态(单闪)
2. 注册 GATT 服务 → 启动 legacy connectable 广播,名字 `SIFLI_RANGE`,`is_auto_restart=1`
3. 被连接 → LED 转常亮 → 接受 GATT 写包(write-without-response)
4. 链路断开 → LED 回搜索态 → 协议栈自动重启广播

> 断连判定**完全由 BLE 协议栈负责**(LL supervision timeout 5s),应用层不维护伪丢包率/连接稳定性看门狗,避免影响最大可达距离测试。

## 目录结构

```
ble_range_test
├── project                  # 工程文件
│   ├── board                # 板级初始化
│   ├── proj.conf            # BT_TX_POWER_VAL_MAX=19 解锁全功率档
│   ├── SConstruct
│   ├── SConscript
│   ├── rtconfig.py
│   └── template.uvprojx
├── src
│   ├── main.c               # BLE 事件机/扫描-连接编排/TX 发包线程
│   ├── range_test.c         # 跳线读取/LED 状态机/HAL_LCPU_CONFIG_set_core 覆盖
│   ├── range_test.h
│   └── SConscript
└── README.md
```

## 编译烧录

```bash
cd project
scons --board=sf32lb52-core_n4 -j8
```

两块板同时烧录本工程,**改 PA27 跳线**来切换角色。

