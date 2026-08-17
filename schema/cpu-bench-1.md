# `cpu-bench/1` — the result document

The only interface between the two halves of this repo. The benchmark
([bench/](../bench/)) writes it; the results hub ([web/](../web/)) reads it.
They share no code, so this file and [tests/test_contract.py](../tests/test_contract.py)
are what keep them in agreement — the test enforces every rule stated here, and
a live sample lives in [testdata/full-run.json](../testdata/full-run.json).

Produced by `cpcpub --json` (also `--per-core --json`, `--full --json`).
Written by `json_open` / `json_result` / `emit_json` in
[bench/src/bench.c](../bench/src/bench.c); read by `validate()` in
[web/server.py](../web/server.py).

## Shape

```json
{
  "schema": "cpu-bench/1",
  "mode": "full",
  "build":  { "compiler": …, "compiler_version": …, "cc": …, "flags": …,
              "target": …, "binary_sha256": …, "vectorize": false, "fma": false },
  "system": { "sysname": …, "release": …, "machine": …, "cpus": 8,
              "cpu_models": … },
  "config": { "threads": …, "seconds_per_phase": …, "reps": …,
              "warmup_seconds": …, "mem_bytes_per_thread": …, "pin": true,
              "clock": "raw", "seed": 1 },
  "dram":   { "name": …, "mhz_min": …, "mhz_max": … },
  "threads": [ record, … ],
  "total":   record,
  "cores":   [ record, … ],
  "core_spread": 1.46,
  "checksum": "0x…"
}
```

`mode` is one of `threads`, `per-core`, `full`. `threads`/`total` are present
for `threads` and `full`; `cores` for `per-core` and `full`. `dram` appears only
on machines exposing a DRAM devfreq node; `system` is only as complete as the
host will say. Each system names its own hardware and the field passes that
through unchanged, so the same silicon reads `aarch64` on Linux, `arm64` on
macOS and `ARM64` on Windows — `build.target` is the normalised one to key on.
`config.clock` records the clock that was *used*: asking for `raw` on a host
with no unadjusted monotonic clock (Windows) yields `mono` here.

A `--disp-sweep` dump carries the same `schema` and `mode: "disp-sweep"`. The
hub **rejects** it: it is a diagnostic curve with no summary metrics to rank.

## `build.binary_sha256`

The SHA-256 of the binary that produced the document, in 64 lowercase hex
digits — the benchmark hashing its own executable at report time, found through
`/proc/self/exe`, `_NSGetExecutablePath` or `GetModuleFileName` depending on the
host. **Absent, never `null`**, where none of those can answer: a key carrying an
empty digest would match the next empty digest and mean the opposite of what it
says.

It exists so a hub can tell a run that says it came from a published build from
one that says nothing. The hub holds the digests a release attached and marks a
matching run as that release's build.

