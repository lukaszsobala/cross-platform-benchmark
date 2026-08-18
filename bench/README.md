# cpcpub

A small, portable CPU benchmark in C (C2x, GCC 13+ or Clang) for 64-bit x86_64,
AArch64, RISC-V, LoongArch, ppc64le and s390x, on Linux (Android included),
macOS and Windows. Nothing beyond libc, libm and pthreads — which on Windows
means a mingw-w64 toolchain (mingw-w64 GCC for x64, llvm-mingw for Arm64); MSVC
has no pthreads and is not supported. 32-bit targets are out of scope: the
throughput kernels keep eight 64-bit chains live at once.

## Build

Run these here or from the repo root — the top-level Makefile forwards every
target and variable, and puts the binary at `bench/cpcpub` either way.

```sh
make
make native                          # -march/-mtune best-effort for the host
make MARCH=x86-64-v3 MTUNE=generic
make loongarch                       # LA64, base ISA
make riscv-v                         # ratified RVV 1.0 hardware
make rva23                           # the RVA23U64 profile (Ubuntu 26.04's baseline)
make sg2000                          # Sophgo SG2000 / T-Head C906 -- see below
```

Cross-building is a matter of naming the compiler; the Makefile works out the
rest, including the `.exe` suffix and the static link a Windows binary needs.
PowerPC has no `-march`, so it takes `MCPU`.

```sh
make CC=powerpc64le-linux-gnu-gcc MCPU=power8
make CC=s390x-linux-gnu-gcc MARCH=z13
make CC=x86_64-w64-mingw32-gcc MARCH=x86-64 MTUNE=generic       # -> cpcpub.exe
make CC=aarch64-w64-mingw32-clang MARCH=armv8-a                 # llvm-mingw
make CC=aarch64-linux-android29-clang MARCH=armv8-a LDFLAGS=-static
```

Objects are stamped with the flags they were built with, so switching `MARCH`
forces a rebuild rather than silently keeping the previous ISA's objects.

**RISC-V, the one real trap:** the C906 (SG2000, LicheeRV and friends)
implements the *draft* 0.7.1 vector extension, which is not RVV 1.0. A `v` in a
GCC `-march` means RVV 1.0, so `-march=rv64imafdcv` there builds a binary that
dies with `Illegal instruction` — and `-fno-tree-vectorize` does not save it,
because the compiler also uses vector registers to inline `memset`. Use
`make sg2000` (no `v`); `make sg2000-xthead` builds 0.7.1 as that hardware
actually implements it, and needs GCC 14+. `make rva23` is the opposite risk: it
is a floor, not a hint, and its binary will not start on anything below the
profile. Plain `make` (`rv64gc`) runs everywhere.

## Run

```sh
./cpcpub --help
./cpcpub --version                        # version, build, digest, and this machine
./cpcpub                                  # all cores at once
./cpcpub --per-core                       # sweep each CPU single-threaded
./cpcpub --full                           # both in one report -- the form to share
./cpcpub --full --variants                # ...once per build variant
./cpcpub --cpus 4-7 --threads 4           # only the big cluster
./cpcpub --time 2.0 --reps 5              # longer and more repetitions
./cpcpub --no-mem / --no-pin              # skip the memory phases / do not pin
./cpcpub --mhz 1050                       # state the clock on a board that will not
./cpcpub -v                               # explain every metric afterwards
./cpcpub --json > run.json                # machine-readable results
./cpcpub --disp-sweep                     # diagnostic: dispatch curve vs period
```

Defaults: threads = online cores, `--time 0.5` per phase, `--reps 3`,
`--warmup 0.15`, pinned, `--clock raw`. The memory working set defaults to
`max(16 MiB, 4 x LLC / threads)` per thread; `--full` sizes its two phases
separately, since N threads share the cache and one core does not.

`--full` is the form worth keeping: the multi-threaded run says what the machine
does at once, the per-core sweep what each core type does on its own, and
neither is interpretable without the other. Each phase runs `--reps` times and
the **best** is kept — interference only ever slows a run down.

