// Portable CPU benchmark: integer, FP, memory and indirect-dispatch kernels for
// 64-bit x86_64 / aarch64 / riscv64. libc + pthreads only, no intrinsics.
//
// Every compute kernel exists in a LAT variant (one dependency chain) and a THR
// variant (LANES independent chains) built from the same op sequence, so thr/lat
// reads out how much parallelism the core extracts. No `volatile` in a hot loop
// (it would measure store-to-load forwarding); dead code is prevented by a
// checksum the caller reads. FP state stays in normal range for any iteration
// count, so no Inf/NaN/denormal timing penalties ever enter a measurement.

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

// Independent chains in the throughput kernels: enough to saturate the widest
// core targeted, few enough to stay in the register file of a 16-GPR ISA.
#define LANES 8

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
// One integer step: four dependent single-cycle ALU ops.
//
// Deliberately contains NO multiply: a multiply-bound kernel is multiplier-bound
// in both variants, so their ratio cancels and reports the same ILP for any core
// width. add/xor/sub against a register is one instruction and one cycle on
// every target ISA, so this measures issue width and nothing else; multiply
// capability is measured separately below.
//
// Only two constants, reused, so the kernel fits 8 lanes + 2 constants + a
// counter without spilling even on 16-GPR x86-64. The intervening xors stop the
// compiler folding `+k1` and `-k1` together.
#define INT_STEP(a, k1, k2)                           \
    do {                                              \
        (a) += (k1);                                  \
        (a) ^= (k2);                                  \
        (a) -= (k1);                                  \
        (a) ^= (k2);                                  \
    } while (0)
#define INT_OPS_PER_STEP 4u

// One multiply step, kept apart from the ALU kernel so that integer multiplier
// throughput is its own number instead of silently gating ILP.
// Odd * odd stays odd, so a lane can never collapse to zero.
#define MUL_STEP(a, m) do { (a) *= (m); } while (0)
#define MUL_OPS_PER_STEP 1u

// One FP step: one multiply and one add, so the kernel measures FP throughput
// rather than saturating whichever pipe it over-uses.
//
// x = x*c + b with |c| < 1 converges to the fixed point b/(1-c) and stays there,
// so the state is bounded for any iteration count and can never reach Inf, NaN
// or a denormal. With -ffp-contract=off this is fmul+fadd; with FMA enabled it
// folds to one FMA. Either way it is 2 FLOPs.
#define FP_STEP(x, c, b) do { (x) = (x) * (c) + (b); } while (0)
#define FP_OPS_PER_STEP 2u

typedef struct { uint64_t l[LANES]; uint64_t k[2]; } int_ctx_t;
typedef struct { uint64_t l[LANES]; uint64_t m; } mul_ctx_t;
typedef struct { double x[LANES], b[LANES], c; } fp_ctx_t;

typedef struct {
    uint64_t *buf;
    size_t    nqwords;
    uint64_t  acc[4];
} mem_ctx_t;

// CHASE_WAYS independent pointer-chase cycles. One cycle measures raw
// random-access latency; all of them in lockstep measures how much of that
// latency the core overlaps. The speedup can exceed CHASE_WAYS because the
// serial chase also serializes TLB page-table walks.
#define CHASE_WAYS 8

typedef struct {
    void   **cur[CHASE_WAYS];
} chase_ctx_t;

// Indirect dispatch through a table of tiny functions selected by data. Cannot
// be if-converted or devirtualized, so it exercises the indirect branch
// predictor and the front end.
typedef uint64_t (*op_fn)(uint64_t);

typedef struct {
    const uint8_t *idx;
    size_t         mask;
    size_t         pos;
    uint64_t       acc;
} disp_ctx_t;

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

// A kernel runs `n` units of work and returns; the driver times batches of them.
typedef void (*kernel_fn)(void *ctx, uint64_t n);

// The k-constants come from the context rather than being literals, so the
// compiler cannot fold `+k1` and `-k3` together across the intervening xors.
NOINLINE static void int_kernel_lat(void *vctx, uint64_t n) {
    int_ctx_t *c = (int_ctx_t *)vctx;
    const uint64_t k1 = c->k[0], k2 = c->k[1];
    uint64_t a0 = c->l[0];
    for (uint64_t i = 0; i < n; ++i) {
        INT_STEP(a0, k1, k2);
    }
    c->l[0] = a0;
}

