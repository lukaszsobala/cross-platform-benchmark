// What the host operating system can and cannot do, and shims for the gaps.
//
// The benchmark is libc + pthreads and nothing else, but neither "libc" nor
// "pthreads" is one thing: barriers are an optional POSIX feature and macOS
// does not implement them, CPU affinity is not in POSIX at all, and Windows
// has no POSIX layer to begin with -- only a mingw-w64 runtime that supplies
// pthreads and leaves the rest to Win32. Rather than sprinkle #ifdefs through
// the measurement code, everything platform-shaped lives here behind two
// capability macros and a handful of small functions.
//
//   PLAT_HAS_AFFINITY   a thread can be bound to a numbered CPU
//   PLAT_HAS_PROC       /proc and /sys are readable (Linux)
//
//                      Linux   macOS   Windows
//   PLAT_HAS_AFFINITY    1       0        1     (SetThreadGroupAffinity)
//   PLAT_HAS_PROC        1       0        0
//   real pthread barrier 1       0      depends on the runtime; shimmed if not
//   load average         1       1        0     (Windows has no such number)
//
// A capability that is absent is reported as absent -- see the `pin` handling
// in bench.c. A benchmark that says it pinned threads on a system where
// pinning does not exist is worse than one that admits it did not.

#ifndef CPCPUB_PLATFORM_H
#define CPCPUB_PLATFORM_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>             // sysconf
#endif

#if defined(__linux__)
#define PLAT_HAS_AFFINITY 1
#define PLAT_HAS_PROC     1
#include <sched.h>
#include <sys/utsname.h>
#elif defined(_WIN32)
// Win32 has thread affinity, and unlike macOS it will say which CPU a thread
// is on, so the per-core sweep means on Windows what it means on Linux.
#define PLAT_HAS_AFFINITY 1
#define PLAT_HAS_PROC     0
#else
#define PLAT_HAS_AFFINITY 0
#define PLAT_HAS_PROC     0
#include <sys/utsname.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>        // _NSGetExecutablePath
#include <sys/sysctl.h>         // sysctlbyname
#include <sys/types.h>
#endif

#ifdef _WIN32
// Windows 7 is the floor that exposes the processor-group API used below
// (SetThreadGroupAffinity, GetActiveProcessorCount, GetCurrentProcessorNumberEx
// and GetLogicalProcessorInformationEx). Every Windows that runs on hardware
// worth benchmarking is far past it; this only stops the headers from hiding
// the declarations.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <malloc.h>             // _aligned_malloc

// Present since Windows 8's SDK; spelled out so an older mingw-w64 header set
// is a missing constant rather than a build that silently reports "unknown".
#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64 12
#endif
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
//
// The condition below picks the shim on any runtime that does not advertise
// barriers, which is how Windows is covered without naming it: winpthreads
// implements them and says so, mingw-w64 runtimes that do not simply fall to
// the shim.
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

#ifdef _WIN32
// Windows numbers logical CPUs *within processor groups* of at most 64, so a
// flat CPU index has to be split into (group, number) before Win32 will take
// it. On a machine with 64 or fewer logical CPUs -- which is nearly all of them
// -- there is one group and the split is the identity; above that, a benchmark
// that ignored groups would pin every thread past 63 to the wrong core and
// report a per-core sweep of a machine it never touched.
static inline int plat_win_split(int cpu, WORD *group, BYTE *number) {
    if (cpu < 0) return -1;
    const WORD groups = GetActiveProcessorGroupCount();
    for (WORD g = 0; g < groups; ++g) {
        const DWORD n = GetActiveProcessorCount(g);
        if ((DWORD)cpu < n) {
            *group = g;
            *number = (BYTE)cpu;
            return 0;
        }
        cpu -= (int)n;
    }
    return -1;
}
#endif

// Bind the calling thread to `cpu`. Returns 0 if the binding took effect.
// Advisory everywhere: a cgroup, a job object or an affinity mask already in
// place can refuse it, which is why the caller reports the CPU it observed
// rather than the one it asked for.
static inline int plat_pin_self(int cpu) {
#if defined(__linux__)
    if (cpu < 0) return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned int)cpu, &set);
    // sched_setaffinity(2) with a pid of 0, not pthread_setaffinity_np: on
    // Linux this call has always been per-thread and 0 means the calling
    // thread, so the two are the same system call -- but bionic does not
    // declare the pthread spelling, and Android is a Linux the benchmark
    // builds for.
    return sched_setaffinity(0, sizeof(set), &set) == 0 ? 0 : -1;
#elif defined(_WIN32)
    WORD group = 0;
    BYTE number = 0;
    if (plat_win_split(cpu, &group, &number) != 0) return -1;
    GROUP_AFFINITY ga;
    memset(&ga, 0, sizeof ga);
    ga.Group = group;
    ga.Mask  = (KAFFINITY)1 << number;
    return SetThreadGroupAffinity(GetCurrentThread(), &ga, NULL) ? 0 : -1;
#else
    (void)cpu;
    return -1;
#endif
}

// The CPU this thread is running on right now, or -1 where the system will not
// say. macOS deliberately does not expose this: the scheduler owns placement.
static inline int plat_current_cpu(void) {
#if defined(__linux__)
    return sched_getcpu();
#elif defined(_WIN32)
    // Flattened back into the same numbering plat_pin_self() accepts, so the
    // observed CPU can be compared against the requested one.
    PROCESSOR_NUMBER pn;
    memset(&pn, 0, sizeof pn);
    GetCurrentProcessorNumberEx(&pn);
    int base = 0;
    for (WORD g = 0; g < pn.Group; ++g) base += (int)GetActiveProcessorCount(g);
    return base + (int)pn.Number;
#else
    return -1;
#endif
}

