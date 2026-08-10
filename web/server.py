#!/usr/bin/env python3
"""cpu-bench results hub: upload `cpu-bench --json` output and compare runs.

Standard library only (http.server + sqlite3), matching the benchmark's own
no-dependencies policy.

    python3 web/server.py --db runs.sqlite3 --port 8080

Uploads are unauthenticated by design -- it is a scoreboard, not a user account
system. Each upload gets a delete token so its submitter can withdraw it, and a
per-IP rate limit keeps the obvious abuse out.
"""

from __future__ import annotations

import argparse
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
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
STATIC_DIR = HERE / "static"
SCHEMA_PATH = HERE / "schema.sql"

SCHEMA_ID = "cpu-bench/1"
MODES = {"threads", "per-core", "full"}
SCOPES = {"cpu", "thread", "total"}

MAX_BODY = 1 << 20          # 1 MiB: a --per-core dump of 256 CPUs is ~120 KiB
MAX_DRAIN = 8 << 20         # how much of a refused body to swallow before
                            # hanging up, so the client can read the 413
MAX_CORES = 1024
MAX_TEXT = 200
MAX_NOTES = 2000
MAX_VALUE = 1e12            # nothing this benchmark reports comes near this

# The metric table is the single source of truth for the API and the UI.
#   kind  rate  -> per-clock normalisation divides by GHz
#         time  -> per-clock normalisation multiplies by GHz (ns -> cycles)
#         ratio -> already clock-independent
#   better  which direction is an improvement; "none" means do not rank it
#   headline  shown by default. Fourteen columns of five significant figures is
#         a data dump, not a result, so the tables carry these three -- the
#         geomean, the compute rate it is mostly made of, and memory bandwidth,
#         which the geomean cannot stand in for -- plus whichever metric the
#         board is being sorted on. The rest are one select away, and every
#         metric is still ranked, filtered and compared on.
METRICS = [
    {"key": "score",       "label": "score",    "unit": "geomean", "kind": "rate",  "better": "high", "headline": True},
    {"key": "int_thr",     "label": "INT-thr",  "unit": "Mops/s",  "kind": "rate",  "better": "high", "headline": True},
    {"key": "int_lat",     "label": "INT-lat",  "unit": "Mops/s",  "kind": "rate",  "better": "high"},
    {"key": "ilp",         "label": "ILP",      "unit": "x",       "kind": "ratio", "better": "high"},
    {"key": "mul_thr",     "label": "MUL-thr",  "unit": "Mmul/s",  "kind": "rate",  "better": "high"},
    {"key": "fp_thr",      "label": "FP-thr",   "unit": "Mflop/s", "kind": "rate",  "better": "high"},
    {"key": "fp_lat",      "label": "FP-lat",   "unit": "Mflop/s", "kind": "rate",  "better": "high"},
    {"key": "filp",        "label": "fILP",     "unit": "x",       "kind": "ratio", "better": "none"},
    {"key": "mem_gbps",    "label": "MEM",      "unit": "GB/s",    "kind": "rate",  "better": "high", "headline": True},
    {"key": "mem_lat_ns",  "label": "MEMlat",   "unit": "ns",      "kind": "time",  "better": "low"},
    {"key": "mem_lat8_ns", "label": "MEMlat/8", "unit": "ns",      "kind": "time",  "better": "low"},
    {"key": "mlp",         "label": "MLP",      "unit": "x",       "kind": "ratio", "better": "high"},
    {"key": "disp_thr",    "label": "DISP-thr", "unit": "Mcall/s", "kind": "rate",  "better": "high"},
    {"key": "disp_cap",    "label": "DISPcap",  "unit": "calls",   "kind": "ratio", "better": "high"},
]
METRIC_KEYS = [m["key"] for m in METRICS]

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

RUN_COLUMNS = [
    "id", "created_at", "label", "notes", "mode", "compiler",
    "compiler_version", "cc", "build_flags", "target",
    "vectorize", "fma", "sysname", "os_release", "machine", "cpus",
    "cpu_models", "threads", "seconds", "reps", "warmup", "mem_bytes", "pin",
    "clock", "seed", "dram_name", "dram_mhz_min", "dram_mhz_max", "checksum",
]


class Invalid(Exception):
    """An upload that will not be stored, with a message the client can show."""


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
#
# Everything here runs on attacker-controlled input: the payload is whatever was
# POSTed, not necessarily something cpu-bench produced.


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
        row = {"scope": scope}
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
# Storage
# ---------------------------------------------------------------------------


def iso_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


