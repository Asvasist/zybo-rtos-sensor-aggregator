/*
 * slcr.c
 */
#include "slcr.h"

#include <stdbool.h>

#include "xil_io.h"
#include "xparameters_ps.h"

#define SLCR_LOCK_OFFSET            0x004U
#define SLCR_UNLOCK_OFFSET          0x008U
#define SLCR_LOCKSTA_OFFSET         0x00CU
#define SLCR_LOCK_KEY               0x767BU
#define SLCR_UNLOCK_KEY             0xDF0DU
#define SLCR_LOCKSTA_LOCKED_MASK    0x1U

uint32_t slcr_read(uint32_t offset)
{
    return Xil_In32(XPS_SYS_CTRL_BASEADDR + offset);
}

void slcr_modify(uint32_t offset, uint32_t clear_mask, uint32_t set_mask)
{
    const bool was_locked = (Xil_In32(XPS_SYS_CTRL_BASEADDR + SLCR_LOCKSTA_OFFSET) & SLCR_LOCKSTA_LOCKED_MASK) != 0U;
    uint32_t   value;

    if (was_locked)
    {
        Xil_Out32(XPS_SYS_CTRL_BASEADDR + SLCR_UNLOCK_OFFSET, SLCR_UNLOCK_KEY);
    }

    value = Xil_In32(XPS_SYS_CTRL_BASEADDR + offset);
    value = (value & ~clear_mask) | set_mask;
    Xil_Out32(XPS_SYS_CTRL_BASEADDR + offset, value);

    if (was_locked)
    {
        Xil_Out32(XPS_SYS_CTRL_BASEADDR + SLCR_LOCK_OFFSET, SLCR_LOCK_KEY);
    }
}
