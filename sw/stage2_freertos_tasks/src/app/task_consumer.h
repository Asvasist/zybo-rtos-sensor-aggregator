/*
 * task_consumer.h
 *
 * Consumer: the lowest priority application task, and the only task that
 * writes to the console once the scheduler is running.
 *
 * It sleeps on its task notification value, used as a set of event bits. The
 * producer sets CONSUMER_EVT_SAMPLE after every sample, the UI task sets the
 * others for button presses and terminal keys. One wait covers every source,
 * and whatever has piled up is handled in one pass.
 *
 * Bits don't count. If the same event is posted twice before the consumer
 * gets to run, it's handled once. For "new sample" that's exactly what's
 * wanted - the mailbox only holds the newest one anyway, and the sequence
 * number shows what was skipped. For UI requests it means a very fast double
 * press can collapse into one, which is acceptable for now; stage 3 moves
 * commands onto a queue.
 */
#ifndef TASK_CONSUMER_H
#define TASK_CONSUMER_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#define CONSUMER_EVT_SAMPLE         (1UL << 0)  /* new sample in the mailbox           */
#define CONSUMER_EVT_REPORT         (1UL << 1)  /* full sensor report                  */
#define CONSUMER_EVT_STREAM_TOGGLE  (1UL << 2)  /* stream on/off                       */
#define CONSUMER_EVT_RATE_TOGGLE    (1UL << 3)  /* every sample / every Nth sample     */
#define CONSUMER_EVT_PRINT_ONE      (1UL << 4)  /* latest sample once, stream or not   */
#define CONSUMER_EVT_STATS          (1UL << 5)  /* diagnostics                         */
#define CONSUMER_EVT_HELP           (1UL << 6)

#define CONSUMER_EVT_ALL            (CONSUMER_EVT_SAMPLE | CONSUMER_EVT_REPORT | CONSUMER_EVT_STREAM_TOGGLE | \
                                     CONSUMER_EVT_RATE_TOGGLE | CONSUMER_EVT_PRINT_ONE | CONSUMER_EVT_STATS | \
                                     CONSUMER_EVT_HELP)

bool task_consumer_create(void);

/* Sets event bits for the consumer. Task context only - there is no ISR variant on purpose. */
void task_consumer_post_event(uint32_t event_bits);

TaskHandle_t task_consumer_handle(void);

#endif /* TASK_CONSUMER_H */
