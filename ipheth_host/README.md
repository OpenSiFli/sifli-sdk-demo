# iPhone USB 网络共享（ipheth）

## 概述

本示例把开发板配置成 USB Host，通过 USB 线连接 iPhone，接入 iPhone 的「个人热点 / USB 网络共享」（Apple 私有协议 ipheth），让开发板自身获得上网能力。

和普通 USB 网络共享（RNDIS / ECM）不同，iPhone 的 ipheth 接口插入后处于「未激活」状态：手机既不向主机推送数据，也不接受主机发来的数据。主机必须先与 iPhone 完成一次 lockdown 配对，手机侧才会打开这条通路。

本示例内置完整配对流程：插上 iPhone 后板子主动发起配对，用户在手机上点一次「信任」，配对完成后手机立即开始收发，链路随即建立。


## 支持的开发板

- sf32lb52 lcd 系列

### 硬件需求

- 支持 USB Host 功能的 SiFli 开发板
- 支持数据传输的 USB 线
- iPhone（需支持「个人热点」）

## 使用方法

### 1. 编译和烧录

进入例程的 project 目录：

```
scons --board=sf32lb52-lcd_n16r8 -j8
build_sf32lb52-lcd_n16r8_hcpu\uart_download.bat
```

按提示选择串口即可下载。

### 2. 打开手机个人热点

iPhone：`设置 → 个人热点 → 允许其他人加入`。

### 3. 插上 USB 线

串口会依次输出：

```
[USB] +0ms device connected
[USB] +725ms configured (SET_CONFIGURATION done)
[USB] +733ms interface 2 class driver started
[pair] ==== start, host id xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx ====
[pair] QueryType -> 'com.apple.mobile.lockdown' (resp 297)
[pair] DevicePublicKey 431 bytes
[pair] sending Pair (4639 bytes), tap TRUST on the phone
```

此时手机会弹出「要信任此电脑吗？」，点 **信任**。

随后：

```
[pair] SUCCESS: host trusted (EscrowBag 32 bytes)
[I/ipheth] ipheth: first rx frame -> pairing confirmed, tx enabled
[I/ipheth] ipheth: carrier on, re-arm set_interface ok
[I/ipheth] ipheth: link UP (+5005ms since class attach)
[USB] DHCP bound IP=172.20.10.3 mask=255.255.255.240 gw=172.20.10.1 (T+8623ms after plugin)
```

### 4. 验证联网

```
msh />ping 8.8.8.8
ping: not found specified netif, using default netdev u2.
60 bytes from 8.8.8.8 icmp_seq=1 ttl=106 time=447 ms
60 bytes from 8.8.8.8 icmp_seq=2 ttl=106 time=278 ms
60 bytes from 8.8.8.8 icmp_seq=3 ttl=106 time=279 ms
60 bytes from 8.8.8.8 icmp_seq=4 ttl=106 time=272 ms
4 packets transmitted, 4 received, 0% packet loss
```

### 手动触发配对

正常流程会自动配对，调试时可以手动执行一次：

```
msh />iph_pair
```

## 注意事项

| 现象 | 说明 |
|---|---|
| 手机锁屏时拿不到 IP | iOS 的设计，PC 上也一样——锁屏状态下连信任弹窗都不会出。解锁后板子每 3 秒自动重试，无需拔插 |
| 每次拔插都要点一次「信任」 | 本示例没有实现 `StartSession`（免弹窗需要 TLS 客户端认证），每次插拔都会重新走一遍配对 |

## 目录结构

```
ipheth_host/
├── project/                      SConstruct / Kconfig / proj.conf
└── src/
    ├── main.c                    例程入口：USB 事件 + 链路/DHCP 监视
    ├── usbh_ipheth.c/.h          ipheth class 驱动（客户提供，未修改）
    ├── usbh_ipheth_lwip.c        ipheth -> lwIP 网卡适配层
    ├── usbh_apple_config.c       Apple 多 configuration 选择
    ├── usb_config.h              USB Host 配置
    └── pairing/                  iPhone 配对模块（可独立移植到其它工程）
        └── README.md             模块说明与移植步骤
```

配对模块的接口、移植方法、资源占用等细节见 [`src/pairing/README.md`](src/pairing/README.md)。
