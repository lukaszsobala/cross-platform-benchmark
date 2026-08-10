# `cpu-bench/1` — the result document

The only interface between the two halves of this repo. The benchmark
([bench/](../bench/)) writes it; the results hub ([web/](../web/)) reads it.
They share no code, so this file and [tests/test_contract.py](../tests/test_contract.py)
are what keep them in agreement — the test enforces every rule stated here, and
a live sample lives in [testdata/full-run.json](../testdata/full-run.json).

Produced by `cpu-bench --json` (also `--per-core --json`, `--full --json`).
Written by `json_open` / `json_result` / `emit_json` in
[bench/src/bench.c](../bench/src/bench.c); read by `validate()` in
[web/server.py](../web/server.py).

## Shape

```json
{
  "schema": "cpu-bench/1",
  "mode": "full",
  "build":  { "compiler": …, "compiler_version": …, "cc": …, "flags": …,
              "target": …, "vectorize": false, "fma": false },
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
on machines exposing a DRAM devfreq node; `system` is only as complete as
`uname(2)` was.

A `--disp-sweep` dump carries the same `schema` and `mode: "disp-sweep"`. The
hub **rejects** it: it is a diagnostic curve with no summary metrics to rank.

## Records

One object per measured core, thread, or whole-machine total. Every key below is
always present; anything not measured is `null`, never absent and never `0`.

`scope` (`cpu`|`thread`|`total`), `cpu`, `mhz`, `mhz_src`, `int_lat_mops`,
`int_thr_mops`, `ilp`, `mul_thr_mmul_s`, `fp_lat_mflops`, `fp_thr_mflops`,
`filp`, `mem_gbps`, `mem_lat_ns`, `mem_lat8_ns`, `mlp`, `disp_thr_mcall_s`,
`disp_cap_calls`, `disp_prediction`, `disp_gain`, `disp_span`, `score`.

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
   its columns from, so no front-end change is needed.
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
