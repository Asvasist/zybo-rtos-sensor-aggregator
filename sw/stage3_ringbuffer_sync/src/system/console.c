/*
 * console.c
 */
#include "console.h"

#include <stdarg.h>
#include <stdio.h>

#include "uart_drv.h"

/* Longest line the app prints is a report row (~70 chars). */
#define CONSOLE_FMT_BUF_LEN     256U
#define CONSOLE_MAX_DECIMALS    3U

static char s_fmt_buf[CONSOLE_FMT_BUF_LEN];

void console_write(const char *text)
{
    if (text == NULL)
    {
        return;
    }

    while (*text != '\0')
    {
        /* Most terminals want CR+LF; keep the C sources using plain \n. */
        if (*text == '\n')
        {
            uart_drv_put_char('\r');
        }
        uart_drv_put_char(*text);
        text++;
    }
}

void console_printf(const char *fmt, ...)
{
    va_list args;
    int     needed_len;

    va_start(args, fmt);
    needed_len = vsnprintf(s_fmt_buf, sizeof(s_fmt_buf), fmt, args);
    va_end(args);

    if (needed_len < 0)
    {
        return;
    }

    console_write(s_fmt_buf);

    /* Make truncation visible instead of silently chopping the line. */
    if ((size_t)needed_len >= sizeof(s_fmt_buf))
    {
        console_write("~\n");
    }
}

const char *console_fmt_milli(char *buf, size_t buf_len, int32_t milli_value, uint32_t decimals)
{
    static const uint32_t pow10_table[CONSOLE_MAX_DECIMALS + 1U] = { 1U, 10U, 100U, 1000U };

    const char *sign = "";
    uint32_t    magnitude;
    uint32_t    drop_divisor;
    uint32_t    frac_unit;

    if ((buf == NULL) || (buf_len == 0U))
    {
        return "";
    }

    if (decimals > CONSOLE_MAX_DECIMALS)
    {
        decimals = CONSOLE_MAX_DECIMALS;
    }

    if (milli_value < 0)
    {
        sign = "-";
        /* written this way so INT32_MIN doesn't overflow on negation */
        magnitude = (uint32_t)(-(milli_value + 1)) + 1U;
    }
    else
    {
        magnitude = (uint32_t)milli_value;
    }

    drop_divisor = pow10_table[CONSOLE_MAX_DECIMALS - decimals];
    magnitude    = (magnitude + (drop_divisor / 2U)) / drop_divisor;
    frac_unit    = pow10_table[decimals];

    /* Don't print "-0.00" for something like -0.001 rounded to 2 places. */
    if (magnitude == 0U)
    {
        sign = "";
    }

    if (decimals == 0U)
    {
        (void)snprintf(buf, buf_len, "%s%lu", sign, (unsigned long)magnitude);
    }
    else
    {
        (void)snprintf(buf, buf_len, "%s%lu.%0*lu", sign,
                       (unsigned long)(magnitude / frac_unit),
                       (int)decimals,
                       (unsigned long)(magnitude % frac_unit));
    }

    return buf;
}
