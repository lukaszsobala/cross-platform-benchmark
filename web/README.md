# cpu-bench results hub

A small web service for collecting `cpu-bench --json` results and comparing them
with everyone else's. It is an addition to the benchmark, not the point of it:
[bench/README.md](../bench/README.md) is what explains the numbers.

Python 3 standard library only — `http.server` + `sqlite3`, no framework, no
build step — matching the benchmark's own no-dependencies policy. Everything
lives in this directory, so the whole service moves as one folder.

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

**Submit `--full` runs.** Only the pair of halves is interpretable: the totals
say what the machine does at once, the per-core records say what each core type
does on its own. `--per-core` and plain runs are accepted and carry the one half.
A `--disp-sweep` dump is rejected; it is a diagnostic curve with no summary
metrics to rank.

**`build.flags` is verbatim, so check what is in it.** It is whatever `CFLAGS`
the binary was compiled with, baked in at build time and published unedited. A
cross-compile carrying `--sysroot=/home/you/toolchain`, or a local `-I` path,
uploads that path to a public leaderboard along with the numbers. An upload also
names your CPU model and kernel release — that is the point of the board, but it
is worth knowing before pointing `submit.sh` at a public hub.

## What the page does

- **Leaderboard** — one row per upload, ranked by whichever column heading you
  click and filtered by architecture, build flags and scope (whole machine /
  best core / best thread). `▸` unfolds the records behind a row. Three metrics
  are shown by default plus whichever is being ranked on; *detailed* restores
  the rest. *per GHz* divides the core rates by clock and turns `MEMlat` into
  cycles; `MEM` stays in `GB/s` either way, being set by the memory controller
  rather than the core clock.
- **Compare** — tick rows and see them side by side, every metric, one bar group
  each. A bar is the measurement, scaled against the largest in the selection,
  and each group says *higher is better* or *lower is better* in words. One
  comparison holds one scope: a whole-machine total is the sum over every
  thread, so changing **Rank by** carries the ticks across to that scope rather
  than mixing the two.
- **Run detail** — the run, its build flags and config, every record it carries,
  and where its best values land as a percentile of everything uploaded with the
  same build flags.
- **Upload** — file picker or paste, plus the curl one-liner for the machine that
  actually ran the benchmark.

Two comparability rules are enforced here rather than left to the reader, since
getting them wrong is the easiest way to draw a wrong conclusion. Results only
compare between builds with matching **vectorize** and **FMA** flags — the
filters can pin them, percentiles are computed only within a matching
population, and *Compare* warns when a selection mixes them. And `score` is a
geomean of absolute rates, so it rewards clock as much as microarchitecture;
*per GHz* is the honest cross-machine view.

## API

| Method | Path | Purpose |
| --- | --- | --- |
| `POST` | `/api/runs?label=&notes=` | upload one JSON document; returns `{id, delete_token, records}` |
| `GET` | `/api/runs?limit=&offset=` | recent runs |
| `GET` | `/api/runs/<id>` | one run with all its records |
| `GET` | `/api/runs/<id>/raw` | the original uploaded document, verbatim |
| `GET` | `/api/runs/<id>/rank` | percentile per metric within the matching build population |
| `DELETE` | `/api/runs/<id>` | withdraw a run; needs the `X-Delete-Token` header |
| `GET` | `/api/cores?scope=&target=&vectorize=&fma=&q=&sort=&order=&limit=` | leaderboard rows: one per upload, each the run's best record at `sort` |
| `GET` | `/api/cores?run=<id>&scope=&sort=&order=` | every record of one run, in board order |
| `GET` | `/api/cores?ids=1,2,3` | named records, for a shared comparison link |
| `GET` | `/api/metrics` | metric definitions (key, label, unit, direction) |
| `GET` | `/api/stats` | run/record counts per architecture |

The three `/api/cores` forms differ only in `group`, which is `run` (one row per
upload) by default and `none` when `run=` or `ids=` is given, since both of those
address records rather than rank them. Pass it explicitly to override either.

`/api/metrics` is the single source of truth for the metric set *as far as the
front end is concerned* — it builds its columns from that response, so no
JavaScript changes when a metric is added. Ingestion is a separate list: a new
metric needs `CORE_FIELDS` and a `cores` column as well, and the full checklist
is in [schema/cpu-bench-1.md](../schema/cpu-bench-1.md).

## Storage

Two SQLite tables ([schema.sql](schema.sql)): `runs` holds the metadata and the
verbatim upload, `cores` holds one flattened row per record so the leaderboard
can sort and filter in SQL. The original document is always kept, so a change to
the flattening can be replayed over existing uploads, and a database made by an
older build gains new columns on startup.

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
