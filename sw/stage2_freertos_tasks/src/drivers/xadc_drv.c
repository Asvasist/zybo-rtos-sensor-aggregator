/*
 * xadc_drv.c
 *
 * XADC driver on top of the Xilinx XAdcPs low-level driver. XAdcPs owns the
 * XADCIF command/read FIFO protocol; this layer owns the sequencer setup,
 * channel map and unit conversion.
 *
 * Reference: UG480 (7 Series XADC), chapter 2 for transfer functions and
 * chapter 4 for the sequencer. UG585 chapter 30 for the PS-XADC interface.
 */
#include "xadc_drv.h"

#include <stddef.h>

#include "xadcps.h"
#include "xstatus.h"

#include "board_zybo.h"

/*
 * Result and min/max registers hold the 12-bit conversion MSB-justified in a
 * 16-bit field; the bottom 4 bits are extra resolution from averaging that the
 * datasheet accuracy figures don't account for, so they're dropped.
 */
#define XADC_RESULT_SHIFT           4U
#define XADC_CODE_FULL_SCALE        4096UL

/*
 * UG480 transfer functions, scaled so everything stays in 32-bit integers:
 *   temperature  T[degC] = code * 503.975 / 4096 - 273.15
 *   supply rail  V[V]    = code * 3.0     / 4096
 * Worst case product is 4095 * 503975 = 2.06e9, inside uint32_t.
 */
#define XADC_TEMP_GAIN_MILLI        503975UL
#define XADC_TEMP_OFFSET_MILLI      273150L
#define XADC_SUPPLY_FS_MV           3000UL

/* Channels the sequencer converts. CALIB keeps the offset/gain coefficients fresh. */
#define XADC_SEQ_CHANNELS           (XADCPS_SEQ_CH_CALIB   | XADCPS_SEQ_CH_TEMP    | \
                                     XADCPS_SEQ_CH_VCCINT  | XADCPS_SEQ_CH_VCCAUX  | \
                                     XADCPS_SEQ_CH_VBRAM   | XADCPS_SEQ_CH_VCCPINT | \
                                     XADCPS_SEQ_CH_VCCPAUX | XADCPS_SEQ_CH_VCCPDRO)

/* Averaging on every measurement channel (averaging doesn't apply to CALIB). */
#define XADC_SEQ_AVG_CHANNELS       (XADC_SEQ_CHANNELS & ~((u32)XADCPS_SEQ_CH_CALIB))

typedef struct
{
    const char *name;
    uint8_t     adc_channel;    /* status register index, XADCPS_CH_*      */
    uint8_t     min_index;      /* XAdcPs_GetMinMaxMeasurement() index      */
    uint8_t     max_index;
    bool        is_temperature;
} xadc_sensor_desc_t;

static const xadc_sensor_desc_t s_sensor_desc[XADC_SENSOR_COUNT] =
{
    [XADC_SENSOR_DIE_TEMP] = { "Die temp", XADCPS_CH_TEMP,    XADCPS_MIN_TEMP,    XADCPS_MAX_TEMP,    true  },
    [XADC_SENSOR_VCCINT]   = { "VCCINT",   XADCPS_CH_VCCINT,  XADCPS_MIN_VCCINT,  XADCPS_MAX_VCCINT,  false },
    [XADC_SENSOR_VCCAUX]   = { "VCCAUX",   XADCPS_CH_VCCAUX,  XADCPS_MIN_VCCAUX,  XADCPS_MAX_VCCAUX,  false },
    [XADC_SENSOR_VCCBRAM]  = { "VCCBRAM",  XADCPS_CH_VBRAM,   XADCPS_MIN_VBRAM,   XADCPS_MAX_VBRAM,   false },
    [XADC_SENSOR_VCCPINT]  = { "VCCPINT",  XADCPS_CH_VCCPINT, XADCPS_MIN_VCCPINT, XADCPS_MAX_VCCPINT, false },
    [XADC_SENSOR_VCCPAUX]  = { "VCCPAUX",  XADCPS_CH_VCCPAUX, XADCPS_MIN_VCCPAUX, XADCPS_MAX_VCCPAUX, false },
    [XADC_SENSOR_VCCO_DDR] = { "VCCO_DDR", XADCPS_CH_VCCPDRO, XADCPS_MIN_VCCPDRO, XADCPS_MAX_VCCPDRO, false },
};

static XAdcPs s_xadc_inst;
static bool   s_xadc_ready;

