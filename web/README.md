# cpu-bench results hub

A small web service for collecting `cpu-bench --json` results and comparing them
with everyone else's.

Python 3 standard library only — `http.server` + `sqlite3`, no framework, no
build step — matching the benchmark's own no-dependencies policy. Everything
lives in this directory and nothing outside it is referenced, so the whole
service can be moved elsewhere as one folder.

```sh
python3 server.py                        # http://127.0.0.1:8080
python3 server.py --db /var/lib/cpu-bench/runs.sqlite3 --host 0.0.0.0 --port 8080
python3 test_server.py                   # 35 tests, ~0.6s
```

From the repo root, `make serve` and `make test` do the first and last of those.
`make check` adds [the contract test](../tests/test_contract.py), which feeds a
freshly built benchmark's real output through this server's ingest path — the
tests here use a hand-written fixture and so cannot notice when the benchmark's
output and this code drift apart.

## Submitting a run

```sh
cpu-bench --full --json | ./submit.sh -l "workstation" -n "quiet, perf governor"

# or without the helper
cpu-bench --full --json | curl -fsS -X POST 'http://localhost:8080/api/runs?label=my+box' \
    -H 'Content-Type: application/json' --data-binary @-
```

The reply carries the run id and a **delete token** — the only way to withdraw a
run later, so keep it. Uploading through the web page stores the token in the
browser's local storage and lists it under *My uploads*.

**Submit `--full` runs.** A full run carries both halves — one record per core
from the per-core sweep, plus one per thread and a whole-machine total from the
multi-threaded run — and only the pair is interpretable: the totals say what the
machine does at once, the per-core records say what each core type does on its
own. `--per-core` and plain runs are accepted too and simply carry the one half.
A `--disp-sweep` dump is rejected; it is a diagnostic curve with no summary
metrics to rank.

Every upload records the toolchain that produced it: compiler and version, the
driver invoked (which is what distinguishes a cross-compile) and the exact
`CFLAGS` the binary was built with. Two results built with different `-march` or
`-O` levels are not the same measurement, and the run detail shows the string so
that is visible rather than assumed.

**`build.flags` is verbatim, so check what is in it.** It is whatever `CFLAGS`
the binary was compiled with, baked in at build time and published unedited. A
cross-compile carrying `--sysroot=/home/you/toolchain`, or a local `-I` path,
uploads that path to a public leaderboard along with the numbers. An upload also
names your CPU model and kernel release — that is the point of the board, but it
is worth knowing before pointing `submit.sh` at a public hub.

## What the page does

- **Leaderboard** — **one row per upload**, filtered by architecture, by build
  flags and by scope (best core / best thread / whole machine), sorted by any
  metric. Toggle *per GHz* to divide rates by clock and turn `MEMlat` into
  cycles, which compares microarchitecture rather than clock speed.
- **Compare** — tick any rows and see them side by side, one bar group per
  metric, normalised to the best of the selection. The one view that always
  shows every metric: it is the drill-down, and trimming it would defeat it.
- **Run detail** — click a machine to see the run: what it is and when it
  landed, with the build flags, config, DRAM clock and checksum folded behind a
  disclosure, then every record and where the run's best values land as a
  percentile of everything uploaded with the same build flags.
- **Upload** — file picker or paste, plus the curl one-liner for the machine that
  actually ran the benchmark.

### One upload is one row

A run measures every core, so a 128-core machine arrives as 128 records. They
are all stored and all shown, but they are one *result*: the board is grouped by
upload, and each row is the record that came out best at the metric being sorted
on — pick `MEMlat` and the row becomes the machine's quickest core, not its
fastest one. Every column in a row comes from that single record, so a row is a
core that existed rather than a per-metric best of several, and `▸` unfolds the
rest of them for the run. Rows at either level can be ticked for *Compare*.

The alternative — a row per core — let one upload fill the top of the board with
eight near-identical entries and pushed everyone else off it.

### Three columns, not fourteen

