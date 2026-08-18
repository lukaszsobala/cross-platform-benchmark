// Portable CPU benchmark: integer, FP, memory and indirect-dispatch kernels for
// 64-bit x86_64 / aarch64 / riscv64 / loongarch64, on Linux, macOS and Windows.
// libc and pthreads only, no intrinsics; on Windows that means a mingw-w64
// runtime, not a POSIX layer. What differs between the systems is behind the
// capability macros and shims in platform.h.
//
// This file is the driver: it selects CPUs, times batches, aggregates and
// reports. The kernels it times live in kernels.c, which is compiled once per
// build variant (scalar/vector x FMA off/on) because both toggles are
// translation-unit flags. See kernels.h for that interface.

#define _GNU_SOURCE 1
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>

// <sys/utsname.h> used to be listed above and is now included from here: there
// is no such header on Windows, and what stands in for it is <windows.h>, which
// platform.h pulls in with the version guards it needs.
#include "platform.h"
#include "kernels.h"
#include "sha256.h"
#include "http.h"

#ifndef ARRAY_LEN
#define ARRAY_LEN(x) ((int)(sizeof(x) / sizeof((x)[0])))
#endif

// Where `--submit` sends a result when it is given no URL of its own. Empty in
// this tree -- there is no public hub yet -- so a build can be pointed at one
// with `make HUB_URL=https://hub.example` and its users then need no address
// at all. See the uploading section below.
#ifndef CPCPUB_HUB_URL
#define CPCPUB_HUB_URL ""
#endif

// What `--version` reports. "dev" unless the build stamped one in -- see
// VERSION in bench/Makefile -- so an unlabelled binary says so rather than
// claiming to be a release it is not.
// A clock the operator supplied with --mhz, in MHz, or 0. See the flag's own
// note in usage(): it exists for the machines that will not say.
static double g_mhz_given = 0.0;

#ifndef CPCPUB_VERSION
#define CPCPUB_VERSION "dev"
#endif

// ---------------------------------------------------------------------------
// Output mode
// ---------------------------------------------------------------------------
//
// json/tsv put the results on stdout and everything else (platform banner,
// warnings, legend) on stderr, so the result stream stays machine-readable.

typedef enum { FMT_TEXT, FMT_JSON, FMT_TSV } out_fmt_t;

static out_fmt_t g_format  = FMT_TEXT;
static int       g_verbose = 0;

// Where human-readable prose goes.
static FILE *msg(void) { return g_format == FMT_TEXT ? stdout : stderr; }

// ---------------------------------------------------------------------------
// The JSON sink
// ---------------------------------------------------------------------------
//
// The result document normally goes straight to stdout. --submit needs the
// same bytes in memory instead -- they are what gets POSTed, and a text-mode
// run has to produce them without printing them. So the JSON emitters write
// through jout(), which is stdout until a buffer is put in front of it.

typedef struct {
    char  *p;
    size_t len, cap;
    int    oom;             // an allocation failed; the document is incomplete
} sbuf_t;

static sbuf_t *g_json_sink = NULL;

static void sbuf_free(sbuf_t *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void jout(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!g_json_sink) {
        vprintf(fmt, ap);
        va_end(ap);
        return;
    }
    sbuf_t *b = g_json_sink;
    for (int attempt = 0; attempt < 2 && !b->oom; ++attempt) {
        va_list copy;
        va_copy(copy, ap);
        const size_t room = b->cap - b->len;
        // NULL with a size of zero is what vsnprintf is defined to accept, and
        // is what the first pass on an empty buffer hands it -- b->p + 0 on a
        // null pointer would not be.
        char *at = b->p ? b->p + b->len : NULL;
        const int n = vsnprintf(at, room, fmt, copy);
        va_end(copy);
        if (n < 0) { b->oom = 1; break; }
        if ((size_t)n < room) { b->len += (size_t)n; break; }
        // It did not fit: grow to hold it and write it again. vsnprintf said
        // exactly how much it wanted, so the second pass always fits.
        size_t want = b->cap ? b->cap : 8192;
        while (want < b->len + (size_t)n + 1) want *= 2;
        char *bigger = (char *)realloc(b->p, want);
        if (!bigger) { b->oom = 1; break; }
        b->p = bigger;
        b->cap = want;
    }
    va_end(ap);
}

static void jputc(char c) {
    if (g_json_sink) jout("%c", c);
    else             putchar(c);
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

static clockid_t g_clock_id = CLOCK_MONOTONIC;
// What --clock actually got, which is not always what it asked for. Reported
// in place of the request: a result that says it used the raw clock on a system
// with no raw clock is describing a run that did not happen.
static int g_clock_raw = 0;

static void set_clock_mode(int use_raw) {
#ifdef CLOCK_MONOTONIC_RAW
    g_clock_raw = use_raw ? 1 : 0;
    g_clock_id = use_raw ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC;
#else
    // No unadjusted monotonic clock here -- mingw-w64's clock_gettime offers
    // CLOCK_MONOTONIC and nothing below it -- so --clock raw quietly gets the
    // NTP-disciplined one. Quietly in the sense that nothing fails: the result
    // document says "mono", because that is what was used.
    (void)use_raw;
    g_clock_raw = 0;
    g_clock_id = CLOCK_MONOTONIC;
#endif
}

static inline double now_sec(void) {
    struct timespec ts;
    clock_gettime(g_clock_id, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Xorshift64* PRNG, used only for setup (never inside a timed loop).
static inline uint64_t xs64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * UINT64_C(2685821657736338717);
}

// ---------------------------------------------------------------------------
// Build variants
// ---------------------------------------------------------------------------
//
// Auto-vectorization and FMA contraction cannot be switched per function, so
// the binary carries four compilations of kernels.c and picks one at runtime.
// The order is the order --variants runs them: the cross-ISA fairness baseline
// first, then one toggle at a time, then both.
//
// `distinct` is whether the toggles can change code generation for the -march
// this binary was built for (see TARGET_HAS_SIMD / TARGET_HAS_FMA). A variant
// that is not distinct is still present and still runnable -- it is simply the
// same machine code as the baseline, so a default --variants run skips it
// rather than measuring one build twice.
typedef struct {
    const kernel_set_t *k;
    int distinct;
} variant_t;

static const variant_t g_variants[] = {
    { &kernels_scalar_nofma, 1 },
    { &kernels_vector_nofma, TARGET_HAS_SIMD },
    { &kernels_scalar_fma,   TARGET_HAS_FMA },
    { &kernels_vector_fma,   TARGET_HAS_SIMD && TARGET_HAS_FMA },
};
#define N_VARIANTS ARRAY_LEN(g_variants)
// The fairness default: scalar FP, no auto-vectorization, no FMA contraction.
#define VARIANT_DEFAULT 0

// ---------------------------------------------------------------------------
// Indirect-dispatch ladders
// ---------------------------------------------------------------------------

// Selector-sequence periods swept by --disp-sweep, in calls.
//
// A uniformly random target sequence is unpredictable for every predictor ever
// built, so a "random" dispatch loop measures the *cost of a mispredict*, which
// is pipeline-depth-dominated and ranks cores by how shallow they are. What
// separates a big front end from a little one is predictor capacity: the length
// of deterministic pattern it can still learn. So the selector stream is a fixed
// random pattern of period L, repeated, and L is swept. Small L: everyone learns
// it. Huge L: nobody does. The knee in between reads out the predictor. The last
// entry is the unpredictable floor.
static const size_t g_disp_periods[] = {
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 65536
};
#define DISP_SWEEP_N   ARRAY_LEN(g_disp_periods)
// Backing array is sized for the largest period; smaller periods use a prefix.
#define DISP_IDX_BYTES ((size_t)65536)

// The ladder the suite itself walks: a single-target reference, a x2 geometric
// span, and the unpredictable floor.
//
// Period 1 calls the same function every time, which any core with a BTB
// predicts. It is not part of the span; it exists to tell "predicts nothing
// beyond a single target" apart from "predicts everything the ladder can throw
// at it", which look identical from the span alone. The floor must be the
// genuinely random point: a floor taken from the end of the ladder is still
// partly predicted on some cores, and a floor set too high compresses every q
// beneath it.
static const size_t g_disp_ladder[] = {
    1,                                                          // reference
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, // the span
    65536                                                       // floor
};
#define DISP_LADDER_N     ARRAY_LEN(g_disp_ladder)
#define DISP_LADDER_BYTES ((size_t)65536)
#define DISP_REF_IDX      0
#define DISP_SPAN_FIRST   1
#define DISP_SPAN_LAST    (DISP_LADDER_N - 2)
// Span points establishing the "everyone predicts this" plateau, starting at
// DISP_SPAN_FIRST. The best is taken, not the mean: the plateau is the reference
// the whole curve is normalised against, so one point depressed by interference
// would inflate every q beneath it.
#define DISP_PLATEAU_PTS  3
// The capacity read-out integrates the curve rather than crossing a threshold
// (a threshold lands on the knee, where noise moves it between ladder steps). At
// each ladder point the rate is normalised to
//   q = (rate - floor) / (plateau - floor)      clipped to [0,1]
// i.e. how much of the available prediction win the core is still getting.
// Summing q gives the width of the predicted region in log2(period) units, so
//   capacity = first_period * 2^(sum(q) - 1)
// which returns exactly L for an ideal core that predicts everything up to L and
// nothing beyond. Dividing the floor out is what stops a shallow pipeline from
// scoring as a good predictor: the floor *is* the mispredict penalty.
//
// Below this plateau/floor ratio the curve is flat and there is no span to
// integrate -- the core has no usable indirect prediction.
#define DISP_MIN_GAIN     1.15

// ---------------------------------------------------------------------------
// Timed driver
// ---------------------------------------------------------------------------
//
// Runs the kernel in batches sized to land near TARGET_BATCH_SEC, which keeps
// clock_gettime out of the measured work while bounding the overshoot past
// `seconds`. Iteration count and elapsed time both include the final batch, so
// the reported rate is exact regardless of overshoot.
#define TARGET_BATCH_SEC 0.004
#define MAX_BATCH_SEC    0.020

static uint64_t run_timed(kernel_fn fn, void *ctx, double seconds,
                          uint64_t batch_init, double *elapsed_out) {
    const double t0 = now_sec();
    const double t_end = t0 + seconds;
    uint64_t batch = batch_init ? batch_init : 1;
    uint64_t iters = 0;
    double t = t0;

    while (t < t_end) {
        fn(ctx, batch);
        iters += batch;
        const double t2 = now_sec();
        const double dt = t2 - t;
        if (dt < TARGET_BATCH_SEC && batch < (UINT64_C(1) << 30)) {
            batch *= 2;
        } else if (dt > MAX_BATCH_SEC && batch > 1) {
            batch /= 2;
        }
        t = t2;
    }

    double elapsed = t - t0;
    if (elapsed <= 0.0) elapsed = seconds;
    *elapsed_out = elapsed;
    return iters;
}

// Volatile so the compiler cannot delete a warm-up loop as dead code.
static volatile uint64_t g_warmup_sink;

// Wake the core from idle and ramp DVFS before measuring. Warms up with the
// variant about to be measured, so the caches hold the code that is next to run.

static void warmup_spin(const kernel_set_t *k, double sec) {
    if (sec <= 0.0) return;
    int_ctx_t c;
    for (int i = 0; i < LANES; ++i) c.l[i] = (uint64_t)i + 1u;
    c.k[0] = UINT64_C(0x9E3779B97F4A7C15); c.k[1] = UINT64_C(0xBF58476D1CE4E5B9);
    const double t_end = now_sec() + sec;
    while (now_sec() < t_end) {
        k->int_thr(&c, 20000);
    }
    for (int i = 0; i < LANES; ++i) g_warmup_sink ^= c.l[i];
}

// ---------------------------------------------------------------------------
// Per-CPU information
// ---------------------------------------------------------------------------

static double read_cpu_khz(int cpu, const char *what) {
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, what);
    FILE *f = fopen(path, "r");
    if (!f) return 0.0;
    double khz = 0.0;
    if (fscanf(f, "%lf", &khz) != 1) khz = 0.0;
    fclose(f);
    return khz;
}

static double cpu_cur_mhz(int cpu) {
    double khz = read_cpu_khz(cpu, "scaling_cur_freq");
    return khz > 0.0 ? khz / 1000.0 : 0.0;
}

static double cpu_max_mhz(int cpu) {
    double khz = read_cpu_khz(cpu, "cpuinfo_max_freq");
    if (khz <= 0.0) khz = read_cpu_khz(cpu, "scaling_max_freq");
    return khz > 0.0 ? khz / 1000.0 : 0.0;
}

// A device-tree integer property: 4 or 8 bytes, big-endian, which is how the
// flattened tree stores them whatever the machine's own byte order is. Zero
// for a missing file or any other length, so a caller reads "no answer" rather
// than a number assembled out of something else. (The other property this file
// reads, the board's `model` string, is in the CPU-naming section below.)
static double dt_be_int(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0.0;
    unsigned char b[8];
    const size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (n != 4 && n != 8) return 0.0;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | b[i];
    return (double)v;
}

// The clock a CPU's device-tree node declares, in MHz.
//
// This is what answers for a board that runs at one fixed frequency and so
// ships no cpufreq driver at all -- most RISC-V SBCs, and the smaller Arm ones.
// Without it such a machine has no clock to report but the one inferred from
// INT-lat, and that inference assumes the core retires one dependent op per
// cycle: a wide out-of-order core does, an in-order one need not, and where it
// does not the reported clock is wrong by exactly the IPC it actually got.
//
// A declared clock is not a measured one -- it is what the tree was written to
// say -- but neither is `cpuinfo_max_freq`, and it beats an assumption about
// the microarchitecture. `cpu@N` is tried for the logical index first, then the
// first `cpu@` node in the tree: those agree on the uniform machines this is
// the fallback for, and a machine with clusters that differ generally has the
// cpufreq driver that would have answered above.
static double dt_cpu_mhz(int cpu) {
    char path[256];
    if (cpu >= 0) {
        snprintf(path, sizeof(path),
                 "/proc/device-tree/cpus/cpu@%d/clock-frequency", cpu);
        const double hz = dt_be_int(path);
        if (hz > 0.0) return hz / 1e6;
    }
    DIR *d = opendir("/proc/device-tree/cpus");
    if (!d) return 0.0;
    double mhz = 0.0;
    const struct dirent *e;
    while (mhz <= 0.0 && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "cpu@", 4) != 0) continue;
        // Bounded: d_name is up to NAME_MAX and `path` is not, and a node
        // name that long is not one of these anyway.
        snprintf(path, sizeof(path),
                 "/proc/device-tree/cpus/%.128s/clock-frequency", e->d_name);
        const double hz = dt_be_int(path);
        if (hz > 0.0) mhz = hz / 1e6;
    }
    closedir(d);
    return mhz;
}

// ---------------------------------------------------------------------------
// DRAM controller frequency (Linux devfreq)
// ---------------------------------------------------------------------------
//
// Some SoCs scale the memory controller clock independently of the CPU, over a
// range wide enough to dominate every memory measurement. The memory phases warm
// up with real traffic to force a ramp, but the governor may still not reach its
// top step, so the observed clock is reported rather than left as variance.
static char g_dram_freq_path[288];
static char g_dram_name[64];

static void find_dram_devfreq(void) {
    g_dram_freq_path[0] = '\0';
    g_dram_name[0] = '\0';
    DIR *d = opendir("/sys/class/devfreq");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char npath[288];
        snprintf(npath, sizeof(npath), "/sys/class/devfreq/%s/name", e->d_name);
        char name[64] = {0};
        FILE *f = fopen(npath, "r");
        if (f) {
            if (!fgets(name, (int)sizeof(name), f)) name[0] = '\0';
            fclose(f);
        }
        // Fall back to the directory name when there is no name attribute.
        const char *id = name[0] ? name : e->d_name;
        if (strstr(id, "dmc") || strstr(id, "ddr") || strstr(id, "mem")) {
            snprintf(g_dram_freq_path, sizeof(g_dram_freq_path),
                     "/sys/class/devfreq/%s/cur_freq", e->d_name);
            size_t len = strlen(id);
            while (len > 0 && (id[len - 1] == '\n' || id[len - 1] == '\r')) len--;
            snprintf(g_dram_name, sizeof(g_dram_name), "%.*s", (int)len, id);
            break;
        }
    }
    closedir(d);
}

