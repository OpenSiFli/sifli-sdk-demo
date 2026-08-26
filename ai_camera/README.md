# SF32LB573 AI Camera

SF32LB573 拍学机原型工程。当前阶段验证 RT-Thread 应用协调和蓝牙 PAN；音频、小智信令、摄像头、PDM、DVP、SD 卡和外置 Wi-Fi 尚未接入。

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
