// The measured kernels, and the contract the driver reaches them through.
//
// Auto-vectorization and FMA contraction are translation-unit flags, not
// something a pragma can switch per function, so a binary that carries more
// than one of them has to compile the kernels more than once. This header is
// the interface: bench.c holds the driver and sees only kernel_set_t, while
// kernels.c is compiled once per variant with its own -ftree-vectorize /
// -ffp-contract pair and defines one kernel_set_t per compilation.
//
// Everything a kernel touches is declared here, because the driver builds the
// contexts and the kernels consume them across the TU boundary.

#ifndef CPCPUB_KERNELS_H
#define CPCPUB_KERNELS_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

// Independent chains in the throughput kernels: enough to saturate the widest
// core targeted, few enough to stay in the register file of a 16-GPR ISA.
#define LANES 8

// CHASE_WAYS independent pointer-chase cycles. One cycle measures raw
// random-access latency; all of them in lockstep measures how much of that
// latency the core overlaps. The speedup can exceed CHASE_WAYS because the
// serial chase also serializes TLB page-table walks.
#define CHASE_WAYS 8

// Ops per step, counted at the C level. The driver turns iterations into rates
// with these, so they belong to the kernel definitions and are declared beside
// them; the steps themselves are in kernels.c.
#define INT_OPS_PER_STEP 4u
#define MUL_OPS_PER_STEP 1u
#define FP_OPS_PER_STEP  2u

typedef struct { uint64_t l[LANES]; uint64_t k[2]; } int_ctx_t;
typedef struct { uint64_t l[LANES]; uint64_t m; } mul_ctx_t;
typedef struct { double x[LANES], b[LANES], c; } fp_ctx_t;

typedef struct {
    uint64_t *buf;
    size_t    nqwords;
    uint64_t  acc[4];
} mem_ctx_t;

typedef struct {
    void   **cur[CHASE_WAYS];
} chase_ctx_t;

// Indirect dispatch through a table of tiny functions selected by data. Cannot
// be if-converted or devirtualized, so it exercises the indirect branch
// predictor and the front end.
typedef struct {
    const uint8_t *idx;
    size_t         mask;
    size_t         pos;
    uint64_t       acc;
} disp_ctx_t;

// A kernel runs `n` units of work and returns; the driver times batches of them.
typedef void (*kernel_fn)(void *ctx, uint64_t n);

// One compilation of the kernels, with the flags it was compiled with. `flags`
// and the two booleans are baked in by the Makefile from the same variables it
// puts on the command line, so a result cannot claim a build it did not get.
typedef struct {
    const char *name;       // "vector-fma"
    const char *flags;      // "-ftree-vectorize -ffp-contract=fast"
    int         vectorize;
    int         fma;

    kernel_fn int_lat;
    kernel_fn int_thr;
    kernel_fn mul_thr;
    kernel_fn fp_lat;
    kernel_fn fp_thr;
    kernel_fn mem_bw;
    kernel_fn mem_lat;
    kernel_fn mem_mlp;
    kernel_fn disp;
    kernel_fn disp_free;
} kernel_set_t;

// The four compilations, in the order the driver runs them: the fairness
// baseline first, then one toggle at a time, then both.
extern const kernel_set_t kernels_scalar_nofma;
extern const kernel_set_t kernels_vector_nofma;
extern const kernel_set_t kernels_scalar_fma;
extern const kernel_set_t kernels_vector_fma;

// Whether either toggle can change code generation for the -march this binary
// was built for. Both are properties of the ISA, not of the toggles, so they
// read the same in every variant and the driver can evaluate them once.
//
// Without a vector unit, -ftree-vectorize has nothing to emit; without a
// hardware fused multiply-add, -ffp-contract=fast has nothing to contract into.
// In either case the two variants are the same machine code and running both
// measures the same build twice. GCC defines __FP_FAST_FMA exactly when an FMA
// is one instruction, which is the question being asked here.
#if defined(__SSE2__) || defined(__AVX__) || defined(__aarch64__) || \
    defined(__ARM_NEON) || defined(__riscv_vector) || \
    defined(__riscv_xtheadvector) || \
    defined(__loongarch_sx) || defined(__loongarch_asx)
#define TARGET_HAS_SIMD 1
#else
#define TARGET_HAS_SIMD 0
#endif

#ifdef __FP_FAST_FMA
#define TARGET_HAS_FMA 1
#else
#define TARGET_HAS_FMA 0
#endif

#endif  // CPCPUB_KERNELS_H
