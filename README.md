# cross-platform-benchmark

A portable CPU benchmark and a place to compare its results.

| | | |
| --- | --- | --- |
| [bench/](bench/) | the benchmark | C2x, GCC 13+, libc + libm + pthreads |
| [web/](web/) | the results hub | Python 3 stdlib, `http.server` + `sqlite3` |
| [schema/](schema/cpu-bench-1.md) | the contract between them | `cpu-bench/1` |

Neither half depends on the other's language, build system or libraries. They
meet at one JSON document, and they are in one repo so that a change to it can
land on both sides in a single commit.

```sh
make                # build the benchmark          -> bench/cpu-bench
make check          # contract test + hub tests
make serve          # the hub on http://127.0.0.1:8080
make testdata       # regenerate testdata/full-run.json on this machine

bench/cpu-bench --full                          # measure this machine
bench/cpu-bench --full --json | web/submit.sh -l "my box"
```

The top-level `Makefile` forwards the benchmark's tuning targets and variables
unchanged, so `make native`, `make sg2000` and `make VECTORIZE=1` work from here
as well as from [bench/](bench/).

## Keeping the two halves in step

The benchmark writes `cpu-bench --json`; the hub ingests it and nothing else.
Because they share no code, a field added to one side and forgotten on the other
fails no compiler and no existing test — the measurement is simply taken and
dropped, or a column fills with `NULL` forever, and neither shows up in the
numbers.

[tests/test_contract.py](tests/test_contract.py) is what catches that. It runs
the binary that was just built, feeds its real output through the hub's
validation and storage, and asserts that the record fields match *exactly* in
both directions — a document with an extra key fails, and so does one missing a
key. It also runs against [testdata/full-run.json](testdata/), a committed sample
of real output, so the check still means something on a machine with no compiler.

[schema/cpu-bench-1.md](schema/cpu-bench-1.md) is the written half of the same
contract: the document shape, which fields the hub deliberately drops, the five
places one new metric has to be added, and when a change forces `cpu-bench/2`.
Read it before changing anything `--json` prints.

```
make check
├── contract   builds bench/, runs it, round-trips the output through the hub
└── test       the hub's own tests (HTTP, validation, SQL)
```
