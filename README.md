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
| `DISP-thr` | indirect calls/s once the target pattern is learned — front end |
| `DISPcap` | longest repeating call pattern the core still predicts, in calls — indirect predictor capacity (see below) |
| `MEM` | sequential read+write bandwidth, GB/s |
| `MEMlat` | random-access latency, one dependent pointer chase |
| `MLP` | `MEMlat / latency with 8 chases in flight` — how much memory latency the core overlaps |

`ILP`, `DISPcap` and `MLP` exist because a register-resident ALU loop is the
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

### Don't measure indirect prediction with random targets

The dispatch phase had the same class of defect. It walked 64 KiB of uniformly
random selectors and called one of four functions through a function pointer. A
random target sequence is unpredictable for *every* predictor ever built, so the
loop never measured prediction accuracy — it measured the cost of a mispredict,
which is pipeline-depth-dominated. It therefore ranked cores by how *shallow*
they were: per clock, the in-order Cortex-A55 beat everything (63.4 calls/kcycle
against 50.8 for the A76 and 40.5 for Lion Cove).

What separates a big front end from a little one is predictor *capacity*: the
length of deterministic pattern it can still learn. So the selector stream is now
a fixed random sequence of period `L`, repeated, and `L` is swept by powers of
two from 8 to 8192. Small `L`: every core predicts it. Large `L`: no core does.
Where a core falls off in between is the read-out. Measured on an Intel Core
Ultra 7 258V (Lunar Lake), which has a big and a little core in one package at a
known clock ratio:

| period `L` | Lion Cove P, calls/kcycle | Skymont E, calls/kcycle |
|---|---|---|
| 8 | 213.5 | 198.3 |
| 128 | 210.5 | 198.2 |
| 256 | 211.1 | 186.7 |
| 512 | 210.9 | 137.8 |
| 1024 | 108.7 | 61.8 |
| 4096 | 41.8 | 41.6 |
| 65536 (random) | 39.4 | 38.8 |

Both cores land within 2% of each other at the random end — that is the old
metric, and it is nearly blind. Lion Cove holds its plateau to `L`≈512 and
Skymont to `L`≈256, a 2x difference in the length of call pattern the front end
can hold.

`DISPcap` turns that curve into one number without a magic threshold. Each rate
is normalised between the plateau and the fully-random floor, giving `q` ∈ [0,1]
= "how much of the available prediction win is this core still getting", and `q`
is summed over the span `L` = 8…16384, which is a factor of two per step:

```
DISPcap = 8 * 2^(sum(q) - 1)      # calls
```

A core that predicts everything up to `L` and nothing beyond reads out exactly
`L`. Dividing the floor out is what removes the old bias, because the floor *is*
the mispredict penalty. Measured across five cores on three machines and two
ISAs, all per clock:

| core | machine | MHz | old metric, calls/kcycle | `DISP-thr`, calls/kcycle | `DISPcap`, calls |
|---|---|---|---|---|---|
| T-Head C906 | SG2000 | 663 | 72.7 | 73.3 | **none** |
| Cortex-A53 | SG2000 | 964 | 50.9 | 74.5 | 23 |
| Cortex-A55 | RK3588 | 1800 | **63.4** | 97.6 | 31 |
| Cortex-A76 | RK3588 | 2400 | 50.8 | 138.9 | **3023** |
| Skymont E | 258V | 3701 | 38.8 | 199 | 418 |
| Lion Cove P | 258V | 4800 | 39.4 | 236 | 550 |

The old column is in bold where it was worst: the little A55 led every core on
it, and the A76 came *below* the A53. `DISPcap` orders them the way the
microarchitectures do, and the two big/little pairs both come out right per
clock — A76 over A55 by 97x, Lion Cove over Skymont by 1.32x.

Three things in that table are worth reading carefully:

- **The A76 beats Lion Cove by 5x.** It holds full rate to `L` = 2048 where Lion
  Cove breaks at 1024. Part of that is a genuinely long-history indirect
  predictor, but part is slack: the A76 needs 7.2 cycles per predicted call
  against Lion Cove's 4.2, so it has more room to absorb an occasional
  mispredict before the *time* moves. `DISPcap` measures the longest pattern
  dispatched at full rate, and that is not quite the same thing as accuracy.
  The fall itself is real and sharp, not a slack artefact — the A76 is at 67% of
  plateau by 4096 and 41% by 8192.
- **`none` for the C906** means the core was no faster calling one single
  repeated target than calling a random stream: gain 1.003 across the entire
  sweep. It has no usable indirect target prediction, so there is no capacity to
  report, and its `DISP-thr` of 72.9 is a mispredict cost rather than a
  prediction rate. That verdict comes from a period-1 reference point, which
  every core with a BTB predicts; without it, a flat curve cannot be told apart
  from a core that outruns the ladder.
- **`DISP-thr` alone would not have fixed anything.** It separates Lion Cove
  from Skymont by only 1.19x per clock, and rates the C906 above the A53.

A 90%-of-plateau threshold crossing was tried first and gave a larger separation
on the 258V but is not reproducible: the Skymont knee sits at 94% of plateau, so
noise slides the crossing between ladder intervals and one core returned
206–322 calls across repeat runs.

**Measure `DISPcap` on a quiet machine.** The plateau is throughput-bound and
speeds up when the machine is idle; the mispredicting end of the curve is
flush-bound and does not. Background load therefore compresses the two together
and *inflates* the result — the same P-core measured 546 at load 0.7 and 745 at
load 1.7. Raising `--reps` does not fix continuous interference. Within one
quiet run the P-cores repeat to ~2% and the E-cores to ~10%, the E-cores being
worse because their knee sits on a steeper part of the curve.