static double dram_mhz(void) {
    if (!g_dram_freq_path[0]) return 0.0;
    FILE *f = fopen(g_dram_freq_path, "r");
    if (!f) return 0.0;
    double hz = 0.0;
    if (fscanf(f, "%lf", &hz) != 1) hz = 0.0;
    fclose(f);
    return hz > 0.0 ? hz / 1e6 : 0.0;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

typedef struct {
    int    id;
    int    cpu;            // CPU to pin to, or -1 for unpinned
    size_t mem_bytes;      // per-thread buffer for the memory phases (0 = skip)
    double seconds;        // measured time per phase
    double warmup;         // warm-up seconds before each phase
    int    reps;           // repetitions per phase; the best one is kept
    plat_barrier_t *bar;
    uint64_t seed;
    const kernel_set_t *ks;   // the build variant this run measures

    volatile uint64_t checksum;

    // results
    double int_lat_mops;
    double int_thr_mops;
    double mul_thr_mops;   // integer 64-bit multiplies per second
    double fp_lat_mflops;
    double fp_thr_mflops;
    double mem_gbps;
    double mem_lat_ns;     // one dependent chain
    double mem_mlp_ns;     // CHASE_WAYS chains in flight, per access
    double disp_ladder[DISP_LADDER_N];  // Mcall/s at each selector period
    int    disp_sweep;     // run only the selector-period sweep, not the suite
    double sweep_serial[DISP_SWEEP_N];  // Mcall/s per period, dependent chain
    double sweep_free[DISP_SWEEP_N];    // Mcall/s per period, independent calls
    double mhz_observed;   // sampled while the core was under load
    double dram_mhz_observed; // DRAM controller clock during the memory phases
    int    cpu_observed;   // where it actually ran, -1 where unknowable
} worker_t;

static void wsync(worker_t *w) {
    if (w->bar) plat_barrier_wait(w->bar);
}

// Enter one repetition of a phase: line all threads up, warm up (first rep
// only), line up again so every thread starts measuring at the same instant.
// Every thread must call this the same number of times or the barrier deadlocks.
static void phase_begin(worker_t *w, int rep) {
    wsync(w);
    if (rep == 0) warmup_spin(w->ks, w->warmup);
    wsync(w);
}

// Warm-up for the memory phases. A pure-ALU spin does not ramp a DRAM
// controller whose governor scales the DDR clock, so the memory phases would
// start measuring at whatever idle clock it was parked at.
//
// `readonly` must be set once a pointer chase has been built into the buffer:
// the read+write sweep would overwrite the chase links and the next chase would
// dereference a wild pointer. A read-only sweep ramps the governor just as well.
NOINLINE static void mem_warmup(const kernel_set_t *k, void *buf, size_t nbytes,
                                double sec, int readonly) {
    if (!buf || nbytes < 4096 || sec <= 0.0) return;
    const double t_end = now_sec() + sec;
    if (readonly) {
        const uint64_t *p = (const uint64_t *)buf;
        const size_t nq = nbytes / sizeof(uint64_t);
        uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        while (now_sec() < t_end) {
            for (size_t i = 0; i + 3 < nq; i += 4) {
                a0 += p[i]; a1 += p[i + 1]; a2 += p[i + 2]; a3 += p[i + 3];
            }
        }
        g_warmup_sink ^= a0 ^ a1 ^ a2 ^ a3;
    } else {
        mem_ctx_t m = { .buf = (uint64_t *)buf,
                        .nqwords = nbytes / sizeof(uint64_t),
                        .acc = {0, 0, 0, 0} };
        while (now_sec() < t_end) {
            k->mem_bw(&m, 1);
        }
        for (int i = 0; i < 4; ++i) g_warmup_sink ^= m.acc[i];
    }
}

// Same contract as phase_begin, but ramps the memory subsystem.
static void phase_begin_mem(worker_t *w, int rep, void *buf, size_t nbytes,
                            int readonly) {
    wsync(w);
    if (rep == 0) mem_warmup(w->ks, buf, nbytes, w->warmup, readonly);
    wsync(w);
}

// Repeated measurements keep the *best* result, not the mean: interference can
// only ever make a run slower, so the fastest run is closest to the machine's
// true capability and averaging would fold the noise in.
static inline void keep_best_rate(double *best, double candidate) {
    if (candidate > *best) *best = candidate;
}
static inline void keep_best_lat(double *best, double candidate) {
    if (candidate > 0.0 && (*best <= 0.0 || candidate < *best)) *best = candidate;
}

// Build `ways` independent random pointer-chase cycles over `buf`, one node per
// `stride` bytes.
//
// Ways are *interleaved* (way w owns nodes w, w+ways, ...), not given contiguous
// slices, so the latency phase (ways=1) and the MLP phase (ways=CHASE_WAYS)
// present the same cache footprint; otherwise the one-chain measurement could
// fit in last-level cache and inflate the apparent MLP. Each way is one full
// cycle over its nodes, so a chase can never collapse into a short cached loop.
static int build_chases(void *buf, size_t nbytes, size_t stride, int ways,
                        uint64_t seed, void **heads_out) {
    const size_t nodes = nbytes / stride;
    const size_t per_way = nodes / (size_t)ways;
    if (per_way < 2) return -1;

    uint32_t *order = (uint32_t *)malloc(per_way * sizeof(uint32_t));
    if (!order) return -1;

    uint8_t *base = (uint8_t *)buf;
    uint64_t s = seed ? seed : UINT64_C(0x243F6A8885A308D3);
    for (int wi = 0; wi < ways; ++wi) {
        for (size_t i = 0; i < per_way; ++i) {
            order[i] = (uint32_t)(i * (size_t)ways + (size_t)wi);
        }
        for (size_t i = per_way - 1; i > 0; --i) {
            const size_t j = (size_t)(xs64(&s) % (uint64_t)(i + 1));
            const uint32_t tmp = order[i];
            order[i] = order[j];
            order[j] = tmp;
        }
        for (size_t i = 0; i < per_way; ++i) {
            void **node = (void **)(base + (size_t)order[i] * stride);
            *node = (void *)(base + (size_t)order[(i + 1) % per_way] * stride);
        }
        heads_out[wi] = (void *)(base + (size_t)order[0] * stride);
    }
    free(order);
    return 0;
}

static void *worker(void *arg) {
    worker_t *w = (worker_t *)arg;
    const kernel_set_t *ks = w->ks;

    (void)plat_pin_self(w->cpu);
    w->cpu_observed = plat_current_cpu();

    uint8_t *buf = NULL;
    const size_t nbytes = w->mem_bytes;
    if (nbytes > 0) {
        buf = (uint8_t *)plat_aligned_alloc(4096, nbytes);
        if (!buf) {
            perror("aligned alloc");
        } else {
            memset(buf, 1, nbytes);   // allocate and fault in every page
        }
    }

    uint64_t seed = w->seed;
    double elapsed;

    const int reps = w->reps > 0 ? w->reps : 1;

    // ---- selector-period sweep (diagnostic mode, --disp-sweep) ----
    // Runs the dispatch kernel at every period in g_disp_periods and nothing
    // else: the raw curve the DISPcap number is derived from.
    if (w->disp_sweep) {
        // INT-lat is one dependent 1-cycle op per cycle, so it doubles as a
        // clock reading on targets with no cpufreq node -- see --mhz for what
        // to do where the core does not manage one op per cycle.
        for (int rep = 0; rep < reps; ++rep) {
            int_ctx_t c;
            for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
            for (int i = 0; i < 2; ++i) c.k[i] = xs64(&seed) | 1u;
            phase_begin(w, rep);
            const uint64_t it = run_timed(ks->int_lat, &c, w->seconds, 1024, &elapsed);
            keep_best_rate(&w->int_lat_mops,
                           (double)it * (double)(INT_OPS_PER_STEP * LANES)
                           / 1e6 / elapsed);
            for (int i = 0; i < LANES; ++i) w->checksum ^= c.l[i];
            if (w->cpu_observed >= 0) {
                const double mhz = cpu_cur_mhz(w->cpu_observed);
                if (mhz > w->mhz_observed) w->mhz_observed = mhz;
            }
        }

        uint8_t *idx = (uint8_t *)malloc(DISP_IDX_BYTES);
        if (idx) for (size_t i = 0; i < DISP_IDX_BYTES; ++i) idx[i] = (uint8_t)xs64(&seed);
        // The barrier count must not depend on the allocation succeeding.
        for (int k = 0; k < DISP_SWEEP_N; ++k) {
            for (int rep = 0; rep < reps; ++rep) {
                phase_begin(w, rep);
                if (!idx) continue;
                disp_ctx_t d = { .idx = idx, .mask = g_disp_periods[k] - 1,
                                 .pos = 0, .acc = 1 };
                const uint64_t calls = run_timed(ks->disp, &d, w->seconds, 1024, &elapsed);
                keep_best_rate(&w->sweep_serial[k], (double)calls / 1e6 / elapsed);
                w->checksum ^= d.acc;
            }
            for (int rep = 0; rep < reps; ++rep) {
                phase_begin(w, rep);
                if (!idx) continue;
                disp_ctx_t d = { .idx = idx, .mask = g_disp_periods[k] - 1,
                                 .pos = 0, .acc = 1 };
                const uint64_t calls = run_timed(ks->disp_free, &d, w->seconds, 1024, &elapsed);
                keep_best_rate(&w->sweep_free[k], (double)calls / 1e6 / elapsed);
                w->checksum ^= d.acc;
            }
        }
        free(idx);
        free(buf);
        return NULL;
    }

    // ---- integer latency: one dependency chain ----
    for (int rep = 0; rep < reps; ++rep) {
        int_ctx_t c;
        for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
        for (int i = 0; i < 2; ++i) c.k[i] = xs64(&seed) | 1u;
        phase_begin(w, rep);
        const uint64_t it = run_timed(ks->int_lat, &c, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->int_lat_mops,
                       (double)it * (double)(INT_OPS_PER_STEP * LANES)
                       / 1e6 / elapsed);
        for (int i = 0; i < LANES; ++i) w->checksum ^= c.l[i];
    }

    // ---- integer throughput: LANES independent chains ----
    for (int rep = 0; rep < reps; ++rep) {
        int_ctx_t c;
        for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
        for (int i = 0; i < 2; ++i) c.k[i] = xs64(&seed) | 1u;
        phase_begin(w, rep);
        const uint64_t it = run_timed(ks->int_thr, &c, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->int_thr_mops,
                       (double)it * (double)(INT_OPS_PER_STEP * LANES) / 1e6 / elapsed);
        for (int i = 0; i < LANES; ++i) w->checksum ^= c.l[i];
        // Sample the clock while this core is still fully loaded.
        if (w->cpu_observed >= 0) {
            const double mhz = cpu_cur_mhz(w->cpu_observed);
            if (mhz > w->mhz_observed) w->mhz_observed = mhz;
        }
    }

    // ---- integer multiply throughput: LANES independent multiply chains ----
    for (int rep = 0; rep < reps; ++rep) {
        mul_ctx_t c;
        for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
        c.m = xs64(&seed) | 1u;
        phase_begin(w, rep);
        const uint64_t it = run_timed(ks->mul_thr, &c, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->mul_thr_mops,
                       (double)it * (double)(MUL_OPS_PER_STEP * LANES) / 1e6 / elapsed);
        for (int i = 0; i < LANES; ++i) w->checksum ^= c.l[i];
    }

    // Converges to the fixed point b/(1-c) and stays there: bounded forever.
    const double co = 0.5;

    // ---- FP latency ----
    for (int rep = 0; rep < reps; ++rep) {
        fp_ctx_t f;
        f.c = co;
        for (int i = 0; i < LANES; ++i) { f.x[i] = 1.0 + (double)i; f.b[i] = 1.0 + 0.25 * (double)i; }
        phase_begin(w, rep);
        const uint64_t it = run_timed(ks->fp_lat, &f, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->fp_lat_mflops,
                       (double)it * (double)FP_OPS_PER_STEP / 1e6 / elapsed);
        w->checksum ^= (uint64_t)(int64_t)(f.x[0] * 1e6);
    }

    // ---- FP throughput ----
    for (int rep = 0; rep < reps; ++rep) {
        fp_ctx_t f;
        f.c = co;
        for (int i = 0; i < LANES; ++i) { f.x[i] = 1.0 + (double)i; f.b[i] = 1.0 + 0.25 * (double)i; }
        phase_begin(w, rep);
        const uint64_t it = run_timed(ks->fp_thr, &f, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->fp_thr_mflops,
                       (double)it * (double)(FP_OPS_PER_STEP * LANES) / 1e6 / elapsed);
        for (int i = 0; i < LANES; ++i) {
            w->checksum ^= (uint64_t)(int64_t)(f.x[i] * 1e6);
        }
    }

    // ---- indirect dispatch: one point per selector period ----
    // Each ladder point gets half a phase's time; less than that widens
    // run-to-run spread noticeably on small cores, more buys little.
    {
        uint8_t *idx = (uint8_t *)malloc(DISP_LADDER_BYTES);
        if (idx) {
            for (size_t i = 0; i < DISP_LADDER_BYTES; ++i) idx[i] = (uint8_t)xs64(&seed);
        }
        const double t_pt = w->seconds / 2.0;
        for (int k = 0; k < DISP_LADDER_N; ++k) {
            for (int rep = 0; rep < reps; ++rep) {
                // Only the first point pays for a warm-up; the barrier is still
                // entered once per point per rep by every thread.
                phase_begin(w, k == 0 ? rep : 1);
                if (!idx) continue;
                disp_ctx_t d = { .idx = idx, .mask = g_disp_ladder[k] - 1,
                                 .pos = 0, .acc = 1 };
                const uint64_t calls = run_timed(ks->disp, &d, t_pt, 1024, &elapsed);
                keep_best_rate(&w->disp_ladder[k], (double)calls / 1e6 / elapsed);
                w->checksum ^= d.acc;
            }
        }
        free(idx);
    }

    const int mem_ok = (buf != NULL && nbytes >= (size_t)CHASE_WAYS * 65536);

    // ---- memory bandwidth: sequential read+write sweep ----
    for (int rep = 0; rep < reps; ++rep) {
        phase_begin_mem(w, rep, buf, nbytes, 0);
        if (!mem_ok) continue;
        mem_ctx_t m = { .buf = (uint64_t *)buf,
                        .nqwords = nbytes / sizeof(uint64_t),
                        .acc = {0, 0, 0, 0} };
        const uint64_t passes = run_timed(ks->mem_bw, &m, w->seconds, 1, &elapsed);
        const double bytes = (double)passes * (double)nbytes * 2.0; // read + write
        keep_best_rate(&w->mem_gbps, bytes / 1e9 / elapsed);
        const double dm = dram_mhz();
        if (dm > w->dram_mhz_observed) w->dram_mhz_observed = dm;
        for (int i = 0; i < 4; ++i) w->checksum ^= m.acc[i];
    }

    // ---- memory latency: one dependent chase over the whole buffer ----
    void *heads[CHASE_WAYS];
    int chase_ok = mem_ok && build_chases(buf, nbytes, 64, 1,
                                          w->seed ^ UINT64_C(0xA5A5A5A5), heads) == 0;
    for (int rep = 0; rep < reps; ++rep) {
        phase_begin_mem(w, rep, buf, nbytes, 1);
        if (!chase_ok) continue;
        chase_ctx_t ch;
        ch.cur[0] = (void **)heads[0];
        const uint64_t hops = run_timed(ks->mem_lat, &ch, w->seconds, 4096, &elapsed);
        keep_best_lat(&w->mem_lat_ns, elapsed / (double)hops * 1e9);
        w->checksum ^= (uint64_t)(uintptr_t)ch.cur[0];
    }

    // ---- memory-level parallelism: CHASE_WAYS chases over the same buffer ----
    // Interleaved, so the footprint matches the latency phase above.
    chase_ok = mem_ok && build_chases(buf, nbytes, 64, CHASE_WAYS,
                                      w->seed ^ UINT64_C(0xA5A5A5A5), heads) == 0;
    for (int rep = 0; rep < reps; ++rep) {
        phase_begin_mem(w, rep, buf, nbytes, 1);
        if (!chase_ok) continue;
        chase_ctx_t ch;
        for (int i = 0; i < CHASE_WAYS; ++i) ch.cur[i] = (void **)heads[i];
        const uint64_t hops = run_timed(ks->mem_mlp, &ch, w->seconds, 1024, &elapsed);
        keep_best_lat(&w->mem_mlp_ns,
                      elapsed / ((double)hops * (double)CHASE_WAYS) * 1e9);
        for (int i = 0; i < CHASE_WAYS; ++i) {
            w->checksum ^= (uint64_t)(uintptr_t)ch.cur[i];
        }
    }

    plat_aligned_free(buf);
    return NULL;
}

// ---------------------------------------------------------------------------
// CPU list parsing / platform info
// ---------------------------------------------------------------------------

static int get_cpu_count(void) {
    int n = plat_cpu_count();
    if (n < 1) n = 1;
    if (n > 1024) n = 1024;
    return n;
}

// Parse "0-3,6,8-11" into a list of CPU ids. Returns count, or -1 on error.
static int parse_cpu_list(const char *spec, int *out, int max_out) {
    int n = 0;
    const char *p = spec;
    while (*p) {
        char *end = NULL;
        const long a = strtol(p, &end, 10);
        if (end == p) return -1;
        long b = a;
        p = end;
        if (*p == '-') {
            p++;
            b = strtol(p, &end, 10);
            if (end == p) return -1;
            p = end;
        }
        if (a < 0 || b < a) return -1;
        for (long v = a; v <= b; ++v) {
            if (n >= max_out) return -1;
            out[n++] = (int)v;
        }
        if (*p == ',') p++;
        else if (*p) return -1;
    }
    return n;
}

static const char *arch_string(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
// Both spellings: __loongarch64 is the older predefine, __loongarch_grlen the
// one the current toolchain convention documents.
#elif defined(__loongarch64) || \
      (defined(__loongarch__) && defined(__loongarch_grlen) && __loongarch_grlen == 64)
    return "loongarch64";
// The two endiannesses of 64-bit POWER are not interchangeable binaries and do
// not share an ABI, so they are separate targets rather than one with a note.
// Every current distribution ships the little-endian one.
#elif defined(__powerpc64__) || defined(__PPC64__)
#if defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return "ppc64le";
#else
    return "ppc64";
#endif
#elif defined(__s390x__)
    return "s390x";
#else
    return "unknown";
#endif
}

// ---------------------------------------------------------------------------
// Naming the CPU
// ---------------------------------------------------------------------------

// The hosts that answer this question out of /proc/cpuinfo: everything that is
// neither Windows, which has the registry, nor macOS, which has sysctl. Named
// once so the helpers below and the branch that calls them cannot drift apart.
#if !defined(_WIN32) && !defined(__APPLE__)
#define CPCPUB_PROC_CPUINFO 1
#endif

#ifdef CPCPUB_PROC_CPUINFO
// Append one name to the " + "-separated list in `out`, if it is not already
// in it. The membership test is per element and exact: a substring test reads
// "Cortex-A5" as already present once "Cortex-A55" is there, and those pairs
// are precisely what the big.LITTLE case produces.
static void models_add(char *out, size_t outsz, const char *name) {
    if (!name[0]) return;
    const size_t n = strlen(name);
    for (const char *p = out; *p; ) {
        const char *sep = strstr(p, " + ");
        const size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0) return;
        if (!sep) break;
        p = sep + 3;
    }
    const size_t used = strlen(out);
    if (used + 1 >= outsz) return;
    snprintf(out + used, outsz - used, "%s%s", used ? " + " : "", name);
}
#endif  // CPCPUB_PROC_CPUINFO

