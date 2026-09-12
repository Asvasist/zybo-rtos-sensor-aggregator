/*
 * sample_timer.c
 *
 * TTC interval timer on top of the Xilinx XTtcPs low-level driver.
 * Reference: UG585 chapter 8 (timers).
 */
#include "sample_timer.h"

#include <stdbool.h>
#include <stddef.h>

#include "xttcps.h"
#include "xstatus.h"

#include "FreeRTOS.h"

#include "board_zybo.h"

/* XTtcPs_CalcIntervalFromFreq() reports "can't do it" with this prescaler value. */
#define TTC_PRESCALER_INVALID   0xFFU

#define USEC_PER_SEC            1000000ULL

static XTtcPs            s_ttc_inst;
static sample_timer_cb_t s_tick_cb;
static void             *s_tick_cb_ctx;
static uint32_t          s_period_us;
static bool              s_timer_ready;

static void sample_timer_isr(void *isr_ctx)
{
    XTtcPs *const ttc = (XTtcPs *)isr_ctx;

    /*
     * The TTC interrupt status register is clear-on-read, so the read below
     * already acknowledges the interrupt. The explicit clear is kept because
     * older XTtcPs versions expect it and it costs one register access.
     */
    const u32 irq_status = XTtcPs_GetInterruptStatus(ttc);
    XTtcPs_ClearInterruptStatus(ttc, irq_status);

    if (((irq_status & XTTCPS_IXR_INTERVAL_MASK) != 0U) && (s_tick_cb != NULL))
    {
        s_tick_cb(s_tick_cb_ctx);
    }
}

sample_timer_status_t sample_timer_init(uint32_t rate_hz, sample_timer_cb_t tick_cb, void *cb_ctx)
{
    XTtcPs_Config *ttc_cfg;
    XInterval      interval;
    u8             prescaler;
    s32            xil_status;
    uint64_t       clock_divisor;

    s_timer_ready = false;

    if ((rate_hz == 0U) || (tick_cb == NULL))
    {
        return SAMPLE_TIMER_ERR_PARAM;
    }

    ttc_cfg = XTtcPs_LookupConfig(BOARD_SAMPLE_TTC_ID);
    if (ttc_cfg == NULL)
    {
        return SAMPLE_TIMER_ERR_LOOKUP;
    }

    xil_status = XTtcPs_CfgInitialize(&s_ttc_inst, ttc_cfg, ttc_cfg->BaseAddress);
    if (xil_status == XST_DEVICE_IS_STARTED)
    {
        /*
         * Still counting from a previous run - typical after restarting the
         * application from the debugger without a system reset. Stop it at
         * register level (XTtcPs_Stop() asserts on an instance that isn't
         * marked ready yet) and try again.
         */
        XTtcPs_WriteReg(ttc_cfg->BaseAddress, XTTCPS_CNT_CNTRL_OFFSET,
                        XTtcPs_ReadReg(ttc_cfg->BaseAddress, XTTCPS_CNT_CNTRL_OFFSET) | XTTCPS_CNT_CNTRL_DIS_MASK);
        xil_status = XTtcPs_CfgInitialize(&s_ttc_inst, ttc_cfg, ttc_cfg->BaseAddress);
    }
    if (xil_status != XST_SUCCESS)
    {
        return SAMPLE_TIMER_ERR_INIT;
    }

    /* Interval mode: count up to the match value, interrupt, restart from 0. No waveform output. */
    if (XTtcPs_SetOptions(&s_ttc_inst, XTTCPS_OPTION_INTERVAL_MODE | XTTCPS_OPTION_WAVE_DISABLE) != XST_SUCCESS)
    {
        return SAMPLE_TIMER_ERR_INIT;
    }

    /*
     * The counter is only 16 bits, so 10 Hz needs the prescaler. With the
     * ~111 MHz TTC clock this ends up at prescaler 2^8, interval 43401.
     */
    XTtcPs_CalcIntervalFromFreq(&s_ttc_inst, rate_hz, &interval, &prescaler);
    if (prescaler == TTC_PRESCALER_INVALID)
    {
        return SAMPLE_TIMER_ERR_RATE;
    }

    XTtcPs_SetInterval(&s_ttc_inst, interval);
    XTtcPs_SetPrescaler(&s_ttc_inst, prescaler);

    /* Prescaler value N divides by 2^(N+1); the "disabled" value means divide by 1. */
    clock_divisor = (prescaler == XTTCPS_CLK_CNTRL_PS_DISABLE) ? 1ULL : (2ULL << prescaler);
    s_period_us   = (uint32_t)((((uint64_t)interval + 1ULL) * clock_divisor * USEC_PER_SEC) / ttc_cfg->InputClockHz);

    /* Nothing from a previous run may fire the moment the GIC line is enabled. */
    XTtcPs_DisableInterrupts(&s_ttc_inst, XTTCPS_IXR_ALL_MASK);
    (void)XTtcPs_GetInterruptStatus(&s_ttc_inst);

    s_tick_cb     = tick_cb;
    s_tick_cb_ctx = cb_ctx;

    /*
     * Default GIC priority (0xA0) and level-sensitive trigger from the GIC
     * distributor init are what's wanted: the priority is low enough to be
     * allowed to call FreeRTOS FromISR functions, and the TTC line is level
     * high. The port asserts if the priority ever ends up wrong.
     */
    if (xPortInstallInterruptHandler((uint8_t)BOARD_SAMPLE_TTC_IRQ, sample_timer_isr, &s_ttc_inst) != pdPASS)
    {
        return SAMPLE_TIMER_ERR_IRQ;
    }

    XTtcPs_EnableInterrupts(&s_ttc_inst, XTTCPS_IXR_INTERVAL_MASK);
    vPortEnableInterrupt((uint8_t)BOARD_SAMPLE_TTC_IRQ);

    s_timer_ready = true;
    return SAMPLE_TIMER_OK;
}

void sample_timer_start(void)
{
    if (s_timer_ready)
    {
        XTtcPs_Start(&s_ttc_inst);
    }
}

void sample_timer_stop(void)
{
    if (s_timer_ready)
    {
        XTtcPs_Stop(&s_ttc_inst);
    }
}

uint32_t sample_timer_period_us(void)
{
    return s_period_us;
}