`cpcpub` warns when the load average says the machine is busy and when the
working set does not clear last-level cache, and reports the DRAM controller
clock it saw. Measure on a quiet machine: `DISPcap` in particular is *biased*
upward by background load, not merely noised, so more reps do not fix it.

## Submitting a result

```sh
./cpcpub --full --submit http://my.server:8782 --token YOUR-TOKEN --label "my box"
```

The upload happens after the last measurement, so a hub that is down costs the
reply and not the run. Without `--token` it goes up anonymously and the reply
carries the delete token that is then the only way to withdraw it — printed on
stderr, along with everything else the upload has to say, so redirecting the
report neither hides a failed upload nor writes that token into a file you might
share. `$CPCPUB_HUB`
and `$CPCPUB_TOKEN` stand in for the two flags, which keeps the token out of the
process list. `--variants --submit` uploads one run per variant, since results
only compare between matching builds — each run's label gets the variant name
appended, which is what tells the four rows apart. `--label` and `--notes` are
trimmed to what the hub keeps (200 and 2000 bytes) and say so when they are.
https needs `curl` on `PATH` — there is no TLS in the binary itself. See
[web/README.md](../web/README.md).

## What it measures

| Metric | Unit | Meaning |
| --- | --- | --- |
| `INT-lat` | Mop/s | integer latency: one dependent chain of 1-cycle ALU ops |
| `INT-thr` | Mop/s | integer throughput: 8 independent chains |
| `ILP` | x | `INT-thr / INT-lat` — integer issue width |
| `MUL-thr` | Mmul/s | 64-bit integer multiply throughput, measured on its own |
| `FP-lat` | Mflop/s | FP latency: one dependent multiply-add chain |
| `FP-thr` | Mflop/s | FP throughput: 8 independent chains |
| `fILP` | x | `FP-thr / FP-lat` — FP ops in flight |
| `MEM` | GB/s | sequential read+write bandwidth |
| `MEMlat` | ns | random-access latency, one dependent pointer chase |
| `MEMlat/8` | ns | the same with 8 chases in flight, per access |
| `MLP` | x | `MEMlat / MEMlat-8` — memory latency the core overlaps |
| `DISP-thr` | Mcall/s | indirect calls/s once the target pattern is learned |
| `DISPcap` | calls | longest repeating call pattern the core still predicts |
| `score` | geomean | composite of `INT-thr`, `MUL-thr`, `FP-thr`, `DISP-thr`, `DISPcap` and the 8-chase random-access rate |

Every compute kernel exists in two forms built from the same op sequence — one
dependency chain and eight independent ones — because the ratio is what
separates a wide out-of-order core from a narrow in-order one. Both forms do the
same number of counted ops per loop iteration, so the loop's own counter and
branch — issued, never counted — cost each of them the same fraction and cancel
in the ratio. They have to: an out-of-order core hides that overhead behind the
chain, an in-order one issues it, and unequal amounts of it would show up as
parallelism that is really just loop bookkeeping. `-v` explains each column
after the tables. Four things are worth knowing without it:

- **`fILP` is not "bigger is better."** A core with slow FP needs more ops in
  flight to fill its pipes and so scores higher; compare `FP-thr` for capability.
- **`DISPcap`** is a *length*: the period of repeating call pattern a core still
  predicts, read off a sweep rather than a threshold. `none` means no usable
  indirect prediction at all; `<8` means it loses the pattern immediately.
  `./cpcpub --disp-sweep` prints the whole curve.
- **`score` compares cores within one run, not machines.** A `total` row's score
  is the same geomean over aggregated numbers, so it grows with core count
  (~`threads^(5/6)`). Rank machines against machines, cores against cores.
- **`MEMlat` and `MLP` vary ~20% between runs** — physical page placement.
  Everything else repeats to ~1%.

## Build variants

Auto-vectorization and FMA contraction are translation-unit flags, so one binary
carrying both toggles has to compile the kernels more than once:
[src/kernels.c](src/kernels.c) is compiled four times and linked into one
`cpcpub`.

