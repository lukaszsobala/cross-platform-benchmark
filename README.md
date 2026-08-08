# cpu-bench

A small, portable CPU benchmark in C (C2x, GCC 13+) for 64-bit x86_64, AArch64
and RISC-V. No dependencies beyond libc, libm and pthreads.

## How it works

The suite runs a set of tiny kernels, each for a fixed wall-clock slice, and
reports the rate achieved. Four things shape the design:

**Latency and throughput are measured separately.** Every compute kernel exists
in two variants built from the *same* op sequence: one dependency chain (latency)
and 8 independent chains (throughput). Their ratio is how much instruction-level
parallelism the core extracts, which is the main thing that separates a wide
out-of-order core from a narrow in-order one. A single-chain benchmark rates them
almost equally.

**Each kernel isolates one resource.** The integer kernel is add/xor/sub only —
one instruction, one cycle, on every target ISA — so its latency variant pins to
1 op/cycle and the ratio reads out issue width. Integer multiply is the scarcest
resource on most cores, so a kernel containing one is multiplier-bound in *both*
variants and the ratio cancels; multiply therefore gets its own phase. The FP
kernel is one multiply and one add in balance, for the same reason.

**Timing is batched and repeated.** Kernels run in batches sized so that each
lands near 4 ms, which keeps `clock_gettime` out of the measured work. Each phase
is repeated (`--reps`, default 3) and the *best* run is kept: interference can
only ever make a run slower, so the fastest run is closest to the machine's real
capability, and averaging would just fold the noise in.

**Threads are pinned and barrier-synchronised.** Every thread warms up, then
lines up at a barrier so all of them start measuring the same phase at the same
instant. The memory phases warm up with real memory traffic, because a pure-ALU
spin does not ramp a DRAM controller whose governor scales the DDR clock.

Deliberate pitfalls avoided:

- **No `volatile` in a hot loop.** A volatile accumulator forces a store+reload
  of every intermediate, turning any kernel into a store-to-load-forwarding
  latency test — roughly constant across core widths. Dead-code elimination is
  prevented with a checksum the caller reads instead.
- **FP state stays in normal range forever.** `x = x*c + b` with `|c| < 1`
  converges to a fixed point, so it can never reach Inf, NaN or a denormal, all
  of which carry data-dependent timing penalties that differ between cores.
- **The latency and MLP pointer chases have identical footprints.** Both walk the
  whole buffer (the 8-way chase is interleaved, not sliced), so the single-chain
  number cannot accidentally fit in last-level cache and inflate the ratio.
- **The working set is sized from the machine's cache.** A fixed default measures
  cache rather than DRAM on a machine with a large last-level cache.

"Ops" are counted at the C level, not as machine instructions: one `INT_STEP` is
4 instructions on AArch64 and more on x86-64. That is intentional — the benchmark
measures how fast a machine performs a specified computation, and ISA efficiency
is part of that.

## What it measures

| Metric | Unit | Meaning |
|---|---|---|
| `INT-lat` | Mops/s | integer latency: one dependent chain of 1-cycle ALU ops |
| `INT-thr` | Mops/s | integer throughput: 8 independent chains |
| `ILP` | x | `INT-thr / INT-lat` — integer issue width |
| `MUL-thr` | Mmul/s | 64-bit integer multiply throughput, measured on its own |
| `FP-lat` | Mflop/s | FP latency: one dependent multiply-add chain |
| `FP-thr` | Mflop/s | FP throughput: 8 independent chains |
| `fILP` | x | `FP-thr / FP-lat` — FP ops in flight |
| `MEM` | GB/s | sequential read+write bandwidth |
| `MEMlat` | ns | random-access latency, one dependent pointer chase |
| `MEMlat/8` | ns | the same with 8 chases in flight, per access |
| `MLP` | x | `MEMlat / MEMlat-8` — how much memory latency the core overlaps |
| `DISP-thr` | Mcall/s | indirect calls/s once the target pattern is learned |
| `DISPcap` | calls | longest repeating call pattern the core still predicts |
| `score` | geomean | composite of `INT-thr`, `MUL-thr`, `FP-thr`, `DISP-thr`, `DISPcap` and the 8-chase random-access rate |

Three of these need a word of interpretation:

