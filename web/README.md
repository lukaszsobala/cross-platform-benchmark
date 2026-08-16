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
python3 test_server.py                   # 62 tests, ~2.5s
```

From the repo root, `make serve` and `make test` do the first and last of those.
`make check` adds [the contract test](../tests/test_contract.py), which feeds a
freshly built benchmark's real output through this server's ingest path — the
tests here use a hand-written fixture and so cannot notice when the benchmark's
output and this code drift apart.

## Submitting a run

Three ways in, in rough order of how much you have to know:

**From the page.** Open the hub, go to *Submit a result*, and drop `run.json`
onto the target — or click it, or paste the document. A `--variants` file holds
one document per build variant; the page sees that, says so, and uploads each as
its own run, since results are only comparable between matching builds.

**From the machine that measured, once it has been told where.**

```sh
web/submit.sh --save -u https://hub.example -t YOUR-TOKEN   # once per machine
bench/cpu-bench --full --json | web/submit.sh -l "workstation"
make submit LABEL="workstation"                             # or all of it at once
```

`--save` writes the hub URL and your upload token to
`~/.config/cpu-bench/hub.conf` (mode 600), so later runs carry no flags. With
nothing on stdin `submit.sh` runs the benchmark itself, so
`web/submit.sh -l "my box"` is the whole gesture.

**Or with no helper at all.**

```sh
cpu-bench --full --json | curl -fsS -X POST 'http://localhost:8080/api/runs?label=my+box' \
    -H 'Authorization: Bearer YOUR-TOKEN' \
    -H 'Content-Type: application/json' --data-binary @-