#if defined(CPCPUB_PROC_CPUINFO) && defined(__aarch64__)
// Arm parts carry no brand string: there is no CPUID leaf to hold one and no
// vendor writes one into /proc/cpuinfo, which publishes the raw MIDR fields
// instead -- an implementer byte and a 12-bit part number. Without the tables
// below every Arm machine names itself "aarch64", which is the one thing a
// reader of these results already knows from build.target.
//
// The numbers are the documented MIDR_EL1 values, as carried by Arm's own
// manuals and the kernel's arch/arm64/include/asm/cputype.h. Only parts that
// can run a 64-bit kernel are listed -- AArch32-only and Cortex-M cores cannot
// be running this binary, so naming them would be dead weight. An unlisted
// part still reports its numbers rather than "unknown": silicon newer than
// this table is exactly the case where the raw pair is worth having.
typedef struct { unsigned part; const char *name; } midr_part_t;

static const midr_part_t midr_arm[] = {
    { 0xd02, "Cortex-A34" },    { 0xd03, "Cortex-A53" },
    { 0xd04, "Cortex-A35" },    { 0xd05, "Cortex-A55" },
    { 0xd06, "Cortex-A65" },    { 0xd07, "Cortex-A57" },
    { 0xd08, "Cortex-A72" },    { 0xd09, "Cortex-A73" },
    { 0xd0a, "Cortex-A75" },    { 0xd0b, "Cortex-A76" },
    { 0xd0c, "Neoverse-N1" },   { 0xd0d, "Cortex-A77" },
    { 0xd0e, "Cortex-A76AE" },  { 0xd14, "Cortex-R82AE" },
    { 0xd15, "Cortex-R82" },    { 0xd40, "Neoverse-V1" },
    { 0xd41, "Cortex-A78" },    { 0xd42, "Cortex-A78AE" },
    { 0xd43, "Cortex-A65AE" },  { 0xd44, "Cortex-X1" },
    { 0xd46, "Cortex-A510" },   { 0xd47, "Cortex-A710" },
    { 0xd48, "Cortex-X2" },     { 0xd49, "Neoverse-N2" },
    { 0xd4a, "Neoverse-E1" },   { 0xd4b, "Cortex-A78C" },
    { 0xd4c, "Cortex-X1C" },    { 0xd4d, "Cortex-A715" },
    { 0xd4e, "Cortex-X3" },     { 0xd4f, "Neoverse-V2" },
    { 0xd80, "Cortex-A520" },   { 0xd81, "Cortex-A720" },
    { 0xd82, "Cortex-X4" },     { 0xd83, "Neoverse-V3AE" },
    { 0xd84, "Neoverse-V3" },   { 0xd85, "Cortex-X925" },
    { 0xd87, "Cortex-A725" },   { 0xd88, "Cortex-A520AE" },
    { 0xd89, "Cortex-A720AE" }, { 0xd8a, "C1-Nano" },
    { 0xd8b, "C1-Pro" },        { 0xd8c, "C1-Ultra" },
    { 0xd8e, "Neoverse-N3" },   { 0xd8f, "Cortex-A320" },
    { 0xd90, "C1-Premium" },
};

// Apple silicon reaches this path only under a Linux port -- macOS answers
// from sysctl above and never gets here -- so the cores are named the way that
// project's users see them rather than by the marketing part.
static const midr_part_t midr_apple[] = {
    { 0x020, "Icestorm-A14" },     { 0x021, "Firestorm-A14" },
    { 0x022, "Icestorm-M1" },      { 0x023, "Firestorm-M1" },
    { 0x024, "Icestorm-M1-Pro" },  { 0x025, "Firestorm-M1-Pro" },
    { 0x026, "Thunder-M10" },      { 0x028, "Icestorm-M1-Max" },
    { 0x029, "Firestorm-M1-Max" }, { 0x030, "Blizzard-A15" },
    { 0x031, "Avalanche-A15" },    { 0x032, "Blizzard-M2" },
    { 0x033, "Avalanche-M2" },     { 0x034, "Blizzard-M2-Pro" },
    { 0x035, "Avalanche-M2-Pro" }, { 0x036, "Sawtooth-A16" },
    { 0x037, "Everest-A16" },      { 0x038, "Blizzard-M2-Max" },
    { 0x039, "Avalanche-M2-Max" },
};

static const midr_part_t midr_qcom[] = {
    { 0x001, "Oryon" },          { 0x201, "Kryo" },
    { 0x205, "Kryo" },           { 0x211, "Kryo" },
    { 0x800, "Falkor-V1/Kryo" }, { 0x801, "Kryo-V2" },
    { 0x802, "Kryo-3XX-Gold" },  { 0x803, "Kryo-3XX-Silver" },
    { 0x804, "Kryo-4XX-Gold" },  { 0x805, "Kryo-4XX-Silver" },
    { 0xc00, "Falkor" },         { 0xc01, "Saphira" },
};

static const midr_part_t midr_cavium[] = {
    { 0x0a0, "ThunderX" },       { 0x0a1, "ThunderX-88XX" },
    { 0x0a2, "ThunderX-81XX" },  { 0x0a3, "ThunderX-83XX" },
    { 0x0af, "ThunderX2-99xx" }, { 0x0b0, "OcteonTX2" },
    { 0x0b1, "OcteonTX2-98XX" }, { 0x0b2, "OcteonTX2-96XX" },
    { 0x0b3, "OcteonTX2-95XX" }, { 0x0b4, "OcteonTX2-95XXN" },
    { 0x0b5, "OcteonTX2-95XXMM" }, { 0x0b6, "OcteonTX2-95XXO" },
    { 0x0b8, "ThunderX3-T110" },
};

// HiSilicon ships two Arm designs under its own implementer byte and advertises
// them as the Arm parts they are, so the names follow what the silicon claims.
static const midr_part_t midr_hisi[] = {
    { 0xd01, "TaiShan-v110" }, { 0xd02, "TaiShan-v120" },
    { 0xd40, "Cortex-A76" },   { 0xd41, "Cortex-A77" },
};

static const midr_part_t midr_nvidia[] = {
    { 0x000, "Denver" }, { 0x003, "Denver-2" },
    { 0x004, "Carmel" }, { 0x010, "Olympus" },
};

static const midr_part_t midr_samsung[] = {
    { 0x001, "exynos-m1" }, { 0x002, "exynos-m3" },
    { 0x003, "exynos-m4" }, { 0x004, "exynos-m5" },
};

static const midr_part_t midr_ampere[] = {
    { 0xac3, "Ampere-1" }, { 0xac4, "Ampere-1a" },
};

static const midr_part_t midr_fujitsu[] = {
    { 0x001, "A64FX" }, { 0x003, "MONAKA" },
};

static const midr_part_t midr_phytium[] = {
    { 0x303, "FTC310" }, { 0x660, "FTC660" }, { 0x661, "FTC661" },
    { 0x662, "FTC662" }, { 0x663, "FTC663" }, { 0x664, "FTC664" },
    { 0x862, "FTC862" },
};

static const midr_part_t midr_brcm[]   = { { 0x100, "Brahma-B53" },
                                           { 0x516, "ThunderX2" } };
static const midr_part_t midr_apm[]    = { { 0x000, "X-Gene" } };
static const midr_part_t midr_ms[]     = { { 0xd49, "Azure-Cobalt-100" } };

typedef struct {
    unsigned            impl;
    const char         *vendor;
    const midr_part_t  *parts;
    int                 n;
} midr_impl_t;

static const midr_impl_t midr_impls[] = {
    { 0x41, "Arm",       midr_arm,     ARRAY_LEN(midr_arm)     },
    { 0x42, "Broadcom",  midr_brcm,    ARRAY_LEN(midr_brcm)    },
    { 0x43, "Cavium",    midr_cavium,  ARRAY_LEN(midr_cavium)  },
    { 0x46, "Fujitsu",   midr_fujitsu, ARRAY_LEN(midr_fujitsu) },
    { 0x48, "HiSilicon", midr_hisi,    ARRAY_LEN(midr_hisi)    },
    { 0x4e, "NVIDIA",    midr_nvidia,  ARRAY_LEN(midr_nvidia)  },
    { 0x50, "APM",       midr_apm,     ARRAY_LEN(midr_apm)     },
    { 0x51, "Qualcomm",  midr_qcom,    ARRAY_LEN(midr_qcom)    },
    { 0x53, "Samsung",   midr_samsung, ARRAY_LEN(midr_samsung) },
    { 0x61, "Apple",     midr_apple,   ARRAY_LEN(midr_apple)   },
    { 0x6d, "Microsoft", midr_ms,      ARRAY_LEN(midr_ms)      },
    { 0x70, "Phytium",   midr_phytium, ARRAY_LEN(midr_phytium) },
    { 0xc0, "Ampere",    midr_ampere,  ARRAY_LEN(midr_ampere)  },
};

// One MIDR implementer/part pair as a name. The vendor is named too, except
// where it would only repeat itself: an Arm part number spells out "Cortex-"
// or "Neoverse-" already, and "Ampere-1" is not improved by "Ampere Ampere-1".
static void midr_name(unsigned impl, unsigned part, char *out, size_t outsz) {
    for (int i = 0; i < ARRAY_LEN(midr_impls); ++i) {
        const midr_impl_t *v = &midr_impls[i];
        if (v->impl != impl) continue;
        for (int p = 0; p < v->n; ++p) {
            if (v->parts[p].part != part) continue;
            const char *name = v->parts[p].name;
            if (impl == 0x41 ||
                strncmp(name, v->vendor, strlen(v->vendor)) == 0) {
                snprintf(out, outsz, "%s", name);
            } else {
                snprintf(out, outsz, "%s %s", v->vendor, name);
            }
            return;
        }
        snprintf(out, outsz, "%s part 0x%03x", v->vendor, part);
        return;
    }
    snprintf(out, outsz, "impl 0x%02x part 0x%03x", impl, part);
}
#endif  // CPCPUB_PROC_CPUINFO && __aarch64__

#ifdef CPCPUB_PROC_CPUINFO
// The board's own name for itself, from the flattened device tree: a property
// holding one NUL-terminated string, e.g. "Radxa ROCK 5B". It answers a
// question the core names cannot -- an RK3588 and an Allwinner A733 are both
// "Cortex-A55 + Cortex-A76", and on a board of SBCs that is the difference
// between two rows and one. Absent on PCs, which have a brand string instead.
static void dt_model(char *out, size_t outsz) {
    out[0] = '\0';
    FILE *f = fopen("/proc/device-tree/model", "rb");
    if (!f) return;
    char raw[128];
    const size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    raw[n] = '\0';
    // Printable ASCII only, and collapsed to single spaces: this string is
    // written by whoever wrote the device tree and ends up in a JSON document.
    size_t w = 0;
    for (size_t i = 0; i < n && raw[i] && w + 1 < outsz; ++i) {
        const unsigned char c = (unsigned char)raw[i];
        if (c < 0x20 || c > 0x7e) continue;
        if (c == ' ' && (w == 0 || out[w - 1] == ' ')) continue;
        out[w++] = (char)c;
    }
    while (w > 0 && out[w - 1] == ' ') w--;
    out[w] = '\0';
}
#endif  // CPCPUB_PROC_CPUINFO

// Distinct CPU model names (more than one on big.LITTLE), and the board they
// are on where the machine will say.
static void detect_cpu_models(char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = '\0';
#if defined(_WIN32)
    // The registry, because Win32 has no API that returns the brand string on
    // both architectures: CPUID is x86-only and there is no Arm equivalent,
    // while the kernel writes what it found for every CPU it enumerated here.
    // Processor 0's entry, matching the /proc/cpuinfo scan below in spirit --
    // Windows does describe each logical CPU separately, but it copies one
    // package name to all of them, so walking the rest would only de-duplicate
    // its way back to this string.
    DWORD len = (DWORD)outsz;
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, NULL, out,
                     &len) != ERROR_SUCCESS) {
        out[0] = '\0';
    }
    return;
#elif defined(__APPLE__)
    // One name per machine: macOS reports the package, not the cluster, so an
    // asymmetric Apple part reads as "Apple M4" rather than naming its P and E
    // cores separately. hw.model ("Mac16,3") is the fallback when the brand
    // string is missing, which is better than an empty field.
    static const char *const keys[] = { "machdep.cpu.brand_string", "hw.model" };
    for (int k = 0; k < ARRAY_LEN(keys); ++k) {
        size_t len = outsz;
        if (sysctlbyname(keys[k], out, &len, NULL, 0) == 0 && out[0]) return;
        out[0] = '\0';
    }
    return;
