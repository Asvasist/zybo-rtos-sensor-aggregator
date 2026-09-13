/*
 * main.c - Stage 1: bare-metal bring-up of UART, MIO GPIO and XADC.
 *
 * Super-loop, no interrupts. The goal of this stage is to prove each
 * peripheral on its own before the RTOS is added on top:
 *
 *   - UART : banner, periodic telemetry, single-key commands from the terminal
 *   - GPIO : LD4 heartbeat, BTN4/BTN5 debounced press events
 *   - XADC : die temperature and supply rails, current and hardware min/max
 *
 * Console: 115200 8N1 on the PROG/UART micro-USB port.
 *
 *   BTN4 / 'r'  full sensor report
 *   BTN5 / 's'  start/stop the 1 s telemetry stream
 *   't'         one telemetry line now
 *   'd'         diagnostics (uptime, UART line errors, button levels)
 *   'h' / '?'   help
 */
#include <stdbool.h>
#include <stdint.h>

/* XTime_GetTime(): xtime_l.h in the classic BSP, the xiltimer library in the SDT flow. */
#ifdef SDT
#include "xiltimer.h"
#else
#include "xtime_l.h"
#endif
#include "sleep.h"

#include "board_zybo.h"
#include "uart_drv.h"
#include "gpio_drv.h"
#include "xadc_drv.h"
#include "console.h"

#define APP_FW_VERSION              "0.1.0"

/*
 * Loop timing. These are soft periods - a long UART print (a full report is
 * ~800 chars, ~70 ms at 115200) stretches them. That's acceptable here; the
 * 100 ms sample period in stage 2 comes from a hardware timer interrupt.
 */
#define APP_BTN_SCAN_PERIOD_MS      10U
#define APP_STREAM_PERIOD_MS        1000U
#define APP_HEARTBEAT_PERIOD_MS     500U

/* LED blink half-period used to signal a fatal init error. */
#define APP_FAULT_BLINK_US          100000U

#define APP_NUM_BUF_LEN             16U

static bool s_stream_enabled = true;

/*
 * Milliseconds since boot from the Cortex-A9 global timer. Wraps after ~49
 * days; every comparison below uses unsigned subtraction so the wrap is
 * harmless. The extra parentheses matter: xiltimer defines COUNTS_PER_SECOND
 * as "XPAR_CPU_CORE_CLOCK_FREQ_HZ/2" with none of its own.
 */
static uint32_t app_uptime_ms(void)
{
    XTime now_ticks;

    XTime_GetTime(&now_ticks);
    return (uint32_t)(now_ticks / ((COUNTS_PER_SECOND) / 1000U));
}

static bool app_period_elapsed(uint32_t *last_ms, uint32_t period_ms, uint32_t now_ms)
{
    if ((now_ms - *last_ms) < period_ms)
    {
        return false;
    }

    /* Re-arm from "now" rather than last+period: after a stall, skip the
     * missed slots instead of firing them back to back. */
    *last_ms = now_ms;
    return true;
}

/*
 * Nothing sensible to fall back to if a peripheral won't come up. Say why
 * (if the UART is alive to say it) and blink LD4 fast so the failure is
 * obvious even without a terminal attached.
 */
static void app_fatal(const char *what, int code)
{
    console_printf("\nFATAL: %s failed (code %d), halting.\n", what, code);
    uart_drv_wait_tx_idle();

    for (;;)
    {
        gpio_drv_led_toggle();
        usleep(APP_FAULT_BLINK_US);
    }
}

static void app_print_uptime_prefix(uint32_t now_ms)
{
    console_printf("[%6lu.%03lu] ", (unsigned long)(now_ms / 1000U), (unsigned long)(now_ms % 1000U));
}

/* Temperatures to 2 decimals (one LSB is ~0.12 C), supplies to 3 (one LSB is ~0.73 mV). */
static const char *app_fmt_sensor(char *buf, xadc_sensor_t sensor, uint16_t raw_code)
{
    const uint32_t decimals = xadc_drv_is_temperature(sensor) ? 2U : 3U;

    return console_fmt_milli(buf, APP_NUM_BUF_LEN, xadc_drv_code_to_milli(sensor, raw_code), decimals);
}

