/*
 * reset_cause.h
 *
 * Why the PS came out of reset, from the reset reason bits in the SLCR
 * REBOOT_STATUS register (bits 22:16).
 *
 * Those bits are sticky through everything except a power-on reset, so they
 * are read once at boot and then cleared. Otherwise an old watchdog reset
 * would still show up after the next JTAG download.
 */
#ifndef RESET_CAUSE_H
#define RESET_CAUSE_H

typedef enum
{
    RESET_CAUSE_UNKNOWN = 0,    /* no flag set - e.g. restarted by the debugger without a reset */
    RESET_CAUSE_POWER_ON,
    RESET_CAUSE_SWDT,           /* system watchdog - the one this firmware uses */
    RESET_CAUSE_CPU_WDT,        /* Cortex-A9 private watchdog */
    RESET_CAUSE_PS_SRST,        /* PS_SRST_B pin (reset button) */
    RESET_CAUSE_DEBUG,          /* JTAG debugger */
    RESET_CAUSE_SOFTWARE        /* SLCR software reset */
} reset_cause_t;

/* Call once, early in main(). Returns the cause and clears the flags. */
reset_cause_t reset_cause_capture(void);

/* What reset_cause_capture() found. */
reset_cause_t reset_cause_get(void);

const char *reset_cause_name(reset_cause_t cause);

#endif /* RESET_CAUSE_H */
