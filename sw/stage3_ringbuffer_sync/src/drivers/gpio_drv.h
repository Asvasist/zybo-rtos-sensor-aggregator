/*
 * gpio_drv.h
 *
 * PS MIO GPIO: user LED LD4 and push buttons BTN4/BTN5.
 *
 * Buttons are debounced in software by a simple integrator: the raw level has
 * to disagree with the debounced level for GPIO_DRV_DEBOUNCE_SCANS consecutive
 * scans before the debounced level flips. With a 10 ms scan period that gives
 * 30 ms, which covers the bounce on the Zybo's tactile switches comfortably.
 *
 * A press is latched as an event on the released->pressed transition, so the
 * application can't miss a short press between two polls and never sees the
 * same press twice.
 *
 * Task usage under FreeRTOS:
 *   - gpio_drv_btn_scan() and gpio_drv_btn_take_press(): UI task only
 *   - gpio_drv_btn_is_down(): any task, it's a single bool written by the scan
 *   - LED functions: consumer task only (and the fault handler)
 * Pin writes go through the MIO mask-data register, so the LED and the button
 * inputs never do a read-modify-write on a shared register.
 */
#ifndef GPIO_DRV_H
#define GPIO_DRV_H

#include <stdbool.h>
#include <stdint.h>

#define GPIO_DRV_DEBOUNCE_SCANS     3U

typedef enum
{
    GPIO_DRV_OK = 0,
    GPIO_DRV_ERR_LOOKUP,
    GPIO_DRV_ERR_INIT
} gpio_drv_status_t;

typedef enum
{
    GPIO_BTN4 = 0,
    GPIO_BTN5,
    GPIO_BTN_COUNT
} gpio_btn_id_t;

gpio_drv_status_t gpio_drv_init(void);

void gpio_drv_led_set(bool on);
void gpio_drv_led_toggle(void);

/* Samples all buttons and runs the debouncer. Call at a fixed period (~10 ms). */
void gpio_drv_btn_scan(void);

/* Debounced level, true while the button is held. */
bool gpio_drv_btn_is_down(gpio_btn_id_t btn);

/* Returns true once per press and clears the latched event. */
bool gpio_drv_btn_take_press(gpio_btn_id_t btn);

#endif /* GPIO_DRV_H */
