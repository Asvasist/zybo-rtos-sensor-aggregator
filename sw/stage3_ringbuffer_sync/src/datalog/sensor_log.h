/*
 * sensor_log.h
 *
 * The shared sample store between the producer and the consumer: a ring
 * buffer of sensor records in DDR, guarded by a FreeRTOS mutex.
 *
 * Locking rules - these are what make it safe to share:
 *   - every access to the ring goes through the mutex, no exceptions
 *   - the lock is held only while records are copied in or out, never
 *     across anything slow (UART output, XADC reads, blocking calls)
 *   - every take has a bounded wait; the producer's is short, so a reader
 *     that misbehaves can cost it samples but never its timing
 *
 * Why a mutex and not stage 2's critical section: a mutex doesn't mask
 * interrupts, so batch copies of any size have no effect on interrupt
 * latency. It also brings priority inheritance: if the producer wants the
 * lock while the low-priority consumer holds it, the consumer runs at the
 * producer's priority until it gives the lock back, so the UI task (in
 * between) can't stretch the producer's wait.
 */
#ifndef SENSOR_LOG_H
#define SENSOR_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"

#include "sensor_record.h"

typedef enum
{
    SENSOR_LOG_OK = 0,
    SENSOR_LOG_ERR_NOT_DDR,     /* storage linked outside DDR - linker script changed?  */
    SENSOR_LOG_ERR_CAPACITY,    /* APP_LOG_CAPACITY not a power of two                  */
    SENSOR_LOG_ERR_NO_MUTEX     /* not enough heap to create the mutex                  */
} sensor_log_status_t;

typedef struct
{
    uint32_t capacity;
    uint32_t fill;              /* records waiting to be read            */
    uint32_t fill_high_water;   /* worst backlog seen                    */
    uint32_t written;           /* records ever written                  */
    uint32_t read;              /* records ever read                     */
    uint32_t overwritten;       /* records lost because the log was full */
    uint32_t lock_hold_max_us;  /* longest time anyone held the mutex    */
} sensor_log_stats_t;

/* Call once from main(), before the scheduler starts. */
sensor_log_status_t sensor_log_init(void);

/* Where the storage landed. Constant after link, no locking needed. */
void sensor_log_storage_info(uintptr_t *base_addr_out, uint32_t *size_bytes_out, uint32_t *capacity_out);

/*
 * Producer side. Appends one record, overwriting the oldest if the log is
 * full. Returns false if the mutex wasn't obtained within max_wait (record
 * not stored). *was_empty_out tells whether the log held nothing before this
 * write, which is when the consumer needs a doorbell.
 */
bool sensor_log_write(const sensor_record_t *record, TickType_t max_wait, bool *was_empty_out);

/*
 * Consumer side. Moves up to max_records of the oldest records out, oldest
 * first. Returns false if the mutex wasn't obtained within max_wait.
 */
bool sensor_log_read(sensor_record_t *records_out, uint32_t max_records, TickType_t max_wait,
                     uint32_t *count_out);

/* Newest record ever written, read or not. False on lock timeout or if nothing was written yet. */
bool sensor_log_peek_newest(sensor_record_t *record_out, TickType_t max_wait);

bool sensor_log_get_stats(sensor_log_stats_t *stats_out, TickType_t max_wait);

#endif /* SENSOR_LOG_H */
