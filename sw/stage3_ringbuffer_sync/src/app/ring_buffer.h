/*
 * ring_buffer.h
 *
 * Fixed-capacity FIFO of sensor records that overwrites its oldest entry
 * when full.
 *
 * Overwrite rather than reject, because this sits behind a real-time
 * producer: the producer must never have to wait for space, and if data has
 * to be lost the newest samples are the ones worth keeping.
 *
 * head and tail are free-running counters, not array indexes. head counts
 * every record ever pushed, tail every record ever removed (popped or
 * overwritten), so the fill level is simply head - tail. Full and empty are
 * never ambiguous and no slot is wasted telling them apart. The capacity
 * must be a power of two: the array index becomes (counter & mask), and the
 * counters stay consistent across their 2^32 wrap.
 *
 * Not thread-safe, on purpose - the caller owns the locking policy
 * (see sensor_log). No RTOS or Xilinx dependencies either, so it is unit
 * tested on a PC: tests/host/test_ring_buffer.c.
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "sensor_record.h"

/* All fields are read-only outside ring_buffer.c. */
typedef struct
{
    sensor_record_t *storage;
    uint32_t         capacity;          /* power of two                              */
    uint32_t         index_mask;        /* capacity - 1                              */
    uint32_t         head;              /* records pushed, free-running              */
    uint32_t         tail;              /* records popped or overwritten             */
    uint32_t         overwritten;       /* records dropped to make room              */
    uint32_t         fill_high_water;   /* highest fill level seen                   */
    bool             has_newest;        /* at least one record has ever been pushed  */
} ring_buffer_t;

/* storage must hold capacity records. Fails on NULL, zero or non power of two capacity. */
bool ring_buffer_init(ring_buffer_t *rb, sensor_record_t *storage, uint32_t capacity);

/* Appends a record. Returns true if the oldest record had to be dropped to make room. */
bool ring_buffer_push(ring_buffer_t *rb, const sensor_record_t *record);

/* Moves up to max_records of the oldest records into records_out, oldest first. Returns how many. */
uint32_t ring_buffer_pop(ring_buffer_t *rb, sensor_record_t *records_out, uint32_t max_records);

/*
 * Copies the most recently pushed record without removing anything. Still
 * works after that record has been popped: pop doesn't clear slots, and the
 * newest slot isn't reused until the next push. False only if nothing was
 * ever pushed.
 */
bool ring_buffer_peek_newest(const ring_buffer_t *rb, sensor_record_t *record_out);

uint32_t ring_buffer_count(const ring_buffer_t *rb);

#endif /* RING_BUFFER_H */
