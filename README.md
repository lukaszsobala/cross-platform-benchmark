# cpu-bench

A tiny, portable CPU benchmark in C (C2x, GCC 13+) for common 64-bit architectures:
- x86_64 (amd64)
- AArch64 (ARMv8-A)
- RISC-V 64 (rv64imafdcvsu)

No external dependencies beyond libc and pthreads. It measures:
- Integer ALU throughput (MOPS)
- Double-precision FLOPs (MFLOPS)
- Memory bandwidth (GB/s, read+write) — disabled by default

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
make native                    # same as MARCH=native MTUNE=native
make MARCH=x86-64-v3 MTUNE=generic
```

## Run

```
./cpu-bench --help
./cpu-bench --threads 4 --time 2.0                    # CPU-only (default)
./cpu-bench --threads 4 --time 2.0 --mem 268435456    # enable memory phase (256 MiB total)
```

Defaults:
- threads = online cores
- time = 1.0 s per phase (int, fp)
- mem = 0 (memory phase disabled)

Example output:

```
build: gcc 13.x, C2x, target=aarch64
system: Linux 6.x #1 SMP, machine=aarch64, cores=8
cpu: Neoverse-N2 (or model name)
cpu-bench: threads=8 time=1.000s mem_total=0 bytes
INT  :    42000.5 MOPS
FP64 :    38000.2 MFLOPS
CHK  : 0x3f9c4a7b2ee6c0d1
```

Notes:
- The numbers are synthetic and not directly comparable to industry benchmarks.
- The checksum is to discourage dead-code elimination and to confirm runs differ by config.
- For apples-to-apples comparisons, pin clocks and disable turbo and frequency scaling.
- Avoid using `-ffast-math` if you care about strict FP semantics; results will change.
- To enable memory, pass `--mem <bytes>` (split across threads). The memory phase uses a cache-line stride and read+write pattern; adjust `stride` in `bench.c` if needed.
