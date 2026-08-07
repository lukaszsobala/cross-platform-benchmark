# cpu-bench

A tiny, portable CPU benchmark in C (C2x, GCC 13+) for common 64-bit architectures:
- x86_64 (amd64)
- AArch64 (ARMv8-A)
- RISC-V 64 (rv64imafdcvsu)

No external dependencies beyond libc, libm and pthreads.

## What it measures, and why

Each compute kernel is run in two variants built from the *same* op sequence:

| Metric | Meaning |
|---|---|
| `INT-lat` / `FP64-lat` | one dependency chain — per-op latency |
| `INT-thr` / `FP64-thr` | 8 independent chains — sustained throughput |
| `ILP` | `INT-thr/INT-lat` — reads out integer issue width (see below) |
| `MUL-thr` | 64-bit integer multiplies/s, measured on its own |
| `fILP` | `FP-thr/FP-lat` — FP ops in flight. *Not* a width measure, see below |
| `DISPATCH` | unpredictable indirect calls/s — indirect branch predictor + front end |
| `MEM` | sequential read+write bandwidth, GB/s |
| `MEMlat` | random-access latency, one dependent pointer chase |
| `MLP` | `MEMlat / latency with 8 chases in flight` — how much memory latency the core overlaps |

`ILP`, `DISPATCH` and `MLP` exist because a register-resident ALU loop is the
*best case* for a small in-order core and will badly underrate a big core. On a
4x Cortex-A55 + 4x Cortex-A76 board, the A76 leads the A55 by only ~1.3x per
clock on pure ALU throughput, but by ~4x on MLP. Real code depends on both.

### Keep the multiply out of the ILP kernel

A 64-bit multiply is the scarcest integer resource on most cores — one pipe,
often unpipelined. A kernel containing one is multiplier-bound in *both* the
latency and the throughput variant, so the ratio cancels and reports the same
number no matter how wide the core is. An earlier version of this benchmark did
exactly that and reported ILP 2.59 for a 2-wide in-order A55 and 2.64 for a
4-wide out-of-order A76 — no separation at all. Measured cause: both cores were
pinned to ~0.32 multiplies/cycle, which is *identical* between them.

The integer kernel is now four dependent add/xor/sub ops against registers —
one instruction and one cycle on every target ISA. `INT-lat` consequently pins
to 1.00 op/cycle, and `INT-thr` lands at ~90% of each core's ALU count, so `ILP`
reads out issue width: **1.78x on the A55 (2-wide), 2.82x on the A76 (3 ALUs)**.
Multiply capability is still measured, as its own `MUL-thr` number.

The FP kernel had the same defect in a worse form: a 2D rotation is 4 multiplies
to 2 adds, so it saturated the A76's single FP multiply pipe at 1.03 mul/cycle
and let the *little* A55 win per clock on `FP-thr`. It is now a balanced
`x = x*c + b` (one multiply, one add), which is also exactly one FMA when built
with `FMA=1`.

`fILP` is reported but must not be read as "bigger is better": FP latency varies
3–6 cycles between cores, so a core with slow FP needs more ops in flight to
fill its pipes and scores *higher*. Compare `FP-thr` for capability.

### Pitfalls this benchmark deliberately avoids

- **No `volatile` in a hot loop.** A volatile accumulator forces a store+reload
  of every intermediate, turning any kernel into a store-to-load-forwarding
  latency test. That is roughly constant across core widths, so a wide
  out-of-order core scores barely above an in-order one. Dead-code elimination
  is prevented with a checksum the caller reads instead.
- **FP state stays in normal range forever.** The FP kernel is a 2D rotation,
  which is norm-preserving up to rounding. A naive FP chain overflows to Inf
  within ~10 iterations and then measures NaN-propagation speed for the
  remaining billion — and NaN/denormal handling differs wildly between cores.
- **Latency and MLP chases have identical footprints.** Both walk the whole
  buffer (the 8-way chase is interleaved, not sliced), so the single-chain
  number cannot accidentally fit in last-level cache and inflate the ratio.
- **The memory phases warm up with memory traffic.** A pure-ALU warm-up does not
  ramp a DRAM controller that scales its clock (devfreq/`dmc_ondemand` spans a
  4x range on some SoCs), which otherwise causes ~2x swings between identical cores.
- **Best-of-N, not mean.** Interference can only make a run slower, so the
  fastest repetition is closest to the machine's true capability.

Note that "ops" are counted at the C level, not as machine instructions. AArch64
folds shifts into ALU operands, so one `INT_STEP` is 4 instructions there and
more on x86-64. That is intentional: the benchmark measures how fast a machine
performs a specified computation, and ISA efficiency is part of that.

## Build (GCC 13+)

```
make
```

You can pass custom flags, e.g. enable native tuning:

```
make CFLAGS="-O3 -march=native -mtune=native"
```