class Store:
    ADDED_COLUMNS = (("compiler_version", "TEXT"), ("cc", "TEXT"),
                     ("build_flags", "TEXT"))

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

    def connect(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.path, timeout=10.0)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys = ON")
        db.execute("PRAGMA journal_mode = WAL")
        return db

    def insert(self, run: dict, cores: list[dict], raw: str,
               label: str | None, notes: str | None) -> tuple[int, str]:
        token = secrets.token_urlsafe(24)
        run = dict(run, created_at=iso_now(), delete_token=token,
                   label=label, notes=notes, raw=raw)
        cols = list(run)
        with self.connect() as db:
            cur = db.execute(
                f"INSERT INTO runs ({','.join(cols)}) "
                f"VALUES ({','.join('?' * len(cols))})",
                [run[c] for c in cols])
            run_id = int(cur.lastrowid)
            ccols = ["run_id", "scope"] + CORE_COLUMNS
            db.executemany(
                f"INSERT INTO cores ({','.join(ccols)}) "
                f"VALUES ({','.join('?' * len(ccols))})",
                [[run_id] + [c.get(col) for col in ccols[1:]] for c in cores])
        return run_id, token

    def delete(self, run_id: int, token: str) -> bool:
        with self.connect() as db:
            row = db.execute("SELECT delete_token FROM runs WHERE id = ?",
                             (run_id,)).fetchone()
            if row is None or not secrets.compare_digest(row["delete_token"], token):
                return False
            db.execute("DELETE FROM cores WHERE run_id = ?", (run_id,))
            db.execute("DELETE FROM runs WHERE id = ?", (run_id,))
        return True

    def run(self, run_id: int) -> dict | None:
        with self.connect() as db:
            row = db.execute(
                f"SELECT {','.join(RUN_COLUMNS)} FROM runs WHERE id = ?",
                (run_id,)).fetchone()
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

    def runs(self, limit: int = 50, offset: int = 0) -> list[dict]:
        with self.connect() as db:
            rows = db.execute(
                f"SELECT r.{', r.'.join(RUN_COLUMNS)}, "
                f"       (SELECT COUNT(*) FROM cores c WHERE c.run_id = r.id) AS records, "
                f"       (SELECT MAX(c.score) FROM cores c "
                f"          WHERE c.run_id = r.id AND c.scope != 'total') AS best_score "
                f"FROM runs r ORDER BY r.id DESC LIMIT ? OFFSET ?",
                (limit, offset)).fetchall()
        return [dict(r) for r in rows]

    def cores(self, *, scope: str | None = "cpu", target: str | None = None,
              vectorize: int | None = None, fma: int | None = None,
              search: str | None = None, ids: list[int] | None = None,
              run: int | None = None, group_run: bool = True,
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
            targets = db.execute(
                "SELECT target, COUNT(*) AS n FROM runs WHERE target IS NOT NULL "
                "GROUP BY target ORDER BY n DESC").fetchall()
        return {"runs": runs, "records": cores,
                "targets": [dict(t) for t in targets]}


# ---------------------------------------------------------------------------
# Rate limiting
# ---------------------------------------------------------------------------


class RateLimiter:
    def __init__(self, limit: int, window: float):
        self.limit = limit
        self.window = window
        self.hits: dict[str, deque[float]] = defaultdict(deque)
        self.lock = threading.Lock()

    def allow(self, key: str) -> bool:
        if self.limit <= 0:
            return True
        now = time.monotonic()
        with self.lock:
            q = self.hits[key]
            while q and now - q[0] > self.window:
                q.popleft()
            if len(q) >= self.limit:
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


class Handler(BaseHTTPRequestHandler):
    server_version = "cpu-bench-hub/1.0"
    protocol_version = "HTTP/1.1"       # every response below sets Content-Length

    # -- plumbing ----------------------------------------------------------
    @property
    def store(self) -> Store:
        return self.server.store            # type: ignore[attr-defined]

    @property
    def limiter(self) -> RateLimiter:
        return self.server.limiter          # type: ignore[attr-defined]

    def client_key(self) -> str:
        if getattr(self.server, "trust_proxy", False):
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

    def log_message(self, fmt, *args):      # one tidy line per request
        sys.stderr.write("%s - %s\n" % (self.client_key(), fmt % args))

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
        except Invalid as e:
            self.fail(HTTPStatus.BAD_REQUEST, str(e))
        except BrokenPipeError:
            pass
        except Exception as e:                      # never leak a traceback
            self.log_message("unhandled error: %r", e)
            try:
                self.fail(HTTPStatus.INTERNAL_SERVER_ERROR, "internal error")
            except Exception:
                pass

    def api(self, method: str, path: str, query: dict):
        if path == "/api/metrics" and method == "GET":
            return self.send_json(HTTPStatus.OK, {"metrics": METRICS,
                                                  "scopes": sorted(SCOPES)})
        if path == "/api/stats" and method == "GET":
            return self.send_json(HTTPStatus.OK, self.store.stats())
        if path == "/api/cores" and method == "GET":
            return self.send_json(HTTPStatus.OK, {"cores": self.query_cores(query)})
        if path == "/api/runs":
            if method == "GET":
                limit = clamp_int(query.get("limit", ["50"])[0], 1, 200, 50)
                offset = clamp_int(query.get("offset", ["0"])[0], 0, 1 << 30, 0)
                return self.send_json(HTTPStatus.OK,
                                      {"runs": self.store.runs(limit, offset)})
            if method == "POST":
                return self.upload()
            return self.fail(HTTPStatus.METHOD_NOT_ALLOWED, "method not allowed")

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
                token = (self.headers.get("X-Delete-Token")
                         or query.get("token", [""])[0])
                if not token:
                    return self.fail(HTTPStatus.UNAUTHORIZED, "delete token required")
                if not self.store.delete(run_id, token):
                    return self.fail(HTTPStatus.FORBIDDEN,
                                     "no such run, or wrong delete token")
                return self.send_json(HTTPStatus.OK, {"deleted": run_id})
        return self.fail(HTTPStatus.NOT_FOUND, "no such endpoint")

    def query_cores(self, query: dict) -> list[dict]:
        def one(name, default=None):
            v = query.get(name, [default])[0]
            return v if v not in ("", None) else default

        # An explicit id list, or one run's records, addresses records directly
        # -- a shared comparison link, or the expanded view of one board row --
        # so neither is narrowed by the leaderboard's scope default, and neither
        # is collapsed to one row per run.
        addressed = bool(one("ids") or one("run"))
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
        if one("ids"):
            try:
                ids = [int(x) for x in one("ids").split(",") if x][:64]
            except ValueError:
                raise Invalid("ids must be a comma-separated list of integers")
        flag = {"1": 1, "0": 0, "true": 1, "false": 0}
        return self.store.cores(
            scope=scope,
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
            return self.fail(HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                             f"body larger than {MAX_BODY} bytes")
        raw = self.rfile.read(length)
        if len(raw) != length:
            raise Invalid("truncated body")
        # After the body is consumed, so that a refused upload still leaves the
        # connection in a state the next request can use.
        if not self.limiter.allow(self.client_key()):
            return self.fail(HTTPStatus.TOO_MANY_REQUESTS,
                             "too many uploads from this address; try later")

        payload = parse_json(raw)
        # Label and notes may ride along in the query string or inside the
        # document, so both the browser form and a plain curl can set them.
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        label = _text(query.get("label", [None])[0], MAX_TEXT, "label")
        notes = _text(query.get("notes", [None])[0], MAX_NOTES, "notes")
        if isinstance(payload, dict):
            label = label or _text(payload.get("label"), MAX_TEXT, "label")
            notes = notes or _text(payload.get("notes"), MAX_NOTES, "notes")

        run, cores = validate(payload)
        run_id, token = self.store.insert(
            run, cores, raw.decode("utf-8"), label, notes)
        self.send_json(HTTPStatus.CREATED, {
            "id": run_id,
            "delete_token": token,
            "url": f"/#run={run_id}",
            "records": len(cores),
        })

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


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def build_server(db_path: str, host: str, port: int, *, rate_limit: int = 30,
                 window: float = 3600.0, trust_proxy: bool = False) -> Server:
    srv = Server((host, port), Handler)
    srv.store = Store(db_path)              # type: ignore[attr-defined]
    srv.limiter = RateLimiter(rate_limit, window)   # type: ignore[attr-defined]
    srv.trust_proxy = trust_proxy           # type: ignore[attr-defined]
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
                    help="uploads per address per hour, 0 to disable")
    ap.add_argument("--trust-proxy", action="store_true",
                    help="take the client address from X-Forwarded-For "
                         "(only behind a proxy that sets it)")
    args = ap.parse_args(argv)

    srv = build_server(args.db, args.host, args.port,
                       rate_limit=args.rate_limit, trust_proxy=args.trust_proxy)
    host, port = srv.server_address[:2]
    print(f"cpu-bench hub on http://{host}:{port}  (db: {args.db})", file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("", file=sys.stderr)
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
