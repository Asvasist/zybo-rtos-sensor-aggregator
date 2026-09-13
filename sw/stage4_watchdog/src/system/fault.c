/*
 * fault.c
 */
#include "fault.h"

#include "xil_exception.h"

#include "console.h"
#include "gpio_drv.h"
#include "uart_drv.h"
#include "uptime.h"
#include "wdt_drv.h"

#define FAULT_BLINK_HALF_PERIOD_US  100000U

static void fault_busy_wait_us(uint32_t delay_us)
{
    const uint64_t start_us = uptime_us();

    /* No RTOS delays and no interrupts in here - the global timer keeps counting regardless. */
    while ((uptime_us() - start_us) < delay_us)
    {
        /* spin */
    }
}

static void fault_park(void) __attribute__((noreturn));
static void fault_park(void)
{
    /*
     * With interrupts masked the watchdog supervisor never runs again, so a
     * running SWDT turns this halt into a board reset within its timeout.
     */
    if (wdt_drv_is_running())
    {
        console_write("\nwaiting for the watchdog to reset the board\n");
    }
    else
    {
        console_write("\nsystem halted\n");
    }
    uart_drv_wait_tx_idle();

    /* The fault may predate uptime_init() in main(); the blink timing needs the timer running. */
    (void)uptime_init();

    for (;;)
    {
        gpio_drv_led_toggle();
        fault_busy_wait_us(FAULT_BLINK_HALF_PERIOD_US);
    }
}

void fault_halt(const char *what, int32_t code)
{
    Xil_ExceptionDisable();

    console_write("\n\nFATAL: ");
    console_write(what);
    console_write(" failed, code ");
    if (code < 0)
    {
        console_write("-");
    }
    /* written this way so INT32_MIN doesn't overflow on negation */
    console_write_dec((code < 0) ? ((uint32_t)(-(code + 1)) + 1U) : (uint32_t)code);

    fault_park();
}

void fault_halt_str(const char *what, const char *detail)
{
    Xil_ExceptionDisable();

    console_write("\n\nFATAL: ");
    console_write(what);
    console_write(": ");
    console_write(detail);

    fault_park();
}
