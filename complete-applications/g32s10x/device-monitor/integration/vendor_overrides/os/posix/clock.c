#include <os.h>
#include <driver/systick.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include "time_utils.h"

clock_t clock(void)
{
#if CLOCKS_PER_SEC == 1000
    return thread_get_totalruntime(NULL) / 1000;
#elif CLOCKS_PER_SEC == 1000000
    return thread_get_totalruntime(NULL);
#else
    return thread_get_totalruntime(NULL) * CLOCKS_PER_SEC / 1000000;
#endif
}

int clock_getcpuclockid(pid_t pid, clockid_t *clock_id)
{
    (void) pid;

    if (clock_id)
        *clock_id = 0;

    return 0;
}

int clock_getres(clockid_t clock_id, struct timespec *res)
{
    (void) clock_id;

    if(res) {
        res->tv_sec = 0;
        res->tv_nsec = 1000;
    }

    return 0;
}


int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    uint64_t us = systick_get_time_us();

    if (tp) {
        if (clk_id == CLOCK_REALTIME) {
            struct timeval tv;

            if (gettimeofday(&tv, NULL) == 0) {
                tp->tv_sec = tv.tv_sec;
                tp->tv_nsec = tv.tv_usec * 1000;
                return 0;
            }
        }
        usecs_to_timespec(us, tp);
    }

    return 0;
}

int clock_nanosleep(clockid_t clock_id, int flags,
    const struct timespec *rqtp, struct timespec *rmtp)
{
    (void) clock_id;
    (void) flags;

    if (!timespec_is_valid(rqtp))
        return EINVAL;

    usleep(timespec_to_usecs(rqtp));

    if (rmtp)
        usecs_to_timespec(0, rmtp);

    return 0;
}

int clock_settime(clockid_t clock_id, const struct timespec *tp)
{
    (void) clock_id;
    (void) tp;

    errno = EPERM;

    return -1;
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    int ret = clock_nanosleep(0, 0, rqtp, rmtp);

    if (ret) {
        errno = ret;
        return -1;
    }

    return 0;
}
