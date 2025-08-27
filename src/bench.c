// Simple portable CPU benchmark (C)
// - Focus on integer, floating-point, and memory throughput
// - Multi-threaded using pthreads (no external deps besides libc/pthreads)
// - Targets 64-bit: amd64, arm64 (ARMv8-A), riscv64 (rv64imafdcvsu)
// - Uses clock_gettime(CLOCK_MONOTONIC) for timing
// - Avoids arch-specific intrinsics for portability
// - Deterministic workload seeded PRNG

#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
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
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#endif

#ifndef ARRAY_LEN
#define ARRAY_LEN(x) ((int)(sizeof(x) / sizeof((x)[0])))
#endif

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

static void warmup_spin(double sec) {
    if (sec <= 0.0) return;
    volatile uint64_t x0=1,x1=2,x2=3,x3=4,x4=5,x5=6,x6=7,x7=8;
    const double t_end = now_sec() + sec;
    while (now_sec() < t_end) {
        x0 += x1; x2 += x3; x4 += x5; x6 += x7;
        x1 ^= x0; x3 ^= x2; x5 ^= x4; x7 ^= x6;
    }
    (void)x0;(void)x1;(void)x2;(void)x3;(void)x4;(void)x5;(void)x6;(void)x7;
}

// Xorshift64* PRNG for deterministic sequences
static inline uint64_t xs64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * UINT64_C(2685821657736338717);
}

static inline uint32_t u64_to_u32(uint64_t x) { return (uint32_t)(x >> 32) ^ (uint32_t)x; }

// Per-thread configuration and results
typedef struct {
    int id;
    int cpu_work;        // millions of ops chunk size per loop (approx)
    size_t mem_bytes;    // memory bytes to touch per thread
    double seconds;      // runtime per phase
    double warmup;       // warm-up seconds per phase
    int pin;             // pin thread to CPU
    pthread_barrier_t *bar; // barrier to align phase starts
    volatile uint64_t checksum;

    // results
    double int_mops;     // integer ops per second (million)
    double fp_mflops;    // floating point ops per second (million)
    double mem_gbps;     // memory bandwidth GB/s (read+write)
} worker_result_t;

typedef struct {
    int threads;
    double duration; // seconds per phase
    unsigned seed;
    size_t mem_bytes; // total memory size to touch (split across threads); 0 disables mem phase
} bench_config_t;

