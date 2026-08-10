# cross-platform-benchmark

`cpu-bench` — a small, portable CPU benchmark in C, and a place to compare what
it measures.

It runs a set of tiny kernels for a fixed wall-clock slice each and reports the
rate achieved: integer and floating-point latency and throughput (and the
instruction-level parallelism their ratio exposes), integer multiply, memory
bandwidth and random-access latency, the memory-level parallelism behind it, and
indirect-call throughput and branch-predictor capacity. One core, every core, or
the whole machine at once. Nothing beyond libc, libm and pthreads; C2x with
GCC 13+ or Clang, on x86_64, AArch64 and RISC-V.

```sh
make                                            # build -> bench/cpu-bench
bench/cpu-bench --full                          # measure this machine
bench/cpu-bench --full --json | web/submit.sh -l "my box"
```

**[bench/README.md](bench/README.md) is the benchmark**: how each phase works,
what every metric means, and what a number may and may not be compared against.
Read it before reading a result — several of the metrics say something other
than what their name suggests.

The top-level `Makefile` forwards the benchmark's tuning targets and variables
unchanged, so `make native`, `make sg2000` and `make VECTORIZE=1` work from here
as well as from [bench/](bench/).

## The results hub

[web/](web/) is an optional addition: a Python-stdlib service that collects
`cpu-bench --json` uploads and puts them side by side. It is not needed to run
the benchmark. See [web/README.md](web/README.md).

```sh
make serve          # the hub on http://127.0.0.1:8080
make check          # contract test + hub tests
make testdata       # regenerate testdata/full-run.json on this machine
```

## Keeping the two halves in step

The benchmark writes `cpu-bench --json`; the hub ingests it and nothing else.
They share no code, so a field added on one side and forgotten on the other
fails no compiler — the measurement is simply taken and dropped, or a column
fills with `NULL` forever. [tests/test_contract.py](tests/test_contract.py)
catches that: it runs the binary just built, feeds its real output through the
hub's validation and storage, and asserts the record fields match exactly in
both directions. It falls back to [testdata/full-run.json](testdata/), a
committed sample, so the check still means something without a compiler.
[schema/cpu-bench-1.md](schema/cpu-bench-1.md) is the written half of the same
contract — read it before changing anything `--json` prints.