NOINLINE static void int_kernel_thr(void *vctx, uint64_t n) {
    int_ctx_t *c = (int_ctx_t *)vctx;
    const uint64_t k1 = c->k[0], k2 = c->k[1];
    uint64_t a0 = c->l[0], a1 = c->l[1], a2 = c->l[2], a3 = c->l[3];
    uint64_t a4 = c->l[4], a5 = c->l[5], a6 = c->l[6], a7 = c->l[7];
    for (uint64_t i = 0; i < n; ++i) {
        INT_STEP(a0, k1, k2); INT_STEP(a1, k1, k2);
        INT_STEP(a2, k1, k2); INT_STEP(a3, k1, k2);
        INT_STEP(a4, k1, k2); INT_STEP(a5, k1, k2);
        INT_STEP(a6, k1, k2); INT_STEP(a7, k1, k2);
    }
    c->l[0] = a0; c->l[1] = a1; c->l[2] = a2; c->l[3] = a3;
    c->l[4] = a4; c->l[5] = a5; c->l[6] = a6; c->l[7] = a7;
}

// Integer multiply throughput: LANES independent multiply chains.
NOINLINE static void mul_kernel_thr(void *vctx, uint64_t n) {
    mul_ctx_t *c = (mul_ctx_t *)vctx;
    const uint64_t m = c->m;
    uint64_t a0 = c->l[0], a1 = c->l[1], a2 = c->l[2], a3 = c->l[3];
    uint64_t a4 = c->l[4], a5 = c->l[5], a6 = c->l[6], a7 = c->l[7];
    for (uint64_t i = 0; i < n; ++i) {
        MUL_STEP(a0, m); MUL_STEP(a1, m); MUL_STEP(a2, m); MUL_STEP(a3, m);
        MUL_STEP(a4, m); MUL_STEP(a5, m); MUL_STEP(a6, m); MUL_STEP(a7, m);
    }
    c->l[0] = a0; c->l[1] = a1; c->l[2] = a2; c->l[3] = a3;
    c->l[4] = a4; c->l[5] = a5; c->l[6] = a6; c->l[7] = a7;
}

NOINLINE static void fp_kernel_lat(void *vctx, uint64_t n) {
    fp_ctx_t *f = (fp_ctx_t *)vctx;
    const double co = f->c;
    const double b0 = f->b[0];
    double x0 = f->x[0];
    for (uint64_t i = 0; i < n; ++i) {
        FP_STEP(x0, co, b0);
    }
    f->x[0] = x0;
}

NOINLINE static void fp_kernel_thr(void *vctx, uint64_t n) {
    fp_ctx_t *f = (fp_ctx_t *)vctx;
    const double co = f->c;
    const double b0 = f->b[0], b1 = f->b[1], b2 = f->b[2], b3 = f->b[3];
    const double b4 = f->b[4], b5 = f->b[5], b6 = f->b[6], b7 = f->b[7];
    double x0 = f->x[0], x1 = f->x[1], x2 = f->x[2], x3 = f->x[3];
    double x4 = f->x[4], x5 = f->x[5], x6 = f->x[6], x7 = f->x[7];
    for (uint64_t i = 0; i < n; ++i) {
        FP_STEP(x0, co, b0); FP_STEP(x1, co, b1);
        FP_STEP(x2, co, b2); FP_STEP(x3, co, b3);
        FP_STEP(x4, co, b4); FP_STEP(x5, co, b5);
        FP_STEP(x6, co, b6); FP_STEP(x7, co, b7);
    }
    f->x[0] = x0; f->x[1] = x1; f->x[2] = x2; f->x[3] = x3;
    f->x[4] = x4; f->x[5] = x5; f->x[6] = x6; f->x[7] = x7;
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

// One unit = one indirect call whose target is chosen by the selector stream.
// The selector array is walked cyclically with `mask` = period-1, so the target
// sequence repeats with period `mask + 1`.
//
// The result feeds the next call's argument. The *target* does not depend on
// `acc`, so this chain does not stop the predictor from running ahead.
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

// Same call site and selector stream, but the result is folded into a side
// accumulator instead of feeding the next argument, so consecutive calls are
// data-independent. Shows whether the serial `acc` chain caps the measurement.
NOINLINE static void disp_kernel_free(void *vctx, uint64_t n) {
    disp_ctx_t *d = (disp_ctx_t *)vctx;
    const uint8_t *idx = d->idx;
    const size_t mask = d->mask;
    size_t pos = d->pos;
    const uint64_t arg = d->acc | 1u;
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; ++i) {
        sum ^= g_op_table[idx[pos] & 3u](arg);
        pos = (pos + 1) & mask;
    }
    d->pos = pos;
    d->acc = sum;
}

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

