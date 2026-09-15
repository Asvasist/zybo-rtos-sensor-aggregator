/*
 * task_watchdog.c
 */
#include "task_watchdog.h"

#include <stddef.h>

#include "console.h"
#include "wdt_drv.h"

typedef struct
{
    const char *name;
    uint32_t    limit_ms;
    TickType_t  last_checkin_tick;  /* written by the client only */
    bool        hang_requested;
} wdog_client_state_t;

/*
 * last_checkin_tick starts at 0, which is also where the tick count starts,
 * so every client gets its full limit to check in for the first time.
 */
static wdog_client_state_t s_clients[WDOG_CLIENT_COUNT] =
{
    [WDOG_CLIENT_PRODUCER] = { "producer", APP_WDT_LIMIT_PRODUCER_MS, 0U, false },
    [WDOG_CLIENT_UI]       = { "ui",       APP_WDT_LIMIT_UI_MS,       0U, false },
    [WDOG_CLIENT_CONSUMER] = { "consumer", APP_WDT_LIMIT_CONSUMER_MS, 0U, false },
};

static TaskHandle_t s_wdog_handle;

/* Written by the supervisor only, read through task_watchdog_get_stats(). */
static wdog_stats_t s_stats;

/* Not portTICK_PERIOD_MS: that one is integer ms per tick and becomes 0 above 1000 Hz. */
static uint32_t wdog_ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(((uint64_t)ticks * 1000ULL) / (uint64_t)configTICK_RATE_HZ);
}

#if (APP_WDT_TEST_COMMANDS != 0)
static void wdog_test_hang(wdog_client_t client) __attribute__((noreturn));
static void wdog_test_hang(wdog_client_t client)
{
    if (client == WDOG_CLIENT_UI)
    {
        /* Blocked, not spinning - nothing is using the CPU, the task just never comes back. */
        for (;;)
        {
            vTaskSuspend(NULL);
        }
    }

    for (;;)
    {
        /* spin */
    }
}
#endif

static void wdog_report_late(wdog_client_t client, uint32_t silence_ms)
{
    /*
     * Raw writes only: the consumer owns the formatted console and may well
     * be the task that's stuck. The line can end up in the middle of another
     * one, which is acceptable for the last thing printed before a reset.
     */
    console_write("\n\nWATCHDOG: ");
    console_write(s_clients[client].name);
    console_write(" has not checked in for ");
    console_write_dec(silence_ms);

    if (wdt_drv_is_running())
    {
        console_write(" ms, no more kicks - SWDT reset in ");
        console_write_dec(wdt_drv_timeout_ms());
        console_write(" ms or less\n");
    }
    else
    {
        console_write(" ms (SWDT not running, no reset)\n");
    }
}

static void wdog_task(void *task_arg)
{
    const TickType_t check_period = pdMS_TO_TICKS(APP_WDT_CHECK_PERIOD_MS);
    TickType_t       last_wake    = xTaskGetTickCount();
    bool             kicking      = true;

    (void)task_arg;

    portTASK_USES_FLOATING_POINT();

    for (;;)
    {
        TickType_t    now_tick;
        wdog_client_t late_client = WDOG_CLIENT_COUNT;
        uint32_t      late_ms     = 0U;
        uint32_t      idx;

        vTaskDelayUntil(&last_wake, check_period);
        now_tick = xTaskGetTickCount();

        for (idx = 0U; idx < (uint32_t)WDOG_CLIENT_COUNT; idx++)
        {
            /*
             * No race with the client: nothing below this priority can run
             * between reading now_tick and last_checkin_tick.
             */
            const uint32_t silence_ms = wdog_ticks_to_ms(now_tick - s_clients[idx].last_checkin_tick);

            if (silence_ms > s_stats.worst_silence_ms[idx])
            {
                taskENTER_CRITICAL();
                s_stats.worst_silence_ms[idx] = silence_ms;
                taskEXIT_CRITICAL();
            }

            if ((silence_ms > s_clients[idx].limit_ms) && (late_client == WDOG_CLIENT_COUNT))
            {
                late_client = (wdog_client_t)idx;
                late_ms     = silence_ms;
            }
        }

        if (!kicking)
        {
            continue;
        }

        if (late_client == WDOG_CLIENT_COUNT)
        {
            if (wdt_drv_is_running())
            {
                wdt_drv_kick();

                taskENTER_CRITICAL();
                s_stats.kicks++;
                taskEXIT_CRITICAL();
            }
        }
        else
        {
            /* No second chance: a task that recovers later still missed its deadline. */
            kicking = false;
            wdog_report_late(late_client, late_ms);
        }
    }
}

bool task_watchdog_create(void)
{
    return xTaskCreate(wdog_task, "watchdog", APP_STACK_WATCHDOG, NULL,
                       APP_PRIO_WATCHDOG, &s_wdog_handle) == pdPASS;
}

void task_watchdog_checkin(wdog_client_t client)
{
    if (client >= WDOG_CLIENT_COUNT)
    {
        return;
    }

#if (APP_WDT_TEST_COMMANDS != 0)
    if (s_clients[client].hang_requested)
    {
        wdog_test_hang(client);
    }
#endif

    /* One writer per client and a single 32-bit write, so no locking. */
    s_clients[client].last_checkin_tick = xTaskGetTickCount();
}

void task_watchdog_get_stats(wdog_stats_t *stats_out)
{
    if (stats_out == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *stats_out = s_stats;
    taskEXIT_CRITICAL();

    stats_out->timeout_ms = wdt_drv_timeout_ms();
}

const char *task_watchdog_client_name(wdog_client_t client)
{
    return (client < WDOG_CLIENT_COUNT) ? s_clients[client].name : "?";
}

TaskHandle_t task_watchdog_handle(void)
{
    return s_wdog_handle;
}

#if (APP_WDT_TEST_COMMANDS != 0)
void task_watchdog_inject_hang(wdog_client_t client)
{
    if (client < WDOG_CLIENT_COUNT)
    {
        s_clients[client].hang_requested = true;
    }
}
#endif