- **`ILP`** tracks issue width because the integer kernel holds no multiply, so
  `INT-lat` pins to 1 op/cycle and the ratio is what the core issues in parallel:
  roughly 2x for a 2-wide in-order core, 3x for three ALUs.
- **`fILP` is not a width measure, and not "bigger is better."** FP latency varies
  by several cycles between cores, and a core with slow FP needs *more* ops in
  flight to fill its pipes, so it scores higher. Compare `FP-thr` for capability.
- **`MLP`** is how much memory latency the core hides. An in-order core stalls on
  the first miss and sits near 1x. It can legitimately exceed 8, because the
  serial chase also serialises TLB page-table walks that the parallel one
  overlaps.

`score` is comparable across the cores of one run, not across machines.

### `DISPcap`: indirect predictor capacity

The dispatch phase calls one of four tiny functions through a function pointer
chosen by a selector stream. A *uniformly random* selector stream is
unpredictable for every predictor ever built, so it does not measure prediction
at all — it measures the cost of a mispredict, which is pipeline-depth-dominated
and therefore ranks cores by how *shallow* they are.

What separates a big front end from a little one is predictor *capacity*: the
length of deterministic pattern it can still learn. So the selector stream is a
fixed random sequence of period `L`, repeated, and `L` is swept by powers of two.
At small `L` every core predicts it; at large `L` none does; where a core falls
off in between is the read-out.

The curve is reduced to one number without a magic threshold (a threshold lands
on the knee, where noise slides it between ladder steps). Each rate is normalised
between the plateau and the fully-random floor, giving `q` ∈ [0,1] = "how much of
the available prediction win is this core still getting", and `q` is summed over
the span `L` = 8…16384, a factor of two per step:

```
DISPcap = 8 * 2^(sum(q) - 1)      # calls
```

A core that predicts everything up to `L` and nothing beyond reads out exactly
`L`. Dividing the floor out is what removes the mispredict-penalty bias, since
the floor *is* that penalty. Being a length rather than a rate, `DISPcap` needs
no clock normalisation.

Two edge cases appear in the tables: `none` means the core was no faster even
calling one single repeated target, i.e. it has no usable indirect target
prediction; `<8` means it loses the pattern before the swept span starts.

**Measure `DISPcap` on a quiet machine.** Background load depresses the plateau
more than the mispredicting end of the curve, which compresses the two together
and *inflates* the result. This is the one column that is biased, rather than
just noised, by interference, so raising `--reps` does not fix it.

`./cpu-bench --disp-sweep` prints the whole rate-vs-period curve for any target,
both for the dependent-accumulator kernel the suite uses and for a variant with
independent calls.

## Build (GCC 13+)

```
make
make native                          # -march/-mtune best-effort for the host
make MARCH=x86-64-v3 MTUNE=generic
make CFLAGS="-O3 -march=native -mtune=native"
```

Fairness defaults for cross-arch comparisons: auto-vectorization off
(`-fno-tree-vectorize`) and FMA contraction off (`-ffp-contract=off`). Enable for
peak per-arch throughput:

```
make VECTORIZE=1                 # allow compiler vectorization
make VECTORIZE=1 FMA=1           # allow vectorization and FMA contraction
```

Keep `VECTORIZE`/`FMA` consistent across the machines you compare.

### RISC-V notes

The canonical `-march` ISA string excludes privilege letters (`S`, `U`), so
`rv64imafdcvsu` is invalid; pass CSR/fence split as named extensions instead.

- Ratified RVV 1.0 hardware: `make riscv-v` (`-march=rv64gcv_zicsr_zifencei`).
- Sophgo SG2000 / T-Head C906: `make sg2000`
  (`-march=rv64imafdc_zicsr_zifencei`, with **no `v`**). The C906 implements the
  draft 0.7.1 vector extension, which is a different, incompatible encoding from
  RVV 1.0 — and `v` in a GCC `-march` string means RVV 1.0. Building `rv64imafdcv`
  for this chip produces a binary that dies with `Illegal instruction`, and
  `-fno-tree-vectorize` does not save you: the compiler also uses vector
  registers to inline `memset`/`memcpy` and struct copies, so the first casualty
  is usually a struct initialiser, long before any kernel runs. Nothing is lost,
  as the default build is scalar on every ISA anyway. For 0.7.1 as the hardware
  actually implements it (only useful with `VECTORIZE=1`, needs GCC 14+), use
  `make sg2000-xthead`.

