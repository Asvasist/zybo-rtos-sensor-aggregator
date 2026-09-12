/*
 * task_consumer.c
 */
#include "task_consumer.h"

#include <stddef.h>

#include "app_config.h"
#include "console.h"
#include "gpio_drv.h"
#include "sensor_mailbox.h"
#include "task_producer.h"
#include "task_ui.h"
#include "uart_drv.h"
#include "uptime.h"
#include "xadc_drv.h"

#define CONSUMER_NUM_BUF_LEN    16U

typedef struct
{
    bool            stream_enabled;
    bool            stream_slow;        /* print every APP_SLOW_STREAM_DIVIDER-th sample only */
    bool            stall_reported;     /* one warning per stall, not one per timeout          */
    uint32_t        received;           /* samples fetched from the mailbox                    */
    uint32_t        missed;             /* samples overwritten before we got to them           */
    TickType_t      last_sample_tick;
    sensor_record_t latest;             /* latest.seq == 0 until the first sample arrives      */
} consumer_state_t;

static TaskHandle_t     s_consumer_handle;

/* Private to the consumer task, no locking needed. */
static consumer_state_t s_state;

static void consumer_print_time_prefix(uint64_t time_us)
{
    const uint64_t time_ms = time_us / 1000ULL;

    console_printf("[%6lu.%03lu] ", (unsigned long)(time_ms / 1000ULL), (unsigned long)(time_ms % 1000ULL));
}

/* Temperatures to 2 decimals (one LSB is ~0.12 C), supplies to 3 (one LSB is ~0.73 mV). */
static const char *consumer_fmt_sensor(char *buf, xadc_sensor_t sensor, uint16_t raw_code)
{
    const uint32_t decimals = xadc_drv_is_temperature(sensor) ? 2U : 3U;

    return console_fmt_milli(buf, CONSUMER_NUM_BUF_LEN, xadc_drv_code_to_milli(sensor, raw_code), decimals);
}

static unsigned int consumer_btn_level(const sensor_record_t *record, gpio_btn_id_t btn)
{
    return ((record->buttons & SENSOR_BTN_BIT(btn)) != 0U) ? 1U : 0U;
}

static void consumer_print_help(void)
{
    console_write("\nCommands:\n"
                  "  r / BTN4   full sensor report (latest sample + min/max)\n"
                  "  s / BTN5   start/stop telemetry stream\n");
    console_printf("  f          stream rate: every sample (%u Hz) / every %uth (%u Hz)\n",
                   APP_SAMPLE_RATE_HZ, APP_SLOW_STREAM_DIVIDER, APP_SAMPLE_RATE_HZ / APP_SLOW_STREAM_DIVIDER);
    console_write("  t          print the latest sample once\n"
                  "  d          diagnostics (timing, missed samples, stacks, heap)\n"
                  "  h / ?      this help\n\n");
}

static void consumer_print_sample_line(const sensor_record_t *record)
{
    char temp_str[CONSUMER_NUM_BUF_LEN];
    char vccint_str[CONSUMER_NUM_BUF_LEN];
    char vccpint_str[CONSUMER_NUM_BUF_LEN];

    consumer_print_time_prefix(record->capture_us);
    console_printf("#%-6lu T %s C | VCCINT %s V | VCCPINT %s V | BTN4 %u BTN5 %u\n",
                   (unsigned long)record->seq,
                   consumer_fmt_sensor(temp_str,    XADC_SENSOR_DIE_TEMP, record->sensors.raw_code[XADC_SENSOR_DIE_TEMP]),
                   consumer_fmt_sensor(vccint_str,  XADC_SENSOR_VCCINT,   record->sensors.raw_code[XADC_SENSOR_VCCINT]),
                   consumer_fmt_sensor(vccpint_str, XADC_SENSOR_VCCPINT,  record->sensors.raw_code[XADC_SENSOR_VCCPINT]),
                   consumer_btn_level(record, GPIO_BTN4),
                   consumer_btn_level(record, GPIO_BTN5));
}

