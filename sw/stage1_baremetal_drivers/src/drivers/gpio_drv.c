/*
 * gpio_drv.c
 *
 * PS MIO GPIO driver on top of the Xilinx XGpioPs low-level driver.
 */
#include "gpio_drv.h"

#include <stddef.h>

#include "xgpiops.h"
#include "xil_io.h"
#include "xstatus.h"

#include "board_zybo.h"

/*
 * SLCR registers for MIO pad configuration (UG585, appendix B). The SLCR is
 * write-protected after ps7_init; it is unlocked only for the pad update and
 * locked again straight after if it was locked before.
 */
#define SLCR_LOCK_OFFSET            0x004U
#define SLCR_UNLOCK_OFFSET          0x008U
#define SLCR_LOCKSTA_OFFSET         0x00CU
#define SLCR_MIO_PIN_00_OFFSET      0x700U      /* MIO_PIN_n at 0x700 + 4 * n */
#define SLCR_LOCK_KEY               0x767BU
#define SLCR_UNLOCK_KEY             0xDF0DU
#define SLCR_LOCKSTA_LOCKED_MASK    0x1U
#define SLCR_MIO_PIN_PULLUP_MASK    0x1000U     /* bit 12 */

typedef struct
{
    uint32_t mio_pin;
    bool     active_high;
} gpio_btn_hw_t;

typedef struct
{
    bool    is_down;            /* debounced level                               */
    uint8_t mismatch_scans;     /* consecutive scans where raw level != is_down  */
    bool    press_latched;      /* press edge not yet consumed by the app        */
} gpio_btn_state_t;

/*
 * Kept as a table so the PL buttons/switches (AXI GPIO, later stage) can be
 * folded into the same debouncer without touching the logic below.
 */
static const gpio_btn_hw_t s_btn_hw[GPIO_BTN_COUNT] =
{
    [GPIO_BTN4] = { BOARD_MIO_BTN4, (BOARD_MIO_BTN_ACTIVE_HIGH != 0) },
    [GPIO_BTN5] = { BOARD_MIO_BTN5, (BOARD_MIO_BTN_ACTIVE_HIGH != 0) },
};

static XGpioPs          s_gpio_inst;
static bool             s_gpio_ready;
static bool             s_led_on;
static gpio_btn_state_t s_btn_state[GPIO_BTN_COUNT];

static bool gpio_btn_read_raw(gpio_btn_id_t btn)
{
    const uint32_t pin_level = XGpioPs_ReadPin(&s_gpio_inst, s_btn_hw[btn].mio_pin);

    return s_btn_hw[btn].active_high ? (pin_level != 0U) : (pin_level == 0U);
}

/* Sets or clears the internal pull-up of one MIO pad. The SLCR unlock is global - call from init only. */
static void gpio_mio_set_pullup(uint32_t mio_pin, bool enable)
{
    const UINTPTR pin_reg    = XPS_SYS_CTRL_BASEADDR + SLCR_MIO_PIN_00_OFFSET + (4U * mio_pin);
    const bool    was_locked = (Xil_In32(XPS_SYS_CTRL_BASEADDR + SLCR_LOCKSTA_OFFSET) & SLCR_LOCKSTA_LOCKED_MASK) != 0U;
    uint32_t      pin_cfg;

    if (was_locked)
    {
        Xil_Out32(XPS_SYS_CTRL_BASEADDR + SLCR_UNLOCK_OFFSET, SLCR_UNLOCK_KEY);
    }

    pin_cfg = Xil_In32(pin_reg);
    pin_cfg = enable ? (pin_cfg | SLCR_MIO_PIN_PULLUP_MASK) : (pin_cfg & ~SLCR_MIO_PIN_PULLUP_MASK);
    Xil_Out32(pin_reg, pin_cfg);

    if (was_locked)
    {
        Xil_Out32(XPS_SYS_CTRL_BASEADDR + SLCR_LOCK_OFFSET, SLCR_LOCK_KEY);
    }
}

