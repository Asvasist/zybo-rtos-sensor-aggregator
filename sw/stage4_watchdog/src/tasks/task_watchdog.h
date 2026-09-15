/*
 * task_watchdog.h
 *
 * Watchdog supervisor - the only code that kicks the SWDT.
 *
 * Each monitored task calls task_watchdog_checkin() from its main loop. The
 * supervisor runs every APP_WDT_CHECK_PERIOD_MS at the highest application
 * priority and kicks only if every task checked in within its limit. As soon
 * as one misses, it stops kicking for good and the SWDT resets the board.
 *
 * Kicking from the lowest priority task alone would catch a task spinning in
 * a loop above it, but not one that is blocked forever (deadlock, a wait
 * that never ends). The check-ins cover both. If the kernel itself stops -
 * interrupts off, scheduler gone - the supervisor doesn't run either and the
 * SWDT takes care of it without any help.
 */
#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_config.h"

typedef enum
{
    WDOG_CLIENT_PRODUCER = 0,
    WDOG_CLIENT_UI,
    WDOG_CLIENT_CONSUMER,
    WDOG_CLIENT_COUNT
} wdog_client_t;

typedef struct
{
    uint32_t timeout_ms;                            /* SWDT timeout, 0 if not running       */
    uint32_t kicks;
    uint32_t worst_silence_ms[WDOG_CLIENT_COUNT];   /* longest time since a check-in seen   */
} wdog_stats_t;

bool task_watchdog_create(void);

/* Called by each client from its own task only. */
void task_watchdog_checkin(wdog_client_t client);

void         task_watchdog_get_stats(wdog_stats_t *stats_out);
const char  *task_watchdog_client_name(wdog_client_t client);
TaskHandle_t task_watchdog_handle(void);

#if (APP_WDT_TEST_COMMANDS != 0)
/*
 * Bring-up test: the client hangs at its next check-in. The producer and the
 * consumer spin in a busy loop, the UI task blocks forever.
 */
void task_watchdog_inject_hang(wdog_client_t client);
#endif

#endif /* TASK_WATCHDOG_H */
