# cpcpub results hub

Collects `cpcpub` results and puts them side by side. Python 3 standard library
only — `http.server` + `sqlite3`, no framework, no build step. Optional: the
benchmark does not need it.

```sh
python3 server.py                        # http://127.0.0.1:8080
python3 server.py --db /var/lib/cpcpub/runs.sqlite3 --host 0.0.0.0 --port 8782
python3 test_server.py                   # the hub's own tests
```

From the repo root: `make serve`, `make test`, and `make check` (which also runs
[the contract test](../tests/test_contract.py) against a freshly built binary).

**Run it behind a TLS-terminating proxy if it is public** — there is no TLS
here, and a password or an upload token would otherwise cross the network in the
clear. With a proxy in front, pass `--trust-proxy` so rate limits see the real
client address and session cookies are marked `Secure`.

## Submitting

From the machine that measured, which is the usual way:

```sh
cpcpub --full --submit http://my.server:8782 --token YOUR-TOKEN --label "my box"
```

The token is optional — without one the upload is anonymous and the reply
carries the delete token that is then the only way to withdraw it. `$CPCPUB_HUB`
and `$CPCPUB_TOKEN` work in place of the flags.

From a browser: the *Submit a result* tab takes a `run.json` by drag, click or
paste. From a file you already have:

```sh
curl -fsS -X POST 'http://my.server:8782/api/runs?label=my+box' \
    -H 'Authorization: Bearer YOUR-TOKEN' \
    -H 'Content-Type: application/json' --data-binary @run.json
```

Submit `--full` runs where you can: only the pair of halves is interpretable.
A `--variants` run uploads as one run per build variant, since results only
compare between matching builds. A `--disp-sweep` dump is refused — it is a
diagnostic curve with no summary metrics to rank.

**`build.flags` is verbatim.** It is whatever `CFLAGS` the binary was built
with, so a cross-compile carrying `--sysroot=/home/you/...` uploads that path
along with the numbers. An upload also names your CPU model and kernel release.

## Accounts

Optional throughout; an anonymous upload is never second-class. An account puts
your name on your runs, lets you withdraw them from any browser, carries the
upload token, and gets a larger upload allowance. It is a name and a password —
**no email**, and so no password reset. Closing one deletes its runs.

Registering costs a small proof of work (a SHA-256 search the browser does in
about half a second) and is capped per address. Neither stops a determined
person; together they stop a script taking every name overnight.

The page hashes at upwards of 130k a second — it carries its own SHA-256, since
`crypto.subtle` needs a secure context a plain-http hub does not have — and each
bit doubles the work: on average 16 is half a second, 20 is eight seconds, and
22, the maximum, is half a minute. It is a random search, so one attempt in ten
finishes almost at once and one in ten takes several times the average. Above 22
the tail runs past what the page will try and past the challenge's own lifetime,
so the ceiling is where the browser is, not where a server would like it.

```sh
python3 server.py --register-pow 20         # more work per account (default 16)
python3 server.py --register-limit 2        # new accounts per address per hour
python3 server.py --signup-token CODE       # invite-only, still readable by all
python3 server.py --no-register             # take no new accounts at all
```

Also: `--rate-limit` (uploads per hour, default 30, four times that for an
account), `--auth-limit` (sign-in attempts per address per 15 minutes).

## What a result proves

Nothing about the numbers, and the board says so rather than implying otherwise.

| shown as | what it means |
| --- | --- |
| `vx.x.x build` | the result names the checksum of a binary that release published, so it compares directly with every other result naming it — the run's own word, not a check |
| nothing | a local build, or one this hub has no manifest for. It ranks exactly the same |

**A benchmark cannot prove it was run.** Any key it carried to sign its own
output would sit inside a binary anyone can download and read out. Evidence
would have to come from somewhere the submitter does not control — and every
arrangement of that means running the benchmark inside a CI job: either on
someone else's hardware, which is not the machine you wanted measured, or on a
build agent installed on your own, which is not a benchmark you run with one
command. GitHub does not even publish that agent for riscv64, loongarch64,
ppc64le or s390x, which are half of what this project exists to compare.