#else
    // Three ways a Linux kernel names a CPU, in order of how much they say:
    //
    //   1. a brand string under one of the model-name keys -- x86, loongarch,
    //      ppc64le, and the RISC-V cores that fill in "uarch";
    //   2. the MIDR implementer/part pair, which is all Arm publishes;
    //   3. the platform's name for itself -- the device tree's `model`, or the
    //      "Hardware" line an Android kernel writes -- which on an SBC or a
    //      phone is the only thing separating two boards built from the same
    //      cores.
    //
    // (2) is a fallback for (1) rather than an addition to it: a kernel that
    // gives both is describing one CPU twice. (3) is an addition to either,
    // in parentheses, and stands alone where neither of the others answered.
    char models[320], platform[128];
    models[0] = platform[0] = '\0';

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        // "cpu" is ppc64le's key and "uarch" is RISC-V's; the others are the
        // several spellings x86, loongarch and MIPS use. Matched whole, since
        // a prefix test for "cpu" would also take x86's "cpu family".
        static const char *const keys[] = {
            "model name", "Model Name", "cpu model", "cpu", "uarch",
        };
#if defined(CPCPUB_PROC_CPUINFO) && defined(__aarch64__)
        char midrs[320];
        midrs[0] = '\0';
        unsigned impl = 0;
        int have_impl = 0;
#endif
        char line[1024];
        int continuation = 0;
        while (fgets(line, (int)sizeof(line), f)) {
            // A Features or flags line can outrun the buffer, and its tail
            // would then be read as a line of its own -- with a colon in it,
            // on some kernels. Anything after a chunk that did not end in a
            // newline is such a tail, and is skipped.
            const size_t got = strlen(line);
            const int was_partial = continuation;
            continuation = got > 0 && line[got - 1] != '\n';
            if (was_partial) continue;

            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            char *key = line, *val = colon + 1;
            char *end = key + strlen(key);
            while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
            while (*val == ' ' || *val == '\t') val++;
            end = val + strlen(val);
            while (end > val && (end[-1] == '\n' || end[-1] == '\r' ||
                                 end[-1] == ' '  || end[-1] == '\t')) *--end = '\0';
            if (!*key || !*val) continue;

            for (int k = 0; k < ARRAY_LEN(keys); ++k) {
                if (strcmp(key, keys[k]) == 0) {
                    models_add(models, sizeof(models), val);
                    break;
                }
            }
            // Not a model name: "Hardware" is the SoC or board, which is why
            // it is held aside as the platform rather than added to the list.
            if (!platform[0] && strcmp(key, "Hardware") == 0) {
                snprintf(platform, sizeof(platform), "%s", val);
            }
#if defined(CPCPUB_PROC_CPUINFO) && defined(__aarch64__)
            // Both fields appear once per processor, implementer first, so a
            // part number is always read against the implementer above it.
            if (strcmp(key, "CPU implementer") == 0) {
                impl = (unsigned)strtoul(val, NULL, 0);
                have_impl = 1;
            } else if (strcmp(key, "CPU part") == 0 && have_impl) {
                char name[96];
                midr_name(impl, (unsigned)strtoul(val, NULL, 0),
                          name, sizeof(name));
                models_add(midrs, sizeof(midrs), name);
            }
#endif
        }
        fclose(f);
#if defined(CPCPUB_PROC_CPUINFO) && defined(__aarch64__)
        if (!models[0]) snprintf(models, sizeof(models), "%s", midrs);
#endif
    }

    // The device tree names the board ("Radxa ROCK 5B"); "Hardware" tends to
    // name the SoC behind it. The first is the more specific of the two, so it
    // wins where the machine offers both.
    char board[128];
    dt_model(board, sizeof(board));
    if (!board[0]) snprintf(board, sizeof(board), "%s", platform);

    if (models[0] && board[0]) snprintf(out, outsz, "%s (%s)", models, board);
    else if (models[0])        snprintf(out, outsz, "%s", models);
    else                       snprintf(out, outsz, "%s", board);
#endif
}

// Short compiler name and version, e.g. "gcc 15.2.0".
static void compiler_string(char *out, size_t outsz) {
#if defined(__clang__)
    snprintf(out, outsz, "clang %d.%d.%d",
             __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    snprintf(out, outsz, "gcc %d.%d.%d",
             __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    snprintf(out, outsz, "cc");
#endif
}

// The toolchain's own full version banner, which carries the distribution's
// patch level -- two "gcc 15.2.0" builds are not always the same compiler.
static const char *compiler_version(void) {
#ifdef __VERSION__
    return __VERSION__;
#else
    return "";
#endif
}

// The driver and flags this binary was compiled with, baked in by the Makefile.
// Without them a result cannot be checked for comparability: -march, -ffast-math
// or a different -O level change the numbers and are invisible otherwise.
//
// These are the flags *every* variant shares. The two that differ come from the
// kernel_set_t, so a result reports the build its kernels actually got rather
// than the one the driver was compiled with.
#ifndef BUILD_CC
#define BUILD_CC "cc"
#endif
#ifndef BUILD_FLAGS
#define BUILD_FLAGS ""
#endif

// The SHA-256 of the running binary, or NULL where it cannot be taken.
//
// The path comes from plat_executable_path(), never from argv[0]: it names the
// file that is actually executing, whatever the shell called it and wherever
// $PATH found it. That is what lets a results hub tell a run produced by a
// published build from one produced by a local compile with flags nobody
// recorded -- see
// `build.binary_sha256` in schema/cpu-bench-1.md. It proves nothing on its own,
// being self-reported like every other field; what it buys is that two results
// claiming the same digest were produced by the same machine code.
//
// Computed once, on first use, and not at all in a run that neither prints the
// banner nor emits JSON. Not reentrant; the callers are single-threaded.
static const char *binary_sha256(void) {
    static char hex[65];
    static int done = 0;
    if (!done) {
        done = 1;
        hex[0] = '\0';
        char path[4096];
        if (plat_executable_path(path, sizeof path) != 0 ||
            sha256_file(path, hex) != 0) hex[0] = '\0';
    }
    return hex[0] ? hex : NULL;
}

// "<shared flags> <this variant's flags>", the complete command line the
// measured code was built with. Not reentrant; the callers are single-threaded.
static const char *variant_flags(const kernel_set_t *k) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s%s%s", BUILD_FLAGS, BUILD_FLAGS[0] ? " " : "",
             k->flags);
    return buf;
}

// The banner naming the variant about to run. Printed before every variant, so
// a --variants run is readable as a sequence rather than as one long table.
static void print_variant(const kernel_set_t *k, int idx, int n_sel) {
    FILE *o = msg();
    if (n_sel > 1) fprintf(o, "\n=== variant %d/%d: ", idx + 1, n_sel);
    else           fprintf(o, "variant: ");
    fprintf(o, "%s  vectorize=%s fma=%s  (%s)%s\n", k->name,
            k->vectorize ? "on" : "off", k->fma ? "on" : "off", k->flags,
            n_sel > 1 ? " ===" : "");
}

static void print_platform(void) {
    FILE *o = msg();
    char cc[64];
    compiler_string(cc, sizeof(cc));
    fprintf(o, "build: %s", cc);
#ifdef __STDC_VERSION__
    fprintf(o, ", C%ld", (long)__STDC_VERSION__);
#endif
    fprintf(o, ", target=%s\n", arch_string());
    if (BUILD_FLAGS[0]) fprintf(o, "flags: %s %s\n", BUILD_CC, BUILD_FLAGS);
    // Printed so it can be checked against the SHA256SUMS of a release without
    // hashing the binary by hand.
    const char *digest = binary_sha256();
    if (digest) fprintf(o, "binary: sha256:%s\n", digest);

    plat_sysinfo_t si;
    if (plat_sysinfo(&si) == 0) {
        fprintf(o, "system: %s %s, machine=%s, cpus=%d\n",
                si.sysname, si.release, si.machine, get_cpu_count());
    }
    char models[512];
    detect_cpu_models(models, sizeof(models));
    if (models[0]) fprintf(o, "cpu: %s\n", models);
}

// ---------------------------------------------------------------------------
// Running a set of workers
// ---------------------------------------------------------------------------

typedef struct {
    double seconds;
    double warmup;
    int    reps;
    size_t mem_per_thread;
    uint64_t seed;
    const kernel_set_t *kernels;   // build variant every worker measures
} run_opts_t;

// Best-of-N rejects most interference, but a loaded box depresses every rep.
static void warn_if_loaded(int threads) {
    // getloadavg(3) rather than /proc/loadavg: same number on Linux, and the
    // only way to ask on a system without /proc. Windows keeps no such average
    // at all, so there the warning is simply not available -- see plat_loadavg.
    double la = 0.0;
    if (plat_loadavg(&la) != 0) return;
    const int cpus = get_cpu_count();
    if (la > 0.5 && la > 0.15 * (double)cpus) {
        fprintf(msg(), "WARNING: load average %.2f on %d CPUs; results will be "
                       "depressed. Quiesce the machine or raise --reps.\n",
                la, cpus);
    }
    (void)threads;
}

static int run_workers(worker_t *w, int n, const run_opts_t *o) {
    plat_barrier_t bar;
    int use_bar = (n > 1);
    if (use_bar && plat_barrier_init(&bar, (unsigned)n) != 0) {
        perror("barrier_init");
        return -1;
    }

    pthread_t *ths = (pthread_t *)calloc((size_t)n, sizeof(*ths));
    if (!ths) { perror("calloc"); return -1; }

    for (int i = 0; i < n; ++i) {
        w[i].id = i;
        w[i].seconds = o->seconds;
        w[i].warmup = o->warmup;
        w[i].reps = o->reps;
        w[i].mem_bytes = o->mem_per_thread;
        w[i].bar = use_bar ? &bar : NULL;
        w[i].ks = o->kernels;
        w[i].seed = o->seed + (uint64_t)i * UINT64_C(0x9E3779B97F4A7C15) + 1u;
        const int rc = pthread_create(&ths[i], NULL, worker, &w[i]);
        if (rc != 0) { errno = rc; perror("pthread_create"); free(ths); return -1; }
    }
    for (int i = 0; i < n; ++i) pthread_join(ths[i], NULL);

    free(ths);
    if (use_bar) plat_barrier_destroy(&bar);
    return 0;
}

// `--version`. The version string is a label the build put there and can say
// anything; the digest below it is what a reader can check against a release's
// SHA256SUMS, so both are printed and the second is the one that settles it.
static void print_version(void) {
    fprintf(msg(), "cpcpub %s, schema cpu-bench/1\n", CPCPUB_VERSION);
    print_platform();
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --variant NAME     build variant to measure: scalar-nofma (default),\n"
        "                     vector-nofma, scalar-fma, vector-fma\n"
        "  --variants         run every variant the toggles change on this\n"
        "                     target, in turn, and compare them. --variants=all\n"
        "                     forces all four, --variants=A,B picks a list\n"
        "  --list-variants    show the variants this binary carries and exit\n"
        "  --threads N        threads to run (default: online CPUs)\n"
        "  --cpus LIST        pin to these CPUs, e.g. 0-3,6 (default: 0..threads-1)\n"
        "  --per-core         run the suite single-threaded on each CPU in turn\n"
        "  --full             both: the multi-threaded run and the per-core\n"
        "                     sweep in one report. Use this for uploads\n"
        "  --disp-sweep       diagnostic: sweep the indirect-dispatch selector\n"
        "                     period on each CPU and print calls/cycle vs period\n"
        "  --time SEC         measured seconds per phase (default 0.5)\n"
        "  --reps N           repetitions per phase, best is kept (default 3)\n"
        "  --warmup SEC       warm-up seconds before each phase (default 0.15)\n"
        "  --mem-per-thread B per-thread buffer for the memory phases (default 16 MiB)\n"
        "  --mem BYTES        total memory across all threads (overrides the above)\n"
        "  --no-mem           skip the memory bandwidth and latency phases\n"
        "  --no-pin           do not set thread affinity\n"
        "  --clock mono|raw   clock source (default raw)\n"
        "  --seed N           PRNG seed for setup (default 1)\n"
        "  --mhz N            the core clock, for a machine that will not say:\n"
        "                     no cpufreq node and no device-tree clock leaves\n"
        "                     only the INT-lat estimate, which assumes one\n"
        "                     dependent op per cycle and reads low on a core\n"
        "                     that does not manage it. Recorded as given, not\n"
        "                     measured, and changes no other number\n"
        "  --format F         output as text (default), json or tsv;\n"
        "                     --json and --tsv are shorthands. In json/tsv the\n"
        "                     results go to stdout and all prose to stderr\n"
        "  -v, --verbose      explain every metric after the results\n"
        "  --version          print the version, the build and this machine,\n"
        "                     then exit\n"
        "\n"
        "Uploading (the results hub):\n"
        "  --submit [URL]     after measuring, upload the result to the hub at\n"
        "                     URL, e.g. http://my.server:8782%s\n"
        "  --token TOKEN      upload token, so the run lands on your account\n"
        "                     rather than anonymously. Get one from the hub's\n"
        "                     Account tab. $CPCPUB_TOKEN is read when this is\n"
        "                     not given, and keeps it out of the process list\n"
        "  --label TEXT       short label for the run, e.g. the machine's name\n"
        "  --notes TEXT       longer notes: cooling, governor, anything that\n"
        "                     explains the numbers\n"
        "The URL may also come from $CPCPUB_HUB. https needs curl on PATH.\n",
        prog,
        CPCPUB_HUB_URL[0] ? " (default: " CPCPUB_HUB_URL ")"
                          : ". This build has no default hub, so the URL is "
                            "required");
}

// Largest cache size the system reports, in bytes (0 if unknown).
static size_t detect_llc_bytes(void) {
    size_t best = 0;
#if defined(_WIN32)
    // GetLogicalProcessorInformationEx(RelationCache) returns one record per
    // cache instance, so the largest CacheSize is the last level -- the same
    // "biggest cache named anywhere" the sysfs walk below takes, and wrong in
    // the same safe direction if a level goes unreported.
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &len);
    if (len == 0) return 0;
    unsigned char *buf = (unsigned char *)malloc(len);
    if (!buf) return 0;
    if (GetLogicalProcessorInformationEx(
            RelationCache, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
        // A variable-length record list: each entry says how long it is, so
        // it is walked by byte offset rather than indexed as an array. The
        // guard is the fixed header -- a relationship and a size -- which has
        // to be readable before Size can be trusted to step to the next one.
        const DWORD header = (DWORD)(sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) +
                                     sizeof(DWORD));
        for (DWORD off = 0; off + header <= len;) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e =
                (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + off);
            if (e->Size == 0) break;
            if (e->Relationship == RelationCache &&
                (size_t)e->Cache.CacheSize > best) {
                best = (size_t)e->Cache.CacheSize;
            }
            off += e->Size;
        }
    }
    free(buf);
    return best;
#elif defined(__APPLE__)
    // Apple Silicon exposes no L3 key; the per-performance-level L2 is the
    // largest cache it names, and on these parts the SLC below it is not
    // reported at all. An underestimate only makes the warning fire sooner,
    // which is the safe direction for a working-set check.
    static const char *const keys[] = {
        "hw.l3cachesize", "hw.l2cachesize",
        "hw.perflevel0.l2cachesize", "hw.perflevel1.l2cachesize",
    };
    for (int k = 0; k < ARRAY_LEN(keys); ++k) {
        uint64_t v = 0;
        size_t len = sizeof(v);
        if (sysctlbyname(keys[k], &v, &len, NULL, 0) == 0 && v > best)
            best = (size_t)v;
    }
    return best;
#else
    for (int i = 0; i < 16; ++i) {
        char path[160];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu0/cache/index%d/size", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char val[32] = {0};
        if (fgets(val, (int)sizeof(val), f)) {
            char *end = NULL;
            const double n = strtod(val, &end);
            size_t mult = 1;
            if (end && (*end == 'K' || *end == 'k')) mult = 1024;
            else if (end && (*end == 'M' || *end == 'm')) mult = 1024 * 1024;
            const size_t bytes = (size_t)(n * (double)mult);
            if (bytes > best) best = bytes;
        }
        fclose(f);
    }
    return best;
