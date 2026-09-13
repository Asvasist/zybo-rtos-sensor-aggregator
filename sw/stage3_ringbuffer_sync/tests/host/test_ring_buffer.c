/*
 * test_ring_buffer.c
 *
 * Host-side unit tests for ring_buffer.c. No target, no RTOS, any C11 compiler:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -I../../src/datalog -I../../src/drivers \
 *       test_ring_buffer.c ../../src/datalog/ring_buffer.c -o test_ring_buffer
 *   ./test_ring_buffer
 *
 * Exit code 0 means every check passed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ring_buffer.h"

#define TEST_CAPACITY       8U
#define MODEL_MAX           64U
#define RANDOM_ITERATIONS   200000U

static unsigned s_checks_run;
static unsigned s_checks_failed;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        s_checks_run++;                                                         \
        if (!(cond))                                                            \
        {                                                                       \
            s_checks_failed++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                       \
    } while (0)

static sensor_record_t make_record(uint32_t seq)
{
    sensor_record_t record;

    memset(&record, 0, sizeof(record));
    record.seq        = seq;
    record.capture_us = (uint64_t)seq * 100000ULL;
    record.sensors.raw_code[XADC_SENSOR_DIE_TEMP] = (uint16_t)(seq & 0x0FFFU);
    return record;
}

static void test_init_rejects_bad_arguments(void)
{
    sensor_record_t storage[TEST_CAPACITY];
    ring_buffer_t   rb;

    printf("init rejects bad arguments\n");
    CHECK(!ring_buffer_init(NULL, storage, TEST_CAPACITY));
    CHECK(!ring_buffer_init(&rb, NULL, TEST_CAPACITY));
    CHECK(!ring_buffer_init(&rb, storage, 0U));
    CHECK(!ring_buffer_init(&rb, storage, 6U));
    CHECK(ring_buffer_init(&rb, storage, 1U));
    CHECK(ring_buffer_init(&rb, storage, TEST_CAPACITY));
}

static void test_empty_buffer(void)
{
    sensor_record_t storage[TEST_CAPACITY];
    sensor_record_t out[TEST_CAPACITY];
    ring_buffer_t   rb;

    printf("empty buffer\n");
    CHECK(ring_buffer_init(&rb, storage, TEST_CAPACITY));
    CHECK(ring_buffer_count(&rb) == 0U);
    CHECK(ring_buffer_pop(&rb, out, TEST_CAPACITY) == 0U);
    CHECK(!ring_buffer_peek_newest(&rb, &out[0]));
}

static void test_fifo_order_and_partial_pop(void)
{
    sensor_record_t storage[TEST_CAPACITY];
    sensor_record_t out[TEST_CAPACITY];
    sensor_record_t record;
    ring_buffer_t   rb;
    uint32_t        seq;

    printf("fifo order, partial pops\n");
    CHECK(ring_buffer_init(&rb, storage, TEST_CAPACITY));

    for (seq = 1U; seq <= 3U; seq++)
    {
        record = make_record(seq);
        CHECK(!ring_buffer_push(&rb, &record));
    }
    CHECK(ring_buffer_count(&rb) == 3U);

    CHECK(ring_buffer_pop(&rb, out, 2U) == 2U);
    CHECK(out[0].seq == 1U);
    CHECK(out[1].seq == 2U);
    CHECK(ring_buffer_count(&rb) == 1U);

    CHECK(ring_buffer_pop(&rb, out, TEST_CAPACITY) == 1U);
    CHECK(out[0].seq == 3U);
    CHECK(out[0].capture_us == 300000ULL);
    CHECK(ring_buffer_count(&rb) == 0U);
}

static void test_overwrite_oldest_when_full(void)
{
    sensor_record_t storage[TEST_CAPACITY];
    sensor_record_t out[TEST_CAPACITY];
    sensor_record_t record;
    ring_buffer_t   rb;
    uint32_t        seq;
    uint32_t        idx;

    printf("overwrite oldest when full\n");
    CHECK(ring_buffer_init(&rb, storage, TEST_CAPACITY));

    for (seq = 1U; seq <= TEST_CAPACITY; seq++)
    {
        record = make_record(seq);
        CHECK(!ring_buffer_push(&rb, &record));
    }
    CHECK(ring_buffer_count(&rb) == TEST_CAPACITY);
    CHECK(rb.fill_high_water == TEST_CAPACITY);

    for (seq = TEST_CAPACITY + 1U; seq <= TEST_CAPACITY + 3U; seq++)
    {
        record = make_record(seq);
        CHECK(ring_buffer_push(&rb, &record));
    }
    CHECK(ring_buffer_count(&rb) == TEST_CAPACITY);
    CHECK(rb.overwritten == 3U);

    /* 1..3 are gone, 4..11 remain in order */
    CHECK(ring_buffer_pop(&rb, out, TEST_CAPACITY) == TEST_CAPACITY);
    for (idx = 0U; idx < TEST_CAPACITY; idx++)
    {
        CHECK(out[idx].seq == (4U + idx));
    }
}

