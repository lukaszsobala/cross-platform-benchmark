# cpcpub

A small, portable CPU benchmark in C (C2x, GCC 13+ or Clang) for 64-bit x86_64,
AArch64, RISC-V, LoongArch, ppc64le and s390x, on Linux (Android included),
macOS and Windows. No dependencies beyond libc, libm and pthreads — which on
Windows means a mingw-w64 toolchain (mingw-w64 GCC for x64, llvm-mingw for
Arm64); MSVC has no pthreads and is not supported.

32-bit targets are out of scope by design, not by accident: the throughput
kernels keep eight independent 64-bit chains live at once, which fits an ISA
with sixteen 64-bit registers and spills to memory on one without.

## How it works

The suite runs a set of tiny kernels, each for a fixed wall-clock slice, and
reports the rate achieved. Four things shape the design:

**Latency and throughput are measured separately.** Every compute kernel exists
in two forms built from the *same* op sequence: one dependency chain (latency)
and 8 independent chains (throughput). Their ratio is how much instruction-level
parallelism the core extracts, which is the main thing that separates a wide
out-of-order core from a narrow in-order one. A single-chain benchmark rates them
almost equally.

**Each kernel isolates one resource.** The integer kernel is add/xor/sub only —
one instruction, one cycle, on every target ISA — so its latency form pins to
1 op/cycle and the ratio reads out issue width. Integer multiply is the scarcest
resource on most cores, so a kernel containing one is multiplier-bound in *both*
forms and the ratio cancels; multiply therefore gets its own phase. The FP
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
| --- | --- | --- |
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

**A `total` row's `score` is a different quantity from a core's.** It is the same
geomean over the same six components, but taken over the aggregated row — five of
them summed across threads, and `DISPcap` left as the average because predictor
size belongs to a core and does not add up. So it grows with core count, at
roughly `threads^(5/6)` before contention; on the 4P+4E part above, eight threads
come to about 4.5x one P-core. Rank machines against machines and cores against
cores; a `total` next to a `cpu` row is meaningless.

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

