/*
 * task_consumer.h
 *
 * Consumer: the lowest priority application task, and the console
 * gatekeeper - the only task that writes to the UART once the scheduler is
 * running. Anything that wants something printed sends it a message.
 *
 * Inputs:
 *   - its message queue: commands from the UI task, and the producer's
 *     DATA_READY doorbell
 *   - the sensor log, drained in batches after every wake-up
 *
 * Data and control travel separately on purpose. Samples live in the log,
 * where a slow consumer only makes the backlog grow. The queue carries short
 * control messages which, unlike stage 2's notification bits, don't merge
 * when sent twice and can carry a payload (the key that was pressed).
 */
#ifndef TASK_CONSUMER_H
#define TASK_CONSUMER_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

typedef enum
{
    CONSUMER_MSG_DATA_READY = 0,    /* producer: the log went from empty to non-empty */
    CONSUMER_MSG_REPORT,            /* full sensor report                             */
    CONSUMER_MSG_STREAM_TOGGLE,     /* stream on/off                                  */
    CONSUMER_MSG_RATE_TOGGLE,       /* every sample / every Nth sample                */
    CONSUMER_MSG_PAUSE_TOGGLE,      /* stop/resume draining the log                   */
    CONSUMER_MSG_PRINT_NEWEST,      /* newest sample once, stream or not              */
    CONSUMER_MSG_STATS,             /* diagnostics                                    */
    CONSUMER_MSG_HELP,
    CONSUMER_MSG_UNKNOWN_KEY,       /* key carries the character                      */
    CONSUMER_MSG_WDT_TEST           /* key '1'..'4' selects the hang to inject        */
} consumer_msg_id_t;

typedef struct
{
    consumer_msg_id_t id;
    char              key;          /* terminal key behind the message, '\0' for buttons and the producer */
} consumer_msg_t;

/* Creates the queue and the task. Call from main() before the scheduler starts. */
bool task_consumer_create(void);

/*
 * Queues a message for the consumer. Never blocks - returns false if the
 * queue is full, and the caller decides what that means. Task context only.
 */
bool task_consumer_send(consumer_msg_id_t id, char key);

TaskHandle_t task_consumer_handle(void);

#endif /* TASK_CONSUMER_H */
