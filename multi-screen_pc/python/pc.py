import psutil
import serial
import time
import clr
from datetime import datetime

class PCMonitor:
    def __init__(self, port='COM3', baudrate=1000000, timeout=1, dll_path="./LibreHardwareMonitorLib"):
        # 串口初始化
        try:
            self.serial_port = serial.Serial(
                port=port,
                baudrate=baudrate,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                bytesize=serial.EIGHTBITS,
                timeout=timeout
            )
        except serial.SerialException as e:
            print(f"串口初始化失败: {e}")
            self.serial_port = None

        # LibreHardwareMonitor 初始化
        self.computer = None
        try:
            clr.AddReference(dll_path)
            from LibreHardwareMonitor.Hardware import Computer
            self.computer = Computer()
            self.computer.IsCpuEnabled = True
            self.computer.IsGpuEnabled = True
            self.computer.IsMemoryEnabled = True
            self.computer.IsDiskEnabled = True
            self.computer.IsNetworkEnabled = True
            self.computer.Open()
        except Exception as e:
            print(f"LibreHardwareMonitor 初始化失败: {str(e).splitlines()[0]}")

        # 数据缓存
        self.cpu_data = {"usage": 0, "temp": 0}  # CPU数据缓存
        self.gpu_data = {"usage": 0, "temp": 0}  # GPU数据缓存
        self.last_net_sent = psutil.net_io_counters().bytes_sent
        self.last_net_recv = psutil.net_io_counters().bytes_recv
        self.last_net_time = time.time()

        # 发送控制参数
        self.cmd_delay = 0.01
        self.last_other_send = time.time()
        self.last_time_send = time.time()

    def _update_hardware(self):
        """强制刷新所有硬件传感器数据"""
        if self.computer:
            for hardware in self.computer.Hardware:
                hardware.Update()

    def _update_all_hardware_data(self):
        """一次遍历同时获取CPU和GPU的利用率与温度"""
        # 初始化默认值
        cpu_loads = []
        cpu_temps = []
        gpu_loads = []
        gpu_temps = []

        if not self.computer:
            # 若Libre未初始化，CPU用psutil，GPU置0
            self.cpu_data["usage"] = round(psutil.cpu_percent(interval=0.1), 1)
            self.cpu_data["temp"] = 0
            self.gpu_data["usage"] = 0
            self.gpu_data["temp"] = 0
            return

        self._update_hardware()  # 一次刷新所有硬件

        # 单次遍历所有硬件，同时采集CPU和GPU数据
        for hardware in self.computer.Hardware:
            hw_type = hardware.HardwareType.ToString()
            hw_name = hardware.Name.lower()

            # 处理CPU相关硬件
            if hw_type == "Cpu" or "cpu" in hw_name or "processor" in hw_name:
                for sensor in hardware.Sensors:
                    sensor_type = sensor.SensorType.ToString()
                    sensor_name = sensor.Name.lower()
                    value = sensor.Value

                    # 匹配CPU利用率（Total负载）
                    if sensor_type == "Load" and "total" in sensor_name:
                        if value is not None and 0 <= value <= 100:
                            cpu_loads.append(value)
                    # 匹配CPU温度（核心/封装温度）
                    elif sensor_type == "Temperature" and value is not None:
                        if (0 < value < 100 and 
                            ("core" in sensor_name or "package" in sensor_name or "tctl/tdie" in sensor_name) and 
                            "distance" not in sensor_name):
                            cpu_temps.append(value)

            # 处理GPU相关硬件
            if "gpu" in hw_type.lower():
                for sensor in hardware.Sensors:
                    sensor_type = sensor.SensorType.ToString()
                    sensor_name = sensor.Name.lower()
                    value = sensor.Value

                    # 匹配GPU利用率（D3D 3D或GPU负载）
                    if sensor_type == "Load" and value is not None and 0 <= value <= 100:
                        if "d3d 3d" in sensor_name or "gpu total" in sensor_name or "utilization" in sensor_name:
                            gpu_loads.append(value)
                    # 匹配GPU温度（核心/芯片温度）
                    elif sensor_type == "Temperature" and value is not None:
                        if 0 < value < 100 and ("core" in sensor_name or "gpu" in sensor_name or "die" in sensor_name):
                            gpu_temps.append(value)

        # 更新CPU数据（无传感器时降级到psutil）
        self.cpu_data["usage"] = round(sum(cpu_loads)/len(cpu_loads), 1) if cpu_loads else round(psutil.cpu_percent(interval=0.1), 1)
        self.cpu_data["temp"] = round(sum(cpu_temps)/len(cpu_temps), 1) if cpu_temps else 0

        # 更新GPU数据
        self.gpu_data["usage"] = round(sum(gpu_loads)/len(gpu_loads), 1) if gpu_loads else 0
        self.gpu_data["temp"] = round(sum(gpu_temps)/len(gpu_temps), 1) if gpu_temps else 0

    # ---------------- CPU 数据获取 ----------------
    def get_cpu_usage(self):
        return self.cpu_data["usage"]

    def get_cpu_temp(self):
        return self.cpu_data["temp"]

    # ---------------- GPU 数据获取 ----------------
    def get_gpu_usage(self):
        return self.gpu_data["usage"]

    def get_gpu_temp(self):
        return self.gpu_data["temp"]

    # ---------------- 内存数据获取 ----------------
    def get_ram_usage(self):
        if not self.computer:
            return psutil.virtual_memory().percent

        self._update_hardware()
        ram_load = []
        for hw in self.computer.Hardware:
            if "memory" in hw.Name.lower() or "ram" in hw.Name.lower():
                for sensor in hw.Sensors:
                    if sensor.SensorType == "Load" and (sensor.Name == "Memory" or "used" in sensor.Name.lower()):
                        if sensor.Value is not None:
                            ram_load.append(sensor.Value)
        return round(sum(ram_load) / len(ram_load), 1) if ram_load else psutil.virtual_memory().percent

    # ---------------- 磁盘数据获取 ----------------
    def get_disk_usage(self):
        """获取C盘和D盘使用率"""
        c_usage = None
        d_usage = None
        
        for part in psutil.disk_partitions():
            if part.fstype and "fixed" in part.opts:
                try:
                    usage = psutil.disk_usage(part.mountpoint).percent
                    if part.device == "C:" or part.mountpoint == "C:\\":
                        c_usage = usage
                    elif part.device == "D:" or part.mountpoint == "D:\\":
                        d_usage = usage
                except:
                    continue
        return (c_usage if c_usage is not None else 0, d_usage if d_usage is not None else 0)

    # ---------------- 网络数据获取 ----------------
    def get_network_speed(self):
        """计算网络上传/下载速度（MB/s）"""
        net = psutil.net_io_counters()
        current_time = time.time()
        time_diff = current_time - self.last_net_time
        if time_diff < 0.1:
            return (0, 0)

        sent_mb = (net.bytes_sent - self.last_net_sent) / (1024 * 1024 * time_diff)
        recv_mb = (net.bytes_recv - self.last_net_recv) / (1024 * 1024 * time_diff)

        self.last_net_sent = net.bytes_sent
        self.last_net_recv = net.bytes_recv
        self.last_net_time = current_time
        return (round(sent_mb, 2), round(recv_mb, 2))

    # ---------------- 时间数据获取 ----------------
    def get_current_datetime(self):
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # ---------------- 命令发送核心逻辑 ----------------
    def _send_cmd(self, key, value):
        """封装串口命令发送"""
        if not self.serial_port or not self.serial_port.is_open:
            return False

        cmd = f"sys_set_cmd {key} {value}\n"
        try:
            self.serial_port.write(cmd.encode('utf-8'))
            self.serial_port.flush()
            time.sleep(self.cmd_delay)
            return True
        except serial.SerialException as e:
            print(f"命令发送失败 ({key}): {e}")
            return False

    # ---------------- 数据发送整合 ----------------
    def send_hardware_data(self):
        """发送所有硬件监控数据并打印"""
        if not self.serial_port or not self.serial_port.is_open:
            return

        # 一次更新所有CPU和GPU数据
        self._update_all_hardware_data()

        # CPU 数据
        cpu_usage = self.get_cpu_usage()
        cpu_temp = self.get_cpu_temp()
        print(f"CPU 使用率: {cpu_usage:.1f}% | 温度: {cpu_temp:.1f}°C")
        self._send_cmd("cpu_usage", f"{cpu_usage:.1f}")
        self._send_cmd("cpu_temp", f"{cpu_temp:.1f}")

        # GPU 数据
        gpu_usage = self.get_gpu_usage()
        gpu_temp = self.get_gpu_temp()
        print(f"GPU 使用率: {gpu_usage:.1f}% | 温度: {gpu_temp:.1f}°C")
        self._send_cmd("gpu_usage", f"{gpu_usage:.1f}")
        self._send_cmd("gpu_temp", f"{gpu_temp:.1f}")

        # 内存数据
        ram_usage = self.get_ram_usage()
        print(f"内存使用率: {ram_usage:.1f}%")
        self._send_cmd("ram_usage", f"{ram_usage:.1f}")

        # 磁盘数据
        disk_c, disk_d = self.get_disk_usage()
        print(f"磁盘使用率: C盘 {disk_c:.1f}% | D盘 {disk_d:.1f}%") 
        self._send_cmd("disk_c", f"{disk_c:.1f}")
        self._send_cmd("disk_d", f"{disk_d:.1f}")

        # 网络数据
        net_up, net_down = self.get_network_speed()
        print(f"网络速度: 上传 {net_up:.2f} MB/s | 下载 {net_down:.2f} MB/s")
        self._send_cmd("net_up", f"{net_up:.2f}")
        self._send_cmd("net_down", f"{net_down:.2f}")
        print("-"*50)

    def send_time_data(self):
        """发送时间数据并打印"""
        if not self.serial_port or not self.serial_port.is_open:
            return
        datetime_str = self.get_current_datetime()
        print(f"当前时间: {datetime_str}") 
        self._send_cmd("datetime", f'"{datetime_str}"')

    # ---------------- 主循环 ----------------
    def run(self, hardware_interval=1, time_interval=0.5):
        """持续发送数据"""
        if not self.serial_port:
            print("串口未初始化，无法运行")
            return

        print(f"开始监控：硬件数据每 {hardware_interval} 秒发送，时间数据每 {time_interval} 秒发送")
        try:
            while True:
                current_time = time.time()

                # 发送硬件数据
                if current_time - self.last_other_send > hardware_interval:
                    self.send_hardware_data()
                    self.last_other_send = current_time

                # 发送时间数据
                if current_time - self.last_time_send > time_interval:
                    self.send_time_data()
                    self.last_time_send = current_time

                time.sleep(0.01)
        except KeyboardInterrupt:
            print("\n程序已停止")
        finally:
            if self.serial_port and self.serial_port.is_open:
                self.serial_port.close()
                print("串口已关闭")
            if self.computer:
                self.computer.Close()
                print("LibreHardwareMonitor 已关闭")

if __name__ == "__main__":
    # 根据实际情况调整参数
    monitor = PCMonitor(
        port='COM3', 
        baudrate=1000000,
        dll_path="./LibreHardwareMonitorLib"  
    )
    monitor.run(hardware_interval=1, time_interval=0.5)