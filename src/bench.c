// Portable CPU benchmark (C)
// - Separates *latency* (one dependency chain) from *throughput* (independent
//   chains), so wide out-of-order cores are not scored like in-order ones
// - Integer, double-precision FP, memory bandwidth and memory latency
// - Multi-threaded using pthreads (no external deps besides libc/pthreads)
// - Targets 64-bit: amd64, arm64 (ARMv8-A), riscv64
// - Avoids arch-specific intrinsics for portability
// - Deterministic workloads, numerically bounded (no Inf/NaN/denormal drift)
//
// Design notes (why the loops look like this):
//   * No `volatile` in a hot loop. A volatile accumulator forces a store+reload
//     of every intermediate value, which turns any kernel into a measurement of
//     store-to-load forwarding latency. That is roughly constant across core
//     widths, so it hides the advantage of a wide core. Dead-code elimination is
//     prevented instead by feeding final state into a checksum the caller reads.
//   * Every kernel exists in a LAT variant (1 chain) and a THR variant (LANES
//     independent chains) built from the *same* op sequence. thr/lat is then a
//     direct measure of how much instruction-level parallelism the core extracts.
//   * FP state must stay in normal range forever. A 2D rotation does that: it is
//     norm-preserving up to rounding, so it can never reach Inf, NaN or denormal.

#define _GNU_SOURCE 1
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/utsname.h>
#include <dirent.h>
#ifdef __linux__
#include <sched.h>
#endif

#ifndef ARRAY_LEN
#define ARRAY_LEN(x) ((int)(sizeof(x) / sizeof((x)[0])))
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

// Number of independent chains in the throughput kernels. 8 is enough to
// saturate every core we target (widest is ~8 integer ports) while still
// fitting comfortably in the register file of a 31-GPR / 32-FPR ISA.
#define LANES 8

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

static clockid_t g_clock_id = CLOCK_MONOTONIC;