Run `./cpu-bench --disp-sweep` to get the whole curve on any target. The floor
has to be the genuinely random point: an earlier version ended the ladder at
8192 and treated that as the floor because it is within 2% of random on both
258V core types, but on the A76 it is 13% high, and a floor set too high
compresses every `q` beneath it.

`DISP-thr` is the plateau of that curve, i.e. call/return throughput with the
pattern learned: 4.7 cycles/call on Lion Cove against 5.0 on Skymont. It is a
front-end throughput and is bigger-is-better, but it separates core sizes far
less than capacity does, which is why both numbers are reported.

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
- For hardware implementing **ratified RVV 1.0**: `make riscv-v` uses `-march=rv64gcv_zicsr_zifencei`.
- For Sophgo SG2000 (T-Head C906):

```
make sg2000
```

This maps to `-march=rv64imafdc_zicsr_zifencei`, with **no `v`**. The C906
implements the draft 0.7.1 vector extension, which is a different, incompatible
encoding from ratified RVV 1.0 — and `v` in a GCC `-march` string means RVV 1.0.
Building `rv64imafdcv` for this chip produces a binary that dies with
`Illegal instruction`, and `-fno-tree-vectorize` does not save you: the compiler
also uses vector registers to inline `memset`/`memcpy` and struct copies, so the
first casualty is usually a plain struct initialiser, long before any kernel
runs. Nothing is lost by dropping it — the default build is scalar on every ISA
for fairness. If you want 0.7.1 as the hardware actually implements it (only
useful with `VECTORIZE=1`, and needs GCC 14+), use `make sg2000-xthead`, which
adds `_xtheadvector`.

Fairness defaults (for cross-arch comparisons):
- Auto-vectorization disabled: `-fno-tree-vectorize`
- FMA contraction disabled: `-ffp-contract=off`

Enable for peak per-arch throughput:

```
make VECTORIZE=1                 # allow compiler vectorization
make VECTORIZE=1 FMA=1           # allow vectorization and FMA contraction
```

### Illegal instruction, or an `-march` that seems to have no effect

Object files are stamped with the flags they were built with (`.build-flags`),
so switching `MARCH` forces a rebuild. Older checkouts did not do this: `make`
after `make sg2000` found `src/bench.o` up to date and silently kept the object
built for the *previous* ISA, so the re-target appeared to succeed while the
binary never changed — including the case where the retained object was the one
containing instructions the CPU does not implement. If in doubt:

```
make clean && make sg2000
objdump -d cpu-bench | grep -c vsetvli    # 0 = no RVV in the binary
```

## Run

```
./cpu-bench --help
./cpu-bench                                  # all cores, all phases
./cpu-bench --per-core                       # sweep each CPU single-threaded (see below)
./cpu-bench --disp-sweep                     # indirect-dispatch curve vs selector period
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
 CPU    MHz   INT-lat  INT-thr   ILP  MUL-thr    FP-lat   FP-thr  fILP      MEM  MEMlat   MLP  DISP-thr DISPcap     score
               Mops/s   Mops/s     x   Mmul/s   Mflop/s  Mflop/s     x     GB/s      ns     x   Mcall/s   calls   geomean
   0   4700    4681.6  24634.8  5.26  11407.3    1559.5  11803.5  7.57    36.34   101.4  8.52    1110.0     546   18499.8
   3   4800    4781.9  25166.5  5.26  11645.3    1594.1  12051.1  7.56    36.07   102.0  8.60    1134.6     549   18772.5
   4   3701    3685.1  20558.4  5.58   4449.7    1474.1   9907.3  6.72    42.42   119.4  8.72     737.0     426   13493.1
   7   3701    3684.5  20547.5  5.58   4448.2    1473.3   9906.6  6.72    43.30   120.0  8.90     736.9     387   13485.2
```

(Core Ultra 7 258V: CPUs 0–3 are Lion Cove P-cores, 4–7 Skymont E-cores. Per
clock the P-core leads by 2.02x on `MUL-thr`, 1.19x on `DISP-thr` and 1.32x on
`DISPcap`, while Skymont is marginally *ahead* per clock on plain ALU and FP
throughput — its 8 ALUs against Lion Cove's 6. Most of the P-core's real-world
advantage here is clock, multiply and front end, which is exactly why the suite
reports those separately.)

For the most stable numbers, quiesce the machine first; `cpu-bench` prints a
warning if the load average suggests otherwise. It also warns if the working set
does not clear last-level cache, and reports the DRAM controller clock observed
during the memory phases (if it moved mid-run, the memory numbers carry that
spread; pin the governor to `performance`).

Compute phases repeat to within <1% across identical cores. `MEMlat` and the
`MLP` derived from it still vary ~20%, which is physical page placement: without
transparent huge pages a 16 MiB random chase spans 4096 pages and the DRAM
bank/rank distribution differs per allocation. `DISPcap` repeats to ~2% on a
big core and ~10% on a little one, whose knee sits on a steeper part of the
curve, and it is the one column that is biased rather than just noised by
background load (see above) — quiesce the machine for it. Raise `--time` if
you want tighter numbers; the whole dispatch ladder scales with it, each of its
14 points getting half a phase.


Notes:
- The numbers are synthetic and not directly comparable to industry benchmarks.
- The checksum is to discourage dead-code elimination and to confirm runs differ by config.
- For apples-to-apples comparisons, pin clocks and disable turbo and frequency scaling.
- Avoid using `-ffast-math` if you care about strict FP semantics; results will change.
- To enable memory, pass `--mem <bytes>` (split across threads). The memory phase uses a cache-line stride and read+write pattern; adjust `stride` in `bench.c` if needed.
- For apples-to-apples across ISAs, keep VECTORIZE/FMA consistent. Enabling RVV on RISC‑V while using baseline x86-64 will skew results.
