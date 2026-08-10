// The measured kernels. Compiled once per build variant.
//
// The Makefile compiles this file four times -- with and without
// -ftree-vectorize, crossed with -ffp-contract=off and =fast -- and passes the
// variant's identity in on the command line:
//
//   KV_ID         symbol suffix, e.g. _vector_fma
//   KV_NAME       the name the user types, e.g. "vector-fma"
//   KV_FLAGS      the exact extra flags this compilation got
//   KV_VECTORIZE  1 if -ftree-vectorize was one of them
//   KV_FMA        1 if -ffp-contract=fast was the other
//
// Every symbol carries KV_ID, so the four copies coexist in one binary and a
// disassembly says which variant a block of code belongs to. The driver picks
// one at runtime through the kernel_set_t each compilation defines.
//
// Every compute kernel exists in a LAT variant (one dependency chain) and a THR
// variant (LANES independent chains) built from the same op sequence, so thr/lat
// reads out how much parallelism the core extracts. No `volatile` in a hot loop
// (it would measure store-to-load forwarding); dead code is prevented by a
// checksum the caller reads. FP state stays in normal range for any iteration
// count, so no Inf/NaN/denormal timing penalties ever enter a measurement.

#include "kernels.h"

#if !defined(KV_ID) || !defined(KV_NAME) || !defined(KV_FLAGS) || \
    !defined(KV_VECTORIZE) || !defined(KV_FMA)
#error "kernels.c is compiled per variant; see bench/Makefile for the -D set"
#endif

// Paste KV_ID onto a base name. Two levels, so KV_ID expands before the ##.
#define KSYM__(a, b) a##b
#define KSYM_(a, b)  KSYM__(a, b)
#define KSYM(base)   KSYM_(base, KV_ID)

// ---------------------------------------------------------------------------
// Steps
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

// One multiply step, kept apart from the ALU kernel so that integer multiplier
// throughput is its own number instead of silently gating ILP.
// Odd * odd stays odd, so a lane can never collapse to zero.
#define MUL_STEP(a, m) do { (a) *= (m); } while (0)

// One FP step: one multiply and one add, so the kernel measures FP throughput
// rather than saturating whichever pipe it over-uses.
//
// x = x*c + b with |c| < 1 converges to the fixed point b/(1-c) and stays there,
// so the state is bounded for any iteration count and can never reach Inf, NaN
// or a denormal. With -ffp-contract=off this is fmul+fadd; with FMA enabled it
// folds to one FMA. Either way it is 2 FLOPs.
#define FP_STEP(x, c, b) do { (x) = (x) * (c) + (b); } while (0)

// ---------------------------------------------------------------------------
// Compute
// ---------------------------------------------------------------------------

// The k-constants come from the context rather than being literals, so the
// compiler cannot fold `+k1` and `-k3` together across the intervening xors.
NOINLINE static void KSYM(int_kernel_lat)(void *vctx, uint64_t n) {
    int_ctx_t *c = (int_ctx_t *)vctx;
    const uint64_t k1 = c->k[0], k2 = c->k[1];
    uint64_t a0 = c->l[0];
    for (uint64_t i = 0; i < n; ++i) {
        INT_STEP(a0, k1, k2);
    }
    c->l[0] = a0;
}

