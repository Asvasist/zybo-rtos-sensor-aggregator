/*
 * uptime.c
 */
#include "uptime.h"

#include "xtime_l.h"

#define USEC_PER_SEC    1000000ULL

uint64_t uptime_us(void)
{
    XTime    counts;
    uint64_t whole_sec;
    uint64_t rem_counts;

    XTime_GetTime(&counts);

    /*
     * counts * 10^6 would overflow 64 bits after about 15 hours of uptime.
     * Splitting off the whole seconds first keeps the multiply small:
     * the remainder is below COUNTS_PER_SECOND (< 2^29).
     */
    whole_sec  = counts / COUNTS_PER_SECOND;
    rem_counts = counts % COUNTS_PER_SECOND;

    return (whole_sec * USEC_PER_SEC) + ((rem_counts * USEC_PER_SEC) / COUNTS_PER_SECOND);
}

uint32_t uptime_ms(void)
{
    return (uint32_t)(uptime_us() / 1000ULL);
}
