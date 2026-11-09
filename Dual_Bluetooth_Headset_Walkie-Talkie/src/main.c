/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>

#include "bf0_hal.h"
#include "drv_io.h"

#include "app_board.h"
#include "app_bt.h"
#include "app_i2s.h"
#include "app_state.h"
#if !defined(SF32LB57X)
#include "app_source.h"
#endif

void HAL_MspInit(void)
{
    BSP_IO_Init();
}

static void app_start_relay(void)
{
    int result = app_i2s_init();

    if (result != RT_EOK)
    {
        rt_kprintf("[intercom] I2S initialization failed: %d\n", result);
        app_state_set_mode(APP_MODE_ERROR);
        return;
    }

    result = app_bt_init();
    if (result != RT_EOK)
    {
        rt_kprintf("[intercom] Bluetooth initialization failed: %d\n", result);
        app_state_set_mode(APP_MODE_ERROR);
        return;
    }

    rt_kprintf("[intercom] relay ready; use 'intercom scan'\n");
}

int main(void)
{
    app_role_t role = app_board_init();
    app_state_init(role);

#if defined(SF32LB57X)
    rt_kprintf("[intercom] 57Z relay, role=%s\n", app_state_role_name(role));
    app_start_relay();
#else
    rt_kprintf("[intercom] PA39=%s, role=%s\n",
               (role == APP_ROLE_SOURCE) ? "high" : "low",
               app_state_role_name(role));

    if (role == APP_ROLE_SOURCE)
    {
        int result = app_source_init();
        if (result != RT_EOK)
        {
            rt_kprintf("[intercom] source initialization failed: %d\n", result);
            app_state_set_mode(APP_MODE_ERROR);
        }
        else
        {
            rt_kprintf("[intercom] source ready; use 'intercom play'\n");
        }
    }
    else
    {
        rt_kprintf("[intercom] dual-headset relay starting\n");
        app_start_relay();
    }
#endif

    rt_kprintf("[intercom] command shell ready; run 'intercom status'\n");
    while (1)
    {
        rt_thread_mdelay(10000);
    }
}
