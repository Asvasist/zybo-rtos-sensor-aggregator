/*
 * sensor_mailbox.h
 *
 * Single-slot, newest-value handoff from the producer to the consumer.
 *
 * The producer overwrites the slot on every sample; the consumer copies out
 * whatever is newest when it gets to run. If the consumer falls behind, the
 * samples in between are gone - the sequence number lets it count how many.
 *
 * Access is guarded by a critical section. For a ~32 byte copy that is the
 * cheapest correct option (a few microseconds with interrupts masked), and
 * the producer can never be blocked by the consumer holding a lock.
 *
 * This is the stage 2 stopgap. Stage 3 replaces it with a mutex-protected
 * ring buffer in DDR so that no sample is lost to a slow consumer.
 */
#ifndef SENSOR_MAILBOX_H
#define SENSOR_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "xadc_drv.h"

#define SENSOR_BTN_BIT(btn)     (1U << (btn))

typedef struct
{
    uint64_t      capture_us;   /* uptime when the producer woke for this sample */
    uint32_t      seq;          /* 1 for the first sample, +1 per sample         */
    xadc_sample_t sensors;      /* raw 12-bit XADC codes                         */
    uint8_t       buttons;      /* debounced levels, SENSOR_BTN_BIT(gpio_btn_id) */
} sensor_record_t;

/* Producer side. Overwrites whatever is in the slot. */
void sensor_mailbox_post(const sensor_record_t *record);

/*
 * Consumer side. Copies the slot out if it holds a sample other than
 * last_seq. Returns false if the slot is empty or has nothing new.
 */
bool sensor_mailbox_fetch_newer(uint32_t last_seq, sensor_record_t *record_out);

#endif /* SENSOR_MAILBOX_H */
