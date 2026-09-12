/*
 * uart_drv.h
 *
 * Polled driver for the PS UART used as the system console.
 *
 * TX blocks while the FIFO is full, RX is a non-blocking poll, the UART
 * interrupt stays off.
 *
 * The driver holds no locks. Under FreeRTOS it relies on one owner per
 * direction instead:
 *   - TX: the consumer task only (plus the fault handler, which masks
 *         interrupts before it writes anything)
 *   - RX: the UI task only, which is also where the line error flags get
 *         collected
 * The two directions use different registers, so the owners don't need to
 * coordinate with each other.
 */
#ifndef UART_DRV_H
#define UART_DRV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    UART_DRV_OK = 0,
    UART_DRV_ERR_LOOKUP,        /* no config entry for the instance in xparameters.h */
    UART_DRV_ERR_INIT,          /* XUartPs_CfgInitialize() rejected the config        */
    UART_DRV_ERR_FORMAT         /* baud rate not reachable from the UART ref clock    */
} uart_drv_status_t;

/*
 * Receive line errors counted since init. Handy when chasing a bad cable or a
 * terminal left at the wrong baud rate - framing errors climb immediately.
 */
typedef struct
{
    uint32_t rx_overrun;
    uint32_t rx_framing;
    uint32_t rx_parity;
} uart_drv_err_counters_t;

/* Brings the console UART up at 8N1 and the requested baud rate, interrupts off. */
uart_drv_status_t uart_drv_init(uint32_t baud_rate);

/* Blocking single character transmit (waits for TX FIFO space). */
void uart_drv_put_char(char ch);

/* Blocking raw write, no newline translation. */
void uart_drv_write(const char *data, size_t length);

/* Non-blocking receive. Returns true and fills *ch_out if a byte was waiting. */
bool uart_drv_try_get_char(char *ch_out);

/*
 * Spins until the TX FIFO is empty AND the transmitter has shifted out the
 * last bit. Needed before anything that resets the chip (watchdog stage),
 * otherwise the final log line gets cut off.
 */
void uart_drv_wait_tx_idle(void);

/*
 * Snapshot of the counters as of the last RX poll. Doesn't touch the
 * hardware, so any task can call it without stepping on the RX owner.
 */
void uart_drv_get_err_counters(uart_drv_err_counters_t *counters_out);

#endif /* UART_DRV_H */
