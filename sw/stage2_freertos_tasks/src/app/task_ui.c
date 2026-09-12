/*
 * task_ui.c
 */
#include "task_ui.h"

#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "gpio_drv.h"
#include "task_consumer.h"
#include "uart_drv.h"

static TaskHandle_t s_ui_handle;

static void ui_handle_key(char key)
{
    switch (key)
    {
    case 'r':
    case 'R':
        task_consumer_post_event(CONSUMER_EVT_REPORT);
        break;

    case 's':
    case 'S':
        task_consumer_post_event(CONSUMER_EVT_STREAM_TOGGLE);
        break;

    case 'f':
    case 'F':
        task_consumer_post_event(CONSUMER_EVT_RATE_TOGGLE);
        break;

    case 't':
    case 'T':
        task_consumer_post_event(CONSUMER_EVT_PRINT_ONE);
        break;

    case 'd':
    case 'D':
        task_consumer_post_event(CONSUMER_EVT_STATS);
        break;

    case '\r':
    case '\n':
    case ' ':
        /* terminals send these on Enter; ignore quietly */
        break;

    default:
        /* 'h', '?' and anything else printable get the help text. */
        if ((key >= ' ') && (key <= '~'))
        {
            task_consumer_post_event(CONSUMER_EVT_HELP);
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
            task_consumer_post_event(CONSUMER_EVT_REPORT);
        }

        if (gpio_drv_btn_take_press(GPIO_BTN5))
        {
            task_consumer_post_event(CONSUMER_EVT_STREAM_TOGGLE);
        }

        /*
         * The RX FIFO holds 64 bytes and gets drained every 10 ms, which is far
         * more than anyone types. Pasting a large block into the terminal can
         * still overrun it; that shows up in the 'd' error counters.
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
