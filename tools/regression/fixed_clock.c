// LD_PRELOAD shim for regression renders: the wall clock (time(), gettimeofday(),
// clock_gettime(CLOCK_REALTIME*)) always reads LWE_FIXED_CLOCK (unix seconds). That covers
// the day/night uniform, text clocks, script Date objects and QuickJS's Math.random seed.
// Monotonic clocks are left alone so frame pacing and timeouts keep working.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

static time_t fixed_seconds (void) {
    static time_t value = -1;

    if (value == -1) {
	const char* env = getenv ("LWE_FIXED_CLOCK");
	value = env != NULL ? (time_t) strtoll (env, NULL, 10) : 1767268800; // 2026-01-01 12:00 UTC
    }

    return value;
}

time_t time (time_t* out) {
    const time_t now = fixed_seconds ();

    if (out != NULL) {
	*out = now;
    }

    return now;
}

int gettimeofday (struct timeval* tv, void* tz) {
    if (tv != NULL) {
	tv->tv_sec = fixed_seconds ();
	tv->tv_usec = 0;
    }

    return 0;
}

int clock_gettime (clockid_t clock, struct timespec* ts) {
    static int (*real) (clockid_t, struct timespec*) = NULL;

    if (clock == CLOCK_REALTIME || clock == CLOCK_REALTIME_COARSE) {
	ts->tv_sec = fixed_seconds ();
	ts->tv_nsec = 0;
	return 0;
    }

    if (real == NULL) {
	real = (int (*) (clockid_t, struct timespec*)) dlsym (RTLD_NEXT, "clock_gettime");
    }

    return real (clock, ts);
}
