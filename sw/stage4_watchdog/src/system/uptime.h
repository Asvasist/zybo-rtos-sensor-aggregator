/*
 * uptime.h
 *
 * Time since boot from the Cortex-A9 global timer.
 *
 * FreeRTOS drives its tick from the private (SCU) timer, which leaves the
 * global timer free-running. At roughly 3 ns per count it gives timestamps
 * far finer than the tick, independent of the tick rate - good enough to
 * measure sampling jitter. Reads are lock-free and safe from any task.
 */
#ifndef UPTIME_H
#define UPTIME_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Makes sure the global timer is counting. Call once from main() before the
 * first timestamp is taken; calling it again later is harmless.
 * Returns false if the timer still isn't advancing afterwards.
 */
bool uptime_init(void);

uint64_t uptime_us(void);

/* Wraps after ~49 days; use unsigned subtraction for intervals. */
uint32_t uptime_ms(void);

#endif /* UPTIME_H */