static void set_clock_mode(int use_raw) {
#ifdef CLOCK_MONOTONIC_RAW
    g_clock_id = use_raw ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC;
#else
    (void)use_raw;
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
// Kernels
// ---------------------------------------------------------------------------
//
// One integer step: multiply, add, two shift+xor, one shift+add.
// Counted honestly as 8 dependent integer ALU ops.
#define INT_STEP(a)                                   \
    do {                                              \
        (a) = (a) * UINT64_C(6364136223846793005)     \
              + UINT64_C(1442695040888963407);        \
        (a) ^= (a) >> 29;                             \
        (a) += (a) << 13;                             \
        (a) ^= (a) >> 17;                             \
    } while (0)
#define INT_OPS_PER_STEP 8u

// One FP step: 2D rotation by a fixed angle. 4 multiplies + 2 adds = 6 FLOPs.
// |(x,y)| is preserved to within rounding, so the state can never overflow to
// Inf, collapse to a denormal, or become NaN, at any iteration count.
#define FP_STEP(x, y, c, s)                           \
    do {                                              \
        double nx_ = (x) * (c) - (y) * (s);           \
        double ny_ = (x) * (s) + (y) * (c);           \
        (x) = nx_;                                    \
        (y) = ny_;                                    \
    } while (0)
#define FP_OPS_PER_STEP 6u

typedef struct { uint64_t l[LANES]; } int_ctx_t;
typedef struct { double x[LANES], y[LANES], c, s; } fp_ctx_t;

typedef struct {
    uint64_t *buf;
    size_t    nqwords;
    uint64_t  acc[4];
} mem_ctx_t;

// CHASE_WAYS independent pointer-chase cycles. Chasing one cycle measures raw
// random-access latency; chasing all of them in lockstep measures how much of
// that latency the core can overlap. An in-order core stalls on the first miss
// and scores near 1x; a deep out-of-order core hides most of it. This is the
// single biggest reason a big core beats a little one on real code, and a
// register-resident ALU loop misses it entirely.
//
// The reported speedup can legitimately exceed CHASE_WAYS: over a working set
// this large, the serial chase also serializes TLB page-table walks, which the
// parallel chase overlaps as well. That is a genuine microarchitectural
// advantage, so it is reported as a speedup rather than as a count of misses.
#define CHASE_WAYS 8

typedef struct {
    void   **cur[CHASE_WAYS];
} chase_ctx_t;

// Indirect dispatch through a table of tiny functions selected by random data.
// Cannot be if-converted or devirtualized, so it reliably exercises the
// indirect branch predictor and the front end -- another big-core strength.
typedef uint64_t (*op_fn)(uint64_t);

typedef struct {
    const uint8_t *idx;
    size_t         mask;
    size_t         pos;
    uint64_t       acc;
} disp_ctx_t;

// A kernel runs `n` units of work and returns; the driver times batches of them.
typedef void (*kernel_fn)(void *ctx, uint64_t n);

NOINLINE static void int_kernel_lat(void *vctx, uint64_t n) {
    int_ctx_t *c = (int_ctx_t *)vctx;
    uint64_t a0 = c->l[0];
    for (uint64_t i = 0; i < n; ++i) {
        INT_STEP(a0);
    }
    c->l[0] = a0;
}

NOINLINE static void int_kernel_thr(void *vctx, uint64_t n) {
    int_ctx_t *c = (int_ctx_t *)vctx;
    uint64_t a0 = c->l[0], a1 = c->l[1], a2 = c->l[2], a3 = c->l[3];
    uint64_t a4 = c->l[4], a5 = c->l[5], a6 = c->l[6], a7 = c->l[7];
    for (uint64_t i = 0; i < n; ++i) {
        INT_STEP(a0); INT_STEP(a1); INT_STEP(a2); INT_STEP(a3);
        INT_STEP(a4); INT_STEP(a5); INT_STEP(a6); INT_STEP(a7);
    }
    c->l[0] = a0; c->l[1] = a1; c->l[2] = a2; c->l[3] = a3;
    c->l[4] = a4; c->l[5] = a5; c->l[6] = a6; c->l[7] = a7;
}

NOINLINE static void fp_kernel_lat(void *vctx, uint64_t n) {
    fp_ctx_t *f = (fp_ctx_t *)vctx;
    const double co = f->c, si = f->s;
    double x0 = f->x[0], y0 = f->y[0];
    for (uint64_t i = 0; i < n; ++i) {
        FP_STEP(x0, y0, co, si);
    }
    f->x[0] = x0; f->y[0] = y0;
}

NOINLINE static void fp_kernel_thr(void *vctx, uint64_t n) {
    fp_ctx_t *f = (fp_ctx_t *)vctx;
    const double co = f->c, si = f->s;
    double x0 = f->x[0], x1 = f->x[1], x2 = f->x[2], x3 = f->x[3];
    double x4 = f->x[4], x5 = f->x[5], x6 = f->x[6], x7 = f->x[7];
    double y0 = f->y[0], y1 = f->y[1], y2 = f->y[2], y3 = f->y[3];
    double y4 = f->y[4], y5 = f->y[5], y6 = f->y[6], y7 = f->y[7];
    for (uint64_t i = 0; i < n; ++i) {
        FP_STEP(x0, y0, co, si); FP_STEP(x1, y1, co, si);
        FP_STEP(x2, y2, co, si); FP_STEP(x3, y3, co, si);
        FP_STEP(x4, y4, co, si); FP_STEP(x5, y5, co, si);
        FP_STEP(x6, y6, co, si); FP_STEP(x7, y7, co, si);
    }
    f->x[0] = x0; f->x[1] = x1; f->x[2] = x2; f->x[3] = x3;
    f->x[4] = x4; f->x[5] = x5; f->x[6] = x6; f->x[7] = x7;
    f->y[0] = y0; f->y[1] = y1; f->y[2] = y2; f->y[3] = y3;
    f->y[4] = y4; f->y[5] = y5; f->y[6] = y6; f->y[7] = y7;
}

// One unit = one full read+write sweep of the buffer.
NOINLINE static void mem_kernel_bw(void *vctx, uint64_t n) {
    mem_ctx_t *m = (mem_ctx_t *)vctx;
    uint64_t *p = m->buf;
    const size_t nq = m->nqwords;
    uint64_t a0 = m->acc[0], a1 = m->acc[1], a2 = m->acc[2], a3 = m->acc[3];
    for (uint64_t pass = 0; pass < n; ++pass) {
        // Four independent accumulators so the sweep is bandwidth-bound rather
        // than bound by a serial add chain.
        for (size_t i = 0; i + 3 < nq; i += 4) {
            uint64_t v0 = p[i + 0], v1 = p[i + 1];
            uint64_t v2 = p[i + 2], v3 = p[i + 3];
            a0 += v0; a1 += v1; a2 += v2; a3 += v3;
            p[i + 0] = v0 + 1u; p[i + 1] = v1 + 1u;
            p[i + 2] = v2 + 1u; p[i + 3] = v3 + 1u;
        }
    }
    m->acc[0] = a0; m->acc[1] = a1; m->acc[2] = a2; m->acc[3] = a3;
}

// One unit = one dependent load. No compiler transform can break this chain.
NOINLINE static void mem_kernel_lat(void *vctx, uint64_t n) {
    chase_ctx_t *ch = (chase_ctx_t *)vctx;
    void **p = ch->cur[0];
    for (uint64_t i = 0; i < n; ++i) {
        p = (void **)*p;
    }
    ch->cur[0] = p;
}

// One unit = CHASE_WAYS independent dependent loads issued together.
NOINLINE static void mem_kernel_mlp(void *vctx, uint64_t n) {
    chase_ctx_t *ch = (chase_ctx_t *)vctx;
    void **p0 = ch->cur[0], **p1 = ch->cur[1], **p2 = ch->cur[2], **p3 = ch->cur[3];
    void **p4 = ch->cur[4], **p5 = ch->cur[5], **p6 = ch->cur[6], **p7 = ch->cur[7];
    for (uint64_t i = 0; i < n; ++i) {
        p0 = (void **)*p0; p1 = (void **)*p1;
        p2 = (void **)*p2; p3 = (void **)*p3;
        p4 = (void **)*p4; p5 = (void **)*p5;
        p6 = (void **)*p6; p7 = (void **)*p7;
    }
    ch->cur[0] = p0; ch->cur[1] = p1; ch->cur[2] = p2; ch->cur[3] = p3;
    ch->cur[4] = p4; ch->cur[5] = p5; ch->cur[6] = p6; ch->cur[7] = p7;
}

// Four tiny leaf operations reached only through a runtime function pointer.
NOINLINE static uint64_t op_add(uint64_t v) { return v + UINT64_C(0x9E3779B9); }
NOINLINE static uint64_t op_xor(uint64_t v) { return v ^ (v >> 7); }
NOINLINE static uint64_t op_mul(uint64_t v) { return v * UINT64_C(2654435761); }
NOINLINE static uint64_t op_rot(uint64_t v) { return (v << 13) | (v >> 51); }

// Non-const and externally visible so the compiler cannot fold the indirect
// call back into a direct one.
op_fn g_op_table[4] = { op_add, op_xor, op_mul, op_rot };

// One unit = one unpredictable indirect call.
NOINLINE static void disp_kernel(void *vctx, uint64_t n) {
    disp_ctx_t *d = (disp_ctx_t *)vctx;
    const uint8_t *idx = d->idx;
    const size_t mask = d->mask;
    size_t pos = d->pos;
    uint64_t acc = d->acc;
    for (uint64_t i = 0; i < n; ++i) {
        acc = g_op_table[idx[pos] & 3u](acc);
        pos = (pos + 1) & mask;
    }
    d->pos = pos;
    d->acc = acc;
}

// ---------------------------------------------------------------------------
// Timed driver
// ---------------------------------------------------------------------------
//
// Runs the kernel in batches, adapting the batch size so that each batch lands
// near TARGET_BATCH_SEC. That keeps clock_gettime out of the measured work
// while bounding how far past `seconds` we can overshoot. Both the iteration
// count and the elapsed time include the final batch, so the reported rate is
// exact regardless of overshoot.
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

// Warm up the core (wake it from idle, ramp DVFS) without spending the whole
// warm-up inside clock_gettime.
// Volatile so the compiler cannot delete a warm-up loop as dead code.
static volatile uint64_t g_warmup_sink;

static void warmup_spin(double sec) {
    if (sec <= 0.0) return;
    int_ctx_t c;
    for (int i = 0; i < LANES; ++i) c.l[i] = (uint64_t)i + 1u;
    const double t_end = now_sec() + sec;
    while (now_sec() < t_end) {
        int_kernel_thr(&c, 20000);
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

// ---------------------------------------------------------------------------
// DRAM controller frequency (Linux devfreq)
// ---------------------------------------------------------------------------
//
// Some SoCs scale the memory controller clock independently of the CPU, over a
// range wide enough to dominate every memory measurement (4x on RK35xx). The
// memory phases warm up with real traffic to force a ramp, but the governor may
// still not reach its top step. Reporting the observed clock makes that visible
// instead of leaving it as unexplained variance.
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
    pthread_barrier_t *bar;
    uint64_t seed;

    volatile uint64_t checksum;

    // results
    double int_lat_mops;
    double int_thr_mops;
    double fp_lat_mflops;
    double fp_thr_mflops;
    double mem_gbps;
    double mem_lat_ns;     // one dependent chain
    double mem_mlp_ns;     // CHASE_WAYS chains in flight, per access
    double disp_mops;      // unpredictable indirect calls per second
    double mhz_observed;   // sampled while the core was under load
    double dram_mhz_observed; // DRAM controller clock during the memory phases
    int    cpu_observed;   // sched_getcpu() during the run
} worker_t;

static void wsync(worker_t *w) {
    if (w->bar) pthread_barrier_wait(w->bar);
}

// Enter one repetition of a phase: line all threads up, warm up, line up again
// so every thread starts measuring at the same instant. Only the first rep
// warms up; by later reps the core is already at its steady-state clock.
//
// Every thread must call this the same number of times or the barrier will
// deadlock, so reps are driven by w->reps rather than by anything data-dependent.
static void phase_begin(worker_t *w, int rep) {
    wsync(w);
    if (rep == 0) warmup_spin(w->warmup);
    wsync(w);
}

// Warm-up for the memory phases. A pure-ALU spin does not ramp the DRAM
// controller: on a platform whose memory governor scales the DDR clock (e.g.
// devfreq/dmc_ondemand, which on this class of SoC spans a 4x frequency range),
// the memory phases would otherwise start measuring at whatever idle DRAM clock
// the governor happened to be parked at. That alone produced ~2x swings in
// measured latency between identical cores. Warm up with real traffic instead.
// `readonly` must be set once a pointer chase has been built into the buffer:
// the read+write sweep would otherwise overwrite the chase links with garbage
// and the next chase would dereference a wild pointer. A read-only sweep ramps
// the DRAM governor just as well.
NOINLINE static void mem_warmup(void *buf, size_t nbytes, double sec, int readonly) {
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
            mem_kernel_bw(&m, 1);
        }
        for (int i = 0; i < 4; ++i) g_warmup_sink ^= m.acc[i];
    }
}

