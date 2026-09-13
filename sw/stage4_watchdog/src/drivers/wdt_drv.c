/*
 * wdt_drv.c
 *
 * SWDT on top of the Xilinx XWdtPs driver. Reference: UG585 chapter 8.
 */
#include "wdt_drv.h"

#include <stddef.h>

#include "xwdtps.h"
#include "xstatus.h"

#include "board_zybo.h"

/*
 * The counter is 24 bits and reloads with (CRV << 12) | 0xFFF, where CRV is
 * the 12-bit value in the CCR. So a timeout is (CRV + 1) * 4096 prescaled
 * clock periods.
 */
#define WDT_CRV_MAX         0xFFFU
#define WDT_CRV_SHIFT       12U

typedef struct
{
    u32      ccr_value;
    uint32_t divider;
} wdt_prescaler_t;

static const wdt_prescaler_t s_prescalers[] =
{
    { XWDTPS_CCR_PSCALE_0008,    8U },
    { XWDTPS_CCR_PSCALE_0064,   64U },
    { XWDTPS_CCR_PSCALE_0512,  512U },
    { XWDTPS_CCR_PSCALE_4096, 4096U },
};

static XWdtPs   s_wdt_inst;
static bool     s_wdt_ready;
static bool     s_wdt_running;
static uint32_t s_timeout_ms;

wdt_drv_status_t wdt_drv_init(uint32_t timeout_ms)
{
    XWdtPs_Config *wdt_cfg;
    uint64_t       clock_hz;
    uint32_t       idx;

    s_wdt_ready = false;

    wdt_cfg = XWdtPs_LookupConfig(BOARD_WDT_ID);
    if (wdt_cfg == NULL)
    {
        return WDT_DRV_ERR_LOOKUP;
    }

    if (XWdtPs_CfgInitialize(&s_wdt_inst, wdt_cfg, wdt_cfg->BaseAddress) != XST_SUCCESS)
    {
        return WDT_DRV_ERR_INIT;
    }

    /* CfgInitialize() doesn't copy the clock into the instance, so take it from the table entry. */
#ifdef SDT
    clock_hz = wdt_cfg->InputClockHz;
#else
    clock_hz = BOARD_WDT_CLK_HZ;
#endif

    /* The FSBL runs the SWDT during boot and should have stopped it at handoff. Make sure. */
    XWdtPs_Stop(&s_wdt_inst);

    /* Smallest prescaler that still fits: finest timeout resolution. */
    for (idx = 0U; idx < (sizeof(s_prescalers) / sizeof(s_prescalers[0])); idx++)
    {
        const uint64_t counts = ((uint64_t)timeout_ms * clock_hz) / (1000ULL * s_prescalers[idx].divider);
        const uint64_t crv    = counts >> WDT_CRV_SHIFT;

        if (crv > WDT_CRV_MAX)
        {
            continue;
        }

        XWdtPs_SetControlValue(&s_wdt_inst, XWDTPS_CLK_PRESCALE, s_prescalers[idx].ccr_value);
        XWdtPs_SetControlValue(&s_wdt_inst, XWDTPS_COUNTER_RESET, (u32)crv);
        XWdtPs_EnableOutput(&s_wdt_inst, XWDTPS_RESET_SIGNAL);
        XWdtPs_DisableOutput(&s_wdt_inst, XWDTPS_IRQ_SIGNAL);

        /* crv is rounded down, the +1 block rounds the timeout back up. */
        s_timeout_ms = (uint32_t)((((crv + 1ULL) << WDT_CRV_SHIFT) * s_prescalers[idx].divider * 1000ULL) / clock_hz);

        s_wdt_ready = true;
        return WDT_DRV_OK;
    }

    return WDT_DRV_ERR_RANGE;
}

void wdt_drv_start(void)
{
    if (!s_wdt_ready)
    {
        return;
    }

    XWdtPs_Start(&s_wdt_inst);

    /* A new CRV only takes effect on a restart, so load it straight away. */
    XWdtPs_RestartWdt(&s_wdt_inst);
    s_wdt_running = true;
}

void wdt_drv_kick(void)
{
    if (s_wdt_running)
    {
        XWdtPs_RestartWdt(&s_wdt_inst);
    }
}

bool wdt_drv_is_running(void)
{
    return s_wdt_running;
}

uint32_t wdt_drv_timeout_ms(void)
{
    return s_wdt_running ? s_timeout_ms : 0U;
}
