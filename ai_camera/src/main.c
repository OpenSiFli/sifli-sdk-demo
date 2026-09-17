/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include "camera_service.h"
#include "camera_ui.h"

int main(void)
{
    int result = camera_service_start();

    if (result != RT_EOK)
    {
        rt_kprintf("camera: service startup failed %d\n", result);
        return result;
    }
    result = camera_ui_start();
    if (result != RT_EOK)
        rt_kprintf("camera: UI startup failed %d\n", result);
    return result;
}