static void app_print_help(void)
{
    console_write("\nCommands:\n"
                  "  r / BTN4   full sensor report\n"
                  "  s / BTN5   start/stop 1 s telemetry stream\n"
                  "  t          one telemetry line now\n"
                  "  d          diagnostics\n"
                  "  h / ?      this help\n\n");
}

static void app_print_telemetry_line(void)
{
    xadc_sample_t sample;
    char          temp_str[APP_NUM_BUF_LEN];
    char          vccint_str[APP_NUM_BUF_LEN];
    char          vccpint_str[APP_NUM_BUF_LEN];

    xadc_drv_read_all(&sample);

    app_print_uptime_prefix(app_uptime_ms());
    console_printf("T %s C | VCCINT %s V | VCCPINT %s V | BTN4 %u BTN5 %u\n",
                   app_fmt_sensor(temp_str,    XADC_SENSOR_DIE_TEMP, sample.raw_code[XADC_SENSOR_DIE_TEMP]),
                   app_fmt_sensor(vccint_str,  XADC_SENSOR_VCCINT,   sample.raw_code[XADC_SENSOR_VCCINT]),
                   app_fmt_sensor(vccpint_str, XADC_SENSOR_VCCPINT,  sample.raw_code[XADC_SENSOR_VCCPINT]),
                   gpio_drv_btn_is_down(GPIO_BTN4) ? 1U : 0U,
                   gpio_drv_btn_is_down(GPIO_BTN5) ? 1U : 0U);
}

static void app_print_full_report(void)
{
    xadc_sample_t sample;
    uint32_t      sensor;
    uint16_t      min_code;
    uint16_t      max_code;
    char          now_str[APP_NUM_BUF_LEN];
    char          min_str[APP_NUM_BUF_LEN];
    char          max_str[APP_NUM_BUF_LEN];

    xadc_drv_read_all(&sample);

    console_write("\n");
    app_print_uptime_prefix(app_uptime_ms());
    console_write("XADC report (min/max tracked by XADC since reset)\n");
    console_printf("  %-9s %10s %10s %10s   %s\n", "sensor", "now", "min", "max", "raw");

    for (sensor = 0U; sensor < (uint32_t)XADC_SENSOR_COUNT; sensor++)
    {
        const xadc_sensor_t id = (xadc_sensor_t)sensor;

        xadc_drv_read_min_max(id, &min_code, &max_code);

        console_printf("  %-9s %8s %s %8s %s %8s %s   0x%03X\n",
                       xadc_drv_sensor_name(id),
                       app_fmt_sensor(now_str, id, sample.raw_code[sensor]), xadc_drv_sensor_unit(id),
                       app_fmt_sensor(min_str, id, min_code),                xadc_drv_sensor_unit(id),
                       app_fmt_sensor(max_str, id, max_code),                xadc_drv_sensor_unit(id),
                       (unsigned int)sample.raw_code[sensor]);
    }
    console_write("\n");
}

static void app_print_diagnostics(void)
{
    uart_drv_err_counters_t uart_errs;
    const uint32_t          now_ms = app_uptime_ms();

    uart_drv_get_err_counters(&uart_errs);

    console_write("\nDiagnostics\n");
    console_printf("  uptime        : %lu.%03lu s\n", (unsigned long)(now_ms / 1000U), (unsigned long)(now_ms % 1000U));
    console_printf("  stream        : %s\n", s_stream_enabled ? "on" : "off");
    console_printf("  BTN4 / BTN5   : %u / %u\n",
                   gpio_drv_btn_is_down(GPIO_BTN4) ? 1U : 0U,
                   gpio_drv_btn_is_down(GPIO_BTN5) ? 1U : 0U);
    console_printf("  UART rx errors: overrun %lu, framing %lu, parity %lu\n\n",
                   (unsigned long)uart_errs.rx_overrun,
                   (unsigned long)uart_errs.rx_framing,
                   (unsigned long)uart_errs.rx_parity);
}

