/*
 * wdt_drv.h
 *
 * Zynq system watchdog (SWDT, the one in the PS - not the Cortex-A9 private
 * watchdog). On expiry it pulls the internal system reset, so the whole PS
 * starts again from the BootROM.
 *
 * Only the watchdog supervisor task kicks it. Note the SWDT keeps counting
 * while a debugger has the CPUs halted - build with APP_WDT_ENABLE 0 when
 * stepping through code.
 */
#ifndef WDT_DRV_H
#define WDT_DRV_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    WDT_DRV_OK = 0,
    WDT_DRV_ERR_LOOKUP,     /* SWDT not in the XSA? */
    WDT_DRV_ERR_INIT,
    WDT_DRV_ERR_RANGE       /* timeout out of reach of the 24-bit counter */
} wdt_drv_status_t;

/* Sets the timeout (rounded up), reset output on, IRQ output off. Does not start the counter. */
wdt_drv_status_t wdt_drv_init(uint32_t timeout_ms);

void wdt_drv_start(void);
void wdt_drv_kick(void);

bool     wdt_drv_is_running(void);
uint32_t wdt_drv_timeout_ms(void);

#endif /* WDT_DRV_H */