| variant | flags |
| --- | --- |
| `scalar-nofma` | `-fno-tree-vectorize -ffp-contract=off` (the default) |
| `vector-nofma` | `-ftree-vectorize -ffp-contract=off` |
| `scalar-fma` | `-fno-tree-vectorize -ffp-contract=fast` |
| `vector-fma` | `-ftree-vectorize -ffp-contract=fast` |

```sh
./cpcpub --list-variants           # what this binary carries, and why
./cpcpub --variant vector-fma      # measure one
./cpcpub --variants                # measure each distinct one, and compare
./cpcpub --variants=all            # all four, distinct or not
```

Which variants are *distinct* is a property of `-march`, not of the flags:
without a vector unit there is nothing to vectorize and without hardware FMA
nothing to contract, so `--variants` runs only the ones the target can tell
apart. Under clang each row also carries `-f[no-]slp-vectorize`, because clang's
`-fno-tree-vectorize` leaves its SLP vectorizer running at `-O3` — without that
flag a clang "scalar" build comes out with SIMD in it. This matters most where
there is no choice of compiler: macOS and Windows-on-Arm.

`scalar-nofma` is what cross-ISA comparisons want, and a `vector-fma` number
means nothing next to a baseline one.

## Platforms

Linux is the reference. What is missing elsewhere is reported as missing, never
approximated:

| | Linux | macOS | Windows |
| --- | --- | --- | --- |
| pin a thread to a CPU | yes | **no** — the scheduler owns placement | yes |
| where a thread actually ran | yes | no, reported as `null` | yes |
| `mhz` from a governor node | yes | no | no |
| DRAM controller clock | on SoCs with devfreq | absent | absent |
| load-average warning | yes | yes | no such number exists |
| `--clock raw` | yes | yes | no; falls back to `mono` and records `mono` |

Where there is no governor node, `MHz` comes from the device tree's declared
`clock-frequency` for the CPU if it has one, and otherwise is derived from
`INT-lat` (one dependent 1-cycle op per cycle by construction), marked `~` in
the tables and `estimated` in the JSON. That derivation is only as good as its
assumption: a core that does not retire one dependent op per cycle reads low by
exactly the margin it misses. `--mhz N` states the clock outright for a board
that will not — several RISC-V and small Arm SBCs have neither a cpufreq driver
nor a clock in their device tree, and only debugfs knows — and records it as
`given` rather than `measured`.

macOS is the one that changes a number rather than omitting it: pinning is
turned off there and `config.pin` records that, so `--per-core` on a Mac sweeps
*requests* and repeatedly measures whichever core the scheduler picked — on
Apple Silicon that is not a P-core versus E-core reading. Whole-machine and
single-thread figures are unaffected. Windows keeps real pinning, including
across processor groups on machines with more than 64 CPUs. Android is Linux and
behaves as such.

Windows binaries are `-static` but use the Universal CRT, which is part of
Windows 10 and newer. A macOS binary fetched by a browser is quarantined and
needs `xattr -d com.apple.quarantine cpcpub-macos-arm64` before it will run;
`curl` does not set that attribute.

## Machine-readable output

`--format json|tsv` (or `--json` / `--tsv`) writes results to **stdout** and
moves everything else — banner, warnings, DRAM clock, prose — to **stderr**.

```sh
./cpcpub --per-core --tsv > cores.tsv
./cpcpub --json | jq '.total.int_thr_mops'
./cpcpub --full --variants --json | jq -r '.[] | "\(.build.flags) \(.total.score)"'
```

TSV is one header line and one row per scope (`cpu`, `thread`, `total`), with a
leading `variant` column. JSON is one object per run carrying `build`, `system`
and `config` alongside the records; `--variants --json` is an array of exactly
those documents, one per variant. Every result records the toolchain and the
exact `CFLAGS` the measured code was built with — an `-march` nobody remembers
passing is otherwise invisible.

The document is specified in
[schema/cpu-bench-1.md](../schema/cpu-bench-1.md) — read it before changing
anything `--json` prints.

Notes: the numbers are synthetic and not comparable to industry benchmarks; the
checksum exists to defeat dead-code elimination and legitimately differs between
variants; pin clocks and disable turbo for apples-to-apples comparisons; avoid
`-ffast-math` unless you accept different FP semantics.