NOINLINE static void KSYM(int_kernel_thr)(void *vctx, uint64_t n) {
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
NOINLINE static void KSYM(mul_kernel_thr)(void *vctx, uint64_t n) {
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

NOINLINE static void KSYM(fp_kernel_lat)(void *vctx, uint64_t n) {
    fp_ctx_t *f = (fp_ctx_t *)vctx;
    const double co = f->c;
    const double b0 = f->b[0];
    double x0 = f->x[0];
    for (uint64_t i = 0; i < n; ++i) {
        FP_STEP(x0, co, b0);
    }
    f->x[0] = x0;
}

NOINLINE static void KSYM(fp_kernel_thr)(void *vctx, uint64_t n) {
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

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

// One unit = one full read+write sweep of the buffer.
NOINLINE static void KSYM(mem_kernel_bw)(void *vctx, uint64_t n) {
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
NOINLINE static void KSYM(mem_kernel_lat)(void *vctx, uint64_t n) {
    chase_ctx_t *ch = (chase_ctx_t *)vctx;
    void **p = ch->cur[0];
    for (uint64_t i = 0; i < n; ++i) {
        p = (void **)*p;
    }
    ch->cur[0] = p;
}

// One unit = CHASE_WAYS independent dependent loads issued together.
NOINLINE static void KSYM(mem_kernel_mlp)(void *vctx, uint64_t n) {
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

// ---------------------------------------------------------------------------
// Indirect dispatch
// ---------------------------------------------------------------------------

typedef uint64_t (*op_fn)(uint64_t);

// Four tiny leaf operations reached only through a runtime function pointer.
NOINLINE static uint64_t KSYM(op_add)(uint64_t v) { return v + UINT64_C(0x9E3779B9); }
NOINLINE static uint64_t KSYM(op_xor)(uint64_t v) { return v ^ (v >> 7); }
NOINLINE static uint64_t KSYM(op_mul)(uint64_t v) { return v * UINT64_C(2654435761); }
NOINLINE static uint64_t KSYM(op_rot)(uint64_t v) { return (v << 13) | (v >> 51); }

// Non-const and externally visible so the compiler cannot fold the indirect
// call back into a direct one. One table per variant, because each holds its
// own copies of the four leaves.
op_fn KSYM(g_op_table)[4] = {
    KSYM(op_add), KSYM(op_xor), KSYM(op_mul), KSYM(op_rot)
};

// One unit = one indirect call whose target is chosen by the selector stream.
// The selector array is walked cyclically with `mask` = period-1, so the target
// sequence repeats with period `mask + 1`.
//
// The result feeds the next call's argument. The *target* does not depend on
// `acc`, so this chain does not stop the predictor from running ahead.
NOINLINE static void KSYM(disp_kernel)(void *vctx, uint64_t n) {
    disp_ctx_t *d = (disp_ctx_t *)vctx;
    const uint8_t *idx = d->idx;
    const size_t mask = d->mask;
    size_t pos = d->pos;
    uint64_t acc = d->acc;
    for (uint64_t i = 0; i < n; ++i) {
        acc = KSYM(g_op_table)[idx[pos] & 3u](acc);
        pos = (pos + 1) & mask;
    }
    d->pos = pos;
    d->acc = acc;
}

// Same call site and selector stream, but the result is folded into a side
// accumulator instead of feeding the next argument, so consecutive calls are
// data-independent. Shows whether the serial `acc` chain caps the measurement.
NOINLINE static void KSYM(disp_kernel_free)(void *vctx, uint64_t n) {
    disp_ctx_t *d = (disp_ctx_t *)vctx;
    const uint8_t *idx = d->idx;
    const size_t mask = d->mask;
    size_t pos = d->pos;
    const uint64_t arg = d->acc | 1u;
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; ++i) {
        sum ^= KSYM(g_op_table)[idx[pos] & 3u](arg);
        pos = (pos + 1) & mask;
    }
    d->pos = pos;
    d->acc = sum;
}

// ---------------------------------------------------------------------------
// The variant this compilation produced
// ---------------------------------------------------------------------------

const kernel_set_t KSYM(kernels) = {
    .name      = KV_NAME,
    .flags     = KV_FLAGS,
    .vectorize = KV_VECTORIZE,
    .fma       = KV_FMA,

    .int_lat   = KSYM(int_kernel_lat),
    .int_thr   = KSYM(int_kernel_thr),
    .mul_thr   = KSYM(mul_kernel_thr),
    .fp_lat    = KSYM(fp_kernel_lat),
    .fp_thr    = KSYM(fp_kernel_thr),
    .mem_bw    = KSYM(mem_kernel_bw),
    .mem_lat   = KSYM(mem_kernel_lat),
    .mem_mlp   = KSYM(mem_kernel_mlp),
    .disp      = KSYM(disp_kernel),
    .disp_free = KSYM(disp_kernel_free),
};