So the hub checks the two things it can. **Did these two results come from the
same program** — what `build.binary_sha256` against a release manifest answers,
and why `?verified=release` is the only filter of its kind left. And **does this
document agree with itself**: `score` is a geometric mean of six numbers in the
same record and `ilp`, `filp` and `mlp` are ratios of two each, so a result
edited after the benchmark wrote it contradicts its own arithmetic. The hub
recomputes all four and refuses the upload with a message naming the field; see
[the schema](../schema/cpu-bench-1.md#four-of-them-are-arithmetic-on-the-others).

That second check raises the price of a fake from "edit one number" to "rescale
six inputs and recompute the four derived from them" — and someone who pays it
passes, so nothing is displayed for passing and there is no badge for it. Treat
every number on the board as self-reported, because it is.

This hub used to accept a GitHub OIDC token signed over an uploaded result and
stored two trust tiers from it. That is gone: `attest.py`, the `measure.yml`
workflow and the `--attest-workflow`, `--no-attest` and `--jwks-file` flags no
longer exist. The `attest_*` columns stay on the `runs` table so an older
database still opens holding what it recorded, and `?verified=ci`, `=attested`
and `=1` still answer — as the release filter — so a saved link does not break.

The `vx.x.x` in that first row is the **newest** release publishing those exact
bytes, not the one that first did. Most releases rebuild every target and only
some of them come out different, so an unchanged binary is published again by
each release after it — and a board that named the earliest would split one
population of identical builds into several that read as different ones. Load
the manifests in any order you like; the answer does not depend on it.

Release digests are loaded once per release, and mark runs already stored:

```sh
python3 verified.py --db runs.sqlite3 \
  https://github.com/OWNER/REPO/releases/download/v0.1.0/verified-builds.json
python3 verified.py --db runs.sqlite3 --list
python3 verified.py --db runs.sqlite3 --forget v0.1.0
```

## API

| Method | Path | Purpose |
| --- | --- | --- |
| `POST` | `/api/runs?label=&notes=` | upload one document; returns `{id, delete_token, user, records}` |
| `GET` | `/api/runs?limit=&offset=&user=` | recent runs, optionally one submitter's |
| `GET` | `/api/runs/<id>`, `/raw`, `/rank` | one run; the document as uploaded; its percentile per metric |
| `DELETE` | `/api/runs/<id>` | withdraw; needs `X-Delete-Token` or a signed-in owner |
| `GET` | `/api/cores?scope=&target=&os=&vectorize=&fma=&q=&user=&verified=release&sort=&order=&norm=&limit=&offset=` | leaderboard rows: one per upload, each its best record at `sort` |
| `GET` | `/api/cores?run=<id>` / `?ids=1,2,3` | every record of one run / named records |
| `GET` | `/api/metrics`, `/api/stats`, `/api/builds` | metric definitions; counts, including one per architecture and one per operating system, which is what fills the board's **Arch** and **OS** pickers; recognised release digests |
| `GET` | `/api/users/<name>` | public profile: name, since, run count |
| `GET` | `/api/auth/policy` | what registering here costs: `{open, invite_required, bits}` |
| `GET` | `/api/auth/challenge` | the same, plus a single-use puzzle to register with |
| `POST` | `/api/auth/register` | `{name, password, challenge, nonce}`, plus `invite` where required |
| `POST` | `/api/auth/login` `/logout` `/token` `/password` `/close` | sign in; sign out; reissue the upload token; change password; delete the account |
| `GET` | `/api/auth/me` | the signed-in account, or `{"user": null}` |

`norm` is which reading of `sort` to rank on: `abs` (default) for the figure as
measured, `ghz` for it per gigahertz of clock — rates divided by GHz, `time`
metrics turned into cycles, `ratio` and `fixed` ones left alone, and a record
whose clock was never reported ranked on its raw value, because that is what is
shown for it. It is the page's **Values** picker, and it is a server-side sort
because the board is paged: ordering a page after it arrives would only sort the
rows that happened to be on it. With `group=run` it also decides which record
stands for an upload — per GHz that is its most efficient core, not its fastest.

Both listings are paged: `limit` (default 50, clamped to 500 — the sizes the
page's own **Show** pickers offer) and `offset`, and both answer with
`{limit, offset, total}` beside the rows. `total` counts the filtered listing,
one row per upload where the board groups them, so it is exactly how many rows
stepping `offset` will walk through. An offset past the end is an empty page,
not an error. The page size decides how much is on screen and never how much
can be read: every result is reachable by paging.

Two ways to authenticate: `Authorization: Bearer <upload token>` for a machine,
or the `cpb_session` cookie for a browser. A cookie-authenticated write must
also echo the `cpb_csrf` cookie in an `X-CSRF-Token` header; a token-authenticated
one needs nothing else. An upload token that names no account is a `401`, never
a silent anonymous upload.

`/api/metrics` is what the front end builds its columns from, so adding a metric
changes no JavaScript. Ingestion is a separate list — see
[schema/cpu-bench-1.md](../schema/cpu-bench-1.md) for the checklist.

## Storage

Five SQLite tables ([schema.sql](schema.sql)): `runs` (metadata plus the
verbatim upload), `cores` (one flattened row per record, which is what the
leaderboard sorts in SQL), `users`, `sessions`, `verified_builds`. Older
databases gain new columns on startup. The database is one file: back it up by
copying it, or `sqlite3 runs.sqlite3 ".backup out.sqlite3"` while it runs.

What is deliberately not here: TLS, password recovery, and any claim that an
uploaded number was produced by the machine it names. Treat the board as
self-reported — see [What a result proves](#what-a-result-proves).
