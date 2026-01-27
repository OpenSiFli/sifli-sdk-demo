# 蓝牙PAN网上下行速度测试

## 1 测试环境配置

### 硬件

- **测试设备**：sf32lb56-lcd_a1218r12n1  
- **手机型号**：Android

### 软件

- **手机软件**：Magic iPerf  

### 软件修改

在 `menuconfig` 中确认是否启用了 `iperf` 功能。如果未启用，请手动启用。

![启用 iperf 功能](./assets/enable_iperf.png)

启用`ifconfig`命令

![启用ifconfig](./assets/iperf_ifconfig.png)

## 2 测试步骤

### 2.1 打开手机端蓝牙网络共享功能

**Android 手机端操作步骤**：

1. 打开设置。
2. 进入 **连接与共享**。
3. 选择 **个人热点**。
4. 启用 **蓝牙网络共享**。

### 2.2 获取服务器 IP 地址

通过串口向开发板发送`ifconfig`命令，即可得到服务端的IP地址
![获取IP地址](./assets/iperf_getip.png)

### 2.3 进行 iperf 测试

1. 通过串口在电脑上发送 finsh 命令 `iperf -s -p 5001` 启动服务端。
   - 测试完成后，发送 finsh 命令 `iperf --stop` 停止服务。
2. 在手机上打开 Magic iPerf，输入以下命令：

```
-c <服务器端 IP> -p 5001 -i 2 -t 100
```
   点击 **Start** 开始测试。
3. 观察 Magic iPerf 和开发板输出的日志。

测试效果如下图所示：

![PC 端测试效果](./assets/iperf_pc.png)  
![手机端测试效果](./assets/iperf_1.jpg)