// Same contract as phase_begin, but ramps the memory subsystem rather than the
// core. Must be called the same number of times by every thread.
static void phase_begin_mem(worker_t *w, int rep, void *buf, size_t nbytes,
                            int readonly) {
    wsync(w);
    if (rep == 0) mem_warmup(buf, nbytes, w->warmup, readonly);
    wsync(w);
}

// Repeated measurements keep the *best* result, not the mean. Any interference
// -- another process, an interrupt, a DVFS dip -- can only ever make a run
// slower, so the fastest observed run is the closest to the machine's true
// capability. Averaging would just fold the noise in.
static inline void keep_best_rate(double *best, double candidate) {
    if (candidate > *best) *best = candidate;
}
static inline void keep_best_lat(double *best, double candidate) {
    if (candidate > 0.0 && (*best <= 0.0 || candidate < *best)) *best = candidate;
}

// Build `ways` independent single-cycle random pointer chases over `buf`, one
// node per `stride` bytes.
//
// Ways are *interleaved*, not given contiguous slices: way w owns nodes
// w, w+ways, w+2*ways, ... so every way's addresses are spread across the whole
// buffer. That matters because the latency phase (ways=1) and the MLP phase
// (ways=CHASE_WAYS) must present the same cache footprint -- otherwise the
// one-chain measurement walks a small enough region to sit in last-level cache
// and reports cache latency while the parallel one reports DRAM latency, which
// silently inflates the apparent MLP.
//
// Each way is one full cycle over its own nodes, so a chase can never collapse
// into a short loop that would stay resident in cache.
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