// Wake the core from idle and ramp DVFS before measuring.

static void warmup_spin(double sec) {
    if (sec <= 0.0) return;
    int_ctx_t c;
    for (int i = 0; i < LANES; ++i) c.l[i] = (uint64_t)i + 1u;
    c.k[0] = UINT64_C(0x9E3779B97F4A7C15); c.k[1] = UINT64_C(0xBF58476D1CE4E5B9);
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
    pthread_barrier_t *bar;
    uint64_t seed;

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
    int    cpu_observed;   // sched_getcpu() during the run
} worker_t;

static void wsync(worker_t *w) {
    if (w->bar) pthread_barrier_wait(w->bar);
}

// Enter one repetition of a phase: line all threads up, warm up (first rep
// only), line up again so every thread starts measuring at the same instant.
// Every thread must call this the same number of times or the barrier deadlocks.
static void phase_begin(worker_t *w, int rep) {
    wsync(w);
    if (rep == 0) warmup_spin(w->warmup);
    wsync(w);
}

// Warm-up for the memory phases. A pure-ALU spin does not ramp a DRAM
// controller whose governor scales the DDR clock, so the memory phases would
// start measuring at whatever idle clock it was parked at.
//
// `readonly` must be set once a pointer chase has been built into the buffer:
// the read+write sweep would overwrite the chase links and the next chase would
// dereference a wild pointer. A read-only sweep ramps the governor just as well.
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

