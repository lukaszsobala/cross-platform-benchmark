# cross-platform-benchmark

`cpcpub` — a small, portable CPU benchmark in C, and a place to compare what
it measures.

It runs a set of tiny kernels for a fixed wall-clock slice each and reports the
rate achieved: integer and floating-point latency and throughput (and the
instruction-level parallelism their ratio exposes), integer multiply, memory
bandwidth and random-access latency, the memory-level parallelism behind it, and
indirect-call throughput and branch-predictor capacity. One core, every core, or
the whole machine at once. Nothing beyond libc, libm and pthreads; C2x with
GCC 13+ or Clang, on x86_64, AArch64, RISC-V and LoongArch, on Linux and
macOS (Apple Silicon).

```sh
make                                            # build -> bench/cpcpub
bench/cpcpub --full                          # measure this machine
bench/cpcpub --full --variants               # ...once per build variant
bench/cpcpub --full --json | web/submit.sh -l "my box"
```

One binary carries four compilations of the kernels — auto-vectorization off/on
crossed with FMA contraction off/on — and `--variants` runs each of them that
the host ISA can tell apart, then compares them. The default is still the
scalar, unfused build that cross-ISA comparisons need.

**[bench/README.md](bench/README.md) is the benchmark**: how each phase works,
what every metric means, and what a number may and may not be compared against.
Read it before reading a result — several of the metrics say something other
than what their name suggests.

The top-level `Makefile` forwards the benchmark's tuning targets and variables
unchanged, so `make native`, `make sg2000`, `make rva23`, `make loongarch` and
`make MARCH=x86-64-v3` work from here as well as from [bench/](bench/).

**macOS builds and runs, with one caveat that changes a number rather than
omitting it: nothing on macOS can bind a thread to a CPU.** `cpcpub` turns
pinning off there and records `pin: false`, so `--per-core` sweeps requests
rather than cores — which on an asymmetric Apple part is not the P-core versus
E-core reading it looks like. See
[bench/README.md](bench/README.md#platforms).

`make rva23` builds for **RVA23U64**, the RISC-V profile Ubuntu 26.04 takes as
its riscv64 baseline. It is a floor rather than a tuning hint — the binary uses
RVV 1.0, `Zba`/`Zbb`/`Zbs`, `Zcb`, `Zfa` and `Zicond` freely and will not start
on anything older, so plain `make` (`rv64gc`) remains the build that runs
everywhere. GCC still refuses profile names in `-march`, so the target probes
the compiler and writes the profile out when it has to; see
[bench/README.md](bench/README.md#rva23).

## The results hub

[web/](web/) is an optional addition: a Python-stdlib service that collects
`cpcpub --json` uploads and puts them side by side. It is not needed to run
the benchmark. See [web/README.md](web/README.md).

```sh
make serve          # the hub on http://127.0.0.1:8080
make submit         # build, measure, upload -- one row on the board
make check          # contract test + hub tests
make testdata       # regenerate testdata/full-run.json on this machine
```

Submitting is a drop target on the *Submit a result* tab — drag `run.json` onto
it, or click, or paste — and a saved config for the machines that have no
browser:

```sh
web/submit.sh --save -u https://hub.example -t YOUR-TOKEN   # once per machine
make submit LABEL="workstation, quiet"                      # every time after
```

An account on the hub is optional and adds three things: your name on the runs
you upload, withdrawing them from any browser rather than only from the one
holding a delete token, and the upload token that makes the line above work.
Anonymous uploads are not second-class and are not going away.

## Verified results

[The release workflow](.github/workflows/release.yml) builds `cpcpub` for
every target on GitHub's runners — x86-64, x86-64-v3, aarch64, rv64gc, RVA23,
loongarch64 and macOS/Apple Silicon — smoke-tests each one (under qemu where it
is cross-built), and attaches
the binaries with a `SHA256SUMS` and a `verified-builds.json` manifest to the
release.

A benchmark cannot vouch for its own numbers. Any key inside a public binary can
be read out of it, so nothing running on a machine its owner controls produces a
figure that owner cannot forge. The digest the benchmark reports
(`build.binary_sha256`) therefore says which binary a result *claims* — useful,
self-reported, and shown as grey text rather than a badge.

What is binding is [the measure workflow](.github/workflows/measure.yml). It
runs a published binary with fixed arguments and asks GitHub for an OIDC token
whose audience is the SHA-256 of the result it just produced, so GitHub signs
those exact bytes. Change a digit and the signature no longer covers it; fork
the workflow and the `job_workflow_ref` claim names your fork. On GitHub's own
runners nothing in the chain belongs to the submitter, and the hub marks the run
**verified**; on a self-hosted runner the signatures still hold but the machine
does not, so it is marked **attested** and never confused with the first.

```yaml
  measure:
    uses: lukaszsobala/cross-platform-benchmark/.github/workflows/measure.yml@v0.1.0
    permissions: { id-token: write, contents: read }
    with: { hub: https://hub.example, runner: self-hosted, label: "my box" }
```

Ordinary uploads need none of this, are not second-class, and rank the same.
See [web/README.md](web/README.md#what-a-result-proves).

## Keeping the two halves in step

The benchmark writes `cpcpub --json`; the hub ingests it and nothing else.
They share no code, so a field added on one side and forgotten on the other
fails no compiler — the measurement is simply taken and dropped, or a column
fills with `NULL` forever. [tests/test_contract.py](tests/test_contract.py)
catches that: it runs the binary just built, feeds its real output through the
hub's validation and storage, and asserts the record fields match exactly in
both directions. It falls back to [testdata/full-run.json](testdata/), a
committed sample, so the check still means something without a compiler.
[schema/cpu-bench-1.md](schema/cpu-bench-1.md) is the written half of the same
contract — read it before changing anything `--json` prints.
