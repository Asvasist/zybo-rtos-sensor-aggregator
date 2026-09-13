/*
 * slcr.h
 *
 * Minimal access to the System Level Control Registers (UG585 appendix B).
 *
 * ps7_init leaves the SLCR write-locked. slcr_modify() unlocks around the
 * write and puts back whatever lock state it found. Not thread-safe - only
 * called from init code before the scheduler starts.
 */
#ifndef SLCR_H
#define SLCR_H

#include <stdint.h>

#define SLCR_MIO_PIN_OFFSET(pin)        (0x700U + (4U * (pin)))
#define SLCR_MIO_PIN_PULLUP_MASK        0x00001000U     /* bit 12 */

#define SLCR_REBOOT_STATUS_OFFSET       0x258U

uint32_t slcr_read(uint32_t offset);

/* Read-modify-write: clears clear_mask, then sets set_mask. */
void slcr_modify(uint32_t offset, uint32_t clear_mask, uint32_t set_mask);

#endif /* SLCR_H */