static void *worker(void *arg) {
    worker_result_t *r = (worker_result_t *)arg;

    // Make a private buffer for memory benchmark
    size_t nbytes = r->mem_bytes;
    uint8_t *buf = NULL;
    if (nbytes > 0) {
        int rc = posix_memalign((void **)&buf, 64, nbytes);
        if (rc != 0 || !buf) {
            perror("posix_memalign");
            pthread_exit(NULL);
        }
        memset(buf, 0, nbytes);
    }

#ifdef __linux__
    // Optionally pin thread to a CPU to reduce jitter
    if (r->pin) {
        cpu_set_t set;
        CPU_ZERO(&set);
        long n = sysconf(_SC_NPROCESSORS_CONF);
        if (n < 1) n = 1;
    CPU_SET((unsigned int)(r->id % (int)n), &set);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
#endif

    // Prefault per-thread buf (memset already did, keeping cheap touch here)
    if (buf && nbytes >= 4096) {
        for (size_t i = 0; i < nbytes; i += 4096) {
            buf[i] ^= (uint8_t)i;
        }
    }

    // Align and warm up before each phase handled by global helper

    // Integer arithmetic loop
    {
        if (r->bar) pthread_barrier_wait(r->bar);
        warmup_spin(r->warmup);
        if (r->bar) pthread_barrier_wait(r->bar);
    const double t_start = now_sec();
    const double t_end = t_start + r->seconds;
    uint64_t s = UINT64_C(0x9E3779B97F4A7C15) ^ (uint64_t)(unsigned)r->id;
        volatile uint64_t acc = 0;
        uint64_t iters = 0;
        const int check_every = 1024;
    const uint64_t check_mask = (uint64_t)check_every - 1u;
        while (1) {
            // Unrolled integer ops, ~32 ops per iteration
            uint64_t a = xs64(&s);
            uint64_t b = xs64(&s);
            acc += (a ^ b) + (a & b);
            acc ^= (a + 0x9E37) * (b | 1);
            acc += (acc << 13) ^ (acc >> 7);
            acc ^= (acc << 17) + (acc >> 11);
            acc += (uint64_t)(iters * 1315423911u);
            // mix a bit more
            acc = (acc * UINT64_C(11400714819323198485)) ^ (acc >> 33);
            iters++;
            if ((iters & check_mask) == 0) {
                if (now_sec() >= t_end) break;
            }
        }
    r->checksum ^= acc;
    // Estimate ops: ~32 per iter (conservative)
    double elapsed = now_sec() - t_start;
    if (elapsed <= 0.0) elapsed = r->seconds;
    r->int_mops = (double)(iters * 32) / 1e6 / elapsed;
    }

    // Floating point loop (double precision)
    {
        if (r->bar) pthread_barrier_wait(r->bar);
        warmup_spin(r->warmup);
        if (r->bar) pthread_barrier_wait(r->bar);
    const double t_start = now_sec();
    const double t_end = t_start + r->seconds;
        volatile double fa = 1.0, fb = 1.0000001, fc = 0.9999997, fd = 1.0000003;
        uint64_t iters = 0;
        const int check_every = 1024;
    const uint64_t check_mask = (uint64_t)check_every - 1u;
    while (1) {
            // ~32 FLOPs per iteration (adds, muls, fmadd-ish patterns)
            fa = fa * fb + fc;
            fb = fb * fd + fa;
            fc = fc * fb - fd;
            fd = fd * fa - fc;

            fa += 1.0; fb -= 0.5; fc += 0.25; fd -= 0.125;
            fa *= 0.9999997; fb *= 1.0000003; fc *= 1.0000001; fd *= 0.9999999;

            // keep values finite
            if (fa > 1e100) { fa *= 1e-100; }
            if (fb > 1e100) { fb *= 1e-100; }
            if (fc < -1e100) { fc *= 1e-100; }
            if (fd < -1e100) { fd *= 1e-100; }

            iters++;
            if ((iters & check_mask) == 0) {
                if (now_sec() >= t_end) break;
            }
        }
    r->checksum ^= (uint64_t)(fa + fb + fc + fd);
    // Estimate flops: each loop ~32 FLOPs
    double elapsed = now_sec() - t_start;
    if (elapsed <= 0.0) elapsed = r->seconds;
    r->fp_mflops = (double)(iters * 32) / 1e6 / elapsed;
    }

    // Memory bandwidth (read + write). We walk the whole buffer per pass and
    // only check the clock after a pass to reduce timing overhead.
    if (buf && nbytes > 0) {
        if (r->bar) pthread_barrier_wait(r->bar);
        warmup_spin(r->warmup);
        if (r->bar) pthread_barrier_wait(r->bar);
    const double t_start = now_sec();
    const double t_end = t_start + r->seconds;
        volatile uint64_t acc = 0;
        size_t touched = 0;

        while (1) {
            // Full-buffer pass with cache-line stride
            const size_t stride = 64;
            for (size_t pos = 0; pos < nbytes; pos += stride) {
                uint64_t *p = (uint64_t *)(buf + pos);
                size_t qwords = stride / sizeof(uint64_t);
                for (size_t i = 0; i < qwords; ++i) {
                    uint64_t v = p[i]; // read
                    acc += v;
                    p[i] = v + acc;    // write
                }
                touched += stride * 2; // read + write
            }
            if (now_sec() >= t_end) break;
        }
    r->checksum ^= acc;
    double elapsed = now_sec() - t_start;
    if (elapsed <= 0.0) elapsed = r->seconds;
    r->mem_gbps = (double)touched / (double)(1ull << 30) / elapsed;
    } else {
        r->mem_gbps = 0.0;
    }

    if (buf) free(buf);
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--threads N] [--time SECONDS] [--mem BYTES] [--no-mem]\\n"
            "           [--warmup SECONDS] [--no-pin] [--clock mono|raw]\\n"
            "Defaults: --threads <cores or 1>, --time 1.0, --mem 0 (memory phase disabled)\\n"
            "          --warmup 0.25, pinned threads, clock=raw (if available)\\n",
            prog);
}

static int get_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 256) n = 256; // clamp sane default max
    return (int)n;
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

static void detect_cpu_model(char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) return;
    buf[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[512];
    const char *keys[] = {"model name", "Processor", "cpu model", "Hardware"};
    while (fgets(line, (int)sizeof(line), f)) {
        for (size_t k = 0; k < sizeof(keys)/sizeof(keys[0]); ++k) {
            const char *p = strstr(line, keys[k]);
            if (p && (p == line || (p > line && p[-1] == '\n'))) {
                const char *colon = strchr(line, ':');
                if (colon) {
                    colon++; while (*colon == ' ' || *colon == '\t') colon++;
                    // trim newline
                    size_t len = strnlen(colon, sizeof(line));
                    while (len > 0 && (colon[len-1] == '\n' || colon[len-1] == '\r')) len--;
                    size_t n = len < bufsz-1 ? len : bufsz-1;
                    memcpy(buf, colon, n);
                    buf[n] = '\0';
                    fclose(f);
                    return;
                }
            }
        }
    }
    fclose(f);
}

