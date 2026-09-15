/*
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Failure-path compatibility for the verified F6D_A libliot_os.a.
 */

#include "liot_os.h"
#include "FreeRTOS.h"
#include "task.h"

extern LiotOSStatus_t __real_liot_rtos_task_create(
    liot_task_t *task, uint32 stack_size, uint8 priority, char *name,
    void (*entry)(void *), void *argument, ...);

LiotOSStatus_t __wrap_liot_rtos_task_create(
    liot_task_t *task, uint32 stack_size, uint8 priority, char *name,
    void (*entry)(void *), void *argument, ...)
{
    /* This exact archive consumes only the six fixed arguments. Its
     * xTaskCreate failure returns TASK_CREATE_FAIL without balancing the
     * preceding vTaskSuspendAll. Other failures occur before that suspend;
     * success already resumes it. The build checks the archive identity. */
    LiotOSStatus_t ret = __real_liot_rtos_task_create(
        task, stack_size, priority, name, entry, argument);

    if (ret == (LiotOSStatus_t)LIOT_OSI_TASK_CREATE_FAIL)
    {
        (void)xTaskResumeAll();
    }
    return ret;
}
