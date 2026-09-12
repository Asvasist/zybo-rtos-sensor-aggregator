/*
 * xadc_drv.h
 *
 * On-chip sensor readout through the PS-XADC interface (DevC XADCIF).
 *
 * No PL logic is involved: the PS talks to the XADC hard macro over its
 * dedicated serial link, so this works on a bitstream-less, PS-only design.
 *
 * The sequencer runs in continuous mode over the die temperature sensor and the
 * PL/PS supply rails, with 16x averaging and the factory calibration applied.
 * Reads just pick up the latest result from the status registers; nothing here
 * waits for a conversion.
 *
 * Results are handled as raw 12-bit codes and only converted to engineering
 * units at the point of display. That keeps the sampling side cheap and a
 * sample record small, which matters once samples go into the ring buffer.
 *
 * Not reentrant: every register read is a command/response exchange over the
 * XADCIF FIFOs. Once the scheduler is running the producer task is the only
 * caller of the read functions. The conversion and name helpers are pure and
 * can be used from anywhere.
 */
#ifndef XADC_DRV_H
#define XADC_DRV_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    XADC_DRV_OK = 0,
    XADC_DRV_ERR_LOOKUP,
    XADC_DRV_ERR_INIT,
    XADC_DRV_ERR_SELFTEST,
    XADC_DRV_ERR_SEQ_CONFIG
} xadc_drv_status_t;

typedef enum
{
    XADC_SENSOR_DIE_TEMP = 0,   /* on-chip temperature sensor              */
    XADC_SENSOR_VCCINT,         /* PL internal core supply,  1.0 V nominal */
    XADC_SENSOR_VCCAUX,         /* PL auxiliary supply,      1.8 V nominal */
    XADC_SENSOR_VCCBRAM,        /* PL block RAM supply,      1.0 V nominal */
    XADC_SENSOR_VCCPINT,        /* PS internal core supply,  1.0 V nominal */
    XADC_SENSOR_VCCPAUX,        /* PS auxiliary supply,      1.8 V nominal */
    XADC_SENSOR_VCCO_DDR,       /* PS DDR I/O supply                       */
    XADC_SENSOR_COUNT
} xadc_sensor_t;

/* One snapshot of every monitored sensor, as raw 12-bit ADC codes. */
typedef struct
{
    uint16_t raw_code[XADC_SENSOR_COUNT];
} xadc_sample_t;

xadc_drv_status_t xadc_drv_init(void);

/* Latest averaged conversion result for one sensor, 12-bit code (0..4095). */
uint16_t xadc_drv_read_raw(xadc_sensor_t sensor);

/* Latest result for every sensor. */
void xadc_drv_read_all(xadc_sample_t *sample_out);

/* Min/max the XADC has tracked in hardware since its last reset (12-bit codes). */
void xadc_drv_read_min_max(xadc_sensor_t sensor, uint16_t *min_code_out, uint16_t *max_code_out);

/*
 * Converts a raw code to milli-units of the sensor's natural unit:
 * milli-degC for the temperature sensor, millivolts for the supply rails.
 */
int32_t xadc_drv_code_to_milli(xadc_sensor_t sensor, uint16_t raw_code);

bool        xadc_drv_is_temperature(xadc_sensor_t sensor);
const char *xadc_drv_sensor_name(xadc_sensor_t sensor);
const char *xadc_drv_sensor_unit(xadc_sensor_t sensor);

#endif /* XADC_DRV_H */
