/*
 * task_consumer.c
 */
#include "task_consumer.h"

#include <stddef.h>

#include "queue.h"

#include "app_config.h"
#include "console.h"
#include "gpio_drv.h"
#include "sensor_log.h"
#include "sensor_record.h"
#include "task_producer.h"
#include "task_ui.h"
#include "uart_drv.h"
#include "uptime.h"
#include "xadc_drv.h"

#define CONSUMER_NUM_BUF_LEN    16U

typedef struct
{
    bool       stream_enabled;
    bool       stream_slow;             /* print every APP_SLOW_STREAM_DIVIDER-th sample only   */
    bool       drain_paused;            /* 'p': leave the log alone and let it fill              */
    bool       stall_reported;          /* one warning per stall, not one per pass               */
    uint32_t   received;                /* records taken out of the log                          */
    uint32_t   last_seq;                /* seq of the last record received, 0 before the first   */
    uint32_t   lost;                    /* sequence numbers that never arrived                   */
    uint32_t   log_lock_timeouts;       /* reads that gave up waiting for the mutex              */
    uint32_t   queue_depth_peak;        /* most messages seen waiting at once                    */
    uint32_t   last_producer_samples;   /* for stall detection                                   */
    TickType_t last_progress_tick;
} consumer_state_t;

static TaskHandle_t     s_consumer_handle;
static QueueHandle_t    s_consumer_queue;

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
                  "  r / BTN4   full sensor report (newest sample + min/max)\n"
                  "  s / BTN5   start/stop telemetry stream\n");
    console_printf("  f          stream rate: every sample (%u Hz) / every %uth (%u Hz)\n",
                   APP_SAMPLE_RATE_HZ, APP_SLOW_STREAM_DIVIDER, APP_SAMPLE_RATE_HZ / APP_SLOW_STREAM_DIVIDER);
    console_write("  p          pause/resume reading the log (samples keep accumulating)\n"
                  "  t          print the newest sample once\n"
                  "  d          diagnostics (timing, log, mutex, queue, stacks, heap)\n"
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

/*
 * The report and 't' show the newest sample in the log rather than the last
 * one this task received, so they stay current while draining is paused.
 */
static bool consumer_get_newest(sensor_record_t *record_out)
{
    if (!sensor_log_peek_newest(record_out, pdMS_TO_TICKS(APP_LOG_READ_WAIT_MS)))
    {
        console_write("no samples yet (or log busy)\n");
        return false;
    }

    return true;
}

static void consumer_print_report(void)
{
    sensor_record_t  record;
    producer_stats_t prod_stats;
    uint32_t         sensor;
    char             now_str[CONSUMER_NUM_BUF_LEN];
    char             min_str[CONSUMER_NUM_BUF_LEN];
    char             max_str[CONSUMER_NUM_BUF_LEN];

    if (!consumer_get_newest(&record))
    {
        return;
    }

    /* Min/max come from the producer, which sees every sample, lost or not. */
    task_producer_get_stats(&prod_stats);

    console_write("\n");
    consumer_print_time_prefix(record.capture_us);
    console_printf("XADC report, sample #%lu (min/max over %lu samples)\n",
                   (unsigned long)record.seq, (unsigned long)prod_stats.samples);
    console_printf("  %-9s %10s %10s %10s   %s\n", "sensor", "now", "min", "max", "raw");

    for (sensor = 0U; sensor < (uint32_t)XADC_SENSOR_COUNT; sensor++)
    {
        const xadc_sensor_t id = (xadc_sensor_t)sensor;

        console_printf("  %-9s %8s %s %8s %s %8s %s   0x%03X\n",
                       xadc_drv_sensor_name(id),
                       consumer_fmt_sensor(now_str, id, record.sensors.raw_code[sensor]),        xadc_drv_sensor_unit(id),
                       consumer_fmt_sensor(min_str, id, prod_stats.sensor_min.raw_code[sensor]), xadc_drv_sensor_unit(id),
                       consumer_fmt_sensor(max_str, id, prod_stats.sensor_max.raw_code[sensor]), xadc_drv_sensor_unit(id),
                       (unsigned int)record.sensors.raw_code[sensor]);
    }
    console_write("\n");
}

static void consumer_print_newest(void)
{
    sensor_record_t record;

    if (consumer_get_newest(&record))
    {
        consumer_print_sample_line(&record);
    }
}

static void consumer_print_stats(void)
{
    producer_stats_t        prod_stats;
    sensor_log_stats_t      log_stats;
    uart_drv_err_counters_t uart_errs;
    uintptr_t               log_base;
    uint32_t                log_bytes;
    const uint32_t          now_ms = uptime_ms();

    task_producer_get_stats(&prod_stats);
    uart_drv_get_err_counters(&uart_errs);
    sensor_log_storage_info(&log_base, &log_bytes, NULL);

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
    console_printf("  log write       : max %lu us incl. mutex wait, %lu dropped on lock timeout\n",
                   (unsigned long)prod_stats.log_write_max_us, (unsigned long)prod_stats.log_write_drops);

    if (sensor_log_get_stats(&log_stats, pdMS_TO_TICKS(APP_LOG_READ_WAIT_MS)))
    {
        console_printf("  sensor log      : %lu / %lu records waiting, peak %lu, %lu overwritten\n",
                       (unsigned long)log_stats.fill, (unsigned long)log_stats.capacity,
                       (unsigned long)log_stats.fill_high_water, (unsigned long)log_stats.overwritten);
        console_printf("  log mutex       : held max %lu us, %lu consumer lock timeouts\n",
                       (unsigned long)log_stats.lock_hold_max_us, (unsigned long)s_state.log_lock_timeouts);
    }
    else
    {
        console_write("  sensor log      : mutex busy, try again\n");
    }

    console_printf("  log storage     : %lu bytes at 0x%08lX (DDR)\n", (unsigned long)log_bytes, (unsigned long)log_base);
    console_printf("  consumer        : %lu received, %lu lost, stream %s, %s, draining %s\n",
                   (unsigned long)s_state.received,
                   (unsigned long)s_state.lost,
                   s_state.stream_enabled ? "on" : "off",
                   s_state.stream_slow ? "slow" : "every sample",
                   s_state.drain_paused ? "PAUSED" : "on");
    console_printf("  message queue   : peak %lu / %u, %lu UI messages dropped, %lu doorbell retries\n",
                   (unsigned long)s_state.queue_depth_peak, APP_CONSUMER_QUEUE_LEN,
                   (unsigned long)task_ui_dropped_msgs(), (unsigned long)prod_stats.doorbell_retries);
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

static void consumer_process_record(const sensor_record_t *record)
{
    bool print_this_one;

    s_state.received++;
    s_state.last_seq = record->seq;

    /*
     * seq counts every sample the producer took. Whatever never arrived here
     * was overwritten in a full log or dropped by the producer on a lock
     * timeout, so once the backlog is drained: lost == overwritten + drops.
     */
    s_state.lost = record->seq - s_state.received;

    /* LD4 blinks at 1 Hz while samples flow, holds while paused, flickers while a backlog is drained. */
    if ((s_state.received % APP_HEARTBEAT_SAMPLES) == 0U)
    {
        gpio_drv_led_toggle();
    }

    print_this_one = s_state.stream_enabled &&
                     ((!s_state.stream_slow) || ((record->seq % APP_SLOW_STREAM_DIVIDER) == 0U));
    if (print_this_one)
    {
        consumer_print_sample_line(record);
    }
}

/*
 * Pulls records out of the log in batches and processes them with the mutex
 * released - printing never happens while the lock is held. Stops after
 * APP_LOG_DRAIN_BATCHES_MAX batches so a large backlog can't keep the
 * consumer away from its queue, and returns true to ask for another pass
 * straight away.
 */
static bool consumer_drain_log(void)
{
    const TickType_t lock_wait = pdMS_TO_TICKS(APP_LOG_READ_WAIT_MS);
    sensor_record_t  batch[APP_LOG_READ_BATCH];
    uint32_t         batch_count;
    uint32_t         batch_num;
    uint32_t         idx;

    if (s_state.drain_paused)
    {
        return false;
    }

    for (batch_num = 0U; batch_num < APP_LOG_DRAIN_BATCHES_MAX; batch_num++)
    {
        if (!sensor_log_read(batch, APP_LOG_READ_BATCH, lock_wait, &batch_count))
        {
            s_state.log_lock_timeouts++;
            return true;
        }

        for (idx = 0U; idx < batch_count; idx++)
        {
            consumer_process_record(&batch[idx]);
        }

        if (batch_count < APP_LOG_READ_BATCH)
        {
            /* The log was emptied, so the producer's next write rings the doorbell again. */
            return false;
        }
    }

    return true;
}

static void consumer_handle_msg(const consumer_msg_t *msg)
{
    switch (msg->id)
    {
    case CONSUMER_MSG_DATA_READY:
        /* Nothing to do here: the log is drained after every wake-up, whatever caused it. */
        break;

    case CONSUMER_MSG_REPORT:
        consumer_print_report();
        break;

    case CONSUMER_MSG_STREAM_TOGGLE:
        s_state.stream_enabled = !s_state.stream_enabled;
        console_printf("telemetry stream %s\n", s_state.stream_enabled ? "ON" : "OFF");
        break;

    case CONSUMER_MSG_RATE_TOGGLE:
        s_state.stream_slow = !s_state.stream_slow;
        console_printf("stream rate: %s\n", s_state.stream_slow ? "slow" : "every sample");
        break;

    case CONSUMER_MSG_PAUSE_TOGGLE:
        s_state.drain_paused = !s_state.drain_paused;
        console_printf("log reading %s\n",
                       s_state.drain_paused ? "PAUSED - samples accumulate in the log" : "RESUMED");
        break;

    case CONSUMER_MSG_PRINT_NEWEST:
        consumer_print_newest();
        break;

    case CONSUMER_MSG_STATS:
        consumer_print_stats();
        break;

    case CONSUMER_MSG_HELP:
        consumer_print_help();
        break;

    case CONSUMER_MSG_UNKNOWN_KEY:
        console_printf("unknown command '%c', 'h' for help\n", msg->key);
        break;

    default:
        console_printf("WARN: unexpected message id %d\n", (int)msg->id);
        break;
    }
}

static void consumer_check_stall(void)
{
    const TickType_t stall_ticks = pdMS_TO_TICKS(APP_STALL_WARN_MS);
    const TickType_t now_tick    = xTaskGetTickCount();
    producer_stats_t prod_stats;

    /*
     * Watches the producer's sample count, not what arrives here: with
     * draining paused nothing arrives, and that isn't a stall.
     */
    task_producer_get_stats(&prod_stats);

    if (prod_stats.samples != s_state.last_producer_samples)
    {
        s_state.last_producer_samples = prod_stats.samples;
        s_state.last_progress_tick    = now_tick;
        s_state.stall_reported        = false;
        return;
    }

    if ((!s_state.stall_reported) && ((now_tick - s_state.last_progress_tick) >= stall_ticks))
    {
        consumer_print_time_prefix(uptime_us());
        console_printf("WARN: producer has taken no sample for %u ms (last #%lu)\n",
                       APP_STALL_WARN_MS, (unsigned long)prod_stats.samples);
        s_state.stall_reported = true;
    }
}

static void consumer_task(void *task_arg)
{
    const TickType_t poll_ticks = pdMS_TO_TICKS(APP_CONSUMER_POLL_MS);
    producer_stats_t prod_stats;
    consumer_msg_t   msg;
    UBaseType_t      queue_depth;
    bool             backlog_pending = false;

    (void)task_arg;

    /* Formatting goes through newlib, which may use VFP registers. */
    portTASK_USES_FLOATING_POINT();

    /* The producer has the higher priority, so its timer is already configured by now. */
    task_producer_get_stats(&prod_stats);
    console_printf("scheduler running, sample period %lu us\n", (unsigned long)prod_stats.timer_period_us);
    consumer_print_help();

    s_state.last_progress_tick = xTaskGetTickCount();

    for (;;)
    {
        /*
         * Normally block on the queue. The timeout is a safety net: even if a
         * doorbell were ever lost, the log still gets drained within
         * APP_CONSUMER_POLL_MS. With a backlog, only glance at the queue and
         * go straight back to draining.
         */
        if (xQueueReceive(s_consumer_queue, &msg, backlog_pending ? 0U : poll_ticks) == pdTRUE)
        {
            /* +1 for the message just taken off */
            queue_depth = uxQueueMessagesWaiting(s_consumer_queue) + 1U;
            if (queue_depth > s_state.queue_depth_peak)
            {
                s_state.queue_depth_peak = (uint32_t)queue_depth;
            }

            consumer_handle_msg(&msg);
        }

        backlog_pending = consumer_drain_log();
        consumer_check_stall();
    }
}

bool task_consumer_create(void)
{
    s_state.stream_enabled = true;
    s_state.stream_slow    = false;
    s_state.drain_paused   = false;

    s_consumer_queue = xQueueCreate(APP_CONSUMER_QUEUE_LEN, sizeof(consumer_msg_t));
    if (s_consumer_queue == NULL)
    {
        return false;
    }

#if defined(configQUEUE_REGISTRY_SIZE) && (configQUEUE_REGISTRY_SIZE > 0)
    vQueueAddToRegistry(s_consumer_queue, "consumer_q");
#endif

    return xTaskCreate(consumer_task, "consumer", APP_STACK_CONSUMER, NULL,
                       APP_PRIO_CONSUMER, &s_consumer_handle) == pdPASS;
}

bool task_consumer_send(consumer_msg_id_t id, char key)
{
    const consumer_msg_t msg = { id, key };

    if (s_consumer_queue == NULL)
    {
        return false;
    }

    /* Zero wait: neither the producer nor the UI task may ever block on the consumer. */
    return xQueueSend(s_consumer_queue, &msg, 0U) == pdTRUE;
}

TaskHandle_t task_consumer_handle(void)
{
    return s_consumer_handle;
}
