/*
 * gpio_drv.c
 *
 * PS MIO GPIO driver on top of the Xilinx XGpioPs low-level driver.
 */
#include "gpio_drv.h"

#include <stddef.h>

#include "xgpiops.h"
#include "xstatus.h"

#include "board_zybo.h"

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
    [GPIO_BTN4] = { BOARD_MIO_BTN4, true },
    [GPIO_BTN5] = { BOARD_MIO_BTN5, true },
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
        XGpioPs_SetDirectionPin(&s_gpio_inst, s_btn_hw[btn].mio_pin, 0U);

        /*
         * Seed the debouncer with the current level. A button that is being
         * held through reset should not show up as a fresh press at boot.
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
     * Plain read-then-clear. Safe only because the scan and the take both
     * run in the UI task. If the scan ever moves into an ISR or a different
     * task, this has to become an atomic exchange.
     */
    pressed = s_btn_state[btn].press_latched;
    s_btn_state[btn].press_latched = false;

    return pressed;
}