static void test_peek_newest(void)
{
    sensor_record_t storage[TEST_CAPACITY];
    sensor_record_t out[TEST_CAPACITY];
    sensor_record_t record;
    ring_buffer_t   rb;
    uint32_t        seq;

    printf("peek newest\n");
    CHECK(ring_buffer_init(&rb, storage, TEST_CAPACITY));

    for (seq = 1U; seq <= 5U; seq++)
    {
        record = make_record(seq);
        (void)ring_buffer_push(&rb, &record);
        CHECK(ring_buffer_peek_newest(&rb, &out[0]));
        CHECK(out[0].seq == seq);
    }
    CHECK(ring_buffer_count(&rb) == 5U);

    /* still available after everything has been popped */
    CHECK(ring_buffer_pop(&rb, out, TEST_CAPACITY) == 5U);
    CHECK(ring_buffer_peek_newest(&rb, &out[0]));
    CHECK(out[0].seq == 5U);
}

static void test_counter_wrap(void)
{
    sensor_record_t storage[TEST_CAPACITY];
    sensor_record_t out[TEST_CAPACITY];
    sensor_record_t record;
    ring_buffer_t   rb;
    uint32_t        seq;
    uint32_t        idx;

    printf("free-running counters across the 2^32 wrap\n");
    CHECK(ring_buffer_init(&rb, storage, TEST_CAPACITY));

    /* White-box: start just below the wrap to cover it without 4 billion pushes. */
    rb.head = 0xFFFFFFFCUL;
    rb.tail = 0xFFFFFFFCUL;

    for (seq = 1U; seq <= 10U; seq++)
    {
        record = make_record(seq);
        (void)ring_buffer_push(&rb, &record);
        CHECK(ring_buffer_count(&rb) <= TEST_CAPACITY);
    }
    CHECK(rb.head == 6U);
    CHECK(ring_buffer_count(&rb) == TEST_CAPACITY);
    CHECK(rb.overwritten == 2U);

    CHECK(ring_buffer_pop(&rb, out, TEST_CAPACITY) == TEST_CAPACITY);
    for (idx = 0U; idx < TEST_CAPACITY; idx++)
    {
        CHECK(out[idx].seq == (3U + idx));
    }
}

/* Deterministic LCG so a failure can be reproduced. */
static uint32_t s_rand_state = 12345U;

static uint32_t next_rand(void)
{
    s_rand_state = (s_rand_state * 1103515245UL) + 12345UL;
    return s_rand_state >> 8;
}

/*
 * Random mix of pushes and pops of random sizes, checked against a naive
 * shift-array model with the same drop-oldest behaviour.
 */
static void test_random_against_model(void)
{
    sensor_record_t storage[TEST_CAPACITY];
    sensor_record_t out[TEST_CAPACITY];
    sensor_record_t record;
    ring_buffer_t   rb;
    uint32_t        model[MODEL_MAX];
    uint32_t        model_count = 0U;
    uint32_t        model_dropped = 0U;
    uint32_t        next_seq = 1U;
    uint32_t        iter;
    unsigned        failures_before = s_checks_failed;

    printf("random push/pop against a reference model (%u iterations)\n", RANDOM_ITERATIONS);
    CHECK(ring_buffer_init(&rb, storage, TEST_CAPACITY));

    for (iter = 0U; (iter < RANDOM_ITERATIONS) && (s_checks_failed == failures_before); iter++)
    {
        if ((next_rand() % 3U) != 0U)
        {
            const bool model_drops = (model_count == TEST_CAPACITY);

            if (model_drops)
            {
                memmove(&model[0], &model[1], (model_count - 1U) * sizeof(model[0]));
                model_count--;
                model_dropped++;
            }
            model[model_count++] = next_seq;

            record = make_record(next_seq);
            CHECK(ring_buffer_push(&rb, &record) == model_drops);
            next_seq++;
        }
        else
        {
            const uint32_t request  = next_rand() % (TEST_CAPACITY + 2U);
            const uint32_t expected = (request < model_count) ? request : model_count;
            const uint32_t got      = ring_buffer_pop(&rb, out, (request < TEST_CAPACITY) ? request : TEST_CAPACITY);
            uint32_t       idx;

            CHECK(got == ((expected < TEST_CAPACITY) ? expected : TEST_CAPACITY));
            for (idx = 0U; idx < got; idx++)
            {
                CHECK(out[idx].seq == model[idx]);
            }
            memmove(&model[0], &model[got], (model_count - got) * sizeof(model[0]));
            model_count -= got;
        }

        CHECK(ring_buffer_count(&rb) == model_count);
        CHECK(rb.overwritten == model_dropped);
    }

    if (s_checks_failed != failures_before)
    {
        printf("  stopped at iteration %u\n", iter);
    }
}

int main(void)
{
    test_init_rejects_bad_arguments();
    test_empty_buffer();
    test_fifo_order_and_partial_pop();
    test_overwrite_oldest_when_full();
    test_peek_newest();
    test_counter_wrap();
    test_random_against_model();

    printf("\n%u checks, %u failed\n", s_checks_run, s_checks_failed);
    return (s_checks_failed == 0U) ? 0 : 1;
}
