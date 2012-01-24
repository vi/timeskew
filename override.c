#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <time.h>

static int (*orig_clock_gettime)(clockid_t, struct timespec*) = NULL;
static int (*orig_nanosleep)(struct timespec*, struct timespec*) = NULL;
static int (*orig_gettimeofday)(struct timeval*, struct timezone*) = NULL;
static struct timespec timebase_monotonic;
static struct timeval timebase_gettimeofday;

struct tiacc {
    long long int lastsysval;
    long long int lastourval;
};

static struct tiacc accumulators[2] = {{0,0}, {0,0}};

static int num = 2;
static int denom = 1;

static long long int filter_time(long long int nanos, struct tiacc* acc) {
    int delta = nanos - acc->lastsysval;
    acc->lastsysval = nanos;

    delta = delta * num / denom;
    acc->lastourval+=delta;
    return acc->lastourval;
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    if(!orig_clock_gettime) {
        orig_clock_gettime = dlsym(RTLD_NEXT, "clock_gettime");
        orig_clock_gettime(CLOCK_MONOTONIC, &timebase_monotonic);
    }
    int ret = orig_clock_gettime(clk_id, tp);


    if (clk_id == CLOCK_MONOTONIC) {
        long long q = 1000000000LL*(tp->tv_sec - timebase_monotonic.tv_sec)
            + (tp->tv_nsec - timebase_monotonic.tv_nsec);

        q = filter_time(q, accumulators+0);

        tp->tv_sec = (q/1000000000)+timebase_monotonic.tv_sec;
        tp->tv_nsec = q%1000000000+timebase_monotonic.tv_nsec;
        if (tp->tv_nsec >= 1000000000) {
            tp->tv_nsec-=1000000000;
            tp->tv_sec+=1;
        }
    }

    return ret;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    if(!orig_gettimeofday) {
        orig_gettimeofday = dlsym(RTLD_NEXT, "gettimeofday");
        gettimeofday(&timebase_gettimeofday, NULL);
    }
    int ret = orig_gettimeofday(tv, tz);
    
    long long q = 1000000LL*(tv->tv_sec-timebase_gettimeofday.tv_sec)
        + (tv->tv_usec - timebase_gettimeofday.tv_usec);

    q = filter_time(q*1000LL, accumulators+1)/1000;

    tv->tv_sec = (q/1000000)+timebase_gettimeofday.tv_sec;
    tv->tv_usec = q%1000000+timebase_gettimeofday.tv_usec;
    if (tv->tv_usec >= 1000000) {
        tv->tv_usec-=1000000;
        tv->tv_sec+=1;
    }

    return ret;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if(!orig_nanosleep) { 
        orig_nanosleep = dlsym(RTLD_NEXT, "nanosleep");
    }
    long long q = 1000000000LL*(req->tv_sec) + req->tv_nsec;

    q = q * denom / num;

    struct timespec ts;

    ts.tv_sec = (q/1000000000);
    ts.tv_nsec = q%1000000000;

    int ret = orig_nanosleep(&ts, rem);
                 
    if (rem) {
        q = 1000000000LL*(rem->tv_sec) + rem->tv_nsec;

        q = q * num / denom;

        rem->tv_sec = (q/1000000000);
        rem->tv_nsec = q%1000000000;

    }          

    return ret;
}
