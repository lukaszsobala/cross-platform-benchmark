#!/usr/bin/env python3
"""Tests for the cpu-bench results hub.

    python3 web/test_server.py

Runs a real server on an ephemeral port against a temporary database, so the
HTTP layer, the validation and the SQL are all exercised together.
"""

import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

import server as srv


def core(scope="cpu", cpu=0, **over):
    rec = {
        "scope": scope, "cpu": cpu, "mhz": 4000.0, "mhz_src": "measured",
        "int_lat_mops": 4000.0, "int_thr_mops": 20000.0, "ilp": 5.0,
        "mul_thr_mmul_s": 9000.0, "fp_lat_mflops": 1500.0,
        "fp_thr_mflops": 11000.0, "filp": 7.3, "mem_gbps": 30.0,
        "mem_lat_ns": 100.0, "mem_lat8_ns": 12.0, "mlp": 8.3,
        "disp_thr_mcall_s": 1000.0, "disp_cap_calls": 500.0,
        "disp_prediction": "measured", "disp_gain": 5.0, "disp_span": 7.0,
        "score": 20000.0,
    }
    rec.update(over)
    return rec


def document(mode="per-core", **over):
    doc = {
        "schema": "cpu-bench/1",
        "mode": mode,
        "build": {"compiler": "gcc 15.2.0", "compiler_version": "15.2.0",
                  "cc": "cc", "flags": "-O3 -std=c2x -fno-tree-vectorize",
                  "target": "x86_64", "vectorize": False, "fma": False},
        "system": {"sysname": "Linux", "release": "7.0.0", "machine": "x86_64",
                   "cpus": 8, "cpu_models": "Test CPU 9000"},
        "config": {"threads": 1, "seconds_per_phase": 0.5, "reps": 3,
                   "warmup_seconds": 0.15, "mem_bytes_per_thread": 33554432,
                   "pin": True, "clock": "raw", "seed": 1},
    }
    if mode in ("per-core", "full"):
        doc["cores"] = [core(cpu=0), core(cpu=1, score=15000.0)]
    if mode in ("threads", "full"):
        doc["threads"] = [core("thread", 0), core("thread", 1)]
        doc["total"] = core("total", None, score=None)
        doc["checksum"] = "0xdeadbeefdeadbeef"
    doc.update(over)
    return doc


class HubTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.server = srv.build_server(str(Path(cls.tmp.name) / "t.sqlite3"),
                                      "127.0.0.1", 0, rate_limit=0)
        cls.base = "http://127.0.0.1:%d" % cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    # -- helpers ----------------------------------------------------------
    def req(self, path, method="GET", body=None, headers=None, raw=False):
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
        r = urllib.request.Request(self.base + path, data=data, method=method,
                                   headers=headers or {})
        if data is not None:
            r.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(r) as resp:
            payload = resp.read()
            return resp.status, (payload if raw else json.loads(payload))

    def expect_error(self, path, method="GET", body=None, headers=None):
        with self.assertRaises(urllib.error.HTTPError) as cm:
            self.req(path, method, body, headers)
        return cm.exception.code, json.loads(cm.exception.read())

    def upload(self, doc=None, query=""):
        status, out = self.req("/api/runs" + query, "POST", doc or document())
        self.assertEqual(status, 201)
        return out

    # -- tests ------------------------------------------------------------
    def test_upload_and_fetch(self):
        out = self.upload()
        self.assertEqual(out["records"], 2)
        status, run = self.req(f"/api/runs/{out['id']}")
        self.assertEqual(status, 200)
        self.assertEqual(run["cpu_models"], "Test CPU 9000")
        self.assertEqual(run["target"], "x86_64")
        self.assertEqual(run["vectorize"], 0)
        self.assertEqual(len(run["cores"]), 2)
        self.assertAlmostEqual(run["cores"][0]["int_thr"], 20000.0)

    def test_threads_mode_keeps_total_row(self):
        out = self.upload(document("threads"))
        _, run = self.req(f"/api/runs/{out['id']}")
        scopes = sorted(c["scope"] for c in run["cores"])
        self.assertEqual(scopes, ["thread", "thread", "total"])
        self.assertEqual(run["checksum"], "0xdeadbeefdeadbeef")

    def test_full_mode_keeps_both_phases(self):
        out = self.upload(document("full"))
        self.assertEqual(out["records"], 5)     # 2 cores + 2 threads + total
        _, run = self.req(f"/api/runs/{out['id']}")
        self.assertEqual(run["mode"], "full")
        counts = {}
        for c in run["cores"]:
            counts[c["scope"]] = counts.get(c["scope"], 0) + 1
        self.assertEqual(counts, {"cpu": 2, "thread": 2, "total": 1})
        # A full run is ranked against the per-core population, not per-thread.
        _, rank = self.req(f"/api/runs/{out['id']}/rank")
        self.assertEqual(rank["scope"], "cpu")

    def test_build_string_is_recorded(self):
        out = self.upload()
        _, run = self.req(f"/api/runs/{out['id']}")
        self.assertEqual(run["compiler"], "gcc 15.2.0")
        self.assertEqual(run["compiler_version"], "15.2.0")
        self.assertEqual(run["cc"], "cc")
        self.assertEqual(run["build_flags"], "-O3 -std=c2x -fno-tree-vectorize")

    def test_full_run_needs_at_least_one_result_array(self):
        doc = document("full")
        del doc["cores"]
        del doc["threads"]
        del doc["total"]
        code, err = self.expect_error("/api/runs", "POST", doc)
        self.assertEqual(code, 400)
        self.assertIn("result array", err["error"])

    def test_raw_document_is_preserved(self):
        doc = document()
        out = self.upload(doc)
        _, body = self.req(f"/api/runs/{out['id']}/raw", raw=True)
        self.assertEqual(json.loads(body), doc)

    def test_leaderboard_sorts_and_filters(self):
        self.upload()
        _, out = self.req("/api/cores?scope=cpu&sort=score&order=desc")
        scores = [c["score"] for c in out["cores"]]
        self.assertEqual(scores, sorted(scores, reverse=True))
        _, out = self.req("/api/cores?scope=cpu&target=riscv64")
        self.assertEqual(out["cores"], [])
        _, out = self.req("/api/cores?scope=cpu&q=Test+CPU")
        self.assertTrue(out["cores"])

    def test_latency_metric_sorts_ascending(self):
        self.upload()
        _, out = self.req("/api/cores?scope=cpu&sort=mem_lat_ns&order=asc&limit=5")
        lats = [c["mem_lat_ns"] for c in out["cores"]]
        self.assertEqual(lats, sorted(lats))

    def test_rank_reports_percentile(self):
        out = self.upload()
        _, rank = self.req(f"/api/runs/{out['id']}/rank")
        self.assertEqual(rank["scope"], "cpu")
        self.assertIn("int_thr", rank["metrics"])
        pct = rank["metrics"]["int_thr"]
        self.assertGreater(pct["population"], 0)
        self.assertTrue(0 < pct["percentile"] <= 100)

    def test_label_and_notes_from_query(self):
        out = self.upload(query="?label=my+box&notes=quiet+room")
        _, run = self.req(f"/api/runs/{out['id']}")
        self.assertEqual(run["label"], "my box")
        self.assertEqual(run["notes"], "quiet room")

    def test_delete_requires_matching_token(self):
        out = self.upload()
        code, err = self.expect_error(f"/api/runs/{out['id']}", "DELETE",
                                      headers={"X-Delete-Token": "wrong"})
        self.assertEqual(code, 403)
        status, _ = self.req(f"/api/runs/{out['id']}", "DELETE",
                             headers={"X-Delete-Token": out["delete_token"]})
        self.assertEqual(status, 200)
        code, _ = self.expect_error(f"/api/runs/{out['id']}")
        self.assertEqual(code, 404)

    def test_delete_removes_core_rows(self):
        out = self.upload(document("per-core", cores=[core(cpu=7, score=1.0)]))
        self.req(f"/api/runs/{out['id']}", "DELETE",
                 headers={"X-Delete-Token": out["delete_token"]})
        _, board = self.req("/api/cores?scope=cpu&sort=score&order=asc&limit=200")
        self.assertFalse([c for c in board["cores"] if c["run_id"] == out["id"]])

    # -- rejected input ---------------------------------------------------
    def test_rejects_wrong_schema(self):
        code, err = self.expect_error("/api/runs", "POST",
                                      document(**{"schema": "something/2"}))
        self.assertEqual(code, 400)
        self.assertIn("schema", err["error"])

    def test_rejects_disp_sweep_dump(self):
        code, err = self.expect_error("/api/runs", "POST", document("disp-sweep"))
        self.assertEqual(code, 400)
        self.assertIn("mode", err["error"])

    def test_rejects_nan_and_infinity(self):
        for literal in (b"NaN", b"Infinity", b"-Infinity"):
            body = json.dumps(document()).encode().replace(b"20000.0", literal, 1)
            code, _ = self.expect_error("/api/runs", "POST", body)
            self.assertEqual(code, 400, literal)

    def test_rejects_absurd_values(self):
        code, _ = self.expect_error("/api/runs", "POST",
                                    document("per-core", cores=[core(int_thr_mops=1e30)]))
        self.assertEqual(code, 400)

    def test_rejects_malformed_json(self):
        code, err = self.expect_error("/api/runs", "POST", b"{nope")
        self.assertEqual(code, 400)
        self.assertIn("invalid JSON", err["error"])

    def test_rejects_empty_core_list(self):
        code, _ = self.expect_error("/api/runs", "POST", document("per-core", cores=[]))
        self.assertEqual(code, 400)

    def test_rejects_oversized_body(self):
        big = document("per-core", cores=[core(cpu=i) for i in range(4000)])
        body = json.dumps(big).encode()
        self.assertGreater(len(body), srv.MAX_BODY)
        code, _ = self.expect_error("/api/runs", "POST", body)
        self.assertEqual(code, 413)
        # The server must still be serving afterwards.
        self.assertEqual(self.req("/api/stats")[0], 200)

    def test_rejects_too_many_records(self):
        # Under the byte cap, over the record cap.
        many = document("per-core",
                        cores=[{"scope": "cpu", "cpu": i, "score": 1.0}
                               for i in range(srv.MAX_CORES + 1)])
        code, err = self.expect_error("/api/runs", "POST", many)
        self.assertEqual(code, 400)
        self.assertIn("too many", err["error"])

    def test_rejects_bad_sort_key(self):
        code, _ = self.expect_error("/api/cores?sort=score;DROP+TABLE+runs")
        self.assertEqual(code, 400)

    def test_control_characters_are_stripped_from_text(self):
        out = self.upload(document(label="ok\x07\x00label"))
        _, run = self.req(f"/api/runs/{out['id']}")
        self.assertEqual(run["label"], "oklabel")

    def test_unknown_endpoint_is_404(self):
        code, _ = self.expect_error("/api/nope")
        self.assertEqual(code, 404)

    def test_static_files_are_served_and_confined(self):
        status, body = self.req("/", raw=True)
        self.assertEqual(status, 200)
        self.assertIn(b"cpu-bench results", body)
        code, _ = self.expect_error("/../server.py")
        self.assertEqual(code, 404)

    def test_rate_limit_blocks_floods(self):
        limiter = srv.RateLimiter(2, 60.0)
        self.assertTrue(limiter.allow("a"))
        self.assertTrue(limiter.allow("a"))
        self.assertFalse(limiter.allow("a"))
        self.assertTrue(limiter.allow("b"))


class ValidateTest(unittest.TestCase):
    def test_scope_defaults_per_mode(self):
        doc = document("per-core")
        for c in doc["cores"]:
            del c["scope"]
        _, rows = srv.validate(doc)
        self.assertEqual({r["scope"] for r in rows}, {"cpu"})

    def test_unknown_scope_rejected(self):
        with self.assertRaises(srv.Invalid):
            srv.validate(document("per-core", cores=[core(scope="weird")]))

    def test_missing_measurements_rejected(self):
        empty = {k: None for k in core()}
        empty["scope"] = "cpu"
        with self.assertRaises(srv.Invalid):
            srv.validate(document("per-core", cores=[empty]))

    def test_nulls_survive(self):
        _, rows = srv.validate(document("per-core",
                                        cores=[core(mem_gbps=None, mlp=None)]))
        self.assertIsNone(rows[0]["mem_gbps"])
        self.assertEqual(rows[0]["int_thr"], 20000.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
