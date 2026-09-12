/*
 * rtos_hooks.c
 *
 * FreeRTOS application hooks.
 *
 * The Xilinx port ships default versions of these (weak in the current BSPs)
 * that print with xil_printf and spin. These replace them so every fatal
 * error ends up in fault_halt(), with the same console message format and
 * LED signal as an init failure.
 *
 * Whether they get called at all depends on the BSP's FreeRTOSConfig.h:
 *   configCHECK_FOR_STACK_OVERFLOW = 2  -> vApplicationStackOverflowHook()
 *   configUSE_MALLOC_FAILED_HOOK   = 1  -> vApplicationMallocFailedHook()
 */
#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"

#include "fault.h"

void vApplicationStackOverflowHook(TaskHandle_t task_handle, char *task_name);
void vApplicationMallocFailedHook(void);

void vApplicationStackOverflowHook(TaskHandle_t task_handle, char *task_name)
{
    (void)task_handle;

    /*
     * Called from the context switch - possibly in IRQ mode - with the
     * offending task's stack already trashed. fault_halt_str() doesn't touch
     * the RTOS or printf, so it's safe here.
     */
    fault_halt_str("stack overflow in task", (task_name != NULL) ? task_name : "?");
}

void vApplicationMallocFailedHook(void)
{
    /*
     * Only startup allocates in this design (tasks, the consumer queue, the
     * log mutex), so this can only trip before the scheduler is running.
     */
    fault_halt_str("heap exhausted", "pvPortMalloc() returned NULL");
}
