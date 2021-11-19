#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <sys/select.h>
#include <time.h>

static int (*orig_clock_gettime)(clockid_t, struct timespec*) = NULL;
static int (*orig_nanosleep)(struct timespec*, struct timespec*) = NULL;
static int (*orig_gettimeofday)(struct timeval*, struct timezone*) = NULL;
static int (*orig_select)(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout) = NULL;
static int (*orig_pselect) (int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, const struct timespec *timeout,
                   const sigset_t *sigmask) = NULL;

static struct timespec timebase_monotonic;
static struct timespec timebase_realtime;
static struct timeval timebase_gettimeofday;

struct tiacc {
    long long int lastsysval;
    long long int lastourval;
};

static struct tiacc accumulators[3] = {{0,0}, {0,0}, {0,0}};

static int num = 1;
static int denom = 1;
static long long int shift = 0;

static int maint_period=-2;
static int maint_counter=0;

static void maint() {
    if (maint_counter==0) {
        if(getenv("TIMESKEW")) {
            sscanf(getenv("TIMESKEW"), "%i%i", &num, &denom);
        }
        if(getenv("TIMESHIFT")) {
            sscanf(getenv("TIMESHIFT"), "%lli", &shift);
        }
    }

    if (maint_period == -2) {
        if(getenv("MAINT_PERIOD")) {
            maint_period = atoi(getenv("MAINT_PERIOD"));
        } else {
            maint_period = 1024;
        }
    }

    ++maint_counter;
    if(maint_period>=0 && maint_counter>=maint_period) {
        maint_counter=1;
    } else {
        return;
    }


    FILE* f=fopen("timeskew", "r");
    if(f) {
        fscanf(f, "%i%i", &num, &denom);
        fclose(f);
    } else {
        if(!getenv("TIMESKEW")) {
            fprintf(stderr, "Usage: LD_PRELOAD=./libtimeskew.so program\n"
                    "    use 'timeskew' file in current directory or\n"
                    "    TIMESKEW environment variable with the following content:\n"
                    "    \"numerator demoninator\" - two numbers separated by space\n"
                    "    File will be reread to readjust time skew.\n");
        }
    }
}

static long long int filter_time(long long int nanos, struct tiacc* acc) {
    maint();
    long long int delta = nanos - acc->lastsysval;
    acc->lastsysval = nanos;

    delta = delta * num / denom;
    acc->lastourval+=delta;
    return acc->lastourval;
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    if(!orig_clock_gettime) {
        orig_clock_gettime = dlsym(RTLD_NEXT, "clock_gettime");
        (*orig_clock_gettime)(CLOCK_MONOTONIC, &timebase_monotonic);
        (*orig_clock_gettime)(CLOCK_REALTIME , &timebase_realtime);
    }
    int ret = orig_clock_gettime(clk_id, tp);


    if (clk_id == CLOCK_MONOTONIC) {
        long long q = 1000000000LL*(tp->tv_sec - timebase_monotonic.tv_sec)
            + (tp->tv_nsec - timebase_monotonic.tv_nsec);

        q = filter_time(q, accumulators+0);

        tp->tv_sec = (q/1000000000)+timebase_monotonic.tv_sec + shift;
        tp->tv_nsec = q%1000000000+timebase_monotonic.tv_nsec;
        if (tp->tv_nsec >= 1000000000) {
            tp->tv_nsec-=1000000000;
            tp->tv_sec+=1;
        }
    }else
    { // if (clk_id == CLOCK_REALTIME) {
        long long q = 1000000000LL*(tp->tv_sec - timebase_realtime.tv_sec)
            + (tp->tv_nsec - timebase_realtime.tv_nsec);

        q = filter_time(q, accumulators+2);

        tp->tv_sec = (q/1000000000)+timebase_realtime.tv_sec + shift;
        tp->tv_nsec = q%1000000000+timebase_realtime.tv_nsec;
        if (tp->tv_nsec >= 1000000000) {
            tp->tv_nsec-=1000000000;
            tp->tv_sec+=1;
        }
    }

    return ret;
}

int gettimeofday(struct timeval *tv, void*tz) {
    if(!orig_gettimeofday) {
        orig_gettimeofday = dlsym(RTLD_NEXT, "gettimeofday");
        (*orig_gettimeofday)(&timebase_gettimeofday, NULL);
    }
    int ret = orig_gettimeofday(tv, tz);
    
    long long q = 1000000LL * (tv->tv_sec - timebase_gettimeofday.tv_sec)
        + (tv->tv_usec - timebase_gettimeofday.tv_usec);

    q = filter_time(q*1000LL, accumulators+1)/1000;

    tv->tv_sec = (q/1000000)+timebase_gettimeofday.tv_sec + shift;
    tv->tv_usec = q%1000000+timebase_gettimeofday.tv_usec;
    if (tv->tv_usec >= 1000000) {
        tv->tv_usec-=1000000;
        tv->tv_sec+=1;
    }

    return ret;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    maint();
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

int select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout) {
    maint();
    if(!orig_select) { 
        orig_select = dlsym(RTLD_NEXT, "select");
    }

    struct timeval ts;
    struct timeval *tsptr = NULL;

    if (timeout) {
        long long q = 1000000LL*(timeout->tv_sec) + timeout->tv_usec;

        q = q * denom / num;

        ts.tv_sec = (q/1000000);
        ts.tv_usec = q%1000000;
        tsptr = &ts;
    }

    int ret = orig_select(nfds, readfds, writefds, exceptfds, tsptr);


    if (timeout) {
        long long q = 1000000LL*(tsptr->tv_sec) + tsptr->tv_usec;

        q = q * num / denom;

        timeout->tv_sec = (q/1000000);
        timeout->tv_usec = q%1000000;
    }

    return ret;
}
int pselect(int nfds, fd_set *readfds, fd_set *writefds,
        fd_set *exceptfds, const struct timespec *timeout,
        const sigset_t *sigmask) {
    maint();
    if(!orig_pselect) {
        orig_pselect = dlsym(RTLD_NEXT, "pselect");
    }

    struct timespec ts;
    struct timespec *tsptr = NULL;

    if (timeout) {
        long long q = 1000000000LL*(timeout->tv_sec) + timeout->tv_nsec;

        q = q * denom / num;

        ts.tv_sec = (q/1000000000);
        ts.tv_nsec = q%1000000000;
        tsptr = &ts;
    }

    int ret = orig_pselect(nfds, readfds, writefds, exceptfds, tsptr, sigmask);
    return ret;
}

