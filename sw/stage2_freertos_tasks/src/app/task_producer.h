/*
 * task_producer.h
 *
 * Producer: the highest priority application task, and the only code that
 * touches the XADC once the scheduler is running.
 *
 * Paced by the TTC sample timer. The timer ISR does nothing but wake this
 * task with a direct-to-task notification; the XADC reads (a handful of
 * XADCIF command/response exchanges) happen here at task level. That keeps
 * the ISR a few instructions long and the slow part pre-emptible.
 *
 * Each sample is posted to the sensor mailbox and the consumer is notified.
 * The producer never waits on the consumer and never prints.
 */
#ifndef TASK_PRODUCER_H
#define TASK_PRODUCER_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "xadc_drv.h"

typedef struct
{
    uint32_t      timer_period_us;  /* actual TTC period                                 */
    uint32_t      samples;          /* samples taken since start                         */
    uint32_t      overruns;         /* timer ticks that arrived while still busy         */
    uint32_t      timeouts;         /* waits in which no tick arrived at all             */
    uint32_t      period_min_us;    /* shortest / longest time between two wake-ups,     */
    uint32_t      period_max_us;    /*   valid once samples >= 2                         */
    uint32_t      read_time_max_us; /* longest wake-to-post time (XADC reads + handoff)  */
    xadc_sample_t sensor_min;       /* per-sensor extremes over every sample taken       */
    xadc_sample_t sensor_max;
} producer_stats_t;

bool task_producer_create(void);

/* Consistent snapshot of the producer's counters; callable from any task. */
void task_producer_get_stats(producer_stats_t *stats_out);

TaskHandle_t task_producer_handle(void);

#endif /* TASK_PRODUCER_H */