#ifdef __linux__
    if (w->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((unsigned int)w->cpu, &set);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
    w->cpu_observed = sched_getcpu();
#else
    w->cpu_observed = -1;
#endif

    uint8_t *buf = NULL;
    const size_t nbytes = w->mem_bytes;
    if (nbytes > 0) {
        if (posix_memalign((void **)&buf, 4096, nbytes) != 0 || !buf) {
            perror("posix_memalign");
            buf = NULL;
        } else {
            memset(buf, 1, nbytes);   // allocate and fault in every page
        }
    }

    uint64_t seed = w->seed;
    double elapsed;

    const int reps = w->reps > 0 ? w->reps : 1;

    // ---- integer latency: one dependency chain ----
    for (int rep = 0; rep < reps; ++rep) {
        int_ctx_t c;
        for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
        phase_begin(w, rep);
        const uint64_t it = run_timed(int_kernel_lat, &c, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->int_lat_mops,
                       (double)it * (double)INT_OPS_PER_STEP / 1e6 / elapsed);
        for (int i = 0; i < LANES; ++i) w->checksum ^= c.l[i];
    }

    // ---- integer throughput: LANES independent chains ----
    for (int rep = 0; rep < reps; ++rep) {
        int_ctx_t c;
        for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
        phase_begin(w, rep);
        const uint64_t it = run_timed(int_kernel_thr, &c, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->int_thr_mops,
                       (double)it * (double)(INT_OPS_PER_STEP * LANES) / 1e6 / elapsed);
        for (int i = 0; i < LANES; ++i) w->checksum ^= c.l[i];
        // Sample the clock while this core is still fully loaded.
        if (w->cpu_observed >= 0) {
            const double mhz = cpu_cur_mhz(w->cpu_observed);
            if (mhz > w->mhz_observed) w->mhz_observed = mhz;
        }
    }

    // A rotation by 1 radian: |(x,y)| is invariant, so the state stays in
    // normal double range for any number of iterations.
    const double angle = 1.0;
    const double co = cos(angle), si = sin(angle);

    // ---- FP latency ----
    for (int rep = 0; rep < reps; ++rep) {
        fp_ctx_t f;
        f.c = co; f.s = si;
        for (int i = 0; i < LANES; ++i) { f.x[i] = 1.0 + (double)i; f.y[i] = 0.5; }
        phase_begin(w, rep);
        const uint64_t it = run_timed(fp_kernel_lat, &f, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->fp_lat_mflops,
                       (double)it * (double)FP_OPS_PER_STEP / 1e6 / elapsed);
        w->checksum ^= (uint64_t)(int64_t)(f.x[0] * 1e6 + f.y[0] * 1e3);
    }

    // ---- FP throughput ----
    for (int rep = 0; rep < reps; ++rep) {
        fp_ctx_t f;
        f.c = co; f.s = si;
        for (int i = 0; i < LANES; ++i) { f.x[i] = 1.0 + (double)i; f.y[i] = 0.5; }
        phase_begin(w, rep);
        const uint64_t it = run_timed(fp_kernel_thr, &f, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->fp_thr_mflops,
                       (double)it * (double)(FP_OPS_PER_STEP * LANES) / 1e6 / elapsed);
        for (int i = 0; i < LANES; ++i) {
            w->checksum ^= (uint64_t)(int64_t)(f.x[i] * 1e6 + f.y[i] * 1e3);
        }
    }

    // ---- unpredictable indirect dispatch ----
    {
        const size_t nidx = 1u << 16;   // 64 KiB of random selectors
        uint8_t *idx = (uint8_t *)malloc(nidx);
        if (idx) {
            for (size_t i = 0; i < nidx; ++i) idx[i] = (uint8_t)xs64(&seed);
            for (int rep = 0; rep < reps; ++rep) {
                disp_ctx_t d = { .idx = idx, .mask = nidx - 1, .pos = 0, .acc = 1 };
                phase_begin(w, rep);
                const uint64_t calls = run_timed(disp_kernel, &d, w->seconds, 1024, &elapsed);
                keep_best_rate(&w->disp_mops, (double)calls / 1e6 / elapsed);
                w->checksum ^= d.acc;
            }
            free(idx);
        } else {
            // Still hit the barrier the same number of times as every other
            // thread, or they would all hang here.
            for (int rep = 0; rep < reps; ++rep) phase_begin(w, rep);
        }
    }

    const int mem_ok = (buf != NULL && nbytes >= (size_t)CHASE_WAYS * 65536);

    // ---- memory bandwidth: sequential read+write sweep ----
    for (int rep = 0; rep < reps; ++rep) {
        phase_begin_mem(w, rep, buf, nbytes, 0);
        if (!mem_ok) continue;
        mem_ctx_t m = { .buf = (uint64_t *)buf,
                        .nqwords = nbytes / sizeof(uint64_t),
                        .acc = {0, 0, 0, 0} };
        const uint64_t passes = run_timed(mem_kernel_bw, &m, w->seconds, 1, &elapsed);
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
        const uint64_t hops = run_timed(mem_kernel_lat, &ch, w->seconds, 4096, &elapsed);
        keep_best_lat(&w->mem_lat_ns, elapsed / (double)hops * 1e9);
        w->checksum ^= (uint64_t)(uintptr_t)ch.cur[0];
    }

    // ---- memory-level parallelism: CHASE_WAYS chases over the same buffer ----
    // Rebuilt as CHASE_WAYS interleaved cycles, so the footprint is identical
    // to the latency phase above and the ratio is a clean MLP measurement.
    chase_ok = mem_ok && build_chases(buf, nbytes, 64, CHASE_WAYS,
                                      w->seed ^ UINT64_C(0xA5A5A5A5), heads) == 0;
    for (int rep = 0; rep < reps; ++rep) {
        phase_begin_mem(w, rep, buf, nbytes, 1);
        if (!chase_ok) continue;
        chase_ctx_t ch;
        for (int i = 0; i < CHASE_WAYS; ++i) ch.cur[i] = (void **)heads[i];
        const uint64_t hops = run_timed(mem_kernel_mlp, &ch, w->seconds, 1024, &elapsed);
        keep_best_lat(&w->mem_mlp_ns,
                      elapsed / ((double)hops * (double)CHASE_WAYS) * 1e9);
        for (int i = 0; i < CHASE_WAYS; ++i) {
            w->checksum ^= (uint64_t)(uintptr_t)ch.cur[i];
        }
    }

    free(buf);
    return NULL;
}