The benchmark reports fourteen rankable metrics per record. All fourteen at five
significant figures is a data dump nobody reads, so the tables carry the
`headline` set from `METRICS` — `score`, `INT-thr` and `MEM`: the geomean, the
compute rate it is mostly made of, and the memory bandwidth it cannot stand in
for — plus whichever metric the board is being sorted on, since a board ordered
by a number it does not print reads as arbitrary. *Columns → all metrics*
restores the rest. Nothing is dropped from the data: every metric is still
sortable, still filtered on, still in *Compare*, and still in the raw document.

Two comparability rules are enforced in the UI rather than left to the reader,
because getting them wrong is the easiest way to draw a wrong conclusion:

- Results are only comparable between builds with matching **vectorize** and
  **FMA** flags. Every row shows its flags, the filters can pin them, the
  percentile is computed only within a matching population, and the compare view
  warns when a selection mixes them.
- `score` is a geomean of absolute rates, so it rewards clock as much as
  microarchitecture, and the benchmark's own README calls it comparable across
  the cores of one run rather than across machines. It is shown, but *per GHz*
  is the honest cross-machine view.

## API

| Method | Path | Purpose |
| --- | --- | --- |
| `POST` | `/api/runs?label=&notes=` | upload one JSON document; returns `{id, delete_token, records}` |
| `GET` | `/api/runs?limit=&offset=` | recent runs |
| `GET` | `/api/runs/<id>` | one run with all its records |
| `GET` | `/api/runs/<id>/raw` | the original uploaded document, verbatim |
| `GET` | `/api/runs/<id>/rank` | percentile per metric within the matching build population |
| `DELETE` | `/api/runs/<id>` | withdraw a run; needs the `X-Delete-Token` header |
| `GET` | `/api/cores?scope=&target=&vectorize=&fma=&q=&sort=&order=&limit=` | leaderboard rows: one per upload, each the run's best record at `sort`, with `records` counting those it stands for |
| `GET` | `/api/cores?run=<id>&scope=&sort=&order=` | every record of one run, in board order — what a row unfolds to |
| `GET` | `/api/cores?ids=1,2,3` | named records, for a shared comparison link |
| `GET` | `/api/metrics` | metric definitions (key, label, unit, direction) |
| `GET` | `/api/stats` | run/record counts per architecture |

The three `/api/cores` forms differ only in `group`, which is `run` (collapse to
one row per upload) by default and `none` when `run=` or `ids=` is given, since
both of those address records rather than rank them. Pass it explicitly to
override either default.

`/api/metrics` is the single source of truth for the metric set *as far as the
front end is concerned* — it builds its columns from that response, so no
JavaScript changes when a metric is added. Ingestion is a separate list: a new
metric needs `CORE_FIELDS` and a `cores` column as well, and the full checklist
is in [schema/cpu-bench-1.md](../schema/cpu-bench-1.md).

## Storage

Two SQLite tables ([schema.sql](schema.sql)): `runs` holds the metadata and the
verbatim upload, `cores` holds one flattened row per record so the leaderboard
can sort and filter in SQL. A `--full` upload is one `runs` row with both kinds of
record hanging off it, told apart by `cores.scope`. The original document is
always kept, so a change to the flattening can be replayed over existing uploads,
and a database made by an older build gains new columns on startup.

## Notes on operating it

Uploads are unauthenticated — it is a scoreboard, not an account system. What is
in place:

- 1 MiB body cap, 1024 records per upload, and range checks on every number;
  `NaN` and `Infinity` are rejected rather than stored.
- Per-address upload rate limit (`--rate-limit`, default 30/hour, `0` disables).
  Behind a reverse proxy pass `--trust-proxy` so the limit sees the real client
  address rather than the proxy's.
- Parameterised SQL throughout; the one interpolated identifier (the sort column)
  is whitelisted against the metric table.
- The front end writes every uploaded string with `textContent`, and the server
  sends `Content-Security-Policy: default-src 'self'` with no inline script, so a
  crafted CPU model string cannot become markup.
- Strings are truncated and stripped of control characters on the way in.

What is deliberately **not** here: TLS, accounts, and any claim that an uploaded
number was actually produced by the machine it names. Anyone can post anything;
treat the board as self-reported. Run it behind a TLS-terminating proxy if it is
public.

The database is a single file — back it up by copying it (or `sqlite3 runs.sqlite3
".backup out.sqlite3"` while running, since WAL is enabled).
