/*
 * sensor_log.c
 */
#include "sensor_log.h"

#include <stddef.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

#include "app_config.h"
#include "board_zybo.h"
#include "ring_buffer.h"
#include "uptime.h"

#if !defined(configUSE_MUTEXES) || (configUSE_MUTEXES != 1)
#error "sensor_log needs configUSE_MUTEXES = 1 in the FreeRTOS BSP settings"
#endif

/*
 * The record storage: APP_LOG_CAPACITY x 32 bytes.
 *
 * Named .bss.* so the stock Xilinx linker script collects it with the rest of
 * .bss - placed in ps7_ddr_0 and zeroed by the startup code - while it still
 * shows up under its own name in the map file. Search the .map for
 * ".bss.sensor_log" to see exactly where it landed. sensor_log_init()
 * double-checks the address at runtime.
 */
static sensor_record_t   s_log_storage[APP_LOG_CAPACITY] __attribute__((section(".bss.sensor_log")));

/* Everything below is only touched with s_log_mutex held. */
static ring_buffer_t     s_ring;
static uint32_t          s_lock_hold_max_us;

static SemaphoreHandle_t s_log_mutex;

static bool sensor_log_lock(TickType_t max_wait, uint64_t *locked_at_us)
{
    if ((s_log_mutex == NULL) || (xSemaphoreTake(s_log_mutex, max_wait) != pdTRUE))
    {
        return false;
    }

    *locked_at_us = uptime_us();
    return true;
}

static void sensor_log_unlock(uint64_t locked_at_us)
{
    const uint32_t held_us = (uint32_t)(uptime_us() - locked_at_us);

    /* Still inside the lock, so the statistic needs no protection of its own. */
    if (held_us > s_lock_hold_max_us)
    {
        s_lock_hold_max_us = held_us;
    }

    (void)xSemaphoreGive(s_log_mutex);
}

#if (APP_TEST_LOG_HOLD_US > 0U)
static void sensor_log_test_hold(void)
{
    const uint64_t start_us = uptime_us();

    /* A spin, not a delay: simulates slow work done while holding the lock. */
    while ((uptime_us() - start_us) < APP_TEST_LOG_HOLD_US)
    {
        /* spin */
    }
}
#endif

sensor_log_status_t sensor_log_init(void)
{
    const uintptr_t storage_first = (uintptr_t)&s_log_storage[0];
    const uintptr_t storage_last  = storage_first + sizeof(s_log_storage) - 1U;

    /*
     * The one piece of RAM this design promises is in DDR. A custom linker
     * script that moves .bss into OCM would otherwise break that silently.
     */
    if ((storage_first < BOARD_DDR_BASE_ADDR) || (storage_last > BOARD_DDR_HIGH_ADDR))
    {
        return SENSOR_LOG_ERR_NOT_DDR;
    }

    if (!ring_buffer_init(&s_ring, s_log_storage, APP_LOG_CAPACITY))
    {
        return SENSOR_LOG_ERR_CAPACITY;
    }

    s_log_mutex = xSemaphoreCreateMutex();
    if (s_log_mutex == NULL)
    {
        return SENSOR_LOG_ERR_NO_MUTEX;
    }

#if defined(configQUEUE_REGISTRY_SIZE) && (configQUEUE_REGISTRY_SIZE > 0)
    /* Makes the mutex show up by name in kernel-aware debug views. */
    vQueueAddToRegistry(s_log_mutex, "log_mutex");
#endif

    return SENSOR_LOG_OK;
}

void sensor_log_storage_info(uintptr_t *base_addr_out, uint32_t *size_bytes_out, uint32_t *capacity_out)
{
    if (base_addr_out != NULL)
    {
        *base_addr_out = (uintptr_t)&s_log_storage[0];
    }
    if (size_bytes_out != NULL)
    {
        *size_bytes_out = (uint32_t)sizeof(s_log_storage);
    }
    if (capacity_out != NULL)
    {
        *capacity_out = APP_LOG_CAPACITY;
    }
}

bool sensor_log_write(const sensor_record_t *record, TickType_t max_wait, bool *was_empty_out)
{
    uint64_t locked_at_us;
    bool     was_empty;

    if (record == NULL)
    {
        return false;
    }

    if (!sensor_log_lock(max_wait, &locked_at_us))
    {
        return false;
    }

    was_empty = (ring_buffer_count(&s_ring) == 0U);
    (void)ring_buffer_push(&s_ring, record);    /* overwrites are counted inside the ring */

    sensor_log_unlock(locked_at_us);

    if (was_empty_out != NULL)
    {
        *was_empty_out = was_empty;
    }

    return true;
}

bool sensor_log_read(sensor_record_t *records_out, uint32_t max_records, TickType_t max_wait,
                     uint32_t *count_out)
{
    uint64_t locked_at_us;
    uint32_t popped;

    if ((records_out == NULL) || (count_out == NULL))
    {
        return false;
    }

    *count_out = 0U;

    if (!sensor_log_lock(max_wait, &locked_at_us))
    {
        return false;
    }

    popped = ring_buffer_pop(&s_ring, records_out, max_records);

#if (APP_TEST_LOG_HOLD_US > 0U)
    sensor_log_test_hold();
#endif

    sensor_log_unlock(locked_at_us);

    *count_out = popped;
    return true;
}

bool sensor_log_peek_newest(sensor_record_t *record_out, TickType_t max_wait)
{
    uint64_t locked_at_us;
    bool     found;

    if (record_out == NULL)
    {
        return false;
    }

    if (!sensor_log_lock(max_wait, &locked_at_us))
    {
        return false;
    }

    found = ring_buffer_peek_newest(&s_ring, record_out);

    sensor_log_unlock(locked_at_us);

    return found;
}

bool sensor_log_get_stats(sensor_log_stats_t *stats_out, TickType_t max_wait)
{
    uint64_t locked_at_us;

    if (stats_out == NULL)
    {
        return false;
    }

    if (!sensor_log_lock(max_wait, &locked_at_us))
    {
        return false;
    }

    stats_out->capacity         = s_ring.capacity;
    stats_out->fill             = ring_buffer_count(&s_ring);
    stats_out->fill_high_water  = s_ring.fill_high_water;
    stats_out->written          = s_ring.head;
    stats_out->read             = s_ring.tail - s_ring.overwritten;
    stats_out->overwritten      = s_ring.overwritten;
    stats_out->lock_hold_max_us = s_lock_hold_max_us;

    sensor_log_unlock(locked_at_us);

    return true;
}
