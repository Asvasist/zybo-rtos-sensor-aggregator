/*
 * console.h
 *
 * Thin text layer over uart_drv: printf-style output with "\n" -> "\r\n"
 * translation, plus a fixed-point formatter so the firmware never needs
 * float support in printf.
 *
 * Not reentrant - one shared format buffer. That's fine in the stage 1
 * super-loop; under FreeRTOS the console gets its own mutex (or a single
 * owner task) before more than one task is allowed to print.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdint.h>

void console_write(const char *text);

void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Formats a value held in milli-units as a decimal string with 0..3 digits
 * after the point, rounded to nearest. Example: (45213, 2) -> "45.21".
 * Returns buf so it can be used inline as a printf argument.
 */
const char *console_fmt_milli(char *buf, size_t buf_len, int32_t milli_value, uint32_t decimals);

#endif /* CONSOLE_H */
