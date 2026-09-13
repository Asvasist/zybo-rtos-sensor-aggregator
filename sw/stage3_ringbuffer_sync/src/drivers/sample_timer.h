/*
 * sample_timer.h
 *
 * Periodic interrupt from one counter of the PS triple timer counter (TTC0),
 * used as the time base for sensor sampling.
 *
 * Why a hardware timer rather than vTaskDelayUntil(): the sample period then
 * comes from a crystal-derived divider and doesn't care about the RTOS tick
 * rate, and the ISR can wake the producer the moment the period expires
 * instead of on the next tick boundary.
 *
 * The callback runs in IRQ context. Keep it to FreeRTOS "FromISR" calls and
 * plain integer code - no printing, no library calls, no floating point (the
 * port doesn't save the FPU registers on IRQ entry).
 *
 * The interrupt is registered through the FreeRTOS port, which owns the GIC
 * instance and only sets it up inside vTaskStartScheduler(). So
 * sample_timer_init() must be called from a task, not from main().
 */
#ifndef SAMPLE_TIMER_H
#define SAMPLE_TIMER_H

#include <stdint.h>

typedef void (*sample_timer_cb_t)(void *cb_ctx);

typedef enum
{
    SAMPLE_TIMER_OK = 0,
    SAMPLE_TIMER_ERR_PARAM,
    SAMPLE_TIMER_ERR_LOOKUP,    /* TTC instance missing - TTC0 not enabled in the XSA? */
    SAMPLE_TIMER_ERR_INIT,
    SAMPLE_TIMER_ERR_RATE,      /* rate not reachable with 16-bit counter + prescaler */
    SAMPLE_TIMER_ERR_IRQ,       /* handler could not be installed on the GIC          */
    SAMPLE_TIMER_ERR_BUSY       /* counter already running - claimed as the RTOS tick? */
} sample_timer_status_t;

/* Configures the counter for rate_hz in interval mode and hooks up the IRQ. Doesn't start it. */
sample_timer_status_t sample_timer_init(uint32_t rate_hz, sample_timer_cb_t tick_cb, void *cb_ctx);

void sample_timer_start(void);
void sample_timer_stop(void);

/*
 * Period the counter actually runs at, in microseconds, computed back from
 * the divider the driver picked. Can differ slightly from 1/rate_hz because
 * the divider is integer; on the Zybo Z7 at 10 Hz it comes out at 100000 us.
 */
uint32_t sample_timer_period_us(void);

#endif /* SAMPLE_TIMER_H */
