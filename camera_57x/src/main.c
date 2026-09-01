#include <rtthread.h>

#include "app/camera_app.h"

int main(void)
{
#ifdef CAMERA_SENSOR_ARDUCAM_OV2640
    rt_kprintf("camera_57x backend=arducam-ov2640 board=sf32lb57-spi-hdk_n16r4\n");
#else
    rt_kprintf("camera_57x backend=gc032a board=sf32lb57-spi-hdk_n16r4\n");
#endif
    return camera_app_start();
}