```text
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

`./cpcpub --disp-sweep` prints the whole rate-vs-period curve for any target,
both for the dependent-accumulator kernel the suite uses and for one with
independent calls.

## Build (GCC 13+)

Run these in this directory, or from the repo root — the top-level `Makefile`
forwards every target and variable here, and puts the binary at `bench/cpcpub`
either way.

```sh
make
make native                          # -march/-mtune best-effort for the host
make MARCH=x86-64-v3 MTUNE=generic
make loongarch                       # LA64, base ISA
make CFLAGS="-O3 -march=native -mtune=native"
```

Cross-building is a matter of naming the compiler; the Makefile works out the
rest, including the `.exe` suffix and the static link a Windows binary needs.
PowerPC has no `-march`, so it takes `MCPU` instead.

```sh
make CC=powerpc64le-linux-gnu-gcc MCPU=power8
make CC=s390x-linux-gnu-gcc MARCH=z13
make CC=x86_64-w64-mingw32-gcc MARCH=x86-64 MTUNE=generic       # -> cpcpub.exe
make CC=aarch64-w64-mingw32-clang MARCH=armv8-a                 # llvm-mingw
make CC=aarch64-linux-android29-clang MARCH=armv8-a LDFLAGS=-static
```

### Platforms

Linux is the reference. macOS and Windows build and run, each missing some of
what Linux exposes — and what is missing is reported as missing, never
approximated:

| | Linux | macOS | Windows |
| --- | --- | --- | --- |
| pin a thread to a CPU | yes | **no** — the scheduler owns placement | yes |
| where a thread actually ran | yes | no, reported as `null` | yes |
| `mhz` from a governor node | yes | no | no |
| DRAM controller clock | on SoCs with devfreq | absent | absent |
| CPU model name | `/proc/cpuinfo` | `sysctl` | registry |
| last-level cache size | sysfs | `sysctl` | `GetLogicalProcessorInformationEx` |
| load-average warning | yes | yes | no such number exists |
| POSIX barriers | yes | absent; [platform.h](src/platform.h) shims them | yes |
| `--clock raw` | yes | yes | no; falls back to `mono` and records `mono` |

Where there is no governor node the `MHz` column is derived from `INT-lat`,
which is one dependent single-cycle op per cycle by construction — the tables
mark it `~` and `mhz_src` in the JSON reads `estimated` rather than `measured`.

macOS's first row is the one that changes a number rather than omitting it.
`cpcpub` turns pinning off there and says so, and `config.pin` in the result
records that it was off — so `--per-core` on a Mac sweeps *requests*, not cores,
and repeatedly measures whichever core the scheduler picked. On an asymmetric
part (every Apple Silicon chip) that means the per-core spread is not a P-core
versus E-core reading. Whole-machine and single-thread figures are unaffected.

Windows keeps pinning, through `SetThreadGroupAffinity`, so `--per-core` means
there what it means on Linux — including on a machine with more than 64 logical
CPUs, where Windows splits them into groups of 64 and `--cpus` numbers straight
through the groups as if it had not.

Android is Linux and behaves as such — real pinning, `/proc`, the lot — so it
gets no column of its own. Its binary is statically linked, which is what lets
one file work across Android versions whose bionic differs; `uname` there says
`Linux`, so a run is told apart from a desktop one by its `build.binary_sha256`
rather than by `system.sysname`.

Two Windows details worth knowing. The binaries are `-static`, so nothing has to
sit beside them, but they use the Universal CRT: that is part of Windows 10 and
newer, and on anything older needs Microsoft's UCRT update installed. And
`config.clock` will read `mono` even when `--clock raw` was asked for, because
mingw-w64's `clock_gettime` has no unadjusted monotonic clock; the field records
what was used, not what was requested.

A downloaded macOS binary is quarantined by the browser that fetched it and
will be refused on first run:

```sh
xattr -d com.apple.quarantine cpcpub-macos-arm64     # or: right-click -> Open
```

`curl` does not set that attribute, so a `curl -fLO` download needs only
`chmod +x`.

### Build variants

Auto-vectorization and FMA contraction are translation-unit flags — no pragma
switches them per function — so a binary that can measure more than one of them
has to compile the kernels more than once. `make` does exactly that:
[src/kernels.c](src/kernels.c) is compiled four times, and all four objects are
linked into the one `cpcpub`.

| variant | flags |
| --- | --- |
| `scalar-nofma` | `-fno-tree-vectorize -ffp-contract=off` |
| `vector-nofma` | `-ftree-vectorize -ffp-contract=off` |
| `scalar-fma` | `-fno-tree-vectorize -ffp-contract=fast` |
| `vector-fma` | `-ftree-vectorize -ffp-contract=fast` |

Under clang each row also carries `-f[no-]slp-vectorize`, and `build.flags` in
the result says so. It is not decoration: clang's `-fno-tree-vectorize` is an
alias for `-fno-vectorize`, which turns off the *loop* vectorizer and leaves the
SLP one running at `-O3` — so without the extra flag a clang `scalar-nofma`
build comes out with SIMD in it, under the name of the build that is supposed
not to. GCC's one switch covers both of its vectorizers and needs no help. This
matters most where there is no choice of compiler: macOS and Windows-on-Arm.

`scalar-nofma` is the default and the one cross-ISA comparisons want: no
vectorization and no FMA contraction, so the kernel is the op sequence the
source says it is on every target.

```sh
./cpcpub --list-variants           # what this binary carries, and why
./cpcpub --variant vector-fma      # measure one
./cpcpub --variants                # measure each distinct one, and compare
./cpcpub --variants=all            # all four, distinct or not
./cpcpub --variants=scalar-nofma,vector-fma
```

`--variants` runs the whole suite once per variant, so it takes as many times as
long; it prints each run in full and then a table of what moved.

**Which variants are distinct is a property of `-march`, not of the flags.**
Without a vector unit `-ftree-vectorize` has nothing to emit, and without a
hardware fused multiply-add `-ffp-contract=fast` has nothing to contract — in
either case the two builds are the same machine code and running both measures
one build twice. `--variants` therefore runs only the ones the target can tell
apart: four on AArch64 and on x86-64 built for `x86-64-v3` or above, two
(`scalar-nofma`, `scalar-fma`) on plain `rv64gc` and on baseline `x86-64`.
`--list-variants` says which, and why.

This is also why building for the SG2000 stays safe: the vector variants are in
the binary, but `-march=rv64imafdc…` has no `v` for them to emit, so nothing
compiles to an instruction the C906 cannot execute. `--variants` there runs the
two FP variants and skips the vector pair as identical.

Keep the variant consistent across the machines you compare — the hub stores
`vectorize`/`fma` per run for exactly that reason, and a `vector-fma` number
means nothing next to a baseline one.

### RISC-V notes

The canonical `-march` ISA string excludes privilege letters (`S`, `U`), so
`rv64imafdcvsu` is invalid; pass CSR/fence split as named extensions instead.

- Ratified RVV 1.0 hardware: `make riscv-v` (`-march=rv64gcv_zicsr_zifencei`).
- RVA23 hardware: `make rva23`. **This is the profile Ubuntu 26.04 takes as its
  riscv64 baseline**, so it is the build that matches a distribution-built
  userland on such a machine. See below.
- Sophgo SG2000 / T-Head C906: `make sg2000`
  (`-march=rv64imafdc_zicsr_zifencei`, with **no `v`**). The C906 implements the
  draft 0.7.1 vector extension, which is a different, incompatible encoding from
  RVV 1.0 — and `v` in a GCC `-march` string means RVV 1.0. Building `rv64imafdcv`
  for this chip produces a binary that dies with `Illegal instruction`, and
  `-fno-tree-vectorize` does not save you: the compiler also uses vector
  registers to inline `memset`/`memcpy` and struct copies, so the first casualty
  is usually a struct initialiser, long before any kernel runs. Nothing is lost:
  with no `v` in the `-march`, the binary's two vector variants compile to the
  same scalar code as its two scalar ones, which is exactly what
  `--list-variants` reports there. For 0.7.1 as the hardware actually implements
  it (needs GCC 14+, and only tells you anything under `--variant vector-nofma`
  or `vector-fma`), use `make sg2000-xthead`.

#### RVA23

`make rva23` builds for **RVA23U64**, the profile ratified in October 2024 and
the baseline Ubuntu 26.04 requires of a riscv64 CPU. It is a *floor*, not a
tuning hint: everything in it is mandatory, so the compiler uses RVV 1.0,
`Zba`/`Zbb`/`Zbs`, `Zcb`, `Zfa`, `Zicond` and the rest without asking, and the
binary dies with `Illegal instruction` on anything that implements less — every
board `make sg2000` exists for included. Plain `make` (`rv64gc`) remains the
build that runs everywhere.

Because RVA23 mandates `V`, all four build variants are distinct in an `rva23`
binary, and `--variants` measures four rather than the two a plain `rv64gc`
build can tell apart. The cross-ISA default is still `scalar-nofma`.

Toolchains disagree about how to spell the profile. LLVM 19+ accepts
`-march=rva23u64`; GCC 15 refuses profile names outright (*"ISA string must
begin with rv32 or rv64"*) and wants the mandatory set written out. The target
probes the compiler and uses whichever it understands, so:

```sh
make rva23                                        # native, on RVA23 hardware
make rva23 CC=riscv64-linux-gnu-gcc               # cross, from anywhere
qemu-riscv64 -cpu rva23u64 -L /usr/riscv64-linux-gnu ./cpcpub --variants
```

The `-march` it settled on is recorded verbatim in `build.flags`, and therefore
in every uploaded result, so which spelling was used is never a guess.

Object files are stamped with the flags they were built with (`.build-flags`), so
switching `MARCH` forces a rebuild rather than silently keeping an object built
for the previous ISA. If in doubt:

```sh
make clean && make sg2000
objdump -d cpcpub | grep -c vsetvli    # 0 = no RVV in the binary
```

## Run

```sh
./cpcpub --help
./cpcpub                                  # all cores, all phases
./cpcpub --per-core                       # sweep each CPU single-threaded
./cpcpub --full                           # both of the above in one report
./cpcpub --full --variants                # ...once per build variant
./cpcpub --variant vector-fma             # one named variant
./cpcpub --disp-sweep                     # indirect-dispatch curve vs period
./cpcpub --cpus 4-7 --threads 4           # only the big cluster
./cpcpub --time 2.0 --reps 5              # longer and more repetitions
./cpcpub --mem-per-thread 33554432        # 32 MiB working set per thread
./cpcpub --no-mem                         # skip the memory phases
./cpcpub --no-pin                         # disable thread pinning
./cpcpub -v                               # explain every metric afterwards
./cpcpub --json > run.json                # machine-readable results
```

Defaults: threads = online cores, `--time 0.5` per phase, `--reps 3`,
`--warmup 0.15`, pinned, `--clock raw`. The memory working set defaults to
`max(16 MiB, 4 x LLC / threads)` per thread, read from sysfs — `--full` sizes the
two phases separately, since N threads share the cache and one core does not.

`--full` runs the multi-threaded run and then the per-core sweep, and reports
both. That is the form worth keeping and sharing: the first says what the machine
does at once, the second what each core type does on its own, and neither is
interpretable without knowing the other.

`cpcpub` warns if the load average suggests the machine is busy, if the
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

```sh
./cpcpub --per-core --tsv > cores.tsv
./cpcpub --json | jq '.total.int_thr_mops'
./cpcpub --disp-sweep --tsv | ...
```

TSV emits a header line and one row per `scope`: `cpu` per core in `--per-core`,
`thread` per thread plus a `total` row in the default run, both sets in `--full`,
and one row per (cpu, period) point in `--disp-sweep`. Rates sum into the `total`
row; latencies, ratios and `DISPcap` average. Unmeasured values are empty. The
leading `variant` column names the build each row was measured with, so a
`--variants` run stays one table.

A `--full` document carries both phases: `threads` + `total` and `cores`.

Every result records the toolchain it was produced with — `build.compiler`, the
compiler's own version banner, the driver, and the exact `CFLAGS` the measured
code was compiled with (the shared ones baked in by the Makefile, the variant's
own pair appended). An `-march` or an `-ffast-math` nobody remembers passing
changes the numbers and is otherwise invisible; the text output prints the same
string on its `flags:` line.

JSON emits one object per run carrying `build`, `system` and `config` metadata
alongside the same records, with `null` for anything not measured:

```json
{
  "schema": "cpu-bench/1",
  "mode": "full",
  "build": { "compiler": "gcc 15.2.0", "compiler_version": "15.2.0", "cc": "cc",
             "flags": "-O3 -pipe -std=c2x ...", "target": "x86_64",
             "vectorize": false, "fma": false },
  "config": { "threads": 8, "seconds_per_phase": 0.5, "reps": 3, "...": "..." },
  "threads": [ { "scope": "thread", "cpu": 0, "int_thr_mops": 24570.3, "...": "..." } ],
  "total":   { "scope": "total", "cpu": null, "...": "..." },
  "cores":   [ { "scope": "cpu", "cpu": 0, "score": 18318.0, "...": "..." } ],
  "core_spread": 1.43,
  "checksum": "0x..."
}
```

`--variants --json` emits a JSON **array** of exactly these documents, one per
variant, in the order they ran. Each element is an unchanged `cpu-bench/1`
document — the hub keys a run on `build.vectorize` / `build.fma` already, so the
variants are separate uploads rather than one document with a new shape, and
nothing about the schema changes. `submit.sh` posts each element in turn.

```sh
./cpcpub --full --variants --json | jq -r '.[] | "\(.build.flags) \(.total.score)"'
```

## Stability of the numbers

Compute phases repeat to within ~1% across identical cores. `MEMlat` and the
`MLP` derived from it vary more (~20%), which is physical page placement: without
transparent huge pages a large random chase spans thousands of pages and the DRAM
bank/rank distribution differs per allocation. `DISPcap` repeats to a few percent
on a big core and worse on a small one, whose knee sits on a steeper part of the
curve. Raise `--time` for tighter numbers; the whole dispatch ladder scales with
it, each of its 14 points getting half a phase.

## Sharing results

[`web/`](../web/) is a small self-contained service for collecting results and
comparing them with other machines: `make serve` from the repo root, then

```sh
./cpcpub --full --json | ../web/submit.sh -l "my box"
./cpcpub --full --variants --json | ../web/submit.sh -l "my box"
```

The second uploads one run per variant, each labelled with the variant's name.

See [web/README.md](../web/README.md). The document the two exchange is
specified in [schema/cpu-bench-1.md](../schema/cpu-bench-1.md) — read it before
changing anything `--json` prints.

Notes:

- The numbers are synthetic and not directly comparable to industry benchmarks.
- The checksum discourages dead-code elimination and confirms that runs with
  different configuration really did differ. It legitimately differs between
  variants: contracting `x*c + b` into one FMA drops a rounding step, so the FP
  state it folds in is not bit-identical.
- For apples-to-apples comparisons, pin clocks and disable turbo and frequency
  scaling.
- Avoid `-ffast-math` if you care about strict FP semantics; results will change.