// ---------------------------------------------------------------------------
// CPU list parsing / platform info
// ---------------------------------------------------------------------------

static int get_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 1024) n = 1024;
    return (int)n;
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
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
#else
    return "unknown";
#endif
}

// Collect the distinct CPU model names seen in /proc/cpuinfo. On heterogeneous
// ARM parts there is more than one.
static void detect_cpu_models(char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;

    char line[512];
    const char *keys[] = { "model name", "Model Name", "cpu model", "Hardware" };
    while (fgets(line, (int)sizeof(line), f)) {
        for (int k = 0; k < ARRAY_LEN(keys); ++k) {
            if (strncmp(line, keys[k], strlen(keys[k])) != 0) continue;
            const char *colon = strchr(line, ':');
            if (!colon) continue;
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            size_t len = strlen(colon);
            while (len > 0 && (colon[len - 1] == '\n' || colon[len - 1] == '\r')) len--;
            if (len == 0) continue;
            char name[192];
            const size_t cp = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
            memcpy(name, colon, cp);
            name[cp] = '\0';
            if (!strstr(out, name)) {                 // de-duplicate
                if (out[0]) {
                    const size_t used = strlen(out);
                    snprintf(out + used, outsz - used, " + %s", name);
                } else {
                    snprintf(out, outsz, "%s", name);
                }
            }
            break;
        }
    }
    fclose(f);
}

