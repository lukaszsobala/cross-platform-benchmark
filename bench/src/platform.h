// What the host operating system can and cannot do, and shims for the gaps.
//
// The benchmark is libc + pthreads and nothing else, but "pthreads" is not one
// thing: barriers are an optional POSIX feature and macOS does not implement
// them, and CPU affinity is not in POSIX at all. Rather than sprinkle #ifdefs
// through the measurement code, everything platform-shaped lives here behind
// two capability macros and one shim.
//
//   PLAT_HAS_AFFINITY   a thread can be bound to a numbered CPU
//   PLAT_HAS_PROC       /proc and /sys are readable (Linux)
//
// A capability that is absent is reported as absent -- see the `pin` handling
// in bench.c. A benchmark that says it pinned threads on a system where
// pinning does not exist is worse than one that admits it did not.

#ifndef CPCPUB_PLATFORM_H
#define CPCPUB_PLATFORM_H

#include <pthread.h>

#if defined(__linux__)
#define PLAT_HAS_AFFINITY 1
#define PLAT_HAS_PROC     1
#include <sched.h>
#else
#define PLAT_HAS_AFFINITY 0
#define PLAT_HAS_PROC     0
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>        // _NSGetExecutablePath
#include <sys/sysctl.h>         // sysctlbyname
#include <sys/types.h>
#endif

// ---------------------------------------------------------------------------
// Barriers
// ---------------------------------------------------------------------------
//
// pthread_barrier_* is part of the optional _POSIX_BARRIERS option, and Apple's
// libpthread has never shipped it. The benchmark needs a barrier for one
// purpose -- releasing every worker into a timed phase at the same instant, so
// that a throughput number reflects N threads running together rather than
// stragglers -- so the shim only has to be correct, not fast: it is crossed
// once per phase, outside the timed region.
//
// A mutex, a condition variable and a generation counter. The generation is
// what makes it reusable: a thread that wakes must be able to tell "the barrier
// I was waiting on has opened" from "a later barrier has opened and I missed
// mine", and a plain counter cannot express that.
// CPCPUB_FORCE_BARRIER_SHIM builds the shim on a platform that does not need
// it, so the barrier can be exercised where there is hardware to exercise it
// on. Without that, this code would first run on the machine of whoever
// downloads the macOS binary.
#if defined(CPCPUB_FORCE_BARRIER_SHIM) || defined(__APPLE__) || \
    !defined(_POSIX_BARRIERS)

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    unsigned        limit;
    unsigned        waiting;
    unsigned        generation;
} plat_barrier_t;

#define PLAT_BARRIER_SERIAL_THREAD 1

static inline int plat_barrier_init(plat_barrier_t *b, unsigned count) {
    if (count == 0) return -1;
    b->limit = count;
    b->waiting = 0;
    b->generation = 0;
    if (pthread_mutex_init(&b->mu, NULL) != 0) return -1;
    if (pthread_cond_init(&b->cv, NULL) != 0) {
        pthread_mutex_destroy(&b->mu);
        return -1;
    }
    return 0;
}

static inline int plat_barrier_wait(plat_barrier_t *b) {
    pthread_mutex_lock(&b->mu);
    const unsigned mine = b->generation;
    if (++b->waiting == b->limit) {
        b->waiting = 0;
        b->generation++;
        pthread_cond_broadcast(&b->cv);
        pthread_mutex_unlock(&b->mu);
        return PLAT_BARRIER_SERIAL_THREAD;
    }
    // Looped, not a bare wait: a condition variable may wake spuriously, and
    // the generation is the predicate that says whether this thread's barrier
    // is the one that opened.
    while (mine == b->generation) pthread_cond_wait(&b->cv, &b->mu);
    pthread_mutex_unlock(&b->mu);
    return 0;
}

static inline int plat_barrier_destroy(plat_barrier_t *b) {
    pthread_cond_destroy(&b->cv);
    pthread_mutex_destroy(&b->mu);
    return 0;
}

#else   // the platform has real barriers; use them

typedef pthread_barrier_t plat_barrier_t;
#define PLAT_BARRIER_SERIAL_THREAD PTHREAD_BARRIER_SERIAL_THREAD

static inline int plat_barrier_init(plat_barrier_t *b, unsigned count) {
    return pthread_barrier_init(b, NULL, count);
}
static inline int plat_barrier_wait(plat_barrier_t *b) {
    return pthread_barrier_wait(b);
}
static inline int plat_barrier_destroy(plat_barrier_t *b) {
    return pthread_barrier_destroy(b);
}

#endif

// ---------------------------------------------------------------------------
// Pinning
// ---------------------------------------------------------------------------

// Bind the calling thread to `cpu`. Returns 0 if the binding took effect.
// Advisory everywhere: a cgroup or an affinity mask already in place can refuse
// it, which is why the caller reports the CPU it observed rather than the one
// it asked for.
static inline int plat_pin_self(int cpu) {
#if PLAT_HAS_AFFINITY
    if (cpu < 0) return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned int)cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0 ? 0 : -1;
#else
    (void)cpu;
    return -1;
#endif
}

// The CPU this thread is running on right now, or -1 where the system will not
// say. macOS deliberately does not expose this: the scheduler owns placement.
static inline int plat_current_cpu(void) {
#if PLAT_HAS_AFFINITY
    return sched_getcpu();
#else
    return -1;
#endif
}

#endif  // CPCPUB_PLATFORM_H