// Same contract as phase_begin, but ramps the memory subsystem.
static void phase_begin_mem(worker_t *w, int rep, void *buf, size_t nbytes,
                            int readonly) {
    wsync(w);
    if (rep == 0) mem_warmup(buf, nbytes, w->warmup, readonly);
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

    // ---- selector-period sweep (diagnostic mode, --disp-sweep) ----
    // Runs the dispatch kernel at every period in g_disp_periods and nothing
    // else: the raw curve the DISPcap number is derived from.
    if (w->disp_sweep) {
        // INT-lat is one dependent 1-cycle op per cycle, so it doubles as a
        // clock reading on targets with no cpufreq node.
        for (int rep = 0; rep < reps; ++rep) {
            int_ctx_t c;
            for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
            for (int i = 0; i < 2; ++i) c.k[i] = xs64(&seed) | 1u;
            phase_begin(w, rep);
            const uint64_t it = run_timed(int_kernel_lat, &c, w->seconds, 1024, &elapsed);
            keep_best_rate(&w->int_lat_mops,
                           (double)it * (double)INT_OPS_PER_STEP / 1e6 / elapsed);
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
                const uint64_t calls = run_timed(disp_kernel, &d, w->seconds, 1024, &elapsed);
                keep_best_rate(&w->sweep_serial[k], (double)calls / 1e6 / elapsed);
                w->checksum ^= d.acc;
            }
            for (int rep = 0; rep < reps; ++rep) {
                phase_begin(w, rep);
                if (!idx) continue;
                disp_ctx_t d = { .idx = idx, .mask = g_disp_periods[k] - 1,
                                 .pos = 0, .acc = 1 };
                const uint64_t calls = run_timed(disp_kernel_free, &d, w->seconds, 1024, &elapsed);
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
        const uint64_t it = run_timed(int_kernel_lat, &c, w->seconds, 1024, &elapsed);
        keep_best_rate(&w->int_lat_mops,
                       (double)it * (double)INT_OPS_PER_STEP / 1e6 / elapsed);
        for (int i = 0; i < LANES; ++i) w->checksum ^= c.l[i];
    }

    // ---- integer throughput: LANES independent chains ----
    for (int rep = 0; rep < reps; ++rep) {
        int_ctx_t c;
        for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
        for (int i = 0; i < 2; ++i) c.k[i] = xs64(&seed) | 1u;
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

    // ---- integer multiply throughput: LANES independent multiply chains ----
    for (int rep = 0; rep < reps; ++rep) {
        mul_ctx_t c;
        for (int i = 0; i < LANES; ++i) c.l[i] = xs64(&seed) | 1u;
        c.m = xs64(&seed) | 1u;
        phase_begin(w, rep);
        const uint64_t it = run_timed(mul_kernel_thr, &c, w->seconds, 1024, &elapsed);
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
        const uint64_t it = run_timed(fp_kernel_lat, &f, w->seconds, 1024, &elapsed);
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
        const uint64_t it = run_timed(fp_kernel_thr, &f, w->seconds, 1024, &elapsed);
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
                const uint64_t calls = run_timed(disp_kernel, &d, t_pt, 1024, &elapsed);
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
    // Interleaved, so the footprint matches the latency phase above.
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

// Distinct CPU model names in /proc/cpuinfo (more than one on big.LITTLE).
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

#ifdef NO_TREE_VECTORIZE
#define VECTORIZE_ON 0
#else
#define VECTORIZE_ON 1
#endif
#ifdef FFP_CONTRACT_OFF
#define FMA_ON 0
#else
#define FMA_ON 1
#endif

static void print_platform(void) {
    FILE *o = msg();
    char cc[64];
    compiler_string(cc, sizeof(cc));
    fprintf(o, "build: %s", cc);
#ifdef __STDC_VERSION__
    fprintf(o, ", C%ld", (long)__STDC_VERSION__);
#endif
    fprintf(o, ", target=%s, vectorize=%s, fma=%s\n", arch_string(),
            VECTORIZE_ON ? "on" : "off", FMA_ON ? "on" : "off");

    struct utsname u;
    if (uname(&u) == 0) {
        fprintf(o, "system: %s %s, machine=%s, cpus=%d\n",
                u.sysname, u.release, u.machine, get_cpu_count());
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
} run_opts_t;

// Best-of-N rejects most interference, but a loaded box depresses every rep.
static void warn_if_loaded(int threads) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;
    double la = 0.0;
    const int got = fscanf(f, "%lf", &la);
    fclose(f);
    if (got != 1) return;
    const int cpus = get_cpu_count();
    if (la > 0.5 && la > 0.15 * (double)cpus) {
        fprintf(msg(), "WARNING: load average %.2f on %d CPUs; results will be "
                       "depressed. Quiesce the machine or raise --reps.\n",
                la, cpus);
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
        "  --format F         output as text (default), json or tsv;\n"
        "                     --json and --tsv are shorthands. In json/tsv the\n"
        "                     results go to stdout and all prose to stderr\n"
        "  -v, --verbose      explain every metric after the results\n",
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
        "  ILP tracks issue width: the integer kernel holds no multiply, so\n"
        "  INT-lat pins to 1 op/cycle and the ratio is what the core issues in\n"
        "  parallel. Roughly 2x for a 2-wide in-order core, 3x for 3 ALUs.\n");
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
        {"MHz",      "core clock",              "observed under load; '~' = estimated from INT-lat"},
        {"INT-lat",  "integer latency",         "one dependent chain of 1-cycle ALU ops, Mops/s"},
        {"INT-thr",  "integer throughput",      "8 independent chains, Mops/s"},
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
    int    mhz_estimated;    // clock derived from INT-lat, not from sysfs
    double int_lat, int_thr, mul_thr, fp_lat, fp_thr;
    double mem_gbps, mem_lat_ns, mem_lat8_ns;
    double ilp, filp, mlp;
    double disp_thr, disp_cap, disp_gain, disp_span;
    int    disp_status;
    double score;
} result_t;

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
    // doubles as a clock estimate where sysfs exposes no cpufreq node.
    r.mhz = w->mhz_observed;
    if (r.mhz <= 0.0 && cpu >= 0) r.mhz = cpu_max_mhz(cpu);
    if (r.mhz <= 0.0) {
        r.mhz = r.int_lat;
        r.mhz_estimated = 1;
    }
    return r;
}

// TSV: one header line, then one row per scope. Missing values are empty.
#define RESULT_COLUMNS \
    "scope\tcpu\tmhz\tmhz_src\tint_lat_mops\tint_thr_mops\tilp\tmul_thr_mmul_s" \
    "\tfp_lat_mflops\tfp_thr_mflops\tfilp\tmem_gbps\tmem_lat_ns\tmem_lat8_ns" \
    "\tmlp\tdisp_thr_mcall_s\tdisp_cap_calls\tdisp_gain\tdisp_span\tscore\n"

static void tsv_num(double v, int ok) {
    if (ok && v > 0.0) printf("\t%.6g", v);
    else               printf("\t");
}

static void tsv_result(const char *scope, const result_t *r) {
    printf("%s", scope);
    if (r->cpu >= 0) printf("\t%d", r->cpu); else printf("\t");
    tsv_num(r->mhz, 1);
    printf("\t%s", r->mhz > 0.0 ? (r->mhz_estimated ? "estimated" : "measured")
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
    if (ok && v > 0.0) printf(", \"%s\": %.6g", key, v);
    else               printf(", \"%s\": null", key);
}

static void j_str(const char *key, const char *v) {
    printf(", \"%s\": \"", key);
    for (const char *p = v; *p; ++p) {
        if (*p == '"' || *p == '\\') putchar('\\');
        if ((unsigned char)*p < 0x20) continue;
        putchar(*p);
    }
    printf("\"");
}

static void json_result(const result_t *r, const char *scope, const char *indent) {
    printf("%s{ \"scope\": \"%s\"", indent, scope);
    if (r->cpu >= 0) printf(", \"cpu\": %d", r->cpu); else printf(", \"cpu\": null");
    j_num("mhz", r->mhz, 1);
    j_str("mhz_src", r->mhz > 0.0 ? (r->mhz_estimated ? "estimated" : "measured")
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
    printf(" }");
}

// Opening half of the JSON document: schema, build, system and run config. The
// caller adds the result arrays and calls json_close().
static void json_open(const char *mode, const run_opts_t *o, int threads,
                      int pin, int clock_raw) {
    char cc[64], models[512];
    compiler_string(cc, sizeof(cc));
    detect_cpu_models(models, sizeof(models));

    printf("{\n  \"schema\": \"cpu-bench/1\"");
    j_str("mode", mode);
    printf(",\n  \"build\": { \"compiler\": \"%s\"", cc);
    j_str("target", arch_string());
    printf(", \"vectorize\": %s, \"fma\": %s }",
           VECTORIZE_ON ? "true" : "false", FMA_ON ? "true" : "false");

    struct utsname u;
    printf(",\n  \"system\": {");
    if (uname(&u) == 0) {
        printf(" \"sysname\": \"%s\"", u.sysname);
        j_str("release", u.release);
        j_str("machine", u.machine);
        printf(",");
    }
    printf(" \"cpus\": %d", get_cpu_count());
    if (models[0]) j_str("cpu_models", models);
    printf(" }");

    printf(",\n  \"config\": { \"threads\": %d, \"seconds_per_phase\": %g,"
           " \"reps\": %d, \"warmup_seconds\": %g, \"mem_bytes_per_thread\": %zu,"
           " \"pin\": %s, \"clock\": \"%s\", \"seed\": %llu }",
           threads, o->seconds, o->reps, o->warmup, o->mem_per_thread,
           pin ? "true" : "false", clock_raw ? "raw" : "mono",
           (unsigned long long)o->seed);
}

static void json_close(void) { printf("\n}\n"); }

int main(int argc, char **argv) {
    int threads = get_cpu_count();
    int cpu_list[1024];
    int n_cpus = 0;
    int per_core = 0;
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

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--threads") && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (!strcmp(a, "--cpus") && i + 1 < argc) {
            n_cpus = parse_cpu_list(argv[++i], cpu_list, ARRAY_LEN(cpu_list));
            if (n_cpus <= 0) { fprintf(stderr, "bad --cpus list\n"); return 2; }
        } else if (!strcmp(a, "--per-core")) {
            per_core = 1;
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
        } else if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) {
            g_verbose = 1;
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
        const int want = (per_core || disp_sweep) ? online : threads;
        for (int i = 0; i < want && i < ARRAY_LEN(cpu_list); ++i) cpu_list[i] = i;
        n_cpus = want < ARRAY_LEN(cpu_list) ? want : ARRAY_LEN(cpu_list);
    }
    set_clock_mode(use_clock_raw);
    find_dram_devfreq();
    print_platform();

    // Size the default working set from the cache the machine actually has. A
    // fixed 16 MiB default silently measures cache, not DRAM, on anything with a
    // large last-level cache -- a 12 MiB LLC leaves a 16 MiB buffer only 1.3x
    // oversubscribed, which is nowhere near enough.
    if (!mem_explicit) {
        const size_t llc = detect_llc_bytes();
        if (llc > 0) {
            const int eff = per_core ? 1 : (threads > 0 ? threads : 1);
            const size_t want = (llc * 4) / (size_t)eff;
            if (want > mem_per_thread) mem_per_thread = want;
        }
    }
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
    // Selector-period sweep (diagnostic)
    // -----------------------------------------------------------------------
    if (disp_sweep) {
        opts.mem_per_thread = 0;
        worker_t *sw = (worker_t *)calloc((size_t)n_cpus, sizeof(*sw));
        if (!sw) { perror("calloc"); return 1; }
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
            json_open("disp-sweep", &opts, 1, pin, use_clock_raw);
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
            for (int variant = 0; variant < 2; ++variant) {
                printf("\n%s: calls per 1000 cycles (Mcall/s in parentheses)\n",
                       variant == 0 ? "serial acc (disp_kernel)"
                                    : "independent calls (disp_kernel_free)");
                printf("%8s", "period");
                for (int i = 0; i < n_cpus; ++i) printf("  %14s%d", "cpu", cpu_list[i]);
                printf("\n");
                for (int k = 0; k < DISP_SWEEP_N; ++k) {
                    printf("%8zu", g_disp_periods[k]);
                    for (int i = 0; i < n_cpus; ++i) {
                        const double r = variant == 0 ? sw[i].sweep_serial[k]
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
    // Per-core sweep
    // -----------------------------------------------------------------------
    if (per_core) {
        fprintf(msg(), "mode: per-core sweep over %d CPUs, %.2fs/phase x %d reps "
                       "(best kept),\n      warmup %.2fs, mem %zu MiB/thread\n\n",
                n_cpus, duration, reps, warmup, mem_per_thread >> 20);

        result_t *res = (result_t *)calloc((size_t)n_cpus, sizeof(*res));
        if (!res) { perror("calloc"); return 1; }

        if (g_format == FMT_TEXT) {
            printf("%4s %6s  %8s %8s %5s %8s  %8s %8s %5s  %7s %7s %5s  %8s %7s  %8s\n",
                   "CPU", "MHz", "INT-lat", "INT-thr", "ILP", "MUL-thr",
                   "FP-lat", "FP-thr", "fILP", "MEM", "MEMlat", "MLP",
                   "DISP-thr", "DISPcap", "score");
            printf("%4s %6s  %8s %8s %5s %8s  %8s %8s %5s  %7s %7s %5s  %8s %7s  %8s\n",
                   "", "", "Mops/s", "Mops/s", "x", "Mmul/s",
                   "Mflop/s", "Mflop/s", "x", "GB/s", "ns", "x",
                   "Mcall/s", "calls", "geomean");
        } else if (g_format == FMT_TSV) {
            printf(RESULT_COLUMNS);
        }

        double best = 0.0, worst = 0.0;
        double dram_lo = 0.0, dram_hi = 0.0;
        for (int i = 0; i < n_cpus; ++i) {
            worker_t w;
            memset(&w, 0, sizeof(w));
            w.cpu = pin ? cpu_list[i] : -1;
            if (run_workers(&w, 1, &opts) != 0) { free(res); return 1; }

            const result_t r = summarize(&w, cpu_list[i]);
            res[i] = r;
            if (r.score > best) best = r.score;
            if (worst == 0.0 || r.score < worst) worst = r.score;
            if (w.dram_mhz_observed > 0.0) {
                if (w.dram_mhz_observed > dram_hi) dram_hi = w.dram_mhz_observed;
                if (dram_lo == 0.0 || w.dram_mhz_observed < dram_lo) {
                    dram_lo = w.dram_mhz_observed;
                }
            }

            if (g_format == FMT_TEXT) {
                char mhzbuf[16], capbuf[16];
                if (r.mhz <= 0.0)          snprintf(mhzbuf, sizeof(mhzbuf), "?");
                else if (r.mhz_estimated)  snprintf(mhzbuf, sizeof(mhzbuf), "~%.0f", r.mhz);
                else                       snprintf(mhzbuf, sizeof(mhzbuf), "%.0f", r.mhz);
                disp_cap_str(r.disp_status, r.disp_cap, capbuf, sizeof(capbuf));
                printf("%4d %6s  %8.1f %8.1f %5.2f %8.1f  %8.1f %8.1f %5.2f  %7.2f %7.1f %5.2f  %8.1f %7s  %8.1f\n",
                       r.cpu, mhzbuf,
                       r.int_lat, r.int_thr, r.ilp, r.mul_thr,
                       r.fp_lat, r.fp_thr, r.filp,
                       r.mem_gbps, r.mem_lat_ns, r.mlp,
                       r.disp_thr, capbuf, r.score);
                fflush(stdout);
            } else if (g_format == FMT_TSV) {
                tsv_result("cpu", &r);
                fflush(stdout);
            }
        }

        if (g_format == FMT_JSON) {
            json_open("per-core", &opts, 1, pin, use_clock_raw);
            if (dram_hi > 0.0) {
                printf(",\n  \"dram\": { \"name\": \"%s\", \"mhz_min\": %.6g,"
                       " \"mhz_max\": %.6g }", g_dram_name, dram_lo, dram_hi);
            }
            printf(",\n  \"cores\": [\n");
            for (int i = 0; i < n_cpus; ++i) {
                json_result(&res[i], "cpu", "    ");
                printf("%s\n", i + 1 < n_cpus ? "," : "");
            }
            printf("  ]");
            if (worst > 0.0) printf(",\n  \"core_spread\": %.6g", best / worst);
            json_close();
        } else if (worst > 0.0) {
            fprintf(msg(), "\nfastest/slowest core ratio: %.2fx\n", best / worst);
        }
        if (g_format != FMT_JSON) report_dram(dram_lo, dram_hi);
        print_legend();
        free(res);
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

    fprintf(msg(), "config: threads=%d time=%.2fs/phase x%d reps warmup=%.2fs "
                   "mem=%zu MiB/thread pin=%s\n\n",
            threads, duration, reps, warmup, mem_per_thread >> 20,
            pin ? "on" : "off");

    if (run_workers(w, threads, &opts) != 0) { free(w); return 1; }

    result_t *res = (result_t *)calloc((size_t)threads, sizeof(*res));
    if (!res) { perror("calloc"); free(w); return 1; }

    // Aggregate: rates sum across threads, latencies and ratios average.
    result_t tot;
    memset(&tot, 0, sizeof(tot));
    tot.cpu = -1;
    double lat_n = 0, mlp_n = 0, cap_n = 0, mhz_n = 0;
    int cap_none = 0, cap_na = 0;
    double dram_lo = 0.0, dram_hi = 0.0;
    uint64_t checksum = 0;
    for (int i = 0; i < threads; ++i) {
        res[i] = summarize(&w[i], w[i].cpu);
        const result_t *r = &res[i];
        tot.int_lat  += r->int_lat;
        tot.int_thr  += r->int_thr;
        tot.mul_thr  += r->mul_thr;
        tot.fp_lat   += r->fp_lat;
        tot.fp_thr   += r->fp_thr;
        tot.mem_gbps += r->mem_gbps;
        tot.disp_thr += r->disp_thr;
        tot.disp_gain += r->disp_gain;
        tot.disp_span += r->disp_span;
        if (r->mhz > 0.0)         { tot.mhz += r->mhz; mhz_n++; }
        if (r->mhz_estimated)     tot.mhz_estimated = 1;
        if (r->mem_lat_ns > 0.0)  { tot.mem_lat_ns += r->mem_lat_ns; lat_n++; }
        if (r->mem_lat8_ns > 0.0) { tot.mem_lat8_ns += r->mem_lat8_ns; mlp_n++; }
        if (r->disp_status == DISP_OK && r->disp_cap > 0.0) {
            tot.disp_cap += r->disp_cap;
            cap_n++;
        } else if (r->disp_status == DISP_NONE) cap_none++;
        else cap_na++;
        const double d = w[i].dram_mhz_observed;
        if (d > 0.0) {
            if (d > dram_hi) dram_hi = d;
            if (dram_lo == 0.0 || d < dram_lo) dram_lo = d;
        }
        checksum ^= w[i].checksum;
    }
    const double n = (double)threads;
    if (mhz_n > 0) tot.mhz /= mhz_n;
    if (lat_n > 0) tot.mem_lat_ns /= lat_n;
    if (mlp_n > 0) tot.mem_lat8_ns /= mlp_n;
    if (cap_n > 0) tot.disp_cap /= cap_n;
    tot.disp_gain /= n;
    tot.disp_span /= n;
    tot.disp_status = cap_n > 0 ? DISP_OK : (cap_none > 0 ? DISP_NONE : DISP_NA);
    tot.ilp  = tot.int_lat > 0.0 ? tot.int_thr / tot.int_lat : 0.0;
    tot.filp = tot.fp_lat  > 0.0 ? tot.fp_thr  / tot.fp_lat  : 0.0;
    tot.mlp  = tot.mem_lat8_ns > 0.0 ? tot.mem_lat_ns / tot.mem_lat8_ns : 0.0;
    (void)cap_na;

    if (g_format == FMT_TSV) {
        printf(RESULT_COLUMNS);
        for (int i = 0; i < threads; ++i) tsv_result("thread", &res[i]);
        tsv_result("total", &tot);
        fprintf(msg(), "checksum: 0x%016llx\n", (unsigned long long)checksum);
    } else if (g_format == FMT_JSON) {
        json_open("threads", &opts, threads, pin, use_clock_raw);
        if (dram_hi > 0.0) {
            printf(",\n  \"dram\": { \"name\": \"%s\", \"mhz_min\": %.6g,"
                   " \"mhz_max\": %.6g }", g_dram_name, dram_lo, dram_hi);
        }
        printf(",\n  \"threads\": [\n");
        for (int i = 0; i < threads; ++i) {
            json_result(&res[i], "thread", "    ");
            printf("%s\n", i + 1 < threads ? "," : "");
        }
        printf("  ],\n  \"total\": ");
        json_result(&tot, "total", "");
        printf(",\n  \"checksum\": \"0x%016llx\"", (unsigned long long)checksum);
        json_close();
    } else {
        printf("%-9s %-23s %8s %11s %11s\n",
               "metric", "what it measures", "unit", "total", "per-thread");
        printf("%-9s %-23s %8s %11.1f %11.1f\n",
               "INT-lat", "integer latency", "Mops/s", tot.int_lat, tot.int_lat / n);
        printf("%-9s %-23s %8s %11.1f %11.1f\n",
               "INT-thr", "integer throughput", "Mops/s", tot.int_thr, tot.int_thr / n);
        printf("%-9s %-23s %8s %11.1f %11.1f\n",
               "MUL-thr", "multiply throughput", "Mmul/s", tot.mul_thr, tot.mul_thr / n);
        printf("%-9s %-23s %8s %11.1f %11.1f\n",
               "FP-lat", "FP latency", "Mflop/s", tot.fp_lat, tot.fp_lat / n);
        printf("%-9s %-23s %8s %11.1f %11.1f\n",
               "FP-thr", "FP throughput", "Mflop/s", tot.fp_thr, tot.fp_thr / n);
        printf("%-9s %-23s %8s %11.1f %11.1f\n",
               "DISP-thr", "indirect dispatch rate", "Mcall/s", tot.disp_thr, tot.disp_thr / n);
        if (tot.disp_status == DISP_OK) {
            printf("%-9s %-23s %8s %11s %11.0f\n",
                   "DISPcap", "indirect predictor size", "calls", "-", tot.disp_cap);
        } else if (tot.disp_status == DISP_NONE) {
            printf("%-9s %-23s %8s %11s %11s\n",
                   "DISPcap", "indirect predictor size", "calls", "-", "none");
        }
        if (mem_per_thread > 0) {
            printf("%-9s %-23s %8s %11.2f %11.2f\n",
                   "MEM", "memory bandwidth", "GB/s", tot.mem_gbps, tot.mem_gbps / n);
            if (lat_n > 0) {
                printf("%-9s %-23s %8s %11s %11.1f\n",
                       "MEMlat", "memory latency", "ns", "-", tot.mem_lat_ns);
            }
            if (mlp_n > 0) {
                printf("%-9s %-23s %8s %11s %11.1f\n",
                       "MEMlat/8", "latency, 8 chases", "ns", "-", tot.mem_lat8_ns);
            }
        }
        // Derived ratios: per-thread only, so the "total" column stays blank.
        printf("%-9s %-23s %8s %11s %11.2f\n", "ILP", "instruction parallelism",
               "x", "-", tot.ilp);
        printf("%-9s %-23s %8s %11s %11.2f\n", "fILP", "FP ops in flight",
               "x", "-", tot.filp);
        if (lat_n > 0 && mlp_n > 0) {
            printf("%-9s %-23s %8s %11s %11.2f\n", "MLP", "memory parallelism",
                   "x", "-", tot.mlp);
        }
        printf("\nchecksum: 0x%016llx\n", (unsigned long long)checksum);
    }

    if (g_format != FMT_JSON) report_dram(dram_lo, dram_hi);
    print_legend_ratios();

    free(res);
    free(w);
    return 0;
}
