/*
 * main.c - Stage 4: hardware watchdog on top of the stage 3 system.
 *
 * Brings the peripherals and the shared objects up while still
 * single-threaded, creates the tasks, starts the SWDT and hands over to the
 * scheduler.
 *
 *   watchdog  highest  every 250 ms: kicks the SWDT if every task checked in
 *   producer           woken by TTC0 every 100 ms, reads the XADC, writes
 *                      the sample into the sensor log
 *   ui                 10 ms button scan and terminal key input
 *   consumer  lowest   drains the log, formats output, console gatekeeper
 *
 *   TTC0 IRQ --notify--> producer --write--> [ sensor log: ring buffer in DDR ]
 *                           |                [ guarded by a mutex             ]
 *                           |                               | read (batches)
 *                           +-- DATA_READY --+              v
 *                                            +--> queue --> consumer --> UART TX
 *   BTN4/BTN5, UART RX --> ui --- commands --+
 *
 *   producer, ui, consumer --check-in--> watchdog --kick--> SWDT --> PS reset
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
#include "reset_cause.h"
#include "sensor_log.h"
#include "task_consumer.h"
#include "task_producer.h"
#include "task_ui.h"
#include "task_watchdog.h"
#include "uart_drv.h"
#include "uptime.h"
#include "wdt_drv.h"
#include "xadc_drv.h"

/* Catch configuration mistakes at build time instead of as odd behaviour on the board. */
_Static_assert(APP_PRIO_WATCHDOG < configMAX_PRIORITIES,
               "watchdog priority is above configMAX_PRIORITIES - raise it in the BSP settings");
_Static_assert((APP_PRIO_WATCHDOG > APP_PRIO_PRODUCER) && (APP_PRIO_PRODUCER > APP_PRIO_UI) &&
               (APP_PRIO_UI > APP_PRIO_CONSUMER),
               "task priority order is part of the design, see app_config.h");
_Static_assert(pdMS_TO_TICKS(APP_UI_SCAN_PERIOD_MS) > 0U,
               "RTOS tick rate too low for the UI scan period");
_Static_assert(pdMS_TO_TICKS(APP_SAMPLE_TIMEOUT_MS) > pdMS_TO_TICKS(1000U / APP_SAMPLE_RATE_HZ),
               "sample timeout must be longer than the sample period");
_Static_assert((APP_LOG_CAPACITY != 0U) && ((APP_LOG_CAPACITY & (APP_LOG_CAPACITY - 1U)) == 0U),
               "APP_LOG_CAPACITY must be a power of two");
_Static_assert((APP_LOG_READ_BATCH > 0U) && (APP_LOG_READ_BATCH <= APP_LOG_CAPACITY),
               "APP_LOG_READ_BATCH out of range");
_Static_assert((2U * APP_LOG_WRITE_WAIT_MS) < (1000U / APP_SAMPLE_RATE_HZ),
               "producer's mutex wait must stay well inside one sample period");
_Static_assert((2U * APP_WDT_CHECK_PERIOD_MS) < APP_WDT_TIMEOUT_MS,
               "the supervisor must get at least two chances to kick per SWDT timeout");

int main(void)
{
    uintptr_t     log_base;
    uint32_t      log_bytes;
    uint32_t      log_capacity;
    reset_cause_t last_reset;
    int32_t       status;

    /* First thing, before anything else could trigger a reset of its own. */
    last_reset = reset_cause_capture();

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

    /* Before anything takes a timestamp - see uptime.c for why this isn't automatic. */
    if (!uptime_init())
    {
        fault_halt("uptime (global timer) init", 0);
    }

    console_write("\n\n"
                  "========================================================\n"
                  " Zybo sensor aggregator - stage 4, watchdog\n");
    console_printf(" fw %s, built %s %s\n", APP_FW_VERSION, __DATE__, __TIME__);
    console_write("========================================================\n");
    console_printf("last reset: %s\n", reset_cause_name(last_reset));

#if (APP_TEST_LOG_HOLD_US > 0U)
    console_printf("*** TEST BUILD: log mutex held an extra %u us on every read ***\n", APP_TEST_LOG_HOLD_US);
#endif

    status = (int32_t)xadc_drv_init();
    if (status != (int32_t)XADC_DRV_OK)
    {
        fault_halt("XADC init", status);
    }
    console_write("XADC up: continuous sequencer, 16x averaging, calibration on\n");

    status = (int32_t)sensor_log_init();
    if (status != (int32_t)SENSOR_LOG_OK)
    {
        fault_halt("sensor log init", status);
    }
    sensor_log_storage_info(&log_base, &log_bytes, &log_capacity);
    console_printf("sensor log: %lu records, %lu bytes at 0x%08lX (DDR)\n",
                   (unsigned long)log_capacity, (unsigned long)log_bytes, (unsigned long)log_base);

#if (APP_WDT_ENABLE != 0)
    status = (int32_t)wdt_drv_init(APP_WDT_TIMEOUT_MS);
    if (status != (int32_t)WDT_DRV_OK)
    {
        fault_halt("watchdog init", status);
    }
#else
    console_write("*** watchdog DISABLED (APP_WDT_ENABLE) ***\n");
#endif

    /*
     * Nothing runs until vTaskStartScheduler(), so creation order doesn't
     * matter for correctness. The sample timer is deliberately not started
     * here - see sample_timer.h for why it has to wait for the scheduler.
     */
    if (!task_watchdog_create())
    {
        fault_halt("watchdog task create", 0);
    }

    if (!task_consumer_create())
    {
        fault_halt("consumer task/queue create", 0);
    }

    if (!task_ui_create())
    {
        fault_halt("ui task create", 0);
    }

    if (!task_producer_create())
    {
        fault_halt("producer task create", 0);
    }

#if (APP_WDT_ENABLE != 0)
    /* Last step before the scheduler: if the scheduler never gets going, nobody kicks and the board resets. */
    wdt_drv_start();
    console_printf("watchdog: SWDT %lu ms, supervisor every %u ms\n",
                   (unsigned long)wdt_drv_timeout_ms(), APP_WDT_CHECK_PERIOD_MS);
#endif

    console_write("tasks created, starting scheduler\n");

    vTaskStartScheduler();

    /* Only gets here if there wasn't enough heap for the idle or timer service task. */
    fault_halt("scheduler start", 0);
}
