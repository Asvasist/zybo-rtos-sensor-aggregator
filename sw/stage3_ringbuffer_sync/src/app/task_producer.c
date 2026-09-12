/*
 * task_producer.c
 */
#include "task_producer.h"

#include <stddef.h>

#include "app_config.h"
#include "fault.h"
#include "gpio_drv.h"
#include "sample_timer.h"
#include "sensor_mailbox.h"
#include "task_consumer.h"
#include "uptime.h"

static TaskHandle_t     s_producer_handle;

/* Written by the producer only, always inside a critical section so readers get a consistent copy. */
static producer_stats_t s_stats;

/* Runs in IRQ context, called from the TTC interrupt handler. */
static void producer_timer_tick_cb(void *cb_ctx)
{
    BaseType_t higher_prio_task_woken = pdFALSE;

    (void)cb_ctx;

    /*
     * Notifications used as a counting semaphore: a tick that arrives while
     * the producer is still busy with the previous one isn't lost, it shows
     * up as a count above one and gets reported as an overrun.
     */
    xTaskNotifyGiveFromISR(s_producer_handle, &higher_prio_task_woken);

    /* Switch straight to the producer on IRQ exit rather than on the next tick. */
    portYIELD_FROM_ISR(higher_prio_task_woken);
}

static uint8_t producer_read_buttons(void)
{
    uint8_t  button_bits = 0U;
    uint32_t btn;

    for (btn = 0U; btn < (uint32_t)GPIO_BTN_COUNT; btn++)
    {
        if (gpio_drv_btn_is_down((gpio_btn_id_t)btn))
        {
            button_bits |= (uint8_t)SENSOR_BTN_BIT(btn);
        }
    }

    return button_bits;
}

static void producer_update_stats(const sensor_record_t *record, uint64_t prev_capture_us,
                                  uint32_t read_time_us, uint32_t missed_ticks)
{
    uint32_t sensor;

    taskENTER_CRITICAL();

    s_stats.samples++;
    s_stats.overruns += missed_ticks;

    if (read_time_us > s_stats.read_time_max_us)
    {
        s_stats.read_time_max_us = read_time_us;
    }

    if (prev_capture_us != 0U)
    {
        const uint32_t interval_us = (uint32_t)(record->capture_us - prev_capture_us);

        if (interval_us < s_stats.period_min_us)
        {
            s_stats.period_min_us = interval_us;
        }
        if (interval_us > s_stats.period_max_us)
        {
            s_stats.period_max_us = interval_us;
        }
    }

    if (s_stats.samples == 1U)
    {
        s_stats.sensor_min = record->sensors;
        s_stats.sensor_max = record->sensors;
    }
    else
    {
        for (sensor = 0U; sensor < (uint32_t)XADC_SENSOR_COUNT; sensor++)
        {
            const uint16_t code = record->sensors.raw_code[sensor];

            if (code < s_stats.sensor_min.raw_code[sensor])
            {
                s_stats.sensor_min.raw_code[sensor] = code;
            }
            if (code > s_stats.sensor_max.raw_code[sensor])
            {
                s_stats.sensor_max.raw_code[sensor] = code;
            }
        }
    }

    taskEXIT_CRITICAL();
}

static void producer_task(void *task_arg)
{
    const TickType_t tick_wait_timeout = pdMS_TO_TICKS(APP_SAMPLE_TIMEOUT_MS);
    sensor_record_t  record;
    uint64_t         prev_capture_us = 0U;
    uint32_t         pending_ticks;
    uint32_t         read_time_us;
    int32_t          status;

    (void)task_arg;

    /*
     * GCC and newlib's memcpy are free to use VFP registers even in integer
     * code (struct copies, for one), so every task gets an FPU context.
     */
    portTASK_USES_FLOATING_POINT();

    /* Has to happen here, not in main(): the port sets up the GIC when the scheduler starts. */
    status = (int32_t)sample_timer_init(APP_SAMPLE_RATE_HZ, producer_timer_tick_cb, NULL);
    if (status != (int32_t)SAMPLE_TIMER_OK)
    {
        fault_halt("sample timer init", status);
    }

    taskENTER_CRITICAL();
    s_stats.timer_period_us = sample_timer_period_us();
    taskEXIT_CRITICAL();

    record.seq = 0U;
    sample_timer_start();

    for (;;)
    {
        pending_ticks = ulTaskNotifyTake(pdTRUE, tick_wait_timeout);

        if (pending_ticks == 0U)
        {
            /* The timer has stopped firing. Keep waiting; the consumer reports the stall. */
            taskENTER_CRITICAL();
            s_stats.timeouts++;
            taskEXIT_CRITICAL();
            continue;
        }

        record.capture_us = uptime_us();
        xadc_drv_read_all(&record.sensors);
        record.buttons = producer_read_buttons();
        record.seq++;

        sensor_mailbox_post(&record);
        task_consumer_post_event(CONSUMER_EVT_SAMPLE);

        read_time_us = (uint32_t)(uptime_us() - record.capture_us);
        producer_update_stats(&record, prev_capture_us, read_time_us, pending_ticks - 1U);
        prev_capture_us = record.capture_us;
    }
}

bool task_producer_create(void)
{
    s_stats.period_min_us = UINT32_MAX;

    return xTaskCreate(producer_task, "producer", APP_STACK_PRODUCER, NULL,
                       APP_PRIO_PRODUCER, &s_producer_handle) == pdPASS;
}

void task_producer_get_stats(producer_stats_t *stats_out)
{
    if (stats_out == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *stats_out = s_stats;
    taskEXIT_CRITICAL();
}

TaskHandle_t task_producer_handle(void)
{
    return s_producer_handle;
}