**It is a claim, not a proof, and the hub presents it as one.** This field is
inside a document its submitter wrote, so copying an official digest into a
hand-edited file produces exactly the same bytes as running the official build.
Nor could the benchmark sign its own output to fix that: a key inside a public
binary is a key anyone can extract. Evidence has to come from somewhere the
submitter does not control, which is what
[the measure workflow](../.github/workflows/measure.yml) and the hub's
attestation checking are for — see
[web/README.md](../web/README.md#what-a-result-proves). What this field buys on
its own is that two results claiming one digest were produced by the same
machine code, which is the comparability question rather than the honesty one.

## More than one document

`cpcpub --variants` measures the same machine once per build variant and
writes a JSON **array** of the documents above, one per variant, in the order
they ran. Each element is an ordinary `cpu-bench/1` document and the array is
the only thing that is new — so this is not a schema change, and the hub is not
asked to understand it.

That works because a variant *is* `build.vectorize` and `build.fma`, which the
hub already stores per run and indexes on. Four variants are four runs of one
machine, which is exactly the shape the leaderboard needs to put them side by
side. Posting the array as one body would instead need a run to hold four of
every metric, and every comparison in the hub to learn about a dimension it does
not have.

`validate()` takes one document, so the splitting happens before the upload.
`cpcpub --variants --submit URL` never builds the array at all: it posts each
variant's document as it finishes, appending the variant name to the label.
Anything else feeding the hub has to split it too — `jq '.[0]'`, or a loop.

## Records

One object per measured core, thread, or whole-machine total. Every key below is
always present; anything not measured is `null`, never absent and never `0`.

`scope` (`cpu`|`thread`|`total`), `cpu`, `mhz`, `mhz_src`, `int_lat_mops`,
`int_thr_mops`, `ilp`, `mul_thr_mmul_s`, `fp_lat_mflops`, `fp_thr_mflops`,
`filp`, `mem_gbps`, `mem_lat_ns`, `mem_lat8_ns`, `mlp`, `disp_thr_mcall_s`,
`disp_cap_calls`, `disp_prediction`, `disp_gain`, `disp_span`, `score`.

`score` means something different by `scope`: on a `cpu` or `thread` record it is
a single core's composite, on the `total` it is the same geomean over the summed
throughputs and so scales with core count. Both are the same key with the same
unit, so nothing rejects a comparison between them — the hub keeps them apart by
only ever ranking a scope against itself.

The set is exact in **both** directions — a record with an extra key fails the
contract test, and so does one missing a key. A field only one side knows about
is measured and then silently dropped, or stored as `NULL` forever; neither is
visible in the numbers.

## What the hub drops

`core_spread` is emitted and not ingested: it summarises the `cores` array, and
the hub already stores every record it is computed from. It is listed in
`UNCONSUMED_TOP` in the contract test — the classification is deliberate, so a
*newly* unread field trips the test instead of joining it unnoticed.

## Adding a metric

Five edits, in one commit, because a partial change is invisible at runtime:

1. `json_result()` in [bench/src/bench.c](../bench/src/bench.c) — emit the key.
2. `CORE_FIELDS` in [web/server.py](../web/server.py) — JSON key → column.
3. `METRICS` in [web/server.py](../web/server.py) — only if it should be
   rankable; this is what `/api/metrics` serves and what the front end builds
   its columns from, so no front-end change is needed. Leave `headline` off
   unless the metric belongs in every table by default: the tables show the
   headline set plus whatever is being sorted on, and the rest are one select
   away.
4. `cores` in [web/schema.sql](../web/schema.sql) — the column.
5. `Store.ADDED_COLUMNS` in [web/server.py](../web/server.py) — the migration
   that adds the column to a database made by an older build.

Then the record key list above, `make testdata`, and `make check`.

> **Gap worth knowing about.** `ADDED_COLUMNS` migrates `runs` only. `schema.sql`
> creates `cores` with `IF NOT EXISTS`, so a new metric column never reaches an
> existing database and the next upload fails on the unknown column. Nothing
> catches this yet — the contract test checks a *fresh* database, which always
> has every column. Adding a metric today means migrating `cores` by hand, or
> teaching `ADDED_COLUMNS` about both tables.

## Changing the version

The hub accepts exactly one `schema` string, so the version is a hard gate, not
a hint.

- **Additive** — a new field, or a new optional section. Stays `cpu-bench/1`.
  An older hub ignores what it does not know; an older benchmark's documents
  still validate, and the new column is `NULL` for them.
- **Breaking** — a renamed or removed field, a changed unit, a changed meaning
  for the same key. Bump to `cpu-bench/2`, and have the hub accept both for at
  least one release: uploads come from machines that were built months ago and
  cross-compiled elsewhere, and a rejected upload is a lost measurement.

A changed unit with an unchanged key is the dangerous case — nothing rejects it
and the leaderboard silently mixes two populations. Rename the key instead
(`mem_lat_ns` → `mem_lat_us`) so the contract test fails loudly.