Object files are stamped with the flags they were built with (`.build-flags`), so
switching `MARCH` forces a rebuild rather than silently keeping an object built
for the previous ISA. If in doubt:

```
make clean && make sg2000
objdump -d cpu-bench | grep -c vsetvli    # 0 = no RVV in the binary
```

## Run

```
./cpu-bench --help
./cpu-bench                                  # all cores, all phases
./cpu-bench --per-core                       # sweep each CPU single-threaded
./cpu-bench --disp-sweep                     # indirect-dispatch curve vs period
./cpu-bench --cpus 4-7 --threads 4           # only the big cluster
./cpu-bench --time 2.0 --reps 5              # longer and more repetitions
./cpu-bench --mem-per-thread 33554432        # 32 MiB working set per thread
./cpu-bench --no-mem                         # skip the memory phases
./cpu-bench --no-pin                         # disable thread pinning
./cpu-bench -v                               # explain every metric afterwards
./cpu-bench --json > run.json                # machine-readable results
```

Defaults: threads = online cores, `--time 0.5` per phase, `--reps 3`,
`--warmup 0.15`, pinned, `--clock raw`. The memory working set defaults to
`max(16 MiB, 4 x LLC / threads)` per thread, read from sysfs.

`cpu-bench` warns if the load average suggests the machine is busy, if the
working set does not clear last-level cache, and reports the DRAM controller
clock observed during the memory phases (if it moved mid-run, the memory numbers
carry that spread; pin the devfreq governor to `performance`).

Where sysfs exposes no cpufreq node, the `MHz` column falls back to an estimate
derived from `INT-lat` and is marked with `~`. That works because `INT-lat` is one
dependent 1-cycle op per cycle by construction.

`--per-core` runs the whole suite single-threaded on each CPU in turn and
tabulates the result, which is how to compare core types on a heterogeneous
machine (big.LITTLE, Intel P/E-cores).

### Verbose output

By default the tables print with their units and nothing else. `-v` / `--verbose`
appends the prose: what every column means, how to read the ratios, and how
`DISPcap` is derived.

### Machine-readable output

`--format json|tsv` (or the `--json` / `--tsv` shorthands) writes results to
**stdout** and moves every other line — platform banner, warnings, DRAM clock,
verbose prose — to **stderr**, so the result stream can be piped directly:

```
./cpu-bench --per-core --tsv > cores.tsv
./cpu-bench --json | jq '.total.int_thr_mops'
./cpu-bench --disp-sweep --tsv | ...
```

TSV emits a header line and one row per `scope`: `cpu` per core in `--per-core`,
`thread` per thread plus a `total` row in the default run, and one row per
(cpu, period) point in `--disp-sweep`. Rates sum into the `total` row; latencies,
ratios and `DISPcap` average. Values that were not measured are empty.

JSON emits one object per run carrying `build`, `system` and `config` metadata
alongside the same records, with `null` for anything not measured:

```json
{
  "schema": "cpu-bench/1",
  "mode": "threads",
  "build": { "compiler": "gcc 15.2.0", "target": "x86_64", "vectorize": false, "fma": false },
  "config": { "threads": 8, "seconds_per_phase": 0.5, "reps": 3, "...": "..." },
  "threads": [ { "scope": "thread", "cpu": 0, "int_thr_mops": 24570.3, "...": "..." } ],
  "total":   { "scope": "total", "cpu": null, "...": "..." },
  "checksum": "0x..."
}
```

## Stability of the numbers

Compute phases repeat to within ~1% across identical cores. `MEMlat` and the
`MLP` derived from it vary more (~20%), which is physical page placement: without
transparent huge pages a large random chase spans thousands of pages and the DRAM
bank/rank distribution differs per allocation. `DISPcap` repeats to a few percent
on a big core and worse on a small one, whose knee sits on a steeper part of the
curve. Raise `--time` for tighter numbers; the whole dispatch ladder scales with
it, each of its 14 points getting half a phase.

Notes:

- The numbers are synthetic and not directly comparable to industry benchmarks.
- The checksum discourages dead-code elimination and confirms that runs with
  different configuration really did differ.
- For apples-to-apples comparisons, pin clocks and disable turbo and frequency
  scaling.
- Avoid `-ffast-math` if you care about strict FP semantics; results will change.