```

The *Submit a result* tab prints both of these with this hub's address and your
token already in them, so there is nothing to edit before pasting.

The reply carries the run id and a **delete token**. On an anonymous upload that
token is the only way to withdraw the run later, so keep it — the web page keeps
it for you in local storage and lists it under *My uploads*. An upload made with
an account needs no token: it can be withdrawn from any browser signed in as
you.

## Accounts

Optional throughout. Uploading, ranking and comparing all work exactly as they
did without one, and an anonymous upload is never second-class. What an account
adds:

- your name on the runs you upload, and a board that can be narrowed to one
  submitter — one person's machines read together rather than scattered;
- withdrawing a run from any browser rather than only from the one holding its
  delete token;
- an **upload token** for the machine that ran the benchmark, so a result goes
  up without a token being copied back by hand;
- a larger upload allowance, since a signed-in submitter is one submitter rather
  than one address.

An account is a name and a password. **No email is collected**, which also means
there is no password reset: a forgotten password is a new account. Closing an
account deletes the runs uploaded under it, rather than leaving rows nobody can
withdraw.

The upload token grants uploading and withdrawing as you, and nothing else — it
cannot change the password or read anything the board does not already show.
*Account* displays it, and reissues it if it leaks. It is a credential: keep it
out of shared shell history and out of anything you publish.

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
- **Submit a result** — the command that produces a result, then a drop target
  that takes it: drag, click or paste. A `--variants` file becomes one run per
  build variant. Folded underneath, the shell commands for submitting from the
  machine itself, carrying this hub's address and your upload token.
- **My uploads** — runs on your account, withdrawable from anywhere, and
  anonymous ones made from this browser with the delete tokens they need.
- **Account** — sign in or create one, read or reissue the upload token, change
  the password, close the account.

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
| `POST` | `/api/runs?label=&notes=` | upload one JSON document; returns `{id, delete_token, user, records}` |
| `GET` | `/api/runs?limit=&offset=&user=` | recent runs, optionally one submitter's |
| `GET` | `/api/runs/<id>` | one run with all its records |
| `GET` | `/api/runs/<id>/raw` | the original uploaded document, verbatim |
| `GET` | `/api/runs/<id>/rank` | percentile per metric within the matching build population |
| `DELETE` | `/api/runs/<id>` | withdraw a run; needs the `X-Delete-Token` header, or a signed-in owner |
| `GET` | `/api/cores?scope=&target=&vectorize=&fma=&q=&user=&sort=&order=&limit=` | leaderboard rows: one per upload, each the run's best record at `sort` |
| `GET` | `/api/cores?run=<id>&scope=&sort=&order=` | every record of one run, in board order |
| `GET` | `/api/cores?ids=1,2,3` | named records, for a shared comparison link |
| `GET` | `/api/metrics` | metric definitions (key, label, unit, direction) |
| `GET` | `/api/stats` | run/record and account counts |
| `GET` | `/api/users/<name>` | public profile: name, since, run count |
| `POST` | `/api/auth/register` `/api/auth/login` | `{name, password}`; sets the session cookies, returns the account and its upload token |
| `GET` | `/api/auth/me` | the signed-in account, or `{"user": null}` |
| `POST` | `/api/auth/logout` | end this browser's session |
| `POST` | `/api/auth/token` | issue a new upload token, retiring the old one |
| `POST` | `/api/auth/password` | `{current, password}`; also ends every other session |
| `POST` | `/api/auth/close` | `{password}`; deletes the account and its runs |

The three `/api/cores` forms differ only in `group`, which is `run` (one row per
upload) by default and `none` when `run=` or `ids=` is given, since both of those
address records rather than rank them. Pass it explicitly to override either.

`user=` takes an account name, or `me` for the signed-in one. An unknown name
matches nothing rather than everything — a board that quietly widened on a typo
would read as that person having uploaded the lot.

Two ways to authenticate, for the two kinds of caller:

```http
Authorization: Bearer <upload token>     # a machine, a script, curl
Cookie: cpb_session=...                  # a browser, from /api/auth/login
```

A token-authenticated request needs nothing else. A cookie-authenticated write
must also echo the `cpb_csrf` cookie in an `X-CSRF-Token` header — see below. An
upload token that names no account is a `401`, never a silent anonymous upload:
a stale token in a script would otherwise keep succeeding off the account it was
meant for.

`/api/metrics` is the single source of truth for the metric set *as far as the
front end is concerned* — it builds its columns from that response, so no
JavaScript changes when a metric is added. Ingestion is a separate list: a new
metric needs `CORE_FIELDS` and a `cores` column as well, and the full checklist
is in [schema/cpu-bench-1.md](../schema/cpu-bench-1.md).

## Storage

Four SQLite tables ([schema.sql](schema.sql)): `runs` holds the metadata and the
verbatim upload, `cores` holds one flattened row per record so the leaderboard
can sort and filter in SQL, `users` holds the accounts and `sessions` the signed-in
browsers. The original document is always kept, so a change to the flattening can
be replayed over existing uploads, and a database made by an older build gains
new columns on startup — including `runs.user_id`, so a hub that has been
collecting anonymous uploads keeps every one of them when it gains accounts.

## Notes on operating it

What is in place:

- 1 MiB body cap, 1024 records per upload, and range checks on every number;
  `NaN` and `Infinity` are rejected rather than stored.
- Upload rate limit (`--rate-limit`, default 30/hour, `0` disables), counted per
  account where there is one and per address where there is not; an account gets
  four times the room, being one submitter rather than one NAT. Behind a reverse
  proxy pass `--trust-proxy` so the limit sees the real client address.
- Sign-in attempts are limited separately (`--auth-limit`, default 20 per 15
  minutes per address) — per address rather than per name, so nobody can lock
  someone else's account by guessing at it.
- Passwords are stored as salted scrypt digests (n=2¹⁴, ~16 MiB per attempt) and
  compared in constant time. Session tokens are stored as SHA-256, so a copy of
  the database is not a list of live sessions. Upload tokens are stored readable,
  because the account page has to be able to show one again.
- Session cookies are `HttpOnly` and `SameSite=Lax`; a cookie-authenticated write
  must also echo the readable `cpb_csrf` cookie in an `X-CSRF-Token` header, so a
  cross-site form cannot upload or delete on a visitor's behalf. Token-authenticated
  requests need no such proof — a header is not something another site can set.
- With `--trust-proxy` the cookies are marked `Secure` when `X-Forwarded-Proto`
  says the browser arrived over TLS.
- Parameterised SQL throughout; the one interpolated identifier (the sort column)
  is whitelisted against the metric table.
- The front end writes every uploaded string with `textContent`, and the server
  sends `Content-Security-Policy: default-src 'self'` with no inline script, so a
  crafted CPU model string cannot become markup.
- Strings are truncated and stripped of control characters on the way in. Account
  names are refused rather than truncated, since a name silently cut to fit is
  not the one that was asked for.

What is deliberately **not** here: TLS, password recovery, and any claim that an
uploaded number was actually produced by the machine it names. An account says
the same person uploaded these runs — not that they are true. Anyone can post
anything; treat the board as self-reported. **Run it behind a TLS-terminating
proxy if it is public**: without TLS a password and an upload token both cross
the network in the clear.

The database is a single file — back it up by copying it (or `sqlite3 runs.sqlite3
".backup out.sqlite3"` while running, since WAL is enabled).