xadc_drv_status_t xadc_drv_init(void)
{
    XAdcPs_Config *xadc_cfg;

    s_xadc_ready = false;

    xadc_cfg = XAdcPs_LookupConfig(BOARD_XADC_ID);
    if (xadc_cfg == NULL)
    {
        return XADC_DRV_ERR_LOOKUP;
    }

    /* Also enables the XADCIF and sets up its FIFO thresholds. */
    if (XAdcPs_CfgInitialize(&s_xadc_inst, xadc_cfg, xadc_cfg->BaseAddress) != XST_SUCCESS)
    {
        return XADC_DRV_ERR_INIT;
    }

    /*
     * Register write/read-back plus an XADC reset. If this fails the XADCIF
     * link itself is broken, and nothing read afterwards could be trusted.
     */
    if (XAdcPs_SelfTest(&s_xadc_inst) != XST_SUCCESS)
    {
        return XADC_DRV_ERR_SELFTEST;
    }

    /*
     * The sequencer has to be parked in safe mode while the channel and
     * averaging selections change; the driver refuses them otherwise.
     */
    XAdcPs_SetSequencerMode(&s_xadc_inst, XADCPS_SEQ_MODE_SAFE);

    /* Apply the factory offset/gain calibration to ADC and supply readings. */
    XAdcPs_SetCalibEnables(&s_xadc_inst,
                           XADCPS_CFR1_CAL_PS_GAIN_OFFSET_MASK | XADCPS_CFR1_CAL_ADC_GAIN_OFFSET_MASK);

    XAdcPs_SetAvg(&s_xadc_inst, XADCPS_AVG_16_SAMPLES);

    if (XAdcPs_SetSeqAvgEnables(&s_xadc_inst, XADC_SEQ_AVG_CHANNELS) != XST_SUCCESS)
    {
        return XADC_DRV_ERR_SEQ_CONFIG;
    }

    if (XAdcPs_SetSeqChEnables(&s_xadc_inst, XADC_SEQ_CHANNELS) != XST_SUCCESS)
    {
        return XADC_DRV_ERR_SEQ_CONFIG;
    }

    /*
     * Alarm thresholds are left at their power-on defaults on purpose. The
     * over-temperature alarm is what drives the automatic thermal shutdown
     * and there's no reason to touch it at this stage.
     */

    XAdcPs_SetSequencerMode(&s_xadc_inst, XADCPS_SEQ_MODE_CONTINPASS);

    /*
     * Stage 1 slept here until the first averaged results landed. Not needed
     * any more: the first read comes one sample period (100 ms) after the
     * scheduler starts, long after the sequencer's first pass. Dropping it
     * also removes the dependency on usleep(), whose behaviour differs
     * between the standalone and FreeRTOS BSPs.
     */

    s_xadc_ready = true;
    return XADC_DRV_OK;
}

uint16_t xadc_drv_read_raw(xadc_sensor_t sensor)
{
    if ((!s_xadc_ready) || (sensor >= XADC_SENSOR_COUNT))
    {
        return 0U;
    }

    return (uint16_t)(XAdcPs_GetAdcData(&s_xadc_inst, s_sensor_desc[sensor].adc_channel) >> XADC_RESULT_SHIFT);
}

void xadc_drv_read_all(xadc_sample_t *sample_out)
{
    uint32_t sensor;

    if (sample_out == NULL)
    {
        return;
    }

    /*
     * Channels are read one after another, so a snapshot can straddle a
     * sequencer pass. At 16x averaging the values move far too slowly for
     * that to matter here.
     */
    for (sensor = 0U; sensor < (uint32_t)XADC_SENSOR_COUNT; sensor++)
    {
        sample_out->raw_code[sensor] = xadc_drv_read_raw((xadc_sensor_t)sensor);
    }
}

void xadc_drv_read_min_max(xadc_sensor_t sensor, uint16_t *min_code_out, uint16_t *max_code_out)
{
    if ((min_code_out == NULL) || (max_code_out == NULL))
    {
        return;
    }

    if ((!s_xadc_ready) || (sensor >= XADC_SENSOR_COUNT))
    {
        *min_code_out = 0U;
        *max_code_out = 0U;
        return;
    }

    *min_code_out = (uint16_t)(XAdcPs_GetMinMaxMeasurement(&s_xadc_inst, s_sensor_desc[sensor].min_index)
                               >> XADC_RESULT_SHIFT);
    *max_code_out = (uint16_t)(XAdcPs_GetMinMaxMeasurement(&s_xadc_inst, s_sensor_desc[sensor].max_index)
                               >> XADC_RESULT_SHIFT);
}

int32_t xadc_drv_code_to_milli(xadc_sensor_t sensor, uint16_t raw_code)
{
    uint32_t scaled;

    if (sensor >= XADC_SENSOR_COUNT)
    {
        return 0;
    }

    if (s_sensor_desc[sensor].is_temperature)
    {
        /* + half an LSB of the divisor for round-to-nearest */
        scaled = (((uint32_t)raw_code * XADC_TEMP_GAIN_MILLI) + (XADC_CODE_FULL_SCALE / 2UL)) / XADC_CODE_FULL_SCALE;
        return (int32_t)scaled - XADC_TEMP_OFFSET_MILLI;
    }

    scaled = (((uint32_t)raw_code * XADC_SUPPLY_FS_MV) + (XADC_CODE_FULL_SCALE / 2UL)) / XADC_CODE_FULL_SCALE;
    return (int32_t)scaled;
}

bool xadc_drv_is_temperature(xadc_sensor_t sensor)
{
    return (sensor < XADC_SENSOR_COUNT) && s_sensor_desc[sensor].is_temperature;
}

const char *xadc_drv_sensor_name(xadc_sensor_t sensor)
{
    return (sensor < XADC_SENSOR_COUNT) ? s_sensor_desc[sensor].name : "?";
}

const char *xadc_drv_sensor_unit(xadc_sensor_t sensor)
{
    return xadc_drv_is_temperature(sensor) ? "C" : "V";
}
