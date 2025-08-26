#include "rtthread.h"
#include "lvgl.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "lv_ex_data.h"
#include "screen.h"
#include "button.h"
#include "littlevgl2rtt.h"


int main(void)
{
    rt_err_t ret = RT_EOK;
    rt_uint32_t ms;

    /* init littlevGL */
    ret = littlevgl2rtt_init("lcd");
    if (ret != RT_EOK)
    {
        return ret;
    }
    lv_ex_data_pool_init();
    lv_init();
    
    
    /* Create a multi-screen UI */
    create_multi_screen_ui();


    while (1)
    {
        ms = lv_task_handler();
        rt_thread_mdelay(ms);
    }
    return RT_EOK;
}
