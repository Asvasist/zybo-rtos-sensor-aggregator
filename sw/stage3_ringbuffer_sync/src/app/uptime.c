/*
 * uptime.c
 */
#include "uptime.h"

/*
 * Classic BSPs provide XTime_GetTime() in xtime_l.h. In the SDT flow it
 * comes from the xiltimer library instead (default sleep timer = the
 * global timer), and xtime_l.h doesn't exist.
 */
#ifdef SDT
#include "xiltimer.h"
#else
#include "xtime_l.h"
#endif

#define USEC_PER_SEC            1000000ULL

/*
 * xiltimer generates "#define COUNTS_PER_SECOND XPAR_CPU_CORE_CLOCK_FREQ_HZ/2"
 * with no parentheses, so "counts / COUNTS_PER_SECOND" would divide by the
 * clock and then by 2 separately. Wrap it once here and never use it bare.
 */
#define UPTIME_COUNTS_PER_SEC   ((uint64_t)(COUNTS_PER_SECOND))

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
    whole_sec  = counts / UPTIME_COUNTS_PER_SEC;
    rem_counts = counts % UPTIME_COUNTS_PER_SEC;

    return (whole_sec * USEC_PER_SEC) + ((rem_counts * USEC_PER_SEC) / UPTIME_COUNTS_PER_SEC);
}

uint32_t uptime_ms(void)
{
    return (uint32_t)(uptime_us() / 1000ULL);
}
