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
 * ------------------------------------------------------------------------- */
#ifndef SDT
#define BOARD_CONSOLE_UART_ID       XPAR_XUARTPS_0_DEVICE_ID
#define BOARD_PS_GPIO_ID            XPAR_XGPIOPS_0_DEVICE_ID
#define BOARD_XADC_ID               XPAR_XADCPS_0_DEVICE_ID
#define BOARD_SAMPLE_TTC_ID         XPAR_XTTCPS_1_DEVICE_ID
#else
#define BOARD_CONSOLE_UART_ID       XPAR_XUARTPS_0_BASEADDR
#define BOARD_PS_GPIO_ID            XPAR_XGPIOPS_0_BASEADDR
#define BOARD_XADC_ID               XPAR_XADCPS_0_BASEADDR
#define BOARD_SAMPLE_TTC_ID         XPAR_XTTCPS_1_BASEADDR
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
 * The MIO pin mux itself is configured by ps7_init (board preset), the
 * firmware only sets direction and output enable.
 * ------------------------------------------------------------------------- */
#define BOARD_MIO_LED4              7U
#define BOARD_MIO_BTN4              50U
#define BOARD_MIO_BTN5              51U

/* -------------------------------------------------------------------------
 * Sample timer
 *
 * TTC0 counter 1 (ps7_ttc_1), GIC SPI ID 43, level sensitive.
 *
 * Counter 0 is left alone on purpose: depending on the BSP version and its
 * settings it can get claimed as the sleep timer or tick source, and two
 * owners of one counter is a miserable bug to chase.
 *
 * TTC0 has to be enabled in the PS configuration - hw/scripts does that.
 * ------------------------------------------------------------------------- */
#define BOARD_SAMPLE_TTC_IRQ        XPS_TTC0_1_INT_ID

/* -------------------------------------------------------------------------
 * Memory map
 *
 * DDR as the Cortex-A9s see it (UG585, system address map): at most
 * 0x0010_0000 to 0x3FFF_FFFF. The Zybo's 512 MB and the Zybo Z7's 1 GB both
 * sit inside that window; the bottom megabyte is left out because OCM can be
 * mapped there. Only used as a sanity check that the sensor log really did
 * get linked into DDR.
 * ------------------------------------------------------------------------- */
#define BOARD_DDR_BASE_ADDR         0x00100000UL
#define BOARD_DDR_HIGH_ADDR         0x3FFFFFFFUL

#endif /* BOARD_ZYBO_H */