#endif
}

// A working set that fits in last-level cache turns MEMlat into a cache latency
// measurement, which looks like a great result rather than a misconfiguration.
static void warn_if_working_set_small(size_t per_thread, int threads, int per_core) {
    if (per_thread == 0) return;
    const size_t llc = detect_llc_bytes();
    if (llc == 0) return;
    const size_t total = per_core ? per_thread : per_thread * (size_t)threads;
    if (total < llc * 2) {
        fprintf(msg(), "WARNING: %zu KiB working set vs %zu KiB last-level "
                       "cache; the memory phases will report cache, not DRAM. "
                       "Raise --mem-per-thread.\n", total >> 10, llc >> 10);
    }
}

// Prose explaining the columns. Printed only under -v: the tables carry their
// own units, and the explanation does not change between runs.
static void print_legend_ratios(void) {
    if (!g_verbose) return;
    FILE *o = msg();
    fprintf(o, "\nreading the ratios\n");
    fprintf(o,
        "  ILP tracks issue width: the integer kernel holds no multiply, and its\n"
        "  two forms carry the same loop overhead per counted op, so that\n"
        "  cancels and the ratio is what the core issues in parallel. Roughly\n"
        "  1x for a single-issue core, 2x for a 2-wide in-order one, 3x for\n"
        "  3 ALUs.\n");
    fprintf(o,
        "  fILP is NOT a width measure and not \"bigger is better\": a core with\n"
        "  slow FP needs more ops in flight to fill its pipes and so scores\n"
        "  higher. Compare FP-thr for capability.\n");
    fprintf(o,
        "  MLP is how much memory latency the core hides. In-order cores stall on\n"
        "  the first miss and sit near 1x; it can exceed %d when parallel page\n"
        "  table walks overlap too.\n", CHASE_WAYS);
    fprintf(o,
        "  DISPcap is a length, not a rate, so it needs no clock normalisation. A\n"
        "  fixed random call sequence of period L is repeated, L is swept %zu..%zu,\n"
        "  and the curve -- normalised between the plateau at small L and the rate\n"
        "  on a fully random stream -- is integrated over log2(L). A core that\n"
        "  predicts everything up to L and nothing beyond reads out exactly L.\n"
        "  'none' means the core was no faster even calling one single repeated\n"
        "  target, i.e. no usable indirect prediction; '<%zu' means it loses the\n"
        "  pattern before the span starts. Background load inflates this column,\n"
        "  so measure it on a quiet machine.\n",
        g_disp_ladder[DISP_SPAN_FIRST], g_disp_ladder[DISP_SPAN_LAST],
        g_disp_ladder[DISP_SPAN_FIRST]);
}

// Names every column of the results table; the headers themselves are shorthand.
static void print_legend(void) {
    if (!g_verbose) return;
    FILE *o = msg();
    fprintf(o, "\nwhat each column means\n");
    static const char *rows[][3] = {
        {"MHz",      "core clock",              "observed under load; '~' = estimated from INT-lat, which assumes one dependent op per cycle -- see --mhz"},
        {"INT-lat",  "integer latency",         "one dependent chain of 1-cycle ALU ops, Mop/s"},
        {"INT-thr",  "integer throughput",      "8 independent chains, Mop/s"},
        {"ILP",      "instruction parallelism", "INT-thr / INT-lat"},
        {"MUL-thr",  "multiply throughput",     "64-bit integer multiplies, Mmul/s"},
        {"FP-lat",   "FP latency",              "one dependent multiply-add chain, Mflop/s"},
        {"FP-thr",   "FP throughput",           "8 independent chains, Mflop/s"},
        {"fILP",     "FP ops in flight",        "FP-thr / FP-lat"},
        {"MEM",      "memory bandwidth",        "sequential read+write sweep, GB/s"},
        {"MEMlat",   "memory latency",          "random dependent pointer chase, ns"},
        {"MLP",      "memory parallelism",      "MEMlat / latency with 8 chases in flight"},
        {"DISP-thr", "indirect dispatch rate",  "calls/s once the target pattern is learned, Mcall/s"},
        {"DISPcap",  "indirect predictor size", "longest repeating call pattern still predicted, in calls"},
        {"score",    "composite",               "geomean of INT-thr, MUL-thr, FP-thr, DISP-thr, DISPcap, MLP rate"},
    };
    for (int i = 0; i < ARRAY_LEN(rows); ++i) {
        fprintf(o, "  %-9s %-24s %s\n", rows[i][0], rows[i][1], rows[i][2]);
    }
    print_legend_ratios();
    fprintf(o, "  score is comparable across cores of one run, not across machines.\n");
    fprintf(o, "  the total row's score sums the throughputs first, so it is a\n"
               "  whole-machine number ~threads times a single core's, not one of them.\n");
}

// The DRAM controller clock seen during the memory phases. If it moved, the
// memory numbers carry that spread.
static void report_dram(double lo, double hi) {
    if (hi <= 0.0) return;
    FILE *o = msg();
    if (hi - lo > 1.0) {
        fprintf(o, "DRAM (%s): %.0f-%.0f MHz during memory phases (changed "
                   "mid-run)\n", g_dram_name, lo, hi);
        if (g_verbose) {
            fprintf(o, "      Memory results carry that spread; pin the devfreq "
                       "governor to 'performance' for stable numbers.\n");
        }
    } else {
        fprintf(o, "DRAM (%s): %.0f MHz during memory phases\n", g_dram_name, hi);
    }
}

static double geomean(const double *v, int n) {
    double s = 0.0;
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (v[i] > 0.0) { s += log(v[i]); m++; }
    }
    return m > 0 ? exp(s / (double)m) : 0.0;
}

// Reduce the dispatch ladder to the numbers the tables report.
//
//   *thr  calls/s once the pattern has been learned -- the plateau, i.e.
//         front-end call throughput. Bigger is better.
//   *cap  longest repeating call sequence the core still predicts, in calls:
//         the floor-normalised curve integrated over log2(period).
//   *gain plateau / unpredictable floor: what prediction is worth on this core
//         at all. Used to reject a flat curve.
//   *span the same integral before exponentiation, i.e. capacity in ladder
//         steps. The score uses this because *cap spans orders of magnitude
//         between cores and would dominate a geomean of rates.
//
// Returns DISP_NA if the ladder was never measured, DISP_NONE if the core
// showed no prediction win at any period including a single repeated target,
// DISP_OK otherwise.
#define DISP_NA   0
#define DISP_OK   1
#define DISP_NONE 2

static int disp_summary(const worker_t *w, double *thr, double *cap,
                        double *gain, double *span) {
    *thr = 0.0; *cap = 0.0; *gain = 0.0; *span = 0.0;
    double plateau = 0.0;
    for (int i = 0; i < DISP_PLATEAU_PTS; ++i) {
        const double v = w->disp_ladder[DISP_SPAN_FIRST + i];
        if (v <= 0.0) return DISP_NA;
        if (v > plateau) plateau = v;
    }
    const double floor_rate = w->disp_ladder[DISP_LADDER_N - 1];
    const double ref_rate = w->disp_ladder[DISP_REF_IDX];
    if (plateau <= 0.0 || floor_rate <= 0.0 || ref_rate <= 0.0) return DISP_NA;

    *thr = plateau;
    *gain = plateau / floor_rate;

    if (*gain < DISP_MIN_GAIN) {
        // Curve is flat over the span. If the single-target reference is no
        // faster either, the core predicts no indirect targets at all; if it is
        // faster, capacity is below the first span point rather than unknown.
        if (ref_rate / floor_rate < DISP_MIN_GAIN) return DISP_NONE;
        *cap = (double)g_disp_ladder[DISP_SPAN_FIRST] / 2.0;
        return DISP_OK;
    }

    for (int i = DISP_SPAN_FIRST; i <= DISP_SPAN_LAST; ++i) {
        double q = (w->disp_ladder[i] - floor_rate) / (plateau - floor_rate);
        if (q > 1.0) q = 1.0;
        if (q < 0.0) q = 0.0;
        *span += q;
    }
    *cap = (double)g_disp_ladder[DISP_SPAN_FIRST] * pow(2.0, *span - 1.0);
    return DISP_OK;
}

// Render DISPcap for a table cell: below the ladder, past the ladder, or no
// prediction at all.
static void disp_cap_str(int st, double cap, char *buf, size_t n) {
    if (st == DISP_NONE)                             snprintf(buf, n, "none");
    else if (st != DISP_OK || cap <= 0.0)            snprintf(buf, n, "-");
    else if (cap < (double)g_disp_ladder[DISP_SPAN_FIRST])
        snprintf(buf, n, "<%zu", g_disp_ladder[DISP_SPAN_FIRST]);
    else if (cap >= (double)g_disp_ladder[DISP_SPAN_LAST])
        snprintf(buf, n, ">=%zu", g_disp_ladder[DISP_SPAN_LAST]);
    else                                             snprintf(buf, n, "%.0f", cap);
}

