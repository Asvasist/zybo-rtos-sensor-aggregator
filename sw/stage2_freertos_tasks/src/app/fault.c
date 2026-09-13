/*
 * fault.c
 */
#include "fault.h"

#include "xil_exception.h"

#include "console.h"
#include "gpio_drv.h"
#include "uart_drv.h"
#include "uptime.h"

#define FAULT_BLINK_HALF_PERIOD_US  100000U

/* "-2147483648" plus terminator */
#define FAULT_INT_STR_LEN           12U

static void fault_busy_wait_us(uint32_t delay_us)
{
    const uint64_t start_us = uptime_us();

    /* No RTOS delays and no interrupts in here - the global timer keeps counting regardless. */
    while ((uptime_us() - start_us) < delay_us)
    {
        /* spin */
    }
}

/* printf is off limits here, so a minimal decimal conversion. */
static void fault_write_int(int32_t value)
{
    char     digits[FAULT_INT_STR_LEN];
    char    *cursor = &digits[FAULT_INT_STR_LEN - 1U];
    uint32_t magnitude;

    /* written this way so INT32_MIN doesn't overflow on negation */
    magnitude = (value < 0) ? ((uint32_t)(-(value + 1)) + 1U) : (uint32_t)value;

    *cursor = '\0';
    do
    {
        cursor--;
        *cursor = (char)('0' + (magnitude % 10U));
        magnitude /= 10U;
    } while (magnitude != 0U);

    if (value < 0)
    {
        cursor--;
        *cursor = '-';
    }

    console_write(cursor);
}

static void fault_park(void) __attribute__((noreturn));
static void fault_park(void)
{
    console_write("\nsystem halted\n");
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
    fault_write_int(code);

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
