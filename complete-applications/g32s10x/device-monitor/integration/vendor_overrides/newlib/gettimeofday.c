#include <time.h>
#include <sys/time.h>
#include <driver/systick.h>
#include <common.h>

__weak int gettimeofday(struct timeval *tv, void *tz_)
{
    struct timezone *tz = tz_;

    if (tv) {
        uint64_t us = systick_get_time_us();
        tv->tv_sec = us / USEC_PER_SEC;
        tv->tv_usec = us % USEC_PER_SEC;
    }

    if (tz) {
        tz->tz_dsttime = 0;
        tz->tz_minuteswest = 0;
    }

    return 0;
}

__weak int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    uint64_t us = systick_get_time_us();

    if (tp) {
        tp->tv_sec = us / USEC_PER_SEC;
        tp->tv_nsec = us % USEC_PER_SEC * 1000;
    }

    return 0;
}