// Synthetic single-core score, spanning compute *and* the two things that
// separate a big core from a little one: indirect dispatch and overlapped cache
// misses. The per-component scale factors are constant factors on a geomean, so
// ratios between cores do not depend on them.
static double core_score(const worker_t *w) {
    double v[6];
    double disp_thr, disp_cap, disp_gain, disp_span;
    const int st = disp_summary(w, &disp_thr, &disp_cap, &disp_gain, &disp_span);
    v[0] = w->int_thr_mops;
    v[1] = w->fp_thr_mflops;
    v[4] = w->mul_thr_mops * 4.0;
    v[2] = disp_thr * 20.0;
    // Predictor capacity in ladder steps, +1 so a core with no indirect
    // prediction still scores poorly instead of dropping out of the geomean.
    v[5] = st == DISP_NA ? 0.0 : (disp_span + 1.0) * 2000.0;
    // Random-access rate with all chases in flight, in millions/s.
    v[3] = w->mem_mlp_ns > 0.0 ? (1000.0 / w->mem_mlp_ns) * 100.0 : 0.0;
    return geomean(v, ARRAY_LEN(v));
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

// One row of results: a single core in the per-core sweep, one thread of a
// multi-threaded run, or the aggregate over all of them.
typedef struct {
    int    cpu;              // pinned CPU, or -1
    double mhz;              // 0 if unknown
    // Where `mhz` came from, because the three are worth very different
    // amounts to a reader: read off the machine, asserted by the operator, or
    // inferred from INT-lat under an assumption the core may not honour.
    enum { MHZ_READ, MHZ_GIVEN, MHZ_ESTIMATED } mhz_src;
    double int_lat, int_thr, mul_thr, fp_lat, fp_thr;
    double mem_gbps, mem_lat_ns, mem_lat8_ns;
    double ilp, filp, mlp;
    double disp_thr, disp_cap, disp_gain, disp_span;
    int    disp_status;
    double score;
} result_t;

// The same six components as core_score(), rescaled to the whole machine: the
// throughputs are the ones aggregate() summed, and the random-access rate is
// multiplied by the thread count for the same reason -- n threads chase n sets
// of pointers at once, so the machine retires n times one thread's misses.
// DISPcap span is left as aggregate() averaged it: predictor size is a property
// of a core and does not add up across them.
//
// The result is therefore ~n times a single core's score and is not comparable
// with one. It answers "how much machine is there"; core_score() answers "how
// good is a core".
static double machine_score(const result_t *t, int n) {
    double v[6];
    v[0] = t->int_thr;
    v[1] = t->fp_thr;
    v[4] = t->mul_thr * 4.0;
    v[2] = t->disp_thr * 20.0;
    v[5] = t->disp_status == DISP_NA ? 0.0 : (t->disp_span + 1.0) * 2000.0;
    v[3] = t->mem_lat8_ns > 0.0
         ? (double)n * (1000.0 / t->mem_lat8_ns) * 100.0 : 0.0;
    return geomean(v, ARRAY_LEN(v));
}

static result_t summarize(const worker_t *w, int cpu) {
    result_t r;
    memset(&r, 0, sizeof(r));
    r.cpu = cpu;
    r.int_lat = w->int_lat_mops;
    r.int_thr = w->int_thr_mops;
    r.mul_thr = w->mul_thr_mops;
    r.fp_lat  = w->fp_lat_mflops;
    r.fp_thr  = w->fp_thr_mflops;
    r.mem_gbps    = w->mem_gbps;
    r.mem_lat_ns  = w->mem_lat_ns;
    r.mem_lat8_ns = w->mem_mlp_ns;
    r.ilp  = r.int_lat > 0.0 ? r.int_thr / r.int_lat : 0.0;
    r.filp = r.fp_lat  > 0.0 ? r.fp_thr  / r.fp_lat  : 0.0;
    r.mlp  = r.mem_lat8_ns > 0.0 ? r.mem_lat_ns / r.mem_lat8_ns : 0.0;
    r.disp_status = disp_summary(w, &r.disp_thr, &r.disp_cap, &r.disp_gain,
                                 &r.disp_span);
    r.score = core_score(w);

    // INT-lat is one dependent 1-cycle op per cycle by construction, so it
    // doubles as a clock estimate where nothing on the machine will say -- but
    // only by construction, so every source that knows is asked first: the
    // clock sampled under load, then cpufreq's ceiling, then whatever the
    // device tree declares for a board that has no cpufreq driver at all.
    if (g_mhz_given > 0.0) {
        r.mhz = g_mhz_given;
        r.mhz_src = MHZ_GIVEN;
    } else {
        r.mhz = w->mhz_observed;
        if (r.mhz <= 0.0 && cpu >= 0) r.mhz = cpu_max_mhz(cpu);
        if (r.mhz <= 0.0)             r.mhz = dt_cpu_mhz(cpu);
        if (r.mhz <= 0.0) {
            r.mhz = r.int_lat;
            r.mhz_src = MHZ_ESTIMATED;
        }
    }
    return r;
}

// The `mhz_src` value in JSON and TSV. "given" is deliberately not "measured":
// a reader can then tell a clock the machine reported from one its owner did.
static const char *mhz_src_name(int src) {
    switch (src) {
        case MHZ_GIVEN:     return "given";
        case MHZ_ESTIMATED: return "estimated";
        default:            return "measured";
    }
}

// TSV: one header line, then one row per scope. Missing values are empty. The
// leading `variant` column is what keeps a --variants run's rows apart, and is
// present in a single-variant run too so the column set never depends on how
// the binary was invoked.
#define RESULT_COLUMNS \
    "variant\tscope\tcpu\tmhz\tmhz_src\tint_lat_mops\tint_thr_mops\tilp" \
    "\tmul_thr_mmul_s\tfp_lat_mflops\tfp_thr_mflops\tfilp\tmem_gbps" \
    "\tmem_lat_ns\tmem_lat8_ns\tmlp\tdisp_thr_mcall_s\tdisp_cap_calls" \
    "\tdisp_gain\tdisp_span\tscore\n"

static void tsv_num(double v, int ok) {
    if (ok && v > 0.0) printf("\t%.6g", v);
    else               printf("\t");
}

static void tsv_result(const char *variant, const char *scope, const result_t *r) {
    printf("%s\t%s", variant, scope);
    if (r->cpu >= 0) printf("\t%d", r->cpu); else printf("\t");
    tsv_num(r->mhz, 1);
    printf("\t%s", r->mhz > 0.0 ? mhz_src_name(r->mhz_src)
                                : "unknown");
    tsv_num(r->int_lat, 1);
    tsv_num(r->int_thr, 1);
    tsv_num(r->ilp, 1);
    tsv_num(r->mul_thr, 1);
    tsv_num(r->fp_lat, 1);
    tsv_num(r->fp_thr, 1);
    tsv_num(r->filp, 1);
    tsv_num(r->mem_gbps, 1);
    tsv_num(r->mem_lat_ns, 1);
    tsv_num(r->mem_lat8_ns, 1);
    tsv_num(r->mlp, 1);
    tsv_num(r->disp_thr, 1);
    tsv_num(r->disp_cap, r->disp_status == DISP_OK);
    tsv_num(r->disp_gain, 1);
    tsv_num(r->disp_span, r->disp_status != DISP_NA);
    tsv_num(r->score, 1);
    printf("\n");
}

// JSON: `,"key": value`, with null for anything not measured.
static void j_num(const char *key, double v, int ok) {
    if (ok && v > 0.0) jout(", \"%s\": %.6g", key, v);
    else               jout(", \"%s\": null", key);
}

static void j_str(const char *key, const char *v) {
    jout(", \"%s\": \"", key);
    for (const char *p = v; *p; ++p) {
        if (*p == '"' || *p == '\\') jputc('\\');
        if ((unsigned char)*p < 0x20) continue;
        jputc(*p);
    }
    jout("\"");
}

static void json_result(const result_t *r, const char *scope, const char *indent) {
    jout("%s{ \"scope\": \"%s\"", indent, scope);
    if (r->cpu >= 0) jout(", \"cpu\": %d", r->cpu); else jout(", \"cpu\": null");
    j_num("mhz", r->mhz, 1);
    j_str("mhz_src", r->mhz > 0.0 ? mhz_src_name(r->mhz_src)
                                  : "unknown");
    j_num("int_lat_mops", r->int_lat, 1);
    j_num("int_thr_mops", r->int_thr, 1);
    j_num("ilp", r->ilp, 1);
    j_num("mul_thr_mmul_s", r->mul_thr, 1);
    j_num("fp_lat_mflops", r->fp_lat, 1);
    j_num("fp_thr_mflops", r->fp_thr, 1);
    j_num("filp", r->filp, 1);
    j_num("mem_gbps", r->mem_gbps, 1);
    j_num("mem_lat_ns", r->mem_lat_ns, 1);
    j_num("mem_lat8_ns", r->mem_lat8_ns, 1);
    j_num("mlp", r->mlp, 1);
    j_num("disp_thr_mcall_s", r->disp_thr, 1);
    j_num("disp_cap_calls", r->disp_cap, r->disp_status == DISP_OK);
    j_str("disp_prediction", r->disp_status == DISP_OK   ? "measured"
                          : r->disp_status == DISP_NONE ? "none" : "unknown");
    j_num("disp_gain", r->disp_gain, 1);
    j_num("disp_span", r->disp_span, r->disp_status != DISP_NA);
    j_num("score", r->score, 1);
    jout(" }");
}

// Opening half of the JSON document: schema, build, system and run config. The
// caller adds the result arrays and calls json_close().
static void json_open(const char *mode, const run_opts_t *o, int threads,
                      int pin) {
    char cc[64], models[512];
    compiler_string(cc, sizeof(cc));
    detect_cpu_models(models, sizeof(models));

    jout("{\n  \"schema\": \"cpu-bench/1\"");
    j_str("mode", mode);
    jout(",\n  \"build\": { \"compiler\": \"%s\"", cc);
    j_str("compiler_version", compiler_version());
    j_str("cc", BUILD_CC);
    j_str("flags", variant_flags(o->kernels));
    j_str("target", arch_string());
    // Omitted rather than null where /proc is not there to answer: a document
    // that carries the key with no value invites a hub to store an empty
    // digest and then match it against another empty one.
    const char *digest = binary_sha256();
    if (digest) j_str("binary_sha256", digest);
    jout(", \"vectorize\": %s, \"fma\": %s }",
         o->kernels->vectorize ? "true" : "false",
         o->kernels->fma ? "true" : "false");

    plat_sysinfo_t si;
    jout(",\n  \"system\": {");
    if (plat_sysinfo(&si) == 0) {
        jout(" \"sysname\": \"%s\"", si.sysname);
        j_str("release", si.release);
        j_str("machine", si.machine);
        jout(",");
    }
    jout(" \"cpus\": %d", get_cpu_count());
    if (models[0]) j_str("cpu_models", models);
    jout(" }");

    jout(",\n  \"config\": { \"threads\": %d, \"seconds_per_phase\": %g,"
           " \"reps\": %d, \"warmup_seconds\": %g, \"mem_bytes_per_thread\": %zu,"
         " \"pin\": %s, \"clock\": \"%s\", \"seed\": %llu }",
         threads, o->seconds, o->reps, o->warmup, o->mem_per_thread,
         pin ? "true" : "false", g_clock_raw ? "raw" : "mono",
         (unsigned long long)o->seed);
}

static void json_close(void) { jout("\n}\n"); }

// ---------------------------------------------------------------------------
// Phases
// ---------------------------------------------------------------------------

// Everything one invocation measured. --full carries both halves: the
// multi-threaded run says what the machine does at once, the per-core sweep says
// what each core type does on its own, and only the pair is worth comparing
// against another machine.
typedef struct {
    result_t *threads;      // one per thread of the multi-threaded run
    int       n_threads;
    result_t  total;        // their aggregate
    int       have_threads;

    result_t *cores;        // one per CPU of the per-core sweep
    int       n_cores;

    uint64_t  checksum;
    double    dram_lo, dram_hi;
} suite_t;

static void suite_free(suite_t *s) {
    free(s->threads);
    free(s->cores);
    s->threads = NULL;
    s->cores = NULL;
}

static void note_dram(suite_t *s, double mhz) {
    if (mhz <= 0.0) return;
    if (mhz > s->dram_hi) s->dram_hi = mhz;
    if (s->dram_lo == 0.0 || mhz < s->dram_lo) s->dram_lo = mhz;
}

// Aggregate the per-thread rows: rates sum, latencies and ratios average.
static void aggregate(const result_t *rows, int n, result_t *tot) {
    memset(tot, 0, sizeof(*tot));
    tot->cpu = -1;
    double lat_n = 0, mlp_n = 0, cap_n = 0, mhz_n = 0;
    int cap_none = 0;
    for (int i = 0; i < n; ++i) {
        const result_t *r = &rows[i];
        tot->int_lat   += r->int_lat;
        tot->int_thr   += r->int_thr;
        tot->mul_thr   += r->mul_thr;
        tot->fp_lat    += r->fp_lat;
        tot->fp_thr    += r->fp_thr;
        tot->mem_gbps  += r->mem_gbps;
        tot->disp_thr  += r->disp_thr;
        tot->disp_gain += r->disp_gain;
        tot->disp_span += r->disp_span;
        if (r->mhz > 0.0)         { tot->mhz += r->mhz; mhz_n++; }
        if (r->mhz_src > tot->mhz_src) tot->mhz_src = r->mhz_src;
        if (r->mem_lat_ns > 0.0)  { tot->mem_lat_ns += r->mem_lat_ns; lat_n++; }
        if (r->mem_lat8_ns > 0.0) { tot->mem_lat8_ns += r->mem_lat8_ns; mlp_n++; }
        if (r->disp_status == DISP_OK && r->disp_cap > 0.0) {
            tot->disp_cap += r->disp_cap;
            cap_n++;
        } else if (r->disp_status == DISP_NONE) {
            cap_none++;
        }
    }
    const double d = n > 0 ? (double)n : 1.0;
    if (mhz_n > 0) tot->mhz /= mhz_n;
    if (lat_n > 0) tot->mem_lat_ns /= lat_n;
    if (mlp_n > 0) tot->mem_lat8_ns /= mlp_n;
    if (cap_n > 0) tot->disp_cap /= cap_n;
    tot->disp_gain /= d;
    tot->disp_span /= d;
    tot->disp_status = cap_n > 0 ? DISP_OK : (cap_none > 0 ? DISP_NONE : DISP_NA);
    tot->ilp  = tot->int_lat > 0.0 ? tot->int_thr / tot->int_lat : 0.0;
    tot->filp = tot->fp_lat  > 0.0 ? tot->fp_thr  / tot->fp_lat  : 0.0;
    tot->mlp  = tot->mem_lat8_ns > 0.0 ? tot->mem_lat_ns / tot->mem_lat8_ns : 0.0;
    tot->score = machine_score(tot, n);   // last: reads the fields set above
}

static int collect_threads(suite_t *s, const run_opts_t *o, int threads, int pin,
                           const int *cpu_list, int n_cpus) {
    worker_t *w = (worker_t *)calloc((size_t)threads, sizeof(*w));
    result_t *res = (result_t *)calloc((size_t)threads, sizeof(*res));
    if (!w || !res) { perror("calloc"); free(w); free(res); return -1; }
    for (int i = 0; i < threads; ++i) w[i].cpu = pin ? cpu_list[i % n_cpus] : -1;

    if (run_workers(w, threads, o) != 0) { free(w); free(res); return -1; }

    for (int i = 0; i < threads; ++i) {
        res[i] = summarize(&w[i], w[i].cpu);
        note_dram(s, w[i].dram_mhz_observed);
        s->checksum ^= w[i].checksum;
    }
    aggregate(res, threads, &s->total);
    s->threads = res;
    s->n_threads = threads;
    s->have_threads = 1;
    free(w);
    return 0;
}

static void text_per_core_row(const result_t *r);

// One CPU at a time, so a heterogeneous machine reports each core type. In text
// mode rows print as they finish: the sweep is the long part of a --full run.
static int collect_per_core(suite_t *s, const run_opts_t *o, const int *cpu_list,
                            int n_cpus, int pin, int text_rows) {
    result_t *res = (result_t *)calloc((size_t)n_cpus, sizeof(*res));
    if (!res) { perror("calloc"); return -1; }
    for (int i = 0; i < n_cpus; ++i) {
        worker_t w;
        memset(&w, 0, sizeof(w));
        w.cpu = pin ? cpu_list[i] : -1;
        if (run_workers(&w, 1, o) != 0) { free(res); return -1; }
        res[i] = summarize(&w, cpu_list[i]);
        note_dram(s, w.dram_mhz_observed);
        s->checksum ^= w.checksum;
        if (text_rows) text_per_core_row(&res[i]);
    }
    s->cores = res;
    s->n_cores = n_cpus;
    return 0;
}

// ---------------------------------------------------------------------------
// Emitters
// ---------------------------------------------------------------------------

static void text_threads(const suite_t *s, size_t mem_per_thread) {
    const result_t *t = &s->total;
    const double n = s->n_threads > 0 ? (double)s->n_threads : 1.0;
    printf("%-9s %-23s %8s %11s %11s\n",
           "metric", "what it measures", "unit", "total", "per-thread");
    printf("%-9s %-23s %8s %11.1f %11.1f\n",
           "INT-lat", "integer latency", "Mop/s", t->int_lat, t->int_lat / n);
    printf("%-9s %-23s %8s %11.1f %11.1f\n",
           "INT-thr", "integer throughput", "Mop/s", t->int_thr, t->int_thr / n);
    printf("%-9s %-23s %8s %11.1f %11.1f\n",
           "MUL-thr", "multiply throughput", "Mmul/s", t->mul_thr, t->mul_thr / n);
    printf("%-9s %-23s %8s %11.1f %11.1f\n",
           "FP-lat", "FP latency", "Mflop/s", t->fp_lat, t->fp_lat / n);
    printf("%-9s %-23s %8s %11.1f %11.1f\n",
           "FP-thr", "FP throughput", "Mflop/s", t->fp_thr, t->fp_thr / n);
    printf("%-9s %-23s %8s %11.1f %11.1f\n",
           "DISP-thr", "indirect dispatch rate", "Mcall/s", t->disp_thr, t->disp_thr / n);
    if (t->disp_status == DISP_OK) {
        printf("%-9s %-23s %8s %11s %11.0f\n",
               "DISPcap", "indirect predictor size", "calls", "-", t->disp_cap);
    } else if (t->disp_status == DISP_NONE) {
        printf("%-9s %-23s %8s %11s %11s\n",
               "DISPcap", "indirect predictor size", "calls", "-", "none");
    }
    if (mem_per_thread > 0) {
        printf("%-9s %-23s %8s %11.2f %11.2f\n",
               "MEM", "memory bandwidth", "GB/s", t->mem_gbps, t->mem_gbps / n);
        if (t->mem_lat_ns > 0.0) {
            printf("%-9s %-23s %8s %11s %11.1f\n",
                   "MEMlat", "memory latency", "ns", "-", t->mem_lat_ns);
        }
        if (t->mem_lat8_ns > 0.0) {
            printf("%-9s %-23s %8s %11s %11.1f\n",
                   "MEMlat/8", "latency, 8 chases", "ns", "-", t->mem_lat8_ns);
        }
    }
    // Derived ratios: per-thread only, so the "total" column stays blank.
    printf("%-9s %-23s %8s %11s %11.2f\n", "ILP", "instruction parallelism",
           "x", "-", t->ilp);
    printf("%-9s %-23s %8s %11s %11.2f\n", "fILP", "FP ops in flight",
           "x", "-", t->filp);
    if (t->mlp > 0.0) {
        printf("%-9s %-23s %8s %11s %11.2f\n", "MLP", "memory parallelism",
               "x", "-", t->mlp);
    }
    // Whole-machine only: it is a geomean of the totals, not of the per-thread
    // column, and is ~n times what one core of this machine scores.
    if (t->score > 0.0) {
        printf("%-9s %-23s %8s %11.1f %11s\n", "score", "composite",
               "geomean", t->score, "-");
    }
}

#define PER_CORE_FMT "%4s %6s  %8s %8s %5s %8s  %8s %8s %5s  %7s %7s %5s  %8s %7s  %8s\n"

static void text_per_core_header(void) {
    printf(PER_CORE_FMT, "CPU", "MHz", "INT-lat", "INT-thr", "ILP", "MUL-thr",
           "FP-lat", "FP-thr", "fILP", "MEM", "MEMlat", "MLP",
           "DISP-thr", "DISPcap", "score");
    printf(PER_CORE_FMT, "", "", "Mop/s", "Mop/s", "x", "Mmul/s",
           "Mflop/s", "Mflop/s", "x", "GB/s", "ns", "x",
           "Mcall/s", "calls", "geomean");
}

static void text_per_core_row(const result_t *r) {
    char mhzbuf[16], capbuf[16];
    if (r->mhz <= 0.0)         snprintf(mhzbuf, sizeof(mhzbuf), "?");
    else if (r->mhz_src == MHZ_ESTIMATED)
        snprintf(mhzbuf, sizeof(mhzbuf), "~%.0f", r->mhz);
    else                       snprintf(mhzbuf, sizeof(mhzbuf), "%.0f", r->mhz);
    disp_cap_str(r->disp_status, r->disp_cap, capbuf, sizeof(capbuf));
    printf("%4d %6s  %8.1f %8.1f %5.2f %8.1f  %8.1f %8.1f %5.2f  %7.2f %7.1f %5.2f  %8.1f %7s  %8.1f\n",
           r->cpu, mhzbuf,
           r->int_lat, r->int_thr, r->ilp, r->mul_thr,
           r->fp_lat, r->fp_thr, r->filp,
           r->mem_gbps, r->mem_lat_ns, r->mlp,
           r->disp_thr, capbuf, r->score);
    fflush(stdout);
}

// Best/worst core of the sweep: on a heterogeneous machine this is the
// big/little ratio, and on a homogeneous one it is a noise estimate.
static double core_spread(const suite_t *s) {
    double best = 0.0, worst = 0.0;
    for (int i = 0; i < s->n_cores; ++i) {
        const double v = s->cores[i].score;
        if (v <= 0.0) continue;
        if (v > best) best = v;
        if (worst == 0.0 || v < worst) worst = v;
    }
    return worst > 0.0 ? best / worst : 0.0;
}

static void text_per_core_footer(const suite_t *s) {
    const double spread = core_spread(s);
    if (spread > 0.0) {
        fprintf(msg(), "\nfastest/slowest core ratio: %.2fx\n", spread);
    }
}

// `first` prints the header; a --variants run emits one block per variant under
// a single header, told apart by the leading column.
static void emit_tsv(const suite_t *s, const char *variant, int first) {
    if (first) printf(RESULT_COLUMNS);
    for (int i = 0; i < s->n_threads; ++i) {
        tsv_result(variant, "thread", &s->threads[i]);
    }
    if (s->have_threads) tsv_result(variant, "total", &s->total);
    for (int i = 0; i < s->n_cores; ++i) tsv_result(variant, "cpu", &s->cores[i]);
    if (s->have_threads) {
        fprintf(msg(), "checksum: 0x%016llx\n", (unsigned long long)s->checksum);
    }
}

static void emit_json(const suite_t *s, const char *mode, const run_opts_t *o,
                      int threads, int pin) {
    json_open(mode, o, threads, pin);
    if (s->dram_hi > 0.0) {
        jout(",\n  \"dram\": { \"name\": \"%s\", \"mhz_min\": %.6g,"
             " \"mhz_max\": %.6g }", g_dram_name, s->dram_lo, s->dram_hi);
    }
    if (s->have_threads) {
        jout(",\n  \"threads\": [\n");
        for (int i = 0; i < s->n_threads; ++i) {
            json_result(&s->threads[i], "thread", "    ");
            jout("%s\n", i + 1 < s->n_threads ? "," : "");
        }
        jout("  ],\n  \"total\": ");
        json_result(&s->total, "total", "");
    }
    if (s->n_cores > 0) {
        jout(",\n  \"cores\": [\n");
        for (int i = 0; i < s->n_cores; ++i) {
            json_result(&s->cores[i], "cpu", "    ");
            jout("%s\n", i + 1 < s->n_cores ? "," : "");
        }
        jout("  ]");
        const double spread = core_spread(s);
        if (spread > 0.0) jout(",\n  \"core_spread\": %.6g", spread);
    }
    jout(",\n  \"checksum\": \"0x%016llx\"", (unsigned long long)s->checksum);
    json_close();
}

// ---------------------------------------------------------------------------
// Uploading
// ---------------------------------------------------------------------------
//
// `--submit URL --token T` posts the same document `--json` prints, straight
// from the process that measured it. The upload is the last thing a run does
// and cannot fail it: the numbers are already on stdout by then, and a hub
// that is down is not a reason to have to measure again.
//
// What the hub keeps of a label and of the notes. It truncates the rest without
// saying so, so the trimming happens here instead -- where the cut can be
// mentioned, and where the variant tag can be kept rather than being the part
// that falls off the end. Bytes here against characters there, which is the
// safe direction to be wrong in: a label of non-ASCII gets fewer characters
// than the hub would have taken, and nothing is lost at the far end.
#define MAX_LABEL 200
#define MAX_NOTES 2000

static int         g_submit = 0;
static int         g_submit_errors = 0;     // uploads that did not land
static const char *g_hub    = NULL;
static const char *g_token  = NULL;
static const char *g_label  = NULL;
static const char *g_notes  = NULL;

// Percent-encode into `out`, keeping only the unreserved set. Everything else,
// including the bytes of a UTF-8 label, goes out as %XX -- so a label may say
// whatever it likes without ending the query string early. Returns 0 if it did
// not fit.
static int url_encode(const char *s, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        const int plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                          (*p >= '0' && *p <= '9') || strchr("-._~", (int)*p);
        if (n + (plain ? 1u : 3u) + 1u > cap) return 0;
        if (plain) {
            out[n++] = (char)*p;
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = '\0';
    return 1;
}

// Copy `s` into `out`, keeping at most cap-1 bytes and never cutting a UTF-8
// character in half -- half a character is not a shorter label, it is a broken
// one. Returns 1 when the whole string fitted.
static int copy_trimmed(char *out, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (n < cap) {
        memcpy(out, s, n + 1);
        return 1;
    }
    n = cap - 1;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) --n;  // continuations
    memcpy(out, s, n);
    out[n] = '\0';
    return 0;
}

// Print what a hub said back. Bytes from the network, not necessarily JSON and
// not necessarily printable: an escape sequence in a "reply" can repaint a
// terminal or scroll away what it just said, and this is the one thing in the
// upload path that puts a stranger's bytes on a screen. Control characters
// therefore go out as \xNN, anything above ASCII passes through so a hub may
// answer in its own language, and the whole thing is capped -- the useful part
// of an answer is its front.
static void print_reply(FILE *o, const char *body) {
    const size_t max = 1024;
    // The newline a reply may or may not end with is not information, and
    // escaping it would only make the ordinary case look like the hostile one.
    // Anything further in stays escaped: a line break in the middle of a reply
    // can forge an output line of its own.
    size_t len = strlen(body);
    while (len > 0 && strchr(" \t\r\n", body[len - 1])) --len;

    size_t i = 0;
    for (; i < len && i < max; ++i) {
        const unsigned char c = (unsigned char)body[i];
        if (c >= 0x20 && c != 0x7f) fputc((int)c, o);
        else                        fprintf(o, "\\x%02x", c);
    }
    if (i < len) fprintf(o, " ...");
}

// POST one result document. `variant` names the build it came from when a
// --variants run is uploading several, which is how the labels on the board
// stay told apart; the hub keys a run on the build flags in the document
// itself, so each variant is a run of its own.
static int submit_document(const char *doc, size_t len, const char *variant) {
    // Not msg(): an upload is a side effect, not part of the report, and its
    // outcome has to be visible when the report itself is being redirected --
    // `cpcpub --full --submit ... > run.txt` must not hide that nothing landed.
    // It also keeps the delete token out of a file that gets shared: it is the
    // credential that withdraws the run.
    FILE *o = stderr;

    // The tag goes on the end and the hub truncates from the end, so the room
    // it needs comes out of the label first. Four rows that all read "(scal"
    // are four rows nobody can tell apart, and on a --variants run the tag is
    // the only thing that distinguishes them.
    char tag[32] = "";
    if (variant && *variant) snprintf(tag, sizeof(tag), "(%s)", variant);
    const size_t reserve = tag[0] ? strlen(tag) + 1 : 0;   // and a space to join

    char label[MAX_LABEL + 1], notes[MAX_NOTES + 1];
    const int label_fit = copy_trimmed(label, sizeof(label) - reserve,
                                       g_label ? g_label : "");
    const int notes_fit = copy_trimmed(notes, sizeof(notes),
                                       g_notes ? g_notes : "");
    if (tag[0]) {
        size_t at = strlen(label);
        if (at) label[at++] = ' ';
        memcpy(label + at, tag, strlen(tag) + 1);
    }
    if (!label_fit)
        fprintf(o, "submit: label trimmed to %d bytes, which is what the hub "
                   "keeps\n", MAX_LABEL);
    if (!notes_fit)
        fprintf(o, "submit: notes trimmed to %d bytes, which is what the hub "
                   "keeps\n", MAX_NOTES);

    // 3 bytes per byte is the worst percent-encoding can do, so after the
    // trimming above this cannot fail. Checked rather than trusted, because the
    // failure it would otherwise be is a URL built past the end of a buffer.
    char enc_label[MAX_LABEL * 3 + 1], enc_notes[MAX_NOTES * 3 + 1];
    if (!url_encode(label, enc_label, sizeof(enc_label)) ||
        !url_encode(notes, enc_notes, sizeof(enc_notes))) {
        fprintf(o, "submit: label or notes too long\n");
        ++g_submit_errors;
        return -1;
    }

    size_t base_len = strlen(g_hub);
    while (base_len > 0 && g_hub[base_len - 1] == '/') --base_len;   // no //api
    const size_t cap = base_len + strlen(enc_label) + strlen(enc_notes) + 64;
    char *url = (char *)malloc(cap);
    if (!url) { fprintf(o, "submit: out of memory\n"); ++g_submit_errors; return -1; }
    int n = snprintf(url, cap, "%.*s/api/runs", (int)base_len, g_hub);
    if (enc_label[0]) n += snprintf(url + n, cap - (size_t)n, "?label=%s", enc_label);
    if (enc_notes[0]) n += snprintf(url + n, cap - (size_t)n, "%cnotes=%s",
                                    enc_label[0] ? '&' : '?', enc_notes);

    fprintf(o, "submitting %zu bytes to %.*s ...\n", len, (int)base_len, g_hub);
    http_reply_t reply;
    const int rc = http_post_json(url, g_token, doc, len, &reply);
    free(url);

    if (rc != 0) {
        fprintf(o, "submit failed: %s\n", reply.error);
        http_free(&reply);
        ++g_submit_errors;
        return -1;
    }
    if (reply.status >= 200 && reply.status < 300) {
        // The reply carries the run id, the delete token an anonymous upload
        // needs to withdraw itself later, and the page to look at. Printed
        // whole rather than picked apart: there is no JSON parser in here, and
        // the delete token is the one thing that cannot be recovered later.
        fprintf(o, "uploaded: ");
        print_reply(o, reply.body);
        fputc('\n', o);
    } else if (reply.status >= 300 && reply.status < 400 && reply.location[0]) {
        // Almost always an http:// address answering for an https:// hub. Not
        // followed: the second address may be a different transport, a
        // different host, or somewhere else entirely, and an upload carrying a
        // token should go where it was addressed.
        fprintf(o, "the hub redirected the upload (HTTP %d) to ", reply.status);
        print_reply(o, reply.location);
        fprintf(o, "\nsubmit to that address instead; this client does not "
                   "follow redirects\n");
    } else {
        fprintf(o, "hub refused the upload (HTTP %d): ", reply.status);
        print_reply(o, reply.body);
        fputc('\n', o);
    }
    const int ok = reply.status >= 200 && reply.status < 300;
    http_free(&reply);
    if (!ok) ++g_submit_errors;
    return ok ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Variant selection
// ---------------------------------------------------------------------------

static const variant_t *find_variant(const char *name) {
    for (int i = 0; i < N_VARIANTS; ++i) {
        if (!strcmp(name, g_variants[i].k->name)) return &g_variants[i];
    }
    return NULL;
}

static void list_variants(FILE *o) {
    fprintf(o, "build variants carried by this binary (--variant NAME):\n");
    for (int i = 0; i < N_VARIANTS; ++i) {
        const variant_t *v = &g_variants[i];
        fprintf(o, "  %-14s %-38s %s\n", v->k->name, v->k->flags,
                v->distinct ? "distinct on this target"
                            : "same code as the baseline here");
    }
    if (!TARGET_HAS_SIMD) {
        fprintf(o, "  (the -march this binary was built for has no vector unit,"
                   " so -ftree-vectorize\n   has nothing to emit)\n");
    }
    if (!TARGET_HAS_FMA) {
        fprintf(o, "  (the -march this binary was built for has no hardware"
                   " fused multiply-add, so\n   -ffp-contract=fast has nothing"
                   " to contract)\n");
    }
    fprintf(o, "--variants runs every distinct one in turn; --variants=all "
               "runs all %d.\n", N_VARIANTS);
}

// Fill `sel` from a --variants list. NULL or "distinct" selects every variant
// the toggles can actually change on this target; "all" selects all four;
// otherwise a comma-separated list of names, in the order given. Returns the
// count, or -1 on an unknown name.
static int select_variants(const char *spec, const variant_t **sel) {
    int n = 0;
    if (!spec || !strcmp(spec, "distinct")) {
        for (int i = 0; i < N_VARIANTS; ++i) {
            if (g_variants[i].distinct) sel[n++] = &g_variants[i];
        }
        return n;
    }
    if (!strcmp(spec, "all")) {
        for (int i = 0; i < N_VARIANTS; ++i) sel[n++] = &g_variants[i];
        return n;
    }
    const char *p = spec;
    while (*p) {
        char name[32];
        const char *comma = strchr(p, ',');
        const size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == 0 || len >= sizeof(name)) return -1;
        memcpy(name, p, len);
        name[len] = '\0';
        const variant_t *v = find_variant(name);
        if (!v || n >= N_VARIANTS) return -1;
        sel[n++] = v;
        p = comma ? comma + 1 : p + len;
    }
    return n;
}

// ---------------------------------------------------------------------------
// One measured pass
// ---------------------------------------------------------------------------

// Everything about the run that does not change between variants.
typedef struct {
    int    threads;
    int    pin;
    const int *cpu_list;
    int    n_cpus;
    size_t mem_mt;          // per thread, multi-threaded phase
    size_t mem_pc;          // per thread, per-core sweep
    int    want_threads;
    int    want_cores;
    const char *mode;
    double duration, warmup;
    int    reps;
} run_cfg_t;

// Run the suite once with one variant and emit it. Text streams as it measures;
// json and tsv write exactly what a single-variant invocation would, so a
// --variants run produces the same documents as a sequence of separate runs
// (wrapped in a JSON array, and told apart by the tsv `variant` column).
//
// `headline` receives the row the comparison table ranks: the whole-machine
// total where there is one, otherwise the fastest core of the sweep.
static int run_variant(const run_cfg_t *c, run_opts_t *opts, int idx, int n_sel,
                       result_t *headline) {
    suite_t s;
    memset(&s, 0, sizeof(s));

    print_variant(opts->kernels, idx, n_sel);

    if (c->want_threads) {
        fprintf(msg(), "config: threads=%d time=%.2fs/phase x%d reps warmup=%.2fs "
                       "mem=%zu MiB/thread pin=%s\n\n",
                c->threads, c->duration, c->reps, c->warmup, c->mem_mt >> 20,
                c->pin ? "on" : "off");
        opts->mem_per_thread = c->mem_mt;
        if (collect_threads(&s, opts, c->threads, c->pin, c->cpu_list,
                            c->n_cpus) != 0) {
            suite_free(&s);
            return -1;
        }
        if (g_format == FMT_TEXT) text_threads(&s, c->mem_mt);
    }

    if (c->want_cores) {
        fprintf(msg(), "%smode: per-core sweep over %d CPUs, %.2fs/phase x %d reps "
                       "(best kept),\n      warmup %.2fs, mem %zu MiB/thread\n\n",
                c->want_threads ? "\n" : "", c->n_cpus, c->duration, c->reps,
                c->warmup, c->mem_pc >> 20);
        opts->mem_per_thread = c->mem_pc;
        if (g_format == FMT_TEXT) text_per_core_header();
        if (collect_per_core(&s, opts, c->cpu_list, c->n_cpus, c->pin,
                             g_format == FMT_TEXT) != 0) {
            suite_free(&s);
            return -1;
        }
        if (g_format == FMT_TEXT) text_per_core_footer(&s);
    }

    if (g_format == FMT_TSV) emit_tsv(&s, opts->kernels->name, idx == 0);
    else if (g_format == FMT_TEXT && s.have_threads) {
        printf("\nchecksum: 0x%016llx\n", (unsigned long long)s.checksum);
    }

    // The document, for whichever of the two asked for it -- and an upload
    // happens whatever the output format, because what is printed here and
    // what goes to a hub are two separate questions.
    if (g_format == FMT_JSON || g_submit) {
        // An upload sends one document per variant rather than the array
        // --variants prints, because the hub stores one run per build.
        sbuf_t doc = { NULL, 0, 0, 0 };
        g_json_sink = &doc;
        emit_json(&s, c->mode, opts, c->threads, c->pin);
        g_json_sink = NULL;
        if (doc.oom) {
            fprintf(msg(), "out of memory building the result document\n");
            sbuf_free(&doc);
            suite_free(&s);
            return -1;
        }
        if (g_format == FMT_JSON) {
            if (n_sel > 1) printf(idx == 0 ? "[\n" : ",\n");
            fwrite(doc.p, 1, doc.len, stdout);
            if (n_sel > 1 && idx + 1 == n_sel) printf("]\n");
            fflush(stdout);
        }
        if (g_submit) {
            submit_document(doc.p, doc.len, n_sel > 1 ? opts->kernels->name : NULL);
        }
        sbuf_free(&doc);
    }

    if (g_format != FMT_JSON) report_dram(s.dram_lo, s.dram_hi);

    memset(headline, 0, sizeof(*headline));
    if (s.have_threads) {
        *headline = s.total;
    } else {
        for (int i = 0; i < s.n_cores; ++i) {
            if (s.cores[i].score > headline->score) *headline = s.cores[i];
        }
    }

    suite_free(&s);
    return 0;
}

// What changed between the variants, side by side. Only the columns the two
// toggles can move are worth a column: the integer and dispatch phases are here
// because vectorization can reach the integer kernels too, and MEM because the
// bandwidth sweep vectorizes on any target with a vector unit.
static void text_variant_table(const variant_t **sel, const result_t *rows,
                               int n, int whole_machine) {
    printf("\n%s across variants  (%s)\n",
           whole_machine ? "whole machine" : "fastest core",
           whole_machine ? "the total row" : "best row of the per-core sweep");
    printf("%-14s %9s %9s %9s %9s %8s %9s %7s\n", "variant",
           "INT-thr", "MUL-thr", "FP-lat", "FP-thr", "MEM", "score", "vs base");
    printf("%-14s %9s %9s %9s %9s %8s %9s %7s\n", "",
           "Mop/s", "Mmul/s", "Mflop/s", "Mflop/s", "GB/s", "geomean", "x");
    const double base = rows[0].score;
    for (int i = 0; i < n; ++i) {
        const result_t *r = &rows[i];
        printf("%-14s %9.1f %9.1f %9.1f %9.1f %8.2f %9.1f", sel[i]->k->name,
               r->int_thr, r->mul_thr, r->fp_lat, r->fp_thr, r->mem_gbps,
               r->score);
        if (base > 0.0 && r->score > 0.0) printf(" %7.3f\n", r->score / base);
        else                              printf(" %7s\n", "-");
    }
    if (g_verbose) {
        printf("\n  FP-lat is the column FMA contraction is meant to move: one\n"
               "  multiply-add per iteration becomes one instruction, so the\n"
               "  dependent chain shortens by a multiply's latency. Vectorization\n"
               "  moves the throughput columns instead, and only where the ISA has\n"
               "  a vector unit to move them with.\n"
               "  Compare variants within one machine. Across machines, compare\n"
               "  like with like: a vector-fma number against another vector-fma\n"
               "  number, never against a baseline one.\n");
    }
}

int main(int argc, char **argv) {
    int threads = get_cpu_count();
    int cpu_list[1024];
    int n_cpus = 0;
    int per_core = 0;
    int full = 0;
    int disp_sweep = 0;
    int pin = 1;
    int use_clock_raw = 1;
    double duration = 0.5;
    double warmup = 0.15;
    int reps = 3;
    size_t mem_per_thread = (size_t)16 << 20;
    size_t mem_total = 0;
    int mem_explicit = 0;
    uint64_t seed = 1;
    const variant_t *sel[N_VARIANTS] = { &g_variants[VARIANT_DEFAULT] };
    int n_sel = 1;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--variant") && i + 1 < argc) {
            const variant_t *v = find_variant(argv[++i]);
            if (!v) {
                fprintf(stderr, "unknown --variant '%s'\n\n", argv[i]);
                list_variants(stderr);
                return 2;
            }
            sel[0] = v;
            n_sel = 1;
        } else if (!strcmp(a, "--variants")) {
            n_sel = select_variants(NULL, sel);
        } else if (!strncmp(a, "--variants=", 11)) {
            n_sel = select_variants(a + 11, sel);
            if (n_sel <= 0) {
                fprintf(stderr, "bad --variants list '%s'\n\n", a + 11);
                list_variants(stderr);
                return 2;
            }
        } else if (!strcmp(a, "--list-variants")) {
            list_variants(stdout);
            return 0;
        } else if (!strcmp(a, "--threads") && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (!strcmp(a, "--cpus") && i + 1 < argc) {
            n_cpus = parse_cpu_list(argv[++i], cpu_list, ARRAY_LEN(cpu_list));
            if (n_cpus <= 0) { fprintf(stderr, "bad --cpus list\n"); return 2; }
        } else if (!strcmp(a, "--per-core")) {
            per_core = 1;
        } else if (!strcmp(a, "--full")) {
            full = 1;
        } else if (!strcmp(a, "--disp-sweep")) {
            disp_sweep = 1;
        } else if (!strcmp(a, "--time") && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else if (!strcmp(a, "--reps") && i + 1 < argc) {
            reps = atoi(argv[++i]);
        } else if (!strcmp(a, "--warmup") && i + 1 < argc) {
            warmup = atof(argv[++i]);
        } else if (!strcmp(a, "--mem-per-thread") && i + 1 < argc) {
            mem_per_thread = (size_t)strtoull(argv[++i], NULL, 0);
            mem_explicit = 1;
        } else if (!strcmp(a, "--mem") && i + 1 < argc) {
            mem_total = (size_t)strtoull(argv[++i], NULL, 0);
            mem_explicit = 1;
        } else if (!strcmp(a, "--no-mem")) {
            mem_per_thread = 0;
            mem_total = 0;
            mem_explicit = 1;
        } else if (!strcmp(a, "--no-pin")) {
            pin = 0;
        } else if (!strcmp(a, "--clock") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "mono")) use_clock_raw = 0;
            else if (!strcmp(m, "raw")) use_clock_raw = 1;
            else { fprintf(stderr, "bad --clock\n"); return 2; }
        } else if (!strcmp(a, "--mhz") && i + 1 < argc) {
            g_mhz_given = atof(argv[++i]);
            if (g_mhz_given < 0.0) g_mhz_given = 0.0;
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--format") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "text"))      g_format = FMT_TEXT;
            else if (!strcmp(m, "json")) g_format = FMT_JSON;
            else if (!strcmp(m, "tsv"))  g_format = FMT_TSV;
            else { fprintf(stderr, "bad --format\n"); return 2; }
        } else if (!strcmp(a, "--json")) {
            g_format = FMT_JSON;
        } else if (!strcmp(a, "--tsv")) {
            g_format = FMT_TSV;
        } else if (!strcmp(a, "--submit")) {
            g_submit = 1;
            // The URL is optional so a build with a default hub needs only the
            // flag. A following word is taken as the URL unless it is another
            // option, which is what `--submit --token X` has to mean.
            if (i + 1 < argc && argv[i + 1][0] != '-') g_hub = argv[++i];
        } else if (!strncmp(a, "--submit=", 9)) {
            g_submit = 1;
            g_hub = a + 9;
        } else if (!strcmp(a, "--token") && i + 1 < argc) {
            g_token = argv[++i];
        } else if (!strcmp(a, "--label") && i + 1 < argc) {
            g_label = argv[++i];
        } else if (!strcmp(a, "--notes") && i + 1 < argc) {
            g_notes = argv[++i];
        } else if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) {
            g_verbose = 1;
        } else if (!strcmp(a, "--version")) {
            print_version();
            return 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (duration < 0.05) duration = 0.05;
    if (warmup < 0.0) warmup = 0.0;
    if (threads < 1) threads = 1;
    if (reps < 1) reps = 1;

    // Where the upload goes and who it is from: the flag, then the
    // environment, then whatever this build was compiled to default to.
    // Settled here rather than at upload time so a missing address is an error
    // before the machine spends a minute measuring.
    if (g_submit) {
        if (!g_hub || !*g_hub)     g_hub = getenv("CPCPUB_HUB");
        if (!g_hub || !*g_hub)     g_hub = CPCPUB_HUB_URL;
        if (!g_token || !*g_token) g_token = getenv("CPCPUB_TOKEN");
        if (!*g_hub) {
            fprintf(stderr, "--submit needs a hub address: "
                            "--submit http://my.server:8782\n"
                            "(or $CPCPUB_HUB; this build has no default hub)\n");
            return 2;
        }
        if (strncmp(g_hub, "http://", 7) && strncmp(g_hub, "https://", 8)) {
            fprintf(stderr, "--submit takes a hub URL: '%s' has no "
                            "http:// or https:// on it\n", g_hub);
            return 2;
        }
        if (disp_sweep) {
            fprintf(stderr, "--disp-sweep produces a diagnostic curve, not a "
                            "result a hub can compare; drop --submit\n");
            return 2;
        }
    }

    // Pinning is not portable and is not optional to be honest about. macOS
    // gives no way to bind a thread to a numbered CPU -- placement is the
    // scheduler's -- so `pin` is turned off here rather than reported as on,
    // and `config.pin` in the result document says what actually happened. A
    // reader comparing a Mac against a pinned Linux box needs to see that
    // difference; a per-core sweep without pinning is measuring whichever core
    // the scheduler chose, repeatedly.
    // #if rather than `if (pin && !PLAT_HAS_AFFINITY)`: the capability is a
    // compile-time constant, and a compiler that says so (-Wconstant-logical-
    // operand) is right to.
#if !PLAT_HAS_AFFINITY
    if (pin) {
        pin = 0;
        fprintf(msg(), "NOTE: this system does not support pinning a thread to "
                       "a CPU, so threads run wherever the scheduler puts "
                       "them%s.\n",
                (per_core || full) ? " -- per-core figures identify a request, "
                                     "not a core" : "");
    }
#endif

    // --disp-sweep prints a curve per CPU, not a summary row, so there is
    // nothing to stack variants into; ask for one explicitly instead.
    if (disp_sweep && n_sel > 1) {
        fprintf(stderr, "--disp-sweep measures one variant; use --variant NAME\n");
        return 2;
    }

    // Default CPU list: 0..threads-1, or every online CPU in --per-core mode.
    if (n_cpus == 0) {
        const int online = get_cpu_count();
        const int want = (per_core || full || disp_sweep) ? online : threads;
        for (int i = 0; i < want && i < ARRAY_LEN(cpu_list); ++i) cpu_list[i] = i;
        n_cpus = want < ARRAY_LEN(cpu_list) ? want : ARRAY_LEN(cpu_list);
    }
    set_clock_mode(use_clock_raw);
    find_dram_devfreq();
    print_platform();
    if (n_sel > 1) {
        fprintf(msg(), "variants: %d in turn, so the run takes %dx as long:",
                n_sel, n_sel);
        for (int i = 0; i < n_sel; ++i) {
            fprintf(msg(), "%s %s", i ? "," : "", sel[i]->k->name);
        }
        fprintf(msg(), "\n");
    }

    // Size the default working set from the cache the machine actually has: a
    // fixed default silently measures cache rather than DRAM on anything with a
    // large last-level cache. The two phases need different sizes -- N threads
    // share the cache, one core has it to itself -- so --full sizes each of
    // them separately rather than running one of the two mis-sized.
    size_t mem_mt = mem_per_thread;    // multi-threaded run, per thread
    size_t mem_pc = mem_per_thread;    // per-core sweep, single thread
    if (!mem_explicit) {
        const size_t llc = detect_llc_bytes();
        if (llc > 0) {
            const int eff = threads > 0 ? threads : 1;
            const size_t want_mt = (llc * 4) / (size_t)eff;
            if (want_mt > mem_mt) mem_mt = want_mt;
            if (llc * 4 > mem_pc) mem_pc = llc * 4;
        }
    }
    if (mem_total > 0) {
        mem_mt = mem_total / (size_t)(threads > 0 ? threads : 1);
        mem_pc = mem_total;
    }
    if (mem_mt > 0 && mem_mt < (size_t)(1 << 20)) mem_mt = (size_t)(1 << 20);
    if (mem_pc > 0 && mem_pc < (size_t)(1 << 20)) mem_pc = (size_t)(1 << 20);
    // Round down to a whole number of 4 KiB pages.
    mem_mt &= ~(size_t)4095;
    mem_pc &= ~(size_t)4095;

    run_opts_t opts = {
        .seconds = duration,
        .warmup = warmup,
        .reps = reps,
        .mem_per_thread = mem_mt,
        .seed = seed,
        .kernels = sel[0]->k,
    };

    warn_if_loaded(threads);
    warn_if_working_set_small(per_core ? mem_pc : mem_mt, threads, per_core);

    // -----------------------------------------------------------------------
    // Selector-period sweep (diagnostic)
    // -----------------------------------------------------------------------
    if (disp_sweep) {
        opts.mem_per_thread = 0;
        worker_t *sw = (worker_t *)calloc((size_t)n_cpus, sizeof(*sw));
        if (!sw) { perror("calloc"); return 1; }
        print_variant(opts.kernels, 0, 1);
        fprintf(msg(), "mode: indirect-dispatch selector-period sweep over %d "
                       "CPUs,\n      %.2fs/phase x %d reps (best kept), "
                       "warmup %.2fs\n\n", n_cpus, duration, reps, warmup);
        for (int i = 0; i < n_cpus; ++i) {
            sw[i].cpu = pin ? cpu_list[i] : -1;
            sw[i].disp_sweep = 1;
            if (run_workers(&sw[i], 1, &opts) != 0) { free(sw); return 1; }
            fprintf(stderr, "cpu %d done\n", cpu_list[i]);
        }

        double mhz[1024];
        for (int i = 0; i < n_cpus; ++i) {
            double m = sw[i].mhz_observed;
            if (m <= 0.0) m = cpu_max_mhz(cpu_list[i]);
            if (m <= 0.0) m = sw[i].int_lat_mops;   // 1 op/cycle by construction
            mhz[i] = m;
        }

        if (g_format == FMT_TSV) {
            printf("cpu\tmhz\tperiod\tserial_mcall_s\tfree_mcall_s\n");
            for (int i = 0; i < n_cpus; ++i) {
                for (int k = 0; k < DISP_SWEEP_N; ++k) {
                    printf("%d\t%.6g\t%zu\t%.6g\t%.6g\n", cpu_list[i], mhz[i],
                           g_disp_periods[k], sw[i].sweep_serial[k],
                           sw[i].sweep_free[k]);
                }
            }
        } else if (g_format == FMT_JSON) {
            json_open("disp-sweep", &opts, 1, pin);
            printf(",\n  \"sweep\": [\n");
            for (int i = 0; i < n_cpus; ++i) {
                printf("    { \"cpu\": %d, \"mhz\": %.6g, \"points\": [\n",
                       cpu_list[i], mhz[i]);
                for (int k = 0; k < DISP_SWEEP_N; ++k) {
                    printf("      { \"period\": %zu, \"serial_mcall_s\": %.6g,"
                           " \"free_mcall_s\": %.6g }%s\n",
                           g_disp_periods[k], sw[i].sweep_serial[k],
                           sw[i].sweep_free[k],
                           k + 1 < DISP_SWEEP_N ? "," : "");
                }
                printf("    ] }%s\n", i + 1 < n_cpus ? "," : "");
            }
            printf("  ]");
            json_close();
        } else {
            // Two tables: the dependent-accumulator kernel the suite uses, and
            // the independent-call variant, which shows whether that serial
            // chain caps the measurement.
            for (int chain = 0; chain < 2; ++chain) {
                printf("\n%s: calls per 1000 cycles (Mcall/s in parentheses)\n",
                       chain == 0 ? "serial acc (disp_kernel)"
                                  : "independent calls (disp_kernel_free)");
                printf("%8s", "period");
                for (int i = 0; i < n_cpus; ++i) printf("  %14s%d", "cpu", cpu_list[i]);
                printf("\n");
                for (int k = 0; k < DISP_SWEEP_N; ++k) {
                    printf("%8zu", g_disp_periods[k]);
                    for (int i = 0; i < n_cpus; ++i) {
                        const double r = chain == 0 ? sw[i].sweep_serial[k]
                                                    : sw[i].sweep_free[k];
                        const double kc = mhz[i] > 0.0 ? r * 1000.0 / mhz[i] : 0.0;
                        printf("  %7.2f (%6.1f)", kc, r);
                    }
                    printf("\n");
                }
            }
        }
        if (g_verbose) {
            fprintf(msg(),
                "\nperiod = length of the repeating selector sequence, in calls.\n"
                "The largest period a core still predicts well is its indirect\n"
                "predictor's usable capacity; the last row is the unpredictable "
                "floor.\n");
        }
        free(sw);
        return 0;
    }

    // -----------------------------------------------------------------------
    // Measured phases, once per selected variant
    // -----------------------------------------------------------------------
    const run_cfg_t cfg = {
        .threads      = threads,
        .pin          = pin,
        .cpu_list     = cpu_list,
        .n_cpus       = n_cpus,
        .mem_mt       = mem_mt,
        .mem_pc       = mem_pc,
        .want_threads = !per_core,             // --full implies both
        .want_cores   = per_core || full,
        .mode         = full ? "full" : (per_core ? "per-core" : "threads"),
        .duration     = duration,
        .warmup       = warmup,
        .reps         = reps,
    };

    result_t headline[N_VARIANTS];
    for (int i = 0; i < n_sel; ++i) {
        opts.kernels = sel[i]->k;
        if (run_variant(&cfg, &opts, i, n_sel, &headline[i]) != 0) return 1;
    }

    if (n_sel > 1 && g_format == FMT_TEXT) {
        text_variant_table(sel, headline, n_sel, cfg.want_threads);
    }
    if (cfg.want_cores) print_legend();
    else                print_legend_ratios();

    // The measurement is what this program is for, so a run that measured and
    // failed to upload still printed its result -- but it says so in the exit
    // status, because a script that submits wants to know.
    return g_submit_errors > 0 ? 1 : 0;
}