static void print_platform(void) {
#if defined(__clang__)
    printf("build: clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    printf("build: gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    printf("build: cc");
#endif
#ifdef __STDC_VERSION__
    printf(", C%ld", (long)__STDC_VERSION__);
#endif
    printf(", target=%s", arch_string());
#ifdef NO_TREE_VECTORIZE
    printf(", vectorize=off");
#else
    printf(", vectorize=on");
#endif
#ifdef FFP_CONTRACT_OFF
    printf(", fma=off");
#else
    printf(", fma=on");
#endif
    printf("\n");

    struct utsname u;
    if (uname(&u) == 0) {
        printf("system: %s %s, machine=%s, cpus=%d\n",
               u.sysname, u.release, u.machine, get_cpu_count());
    }
    char models[512];
    detect_cpu_models(models, sizeof(models));
    if (models[0]) printf("cpu: %s\n", models);
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
} run_opts_t;

// Warn if the machine is already busy. Best-of-N rejects most interference,
// but a heavily loaded box can still depress every repetition.
static void warn_if_loaded(int threads) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;
    double la = 0.0;
    const int got = fscanf(f, "%lf", &la);
    fclose(f);
    if (got != 1) return;
    const int cpus = get_cpu_count();
    if (la > 0.5 && la > 0.15 * (double)cpus) {
        printf("WARNING: load average is %.2f on %d CPUs. Other work on this "
               "machine will\n"
               "         depress results; best-of-N rejects some of it but not "
               "all. Consider\n"
               "         raising --reps or quiescing the system.\n", la, cpus);
    }
    (void)threads;
}

static int run_workers(worker_t *w, int n, const run_opts_t *o) {
    pthread_barrier_t bar;
    int use_bar = (n > 1);
    if (use_bar && pthread_barrier_init(&bar, NULL, (unsigned)n) != 0) {
        perror("pthread_barrier_init");
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
        w[i].seed = o->seed + (uint64_t)i * UINT64_C(0x9E3779B97F4A7C15) + 1u;
        const int rc = pthread_create(&ths[i], NULL, worker, &w[i]);
        if (rc != 0) { errno = rc; perror("pthread_create"); free(ths); return -1; }
    }
    for (int i = 0; i < n; ++i) pthread_join(ths[i], NULL);

    free(ths);
    if (use_bar) pthread_barrier_destroy(&bar);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --threads N        threads to run (default: online CPUs)\n"
        "  --cpus LIST        pin to these CPUs, e.g. 0-3,6 (default: 0..threads-1)\n"
        "  --per-core         run the suite single-threaded on each CPU in turn\n"
        "  --time SEC         measured seconds per phase (default 0.5)\n"
        "  --reps N           repetitions per phase, best is kept (default 3)\n"
        "  --warmup SEC       warm-up seconds before each phase (default 0.15)\n"
        "  --mem-per-thread B per-thread buffer for the memory phases (default 16 MiB)\n"
        "  --mem BYTES        total memory across all threads (overrides the above)\n"
        "  --no-mem           skip the memory bandwidth and latency phases\n"
        "  --no-pin           do not set thread affinity\n"
        "  --clock mono|raw   clock source (default raw)\n"
        "  --seed N           PRNG seed for setup (default 1)\n",
        prog);
}

// Largest cache size reported by sysfs, in bytes (0 if unknown).
static size_t detect_llc_bytes(void) {
    size_t best = 0;
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
}

// The memory phases only mean anything if the working set clears last-level
// cache. A buffer that fits in cache silently turns "MEM-lat" into a cache
// latency measurement, which looks like a spectacular result rather than a
// misconfiguration.
static void warn_if_working_set_small(size_t per_thread, int threads, int per_core) {
    if (per_thread == 0) return;
    const size_t llc = detect_llc_bytes();
    if (llc == 0) return;
    const size_t total = per_core ? per_thread : per_thread * (size_t)threads;
    if (total < llc * 2) {
        printf("WARNING: %zu KiB working set vs %zu KiB last-level cache. The "
               "memory phases\n"
               "         will report cache behaviour, not DRAM. Raise "
               "--mem-per-thread.\n",
               total >> 10, llc >> 10);
    }
}