// ---------------------------------------------------------------------------
// How many CPUs there are
// ---------------------------------------------------------------------------

static inline int plat_cpu_count(void) {
#ifdef _WIN32
    // ALL_PROCESSOR_GROUPS, so a machine with more than 64 logical CPUs counts
    // as the machine it is rather than as its first group.
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n < 1 ? 1 : (int)n;
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
#endif
}

// ---------------------------------------------------------------------------
// Load average
// ---------------------------------------------------------------------------

// The one-minute load average, or -1 where the system has no such notion.
// Windows genuinely does not: its scheduler keeps no run-queue-length average,
// and the counters that get suggested as substitutes ("% Processor Time") are
// a different measurement wearing the same name. Absent, therefore, rather
// than approximated -- the caller only uses it to warn.
static inline int plat_loadavg(double *out) {
#ifdef _WIN32
    (void)out;
    return -1;
#else
    return getloadavg(out, 1) == 1 ? 0 : -1;
#endif
}

// ---------------------------------------------------------------------------
// Page-aligned allocation
// ---------------------------------------------------------------------------
//
// Paired: memory from plat_aligned_alloc() must go back through
// plat_aligned_free(), because the Windows allocator behind it keeps its
// bookkeeping in front of the pointer it hands out and free(3) would not find
// it.

static inline void *plat_aligned_alloc(size_t align, size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, align);   // note: size first, then alignment
#else
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) return NULL;
    return p;
#endif
}

static inline void plat_aligned_free(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

// ---------------------------------------------------------------------------
// The running executable
// ---------------------------------------------------------------------------

// The path of the file this process is executing, not argv[0]: it names what
// the loader actually mapped, whatever the shell called it and wherever $PATH
// found it. Returns 0 on success. The path may contain symlinks or `..`, which
// does not matter to the one caller -- what it hashes is the contents that path
// opens to.
static inline int plat_executable_path(char *out, size_t outsz) {
    if (!out || outsz == 0) return -1;
    out[0] = '\0';
#if defined(_WIN32)
    const DWORD n = GetModuleFileNameA(NULL, out, (DWORD)outsz);
    // n == outsz means the name was truncated: GetModuleFileNameA reports a
    // full buffer without an error, so length is the only signal.
    if (n == 0 || (size_t)n >= outsz) { out[0] = '\0'; return -1; }
    return 0;
#elif defined(__APPLE__)
    // macOS has no /proc. _NSGetExecutablePath is the supported way to ask the
    // loader which file it mapped.
    uint32_t sz = (uint32_t)outsz;
    if (_NSGetExecutablePath(out, &sz) != 0) { out[0] = '\0'; return -1; }
    return 0;
#elif PLAT_HAS_PROC
    if (outsz < sizeof "/proc/self/exe") return -1;
    memcpy(out, "/proc/self/exe", sizeof "/proc/self/exe");
    return 0;
#else
    return -1;
#endif
}

// ---------------------------------------------------------------------------
// Naming the system
// ---------------------------------------------------------------------------

// uname(2) where there is one, and the Win32 equivalents where there is not.
// The fields carry the same meanings as `uname -s`, `-r` and `-m`, including
// the fact that each system names its own hardware: the same silicon reads
// "aarch64" on Linux, "arm64" on macOS and "ARM64" on Windows.
typedef struct {
    char sysname[96];
    char release[96];
    char machine[96];
} plat_sysinfo_t;

static inline int plat_sysinfo(plat_sysinfo_t *si) {
#ifdef _WIN32
    snprintf(si->sysname, sizeof si->sysname, "%s", "Windows");

    // GetVersionEx reports 6.2 on everything newer unless the binary carries a
    // compatibility manifest naming the version it was tested against -- a shim
    // for old software, and a silent misreport here. RtlGetVersion is the call
    // that shim sits in front of, and it answers honestly. Resolved at runtime
    // because it is not in any import library.
    typedef LONG(WINAPI * rtl_get_version_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW vi;
    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    // Through void *: casting between function pointer types directly is what
    // -Wcast-function-type exists to complain about, and GetProcAddress has no
    // other way to hand back a typed pointer.
    rtl_get_version_t rtl =
        ntdll ? (rtl_get_version_t)(void *)GetProcAddress(ntdll, "RtlGetVersion")
              : NULL;
    if (rtl && rtl(&vi) == 0) {
        snprintf(si->release, sizeof si->release, "%lu.%lu.%lu",
                 vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    } else {
        snprintf(si->release, sizeof si->release, "%s", "unknown");
    }

    // Native, not the process's view: an x86_64 build running under emulation
    // on an Arm machine then reports machine=ARM64 against target=x86_64, which
    // is exactly the discrepancy a reader of those numbers needs to see.
    SYSTEM_INFO sys;
    memset(&sys, 0, sizeof sys);
    GetNativeSystemInfo(&sys);
    const char *m;
    switch (sys.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: m = "AMD64";   break;
        case PROCESSOR_ARCHITECTURE_ARM64: m = "ARM64";   break;
        case PROCESSOR_ARCHITECTURE_INTEL: m = "x86";     break;
        default:                           m = "unknown"; break;
    }
    snprintf(si->machine, sizeof si->machine, "%s", m);
    return 0;
#else
    struct utsname u;
    if (uname(&u) != 0) return -1;
    snprintf(si->sysname, sizeof si->sysname, "%s", u.sysname);
    snprintf(si->release, sizeof si->release, "%s", u.release);
    snprintf(si->machine, sizeof si->machine, "%s", u.machine);
    return 0;
#endif
}

#endif  // CPCPUB_PLATFORM_H