static void print_build_runtime_info(int cores) {
    // Compiler/version
#if defined(__clang__)
    const char *cc = "clang";
    int cc_major = __clang_major__;
    int cc_minor = __clang_minor__;
    int cc_patch = __clang_patchlevel__;
#elif defined(__GNUC__)
    const char *cc = "gcc";
    int cc_major = __GNUC__;
    int cc_minor = __GNUC_MINOR__;
    int cc_patch = __GNUC_PATCHLEVEL__;
#else
    const char *cc = "cc";
    int cc_major = 0, cc_minor = 0, cc_patch = 0;
#endif

    long stdver = 0;
#ifdef __STDC_VERSION__
    stdver = __STDC_VERSION__;
#endif

    struct utsname u;
    u.sysname[0] = '\0';
    if (uname(&u) != 0) {
        memset(&u, 0, sizeof(u));
    }
    char cpu_model[256];
    detect_cpu_model(cpu_model, sizeof(cpu_model));

    printf("build: %s %d.%d.%d, C%ld, target=%s\n",
           cc, cc_major, cc_minor, cc_patch, stdver, arch_string());
    if (u.sysname[0]) {
        printf("system: %s %s %s, machine=%s, cores=%d\n",
               u.sysname, u.release, u.version, u.machine, cores);
    } else {
        printf("system: cores=%d\n", cores);
    }
    if (cpu_model[0]) {
        printf("cpu: %s\n", cpu_model);
    }
}

int main(int argc, char **argv) {
    bench_config_t cfg = {
        .threads = get_cpu_count(),
        .duration = 1.0,
        .seed = 1u,
        .mem_bytes = 0 // memory phase disabled by default
    };
    double warmup = 0.25;
    int pin = 1;
    int use_clock_raw = 1;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            cfg.threads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
            cfg.duration = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--mem") && i + 1 < argc) {
            cfg.mem_bytes = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--no-mem")) {
            cfg.mem_bytes = 0;
        } else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) {
            warmup = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--no-pin")) {
            pin = 0;
        } else if (!strcmp(argv[i], "--clock") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "mono")) use_clock_raw = 0;
            else if (!strcmp(m, "raw")) use_clock_raw = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.threads < 1) cfg.threads = 1;
    if (cfg.duration < 0.1) cfg.duration = 0.1;
    if (cfg.mem_bytes > 0 && cfg.mem_bytes < (size_t)1024) cfg.mem_bytes = (size_t)1024;

    set_clock_mode(use_clock_raw);
    print_build_runtime_info(cfg.threads);

    printf("cpu-bench: threads=%d time=%.3fs mem_total=%zu bytes\n", cfg.threads, cfg.duration, cfg.mem_bytes);

    pthread_t *ths = (pthread_t *)calloc((size_t)cfg.threads, sizeof(*ths));
    worker_result_t *res = (worker_result_t *)calloc((size_t)cfg.threads, sizeof(*res));
    if (!ths || !res) { perror("calloc"); return 1; }

    const size_t per_thread_mem = cfg.mem_bytes / (size_t)cfg.threads;

    pthread_barrier_t bar;
    if (cfg.threads > 1) {
        if (pthread_barrier_init(&bar, NULL, (unsigned)cfg.threads) != 0) {
            perror("pthread_barrier_init");
            return 1;
        }
    }

    for (int t = 0; t < cfg.threads; ++t) {
        res[t].id = t;
        res[t].cpu_work = 1; // reserved, not used in this version
        res[t].mem_bytes = per_thread_mem;
    res[t].seconds = cfg.duration;
    res[t].warmup = warmup;
    res[t].pin = pin;
    res[t].bar = (cfg.threads > 1) ? &bar : NULL;
        res[t].checksum = 0;
        int rc = pthread_create(&ths[t], NULL, worker, &res[t]);
        if (rc != 0) { errno = rc; perror("pthread_create"); return 1; }
    }

    for (int t = 0; t < cfg.threads; ++t) {
        int rc = pthread_join(ths[t], NULL);
        if (rc != 0) { errno = rc; perror("pthread_join"); return 1; }
    }

    double int_mops = 0.0, fp_mflops = 0.0, mem_gbps = 0.0;
    uint64_t checksum = 0;
    for (int t = 0; t < cfg.threads; ++t) {
        int_mops += res[t].int_mops;
        fp_mflops += res[t].fp_mflops;
        mem_gbps += res[t].mem_gbps;
        checksum ^= res[t].checksum;
    }

    printf("INT  : %10.1f MOPS\n", int_mops);
    printf("FP64 : %10.1f MFLOPS\n", fp_mflops);
    if (cfg.mem_bytes > 0) {
        printf("MEM  : %10.2f GB/s\n", mem_gbps);
    }
    printf("CHK  : 0x%016llx\n", (unsigned long long)checksum);

    if (cfg.threads > 1) {
        (void)pthread_barrier_destroy(&bar);
    }
    free(ths);
    free(res);
    return 0;
}
