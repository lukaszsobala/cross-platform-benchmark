#!/usr/bin/env python3
"""cpcpub results hub: upload `cpcpub --json` output and compare runs.

Standard library only (http.server + sqlite3), matching the benchmark's own
no-dependencies policy.

    python3 web/server.py --db runs.sqlite3 --port 8080

An upload may be signed in or anonymous. An account is a name and a password;
it owns the runs uploaded under it, so they can be withdrawn from any browser
and carry a submitter on the board, and it carries an upload token that makes
`cpcpub --submit URL --token T` put the run on your name. Anonymous uploads
keep working exactly as before, delete token and all -- an account is a
convenience, never a gate. A rate limit, per account where there is one and per
address where there is not, keeps the obvious abuse out; registering costs a
small proof of work as well, which is what keeps a script from taking every
name on the hub overnight (--register-pow, --register-limit, --signup-token,
--no-register).

Nothing here claims an uploaded number was produced by the machine it names:
an account says the same person uploaded these runs, not that they are true.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import mimetypes
import re
import secrets
import sqlite3
import sys
import threading
import time
import urllib.parse
from collections import defaultdict, deque
from datetime import datetime, timedelta, timezone
from http import HTTPStatus
from http.cookies import CookieError, SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import cast

import attest

HERE = Path(__file__).resolve().parent
STATIC_DIR = HERE / "static"
SCHEMA_PATH = HERE / "schema.sql"

SCHEMA_ID = "cpu-bench/1"
MODES = {"threads", "per-core", "full"}
SCOPES = {"cpu", "thread", "total"}

# What the benchmark writes for build.binary_sha256, and what a release manifest
# publishes for each binary it attached.
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
BUILDS_SCHEMA_ID = "cpu-bench-builds/1"

# Where an upload carries its proof, when it has one. A header rather than a
# field in the document, because the signature is over the document's exact
# bytes and cannot be inside them.
ATTEST_HEADER = "X-CPU-Bench-Attestation"

MAX_BODY = 1 << 20          # 1 MiB: a --per-core dump of 256 CPUs is ~120 KiB
MAX_AUTH_BODY = 4096        # a name and a password; anything larger is noise
MAX_DRAIN = 8 << 20         # how much of a refused body to swallow before
                            # hanging up, so the client can read the 413
MAX_CORES = 1024
MAX_TEXT = 200
MAX_NOTES = 2000
MAX_VALUE = 1e12            # nothing this benchmark reports comes near this
ACCOUNT_RATE_FACTOR = 4     # how much more room a signed-in submitter gets

# Accounts -------------------------------------------------------------------
#
# A name has to be safe to print on the board and unambiguous between accounts:
# letters, digits and the three joiners, starting on a letter or digit.
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{2,31}$")
MIN_PASSWORD = 8
MAX_PASSWORD = 256          # scrypt costs the same either way; this bounds work
SESSION_DAYS = 30

# Registration -------------------------------------------------------------
#
# There is no email address to confirm and no captcha to farm out, so what
# stands between this hub and a script that registers all night is a small
# proof of work plus a hard per-address limit. Neither stops a determined
# person, and neither is meant to: they turn "free and instant" into "a second
# of CPU each and five an hour", which is what actually deters bulk signup.
#
# 16 bits is ~65k SHA-256 attempts. The page hashes at roughly 130k a second --
# it carries its own SHA-256, because crypto.subtle needs a secure context this
# hub may not have -- so that is about half a second: unnoticed by a person, and
# the difference between a thousand accounts a minute and a few. A hub that is
# being hammered can raise it.
REGISTER_POW_BITS = 16
# Each bit doubles the work, so at that rate 20 bits averages ~8 seconds and 22
# ~30 -- an average over a memoryless search, so a given attempt can take
# several times that and one in ten takes almost none. The ceiling is where the
# page is, not where a server would like it: it searches 2^26 nonces before
# giving up, and the challenge below expires, and 22 bits leaves both a wide
# margin (2^26 is sixteen times the mean, 600s is twenty). Asking for work that
# runs past either would be a door that looks open and is not.
MAX_POW_BITS = 22
CHALLENGE_TTL = 600.0       # seconds; long enough to solve, short enough to forget
MAX_CHALLENGES = 20000      # bound the memory a challenge flood can take
MAX_NONCE = 64
SESSION_COOKIE = "cpb_session"
CSRF_COOKIE = "cpb_csrf"    # readable by the page, unlike the session cookie
CSRF_HEADER = "X-CSRF-Token"

# scrypt parameters: ~16 MiB and a few tens of ms per attempt, which is a wall
# to offline guessing and imperceptible on a login. maxmem has to be given
# because OpenSSL's default is below what n=2^14 asks for on some builds.
SCRYPT_N, SCRYPT_R, SCRYPT_P = 1 << 14, 8, 1
SCRYPT_MAXMEM = 64 << 20

# The metric table is the single source of truth for the API and the UI.
#   kind  rate  -> per-clock normalisation divides by GHz
#         time  -> per-clock normalisation multiplies by GHz (ns -> cycles)
#         ratio -> already clock-independent
#         fixed -> the core clock is not what sets it; never normalise
#
#   MEM is `fixed` rather than `rate`. It is a DRAM streaming figure -- the
#   buffer is sized past LLC deliberately -- so the memory controller and the
#   DRAM clock set it, not the core's. GB/s per core-GHz would answer no
#   question: it would flatter whichever core happened to be clocked lower
#   while measuring the same memory. MEMlat stays `time` even though it is also
#   a DRAM figure, because latency in core cycles is a real quantity -- it is
#   what the stall costs *this* core, and a faster core does lose more cycles
#   to the same nanoseconds.
#   better  which direction is an improvement; "none" means do not rank it
#   headline  shown by default. Fourteen columns of five significant figures is
#         a data dump, not a result, so the tables carry these three -- the
#         geomean, the compute rate it is mostly made of, and memory bandwidth,
#         which the geomean cannot stand in for -- plus whichever metric the
#         board is being sorted on. The rest are one select away, and every
#         metric is still ranked, filtered and compared on.
METRICS = [
    {"key": "score",       "label": "score",    "unit": "geomean", "kind": "rate",  "better": "high", "headline": True},
    {"key": "int_thr",     "label": "INT-thr",  "unit": "Mop/s",  "kind": "rate",  "better": "high", "headline": True},
    {"key": "int_lat",     "label": "INT-lat",  "unit": "Mop/s",  "kind": "rate",  "better": "high"},
    {"key": "ilp",         "label": "ILP",      "unit": "x",       "kind": "ratio", "better": "high"},
    {"key": "mul_thr",     "label": "MUL-thr",  "unit": "Mmul/s",  "kind": "rate",  "better": "high"},
    {"key": "fp_thr",      "label": "FP-thr",   "unit": "Mflop/s", "kind": "rate",  "better": "high"},
    {"key": "fp_lat",      "label": "FP-lat",   "unit": "Mflop/s", "kind": "rate",  "better": "high"},
    {"key": "filp",        "label": "fILP",     "unit": "x",       "kind": "ratio", "better": "none"},
    {"key": "mem_gbps",    "label": "MEM",      "unit": "GB/s",    "kind": "fixed", "better": "high", "headline": True},
    {"key": "mem_lat_ns",  "label": "MEMlat",   "unit": "ns",      "kind": "time",  "better": "low"},
    {"key": "mem_lat8_ns", "label": "MEMlat/8", "unit": "ns",      "kind": "time",  "better": "low"},
    {"key": "mlp",         "label": "MLP",      "unit": "x",       "kind": "ratio", "better": "high"},
    {"key": "disp_thr",    "label": "DISP-thr", "unit": "Mcall/s", "kind": "rate",  "better": "high"},
    {"key": "disp_cap",    "label": "DISPcap",  "unit": "calls",   "kind": "ratio", "better": "high"},
]
METRIC_KEYS = [m["key"] for m in METRICS]
# The front end switches on these strings and falls through to "leave the value
# alone" for anything it does not know, so a typo here is a metric that quietly
# stops being normalised. The contract test pins the vocabulary.
METRIC_KINDS = {"rate", "time", "ratio", "fixed"}

# JSON key -> (column, type) for one core/thread record.
CORE_FIELDS = [
    ("cpu",              "cpu",             "int"),
    ("mhz",              "mhz",             "num"),
    ("mhz_src",          "mhz_src",         "str"),
    ("int_lat_mops",     "int_lat",         "num"),
    ("int_thr_mops",     "int_thr",         "num"),
    ("ilp",              "ilp",             "num"),
    ("mul_thr_mmul_s",   "mul_thr",         "num"),
    ("fp_lat_mflops",    "fp_lat",          "num"),
    ("fp_thr_mflops",    "fp_thr",          "num"),
    ("filp",             "filp",            "num"),
    ("mem_gbps",         "mem_gbps",        "num"),
    ("mem_lat_ns",       "mem_lat_ns",      "num"),
    ("mem_lat8_ns",      "mem_lat8_ns",     "num"),
    ("mlp",              "mlp",             "num"),
    ("disp_thr_mcall_s", "disp_thr",        "num"),
    ("disp_cap_calls",   "disp_cap",        "num"),
    ("disp_prediction",  "disp_prediction", "str"),
    ("disp_gain",        "disp_gain",       "num"),
    ("disp_span",        "disp_span",       "num"),
    ("score",            "score",           "num"),
]
CORE_COLUMNS = [col for _, col, _ in CORE_FIELDS]

# The submitter's name, resolved wherever a run is read. A correlated subquery
# rather than a join, so a run whose account has since been closed still reads
# back -- with no submitter, which is what it now is.
SUBMITTER = "(SELECT u.name FROM users u WHERE u.id = r.user_id) AS user"

# The release a run's binary *says* it came from, or NULL. Resolved at read
# time rather than stamped on at upload: a hub that loads a release manifest
# afterwards then marks the runs already sitting in it, instead of leaving them
# unmarked for having arrived first.
#
# Deliberately not called "verified" any more. It rests on a digest the
# submitter wrote into their own document, so it says which build a result
# claims -- useful, and not a proof of anything. The proof, where there is one,
# is in the attest_* columns, which only a signature can set.
#
# The newest release that published those bytes, where several did. Most
# releases rebuild a target without changing it -- same source, same pinned
# toolchain, same flags -- so one digest is typically published by every release
# from the one that introduced it onwards, and naming the oldest of them would
# label a current binary with a version it has long outlived. Ordered by
# release_rank, because the tags themselves do not sort; see release_rank().
RELEASE_BUILD = ("(SELECT vb.release FROM verified_builds vb "
                 "WHERE vb.sha256 = r.binary_sha256 "
                 "ORDER BY vb.release_rank DESC, vb.added_at DESC "
                 "LIMIT 1) AS release_build")

RUN_COLUMNS = [
    "id", "created_at", "label", "notes", "mode", "binary_sha256",
    "attest_tier", "attest_repo", "attest_workflow", "attest_run_url",
    "attest_at", "compiler",
    "compiler_version", "cc", "build_flags", "target",
    "vectorize", "fma", "sysname", "os_release", "machine", "cpus",
    "cpu_models", "threads", "seconds", "reps", "warmup", "mem_bytes", "pin",
    "clock", "seed", "dram_name", "dram_mhz_min", "dram_mhz_max", "checksum",
]


class HttpError(Exception):
    """A refusal with the status it should be reported as."""

    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


class Invalid(HttpError):
    """An upload that will not be stored, with a message the client can show."""

    def __init__(self, message: str):
        super().__init__(HTTPStatus.BAD_REQUEST, message)


class Denied(HttpError):
    """Refused for who is asking rather than for what was sent."""

    def __init__(self, message: str, status: int = HTTPStatus.FORBIDDEN):
        super().__init__(status, message)


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
#
# Everything here runs on attacker-controlled input: the payload is whatever was
# POSTed, not necessarily something cpcpub produced.


def _reject_constant(name: str):
    raise Invalid(f"{name} is not a valid value")


def parse_json(raw: bytes) -> object:
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise Invalid("body must be UTF-8")
    try:
        # parse_constant rejects the NaN/Infinity literals Python would
        # otherwise accept and which no downstream consumer handles sanely.
        return json.loads(text, parse_constant=_reject_constant)
    except json.JSONDecodeError as e:
        raise Invalid(f"invalid JSON: {e.msg} at line {e.lineno} column {e.colno}")


def _text(v, maxlen=MAX_TEXT, field="value"):
    if v is None:
        return None
    if not isinstance(v, str):
        raise Invalid(f"{field} must be a string")
    s = "".join(c for c in v if c == "\n" or c >= " ")
    s = s.strip()[:maxlen].strip()
    return s or None


def _num(v, field, lo=0.0, hi=MAX_VALUE):
    if v is None:
        return None
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise Invalid(f"{field} must be a number or null")
    x = float(v)
    if not math.isfinite(x):
        raise Invalid(f"{field} must be finite")
    if not (lo <= x <= hi):
        raise Invalid(f"{field} is out of range")
    return x


def _int(v, field, lo=0, hi=1 << 40):
    if v is None:
        return None
    if isinstance(v, bool) or not isinstance(v, int):
        if isinstance(v, float) and v.is_integer():
            v = int(v)
        else:
            raise Invalid(f"{field} must be an integer or null")
    if not (lo <= v <= hi):
        raise Invalid(f"{field} is out of range")
    return v


def _bool(v, field):
    if v is None:
        return None
    if not isinstance(v, bool):
        raise Invalid(f"{field} must be true or false")
    return int(v)


def _hex64(v, field):
    """A SHA-256 as the benchmark writes it, or None.

    Strict about the shape because it is used as an identity: a value that is
    not a digest can only ever match another value that is not a digest, and
    doing that under the word "verified" is worse than not verifying at all.
    """
    if v is None:
        return None
    if not isinstance(v, str):
        raise Invalid(f"{field} must be a string")
    s = v.strip().lower()
    if not SHA256_RE.match(s):
        raise Invalid(f"{field} must be 64 hexadecimal characters")
    return s


def _obj(payload, key):
    v = payload.get(key)
    if v is None:
        return {}
    if not isinstance(v, dict):
        raise Invalid(f"{key} must be an object")
    return v


def validate(payload: object) -> tuple[dict, list[dict]]:
    """Turn an uploaded document into (run row, core rows) or raise Invalid."""
    if not isinstance(payload, dict):
        raise Invalid("body must be a JSON object")
    if payload.get("schema") != SCHEMA_ID:
        raise Invalid(f"unsupported schema {payload.get('schema')!r}; "
                      f"expected {SCHEMA_ID}")

    mode = payload.get("mode")
    if mode not in MODES:
        raise Invalid("mode must be 'full', 'per-core' or 'threads'; a "
                      "--disp-sweep dump carries no summary metrics to compare")

    build = _obj(payload, "build")
    system = _obj(payload, "system")
    config = _obj(payload, "config")
    dram = _obj(payload, "dram")

    run = {
        "mode": mode,
        "compiler": _text(build.get("compiler"), field="build.compiler"),
        "compiler_version": _text(build.get("compiler_version"), 200,
                                  "build.compiler_version"),
        "cc": _text(build.get("cc"), 64, "build.cc"),
        # The exact flags matter for comparability and are long; keep them whole.
        "build_flags": _text(build.get("flags"), 1000, "build.flags"),
        "target": _text(build.get("target"), 32, "build.target"),
        "binary_sha256": _hex64(build.get("binary_sha256"), "build.binary_sha256"),
        "vectorize": _bool(build.get("vectorize"), "build.vectorize"),
        "fma": _bool(build.get("fma"), "build.fma"),
        "sysname": _text(system.get("sysname"), 64, "system.sysname"),
        "os_release": _text(system.get("release"), 64, "system.release"),
        "machine": _text(system.get("machine"), 64, "system.machine"),
        "cpus": _int(system.get("cpus"), "system.cpus", 0, 4096),
        "cpu_models": _text(system.get("cpu_models"), 300, "system.cpu_models"),
        "threads": _int(config.get("threads"), "config.threads", 0, 4096),
        "seconds": _num(config.get("seconds_per_phase"), "config.seconds_per_phase", 0, 1e6),
        "reps": _int(config.get("reps"), "config.reps", 0, 1 << 20),
        "warmup": _num(config.get("warmup_seconds"), "config.warmup_seconds", 0, 1e6),
        "mem_bytes": _int(config.get("mem_bytes_per_thread"), "config.mem_bytes_per_thread"),
        "pin": _bool(config.get("pin"), "config.pin"),
        "clock": _text(config.get("clock"), 16, "config.clock"),
        "seed": _int(config.get("seed"), "config.seed", 0, (1 << 64) - 1),
        "dram_name": _text(dram.get("name"), 64, "dram.name"),
        "dram_mhz_min": _num(dram.get("mhz_min"), "dram.mhz_min", 0, 1e7),
        "dram_mhz_max": _num(dram.get("mhz_max"), "dram.mhz_max", 0, 1e7),
        "checksum": _text(payload.get("checksum"), 32, "checksum"),
    }

    records: list[tuple[str, object]] = []
    cores = payload.get("cores")
    threads = payload.get("threads")
    if mode in ("per-core", "full") and cores is not None:
        if not isinstance(cores, list) or not cores:
            raise Invalid("'cores' must be a non-empty array")
        records += [("cpu", c) for c in cores]
    if mode in ("threads", "full") and threads is not None:
        if not isinstance(threads, list) or not threads:
            raise Invalid("'threads' must be a non-empty array")
        records += [("thread", t) for t in threads]
        if payload.get("total") is not None:
            records.append(("total", payload["total"]))
    if not records:
        raise Invalid(f"a {mode} run must carry the matching result array "
                      "('cores' and/or 'threads')")

    if len(records) > MAX_CORES:
        raise Invalid(f"too many records (limit {MAX_CORES})")

    rows = []
    for default_scope, rec in records:
        if not isinstance(rec, dict):
            raise Invalid("each result record must be an object")
        scope = _text(rec.get("scope"), 16, "scope") or default_scope
        if scope not in SCOPES:
            raise Invalid(f"unknown scope {scope!r}")
        # Mixed on purpose: a record is a scope name and fourteen numbers, any
        # of which the benchmark may leave out.
        row: dict[str, object] = {"scope": scope}
        for jkey, col, kind in CORE_FIELDS:
            v = rec.get(jkey)
            if kind == "num":
                row[col] = _num(v, jkey)
            elif kind == "int":
                row[col] = _int(v, jkey, -1, 1 << 20)
            else:
                row[col] = _text(v, 32, jkey)
        rows.append(row)

    if not any(r.get("score") or r.get("int_thr") for r in rows):
        raise Invalid("no usable measurements in this upload")
    return run, rows


# ---------------------------------------------------------------------------
# Accounts
# ---------------------------------------------------------------------------
#
# Passwords are stored as scrypt digests, salted per account, and compared in
# constant time. Session tokens are stored as SHA-256 -- they are random 256-bit
# strings rather than guessable secrets, so a plain hash with no salt is the
# right shape: it stops a leaked database being a list of live sessions without
# pretending to slow down an attack nobody can mount.


def check_name(v) -> str:
    if not isinstance(v, str):
        raise Invalid("name must be a string")
    # Deliberately not _text(): that truncates to fit, and a name quietly cut
    # to thirty-two characters is not the name anyone asked to register.
    name = "".join(c for c in v if c >= " ").strip()
    if not NAME_RE.match(name):
        raise Invalid("a name is 3 to 32 characters of letters, digits, "
                      "'.', '_' or '-', starting with a letter or digit")
    return name


def check_password(v) -> str:
    if not isinstance(v, str):
        raise Invalid("password must be a string")
    if len(v) < MIN_PASSWORD:
        raise Invalid(f"password must be at least {MIN_PASSWORD} characters")
    if len(v) > MAX_PASSWORD:
        raise Invalid(f"password must be at most {MAX_PASSWORD} characters")
    return v


def hash_password(password: str) -> str:
    salt = secrets.token_bytes(16)
    dk = hashlib.scrypt(password.encode("utf-8"), salt=salt, n=SCRYPT_N,
                        r=SCRYPT_R, p=SCRYPT_P, maxmem=SCRYPT_MAXMEM, dklen=32)
    return "$".join(["scrypt", str(SCRYPT_N), str(SCRYPT_R), str(SCRYPT_P),
                     salt.hex(), dk.hex()])


def password_matches(password: str, stored: str) -> bool:
    """Whether `password` produces `stored`. Never raises on a malformed row."""
    try:
        algo, n, r, p, salt_hex, want_hex = stored.split("$")
        if algo != "scrypt":
            return False
        want = bytes.fromhex(want_hex)
        got = hashlib.scrypt(password.encode("utf-8"), salt=bytes.fromhex(salt_hex),
                             n=int(n), r=int(r), p=int(p),
                             maxmem=SCRYPT_MAXMEM, dklen=len(want))
    except (ValueError, TypeError):
        return False
    return secrets.compare_digest(got, want)


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def pow_solved(challenge: str, nonce: str, bits: int) -> bool:
    """Whether sha256("challenge:nonce") starts with `bits` zero bits.

    The client searches for a nonce; the server does one hash to check it.
    That asymmetry is the whole mechanism, and it needs no state beyond the
    challenge having been issued and not yet spent.
    """
    digest = hashlib.sha256(f"{challenge}:{nonce}".encode("utf-8")).digest()
    return int.from_bytes(digest, "big") < (1 << (256 - bits))


class Challenges:
    """Registration challenges: issued by us, spent once, forgotten quickly.

    Server-issued rather than derived from the request, so a challenge cannot
    be prepared offline in bulk; single-use, so one solved puzzle is one
    account; expiring, so the set stays small.
    """

    def __init__(self, ttl: float = CHALLENGE_TTL, cap: int = MAX_CHALLENGES):
        self.ttl = ttl
        self.cap = cap
        self.live: dict[str, float] = {}
        self.lock = threading.Lock()

    def issue(self) -> str:
        token = secrets.token_urlsafe(16)
        now = time.monotonic()
        with self.lock:
            if len(self.live) >= self.cap:
                for k in [k for k, exp in self.live.items() if exp <= now]:
                    del self.live[k]
                # Still full: the oldest go, which is the right thing to lose.
                while len(self.live) >= self.cap:
                    del self.live[min(self.live, key=self.live.__getitem__)]
            self.live[token] = now + self.ttl
        return token

    def spend(self, token: str) -> bool:
        """Take `token` if it is live. A second attempt with it fails."""
        with self.lock:
            expires = self.live.pop(token, None)
        return expires is not None and expires > time.monotonic()


def _url(v, field):
    """An http(s) URL, or None.

    The scheme is checked because this value ends up as an href in the page:
    `javascript:` in a link the hub itself renders is a script the CSP was
    supposed to have made impossible.
    """
    s = _text(v, 300, field)
    if s is None:
        return None
    if not s.startswith(("http://", "https://")):
        raise Invalid(f"{field} must be an http:// or https:// URL")
    return s


def release_rank(tag: str) -> str:
    """A sortable key for a release tag, so newest means newest.

    Tags are text, and text does not sort the way versions do: every string
    comparison there is puts "v0.10.0" before "v0.9.0", and it only takes one
    two-digit component for a board to start labelling binaries with a release
    they were superseded in. So each run of digits is padded to a fixed width
    and compared as a number, and what is left between them is compared as
    written.

    Two smaller rules, both of them what a reader of the tag would expect:

      * a leading "v" is dropped, so a repository that tagged "0.4.0" after
        "v0.3.0" is not read as having gone backwards;
      * a pre-release suffix ranks *below* the release it leads to -- v1.0.0-rc1
        precedes v1.0.0 -- which the padding alone would get backwards, a longer
        string being the greater one.

    The key is stored rather than computed per query: SQLite has no version
    collation to ORDER BY, and a Python-side sort would mean reading every row
    of the table to answer one row of the board.
    """
    core, _, pre = tag.strip().lower().partition("-")
    if core.startswith("v"):
        core = core[1:]

    def pad(s: str) -> str:
        out = []
        for part in re.findall(r"\d+|\D+", s):
            if part.isdigit():
                n = int(part)
                # A component too large to be a version number is not one; it
                # is clamped rather than allowed to widen the field and so
                # scramble the ordering of every tag around it.
                out.append("%012d" % n if n < 10 ** 12 else "9" * 12)
            else:
                out.append(part)
        return "".join(out)

    # "!" sorts below "~", which is the whole trick: v1.0.0 gets the "~" and so
    # outranks every v1.0.0-<anything>.
    return pad(core) + ("!" + pad(pre) if pre else "~")


def parse_builds_manifest(doc: object) -> tuple[str, str | None, list[dict]]:
    """Validate a release build manifest into (release tag, url, builds).

    The document the release workflow attaches next to the binaries:

        {"schema": "cpu-bench-builds/1", "release": "v0.1.0",
         "url": "https://github.com/…/releases/tag/v0.1.0",
         "builds": [{"filename": …, "sha256": …, "target": …, "march": …}, …]}

    Checked as strictly as an upload is. It arrives from a public URL over a
    network the hub does not control, and everything in it becomes the meaning
    of the word "verified" on the board.
    """
    if not isinstance(doc, dict):
        raise Invalid("a build manifest must be a JSON object")
    if doc.get("schema") != BUILDS_SCHEMA_ID:
        raise Invalid(f"unsupported manifest schema {doc.get('schema')!r}; "
                      f"expected {BUILDS_SCHEMA_ID}")
    release = _text(doc.get("release"), 64, "release")
    if not release:
        raise Invalid("a build manifest must name the release it describes")
    builds = doc.get("builds")
    if not isinstance(builds, list) or not builds:
        raise Invalid("'builds' must be a non-empty array")
    if len(builds) > 100:
        raise Invalid("too many builds in one manifest")

    seen: set[str] = set()
    out = []
    for b in builds:
        if not isinstance(b, dict):
            raise Invalid("each build must be an object")
        digest = _hex64(b.get("sha256"), "builds[].sha256")
        if digest is None:
            raise Invalid("each build must carry a sha256")
        # Two names for one digest is one binary published twice; harmless, but
        # it would make the count say something untrue.
        if digest in seen:
            continue
        seen.add(digest)
        out.append({
            "sha256": digest,
            "filename": _text(b.get("filename"), 128, "builds[].filename"),
            "target": _text(b.get("target"), 32, "builds[].target"),
            "march": _text(b.get("march"), 200, "builds[].march"),
        })
    return release, _url(doc.get("url"), "url"), out


def public_user(user: dict, *, runs: int | None = None,
                token: bool = False) -> dict:
    """An account as the API describes it.

    The password hash never leaves here, and the upload token only when the
    account itself is asking -- `/api/users/<name>` is a public page.
    """
    out = {"name": user["name"], "created_at": user["created_at"]}
    if runs is not None:
        out["runs"] = runs
    if token:
        out["api_token"] = user["api_token"]
    return out


# ---------------------------------------------------------------------------
# Storage
# ---------------------------------------------------------------------------


def iso_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


class Store:
    ADDED_COLUMNS = (("compiler_version", "TEXT"), ("cc", "TEXT"),
                     ("build_flags", "TEXT"), ("user_id", "INTEGER"),
                     ("binary_sha256", "TEXT"), ("attest_tier", "TEXT"),
                     ("attest_repo", "TEXT"), ("attest_workflow", "TEXT"),
                     ("attest_run_url", "TEXT"), ("attest_at", "TEXT"))

    def __init__(self, path: str):
        self.path = path
        with self.connect() as db:
            db.executescript(SCHEMA_PATH.read_text())
            # Bring a database made by an older build up to date. Names are
            # from the tuple above, never from a request.
            have = {r["name"] for r in db.execute("PRAGMA table_info(runs)")}
            for col, decl in self.ADDED_COLUMNS:
                if col not in have:
                    db.execute(f"ALTER TABLE runs ADD COLUMN {col} {decl}")
            # After the ALTER, not in schema.sql: an older database has neither
            # column when the script runs, and an index over a column that does
            # not exist yet is an error rather than a no-op.
            db.execute("CREATE INDEX IF NOT EXISTS runs_user ON runs(user_id, id)")
            db.execute("CREATE INDEX IF NOT EXISTS runs_binary "
                       "ON runs(binary_sha256)")
            self._migrate_verified_builds(db)

    @staticmethod
    def _migrate_verified_builds(db: sqlite3.Connection) -> None:
        """Re-key a verified_builds table written before releases could share.

        The old table was keyed on the digest alone, which cannot hold the same
        binary appearing in two releases: the second manifest to be loaded
        overwrote the first, so which release a run was labelled with came down
        to the order an operator happened to run web/verified.py in. The primary
        key is now (sha256, release), which SQLite cannot express as an ALTER --
        hence the copy.

        Nothing is lost and nothing is invented: whatever pairs the old table
        held are carried over as they stand. The releases it dropped along the
        way come back the next time their manifests are loaded, which is a
        re-run of web/verified.py and not a repair job.
        """
        cols = list(db.execute("PRAGMA table_info(verified_builds)"))
        if not cols:                        # a fresh database; schema.sql made it
            return
        key = {c["name"] for c in cols if c["pk"]}
        if key == {"sha256", "release"} and any(c["name"] == "release_rank"
                                                for c in cols):
            return

        db.execute("""
            CREATE TABLE verified_builds_new (
                sha256      TEXT    NOT NULL,
                release     TEXT    NOT NULL,
                release_rank TEXT   NOT NULL,
                release_url TEXT,
                filename    TEXT,
                target      TEXT,
                march       TEXT,
                added_at    TEXT    NOT NULL,
                PRIMARY KEY (sha256, release)
            )""")
        rows = [(r["sha256"], r["release"], release_rank(r["release"]),
                 r["release_url"], r["filename"], r["target"], r["march"],
                 r["added_at"])
                for r in db.execute("SELECT * FROM verified_builds")]
        db.executemany(
            "INSERT OR REPLACE INTO verified_builds_new "
            "(sha256, release, release_rank, release_url, filename, target, "
            " march, added_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)", rows)
        db.execute("DROP TABLE verified_builds")
        db.execute("ALTER TABLE verified_builds_new RENAME TO verified_builds")
        # The old table's indexes went with it.
        db.execute("CREATE INDEX IF NOT EXISTS verified_builds_sha "
                   "ON verified_builds(sha256)")

    def connect(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.path, timeout=10.0)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys = ON")
        db.execute("PRAGMA journal_mode = WAL")
        return db

    def insert(self, run: dict, cores: list[dict], raw: str,
               label: str | None, notes: str | None,
               user_id: int | None = None,
               attest: dict | None = None) -> tuple[int, str]:
        token = secrets.token_urlsafe(24)
        # Stamped rather than joined, unlike release_build: this was checked
        # against these exact bytes at this exact moment, and re-deriving it
        # later is not possible -- the token is short-lived by design.
        if attest:
            run = dict(run, attest_tier=attest["tier"],
                       attest_repo=attest["repository"],
                       attest_workflow=attest["workflow_ref"],
                       attest_run_url=attest["run_url"],
                       attest_at=iso_now())
        # The delete token is issued whether or not an account owns the run:
        # signing in later must not be the only way back to something uploaded
        # from a shell, and an account holder loses nothing by also having one.
        run = dict(run, created_at=iso_now(), delete_token=token,
                   label=label, notes=notes, raw=raw, user_id=user_id)
        cols = list(run)
        with self.connect() as db:
            cur = db.execute(
                f"INSERT INTO runs ({','.join(cols)}) "
                f"VALUES ({','.join('?' * len(cols))})",
                [run[c] for c in cols])
            # sqlite3 sets lastrowid on every successful INSERT into a rowid
            # table; None here would mean the row above did not happen.
            assert cur.lastrowid is not None
            run_id = int(cur.lastrowid)
            ccols = ["run_id", "scope"] + CORE_COLUMNS
            db.executemany(
                f"INSERT INTO cores ({','.join(ccols)}) "
                f"VALUES ({','.join('?' * len(ccols))})",
                [[run_id] + [c.get(col) for col in ccols[1:]] for c in cores])
        return run_id, token

    def delete(self, run_id: int, token: str | None = None,
               user_id: int | None = None) -> bool:
        """Withdraw a run, for whoever can show one of the two claims to it.

        The delete token proves whoever holds the upload's reply, and the
        account id proves the run was uploaded under it. Either is enough on
        its own; neither is checked against the other.
        """
        with self.connect() as db:
            row = db.execute("SELECT delete_token, user_id FROM runs WHERE id = ?",
                             (run_id,)).fetchone()
            if row is None:
                return False
            owns = user_id is not None and row["user_id"] == user_id
            # compare_digest even though a token mismatch is not timed against
            # anything an attacker controls the length of; it costs nothing.
            holds = token is not None and secrets.compare_digest(
                row["delete_token"], token)
            if not (owns or holds):
                return False
            db.execute("DELETE FROM cores WHERE run_id = ?", (run_id,))
            db.execute("DELETE FROM runs WHERE id = ?", (run_id,))
        return True

    def run(self, run_id: int) -> dict | None:
        with self.connect() as db:
            row = db.execute(
                f"SELECT {','.join(RUN_COLUMNS)}, {SUBMITTER}, {RELEASE_BUILD} "
                f"FROM runs r WHERE id = ?", (run_id,)).fetchone()
            if row is None:
                return None
            cores = db.execute(
                f"SELECT id, scope, {','.join(CORE_COLUMNS)} FROM cores "
                f"WHERE run_id = ? ORDER BY id", (run_id,)).fetchall()
        out = dict(row)
        out["cores"] = [dict(c) for c in cores]
        return out

    def raw(self, run_id: int) -> str | None:
        with self.connect() as db:
            row = db.execute("SELECT raw FROM runs WHERE id = ?", (run_id,)).fetchone()
        return row["raw"] if row else None

    def runs(self, limit: int = 50, offset: int = 0,
             user_id: int | None = None) -> list[dict]:
        where = "WHERE r.user_id = ? " if user_id is not None else ""
        args: list[object] = [user_id] if user_id is not None else []
        with self.connect() as db:
            rows = db.execute(
                f"SELECT r.{', r.'.join(RUN_COLUMNS)}, {SUBMITTER}, {RELEASE_BUILD}, "
                f"       (SELECT COUNT(*) FROM cores c WHERE c.run_id = r.id) AS records, "
                f"       (SELECT MAX(c.score) FROM cores c "
                f"          WHERE c.run_id = r.id AND c.scope != 'total') AS best_score "
                f"FROM runs r {where}ORDER BY r.id DESC LIMIT ? OFFSET ?",
                args + [limit, offset]).fetchall()
        return [dict(r) for r in rows]

    def cores(self, *, scope: str | None = "cpu", target: str | None = None,
              vectorize: int | None = None, fma: int | None = None,
              search: str | None = None, ids: list[int] | None = None,
              run: int | None = None, user_id: int | None = None,
              verified: str | None = None, group_run: bool = True,
              sort: str = "score", desc: bool = True, limit: int = 50) -> list[dict]:
        """Records matching the filters, best first.

        With `group_run` (the leaderboard's default) an upload contributes one
        row, not one per core: the run's best record at `sort` stands for it,
        and `records` says how many it was picked from. Without it every
        matching record is a row of its own -- what the expanded view of a run
        and a shared comparison link both address.
        """
        where = ["1 = 1"]
        args: list[object] = []
        if scope is not None:
            where.append("c.scope = ?")
            args.append(scope)
        if run is not None:
            where.append("c.run_id = ?")
            args.append(run)
        if user_id is not None:
            where.append("r.user_id = ?")
            args.append(user_id)
        # Three different questions, kept apart because conflating them is the
        # whole complaint against a self-reported badge.
        if verified == "ci":
            where.append("r.attest_tier = 'ci'")
        elif verified == "attested":
            where.append("r.attest_tier IN ('ci', 'attested')")
        elif verified == "release":
            where.append("EXISTS (SELECT 1 FROM verified_builds vb "
                         "WHERE vb.sha256 = r.binary_sha256)")
        if target:
            where.append("r.target = ?")
            args.append(target)
        if vectorize is not None:
            where.append("r.vectorize = ?")
            args.append(vectorize)
        if fma is not None:
            where.append("r.fma = ?")
            args.append(fma)
        if search:
            where.append("(r.cpu_models LIKE ? OR r.label LIKE ? OR r.machine LIKE ?)")
            pat = f"%{search}%"
            args += [pat, pat, pat]
        if ids:
            where.append(f"c.id IN ({','.join('?' * len(ids))})")
            args += ids
        # `sort` is whitelisted by the caller; never interpolate raw input here.
        assert sort in METRIC_KEYS or sort == "mhz"
        direction = "DESC" if desc else "ASC"
        # Nulls last whichever way the metric runs, then the metric, then the
        # id so the order is total. Used twice: once to pick each run's
        # representative, once to order the board itself.
        order = f"(c.{sort} IS NULL), c.{sort} {direction}, c.id ASC"
        sql = (
            f"WITH matched AS ("
            f"  SELECT c.id AS core_id, "
            f"         COUNT(*)     OVER (PARTITION BY c.run_id) AS peers, "
            f"         ROW_NUMBER() OVER (PARTITION BY c.run_id "
            f"                            ORDER BY {order}) AS pick "
            f"  FROM cores c JOIN runs r ON r.id = c.run_id "
            f"  WHERE {' AND '.join(where)}) "
            f"SELECT c.id, c.run_id, c.scope, c.cpu, c.mhz, c.mhz_src, "
            f"       c.{', c.'.join(CORE_COLUMNS[3:])}, "
            f"       r.target, r.vectorize, r.fma, r.cpu_models, r.label, "
            f"       r.created_at, r.mode, r.threads, r.compiler, r.machine, "
            f"       {SUBMITTER}, {RELEASE_BUILD}, r.attest_tier, "
            f"       m.peers AS records "
            f"FROM matched m "
            f"JOIN cores c ON c.id = m.core_id "
            f"JOIN runs r ON r.id = c.run_id "
            + ("WHERE m.pick = 1 " if group_run else "")
            + f"ORDER BY {order} "
            f"LIMIT ?")
        args.append(limit)
        with self.connect() as db:
            return [dict(r) for r in db.execute(sql, args).fetchall()]

    def rank(self, run_id: int) -> dict:
        """Percentile of this run's best value for every metric.

        Compared only against records of the same scope built with the same
        fairness flags -- a vectorised build is not comparable with a scalar one.
        """
        with self.connect() as db:
            run = db.execute(
                "SELECT vectorize, fma, mode FROM runs WHERE id = ?",
                (run_id,)).fetchone()
            if run is None:
                return {}
            scope = "thread" if run["mode"] == "threads" else "cpu"
            out = {"scope": scope, "metrics": {}}
            for m in METRICS:
                key = m["key"]
                if m["better"] == "none":
                    continue
                agg = "MIN" if m["better"] == "low" else "MAX"
                mine = db.execute(
                    f"SELECT {agg}({key}) AS v FROM cores "
                    f"WHERE run_id = ? AND scope = ?", (run_id, scope)).fetchone()["v"]
                if mine is None:
                    continue
                cmp_op = "<" if m["better"] == "low" else ">"
                row = db.execute(
                    f"SELECT COUNT(*) AS total, "
                    f"       SUM(CASE WHEN c.{key} {cmp_op} ? THEN 1 ELSE 0 END) AS worse "
                    f"FROM cores c JOIN runs r ON r.id = c.run_id "
                    f"WHERE c.scope = ? AND c.{key} IS NOT NULL "
                    f"  AND r.vectorize IS ? AND r.fma IS ?",
                    (mine, scope, run["vectorize"], run["fma"])).fetchone()
                total = row["total"] or 0
                worse = row["worse"] or 0
                out["metrics"][key] = {
                    "value": mine,
                    "population": total,
                    # Share of the population this run beats.
                    "percentile": (100.0 * (total - worse) / total) if total else None,
                }
        return out

    def stats(self) -> dict:
        with self.connect() as db:
            runs = db.execute("SELECT COUNT(*) AS n FROM runs").fetchone()["n"]
            cores = db.execute("SELECT COUNT(*) AS n FROM cores").fetchone()["n"]
            users = db.execute("SELECT COUNT(*) AS n FROM users").fetchone()["n"]
            release_builds = db.execute(
                "SELECT COUNT(*) AS n FROM runs r WHERE EXISTS "
                "(SELECT 1 FROM verified_builds vb "
                " WHERE vb.sha256 = r.binary_sha256)").fetchone()["n"]
            attested = {r["tier"]: r["n"] for r in db.execute(
                "SELECT attest_tier AS tier, COUNT(*) AS n FROM runs "
                "WHERE attest_tier IS NOT NULL GROUP BY tier")}
            targets = db.execute(
                "SELECT target, COUNT(*) AS n FROM runs WHERE target IS NOT NULL "
                "GROUP BY target ORDER BY n DESC").fetchall()
            # The newest release this hub knows of, for the page to point at
            # when it explains what a release build is. By version and not by
            # when the manifest happened to be loaded: an operator catching up
            # on old releases must not leave the page recommending one of them.
            latest = db.execute(
                "SELECT release, release_url FROM verified_builds "
                "ORDER BY release_rank DESC, added_at DESC LIMIT 1").fetchone()
        release = None
        if latest and latest["release"]:
            release = {"release": latest["release"], "url": latest["release_url"]}
        return {"runs": runs, "records": cores, "users": users,
                # Three separate numbers on purpose: how many results carry a
                # signature nobody could have written (ci), how many went
                # through the workflow on the submitter's own machine
                # (attested), and how many merely claim a release binary.
                "attested": {"ci": attested.get("ci", 0),
                             "attested": attested.get("attested", 0)},
                "release_builds": release_builds, "release": release,
                "targets": [dict(t) for t in targets]}

    # -- verified builds ---------------------------------------------------

    def add_verified_builds(self, manifest: object) -> tuple[int, str]:
        """Load a release manifest. Returns (builds stored, release tag).

        Re-loading a release replaces what it published rather than adding to
        it, so a corrected manifest is a re-run of the same command and not a
        database to clean up by hand. Other releases are untouched: a hub keeps
        verifying the runs of every version it has been told about.
        """
        release, url, builds = parse_builds_manifest(manifest)
        now = iso_now()
        rank = release_rank(release)
        rows = [(b["sha256"], release, rank, url, b["filename"],
                 b["target"], b["march"], now) for b in builds]
        with self.connect() as db:
            db.execute("DELETE FROM verified_builds WHERE release = ?", (release,))
            db.executemany(
                "INSERT OR REPLACE INTO verified_builds "
                "(sha256, release, release_rank, release_url, filename, target, "
                " march, added_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)", rows)
        return len(rows), release

    def verified_builds(self) -> list[dict]:
        """Every (binary, release) pair this hub holds, newest release first.

        A digest published unchanged by several releases appears once per
        release: that is the table's shape, and flattening it here would hide
        exactly the fact a reader of this list is checking -- which versions a
        given binary is current in.
        """
        with self.connect() as db:
            return [dict(r) for r in db.execute(
                "SELECT * FROM verified_builds "
                "ORDER BY release_rank DESC, added_at DESC, filename")]

    def forget_release(self, release: str) -> int:
        with self.connect() as db:
            cur = db.execute("DELETE FROM verified_builds WHERE release = ?",
                             (release,))
        return cur.rowcount

    # -- accounts ----------------------------------------------------------

    def create_user(self, name: str, password: str) -> dict:
        row = {"name": name, "password_hash": hash_password(password),
               "api_token": secrets.token_urlsafe(24), "created_at": iso_now()}
        try:
            with self.connect() as db:
                cur = db.execute(
                    "INSERT INTO users (name, password_hash, api_token, created_at) "
                    "VALUES (:name, :password_hash, :api_token, :created_at)", row)
        except sqlite3.IntegrityError:
            # The unique index over lower(name) is the only constraint an
            # ordinary registration can trip.
            raise Denied("that name is taken", HTTPStatus.CONFLICT)
        assert cur.lastrowid is not None
        return dict(row, id=int(cur.lastrowid))

    def user_by_name(self, name: str) -> dict | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM users WHERE lower(name) = lower(?)",
                             (name,)).fetchone()
        return dict(row) if row else None

    def user_by_id(self, user_id: int) -> dict | None:
        with self.connect() as db:
            row = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
        return dict(row) if row else None

    def user_by_api_token(self, token: str) -> dict | None:
        """The account an `Authorization: Bearer` upload belongs to.

        Looked up by equality: the token is a 192-bit random string, so there
        is no prefix of it worth learning from how long the lookup took.
        """
        if not token:
            return None
        with self.connect() as db:
            row = db.execute("SELECT * FROM users WHERE api_token = ?",
                             (token,)).fetchone()
        return dict(row) if row else None

    def set_password(self, user_id: int, password: str) -> None:
        with self.connect() as db:
            db.execute("UPDATE users SET password_hash = ? WHERE id = ?",
                       (hash_password(password), user_id))

    def rotate_api_token(self, user_id: int) -> str:
        token = secrets.token_urlsafe(24)
        with self.connect() as db:
            db.execute("UPDATE users SET api_token = ? WHERE id = ?",
                       (token, user_id))
        return token

    def close_account(self, user_id: int) -> int:
        """Delete an account and everything uploaded under it.

        The runs go too, rather than being orphaned: an anonymous run nobody
        holds the delete token for is a row that can never be withdrawn, and
        leaving those behind is not what closing an account is understood to
        mean. Returns how many runs went with it.
        """
        with self.connect() as db:
            ids = [r["id"] for r in db.execute(
                "SELECT id FROM runs WHERE user_id = ?", (user_id,))]
            if ids:
                marks = ",".join("?" * len(ids))
                db.execute(f"DELETE FROM cores WHERE run_id IN ({marks})", ids)
                db.execute(f"DELETE FROM runs WHERE id IN ({marks})", ids)
            db.execute("DELETE FROM sessions WHERE user_id = ?", (user_id,))
            db.execute("DELETE FROM users WHERE id = ?", (user_id,))
        return len(ids)

    def run_count(self, user_id: int) -> int:
        with self.connect() as db:
            return db.execute("SELECT COUNT(*) AS n FROM runs WHERE user_id = ?",
                              (user_id,)).fetchone()["n"]

    # -- sessions ----------------------------------------------------------

    def create_session(self, user_id: int) -> tuple[str, int]:
        """A new session for one browser: (cookie value, lifetime in seconds)."""
        token = secrets.token_urlsafe(32)
        expires = datetime.now(timezone.utc) + timedelta(days=SESSION_DAYS)
        with self.connect() as db:
            # Cheap enough to do on every sign-in, and it means an abandoned
            # browser's row does not outlive the session it stands for.
            db.execute("DELETE FROM sessions WHERE expires_at < ?", (iso_now(),))
            db.execute("INSERT INTO sessions (token_hash, user_id, created_at, "
                       "expires_at) VALUES (?, ?, ?, ?)",
                       (token_hash(token), user_id, iso_now(),
                        expires.isoformat(timespec="seconds").replace("+00:00", "Z")))
        return token, SESSION_DAYS * 86400

    def user_by_session(self, token: str) -> dict | None:
        if not token:
            return None
        with self.connect() as db:
            row = db.execute(
                "SELECT u.* FROM sessions s JOIN users u ON u.id = s.user_id "
                "WHERE s.token_hash = ? AND s.expires_at > ?",
                (token_hash(token), iso_now())).fetchone()
        return dict(row) if row else None

    def end_session(self, token: str) -> None:
        with self.connect() as db:
            db.execute("DELETE FROM sessions WHERE token_hash = ?",
                       (token_hash(token),))

    def end_all_sessions(self, user_id: int) -> None:
        """Sign every browser out -- what a password change has to mean."""
        with self.connect() as db:
            db.execute("DELETE FROM sessions WHERE user_id = ?", (user_id,))


# ---------------------------------------------------------------------------
# Rate limiting
# ---------------------------------------------------------------------------


class RateLimiter:
    def __init__(self, limit: int, window: float):
        self.limit = limit
        self.window = window
        self.hits: dict[str, deque[float]] = defaultdict(deque)
        self.lock = threading.Lock()

    def allow(self, key: str, limit: int | None = None) -> bool:
        """Whether `key` may act now. `limit` overrides the default for one
        class of caller -- a signed-in account is a known submitter and gets
        more room than a bare address."""
        limit = self.limit if limit is None else limit
        if self.limit <= 0:                 # 0 disables the limiter outright
            return True
        now = time.monotonic()
        with self.lock:
            q = self.hits[key]
            while q and now - q[0] > self.window:
                q.popleft()
            if len(q) >= limit:
                return False
            q.append(now)
            if len(self.hits) > 10000:          # bound memory on a busy host
                for k in [k for k, v in self.hits.items() if not v][:5000]:
                    del self.hits[k]
            return True


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

SECURITY_HEADERS = {
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
    "Content-Security-Policy": (
        "default-src 'self'; base-uri 'none'; form-action 'none'; "
        "frame-ancestors 'none'; object-src 'none'"),
}

ID_RE = re.compile(r"^/api/runs/(\d+)(/raw|/rank)?$")
USER_RE = re.compile(r"^/api/users/([A-Za-z0-9._-]{3,32})$")


class Handler(BaseHTTPRequestHandler):
    server_version = "cpcpub-hub/1.0"
    protocol_version = "HTTP/1.1"       # every response below sets Content-Length

    # -- plumbing ----------------------------------------------------------
    @property
    def hub(self) -> Server:
        """`self.server`, which is always the Server below.

        `BaseRequestHandler` declares that attribute as the base class, and it
        is not ours to redeclare, so the narrowing happens here once instead of
        at each of the half-dozen places that want what Server carries.
        """
        return cast("Server", self.server)

    @property
    def store(self) -> Store:
        return self.hub.store

    @property
    def limiter(self) -> RateLimiter:
        return self.hub.limiter

    @property
    def auth_limiter(self) -> RateLimiter:
        return self.hub.auth_limiter

    @property
    def register_limiter(self) -> RateLimiter:
        return self.hub.register_limiter

    def client_key(self) -> str:
        if self.hub.trust_proxy:
            fwd = self.headers.get("X-Forwarded-For", "")
            if fwd:
                return fwd.split(",")[0].strip()[:64]
        return self.client_address[0]

    def send_bytes(self, status: int, body: bytes, ctype: str, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in SECURITY_HEADERS.items():
            self.send_header(k, v)
        # A list rather than the dict `extra` is, because a response may carry
        # two Set-Cookie headers and a dict holds one.
        for k, v in getattr(self, "extra_headers", []):
            self.send_header(k, v)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_json(self, status: int, obj: object, extra=None):
        body = json.dumps(obj, allow_nan=False).encode()
        self.send_bytes(status, body, "application/json; charset=utf-8", extra)

    def fail(self, status: int, message: str):
        self.send_json(status, {"error": message})

    # `format` shadows the builtin, and is the name BaseHTTPRequestHandler gives
    # this parameter -- an override that renames it is one a caller passing it
    # by keyword would not survive.
    def log_message(self, format, *args):   # one tidy line per request
        sys.stderr.write("%s - %s\n" % (self.client_key(), format % args))

    # -- who is asking -----------------------------------------------------
    #
    # Two ways in, deliberately different. A browser holds a session cookie the
    # page cannot read; a machine that ran the benchmark holds an upload token
    # it sends in a header. The difference matters for CSRF: a cookie rides
    # along on a cross-site request without the other site having to know it
    # exists, so cookie-authenticated writes carry a token the page had to read
    # to send. A header the other site would have to set itself needs no such
    # proof, and neither do anonymous requests, which claim nothing.

    def cookies(self) -> dict[str, str]:
        jar = SimpleCookie()
        try:
            jar.load(self.headers.get("Cookie", ""))
        except CookieError:
            return {}
        return {k: m.value for k, m in jar.items()}

    def identify(self) -> tuple[dict | None, str | None]:
        """(account, how) for this request; (None, None) when anonymous.

        An upload token that names no account is an error rather than an
        anonymous request: a stale token in a submit script would otherwise
        keep uploading successfully, off the account it was meant for, with
        nothing to notice but the runs never appearing under the name. An
        expired session cookie is the opposite -- a browser is entitled to
        carry a dead one, and reading the board with it is not a mistake.
        """
        if not hasattr(self, "_who"):
            self._who: tuple[dict | None, str | None] = (None, None)
            auth = self.headers.get("Authorization", "")
            if auth.startswith("Bearer "):
                user = self.store.user_by_api_token(auth[7:].strip()[:128])
                if user is None:
                    raise Denied("unknown upload token; the account page has "
                                 "the current one", HTTPStatus.UNAUTHORIZED)
                self._who = (user, "token")
            elif SESSION_COOKIE in self.cookies():
                user = self.store.user_by_session(self.cookies()[SESSION_COOKIE])
                if user:
                    self._who = (user, "cookie")
        return self._who

    def require_account(self) -> dict:
        user, how = self.identify()
        if user is None:
            raise Denied("sign in first", HTTPStatus.UNAUTHORIZED)
        if how == "cookie":
            self.check_csrf()
        return user

    def check_csrf(self) -> None:
        sent = self.headers.get(CSRF_HEADER, "")
        held = self.cookies().get(CSRF_COOKIE, "")
        if not sent or not held or not secrets.compare_digest(sent, held):
            raise Denied("stale page: reload and try again")

    def is_secure(self) -> bool:
        """Whether the browser reached us over TLS, so `Secure` can be set.

        Only believed behind a proxy we were told to trust: an unproxied server
        that took X-Forwarded-Proto from the request would let any client
        decide the cookie flags.
        """
        if self.hub.trust_proxy:
            return self.headers.get("X-Forwarded-Proto", "").lower() == "https"
        return False

    def set_cookie(self, name: str, value: str, max_age: int, *,
                   http_only: bool = True) -> None:
        parts = [f"{name}={value}", "Path=/", f"Max-Age={max_age}", "SameSite=Lax"]
        if http_only:
            parts.append("HttpOnly")
        if self.is_secure():
            parts.append("Secure")
        self.extra_headers.append(("Set-Cookie", "; ".join(parts)))

    def start_session(self, user: dict) -> str:
        """Sign this browser in and hand the page its CSRF token."""
        token, ttl = self.store.create_session(user["id"])
        csrf = secrets.token_urlsafe(24)
        self.set_cookie(SESSION_COOKIE, token, ttl)
        self.set_cookie(CSRF_COOKIE, csrf, ttl, http_only=False)
        return csrf

    def end_session(self) -> None:
        token = self.cookies().get(SESSION_COOKIE)
        if token:
            self.store.end_session(token)
        self.set_cookie(SESSION_COOKIE, "", 0)
        self.set_cookie(CSRF_COOKIE, "", 0, http_only=False)

    def read_json(self, cap: int) -> dict:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            raise Invalid("bad Content-Length")
        if length <= 0:
            raise Invalid("empty body")
        if length > cap:
            self.drain(length)
            self.body_read = True
            raise HttpError(HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                            f"body larger than {cap} bytes")
        self.body_read = True
        payload = parse_json(self.rfile.read(length))
        if not isinstance(payload, dict):
            raise Invalid("body must be a JSON object")
        return payload

    # -- routing -----------------------------------------------------------
    def do_GET(self):
        self.route("GET")

    def do_HEAD(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_DELETE(self):
        self.route("DELETE")

    def route(self, method: str):
        # Per request: this handler instance serves every request on a
        # keep-alive connection, so the response headers cannot accumulate.
        self.extra_headers: list[tuple[str, str]] = []
        self.body_read = False
        self.__dict__.pop("_who", None)
        try:
            url = urllib.parse.urlsplit(self.path)
            path = urllib.parse.unquote(url.path)
            query = urllib.parse.parse_qs(url.query)

            if path.startswith("/api/"):
                self.api(method, path, query)
            elif method == "GET":
                self.static(path)
            else:
                self.fail(HTTPStatus.METHOD_NOT_ALLOWED, "method not allowed")
        except HttpError as e:
            # A refusal decided before the body was read -- a failed CSRF check,
            # say -- would otherwise leave those bytes in front of the next
            # request on a keep-alive connection, which then reads a JSON
            # document where a request line should be.
            self.drain_unread()
            self.fail(e.status, str(e))
        except BrokenPipeError:
            pass
        except Exception as e:                      # never leak a traceback
            self.log_message("unhandled error: %r", e)
            try:
                self.fail(HTTPStatus.INTERNAL_SERVER_ERROR, "internal error")
            except Exception:
                pass

    def api(self, method: str, path: str, query: dict):
        if path.startswith("/api/auth/"):
            return self.auth(method, path[len("/api/auth/"):])
        if path == "/api/metrics" and method == "GET":
            return self.send_json(HTTPStatus.OK, {"metrics": METRICS,
                                                  "scopes": sorted(SCOPES)})
        if path == "/api/stats" and method == "GET":
            return self.send_json(HTTPStatus.OK, self.store.stats())
        if path == "/api/builds" and method == "GET":
            # Public on purpose: what a hub calls verified is a claim about
            # which binaries it trusts, and a claim nobody can read is not one
            # anybody should believe. Every digest here is also in the release
            # the manifest names, so the two can be checked against each other.
            return self.send_json(HTTPStatus.OK,
                                  {"builds": self.store.verified_builds()})
        if path == "/api/cores" and method == "GET":
            return self.send_json(HTTPStatus.OK, {"cores": self.query_cores(query)})
        if path == "/api/runs":
            if method == "GET":
                limit = clamp_int(query.get("limit", ["50"])[0], 1, 200, 50)
                offset = clamp_int(query.get("offset", ["0"])[0], 0, 1 << 30, 0)
                return self.send_json(HTTPStatus.OK, {
                    "runs": self.store.runs(limit, offset,
                                            self.who_filter(query))})
            if method == "POST":
                return self.upload()
            return self.fail(HTTPStatus.METHOD_NOT_ALLOWED, "method not allowed")

        m = USER_RE.match(path)
        if m and method == "GET":
            user = self.store.user_by_name(m.group(1))
            if user is None:
                return self.fail(HTTPStatus.NOT_FOUND, "no such account")
            return self.send_json(HTTPStatus.OK, {"user": public_user(
                user, runs=self.store.run_count(user["id"]))})

        m = ID_RE.match(path)
        if m:
            run_id, suffix = int(m.group(1)), m.group(2)
            if method == "GET" and suffix == "/raw":
                raw = self.store.raw(run_id)
                if raw is None:
                    return self.fail(HTTPStatus.NOT_FOUND, "no such run")
                return self.send_bytes(HTTPStatus.OK, raw.encode(),
                                       "application/json; charset=utf-8")
            if method == "GET" and suffix == "/rank":
                return self.send_json(HTTPStatus.OK, self.store.rank(run_id))
            if method == "GET" and suffix is None:
                run = self.store.run(run_id)
                if run is None:
                    return self.fail(HTTPStatus.NOT_FOUND, "no such run")
                return self.send_json(HTTPStatus.OK, run)
            if method == "DELETE" and suffix is None:
                return self.withdraw(run_id, query)
        return self.fail(HTTPStatus.NOT_FOUND, "no such endpoint")

    def withdraw(self, run_id: int, query: dict):
        """Delete a run for its owner, or for whoever holds its delete token."""
        user, how = self.identify()
        if user is not None and how == "cookie":
            self.check_csrf()
        token = (self.headers.get("X-Delete-Token")
                 or query.get("token", [""])[0])
        if not token and user is None:
            return self.fail(HTTPStatus.UNAUTHORIZED,
                             "sign in, or send the run's delete token")
        if not self.store.delete(run_id, token or None,
                                 user["id"] if user else None):
            return self.fail(HTTPStatus.FORBIDDEN,
                             "no such run, or it is not yours to withdraw")
        return self.send_json(HTTPStatus.OK, {"deleted": run_id})

    def who_filter(self, query: dict) -> int | None:
        """The account a listing is narrowed to, from `user=name` or `user=me`.

        An unknown name filters to nothing rather than to everything: a board
        that quietly widened when a name was misspelt would read as that
        person having uploaded the lot.
        """
        name = _text(query.get("user", [None])[0], 32, "user")
        if not name:
            return None
        if name == "me":
            user, _ = self.identify()
            if user is None:
                raise Denied("sign in first", HTTPStatus.UNAUTHORIZED)
            return int(user["id"])
        found = self.store.user_by_name(name)
        return int(found["id"]) if found else -1

    # -- accounts ----------------------------------------------------------

    def register_policy(self) -> dict:
        """What this hub asks of a new account. Costs nothing to answer."""
        return {"open": self.hub.register_open,
                "invite_required": bool(self.hub.signup_token),
                "bits": self.hub.register_pow}

    def auth(self, method: str, action: str):
        if action == "policy" and method == "GET":
            # Deliberately separate from the puzzle below, and deliberately
            # unlimited. The page reads this whenever it draws the sign-in form,
            # which is once per visitor who never registers at all: issuing a
            # challenge there would spend a store slot and a rate-limit slot per
            # page view, and a shared address could then exhaust the very limit
            # that registering needs. It is three booleans about the hub -- the
            # same class of answer as /api/stats.
            return self.send_json(HTTPStatus.OK, self.register_policy())

        if action == "challenge" and method == "GET":
            # A puzzle to register with, plus the policy again so a client that
            # wants both still needs only one request.
            #
            # Limited on its own key, and loosely: what the limit is for is a
            # script filling the challenge store, and for that a generous
            # ceiling is still a ceiling.
            if not self.auth_limiter.allow("chal:" + self.client_key(),
                                           self.auth_limiter.limit * 5):
                raise Denied("too many requests from this address; try later",
                             HTTPStatus.TOO_MANY_REQUESTS)
            out = self.register_policy()
            if self.hub.register_open and self.hub.register_pow > 0:
                out["challenge"] = self.hub.challenges.issue()
            return self.send_json(HTTPStatus.OK, out)

        if action == "me" and method == "GET":
            user, how = self.identify()
            if user is None:
                return self.send_json(HTTPStatus.OK, {"user": None})
            # A signed-in page that has lost its CSRF cookie (it expires with
            # the session, but a browser may drop it alone) can do nothing but
            # read. Reissuing here costs one header and keeps a returning tab
            # working without a sign-in it does not need.
            if how == "cookie" and CSRF_COOKIE not in self.cookies():
                csrf = secrets.token_urlsafe(24)
                self.set_cookie(CSRF_COOKIE, csrf, SESSION_DAYS * 86400,
                                http_only=False)
            return self.send_json(HTTPStatus.OK, {
                "user": public_user(user, runs=self.store.run_count(user["id"]),
                                    token=(how == "cookie"))})

        if method != "POST":
            return self.fail(HTTPStatus.METHOD_NOT_ALLOWED, "method not allowed")

        if action == "register":
            return self.register()
        if action == "login":
            return self.login()
        if action == "logout":
            # Deliberately not CSRF-guarded and deliberately always 200: being
            # signed out against your will is a nuisance, not a compromise, and
            # a logout that can fail leaves a page that cannot get rid of a
            # session it no longer wants.
            self.end_session()
            return self.send_json(HTTPStatus.OK, {"user": None})
        if action == "token":
            user = self.require_account()
            return self.send_json(HTTPStatus.OK,
                                  {"api_token": self.store.rotate_api_token(user["id"])})
        if action == "password":
            return self.change_password()
        if action == "close":
            return self.close_account()
        return self.fail(HTTPStatus.NOT_FOUND, "no such endpoint")

    def auth_allowed(self) -> None:
        """Rate limit on guessing, per address rather than per name.

        Per name would let anyone lock an account out by guessing at it.
        """
        if not self.auth_limiter.allow("auth:" + self.client_key()):
            raise Denied("too many attempts from this address; try later",
                         HTTPStatus.TOO_MANY_REQUESTS)

    def register(self):
        """Create an account, for a client that can show it is not a script.

        Three gates, cheapest first: is this hub taking accounts at all, is the
        name and password even usable, and has the caller done the work. The
        per-address limit is checked last, after the work has been shown, so a
        flood of unsolved attempts cannot use up an honest neighbour's quota.
        """
        if not self.hub.register_open:
            raise Denied("this hub is not taking new accounts; uploads still "
                         "work without one")
        self.auth_allowed()
        body = self.read_json(MAX_AUTH_BODY)
        name = check_name(body.get("name"))
        password = check_password(body.get("password"))

        if self.hub.signup_token:
            given = body.get("invite")
            if not isinstance(given, str) or not secrets.compare_digest(
                    given, self.hub.signup_token):
                raise Denied("this hub needs an invite code to register")

        # Before the work is checked rather than after it. A name already taken
        # is someone who forgot they had an account, or a name two people both
        # wanted -- neither is an attempt at abuse, and neither should cost a
        # solved puzzle and another wait. create_user's unique index is still
        # what decides it: two registrations racing for one name is exactly what
        # that index is there for, and this check cannot see the other one.
        if self.store.user_by_name(name) is not None:
            raise Denied("that name is taken", HTTPStatus.CONFLICT)

        bits = self.hub.register_pow
        if bits > 0:
            challenge = body.get("challenge")
            nonce = body.get("nonce")
            if not isinstance(challenge, str) or not isinstance(nonce, str) \
                    or len(nonce) > MAX_NONCE:
                raise Invalid("registering needs a solved challenge from "
                              "/api/auth/challenge")
            # Spent whether or not it turns out to be solved: a challenge that
            # survived a wrong answer would be a free retry, which is exactly
            # what the work is supposed to cost.
            if not self.hub.challenges.spend(challenge):
                raise Denied("that challenge is unknown or has expired; ask "
                             "for another")
            if not pow_solved(challenge, nonce, bits):
                raise Denied("the challenge was not solved")

        if not self.register_limiter.allow("reg:" + self.client_key()):
            raise Denied("too many accounts from this address; try later",
                         HTTPStatus.TOO_MANY_REQUESTS)

        user = self.store.create_user(name, password)
        csrf = self.start_session(user)
        self.send_json(HTTPStatus.CREATED,
                       {"user": public_user(user, runs=0, token=True),
                        "csrf": csrf})

    def login(self):
        self.auth_allowed()
        body = self.read_json(MAX_AUTH_BODY)
        name = _text(body.get("name"), 32, "name") or ""
        password = body.get("password")
        user = self.store.user_by_name(name) if name else None
        if user is None or not isinstance(password, str) \
                or not password_matches(password, user["password_hash"]):
            # One message for both halves: which of the two was wrong is not
            # something a sign-in page should be willing to say.
            raise Denied("wrong name or password", HTTPStatus.UNAUTHORIZED)
        csrf = self.start_session(user)
        self.send_json(HTTPStatus.OK, {
            "user": public_user(user, runs=self.store.run_count(user["id"]),
                                token=True),
            "csrf": csrf})

    def change_password(self):
        self.auth_allowed()
        user = self.require_account()
        body = self.read_json(MAX_AUTH_BODY)
        if not password_matches(str(body.get("current", "")), user["password_hash"]):
            raise Denied("the current password is wrong", HTTPStatus.UNAUTHORIZED)
        new = check_password(body.get("password"))
        self.store.set_password(user["id"], new)
        # Every other browser goes with it -- a password change is what you do
        # when you think someone else has one, so leaving them signed in would
        # undo the point of it. This one is signed straight back in.
        self.store.end_all_sessions(user["id"])
        csrf = self.start_session(user)
        self.send_json(HTTPStatus.OK, {"ok": True, "csrf": csrf})

    def close_account(self):
        self.auth_allowed()
        user = self.require_account()
        body = self.read_json(MAX_AUTH_BODY)
        if not password_matches(str(body.get("password", "")), user["password_hash"]):
            raise Denied("wrong password", HTTPStatus.UNAUTHORIZED)
        removed = self.store.close_account(user["id"])
        self.end_session()
        self.send_json(HTTPStatus.OK, {"closed": user["name"], "runs_deleted": removed})

    def query_cores(self, query: dict) -> list[dict]:
        def one(name, default=None):
            v = query.get(name, [default])[0]
            return v if v not in ("", None) else default

        # An explicit id list, or one run's records, addresses records directly
        # -- a shared comparison link, or the expanded view of one board row --
        # so neither is narrowed by the leaderboard's scope default, and neither
        # is collapsed to one row per run.
        raw_ids = one("ids")
        addressed = bool(raw_ids or one("run"))
        scope = one("scope", None if addressed else "cpu")
        if scope is not None and scope not in SCOPES:
            raise Invalid(f"unknown scope {scope!r}")
        group = one("group", "none" if addressed else "run")
        if group not in ("run", "none"):
            raise Invalid("group must be 'run' or 'none'")
        run = one("run")
        if run is not None:
            try:
                run = int(run)
            except ValueError:
                raise Invalid("run must be an integer")
        sort = one("sort", "score")
        if sort not in METRIC_KEYS and sort != "mhz":
            raise Invalid(f"unknown sort metric {sort!r}")
        order = one("order", "desc")
        if order not in ("asc", "desc"):
            raise Invalid("order must be asc or desc")
        ids = None
        if raw_ids:
            try:
                ids = [int(x) for x in raw_ids.split(",") if x][:64]
            except ValueError:
                raise Invalid("ids must be a comma-separated list of integers")
        # `1` used to mean "the digest matched a release". It now means the
        # strongest thing on offer, because a filter that quietly kept its old,
        # weaker meaning is exactly the confusion this rework is about.
        verified = one("verified")
        if verified in ("1", "true"):
            verified = "attested"
        if verified is not None and verified not in ("ci", "attested", "release"):
            raise Invalid("verified must be 'ci', 'attested' or 'release'")

        flag = {"1": 1, "0": 0, "true": 1, "false": 0}
        return self.store.cores(
            scope=scope,
            user_id=self.who_filter(query),
            verified=verified,
            target=_text(one("target"), 32, "target"),
            vectorize=flag.get(str(one("vectorize", "")).lower()),
            fma=flag.get(str(one("fma", "")).lower()),
            search=_text(one("q"), 64, "q"),
            ids=ids,
            run=run,
            group_run=(group == "run"),
            sort=sort,
            desc=(order == "desc"),
            limit=clamp_int(one("limit", "50"), 1, 500, 50),
        )

    def drain_unread(self):
        """Swallow a request body nothing got round to reading."""
        if getattr(self, "body_read", True):
            return
        self.body_read = True
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.close_connection = True
            return
        if length > 0:
            self.drain(length)

    def drain(self, length: int):
        """Swallow a body we are about to refuse.

        Without this the client is still writing when the response goes out and
        sees a broken pipe instead of the 413. Bounded, so an enormous upload
        cannot be turned into free bandwidth: past the cap the connection is
        closed rather than drained.
        """
        remaining = min(length, MAX_DRAIN)
        if length > MAX_DRAIN:
            self.close_connection = True
        while remaining > 0:
            chunk = self.rfile.read(min(65536, remaining))
            if not chunk:
                break
            remaining -= len(chunk)

    def upload(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            raise Invalid("bad Content-Length")
        if length <= 0:
            raise Invalid("empty body")
        if length > MAX_BODY:
            self.drain(length)
            self.body_read = True
            return self.fail(HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                             f"body larger than {MAX_BODY} bytes")
        self.body_read = True
        raw = self.rfile.read(length)
        if len(raw) != length:
            raise Invalid("truncated body")
        user, how = self.identify()
        if user is not None and how == "cookie":
            self.check_csrf()
        # After the body is consumed, so that a refused upload still leaves the
        # connection in a state the next request can use.
        #
        # An account is limited as itself rather than as an address, so a lab
        # behind one NAT is not one submitter, and gets more room because there
        # is something to take away if it is abused. Anonymous uploads keep the
        # per-address limit exactly as it was.
        if user is not None:
            allowed = self.limiter.allow(f"user:{user['id']}",
                                         self.limiter.limit * ACCOUNT_RATE_FACTOR)
            whose = "from this account"
        else:
            allowed = self.limiter.allow(self.client_key())
            whose = "from this address"
        if not allowed:
            return self.fail(HTTPStatus.TOO_MANY_REQUESTS,
                             f"too many uploads {whose}; try later")

        payload = parse_json(raw)
        # Label and notes may ride along in the query string or inside the
        # document, so both the browser form and a plain curl can set them.
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        label = _text(query.get("label", [None])[0], MAX_TEXT, "label")
        notes = _text(query.get("notes", [None])[0], MAX_NOTES, "notes")
        if isinstance(payload, dict):
            label = label or _text(payload.get("label"), MAX_TEXT, "label")
            notes = notes or _text(payload.get("notes"), MAX_NOTES, "notes")

        # Checked over `raw` -- the bytes as received, before any parsing --
        # because that is what the signature covers. Re-serialising the parsed
        # document first would break every attestation, and quietly accepting
        # one that no longer matched would be worse.
        attestation = self.check_attestation(raw)

        run, cores = validate(payload)
        run_id, token = self.store.insert(
            run, cores, raw.decode("utf-8"), label, notes,
            user_id=user["id"] if user else None, attest=attestation)
        self.send_json(HTTPStatus.CREATED, {
            "id": run_id,
            "delete_token": token,
            "user": user["name"] if user else None,
            "attested": attestation["tier"] if attestation else None,
            "url": f"/#run={run_id}",
            "records": len(cores),
        })

    def check_attestation(self, raw: bytes) -> dict | None:
        """Verify the attestation header, if one was sent.

        A refusal fails the whole upload rather than storing the run without
        the mark. Someone who went to the trouble of attesting a result needs
        to hear that it did not take; silently downgrading them to an ordinary
        row is how a hub ends up full of results nobody can explain.
        """
        token = self.headers.get(ATTEST_HEADER)
        if not token:
            return None
        verifier = self.hub.attest
        if verifier is None:
            raise Denied("this hub does not accept attestations",
                         HTTPStatus.NOT_IMPLEMENTED)
        keys, workflows = verifier
        try:
            return attest.check(token.strip(), raw, keys, workflows)
        except attest.AttestationError as e:
            raise Denied(f"attestation rejected: {e}")
        except OSError as e:
            # The issuer's keys could not be fetched. Not the submitter's
            # fault, and not something to record as a failed attestation.
            self.log_message("JWKS fetch failed: %r", e)
            raise HttpError(HTTPStatus.SERVICE_UNAVAILABLE,
                            "cannot check attestations right now; try again")

    def static(self, path: str):
        if path in ("/", ""):
            path = "/index.html"
        target = (STATIC_DIR / path.lstrip("/")).resolve()
        if not str(target).startswith(str(STATIC_DIR)) or not target.is_file():
            return self.fail(HTTPStatus.NOT_FOUND, "not found")
        ctype = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript",
                                                  "text/javascript"):
            ctype += "; charset=utf-8"
        self.send_bytes(HTTPStatus.OK, target.read_bytes(), ctype,
                        {"Cache-Control": "no-cache"})


def clamp_int(v, lo: int, hi: int, default: int) -> int:
    try:
        n = int(v)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, n))


# What it takes to check an attestation: the issuer's keys, and the workflows
# whose tokens this hub will act on. Both together or neither -- keys without a
# workflow allowlist would verify a signature and learn nothing from it.
Verifier = tuple[attest.KeyStore, list[str]]


class Server(ThreadingHTTPServer):
    """The listening socket, plus the five things a handler reaches for.

    Declared here rather than only attached in `build_server`: a handler
    touches them on every request, and there is otherwise nothing that says
    they exist -- not for a reader, and not for a type checker. The three
    without a default are required; a server built without them is broken, so
    the AttributeError is the right outcome.
    """

    daemon_threads = True
    allow_reuse_address = True

    store: Store
    limiter: RateLimiter
    auth_limiter: RateLimiter
    # What it takes to get an account here: a proof of work, a hard limit on
    # how many an address may create, and optionally an invite code -- or the
    # door shut entirely.
    register_limiter: RateLimiter
    challenges: Challenges
    register_open: bool = True
    register_pow: int = REGISTER_POW_BITS
    signup_token: str | None = None
    trust_proxy: bool = False
    # The keys to check attestations against and the workflows to accept them
    # from, or None for a hub that takes no attestations at all.
    attest: Verifier | None = None


def build_server(db_path: str, host: str, port: int, *, rate_limit: int = 30,
                 window: float = 3600.0, trust_proxy: bool = False,
                 auth_limit: int = 20, attest_workflows: list[str] | None = None,
                 jwks_file: str | None = None, register_open: bool = True,
                 register_pow: int = REGISTER_POW_BITS,
                 register_limit: int = 5,
                 signup_token: str | None = None) -> Server:
    srv = Server((host, port), Handler)
    srv.store = Store(db_path)
    # An empty workflow list refuses attestations, spelled explicitly: a hub
    # that trusts no workflow can verify nothing, and should say so rather than
    # accept tokens it has no policy for.
    workflows = [attest.DEFAULT_WORKFLOW] if attest_workflows is None \
        else [w for w in attest_workflows if w]
    srv.attest = (attest.KeyStore(path=jwks_file), workflows) if workflows else None
    srv.limiter = RateLimiter(rate_limit, window)
    # Sign-ins are limited separately and much harder: uploading a lot is
    # enthusiasm, trying twenty passwords in a quarter of an hour is not.
    srv.auth_limiter = RateLimiter(auth_limit, 900.0)
    # New accounts are limited per hour rather than per quarter of an hour: a
    # person makes one, ever, and the number that matters is how many a script
    # can make overnight.
    srv.register_limiter = RateLimiter(register_limit, 3600.0)
    srv.challenges = Challenges()
    srv.register_open = register_open
    srv.register_pow = max(0, min(MAX_POW_BITS, register_pow))
    srv.signup_token = signup_token or None
    srv.trust_proxy = trust_proxy
    return srv


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=str(HERE / "runs.sqlite3"),
                    help="SQLite database file (created if absent)")
    ap.add_argument("--host", default="127.0.0.1",
                    help="bind address (default 127.0.0.1; use 0.0.0.0 to expose)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--rate-limit", type=int, default=30,
                    help="uploads per address per hour, 0 to disable "
                         f"(a signed-in account gets {ACCOUNT_RATE_FACTOR}x this)")
    ap.add_argument("--auth-limit", type=int, default=20,
                    help="sign-in attempts per address per 15 minutes, "
                         "0 to disable")
    ap.add_argument("--register-limit", type=int, default=5,
                    help="new accounts per address per hour, 0 to disable")
    ap.add_argument("--register-pow", type=int, default=REGISTER_POW_BITS,
                    metavar="BITS",
                    help="proof-of-work a new account must show, in leading "
                         f"zero bits (default {REGISTER_POW_BITS}, max "
                         f"{MAX_POW_BITS}; 0 disables it). Each bit doubles "
                         "the work: on average 16 is half a second in the "
                         "browser, 20 is eight seconds and 22 is half a minute "
                         "-- a random search, so any one attempt may take "
                         "several times that")
    ap.add_argument("--signup-token", metavar="CODE",
                    help="also require this invite code to register, so the "
                         "hub is readable and uploadable by anyone but only "
                         "joinable by people you gave the code to")
    ap.add_argument("--no-register", action="store_true",
                    help="take no new accounts at all (existing ones, "
                         "anonymous uploads and the board are unaffected)")
    ap.add_argument("--attest-workflow", action="append", metavar="REF",
                    help="a job_workflow_ref whose GitHub OIDC attestations "
                         "this hub accepts, without the @ref part (repeatable; "
                         f"default {attest.DEFAULT_WORKFLOW})")
    ap.add_argument("--no-attest", action="store_true",
                    help="refuse attestations entirely")
    ap.add_argument("--jwks-file", metavar="PATH",
                    help="verify against a saved copy of the issuer's keys "
                         "instead of fetching them (for a hub with no outbound "
                         "network; refresh it when GitHub rotates keys)")
    ap.add_argument("--trust-proxy", action="store_true",
                    help="take the client address from X-Forwarded-For, and "
                         "the scheme from X-Forwarded-Proto so session cookies "
                         "are marked Secure (only behind a proxy that sets them)")
    args = ap.parse_args(argv)

    srv = build_server(args.db, args.host, args.port,
                       rate_limit=args.rate_limit, trust_proxy=args.trust_proxy,
                       auth_limit=args.auth_limit,
                       attest_workflows=[] if args.no_attest else args.attest_workflow,
                       jwks_file=args.jwks_file,
                       register_open=not args.no_register,
                       register_pow=args.register_pow,
                       register_limit=args.register_limit,
                       signup_token=args.signup_token)
    host, port = srv.server_address[:2]
    print(f"cpcpub hub on http://{host}:{port}  (db: {args.db})", file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("", file=sys.stderr)
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
