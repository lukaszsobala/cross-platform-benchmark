# cross-platform-benchmark

`cpcpub` — a small, portable CPU benchmark in C, and a place to compare what
it measures.

It runs a set of tiny kernels for a fixed wall-clock slice each and reports the
rate achieved: integer and floating-point latency and throughput (and the
instruction-level parallelism their ratio exposes), integer multiply, memory
bandwidth and random-access latency, the memory-level parallelism behind it, and
indirect-call throughput and branch-predictor capacity. One core, every core, or
the whole machine at once.

## Get it

Download a binary from [the latest release][rel] — each is self-contained and
needs nothing installed:

| | Linux | macOS | Windows | Android |
| --- | --- | --- | --- | --- |
| x86-64 | `linux-x86_64`, `linux-x86_64-v3` | `macos-x86_64` | `windows-x86_64.exe`, `windows-x86_64-v3.exe` | |
| Arm64 | `linux-aarch64` | `macos-arm64` | `windows-arm64.exe` | `android-arm64` |
| RISC-V | `linux-riscv64`, `linux-riscv64-rva23` | | | |
| Other | `linux-loongarch64`, `linux-ppc64le`, `linux-s390x` | | | |

Every asset name is prefixed `cpcpub-`. Check what you downloaded against the
`SHA256SUMS` published beside it:

```sh
base=https://github.com/lukaszsobala/cross-platform-benchmark/releases/latest/download
curl -fLO $base/cpcpub-linux-x86_64
curl -fLs $base/SHA256SUMS | sha256sum -c --ignore-missing
chmod +x cpcpub-linux-x86_64
```

The `-v3` and `-rva23` builds need a newer ISA than the plain ones and will not
start on older hardware; the unsuffixed build runs everywhere. On macOS a binary
fetched by a browser is quarantined and needs
`xattr -d com.apple.quarantine cpcpub-macos-arm64` before it will run — `curl`
does not set that attribute.

Or build it. Nothing beyond libc, libm and pthreads; C2x with GCC 13+ or Clang:

```sh
make                # -> bench/cpcpub
make native         # tuned for this machine, and comparable with nothing else
```

`make rva23`, `make loongarch`, `make sg2000` and `make MARCH=x86-64-v3` select
other targets; see [bench/README.md](bench/README.md).

## Run it

```sh
bench/cpcpub --full                 # one core, every core, and the machine at once
bench/cpcpub --full --variants      # ...once per build variant
bench/cpcpub --full -v              # with every metric explained
bench/cpcpub --version              # which build this is, and what it is running on
bench/cpcpub --help
```

One binary carries four compilations of the kernels — auto-vectorization off/on
crossed with FMA contraction off/on — and `--variants` runs each one the host
ISA can tell apart, then compares them. The default is the scalar, unfused build
that cross-ISA comparisons need.

[bench/README.md](bench/README.md) covers building for each target, what every
metric means, and what each platform can and cannot report. Read it before
reading a result — several of the metrics say something other than what their
name suggests. `--json` output is specified in
[schema/cpu-bench-1.md](schema/cpu-bench-1.md).

## Share it

[web/](web/) is an optional addition: a Python-stdlib service that collects
results and puts them side by side. The benchmark uploads its own, straight
after measuring:

```sh
bench/cpcpub --full --submit http://my.server:8782 --token YOUR-TOKEN \
             --label "my box"
```

The token comes from the hub's Account tab and puts the run on your name;
without one the upload is anonymous, which every hub accepts just the same.
`$CPCPUB_HUB` and `$CPCPUB_TOKEN` stand in for the two flags. There is no public
hub yet — until there is, `--submit` needs an address, and a build can be given
a default with `make HUB_URL=https://hub.example`.

To run one yourself:

```sh
make serve          # the hub on http://127.0.0.1:8080
make submit         # build, measure, upload -- one row on the board
```

A machine that cannot reach the hub itself can hand its `run.json` to the
*Submit a result* tab instead — drag it on, or click, or paste.

An account is optional and adds three things: your name on the runs you upload,
withdrawing them from any browser rather than only from the one holding a delete
token, and the upload token above. Anonymous uploads are not second-class and
rank the same. See [web/README.md](web/README.md).

## What a result on the board means

A run carries the SHA-256 of the binary that produced it, so a run made with a
published release build is labelled with that release — in grey, as the run's
own word about itself, because a benchmark cannot vouch for its own numbers.

Two stronger marks come from [the measure workflow](.github/workflows/measure.yml),
which runs a published binary with fixed arguments and has GitHub sign the exact
result bytes. Call it from your own repository to measure your own machine:

```yaml
  measure:
    uses: lukaszsobala/cross-platform-benchmark/.github/workflows/measure.yml@v0.3.4
    permissions: { id-token: write, contents: read }
    with: { hub: https://hub.example, runner: self-hosted, label: "my box" }
```

On GitHub's own runners nothing in the chain belongs to the submitter and the
run is marked **verified**. On a self-hosted runner the signatures still hold
but the machine does not, so it is marked **attested** and never confused with
the first. Ordinary uploads need none of this and rank the same.

[rel]: https://github.com/lukaszsobala/cross-platform-benchmark/releases/latest
