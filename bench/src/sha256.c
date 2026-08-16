// SHA-256 (FIPS 180-4), just enough of it to identify one file.
//
// Here for a single field: `build.binary_sha256` in the result document. A hub
// can hold the digests of the binaries a release published and so tell a result
// produced by one of those from a result produced by a local build with unknown
// flags -- which is the difference between two numbers being comparable and
// merely being adjacent. Nothing here is a security boundary: the digest is
// self-reported like every other field, and a determined liar edits the JSON.
//
// Not a general-purpose implementation and not on any hot path: it hashes one
// file, once, and only when a run is about to report or print its identity.
// libc only, like the rest of the benchmark.

#include "sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t h[8];
    uint64_t bytes;             // total fed in, for the length block
    unsigned char buf[64];
    size_t used;                // bytes sitting in buf
} sha256_ctx;

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_init(sha256_ctx *c) {
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->bytes = 0;
    c->used = 0;
}

static void sha256_block(sha256_ctx *c, const unsigned char *p) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4u] << 24) | ((uint32_t)p[i * 4u + 1u] << 16)
             | ((uint32_t)p[i * 4u + 2u] << 8) | (uint32_t)p[i * 4u + 3u];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_update(sha256_ctx *c, const unsigned char *p, size_t n) {
    c->bytes += n;
    while (n > 0) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p += take;
        n -= take;
        if (c->used == 64) {
            sha256_block(c, c->buf);
            c->used = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, char out_hex[65]) {
    uint64_t bits = c->bytes * 8u;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    // The length goes in the last eight bytes of a block, so pad with zeros
    // until exactly that much room is left.
    unsigned char zero = 0;
    while (c->used != 56) sha256_update(c, &zero, 1);
    unsigned char len[8];
    for (unsigned i = 0; i < 8; i++) {
        len[i] = (unsigned char)((bits >> (56u - 8u * i)) & 0xffu);
    }
    // `bits` was taken before any of the padding, so what the counter does from
    // here does not matter -- nothing reads it again.
    sha256_update(c, len, 8);

    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 4; j++) {
            uint32_t byte = (c->h[i] >> (24u - 8u * j)) & 0xffu;
            out_hex[i * 8u + j * 2u]      = hex[byte >> 4];
            out_hex[i * 8u + j * 2u + 1u] = hex[byte & 0xfu];
        }
    }
    out_hex[64] = '\0';
}

int sha256_file(const char *path, char out_hex[65]) {
    out_hex[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    sha256_ctx c;
    sha256_init(&c);
    static unsigned char chunk[64 * 1024];   // one caller, one call, no thread
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) sha256_update(&c, chunk, n);
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        out_hex[0] = '\0';
        return -1;
    }
    sha256_final(&c, out_hex);
    return 0;
}
