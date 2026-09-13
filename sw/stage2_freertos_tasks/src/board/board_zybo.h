/*
 * board_zybo.h
 *
 * Board resource map for the Digilent Zybo / Zybo Z7-10 / Zybo Z7-20.
 *
 * Only resources that the PS can reach without any PL logic are listed here.
 * The slide switches SW0..SW3 and push buttons BTN0..BTN3 sit on PL pins and
 * get added once the AXI GPIO block goes into the hardware design (last stage).
 *
 * Drivers take their instance IDs and pin numbers from this file rather than
 * from xparameters.h directly, so a board change or a move between the classic
 * and SDT BSP flows only touches this one header.
 */
#ifndef BOARD_ZYBO_H
#define BOARD_ZYBO_H

#include "xparameters.h"
#include "xparameters_ps.h"

/* -------------------------------------------------------------------------
 * Driver instance selection
 *
 * Classic flow (Vitis <= 2023.1): LookupConfig() takes a device ID.
 * SDT flow     (Vitis >= 2023.2): LookupConfig() takes the base address.
 *
 * The Zybo board preset only enables UART1, so it is instance 0 in both flows.
 * If a second PS UART is ever enabled, re-check this against xparameters.h.
 *
 * TTC instances are numbered per counter: TTC0 counters 0/1/2 are instances
 * 0/1/2, TTC1 (if enabled) follows as 3/4/5.
 *
 * There is only one XADC. In the SDT flow its lookup is done with base
 * address 0, which XAdcPs_LookupConfig() treats as "first instance" - the
 * same thing the Xilinx SDT examples do.
 * ------------------------------------------------------------------------- */
#ifndef SDT
#define BOARD_CONSOLE_UART_ID       XPAR_XUARTPS_0_DEVICE_ID
#define BOARD_PS_GPIO_ID            XPAR_XGPIOPS_0_DEVICE_ID
#define BOARD_XADC_ID               XPAR_XADCPS_0_DEVICE_ID
#define BOARD_SAMPLE_TTC_ID         XPAR_XTTCPS_2_DEVICE_ID
#else
#define BOARD_CONSOLE_UART_ID       XPAR_XUARTPS_0_BASEADDR
#define BOARD_PS_GPIO_ID            XPAR_XGPIOPS_0_BASEADDR
#define BOARD_XADC_ID               0U
#define BOARD_SAMPLE_TTC_ID         XPAR_XTTCPS_2_BASEADDR
#endif

/* -------------------------------------------------------------------------
 * Console
 *
 * PS UART1 on MIO48 (TX) / MIO49 (RX), bridged to USB by the FTDI part on the
 * shared PROG/UART micro-USB connector.
 * ------------------------------------------------------------------------- */
#define BOARD_CONSOLE_BAUD          115200U

/* -------------------------------------------------------------------------
 * PS MIO GPIO
 *
 * LD4  - MIO7,  active high. MIO7 is one of the two output-only MIO pins.
 * BTN4 - MIO50, active high, pulled down on the board.
 * BTN5 - MIO51, active high, pulled down on the board.
 *
 * The pin mux comes from ps7_init (board preset); the firmware sets
 * direction and output enable, and the pad pull-up on the button pins.
 *
 * Button pull-ups: the original Zybo preset disables the Zynq's internal
 * pull-up on MIO50/51 so the board's pull-down resistors set the idle level.
 * The Zybo Z7 preset leaves them at Vivado's default (enabled), and the
 * buttons then read as permanently pressed. gpio_drv_init() therefore
 * applies BOARD_MIO_BTN_PULLUP itself, whatever the XSA says.
 *
 * To be confirmed on the Z7 during stage 4 bring-up (press = 1, release = 0).
 * If a Z7 revision turns out to wire them active low instead, set
 * BOARD_MIO_BTN_ACTIVE_HIGH to 0 and BOARD_MIO_BTN_PULLUP to 1.
 * ------------------------------------------------------------------------- */
#define BOARD_MIO_LED4              7U
#define BOARD_MIO_BTN4              50U
#define BOARD_MIO_BTN5              51U
#define BOARD_MIO_BTN_ACTIVE_HIGH   1
#define BOARD_MIO_BTN_PULLUP        0

/* -------------------------------------------------------------------------
 * Sample timer
 *
 * TTC0 counter 2 (ps7_ttc_2), GIC ID 44, level sensitive.
 *
 * Originally counter 1. That collides with the SDT flow (Vitis 2023.2 and
 * later): for a FreeRTOS BSP, xiltimer always enables a tick timer, and with
 * TTC0 in the design its default pick can be the middle TTC instance,
 * ps7_ttc_1. Counter 0 can end up as the sleep timer in some configurations.
 * Counter 2 is claimed by neither default. sample_timer_init() refuses a
 * counter that is already running, so a clash still fails loudly rather
 * than killing the RTOS tick.
 *
 * Only the classic flow uses the plain GIC ID below. The SDT flow takes the
 * encoded interrupt ID from the TTC config table instead - see sample_timer.c.
 *
 * TTC0 has to be enabled in the PS configuration - hw/scripts does that.
 * ------------------------------------------------------------------------- */
#define BOARD_SAMPLE_TTC_IRQ        XPS_TTC0_2_INT_ID

#endif /* BOARD_ZYBO_H */
