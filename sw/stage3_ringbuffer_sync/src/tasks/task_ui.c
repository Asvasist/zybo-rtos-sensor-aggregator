/*
 * task_ui.c
 */
#include "task_ui.h"

#include <stddef.h>

#include "app_config.h"
#include "gpio_drv.h"
#include "task_consumer.h"
#include "uart_drv.h"

static TaskHandle_t s_ui_handle;

/* Written by the UI task only; a plain 32-bit read from another task is atomic on this core. */
static uint32_t     s_dropped_msgs;

static void ui_send(consumer_msg_id_t id, char key)
{
    if (!task_consumer_send(id, key))
    {
        /*
         * Queue full: the consumer is stuck or far behind. Dropping the key
         * press is the right call - waiting here would stall the button scan
         * and the RX FIFO drain along with it.
         */
        s_dropped_msgs++;
    }
}

static void ui_handle_key(char key)
{
    switch (key)
    {
    case 'r':
    case 'R':
        ui_send(CONSUMER_MSG_REPORT, key);
        break;

    case 's':
    case 'S':
        ui_send(CONSUMER_MSG_STREAM_TOGGLE, key);
        break;

    case 'f':
    case 'F':
        ui_send(CONSUMER_MSG_RATE_TOGGLE, key);
        break;

    case 'p':
    case 'P':
        ui_send(CONSUMER_MSG_PAUSE_TOGGLE, key);
        break;

    case 't':
    case 'T':
        ui_send(CONSUMER_MSG_PRINT_NEWEST, key);
        break;

    case 'd':
    case 'D':
        ui_send(CONSUMER_MSG_STATS, key);
        break;

    case 'h':
    case 'H':
    case '?':
        ui_send(CONSUMER_MSG_HELP, key);
        break;

    case '\r':
    case '\n':
    case ' ':
        /* terminals send these on Enter; ignore quietly */
        break;

    default:
        if ((key >= ' ') && (key <= '~'))
        {
            /* The queue can carry the character, so the consumer can say which key it didn't know. */
            ui_send(CONSUMER_MSG_UNKNOWN_KEY, key);
        }
        break;
    }
}

static void ui_task(void *task_arg)
{
    const TickType_t scan_period = pdMS_TO_TICKS(APP_UI_SCAN_PERIOD_MS);
    TickType_t       last_wake   = xTaskGetTickCount();
    char             rx_char;

    (void)task_arg;

    portTASK_USES_FLOATING_POINT();

    for (;;)
    {
        /* Fixed-rate, not fixed-delay: the debounce time assumes an even scan period. */
        vTaskDelayUntil(&last_wake, scan_period);

        gpio_drv_btn_scan();

        if (gpio_drv_btn_take_press(GPIO_BTN4))
        {
            ui_send(CONSUMER_MSG_REPORT, '\0');
        }

        if (gpio_drv_btn_take_press(GPIO_BTN5))
        {
            ui_send(CONSUMER_MSG_STREAM_TOGGLE, '\0');
        }

        /*
         * The RX FIFO holds 64 bytes and gets drained every 10 ms, which is far
         * more than anyone types. Pasting a large block into the terminal can
         * still overrun it (UART error counters) or fill the consumer queue
         * (dropped messages) - both show up in 'd'.
         */
        while (uart_drv_try_get_char(&rx_char))
        {
            ui_handle_key(rx_char);
        }
    }
}

bool task_ui_create(void)
{
    return xTaskCreate(ui_task, "ui", APP_STACK_UI, NULL, APP_PRIO_UI, &s_ui_handle) == pdPASS;
}

TaskHandle_t task_ui_handle(void)
{
    return s_ui_handle;
}

uint32_t task_ui_dropped_msgs(void)
{
    return s_dropped_msgs;
}
