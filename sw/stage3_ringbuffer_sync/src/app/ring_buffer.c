/*
 * ring_buffer.c
 */
#include "ring_buffer.h"

#include <stddef.h>

/* Keeps head - tail representable even when the buffer is full. */
#define RING_CAPACITY_MAX   0x80000000UL

bool ring_buffer_init(ring_buffer_t *rb, sensor_record_t *storage, uint32_t capacity)
{
    if ((rb == NULL) || (storage == NULL))
    {
        return false;
    }

    if ((capacity == 0U) || ((capacity & (capacity - 1U)) != 0U) || (capacity > RING_CAPACITY_MAX))
    {
        return false;
    }

    rb->storage         = storage;
    rb->capacity        = capacity;
    rb->index_mask      = capacity - 1U;
    rb->head            = 0U;
    rb->tail            = 0U;
    rb->overwritten     = 0U;
    rb->fill_high_water = 0U;
    rb->has_newest      = false;

    return true;
}

uint32_t ring_buffer_count(const ring_buffer_t *rb)
{
    /* Modular arithmetic, correct across the counter wrap. */
    return (rb != NULL) ? (rb->head - rb->tail) : 0U;
}

bool ring_buffer_push(ring_buffer_t *rb, const sensor_record_t *record)
{
    bool     dropped_oldest = false;
    uint32_t fill;

    if ((rb == NULL) || (rb->storage == NULL) || (record == NULL))
    {
        return false;
    }

    if (ring_buffer_count(rb) == rb->capacity)
    {
        /* Full: give up the oldest record. Its slot is the one head is about to reuse. */
        rb->tail++;
        rb->overwritten++;
        dropped_oldest = true;
    }

    rb->storage[rb->head & rb->index_mask] = *record;
    rb->head++;
    rb->has_newest = true;

    fill = ring_buffer_count(rb);
    if (fill > rb->fill_high_water)
    {
        rb->fill_high_water = fill;
    }

    return dropped_oldest;
}

uint32_t ring_buffer_pop(ring_buffer_t *rb, sensor_record_t *records_out, uint32_t max_records)
{
    uint32_t pop_count;
    uint32_t idx;

    if ((rb == NULL) || (rb->storage == NULL) || (records_out == NULL))
    {
        return 0U;
    }

    pop_count = ring_buffer_count(rb);
    if (pop_count > max_records)
    {
        pop_count = max_records;
    }

    for (idx = 0U; idx < pop_count; idx++)
    {
        records_out[idx] = rb->storage[(rb->tail + idx) & rb->index_mask];
    }
    rb->tail += pop_count;

    return pop_count;
}

bool ring_buffer_peek_newest(const ring_buffer_t *rb, sensor_record_t *record_out)
{
    if ((rb == NULL) || (rb->storage == NULL) || (record_out == NULL) || (!rb->has_newest))
    {
        return false;
    }

    *record_out = rb->storage[(rb->head - 1U) & rb->index_mask];
    return true;
}