// Report the DRAM controller clock seen while the memory phases were running.
// If it moved during the run, or never reached the top step, the memory numbers
// carry that variance and the reader needs to know.
static void report_dram(double lo, double hi) {
    if (hi <= 0.0) return;
    if (hi - lo > 1.0) {
        printf("DRAM (%s): %.0f-%.0f MHz during memory phases -- the controller "
               "changed\n"
               "      speed mid-run, so memory results carry that spread. Pin its "
               "governor\n"
               "      to 'performance' for stable numbers.\n", g_dram_name, lo, hi);
    } else {
        printf("DRAM (%s): %.0f MHz during memory phases\n", g_dram_name, hi);
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

// Synthetic single-core score. Deliberately spans compute *and* the two things
// that actually separate a big core from a little one: unpredictable indirect
// dispatch and overlapped cache misses. The per-component scale factors only
// shift the absolute magnitude -- a constant factor on a geomean -- so ratios
// between cores are unaffected by them.
static double core_score(const worker_t *w) {
    double v[4];
    v[0] = w->int_thr_mops;
    v[1] = w->fp_thr_mflops;
    v[2] = w->disp_mops * 100.0;
    // Random-access rate with all chases in flight, in millions/s.
    v[3] = w->mem_mlp_ns > 0.0 ? (1000.0 / w->mem_mlp_ns) * 100.0 : 0.0;
    return geomean(v, ARRAY_LEN(v));
}

int main(int argc, char **argv) {
    int threads = get_cpu_count();
    int cpu_list[1024];
    int n_cpus = 0;
    int per_core = 0;
    int pin = 1;
    int use_clock_raw = 1;
    double duration = 0.5;
    double warmup = 0.15;
    int reps = 3;
    size_t mem_per_thread = (size_t)16 << 20;
    size_t mem_total = 0;
    uint64_t seed = 1;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--threads") && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (!strcmp(a, "--cpus") && i + 1 < argc) {
            n_cpus = parse_cpu_list(argv[++i], cpu_list, ARRAY_LEN(cpu_list));
            if (n_cpus <= 0) { fprintf(stderr, "bad --cpus list\n"); return 2; }
        } else if (!strcmp(a, "--per-core")) {
            per_core = 1;
        } else if (!strcmp(a, "--time") && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else if (!strcmp(a, "--reps") && i + 1 < argc) {
            reps = atoi(argv[++i]);
        } else if (!strcmp(a, "--warmup") && i + 1 < argc) {
            warmup = atof(argv[++i]);
        } else if (!strcmp(a, "--mem-per-thread") && i + 1 < argc) {
            mem_per_thread = (size_t)strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--mem") && i + 1 < argc) {
            mem_total = (size_t)strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(a, "--no-mem")) {
            mem_per_thread = 0;
            mem_total = 0;
        } else if (!strcmp(a, "--no-pin")) {
            pin = 0;
        } else if (!strcmp(a, "--clock") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "mono")) use_clock_raw = 0;
            else if (!strcmp(m, "raw")) use_clock_raw = 1;
            else { fprintf(stderr, "bad --clock\n"); return 2; }
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 0);
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

    // Default CPU list: 0..threads-1, or every online CPU in --per-core mode.
    if (n_cpus == 0) {
        const int online = get_cpu_count();
        const int want = per_core ? online : threads;
        for (int i = 0; i < want && i < ARRAY_LEN(cpu_list); ++i) cpu_list[i] = i;
        n_cpus = want < ARRAY_LEN(cpu_list) ? want : ARRAY_LEN(cpu_list);
    }
    set_clock_mode(use_clock_raw);
    find_dram_devfreq();
    print_platform();

    if (mem_total > 0) {
        const int div = per_core ? 1 : threads;
        mem_per_thread = mem_total / (size_t)(div > 0 ? div : 1);
    }
    if (mem_per_thread > 0 && mem_per_thread < (size_t)(1 << 20)) {
        mem_per_thread = (size_t)(1 << 20);
    }
    // Round down to a whole number of 4 KiB pages and 32-byte groups.
    mem_per_thread &= ~(size_t)4095;

    run_opts_t opts = {
        .seconds = duration,
        .warmup = warmup,
        .reps = reps,
        .mem_per_thread = mem_per_thread,
        .seed = seed,
    };

    warn_if_loaded(threads);
    warn_if_working_set_small(mem_per_thread, threads, per_core);

    // -----------------------------------------------------------------------
    // Per-core sweep
    // -----------------------------------------------------------------------
    if (per_core) {
        printf("mode: per-core sweep over %d CPUs, %.2fs/phase x %d reps (best kept),\n"
               "      warmup %.2fs, mem %zu MiB/thread\n\n",
               n_cpus, duration, reps, warmup, mem_per_thread >> 20);
        printf("%4s %6s  %8s %8s %5s  %8s %8s  %7s %7s %5s  %8s   %8s\n",
               "CPU", "MHz", "INT-lat", "INT-thr", "ILP", "FP-lat", "FP-thr",
               "MEM", "MEMlat", "MLP", "DISPATCH", "score");
        printf("%4s %6s  %8s %8s %5s  %8s %8s  %7s %7s %5s  %8s   %8s\n",
               "", "", "Mops/s", "Mops/s", "x", "Mflop/s", "Mflop/s",
               "GB/s", "ns", "x", "Mcall/s", "geomean");

        double best = 0.0, worst = 0.0;
        double dram_lo = 0.0, dram_hi = 0.0;
        for (int i = 0; i < n_cpus; ++i) {
            worker_t w;
            memset(&w, 0, sizeof(w));
            w.cpu = pin ? cpu_list[i] : -1;
            if (run_workers(&w, 1, &opts) != 0) return 1;

            double mhz = w.mhz_observed;
            if (mhz <= 0.0) mhz = cpu_max_mhz(cpu_list[i]);
            const double ilp = w.int_lat_mops > 0.0 ? w.int_thr_mops / w.int_lat_mops : 0.0;
            const double mlp = w.mem_mlp_ns > 0.0 ? w.mem_lat_ns / w.mem_mlp_ns : 0.0;
            const double score = core_score(&w);
            if (score > best) best = score;
            if (worst == 0.0 || score < worst) worst = score;
            if (w.dram_mhz_observed > 0.0) {
                if (w.dram_mhz_observed > dram_hi) dram_hi = w.dram_mhz_observed;
                if (dram_lo == 0.0 || w.dram_mhz_observed < dram_lo) {
                    dram_lo = w.dram_mhz_observed;
                }
            }

            printf("%4d %6.0f  %8.1f %8.1f %5.2f  %8.1f %8.1f  %7.2f %7.1f %5.2f  %8.1f   %8.1f\n",
                   cpu_list[i], mhz,
                   w.int_lat_mops, w.int_thr_mops, ilp,
                   w.fp_lat_mflops, w.fp_thr_mflops,
                   w.mem_gbps, w.mem_lat_ns, mlp,
                   w.disp_mops, score);
            fflush(stdout);
        }
        if (worst > 0.0) {
            printf("\nfastest/slowest core ratio: %.2fx\n", best / worst);
        }
        report_dram(dram_lo, dram_hi);
        printf("ILP = INT-thr/INT-lat, how much instruction parallelism the core extracts\n"
               "      from 8 independent chains. A 2-wide in-order core caps out near 2x.\n"
               "MLP = MEMlat / (latency with %d chases in flight): how much memory latency\n"
               "      the core overlaps. In-order cores stall on the first miss and sit\n"
               "      near 1x. Can exceed %d when parallel TLB walks overlap too.\n"
               "score = geomean of INT-thr, FP-thr, dispatch rate and parallel random-access\n"
               "        rate. Comparable across cores of one run, not across machines.\n",
               CHASE_WAYS, CHASE_WAYS);
        return 0;
    }

    // -----------------------------------------------------------------------
    // Normal (multi-threaded) run
    // -----------------------------------------------------------------------
    worker_t *w = (worker_t *)calloc((size_t)threads, sizeof(*w));
    if (!w) { perror("calloc"); return 1; }
    for (int i = 0; i < threads; ++i) {
        w[i].cpu = pin ? cpu_list[i % n_cpus] : -1;
    }

    printf("config: threads=%d time=%.2fs/phase x%d reps warmup=%.2fs "
           "mem=%zu MiB/thread pin=%s\n\n",
           threads, duration, reps, warmup, mem_per_thread >> 20, pin ? "on" : "off");

    if (run_workers(w, threads, &opts) != 0) return 1;

    double int_lat = 0, int_thr = 0, fp_lat = 0, fp_thr = 0, gbps = 0, disp = 0;
    double lat_ns_sum = 0, mlp_ns_sum = 0;
    int lat_n = 0, mlp_n = 0;
    uint64_t checksum = 0;
    for (int i = 0; i < threads; ++i) {
        int_lat += w[i].int_lat_mops;
        int_thr += w[i].int_thr_mops;
        fp_lat  += w[i].fp_lat_mflops;
        fp_thr  += w[i].fp_thr_mflops;
        gbps    += w[i].mem_gbps;
        disp    += w[i].disp_mops;
        if (w[i].mem_lat_ns > 0.0) { lat_ns_sum += w[i].mem_lat_ns; lat_n++; }
        if (w[i].mem_mlp_ns > 0.0) { mlp_ns_sum += w[i].mem_mlp_ns; mlp_n++; }
        checksum ^= w[i].checksum;
    }
    const double n = (double)threads;

    printf("%-18s %12s %12s\n", "metric", "total", "per-thread");
    printf("%-18s %12.1f %12.1f\n", "INT-lat  Mops/s", int_lat, int_lat / n);
    printf("%-18s %12.1f %12.1f\n", "INT-thr  Mops/s", int_thr, int_thr / n);
    printf("%-18s %12.1f %12.1f\n", "FP64-lat Mflop/s", fp_lat, fp_lat / n);
    printf("%-18s %12.1f %12.1f\n", "FP64-thr Mflop/s", fp_thr, fp_thr / n);
    printf("%-18s %12.1f %12.1f\n", "DISPATCH Mcall/s", disp, disp / n);
    if (mem_per_thread > 0) {
        printf("%-18s %12.2f %12.2f\n", "MEM-bw   GB/s", gbps, gbps / n);
        if (lat_n > 0) {
            printf("%-18s %12s %12.1f\n", "MEM-lat  ns", "-", lat_ns_sum / lat_n);
        }
        if (mlp_n > 0) {
            printf("%-18s %12s %12.1f\n", "MEM-lat/8 ns", "-", mlp_ns_sum / mlp_n);
        }
    }
    {
        double dlo = 0.0, dhi = 0.0;
        for (int i = 0; i < threads; ++i) {
            const double d = w[i].dram_mhz_observed;
            if (d <= 0.0) continue;
            if (d > dhi) dhi = d;
            if (dlo == 0.0 || d < dlo) dlo = d;
        }
        printf("\n");
        report_dram(dlo, dhi);
    }
    printf("ILP (INT-thr / INT-lat): %.2fx\n", int_lat > 0.0 ? int_thr / int_lat : 0.0);
    if (lat_n > 0 && mlp_n > 0) {
        printf("MLP (latency overlapped): %.2fx\n",
               (lat_ns_sum / lat_n) / (mlp_ns_sum / mlp_n));
    }
    printf("CHK : 0x%016llx\n", (unsigned long long)checksum);

    free(w);
    return 0;
}
