/*
 * sensor_record.h
 *
 * One sample as it travels from the producer to the consumer: capture time,
 * sequence number, raw XADC codes and button levels.
 *
 * Deliberately raw and fixed-size - no pointers, copyable by assignment - so
 * records can be stored in bulk in the DDR ring buffer. Conversion to
 * engineering units only happens when something is printed.
 */
#ifndef SENSOR_RECORD_H
#define SENSOR_RECORD_H

#include <stdint.h>

#include "xadc_drv.h"

#define SENSOR_BTN_BIT(btn)     (1U << (btn))

typedef struct
{
    uint64_t      capture_us;   /* uptime when the producer woke for this sample */
    uint32_t      seq;          /* 1 for the first sample, +1 per sample taken   */
    xadc_sample_t sensors;      /* raw 12-bit XADC codes                         */
    uint8_t       buttons;      /* debounced levels, SENSOR_BTN_BIT(gpio_btn_id) */
} sensor_record_t;

/* The log sizing in app_config.h and the Readme assume this. */
_Static_assert(sizeof(sensor_record_t) == 32U, "sensor_record_t size changed - revisit the log sizing");

#endif /* SENSOR_RECORD_H */
