#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <driver/rtc.h>
#include <driver/systick.h>
#include <common.h>

static int get_rtc_wall_time(time_t *seconds)
{
#ifdef CONFIG_RTC
    struct rtc_time rtc_tm;
    unsigned long long local_seconds;
    int64_t utc_seconds;
    time_t value;

    if (seconds == NULL) {
        return -1;
    }
    rtc_get_current_tm(&rtc_tm);
    if (rtc_tm.tm_year + 1900 < 2024 || rtc_valid_tm(&rtc_tm) != 0) {
        return -1;
    }
    rtc_tm_to_time(&rtc_tm, &local_seconds);

    /* RTC stores local civil time; _timezone is seconds west of UTC. */
    utc_seconds = (int64_t)local_seconds + (int64_t)_timezone;
    if (utc_seconds < 0) {
        return -1;
    }
    value = (time_t)utc_seconds;
    if ((int64_t)value != utc_seconds) {
        return -1;
    }
    *seconds = value;
    return 0;
#else
    (void)seconds;
    return -1;
#endif
}

__weak int gettimeofday(struct timeval *tv, void *tz_)
{
    struct timezone *tz = tz_;

    if (tv) {
        time_t wall_time;

        if (get_rtc_wall_time(&wall_time) == 0) {
            tv->tv_sec = wall_time;
            tv->tv_usec = 0;
        } else {
            uint64_t us = systick_get_time_us();

            tv->tv_sec = us / USEC_PER_SEC;
            tv->tv_usec = us % USEC_PER_SEC;
        }
    }

    if (tz) {
        tz->tz_dsttime = 0;
        tz->tz_minuteswest = 0;
    }

    return 0;
}

int _gettimeofday(struct timeval *tv, void *tz_)
{
    return gettimeofday(tv, tz_);
}

__weak int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    uint64_t us = systick_get_time_us();

    if (tp) {
        time_t wall_time;

        if (clk_id == CLOCK_REALTIME && get_rtc_wall_time(&wall_time) == 0) {
            tp->tv_sec = wall_time;
            tp->tv_nsec = 0;
        } else {
            tp->tv_sec = us / USEC_PER_SEC;
            tp->tv_nsec = us % USEC_PER_SEC * 1000;
        }
    }

    return 0;
}
