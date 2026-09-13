/*
 * fault.h
 *
 * Last-resort error handling: say what went wrong, then park the system with
 * LD4 flashing fast.
 *
 * Deliberately independent of the RTOS and of printf. It is called before the
 * scheduler starts, from tasks, and from the FreeRTOS hooks, where the stack
 * of the task that failed can't be trusted. IRQs are masked at the CPU first,
 * so once in here nothing else runs. The message may land in the middle of a
 * half-printed telemetry line, hence the leading newlines.
 *
 * Once the SWDT is running, nothing kicks it after a fault, so the halt ends
 * in a board reset a watchdog timeout later.
 */
#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

/* Prints "FATAL: <what> failed, code <code>" and halts. */
void fault_halt(const char *what, int32_t code) __attribute__((noreturn));

/* Prints "FATAL: <what>: <detail>" and halts. */
void fault_halt_str(const char *what, const char *detail) __attribute__((noreturn));

#endif /* FAULT_H */