gpio_drv_status_t gpio_drv_init(void)
{
    XGpioPs_Config *gpio_cfg;
    uint32_t        btn;

    s_gpio_ready = false;

    gpio_cfg = XGpioPs_LookupConfig(BOARD_PS_GPIO_ID);
    if (gpio_cfg == NULL)
    {
        return GPIO_DRV_ERR_LOOKUP;
    }

    if (XGpioPs_CfgInitialize(&s_gpio_inst, gpio_cfg, gpio_cfg->BaseAddr) != XST_SUCCESS)
    {
        return GPIO_DRV_ERR_INIT;
    }

    /* LD4: output, driven low before the output enable goes on so it doesn't flash. */
    XGpioPs_WritePin(&s_gpio_inst, BOARD_MIO_LED4, 0U);
    XGpioPs_SetDirectionPin(&s_gpio_inst, BOARD_MIO_LED4, 1U);
    XGpioPs_SetOutputEnablePin(&s_gpio_inst, BOARD_MIO_LED4, 1U);
    s_led_on = false;

    s_gpio_ready = true;

    for (btn = 0U; btn < (uint32_t)GPIO_BTN_COUNT; btn++)
    {
        /* Pad pull-up per board wiring, not per whatever preset built the XSA - see board_zybo.h. */
        gpio_mio_set_pullup(s_btn_hw[btn].mio_pin, (BOARD_MIO_BTN_PULLUP != 0));
        XGpioPs_SetDirectionPin(&s_gpio_inst, s_btn_hw[btn].mio_pin, 0U);

        /*
         * Seed the debouncer with the current level, so a button held through
         * reset doesn't show up as a fresh press at boot. If the pad hasn't
         * settled yet after the pull-up change, the debouncer corrects the
         * seed within three scans without reporting a press.
         */
        s_btn_state[btn].is_down        = gpio_btn_read_raw((gpio_btn_id_t)btn);
        s_btn_state[btn].mismatch_scans = 0U;
        s_btn_state[btn].press_latched  = false;
    }

    return GPIO_DRV_OK;
}

void gpio_drv_led_set(bool on)
{
    if (!s_gpio_ready)
    {
        return;
    }

    XGpioPs_WritePin(&s_gpio_inst, BOARD_MIO_LED4, on ? 1U : 0U);
    s_led_on = on;
}

void gpio_drv_led_toggle(void)
{
    gpio_drv_led_set(!s_led_on);
}

void gpio_drv_btn_scan(void)
{
    uint32_t btn;

    if (!s_gpio_ready)
    {
        return;
    }

    for (btn = 0U; btn < (uint32_t)GPIO_BTN_COUNT; btn++)
    {
        gpio_btn_state_t *state   = &s_btn_state[btn];
        const bool        raw_down = gpio_btn_read_raw((gpio_btn_id_t)btn);

        if (raw_down == state->is_down)
        {
            /* Any bounce back to the stable level restarts the count. */
            state->mismatch_scans = 0U;
            continue;
        }

        state->mismatch_scans++;
        if (state->mismatch_scans >= GPIO_DRV_DEBOUNCE_SCANS)
        {
            state->is_down        = raw_down;
            state->mismatch_scans = 0U;

            if (raw_down)
            {
                state->press_latched = true;
            }
        }
    }
}

bool gpio_drv_btn_is_down(gpio_btn_id_t btn)
{
    if (btn >= GPIO_BTN_COUNT)
    {
        return false;
    }

    return s_btn_state[btn].is_down;
}

bool gpio_drv_btn_take_press(gpio_btn_id_t btn)
{
    bool pressed;

    if (btn >= GPIO_BTN_COUNT)
    {
        return false;
    }

    /*
     * Read-then-clear is fine while scan and take run in the same context
     * (super-loop). Once the scan moves into a timer ISR this needs to become
     * an atomic exchange or run with the timer interrupt masked.
     */
    pressed = s_btn_state[btn].press_latched;
    s_btn_state[btn].press_latched = false;

    return pressed;
}