static void consumer_print_report(void)
{
    const sensor_record_t *const record = &s_state.latest;
    producer_stats_t             prod_stats;
    uint32_t                     sensor;
    char                         now_str[CONSUMER_NUM_BUF_LEN];
    char                         min_str[CONSUMER_NUM_BUF_LEN];
    char                         max_str[CONSUMER_NUM_BUF_LEN];

    if (record->seq == 0U)
    {
        console_write("no samples yet\n");
        return;
    }

    /*
     * Min/max come from the producer, which sees every sample, not from what
     * the consumer happened to receive. They may already include a sample or
     * two newer than the one shown as "now".
     */
    task_producer_get_stats(&prod_stats);

    console_write("\n");
    consumer_print_time_prefix(record->capture_us);
    console_printf("XADC report, sample #%lu (min/max over %lu samples)\n",
                   (unsigned long)record->seq, (unsigned long)prod_stats.samples);
    console_printf("  %-9s %10s %10s %10s   %s\n", "sensor", "now", "min", "max", "raw");

    for (sensor = 0U; sensor < (uint32_t)XADC_SENSOR_COUNT; sensor++)
    {
        const xadc_sensor_t id = (xadc_sensor_t)sensor;

        console_printf("  %-9s %8s %s %8s %s %8s %s   0x%03X\n",
                       xadc_drv_sensor_name(id),
                       consumer_fmt_sensor(now_str, id, record->sensors.raw_code[sensor]),   xadc_drv_sensor_unit(id),
                       consumer_fmt_sensor(min_str, id, prod_stats.sensor_min.raw_code[sensor]), xadc_drv_sensor_unit(id),
                       consumer_fmt_sensor(max_str, id, prod_stats.sensor_max.raw_code[sensor]), xadc_drv_sensor_unit(id),
                       (unsigned int)record->sensors.raw_code[sensor]);
    }
    console_write("\n");
}

static void consumer_print_stats(void)
{
    producer_stats_t        prod_stats;
    uart_drv_err_counters_t uart_errs;
    const uint32_t          now_ms = uptime_ms();

    task_producer_get_stats(&prod_stats);
    uart_drv_get_err_counters(&uart_errs);

    console_write("\nDiagnostics\n");
    console_printf("  uptime          : %lu.%03lu s\n", (unsigned long)(now_ms / 1000U), (unsigned long)(now_ms % 1000U));
    console_printf("  sample period   : %lu us (timer)\n", (unsigned long)prod_stats.timer_period_us);
    console_printf("  producer        : %lu samples, %lu overruns, %lu timeouts\n",
                   (unsigned long)prod_stats.samples,
                   (unsigned long)prod_stats.overruns,
                   (unsigned long)prod_stats.timeouts);

    if (prod_stats.samples >= 2U)
    {
        console_printf("  sample interval : min %lu us, max %lu us\n",
                       (unsigned long)prod_stats.period_min_us, (unsigned long)prod_stats.period_max_us);
    }
    else
    {
        console_write("  sample interval : n/a\n");
    }

    console_printf("  XADC read time  : max %lu us\n", (unsigned long)prod_stats.read_time_max_us);
    console_printf("  consumer        : %lu received, %lu missed, stream %s, %s\n",
                   (unsigned long)s_state.received,
                   (unsigned long)s_state.missed,
                   s_state.stream_enabled ? "on" : "off",
                   s_state.stream_slow ? "slow" : "every sample");
    console_printf("  UART rx errors  : overrun %lu, framing %lu, parity %lu\n",
                   (unsigned long)uart_errs.rx_overrun,
                   (unsigned long)uart_errs.rx_framing,
                   (unsigned long)uart_errs.rx_parity);

#if defined(INCLUDE_uxTaskGetStackHighWaterMark) && (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    /* Minimum free stack seen so far, in words. Anything near zero needs a bigger stack. */
    console_printf("  stack headroom  : producer %lu, ui %lu, consumer %lu words\n",
                   (unsigned long)uxTaskGetStackHighWaterMark(task_producer_handle()),
                   (unsigned long)uxTaskGetStackHighWaterMark(task_ui_handle()),
                   (unsigned long)uxTaskGetStackHighWaterMark(NULL));
#endif

    console_printf("  heap free       : %lu bytes\n\n", (unsigned long)xPortGetFreeHeapSize());
}

