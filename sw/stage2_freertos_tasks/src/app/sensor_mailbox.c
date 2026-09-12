/*
 * sensor_mailbox.c
 */
#include "sensor_mailbox.h"

#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"

/* seq == 0 marks the slot as never written. */
static sensor_record_t s_slot;

void sensor_mailbox_post(const sensor_record_t *record)
{
    if (record == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    s_slot = *record;
    taskEXIT_CRITICAL();
}

bool sensor_mailbox_fetch_newer(uint32_t last_seq, sensor_record_t *record_out)
{
    bool has_newer;

    if (record_out == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();

    has_newer = (s_slot.seq != 0U) && (s_slot.seq != last_seq);
    if (has_newer)
    {
        *record_out = s_slot;
    }

    taskEXIT_CRITICAL();

    return has_newer;
}
