/* F6D_A compatibility, gated by the vendor archive SHA-256 in the Makefile.
 * liot_rtos_task_create calls vTaskSuspendAll before xTaskCreate, but skips
 * xTaskResumeAll on TASK_CREATE_FAIL (0x81000002). Restore exactly that one
 * nesting level. Other validation failures never suspended the scheduler.
 * All six documented arguments are forwarded; this SDK ignores varargs. */
#include "liot_os.h"
#include <stdint.h>

extern LiotOSStatus_t __real_liot_rtos_task_create(liot_task_t *, uint32,
    uint8, char *, void (*)(void *), void *, ...);
extern long xTaskResumeAll(void);

LiotOSStatus_t __wrap_liot_rtos_task_create(liot_task_t *task, uint32 stack,
    uint8 priority, char *name, void (*entry)(void *), void *arg, ...)
{
    LiotOSStatus_t status = __real_liot_rtos_task_create(task, stack, priority,
                                                      name, entry, arg);
    if ((uint32_t)status == UINT32_C(0x81000002))
        (void)xTaskResumeAll();
    return status;
}