Or use the convenience make target or variables:

```
make native                    # uses -march/-mtune best-effort for host
make MARCH=x86-64-v3 MTUNE=generic
```

On some RISC-V toolchains, `-march=native` can emit an invalid ISA string. The `native` target auto-falls back to `-march=rv64gc -mtune=generic`. You can also set it explicitly:

```
make MARCH=rv64gc MTUNE=generic
```

RISC‑V notes:
- The canonical ISA string for compilers excludes privilege letters (`S`, `U`). So `rv64imafdcvsu` is invalid for `-march` because `su` are privilege levels, not ISA extensions. Use only ISA subsets and standard extensions in `-march`, and pass CSR/fence split via named extensions instead.
- For vector-capable toolchains: `make riscv-v` uses `-march=rv64gcv_zicsr_zifencei`.
- For Sophgo SG2000 (rv64imafdcv + CSR+fence split), use the provided target:

```
make sg2000
```

This maps to `-march=rv64imafdcv_zicsr_zifencei -mtune=generic`. Adjust `MTUNE` if your toolchain provides a specific tuner.
```

Fairness defaults (for cross-arch comparisons):
- Auto-vectorization disabled: `-fno-tree-vectorize`
- FMA contraction disabled: `-ffp-contract=off`

Enable for peak per-arch throughput:

```
make VECTORIZE=1                 # allow compiler vectorization
make VECTORIZE=1 FMA=1           # allow vectorization and FMA contraction
```

## Run

```
./cpu-bench --help
./cpu-bench                                  # all cores, all phases
./cpu-bench --per-core                       # sweep each CPU single-threaded (see below)
./cpu-bench --cpus 4-7 --threads 4           # only the big cluster
./cpu-bench --time 2.0 --reps 5              # longer and more repetitions
./cpu-bench --mem-per-thread 33554432        # 32 MiB working set per thread
./cpu-bench --no-mem                         # skip the memory phases
./cpu-bench --no-pin                         # disable thread pinning
```

Defaults: threads = online cores, `--time 0.5` per phase, `--reps 3`,
`--warmup 0.15`, pinned, `--clock raw`. The memory working set defaults to
`max(16 MiB, 4 x LLC / threads)` per thread, read from sysfs — a fixed 16 MiB
measures cache rather than DRAM on a machine with a 12 MiB last-level cache.

Where sysfs exposes no cpufreq node (some SoCs), the `MHz` column falls back to
an estimate derived from `INT-lat` and is marked with `~`. That works because
`INT-lat` is one dependent 1-cycle op per cycle by construction; it measured
0.97–1.01 op/cycle across Cortex-A53, A55, A76, Skymont and Lion Cove.

### Per-core sweep

On a heterogeneous machine (big.LITTLE, Intel P/E-cores) `--per-core` runs the
whole suite single-threaded on each CPU in turn and tabulates the result, so
core types can be compared directly. Both this table and the default
multi-threaded one name every metric in full underneath, so the shorthand
column headers do not have to be memorised:

```
 CPU    MHz   INT-lat  INT-thr   ILP  MUL-thr    FP-lat   FP-thr  fILP      MEM  MEMlat   MLP  DISPATCH     score
               Mops/s   Mops/s     x   Mmul/s   Mflop/s  Mflop/s     x     GB/s      ns     x   Mcall/s   geomean
   0   1800    1817.8   3230.7  1.78    582.2     454.5   2907.3  6.40    10.34   153.8  2.53     113.9    3331.2
   ...
   4   2400    2331.3   6581.3  2.82    777.4     932.9   4145.1  4.44    22.04   201.2 12.91     121.7    5810.7
```

For the most stable numbers, quiesce the machine first; `cpu-bench` prints a
warning if the load average suggests otherwise. It also warns if the working set
does not clear last-level cache, and reports the DRAM controller clock observed
during the memory phases (if it moved mid-run, the memory numbers carry that
spread; pin the governor to `performance`).

Compute phases repeat to within <1% across identical cores. `MEMlat` and the
`MLP` derived from it still vary ~20%, which is physical page placement: without
transparent huge pages a 16 MiB random chase spans 4096 pages and the DRAM
bank/rank distribution differs per allocation.


Notes:
- The numbers are synthetic and not directly comparable to industry benchmarks.
- The checksum is to discourage dead-code elimination and to confirm runs differ by config.
- For apples-to-apples comparisons, pin clocks and disable turbo and frequency scaling.
- Avoid using `-ffast-math` if you care about strict FP semantics; results will change.
- To enable memory, pass `--mem <bytes>` (split across threads). The memory phase uses a cache-line stride and read+write pattern; adjust `stride` in `bench.c` if needed.
- For apples-to-apples across ISAs, keep VECTORIZE/FMA consistent. Enabling RVV on RISC‑V while using baseline x86-64 will skew results.
