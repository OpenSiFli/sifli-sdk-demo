#include "screen.h"
#include <finsh.h> 
#include <rtthread.h>

void sys_set_cmd(int argc, char **argv)
{
    if (argc != 3) return;

    const char *key = argv[1];
    const char *value = argv[2];
    if (strcmp(key, "cpu_usage") == 0) {
        system_status.cpu_usage = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "cpu_temp") == 0) {
        system_status.cpu_temp = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "gpu_usage") == 0) {
        system_status.gpu_usage = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "gpu_temp") == 0) {
        system_status.gpu_temp = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "ram_usage") == 0) {
        system_status.ram_usage = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "disk_c") == 0) {
        system_status.disk_c = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "disk_d") == 0) {
        system_status.disk_d = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "net_up") == 0) {
        system_status.net_up = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "net_down") == 0) {
        system_status.net_down = atof(value);
        system_status.has_monitor_data = true;
    }
    else if (strcmp(key, "datetime") == 0) {
        strncpy(system_status.datetime, value, sizeof(system_status.datetime)-1);
        system_status.datetime[sizeof(system_status.datetime)-1] = '\0';
        system_status.has_time_data = true;
    }else {
        rt_kprintf("Unknown key: %s\n", key);
        return;
    }
    rt_kprintf("Set %s = %s success\n", key, value);
}
MSH_CMD_EXPORT(sys_set_cmd, Set system status variables);

