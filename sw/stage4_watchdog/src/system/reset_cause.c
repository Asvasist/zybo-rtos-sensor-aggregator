/*
 * reset_cause.c
 */
#include "reset_cause.h"

#include <stdint.h>

#include "slcr.h"

/* REBOOT_STATUS reset reason bits (UG585 appendix B, slcr.REBOOT_STATUS). */
#define REBOOT_SWDT_RST     (1UL << 16)
#define REBOOT_AWDT0_RST    (1UL << 17)
#define REBOOT_AWDT1_RST    (1UL << 18)
#define REBOOT_SLC_RST      (1UL << 19)
#define REBOOT_DBG_RST      (1UL << 20)
#define REBOOT_SRST_B       (1UL << 21)
#define REBOOT_POR          (1UL << 22)
#define REBOOT_REASON_MASK  0x007F0000UL

static reset_cause_t s_cause = RESET_CAUSE_UNKNOWN;

reset_cause_t reset_cause_capture(void)
{
    const uint32_t status = slcr_read(SLCR_REBOOT_STATUS_OFFSET);

    /* Normally only one bit is set, since they're cleared every boot. The watchdogs win if not. */
    if ((status & REBOOT_SWDT_RST) != 0U)
    {
        s_cause = RESET_CAUSE_SWDT;
    }
    else if ((status & (REBOOT_AWDT0_RST | REBOOT_AWDT1_RST)) != 0U)
    {
        s_cause = RESET_CAUSE_CPU_WDT;
    }
    else if ((status & REBOOT_POR) != 0U)
    {
        s_cause = RESET_CAUSE_POWER_ON;
    }
    else if ((status & REBOOT_SRST_B) != 0U)
    {
        s_cause = RESET_CAUSE_PS_SRST;
    }
    else if ((status & REBOOT_DBG_RST) != 0U)
    {
        s_cause = RESET_CAUSE_DEBUG;
    }
    else if ((status & REBOOT_SLC_RST) != 0U)
    {
        s_cause = RESET_CAUSE_SOFTWARE;
    }
    else
    {
        s_cause = RESET_CAUSE_UNKNOWN;
    }

    /*
     * Only the reason bits. REBOOT_STATE (31:24) belongs to the FSBL and the
     * low 16 bits are the BootROM error code.
     * TODO: keep a watchdog reset count in the spare REBOOT_STATE bits.
     */
    slcr_modify(SLCR_REBOOT_STATUS_OFFSET, REBOOT_REASON_MASK, 0U);

    return s_cause;
}

reset_cause_t reset_cause_get(void)
{
    return s_cause;
}

const char *reset_cause_name(reset_cause_t cause)
{
    switch (cause)
    {
    case RESET_CAUSE_POWER_ON: return "power-on";
    case RESET_CAUSE_SWDT:     return "WATCHDOG (SWDT)";
    case RESET_CAUSE_CPU_WDT:  return "CPU private watchdog";
    case RESET_CAUSE_PS_SRST:  return "PS reset button";
    case RESET_CAUSE_DEBUG:    return "debugger";
    case RESET_CAUSE_SOFTWARE: return "software (SLCR)";
    default:                   return "unknown";
    }
}
