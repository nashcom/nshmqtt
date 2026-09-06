// Alpine/musl compatibility shim -- NOT part of the regular src/ build
// (glibc already provides these symbols natively, so linking this into a
// normal Ubuntu/glibc build would fail with "multiple definition"; it's
// compiled and linked only by docker/Dockerfile).
//
// Reproduces the same issue nshgeoip's own docker/fortify_shim.cpp
// documents (see that file): on Alpine, g++/libstdc++ still emit calls to
// glibc-style _FORTIFY_SOURCE wrapper symbols (__printf_chk,
// __fprintf_chk, __snprintf_chk) at -O2, which musl's libc does not
// provide -- true for nshmqtt's own dynamic Alpine build here too, not
// only nshgeoip's static one. nshmqtt additionally hits two symbols
// nshgeoip's shim never needed, because nshmqtt actually uses the C++
// features that reference them (nshgeoip doesn't use std::shared_ptr or
// condition_variable::wait_for with a steady_clock deadline; mqtt.h does,
// for the publish-job queue and the MQTT worker's reconnect backoff):
//
//  - __libc_single_threaded: a fast-path flag libstdc++'s std::shared_ptr
//    (mqtt.cpp's Job queue) checks before deciding whether to skip atomic
//    refcount operations. This process is never actually single-threaded
//    once the worker threads exist, but even getting that detection wrong
//    is a correctness hazard (a skipped atomic op on a refcount shared
//    across threads is a real data race), so this shim does not attempt
//    to track it -- it is defined as a constant 0 ("not single-threaded"),
//    which always forces the safe atomic path. The only cost is a
//    micro-optimization libstdc++ would otherwise take; there is no
//    correctness downside to always saying no.
//  - pthread_cond_clockwait: what libstdc++ calls for
//    condition_variable::wait_for()/wait_until() against a
//    monotonic/steady_clock deadline (used throughout mqtt.cpp and
//    thread_pool.cpp), so it does not have to reinterpret a monotonic
//    duration against a condvar whose underlying pthread_cond_t is
//    CLOCK_REALTIME-based (the default, and what plain
//    std::condition_variable uses). This musl version doesn't export it,
//    so this shim reimplements it in terms of pthread_cond_timedwait(),
//    which musl does have: convert the caller's clockid-relative deadline
//    into an equivalent CLOCK_REALTIME deadline (by measuring "how much
//    time remains" on the caller's clock, then adding that remaining
//    duration to the current wall-clock time) and wait on that instead.
//    This has the same small imprecision every such clock-translation
//    shim has -- a wall-clock adjustment mid-wait could shift the actual
//    wake-up slightly -- which is immaterial here: every caller treats
//    this as a best-effort backoff/timeout, never a precise deadline.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <pthread.h>

extern "C"
{

int __printf_chk(int /*flag*/, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vprintf(format, ap);
    va_end(ap);
    return ret;
}

int __fprintf_chk(FILE *stream, int /*flag*/, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(stream, format, ap);
    va_end(ap);
    return ret;
}

int __snprintf_chk(char *s, size_t maxlen, int /*flag*/, size_t /*slen*/, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(s, maxlen, format, ap);
    va_end(ap);
    return ret;
}

long __isoc23_strtol(const char *nptr, char **endptr, int base)
{
    return strtol(nptr, endptr, base);
}

double __isoc23_strtod(const char *nptr, char **endptr)
{
    return strtod(nptr, endptr);
}

// See the file-level comment: always "not single-threaded", the safe
// (always-atomic) answer regardless of the process's actual thread count.
int __libc_single_threaded = 0;

int pthread_cond_clockwait(pthread_cond_t *cond, pthread_mutex_t *mutex, clockid_t clockid,
                           const struct timespec *abstime)
{
    struct timespec caller_now{};
    clock_gettime(clockid, &caller_now);

    long sec_remaining = abstime->tv_sec - caller_now.tv_sec;
    long nsec_remaining = abstime->tv_nsec - caller_now.tv_nsec;
    if (nsec_remaining < 0)
    {
        nsec_remaining += 1000000000L;
        sec_remaining -= 1;
    }
    if (sec_remaining < 0)
    {
        sec_remaining = 0;
        nsec_remaining = 0;
    }

    struct timespec real_deadline{};
    clock_gettime(CLOCK_REALTIME, &real_deadline);
    real_deadline.tv_sec += sec_remaining;
    real_deadline.tv_nsec += nsec_remaining;
    if (real_deadline.tv_nsec >= 1000000000L)
    {
        real_deadline.tv_nsec -= 1000000000L;
        real_deadline.tv_sec += 1;
    }

    return pthread_cond_timedwait(cond, mutex, &real_deadline);
}

} // extern "C"
