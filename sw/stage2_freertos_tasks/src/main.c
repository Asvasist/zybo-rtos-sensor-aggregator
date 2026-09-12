/*
 * main.c - Stage 2: FreeRTOS task architecture.
 *
 * Brings the peripherals up while still single-threaded, creates the three
 * application tasks and hands over to the scheduler.
 *
 *   producer  highest  woken by TTC0 every 100 ms, reads the XADC, posts the
 *                      sample to the mailbox
 *   ui        middle   10 ms button scan and terminal key input
 *   consumer  lowest   formats samples and command output, only UART writer
 *
 *   TTC0 IRQ --notify--> producer --mailbox + notify--> consumer --> UART TX
 *                                                          ^
 *   BTN4/BTN5, UART RX --> ui --------- notify ------------+
 *
 * Console: 115200 8N1 on the PROG/UART micro-USB port, 'h' for commands.
 */
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board_zybo.h"
#include "app_config.h"
#include "console.h"
#include "fault.h"
#include "gpio_drv.h"
#include "task_consumer.h"
#include "task_producer.h"
#include "task_ui.h"
#include "uart_drv.h"
#include "xadc_drv.h"

/* Catch configuration mistakes at build time instead of as odd behaviour on the board. */
_Static_assert(APP_PRIO_PRODUCER < configMAX_PRIORITIES,
               "producer priority is above configMAX_PRIORITIES - raise it in the BSP settings");
_Static_assert((APP_PRIO_PRODUCER > APP_PRIO_UI) && (APP_PRIO_UI > APP_PRIO_CONSUMER),
               "task priority order is part of the design, see app_config.h");
_Static_assert(pdMS_TO_TICKS(APP_UI_SCAN_PERIOD_MS) > 0U,
               "RTOS tick rate too low for the UI scan period");
_Static_assert(pdMS_TO_TICKS(APP_SAMPLE_TIMEOUT_MS) > pdMS_TO_TICKS(1000U / APP_SAMPLE_RATE_HZ),
               "sample timeout must be longer than the sample period");

int main(void)
{
    int32_t status;

    /*
     * GPIO first so a failure in anything after it can at least be shown on
     * LD4, then the UART so every later failure can be reported by text.
     */
    status = (int32_t)gpio_drv_init();
    if (status != (int32_t)GPIO_DRV_OK)
    {
        /* No LED and maybe no console either - nothing useful left to do. */
        for (;;)
        {
        }
    }

    status = (int32_t)uart_drv_init(BOARD_CONSOLE_BAUD);
    if (status != (int32_t)UART_DRV_OK)
    {
        fault_halt("UART init", status);
    }

    console_write("\n\n"
                  "==================================================\n"
                  " Zybo sensor aggregator - stage 2, FreeRTOS tasks\n");
    console_printf(" fw %s, built %s %s\n", APP_FW_VERSION, __DATE__, __TIME__);
    console_write("==================================================\n");

    status = (int32_t)xadc_drv_init();
    if (status != (int32_t)XADC_DRV_OK)
    {
        fault_halt("XADC init", status);
    }
    console_write("XADC up: continuous sequencer, 16x averaging, calibration on\n");

    /*
     * Nothing runs until vTaskStartScheduler(), so creation order doesn't
     * matter for correctness. The sample timer is deliberately not started
     * here - see sample_timer.h for why it has to wait for the scheduler.
     */
    if (!task_consumer_create())
    {
        fault_halt("consumer task create", 0);
    }

    if (!task_ui_create())
    {
        fault_halt("ui task create", 0);
    }

    if (!task_producer_create())
    {
        fault_halt("producer task create", 0);
    }

    console_write("tasks created, starting scheduler\n");

    vTaskStartScheduler();

    /* Only gets here if there wasn't enough heap for the idle or timer service task. */
    fault_halt("scheduler start", 0);
}
