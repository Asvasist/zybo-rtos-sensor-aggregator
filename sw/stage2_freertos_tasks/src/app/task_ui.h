/*
 * task_ui.h
 *
 * UI task: button debouncing and terminal key input.
 *
 * Runs every 10 ms from vTaskDelayUntil(). Tick-based timing is plenty for a
 * human interface, and it keeps the UI off the sample timer entirely. The
 * task owns the button scan and UART RX and turns what it sees into event
 * bits for the consumer. It never prints.
 */
#ifndef TASK_UI_H
#define TASK_UI_H

#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"

bool task_ui_create(void);

TaskHandle_t task_ui_handle(void);

#endif /* TASK_UI_H */
