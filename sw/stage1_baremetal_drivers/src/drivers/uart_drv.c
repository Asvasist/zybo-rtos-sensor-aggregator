/*
 * uart_drv.c
 *
 * Console UART driver on top of the Xilinx XUartPs low-level driver.
 * See uart_drv.h for the usage model.
 */
#include "uart_drv.h"

#include "xuartps.h"
#include "xstatus.h"

#include "board_zybo.h"

/*
 * The error bits in the channel interrupt status register latch regardless of
 * the interrupt mask, which lets the polled driver still account for them.
 * They are write-one-to-clear.
 */
#define UART_RX_LINE_ERR_MASK   (XUARTPS_IXR_OVER | XUARTPS_IXR_FRAMING | XUARTPS_IXR_PARITY)

static XUartPs                  s_uart_inst;
static UINTPTR                  s_uart_base;
static bool                     s_uart_ready;
static uart_drv_err_counters_t  s_err_counters;

static void uart_collect_line_errors(void)
{
    const uint32_t pending = XUartPs_ReadReg(s_uart_base, XUARTPS_ISR_OFFSET) & UART_RX_LINE_ERR_MASK;

    if (pending == 0U)
    {
        return;
    }

    if ((pending & XUARTPS_IXR_OVER) != 0U)
    {
        s_err_counters.rx_overrun++;
    }
    if ((pending & XUARTPS_IXR_FRAMING) != 0U)
    {
        s_err_counters.rx_framing++;
    }
    if ((pending & XUARTPS_IXR_PARITY) != 0U)
    {
        s_err_counters.rx_parity++;
    }

    XUartPs_WriteReg(s_uart_base, XUARTPS_ISR_OFFSET, pending);
}

uart_drv_status_t uart_drv_init(uint32_t baud_rate)
{
    XUartPs_Config *uart_cfg;
    XUartPs_Format  line_format;

    s_uart_ready = false;

    uart_cfg = XUartPs_LookupConfig(BOARD_CONSOLE_UART_ID);
    if (uart_cfg == NULL)
    {
        return UART_DRV_ERR_LOOKUP;
    }

    /*
     * This UART is already running at this point - ps7_init/FSBL enabled it
     * because it is also the BSP stdout. Let whatever is still in the TX FIFO
     * drain before the baud generator gets reprogrammed, otherwise the tail of
     * the boot messages turns into garbage on the terminal. TXEMPTY resets to 1,
     * so this can't hang on a UART that was never enabled.
     */
    while ((XUartPs_ReadReg(uart_cfg->BaseAddress, XUARTPS_SR_OFFSET) & XUARTPS_SR_TXEMPTY) == 0U)
    {
        /* wait */
    }

    if (XUartPs_CfgInitialize(&s_uart_inst, uart_cfg, uart_cfg->BaseAddress) != XST_SUCCESS)
    {
        return UART_DRV_ERR_INIT;
    }
    s_uart_base = uart_cfg->BaseAddress;

    line_format.BaudRate = baud_rate;
    line_format.DataBits = XUARTPS_FORMAT_8_BITS;
    line_format.Parity   = XUARTPS_FORMAT_NO_PARITY;
    line_format.StopBits = XUARTPS_FORMAT_1_STOP_BIT;

    /*
     * SetDataFormat() runs the CD/BDIV divisor search internally and refuses
     * the rate if the best match is more than 3 % off. With the default 100 MHz
     * UART ref clock, 115200 lands at CD=124, BDIV=6 -> 115207 baud (+0.006 %).
     */
    if (XUartPs_SetDataFormat(&s_uart_inst, &line_format) != XST_SUCCESS)
    {
        return UART_DRV_ERR_FORMAT;
    }

    XUartPs_SetOperMode(&s_uart_inst, XUARTPS_OPER_MODE_NORMAL);

    /* Polled operation for now: every interrupt source stays masked. */
    XUartPs_SetInterruptMask(&s_uart_inst, 0U);

    /* Start the error accounting from a clean slate. */
    XUartPs_WriteReg(s_uart_base, XUARTPS_ISR_OFFSET, XUartPs_ReadReg(s_uart_base, XUARTPS_ISR_OFFSET));
    s_err_counters.rx_overrun = 0U;
    s_err_counters.rx_framing = 0U;
    s_err_counters.rx_parity  = 0U;

    s_uart_ready = true;
    return UART_DRV_OK;
}

void uart_drv_put_char(char ch)
{
    if (!s_uart_ready)
    {
        return;
    }

    /* XUartPs_SendByte() spins on TXFULL before writing the FIFO. */
    XUartPs_SendByte(s_uart_base, (u8)ch);
}

void uart_drv_write(const char *data, size_t length)
{
    size_t idx;

    if (data == NULL)
    {
        return;
    }

    for (idx = 0U; idx < length; idx++)
    {
        uart_drv_put_char(data[idx]);
    }
}

bool uart_drv_try_get_char(char *ch_out)
{
    if ((!s_uart_ready) || (ch_out == NULL))
    {
        return false;
    }

    uart_collect_line_errors();

    if (!XUartPs_IsReceiveData(s_uart_base))
    {
        return false;
    }

    *ch_out = (char)XUartPs_ReadReg(s_uart_base, XUARTPS_FIFO_OFFSET);
    return true;
}

void uart_drv_wait_tx_idle(void)
{
    uint32_t status;

    if (!s_uart_ready)
    {
        return;
    }

    /* TXEMPTY only covers the FIFO; TACTIVE covers the shift register. */
    do
    {
        status = XUartPs_ReadReg(s_uart_base, XUARTPS_SR_OFFSET);
    } while (((status & XUARTPS_SR_TXEMPTY) == 0U) || ((status & XUARTPS_SR_TACTIVE) != 0U));
}

void uart_drv_get_err_counters(uart_drv_err_counters_t *counters_out)
{
    if (counters_out == NULL)
    {
        return;
    }

    uart_collect_line_errors();
    *counters_out = s_err_counters;
}
