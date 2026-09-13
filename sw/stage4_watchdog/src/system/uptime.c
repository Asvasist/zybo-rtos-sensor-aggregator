/*
 * uptime.c
 */
#include "uptime.h"

#include "sleep.h"

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

bool uptime_init(void)
{
    XTime counts;

    /*
     * The classic BSP starts the global timer in its C startup code. The SDT
     * xiltimer library doesn't: it starts it - and zeroes the count - on the
     * first sleep call, and a firmware that never sleeps reads 0 forever
     * (every timestamp on the first board run was 0.000).
     *
     * A 1 us sleep goes through exactly that path. It is a plain busy-wait
     * with no RTOS involvement, so it's fine before the scheduler starts, and
     * once xiltimer has started the timer it never zeroes it again, so a
     * sleep call anywhere later can't make the uptime jump backwards. On the
     * classic BSP the timer is already running and this costs 1 us.
     */
    usleep(1U);

    XTime_GetTime(&counts);
    return counts != 0U;
}

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