static void consumer_handle_sample(void)
{
    sensor_record_t record;
    bool            print_this_one;

    /* Can come up empty: a sample fetched on the previous pass may have set the bit again. */
    if (!sensor_mailbox_fetch_newer(s_state.latest.seq, &record))
    {
        return;
    }

    s_state.latest = record;
    s_state.received++;
    s_state.last_sample_tick = xTaskGetTickCount();
    s_state.stall_reported   = false;

    /* seq counts everything produced, received everything we saw - the difference was overwritten. */
    s_state.missed = record.seq - s_state.received;

    /* LD4 keeps blinking only while timer, ISR, producer and consumer are all alive. */
    if ((s_state.received % APP_HEARTBEAT_SAMPLES) == 0U)
    {
        gpio_drv_led_toggle();
    }

    print_this_one = s_state.stream_enabled &&
                     ((!s_state.stream_slow) || ((record.seq % APP_SLOW_STREAM_DIVIDER) == 0U));
    if (print_this_one)
    {
        consumer_print_sample_line(&record);
    }
}

static void consumer_check_stall(TickType_t stall_ticks)
{
    /*
     * Samples arrive every 100 ms whether the stream is on or not, so a
     * whole second without one means the timer or the producer has stopped.
     * Checked on every wake-up, not only on timeout, so a stream of UI events
     * can't hide it.
     */
    if (s_state.stall_reported || ((xTaskGetTickCount() - s_state.last_sample_tick) < stall_ticks))
    {
        return;
    }

    consumer_print_time_prefix(uptime_us());
    console_printf("WARN: no sample for %u ms (last #%lu)\n", APP_STALL_WARN_MS, (unsigned long)s_state.latest.seq);
    s_state.stall_reported = true;
}

static void consumer_task(void *task_arg)
{
    const TickType_t stall_ticks = pdMS_TO_TICKS(APP_STALL_WARN_MS);
    producer_stats_t prod_stats;
    uint32_t         events;

    (void)task_arg;

    /* Formatting goes through newlib, which may use VFP registers. */
    portTASK_USES_FLOATING_POINT();

    /* The producer has the higher priority, so its timer is already configured by now. */
    task_producer_get_stats(&prod_stats);
    console_printf("scheduler running, sample period %lu us\n", (unsigned long)prod_stats.timer_period_us);
    consumer_print_help();

    s_state.last_sample_tick = xTaskGetTickCount();

    for (;;)
    {
        /* Bits set while we're busy stay pending and are picked up on the next pass. */
        events = 0U;
        if (xTaskNotifyWait(0U, CONSUMER_EVT_ALL, &events, stall_ticks) != pdTRUE)
        {
            events = 0U;
        }

        if ((events & CONSUMER_EVT_SAMPLE) != 0U)
        {
            consumer_handle_sample();
        }

        if ((events & CONSUMER_EVT_STREAM_TOGGLE) != 0U)
        {
            s_state.stream_enabled = !s_state.stream_enabled;
            console_printf("telemetry stream %s\n", s_state.stream_enabled ? "ON" : "OFF");
        }

        if ((events & CONSUMER_EVT_RATE_TOGGLE) != 0U)
        {
            s_state.stream_slow = !s_state.stream_slow;
            console_printf("stream rate: %s\n", s_state.stream_slow ? "slow" : "every sample");
        }

        if ((events & CONSUMER_EVT_PRINT_ONE) != 0U)
        {
            if (s_state.latest.seq != 0U)
            {
                consumer_print_sample_line(&s_state.latest);
            }
        }

        if ((events & CONSUMER_EVT_REPORT) != 0U)
        {
            consumer_print_report();
        }

        if ((events & CONSUMER_EVT_STATS) != 0U)
        {
            consumer_print_stats();
        }

        if ((events & CONSUMER_EVT_HELP) != 0U)
        {
            consumer_print_help();
        }

        consumer_check_stall(stall_ticks);
    }
}

bool task_consumer_create(void)
{
    s_state.stream_enabled = true;
    s_state.stream_slow    = false;

    return xTaskCreate(consumer_task, "consumer", APP_STACK_CONSUMER, NULL,
                       APP_PRIO_CONSUMER, &s_consumer_handle) == pdPASS;
}

void task_consumer_post_event(uint32_t event_bits)
{
    if (s_consumer_handle != NULL)
    {
        (void)xTaskNotify(s_consumer_handle, event_bits, eSetBits);
    }
}

TaskHandle_t task_consumer_handle(void)
{
    return s_consumer_handle;
}