static void app_toggle_stream(void)
{
    s_stream_enabled = !s_stream_enabled;
    console_printf("telemetry stream %s\n", s_stream_enabled ? "ON" : "OFF");
}

static void app_handle_command(char cmd)
{
    switch (cmd)
    {
    case 'r':
    case 'R':
        app_print_full_report();
        break;

    case 's':
    case 'S':
        app_toggle_stream();
        break;

    case 't':
    case 'T':
        app_print_telemetry_line();
        break;

    case 'd':
    case 'D':
        app_print_diagnostics();
        break;

    case 'h':
    case 'H':
    case '?':
        app_print_help();
        break;

    case '\r':
    case '\n':
    case ' ':
        /* terminals send these on Enter; ignore quietly */
        break;

    default:
        if ((cmd >= ' ') && (cmd <= '~'))
        {
            console_printf("unknown command '%c', 'h' for help\n", cmd);
        }
        break;
    }
}

int main(void)
{
    uint32_t last_btn_scan_ms;
    uint32_t last_stream_ms;
    uint32_t last_heartbeat_ms;
    int      status;

    /*
     * GPIO first so a failure in anything after it can at least be shown on
     * LD4, then the UART so every later failure can be reported by text.
     */
    status = (int)gpio_drv_init();
    if (status != (int)GPIO_DRV_OK)
    {
        /* No LED and maybe no console either - nothing useful left to do. */
        for (;;)
        {
        }
    }

    status = (int)uart_drv_init(BOARD_CONSOLE_BAUD);
    if (status != (int)UART_DRV_OK)
    {
        app_fatal("UART init", status);
    }

    /*
     * Make sure the global timer is counting before the loop relies on it.
     * The classic BSP starts it in its C startup code; the SDT xiltimer
     * library only starts it on the first sleep call. A 1 us sleep covers
     * both and doesn't depend on some driver happening to sleep first.
     */
    usleep(1U);

    console_write("\n\n"
                  "==================================================\n"
                  " Zybo sensor aggregator - stage 1, bare-metal I/O\n");
    console_printf(" fw %s, built %s %s\n", APP_FW_VERSION, __DATE__, __TIME__);
    console_write("==================================================\n");

    status = (int)xadc_drv_init();
    if (status != (int)XADC_DRV_OK)
    {
        app_fatal("XADC init", status);
    }
    console_write("XADC up: continuous sequencer, 16x averaging, calibration on\n");

    app_print_help();
    app_print_full_report();

    last_btn_scan_ms  = app_uptime_ms();
    last_stream_ms    = last_btn_scan_ms;
    last_heartbeat_ms = last_btn_scan_ms;

    for (;;)
    {
        const uint32_t now_ms = app_uptime_ms();
        char           rx_char;

        if (app_period_elapsed(&last_btn_scan_ms, APP_BTN_SCAN_PERIOD_MS, now_ms))
        {
            gpio_drv_btn_scan();
        }

        if (gpio_drv_btn_take_press(GPIO_BTN4))
        {
            app_print_full_report();
        }

        if (gpio_drv_btn_take_press(GPIO_BTN5))
        {
            app_toggle_stream();
        }

        /* Drain everything the terminal sent since the last pass. */
        while (uart_drv_try_get_char(&rx_char))
        {
            app_handle_command(rx_char);
        }

        if (app_period_elapsed(&last_stream_ms, APP_STREAM_PERIOD_MS, now_ms) && s_stream_enabled)
        {
            app_print_telemetry_line();
        }

        /* Visible sign the loop is alive; also what the watchdog stage will guard. */
        if (app_period_elapsed(&last_heartbeat_ms, APP_HEARTBEAT_PERIOD_MS, now_ms))
        {
            gpio_drv_led_toggle();
        }
    }

    /* not reached */
    return 0;
}
