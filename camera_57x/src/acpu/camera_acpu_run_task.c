/*
 * Strong override of the SDK weak acpu_run_task().
 *
 * ACPU software JPEG decode of large photos can exceed the stock 1 s
 * synchronous timeout, which made the SDK implementation assert on HCPU.
 * This override keeps the same IPC protocol but waits up to 5 s.
 */
#include <rtconfig.h>
#include <board.h>
#include <rtthread.h>
#include <string.h>

#include <acpu_ctrl.h>
#include <acpu_ctrl_private.h>
#include <bf0_mbox_common.h>
#include <ipc_queue.h>

#define CAMERA_ACPU_TASK_TIMEOUT_MS 5000U

void *acpu_run_task(uint8_t task_name, void *param, uint32_t param_size,
                    uint8_t *error_code)
{
    rt_err_t err;

    SCB_CleanInvalidateDCache();

#ifdef RT_USING_PM
    rt_pm_request(PM_SLEEP_MODE_IDLE);
#endif /* RT_USING_PM */

    /* Create the blocking semaphore the ACPU response releases. */
    char task_name_str[RT_NAME_MAX];
    rt_snprintf(task_name_str, sizeof(task_name_str), "tsk_%d", task_name);
    rt_sem_t sem = rt_sem_create(task_name_str, 0, RT_IPC_FLAG_FIFO);
    RT_ASSERT(sem);

    /* Send the task to ACPU via the IPC queue. */
    size_t wr_size;
    size_t msg_size;
    acpu_ctrl_ipc_msg_t msg;
    acpu_ctrl_ipc_msg_t *p_msg = &msg;
    msg.is_rsp = 0;
    msg.task_id = task_name;
    msg.task_param_size = param_size;
    msg.task_param = param;
    msg.sema = sem;
    msg.ret_error_code = 0;
    msg.ret_value = 0;

    msg_size = sizeof(acpu_ctrl_ipc_msg_t *);
    wr_size = ipc_queue_write(sys_get_ha_ipc_queue(), &p_msg, msg_size, 1000);
    RT_ASSERT(wr_size == msg_size);

    /* Wait for task completion, then delete the semaphore. */
    err = rt_sem_take(sem,
                      rt_tick_from_millisecond(CAMERA_ACPU_TASK_TIMEOUT_MS));
    RT_ASSERT(RT_EOK == err);
    rt_sem_delete(sem);

    /* Make sure the message was processed by ACPU. */
    RT_ASSERT(1 == msg.is_rsp);
    RT_ASSERT(task_name == msg.task_id);

    if (msg.ret_error_code == (uint8_t)ACPU_ERR_ASSERT)
    {
        rt_kprintf("acpu assert: %s\n", msg.ret_value);
        RT_ASSERT(0);
    }
    if (error_code != RT_NULL)
        *error_code = msg.ret_error_code;

#ifdef RT_USING_PM
    rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif /* RT_USING_PM */

    return (void *)msg.ret_value;
}
