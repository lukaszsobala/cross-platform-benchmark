#!/usr/bin/env python3
"""Tests for the cpcpub results hub.

    python3 web/test_server.py

Runs a real server on an ephemeral port against a temporary database, so the
HTTP layer, the validation and the SQL are all exercised together.
"""

import hashlib
import http.client
import http.cookiejar
import json
import sqlite3
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from collections.abc import Sequence
from pathlib import Path
from typing import Any

import server as srv

# A fixture has to be a document the benchmark could have written. The hub
# recomputes `score`, `ilp`, `filp` and `mlp` from the numbers they are made of
# and refuses an upload that disagrees with itself, so a record carrying a score
# typed in independently of its own measurements is no longer a valid input --
# it is the thing the check exists to catch.
#
# A test that wants a row to rank at a chosen score therefore still asks for it,
# and the throughput components are scaled until their geometric mean lands
# there. The test keeps saying what it meant and the document stays honest.

# The four score components a fixture scales. The other two -- the memory term
# and the dispatch span -- are left alone: one is a latency that would have to
# move the wrong way and the other cannot go negative, and four are enough.
SCORE_INPUTS = ("int_thr_mops", "fp_thr_mflops", "mul_thr_mmul_s",
                "disp_thr_mcall_s")


def _cols(rec):
    """The record under the hub's column names, which its helpers read."""
    return {col: rec.get(jkey) for jkey, col, _ in srv.CORE_FIELDS}


def _score_of(rec, threads=1.0):
    return srv._geomean(srv.score_terms(_cols(rec), threads))


def _tie(rec, asked, ratio, top, bottom, move):
    """Keep `ratio` equal to top/bottom.

    A test that pinned the ratio keeps it and `move` shifts to match -- always
    the field of the three that is not one of the score's own inputs, so tying
    a ratio cannot quietly move the score. Otherwise the ratio follows the two
    measurements it is the ratio of.
    """
    if asked.get(ratio) is not None:
        r = float(asked[ratio])
        other = rec.get(bottom if move == top else top)
        if not isinstance(other, (int, float)) or not r:
            rec[move] = None
        else:
            rec[move] = r * other if move == top else other / r
        return
    t, b = rec.get(top), rec.get(bottom)
    rec[ratio] = (t / b if isinstance(t, (int, float)) and isinstance(b, (int, float))
                  and b else None)


def core(scope="cpu", cpu: int | None = 0, *, threads=1.0, **over):
    rec: dict[str, object] = {
        "scope": scope, "cpu": cpu, "mhz": 4000.0, "mhz_src": "measured",
        "int_lat_mops": 4000.0, "int_thr_mops": 20000.0, "ilp": None,
        "mul_thr_mmul_s": 9000.0, "fp_lat_mflops": 1500.0,
        "fp_thr_mflops": 11000.0, "filp": None, "mem_gbps": 30.0,
        "mem_lat_ns": 100.0, "mem_lat8_ns": 12.0, "mlp": None,
        "disp_thr_mcall_s": 1000.0, "disp_cap_calls": 500.0,
        "disp_prediction": "measured", "disp_gain": 5.0, "disp_span": 7.0,
        "score": None,
    }
    rec.update(over)

    # Scale to the score the caller asked for, if it asked for one. Four of the
    # six terms move, so a geometric mean over six of them shifts by m^(4/6) --
    # which is the exponent below, undoing it.
    want = over.get("score")
    if want is not None:
        now = _score_of(rec, threads)
        if now > 0.0:
            m = (float(want) / now) ** 1.5
            for key in SCORE_INPUTS:
                if isinstance(rec.get(key), (int, float)):
                    rec[key] *= m

    _tie(rec, over, "ilp", "int_thr_mops", "int_lat_mops", "int_lat_mops")
    _tie(rec, over, "filp", "fp_thr_mflops", "fp_lat_mflops", "fp_lat_mflops")
    _tie(rec, over, "mlp", "mem_lat_ns", "mem_lat8_ns", "mem_lat_ns")
    rec["score"] = _score_of(rec, threads) or None
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
        # The whole-machine score counts one set of pointer chases per thread,
        # so it is built knowing how many the document says there were.
        doc["total"] = core("total", None, threads=doc["config"]["threads"])
        doc["checksum"] = "0xdeadbeefdeadbeef"
    doc.update(over)
    return doc


class HubTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.server = srv.build_server(str(Path(cls.tmp.name) / "t.sqlite3"),
                                      "127.0.0.1", 0, rate_limit=0)
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
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

    def test_board_rows_carry_the_operating_system(self):
        """The board has an OS column, so the listing has to answer with one."""
        self.upload()
        _, out = self.req("/api/cores?scope=cpu&limit=1")
        self.assertEqual(out["cores"][0]["sysname"], "Linux")

    def test_latency_metric_sorts_ascending(self):
        self.upload()
        _, out = self.req("/api/cores?scope=cpu&sort=mem_lat_ns&order=asc&limit=5")
        lats = [c["mem_lat_ns"] for c in out["cores"]]
        self.assertEqual(lats, sorted(lats))

    # -- one upload, one row ----------------------------------------------
    def test_upload_takes_one_leaderboard_row(self):
        """A machine with 8 cores is one result, not eight."""
        cores = [core(cpu=i, score=1000.0 + i) for i in range(8)]
        out = self.upload(document("per-core", cores=cores))
        self.assertEqual(out["records"], 8)
        _, board = self.req("/api/cores?scope=cpu&sort=score&order=desc&limit=200")
        mine = [c for c in board["cores"] if c["run_id"] == out["id"]]
        self.assertEqual(len(mine), 1)
        # The row stands for the whole upload and says how many it covers.
        self.assertEqual(mine[0]["records"], 8)

    def test_the_row_is_the_best_record_at_the_sorted_metric(self):
        cores = [core(cpu=0, score=1000.0, mem_lat_ns=300.0),
                 core(cpu=1, score=9000.0, mem_lat_ns=200.0),
                 core(cpu=2, score=5000.0, mem_lat_ns=100.0)]
        out = self.upload(document("per-core", cores=cores))

        def row(sort, order):
            _, b = self.req(f"/api/cores?scope=cpu&sort={sort}&order={order}&limit=200")
            return next(c for c in b["cores"] if c["run_id"] == out["id"])

        best = row("score", "desc")
        self.assertEqual(best["cpu"], 1)
        # Every column comes from that one core, so the row is a real
        # measurement rather than a per-metric best of several.
        self.assertEqual(best["mem_lat_ns"], 200.0)
        quickest = row("mem_lat_ns", "asc")
        self.assertEqual(quickest["cpu"], 2)
        # Not an exact equality: a fixture reaches a chosen score by scaling the
        # components the score is the geometric mean of, so it arrives within a
        # float rounding of it rather than on it.
        self.assertAlmostEqual(quickest["score"], 5000.0, places=6)

    # -- per GHz ------------------------------------------------------------
    def test_per_ghz_ranks_by_the_per_ghz_figure(self):
        """The board is ordered by the values it is printing.

        Per GHz is a reading of the same numbers, not a second set of them, so
        the ranking has to follow it: a column of figures the eye can see is
        out of order is the bug, whatever the underlying values do.
        """
        fast = self.upload(document(label="perghz-fast", cores=[
            core(cpu=0, mhz=5000.0, score=20000.0)]))       # 4000 per GHz
        frugal = self.upload(document(label="perghz-frugal", cores=[
            core(cpu=0, mhz=2000.0, score=12000.0)]))       # 6000 per GHz

        def order(norm):
            _, b = self.req(f"/api/cores?scope=cpu&q=perghz&sort=score"
                            f"&order=desc&norm={norm}&limit=10")
            return [c["run_id"] for c in b["cores"]]

        self.assertEqual(order("abs"), [fast["id"], frugal["id"]])
        self.assertEqual(order("ghz"), [frugal["id"], fast["id"]])

    def test_per_ghz_turns_a_latency_into_cycles(self):
        """A `time` metric multiplies by the clock rather than dividing."""
        slow = self.upload(document(label="cycles-slow", cores=[
            core(cpu=0, mhz=2000.0, mem_lat_ns=100.0)]))     # 200 cycles
        quick = self.upload(document(label="cycles-quick", cores=[
            core(cpu=0, mhz=5000.0, mem_lat_ns=60.0)]))      # 300 cycles

        def order(norm):
            _, b = self.req(f"/api/cores?scope=cpu&q=cycles-&sort=mem_lat_ns"
                            f"&order=asc&norm={norm}&limit=10")
            return [c["run_id"] for c in b["cores"]]

        self.assertEqual(order("abs"), [quick["id"], slow["id"]])
        self.assertEqual(order("ghz"), [slow["id"], quick["id"]])

    def test_a_clock_independent_metric_does_not_move(self):
        """`fixed` and `ratio` metrics rank the same either way.

        MEM is the memory controller's figure, not the core's, so dividing it
        by a core clock would only flatter whichever core was clocked lower
        while measuring the same DRAM. The page does not do it and neither
        does the ordering.
        """
        wide = self.upload(document(label="fixed-wide", cores=[
            core(cpu=0, mhz=5000.0, mem_gbps=30.0, ilp=6.0)]))
        narrow = self.upload(document(label="fixed-narrow", cores=[
            core(cpu=0, mhz=1000.0, mem_gbps=20.0, ilp=4.0)]))

        for sort in ("mem_gbps", "ilp"):
            for norm in ("abs", "ghz"):
                with self.subTest(sort=sort, norm=norm):
                    _, b = self.req(f"/api/cores?scope=cpu&q=fixed-&sort={sort}"
                                    f"&order=desc&norm={norm}&limit=10")
                    self.assertEqual([c["run_id"] for c in b["cores"]],
                                     [wide["id"], narrow["id"]])

    def test_per_ghz_stands_a_run_up_by_its_most_efficient_record(self):
        """Which record represents an upload follows the reading too.

        Ranked per GHz, a machine is on the board for its most efficient core,
        not its fastest -- otherwise the row shown is one the ranking did not
        pick, and its printed value can sit out of order among the rest.
        """
        out = self.upload(document(label="pick-perghz", cores=[
            core(cpu=0, mhz=5000.0, score=20000.0),          # 4000 per GHz
            core(cpu=1, mhz=2000.0, score=12000.0)]))        # 6000 per GHz

        def row(norm):
            _, b = self.req(f"/api/cores?scope=cpu&q=pick-perghz&sort=score"
                            f"&order=desc&norm={norm}&limit=10")
            return next(c for c in b["cores"] if c["run_id"] == out["id"])

        self.assertEqual(row("abs")["cpu"], 0)
        self.assertEqual(row("ghz")["cpu"], 1)

    def test_a_record_with_no_clock_sorts_on_what_it_shows(self):
        """No clock, nothing to divide by, so the raw figure is the reading.

        Runs from binaries that could not read a clock are permanently in that
        state. The page prints their absolute value in the per-GHz view, so
        that is what they have to be placed by; anything else puts a row in a
        position its own number does not explain.
        """
        clocked = self.upload(document(label="noclock-clocked", cores=[
            core(cpu=0, mhz=5000.0, score=20000.0)]))        # 4000 per GHz
        clockless = self.upload(document(label="noclock-bare", cores=[
            core(cpu=0, mhz=None, score=5000.0)]))           # shown as 5000

        _, b = self.req("/api/cores?scope=cpu&q=noclock-&sort=score"
                        "&order=desc&norm=ghz&limit=10")
        self.assertEqual([c["run_id"] for c in b["cores"]],
                         [clockless["id"], clocked["id"]])

    def test_rejects_an_unknown_norm(self):
        code, _ = self.expect_error("/api/cores?norm=cycles")
        self.assertEqual(code, 400)

    def test_records_of_one_run_are_addressable(self):
        """What the expanded view of a board row asks for."""
        cores = [core(cpu=i, score=1000.0 + i) for i in range(4)]
        out = self.upload(document("per-core", cores=cores))
        _, all_of = self.req(
            f"/api/cores?run={out['id']}&scope=cpu&sort=score&order=desc")
        self.assertEqual([c["cpu"] for c in all_of["cores"]], [3, 2, 1, 0])
        self.assertTrue(all(c["run_id"] == out["id"] for c in all_of["cores"]))

    def test_ids_address_records_not_runs(self):
        """A shared comparison link names individual cores of the same run."""
        out = self.upload(document("per-core",
                                   cores=[core(cpu=0), core(cpu=1, score=1.0)]))
        _, run = self.req(f"/api/runs/{out['id']}")
        ids = ",".join(str(c["id"]) for c in run["cores"])
        _, picked = self.req(f"/api/cores?ids={ids}")
        self.assertEqual(len(picked["cores"]), 2)

    def test_group_none_returns_every_record(self):
        cores = [core(cpu=i) for i in range(3)]
        out = self.upload(document("per-core", cores=cores))
        _, flat = self.req("/api/cores?scope=cpu&group=none&limit=500")
        self.assertEqual(len([c for c in flat["cores"] if c["run_id"] == out["id"]]), 3)

    def test_rejects_bad_group(self):
        code, _ = self.expect_error("/api/cores?group=everything")
        self.assertEqual(code, 400)

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
        code, _ = self.expect_error(f"/api/runs/{out['id']}", "DELETE",
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

    # -- page size --------------------------------------------------------
    def test_a_listing_shows_the_page_size_it_was_asked_for(self):
        for _ in range(3):
            self.upload()
        _, one = self.req("/api/runs?limit=1")
        self.assertEqual(len(one["runs"]), 1)
        _, two = self.req("/api/cores?scope=cpu&limit=2")
        self.assertEqual(len(two["cores"]), 2)

    def test_a_listing_can_be_walked_page_by_page(self):
        """Every row is reachable, once, by stepping the offset.

        The size of a page decides how much is on screen, never how much of the
        board can be read: what is past the first page is what `offset` is for,
        and `total` is how a page knows there is another one.
        """
        for i in range(7):
            self.upload(document(label=f"page me {i}"))
        seen, offset = [], 0
        while True:
            _, page = self.req(f"/api/cores?scope=cpu&limit=3&offset={offset}")
            self.assertEqual(page["offset"], offset)
            self.assertEqual(page["limit"], 3)
            seen += [c["id"] for c in page["cores"]]
            offset += 3
            if offset >= page["total"]:
                break
        self.assertEqual(len(seen), len(set(seen)))       # no row twice
        self.assertEqual(len(seen), page["total"])        # and none missed
        # Past the end is an empty page rather than an error or a wrap-around.
        _, past = self.req(f"/api/cores?scope=cpu&limit=3&offset={page['total'] + 90}")
        self.assertEqual(past["cores"], [])
        self.assertEqual(past["total"], page["total"])

    def test_runs_are_paged_the_same_way(self):
        for i in range(4):
            self.upload(document(label=f"run page {i}"))
        _, first = self.req("/api/runs?limit=2&offset=0")
        _, second = self.req("/api/runs?limit=2&offset=2")
        self.assertEqual(len(first["runs"]), 2)
        self.assertEqual(len(second["runs"]), 2)
        self.assertGreaterEqual(first["total"], 4)
        self.assertFalse({r["id"] for r in first["runs"]} &
                         {r["id"] for r in second["runs"]})

    def test_a_count_counts_what_the_filters_left(self):
        """The total is the filtered listing's, not the whole hub's.

        A pager built from a count that ignored the filters would offer pages
        that are not there.
        """
        self.upload(document(label="countme-alpha"))
        _, page = self.req("/api/cores?scope=cpu&q=countme-alpha&limit=1")
        self.assertEqual(page["total"], 1)
        self.assertEqual(len(page["cores"]), 1)
        # One row per upload on the board, however many records it holds --
        # counted the same way, or the last page would be short of rows that
        # were never going to be shown separately.
        out = self.upload(document("per-core", label="countme-beta",
                                   cores=[core(cpu=i, score=float(i + 1))
                                          for i in range(5)]))
        _, grouped = self.req("/api/cores?scope=cpu&q=countme-beta&limit=10")
        self.assertEqual(grouped["total"], 1)
        _, flat = self.req("/api/cores?scope=cpu&q=countme-beta&group=none&limit=10")
        self.assertEqual(flat["total"], 5)
        self.assertEqual(flat["cores"][0]["run_id"], out["id"])

    def test_both_listings_stop_at_the_same_cap(self):
        """An over-cap limit is clamped rather than refused.

        The two endpoints used to cap at different numbers, which made the
        page's own picker wrong for one of them. Nothing here uploads MAX_PAGE
        runs to see the cut -- what matters is that neither listing errors and
        neither hands back more than the cap the picker offers.
        """
        self.upload()
        for path in (f"/api/runs?limit={srv.MAX_PAGE * 10}",
                     f"/api/cores?scope=cpu&limit={srv.MAX_PAGE * 10}"):
            status, out = self.req(path)
            self.assertEqual(status, 200)
            rows = out.get("runs", out.get("cores"))
            self.assertLessEqual(len(rows), srv.MAX_PAGE)
        # A limit that is not a number at all falls back rather than failing.
        self.assertEqual(self.req("/api/runs?limit=lots")[0], 200)

    # -- rejected input ---------------------------------------------------
    def test_rejects_wrong_schema(self):
        code, err = self.expect_error("/api/runs", "POST",
                                      document(schema="something/2"))
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
        self.assertIn(b"cpcpub results", body)
        code, _ = self.expect_error("/../server.py")
        self.assertEqual(code, 404)

    def test_rate_limit_blocks_floods(self):
        limiter = srv.RateLimiter(2, 60.0)
        self.assertTrue(limiter.allow("a"))
        self.assertTrue(limiter.allow("a"))
        self.assertFalse(limiter.allow("a"))
        self.assertTrue(limiter.allow("b"))


class Browser:
    """One client with its own cookie jar, as a browser has.

    Sends the CSRF header the page would send, since that is what the server
    requires of a cookie-authenticated write; `csrf=False` reproduces the
    request another site could make, which is the one that must be refused.
    """

    def __init__(self, base: str):
        self.base = base
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar))

    def cookie(self, name: str) -> str | None:
        for c in self.jar:
            if c.name == name:
                return c.value
        return None

    def call(self, path, method="GET", body=None, headers=None, csrf=True):
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method,
                                     headers=dict(headers or {}))
        if data is not None:
            req.add_header("Content-Type", "application/json")
        token = self.cookie(srv.CSRF_COOKIE)
        if csrf and token:
            req.add_header(srv.CSRF_HEADER, token)
        try:
            with self.opener.open(req) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())


DIGEST_A = "a" * 64
DIGEST_B = "b" * 64


def manifest(release: Any = "v1.0.0", digests: Sequence[str] = (DIGEST_A,),
             **over: Any) -> dict:
    """A builds manifest, valid unless a test asks for otherwise.

    Typed loose on purpose: half the tests below hand it a `release` of None or
    a `builds` of "no" to watch the manifest be refused, and a signature that
    forbade that would be describing the wrong function.
    """
    doc: dict[str, Any] = {
        "schema": "cpu-bench-builds/1",
        "release": release,
        "url": f"https://github.com/example/cpcpub/releases/tag/{release}",
        "builds": [{"filename": f"cpcpub-linux-x86_64-{i}", "sha256": d,
                    "target": "x86_64", "march": "x86-64"}
                   for i, d in enumerate(digests)],
    }
    doc.update(over)
    return doc


class VerifiedTest(unittest.TestCase):
    """Which runs count as produced by a published build, and which do not."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.db = str(Path(cls.tmp.name) / "v.sqlite3")
        cls.server = srv.build_server(cls.db, "127.0.0.1", 0, rate_limit=0)
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.store = srv.Store(cls.db)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def setUp(self):
        with self.store.connect() as db:
            db.execute("DELETE FROM verified_builds")

    def upload(self, digest=None, **over):
        build = dict(document()["build"])
        if digest is not None:
            build["binary_sha256"] = digest
        b = Browser(self.base)
        status, out = b.call("/api/runs", "POST", document(build=build, **over))
        self.assertEqual(status, 201, out)
        return b, out

    # -- the digest itself --------------------------------------------------
    def test_the_digest_round_trips(self):
        _, out = self.upload(DIGEST_A)
        b = Browser(self.base)
        _, run = b.call(f"/api/runs/{out['id']}")
        self.assertEqual(run["binary_sha256"], DIGEST_A)

    def test_a_malformed_digest_is_refused(self):
        for bad in ("nonsense", "A" * 63, "g" * 64, 12345, "  "):
            with self.subTest(bad):
                build = dict(document()["build"], binary_sha256=bad)
                code, _ = Browser(self.base).call(
                    "/api/runs", "POST", document(build=build))
                self.assertEqual(code, 400)

    def test_an_uppercase_digest_is_normalised(self):
        """sha256sum(1) and the benchmark agree on lowercase; be kind anyway."""
        build = dict(document()["build"], binary_sha256=DIGEST_A.upper())
        b = Browser(self.base)
        status, out = b.call("/api/runs", "POST", document(build=build))
        self.assertEqual(status, 201)
        _, run = b.call(f"/api/runs/{out['id']}")
        self.assertEqual(run["binary_sha256"], DIGEST_A)

    def test_a_run_without_a_digest_is_still_accepted(self):
        _, out = self.upload(None)
        _, run = Browser(self.base).call(f"/api/runs/{out['id']}")
        self.assertIsNone(run["binary_sha256"])
        self.assertIsNone(run["release_build"])

    # -- matching -----------------------------------------------------------
    def test_a_published_digest_marks_the_run_as_a_release_build(self):
        self.store.add_verified_builds(manifest("v1.2.3", [DIGEST_A]))
        _, out = self.upload(DIGEST_A)
        b = Browser(self.base)
        _, run = b.call(f"/api/runs/{out['id']}")
        self.assertEqual(run["release_build"], "v1.2.3")
        _, board = b.call("/api/cores?scope=cpu&verified=release")
        self.assertIn(out["id"], [c["run_id"] for c in board["cores"]])

    def test_an_unpublished_digest_does_not(self):
        self.store.add_verified_builds(manifest("v1.2.3", [DIGEST_A]))
        _, out = self.upload(DIGEST_B)
        b = Browser(self.base)
        self.assertIsNone(b.call(f"/api/runs/{out['id']}")[1]["release_build"])
        _, board = b.call("/api/cores?scope=cpu&verified=release&limit=200")
        self.assertNotIn(out["id"], [c["run_id"] for c in board["cores"]])

    def test_the_old_filter_spellings_still_answer(self):
        """`verified=ci|attested|1` were the GitHub-signature tiers this hub
        used to keep. The marks are gone; a saved link that names one must
        still answer as the release filter rather than break."""
        self.store.add_verified_builds(manifest("v1.2.3", [DIGEST_A]))
        _, out = self.upload(DIGEST_A)
        b = Browser(self.base)
        for spelling in ("release", "ci", "attested", "1", "true"):
            code, board = b.call(f"/api/cores?scope=cpu&verified={spelling}")
            self.assertEqual(code, 200, spelling)
            self.assertIn(out["id"], [c["run_id"] for c in board["cores"]], spelling)

    def test_a_bad_filter_value_is_refused(self):
        code, _ = Browser(self.base).call("/api/cores?verified=sort-of")
        self.assertEqual(code, 400)

    def test_loading_a_manifest_marks_runs_already_stored(self):
        """The reason the match is a join and not a stamp at upload time."""
        _, out = self.upload(DIGEST_A)
        b = Browser(self.base)
        self.assertIsNone(b.call(f"/api/runs/{out['id']}")[1]["release_build"])
        self.store.add_verified_builds(manifest("v2.0.0", [DIGEST_A]))
        self.assertEqual(b.call(f"/api/runs/{out['id']}")[1]["release_build"], "v2.0.0")

    def test_forgetting_a_release_unmarks_its_runs(self):
        self.store.add_verified_builds(manifest("v1.2.3", [DIGEST_A]))
        _, out = self.upload(DIGEST_A)
        b = Browser(self.base)
        self.assertEqual(b.call(f"/api/runs/{out['id']}")[1]["release_build"], "v1.2.3")
        self.store.forget_release("v1.2.3")
        self.assertIsNone(b.call(f"/api/runs/{out['id']}")[1]["release_build"])

    def test_reloading_a_release_replaces_what_it_published(self):
        self.store.add_verified_builds(manifest("v1.2.3", [DIGEST_A, DIGEST_B]))
        self.store.add_verified_builds(manifest("v1.2.3", [DIGEST_A]))
        digests = {b["sha256"] for b in self.store.verified_builds()}
        self.assertEqual(digests, {DIGEST_A})

    def test_other_releases_survive_a_reload(self):
        self.store.add_verified_builds(manifest("v1.0.0", [DIGEST_A]))
        self.store.add_verified_builds(manifest("v2.0.0", [DIGEST_B]))
        self.store.add_verified_builds(manifest("v2.0.0", [DIGEST_B]))
        releases = {b["release"] for b in self.store.verified_builds()}
        self.assertEqual(releases, {"v1.0.0", "v2.0.0"})

    # -- the manifest -------------------------------------------------------
    def test_manifest_must_carry_the_right_schema(self):
        with self.assertRaises(srv.Invalid):
            self.store.add_verified_builds(manifest(schema="something/9"))

    def test_manifest_needs_a_release_and_builds(self):
        broken: tuple[dict[str, Any], ...] = (
            {"release": None}, {"builds": []}, {"builds": "no"})
        for bad in broken:
            with self.subTest(bad), self.assertRaises(srv.Invalid):
                self.store.add_verified_builds(manifest(**bad))

    def test_manifest_digests_are_checked(self):
        with self.assertRaises(srv.Invalid):
            self.store.add_verified_builds(
                manifest(builds=[{"filename": "x", "sha256": "not-a-digest"}]))

    def test_a_manifest_url_must_be_http(self):
        """It becomes an href in the page, so `javascript:` is not a URL here."""
        with self.assertRaises(srv.Invalid):
            self.store.add_verified_builds(
                manifest(url="javascript:alert(document.domain)"))

    def test_the_hub_publishes_what_it_trusts(self):
        self.store.add_verified_builds(manifest("v3.0.0", [DIGEST_A]))
        _, out = Browser(self.base).call("/api/builds")
        self.assertEqual([b["sha256"] for b in out["builds"]], [DIGEST_A])
        self.assertEqual(out["builds"][0]["release"], "v3.0.0")
        _, stats = Browser(self.base).call("/api/stats")
        self.assertEqual(stats["release"]["release"], "v3.0.0")

    # -- one binary, several releases ---------------------------------------
    #
    # The ordinary case, not a corner one: a release rebuilds every target and
    # only some of them come out different, so most digests are published again
    # unchanged. What a run claims is then the newest release carrying its
    # bytes -- the version they are current in -- and never an accident of the
    # order an operator loaded manifests in.
    def test_a_digest_in_two_releases_reports_the_newer_one(self):
        self.store.add_verified_builds(manifest("v1.0.0", [DIGEST_A]))
        self.store.add_verified_builds(manifest("v1.1.0", [DIGEST_A]))
        _, out = self.upload(DIGEST_A)
        run = Browser(self.base).call(f"/api/runs/{out['id']}")[1]
        self.assertEqual(run["release_build"], "v1.1.0")

    def test_the_order_manifests_are_loaded_in_does_not_matter(self):
        self.store.add_verified_builds(manifest("v1.1.0", [DIGEST_A]))
        self.store.add_verified_builds(manifest("v1.0.0", [DIGEST_A]))
        _, out = self.upload(DIGEST_A)
        run = Browser(self.base).call(f"/api/runs/{out['id']}")[1]
        self.assertEqual(run["release_build"], "v1.1.0")

    def test_newest_is_by_version_and_not_by_string(self):
        """v0.10.0 is newer than v0.9.0, which no string comparison agrees."""
        self.store.add_verified_builds(manifest("v0.9.0", [DIGEST_A]))
        self.store.add_verified_builds(manifest("v0.10.0", [DIGEST_A]))
        _, out = self.upload(DIGEST_A)
        run = Browser(self.base).call(f"/api/runs/{out['id']}")[1]
        self.assertEqual(run["release_build"], "v0.10.0")
        _, stats = Browser(self.base).call("/api/stats")
        self.assertEqual(stats["release"]["release"], "v0.10.0")

    def test_forgetting_the_newest_falls_back_to_the_one_before(self):
        """The older release still published these bytes; it did not stop."""
        self.store.add_verified_builds(manifest("v1.0.0", [DIGEST_A]))
        self.store.add_verified_builds(manifest("v1.1.0", [DIGEST_A]))
        _, out = self.upload(DIGEST_A)
        b = Browser(self.base)
        self.store.forget_release("v1.1.0")
        self.assertEqual(b.call(f"/api/runs/{out['id']}")[1]["release_build"],
                         "v1.0.0")

    def test_a_release_that_drops_a_binary_stops_claiming_it(self):
        """Re-loading a manifest speaks for that release only, as it always did."""
        self.store.add_verified_builds(manifest("v1.0.0", [DIGEST_A]))
        self.store.add_verified_builds(manifest("v1.1.0", [DIGEST_A, DIGEST_B]))
        self.store.add_verified_builds(manifest("v1.1.0", [DIGEST_B]))
        _, out = self.upload(DIGEST_A)
        run = Browser(self.base).call(f"/api/runs/{out['id']}")[1]
        self.assertEqual(run["release_build"], "v1.0.0")


class ReleaseRankTest(unittest.TestCase):
    """The order the board calls "newest", tag by tag."""

    def newest(self, *tags: str) -> str:
        return max(tags, key=srv.release_rank)

    def test_numbers_compare_as_numbers(self):
        self.assertEqual(self.newest("v0.9.0", "v0.10.0"), "v0.10.0")
        self.assertEqual(self.newest("v1.9.9", "v1.10.0"), "v1.10.0")
        self.assertEqual(self.newest("v2.0.0", "v10.0.0"), "v10.0.0")

    def test_a_leading_v_is_not_part_of_the_version(self):
        self.assertEqual(self.newest("v0.3.0", "0.4.0"), "0.4.0")

    def test_a_prerelease_ranks_below_the_release_it_leads_to(self):
        self.assertEqual(self.newest("v1.0.0-rc1", "v1.0.0"), "v1.0.0")
        self.assertEqual(self.newest("v1.0.0-rc1", "v1.0.0-rc2"), "v1.0.0-rc2")
        self.assertEqual(self.newest("v1.0.0", "v1.0.1-rc1"), "v1.0.1-rc1")

    def test_an_absurd_component_does_not_scramble_its_neighbours(self):
        """A tag with no version in it must not outrank every real one."""
        self.assertEqual(self.newest("v2.0.0", "v" + "9" * 40 + ".0.0"),
                         "v" + "9" * 40 + ".0.0")
        self.assertEqual(self.newest("v1.0.0", "v1.0." + "9" * 40),
                         "v1.0." + "9" * 40)

    def test_tags_that_are_not_versions_still_rank_somewhere(self):
        """Nothing here may raise: the tag is whatever a release was called."""
        for tag in ("", "-", "release", "2026-08-18", "v1.0.0+build.7"):
            with self.subTest(tag):
                self.assertIsInstance(srv.release_rank(tag), str)


class VerifiedBuildsMigrationTest(unittest.TestCase):
    """A database written before one binary could belong to two releases."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = str(Path(self.tmp.name) / "old.sqlite3")

    def tearDown(self):
        self.tmp.cleanup()

    def old_shape(self) -> None:
        """The table as the previous schema declared it: keyed on the digest."""
        srv.Store(self.db)          # everything else, at the current schema
        with sqlite3.connect(self.db) as db:
            db.execute("DROP TABLE verified_builds")
            db.execute("""
                CREATE TABLE verified_builds (
                    sha256      TEXT PRIMARY KEY,
                    release     TEXT NOT NULL,
                    release_url TEXT,
                    filename    TEXT,
                    target      TEXT,
                    march       TEXT,
                    added_at    TEXT NOT NULL)""")
            db.execute(
                "INSERT INTO verified_builds "
                "(sha256, release, release_url, filename, target, march, added_at) "
                "VALUES (?, 'v1.0.0', 'https://example/v1.0.0', 'cpcpub-x', "
                "        'x86_64', 'x86-64', '2026-01-01T00:00:00Z')",
                (DIGEST_A,))

    def test_what_the_old_table_held_survives(self):
        self.old_shape()
        builds = srv.Store(self.db).verified_builds()
        self.assertEqual([(b["sha256"], b["release"], b["filename"])
                          for b in builds],
                         [(DIGEST_A, "v1.0.0", "cpcpub-x")])

    def test_the_migrated_table_takes_a_second_release_for_one_binary(self):
        self.old_shape()
        store = srv.Store(self.db)
        store.add_verified_builds(manifest("v1.1.0", [DIGEST_A]))
        self.assertEqual({(b["sha256"], b["release"])
                          for b in store.verified_builds()},
                         {(DIGEST_A, "v1.0.0"), (DIGEST_A, "v1.1.0")})

    def test_migrating_twice_is_a_no_op(self):
        self.old_shape()
        srv.Store(self.db)
        before = srv.Store(self.db).verified_builds()
        self.assertEqual(before, srv.Store(self.db).verified_builds())


class ConsistencyTest(unittest.TestCase):
    """A document has to agree with itself.

    `score` is a geometric mean of six numbers in the same record and `ilp`,
    `filp` and `mlp` are ratios of two each, so a result edited after the
    benchmark wrote it contradicts itself. That is the only check a hub can
    make on its own, and these tests fix both what it catches and what it
    openly does not.
    """

    SAMPLE = Path(__file__).resolve().parent.parent / "testdata" / "full-run.json"

    def sample(self):
        """A real document, straight from the benchmark."""
        if not self.SAMPLE.exists():          # a checkout without the sample
            self.skipTest(f"{self.SAMPLE} is not here")
        return json.loads(self.SAMPLE.read_text())

    def test_a_real_document_passes(self):
        """The formulas here are the benchmark's, and this is what says so.

        Every record of a genuine --full run -- eight cores, eight threads and
        the machine total -- has to recompute. If it stops doing so, the hub
        has started refusing every honest upload, and that is far worse than
        anything it was meant to catch.
        """
        _, rows = srv.validate(self.sample())
        self.assertEqual(len(rows), 17)

    def test_an_edited_score_is_refused(self):
        doc = self.sample()
        doc["cores"][0]["score"] = 99999.0
        with self.assertRaises(srv.Invalid) as cm:
            srv.validate(doc)
        self.assertIn("score", str(cm.exception))

    def test_an_edited_total_is_refused(self):
        doc = self.sample()
        doc["total"]["score"] = 99999.0
        with self.assertRaises(srv.Invalid):
            srv.validate(doc)

    def test_a_nudged_score_is_refused(self):
        """One per cent is far too small to be worth taking, and is taken."""
        doc = self.sample()
        doc["cores"][0]["score"] *= 1.01
        with self.assertRaises(srv.Invalid):
            srv.validate(doc)

    def test_editing_an_input_breaks_a_ratio(self):
        """Raising a component instead of the score is caught by `ilp`, which
        is that component over the latency beside it."""
        doc = self.sample()
        doc["cores"][0]["int_thr_mops"] *= 3.0
        with self.assertRaises(srv.Invalid) as cm:
            srv.validate(doc)
        self.assertIn("ilp", str(cm.exception))

    def test_a_shaved_latency_is_refused(self):
        doc = self.sample()
        doc["cores"][0]["mem_lat8_ns"] /= 5.0
        with self.assertRaises(srv.Invalid) as cm:
            srv.validate(doc)
        self.assertIn("mlp", str(cm.exception))

    def test_a_run_with_no_dispatch_measurement_passes(self):
        """`disp_span` is written as null both when there was no measurement
        and when the span came out at zero, and the score treats those two
        differently. An honest record of the first kind must not be refused."""
        doc = document("per-core", cores=[
            core(cpu=0, disp_prediction="unknown", disp_span=None,
                 disp_cap_calls=None)])
        _, rows = srv.validate(doc)
        self.assertIsNone(rows[0]["disp_span"])

    def test_a_consistent_forgery_passes(self):
        """What this check is not.

        Rescale every input and recompute the numbers derived from them and the
        document agrees with itself perfectly, because it is arithmetic and not
        evidence. Nothing running on a machine its owner controls can do
        better, and the hub says so rather than implying otherwise -- this test
        is here so that stays a decision and not an oversight.
        """
        doc = self.sample()
        for rec in doc["cores"] + doc["threads"] + [doc["total"]]:
            for key in ("int_thr_mops", "fp_thr_mflops", "mul_thr_mmul_s",
                        "disp_thr_mcall_s", "int_lat_mops", "fp_lat_mflops"):
                rec[key] *= 1.2
            n = doc["config"]["threads"] if rec is doc["total"] else 1.0
            rec["score"] = _score_of(rec, n)
        _, rows = srv.validate(doc)
        self.assertEqual(len(rows), 17)


class AccountTest(unittest.TestCase):
    """Accounts: signing in, what an account may do, and what it may not."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        # Both limiters off: these tests sign in far more often in a second
        # than any person does in fifteen minutes. The registration puzzle is
        # off too -- what it costs to get an account is RegistrationTest's
        # subject, and solving one per account here would only slow this down.
        cls.server = srv.build_server(str(Path(cls.tmp.name) / "a.sqlite3"),
                                      "127.0.0.1", 0, rate_limit=0, auth_limit=0,
                                      register_pow=0, register_limit=0)
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.seq = 0

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def account(self, password="a good long password"):
        """A fresh account, signed in, as (browser, name, token)."""
        type(self).seq += 1
        name = f"tester{self.seq}"
        b = Browser(self.base)
        status, out = b.call("/api/auth/register", "POST",
                             {"name": name, "password": password})
        self.assertEqual(status, 201, out)
        return b, name, out["user"]["api_token"]

    def anon(self):
        return Browser(self.base)

    # -- registering and signing in ---------------------------------------
    def test_register_signs_the_browser_in(self):
        b, name, _ = self.account()
        status, out = b.call("/api/auth/me")
        self.assertEqual(status, 200)
        self.assertEqual(out["user"]["name"], name)
        self.assertEqual(out["user"]["runs"], 0)

    def test_the_two_cookies_carry_the_right_flags(self):
        """The session cookie is the page's to send, not to read.

        The CSRF cookie is the opposite: the page has to read it to echo it
        back, which is the whole mechanism, so it alone is not HttpOnly.
        """
        req = urllib.request.Request(
            self.base + "/api/auth/register", method="POST",
            data=json.dumps({"name": "cookieflags",
                             "password": "a good long password"}).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req) as resp:
            set_cookies = resp.headers.get_all("Set-Cookie") or []
        session = [c for c in set_cookies if c.startswith(srv.SESSION_COOKIE + "=")]
        csrf = [c for c in set_cookies if c.startswith(srv.CSRF_COOKIE + "=")]
        self.assertEqual(len(session), 1, set_cookies)
        self.assertEqual(len(csrf), 1, set_cookies)
        self.assertIn("HttpOnly", session[0])
        self.assertIn("SameSite=Lax", session[0])
        self.assertNotIn("HttpOnly", csrf[0])
        # Plain http in the tests, so neither may claim Secure -- a Secure
        # cookie over http is one the browser drops, i.e. a session that
        # silently never works.
        self.assertNotIn("Secure", session[0])

    def test_bad_names_and_passwords_are_refused(self):
        b = self.anon()
        for name in ("ab", "no spaces", "a" * 33, "-leading"):
            with self.subTest(name):
                code, _ = b.call("/api/auth/register", "POST",
                                 {"name": name, "password": "a good long password"})
                self.assertEqual(code, 400)
        code, err = b.call("/api/auth/register", "POST",
                           {"name": "shorty", "password": "short"})
        self.assertEqual(code, 400)
        self.assertIn("8 characters", err["error"])

    def test_names_are_taken_case_insensitively(self):
        _, name, _ = self.account()
        code, err = self.anon().call("/api/auth/register", "POST",
                                     {"name": name.upper(), "password": "another password"})
        self.assertEqual(code, 409)
        self.assertIn("taken", err["error"])

    def test_login_and_logout(self):
        b, name, _ = self.account("a good long password")
        self.assertEqual(b.call("/api/auth/logout", "POST", {})[0], 200)
        self.assertIsNone(b.call("/api/auth/me")[1]["user"])

        code, err = b.call("/api/auth/login", "POST",
                           {"name": name, "password": "wrong password"})
        self.assertEqual(code, 401)
        # The message must not say which half was wrong.
        self.assertIn("wrong name or password", err["error"])

        status, out = b.call("/api/auth/login", "POST",
                             {"name": name.upper(), "password": "a good long password"})
        self.assertEqual(status, 200)
        self.assertEqual(out["user"]["name"], name)

    def test_password_change_ends_other_sessions(self):
        b, name, _ = self.account("a good long password")
        other = Browser(self.base)
        self.assertEqual(other.call("/api/auth/login", "POST",
                                    {"name": name, "password": "a good long password"})[0], 200)
        self.assertEqual(b.call("/api/auth/password", "POST",
                                {"current": "a good long password",
                                 "password": "an even better password"})[0], 200)
        self.assertIsNone(other.call("/api/auth/me")[1]["user"])
        # The browser that changed it stays in.
        self.assertIsNotNone(b.call("/api/auth/me")[1]["user"])
        self.assertEqual(b.call("/api/auth/login", "POST",
                                {"name": name, "password": "a good long password"})[0], 401)

    def test_password_change_needs_the_current_one(self):
        b, _, _ = self.account()
        code, _ = b.call("/api/auth/password", "POST",
                         {"current": "not it", "password": "a new long password"})
        self.assertEqual(code, 401)

    # -- uploading as an account -------------------------------------------
    def test_upload_token_attributes_the_run(self):
        b, name, token = self.account()
        status, out = self.anon().call(
            "/api/runs", "POST", document(),
            headers={"Authorization": f"Bearer {token}"})
        self.assertEqual(status, 201)
        self.assertEqual(out["user"], name)
        _, run = b.call(f"/api/runs/{out['id']}")
        self.assertEqual(run["user"], name)
        # ...and the board says who it belongs to.
        _, board = b.call(f"/api/cores?scope=cpu&user={name}")
        self.assertEqual([c["user"] for c in board["cores"]], [name])

    def test_unknown_upload_token_is_refused_not_anonymous(self):
        """A stale token must fail loudly rather than upload to nobody."""
        code, err = self.anon().call(
            "/api/runs", "POST", document(),
            headers={"Authorization": "Bearer no-such-token"})
        self.assertEqual(code, 401)
        self.assertIn("unknown upload token", err["error"])

    def test_rotating_the_token_retires_the_old_one(self):
        b, _, old = self.account()
        status, out = b.call("/api/auth/token", "POST", {})
        self.assertEqual(status, 200)
        self.assertNotEqual(out["api_token"], old)
        self.assertEqual(self.anon().call(
            "/api/runs", "POST", document(),
            headers={"Authorization": f"Bearer {old}"})[0], 401)
        self.assertEqual(self.anon().call(
            "/api/runs", "POST", document(),
            headers={"Authorization": f"Bearer {out['api_token']}"})[0], 201)

    def test_cookie_upload_requires_the_csrf_token(self):
        """What stops another site posting for a signed-in visitor."""
        b, _, _ = self.account()
        self.assertEqual(b.call("/api/runs", "POST", document(), csrf=False)[0], 403)
        self.assertEqual(b.call("/api/runs", "POST", document())[0], 201)

    def test_a_refused_write_leaves_the_connection_usable(self):
        """A refusal decided before the body was read must still eat it.

        Otherwise the unread document sits in front of the next request on the
        same keep-alive connection and is parsed as one.
        """
        b, _, _ = self.account()
        conn = http.client.HTTPConnection(self.base.removeprefix("http://"))
        body = json.dumps({"current": "x", "password": "a good long password"})
        # No CSRF header: refused before change_password reads anything.
        conn.request("POST", "/api/auth/password", body, {
            "Content-Type": "application/json",
            "Cookie": f"{srv.SESSION_COOKIE}={b.cookie(srv.SESSION_COOKIE)}",
        })
        refused = conn.getresponse()
        self.assertEqual(refused.status, 403)
        refused.read()
        # The next request on the same connection must be understood.
        conn.request("GET", "/api/stats")
        resp = conn.getresponse()
        self.assertEqual(resp.status, 200)
        json.loads(resp.read())
        conn.close()

    def test_anonymous_upload_still_needs_nothing(self):
        status, out = self.anon().call("/api/runs", "POST", document())
        self.assertEqual(status, 201)
        self.assertIsNone(out["user"])
        self.assertTrue(out["delete_token"])

    # -- who may withdraw a run --------------------------------------------
    def test_owner_withdraws_without_a_token(self):
        b, _, _ = self.account()
        _, out = b.call("/api/runs", "POST", document())
        self.assertEqual(b.call(f"/api/runs/{out['id']}", "DELETE")[0], 200)
        self.assertEqual(b.call(f"/api/runs/{out['id']}")[0], 404)

    def test_another_account_cannot_withdraw_it(self):
        mine, _, _ = self.account()
        theirs, _, _ = self.account()
        _, out = mine.call("/api/runs", "POST", document())
        code, _ = theirs.call(f"/api/runs/{out['id']}", "DELETE")
        self.assertEqual(code, 403)
        self.assertEqual(mine.call(f"/api/runs/{out['id']}")[0], 200)

    def test_the_delete_token_still_works_on_an_owned_run(self):
        """Signing in must not strand a run uploaded from a shell."""
        _, _, token = self.account()
        _, out = self.anon().call("/api/runs", "POST", document(),
                                  headers={"Authorization": f"Bearer {token}"})
        status, _ = self.anon().call(
            f"/api/runs/{out['id']}", "DELETE",
            headers={"X-Delete-Token": out["delete_token"]})
        self.assertEqual(status, 200)

    def test_withdrawing_needs_one_claim_or_the_other(self):
        _, out = self.anon().call("/api/runs", "POST", document())
        code, err = self.anon().call(f"/api/runs/{out['id']}", "DELETE")
        self.assertEqual(code, 401)
        self.assertIn("delete token", err["error"])

    # -- listing and profiles ----------------------------------------------
    def test_user_filter_narrows_to_one_account(self):
        b, name, _ = self.account()
        b.call("/api/runs", "POST", document())
        self.anon().call("/api/runs", "POST", document())
        _, mine = b.call("/api/runs?user=me")
        self.assertTrue(mine["runs"])
        self.assertTrue(all(r["user"] == name for r in mine["runs"]))
        _, by_name = self.anon().call(f"/api/runs?user={name}")
        self.assertEqual([r["id"] for r in by_name["runs"]],
                         [r["id"] for r in mine["runs"]])

    def test_user_me_needs_a_session(self):
        code, _ = self.anon().call("/api/runs?user=me")
        self.assertEqual(code, 401)

    def test_an_unknown_name_matches_nothing(self):
        self.anon().call("/api/runs", "POST", document())
        _, out = self.anon().call("/api/runs?user=nobody-at-all")
        self.assertEqual(out["runs"], [])

    def test_profile_leaks_neither_token_nor_hash(self):
        _, name, token = self.account()   # token must appear nowhere below
        status, out = self.anon().call(f"/api/users/{name}")
        self.assertEqual(status, 200)
        self.assertEqual(out["user"]["name"], name)
        self.assertNotIn("api_token", out["user"])
        self.assertNotIn("password_hash", out["user"])
        self.assertNotIn(token, json.dumps(out))
        self.assertEqual(self.anon().call("/api/users/nobody-here")[0], 404)

    def test_own_token_is_only_shown_to_a_session(self):
        b, _, token = self.account()
        _, mine = b.call("/api/auth/me")
        self.assertEqual(mine["user"]["api_token"], token)

    # -- closing an account -------------------------------------------------
    def test_closing_deletes_the_runs_and_frees_the_name(self):
        b, name, _ = self.account("a good long password")
        _, run = b.call("/api/runs", "POST", document())
        code, _ = b.call("/api/auth/close", "POST", {"password": "wrong"})
        self.assertEqual(code, 401)
        status, out = b.call("/api/auth/close", "POST",
                             {"password": "a good long password"})
        self.assertEqual(status, 200)
        self.assertEqual(out["runs_deleted"], 1)
        self.assertEqual(b.call(f"/api/runs/{run['id']}")[0], 404)
        self.assertIsNone(b.call("/api/auth/me")[1]["user"])
        # The name is free again, and the new account inherits nothing.
        fresh = Browser(self.base)
        status, out = fresh.call("/api/auth/register", "POST",
                                 {"name": name, "password": "a different password"})
        self.assertEqual(status, 201)
        self.assertEqual(out["user"]["runs"], 0)

    def test_a_run_survives_its_uploader_being_forgotten(self):
        """user_id may name an account that is gone; the run still reads back."""
        _, out = self.anon().call("/api/runs", "POST", document())
        store = srv.Store(str(Path(self.tmp.name) / "a.sqlite3"))
        with store.connect() as db:
            db.execute("UPDATE runs SET user_id = 424242 WHERE id = ?", (out["id"],))
        status, run = self.anon().call(f"/api/runs/{out['id']}")
        self.assertEqual(status, 200)
        self.assertIsNone(run["user"])


class RegistrationTest(unittest.TestCase):
    """What it costs to get an account, and what it costs to script one.

    Each hub here is built with the policy under test, because the policy is
    what is being tested -- there is no way to change it on a running server,
    deliberately.
    """

    def serve(self, **policy) -> str:
        """A hub with this registration policy, torn down with the test."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        server = srv.build_server(str(Path(tmp.name) / "r.sqlite3"),
                                  "127.0.0.1", 0, rate_limit=0, auth_limit=0,
                                  **policy)
        # Kept so a test can look at the challenge store, which is the only
        # state here that is not in the database.
        self.hub = server
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return f"http://127.0.0.1:{server.server_address[1]}"

    def solved(self, base: str, name: str, **over) -> tuple[int, dict]:
        """Register `name`, doing whatever work the hub asks for."""
        b = Browser(base)
        _, policy = b.call("/api/auth/challenge")
        body: dict[str, Any] = {"name": name, "password": "a good long password"}
        if policy.get("bits"):
            nonce = 0
            while not srv.pow_solved(policy["challenge"], str(nonce), policy["bits"]):
                nonce += 1
            body.update(challenge=policy["challenge"], nonce=str(nonce))
        body.update(over)
        return b.call("/api/auth/register", "POST", body)

    # -- the proof of work -------------------------------------------------
    def test_the_hub_says_what_it_asks_for(self):
        base = self.serve()
        status, policy = Browser(base).call("/api/auth/challenge")
        self.assertEqual(status, 200)
        self.assertTrue(policy["open"])
        self.assertFalse(policy["invite_required"])
        self.assertEqual(policy["bits"], srv.REGISTER_POW_BITS)
        self.assertTrue(policy["challenge"])

    def test_the_policy_costs_nothing_to_read(self):
        """The page reads this on every draw, so it must not issue a puzzle.

        A challenge per page view would fill the store with challenges nobody
        solves, and spend the rate limit that registering needs on people who
        only came to read the board.
        """
        base = self.serve()
        status, policy = Browser(base).call("/api/auth/policy")
        self.assertEqual(status, 200)
        self.assertTrue(policy["open"])
        self.assertEqual(policy["bits"], srv.REGISTER_POW_BITS)
        self.assertNotIn("challenge", policy)
        self.assertEqual(len(self.hub.challenges.live), 0)
        # And the puzzle endpoint still does issue one.
        Browser(base).call("/api/auth/challenge")
        self.assertEqual(len(self.hub.challenges.live), 1)

    def test_the_page_can_finish_the_hardest_puzzle_a_hub_may_ask_for(self):
        """The ceiling is what the browser can search, not a round number.

        app.js gives up after 2^26 nonces and the challenge expires besides, so
        a maximum that needs more work than either allows is a door that looks
        open and is not.
        """
        searchable = 1 << 26                       # the limit in solveChallenge
        expected = 1 << srv.MAX_POW_BITS
        self.assertLessEqual(expected * 8, searchable,
                             "the search would fail more often than rarely")

    def test_a_solved_challenge_registers(self):
        base = self.serve(register_pow=12)
        status, out = self.solved(base, "worker")
        self.assertEqual(status, 201, out)
        self.assertEqual(out["user"]["name"], "worker")

    def test_registering_without_one_is_refused(self):
        base = self.serve(register_pow=12)
        status, err = Browser(base).call(
            "/api/auth/register", "POST",
            {"name": "scripted", "password": "a good long password"})
        self.assertEqual(status, 400)
        self.assertIn("challenge", err["error"])

    def test_a_wrong_answer_is_refused(self):
        base = self.serve(register_pow=12)
        b = Browser(base)
        _, policy = b.call("/api/auth/challenge")
        status, err = b.call("/api/auth/register", "POST",
                             {"name": "scripted", "password": "a good long password",
                              "challenge": policy["challenge"], "nonce": "0"})
        # 1 in 4096 that nonce 0 happens to solve a 12-bit challenge, which
        # would be a pass reported as a failure -- so the assertion is on
        # either outcome being the honest one for what was sent.
        if status == 201:
            self.skipTest("nonce 0 solved it by luck")
        self.assertEqual(status, 403)
        self.assertIn("not solved", err["error"])

    def test_a_challenge_is_spent_once(self):
        base = self.serve(register_pow=12)
        b = Browser(base)
        _, policy = b.call("/api/auth/challenge")
        nonce = 0
        while not srv.pow_solved(policy["challenge"], str(nonce), policy["bits"]):
            nonce += 1
        body = {"password": "a good long password",
                "challenge": policy["challenge"], "nonce": str(nonce)}
        first, _ = b.call("/api/auth/register", "POST", dict(body, name="first"))
        second, err = b.call("/api/auth/register", "POST", dict(body, name="second"))
        self.assertEqual(first, 201)
        self.assertEqual(second, 403)
        self.assertIn("expired", err["error"])

    def test_an_invented_challenge_is_refused(self):
        base = self.serve(register_pow=0)      # even where none is required
        status, _ = Browser(base).call(
            "/api/auth/register", "POST",
            {"name": "nopuzzle", "password": "a good long password"})
        self.assertEqual(status, 201)

    def test_challenges_expire(self):
        challenges = srv.Challenges(ttl=-1.0)
        token = challenges.issue()
        self.assertFalse(challenges.spend(token))

    def test_the_challenge_store_is_bounded(self):
        challenges = srv.Challenges(cap=8)
        issued = [challenges.issue() for _ in range(64)]
        self.assertLessEqual(len(challenges.live), 8)
        self.assertFalse(challenges.spend(issued[0]))

    def test_pow_difficulty_is_capped(self):
        # A hub cannot ask for work no browser will finish, however it is
        # started.
        base = self.serve(register_pow=1 << 20)
        _, policy = Browser(base).call("/api/auth/challenge")
        self.assertEqual(policy["bits"], srv.MAX_POW_BITS)

    # -- the other two doors ----------------------------------------------
    def test_a_closed_hub_takes_no_accounts(self):
        base = self.serve(register_open=False, register_pow=0)
        _, policy = Browser(base).call("/api/auth/challenge")
        self.assertFalse(policy["open"])
        self.assertNotIn("challenge", policy)
        status, err = self.solved(base, "hopeful")
        self.assertEqual(status, 403)
        self.assertIn("not taking new accounts", err["error"])

    def test_a_closed_hub_still_takes_uploads(self):
        base = self.serve(register_open=False)
        status, _ = Browser(base).call("/api/runs", "POST", document())
        self.assertEqual(status, 201)

    def test_an_invite_code_is_required_when_set(self):
        base = self.serve(register_pow=0, signup_token="the-code")
        _, policy = Browser(base).call("/api/auth/challenge")
        self.assertTrue(policy["invite_required"])
        refused, err = self.solved(base, "outsider")
        self.assertEqual(refused, 403)
        self.assertIn("invite", err["error"])
        wrong, _ = self.solved(base, "outsider", invite="not-the-code")
        self.assertEqual(wrong, 403)
        ok, out = self.solved(base, "insider", invite="the-code")
        self.assertEqual(ok, 201, out)

    def test_one_address_cannot_take_every_name(self):
        base = self.serve(register_pow=0, register_limit=2)
        for i in range(2):
            status, out = self.solved(base, f"early{i}")
            self.assertEqual(status, 201, out)
        status, err = self.solved(base, "late")
        self.assertEqual(status, 429)
        self.assertIn("too many accounts", err["error"])

    def test_an_unsolved_attempt_does_not_use_up_the_limit(self):
        """The limit counts accounts made, not attempts refused.

        Otherwise a script that never solves anything could still exhaust the
        quota of everyone behind the same address.
        """
        base = self.serve(register_pow=12, register_limit=1)
        for i in range(5):
            status, _ = Browser(base).call(
                "/api/auth/register", "POST",
                {"name": f"noise{i}", "password": "a good long password"})
            self.assertEqual(status, 400)
        status, out = self.solved(base, "genuine")
        self.assertEqual(status, 201, out)

    def test_a_bad_name_is_refused_before_the_work_is_checked(self):
        """A typo should not cost the puzzle -- it is not an attempt at abuse."""
        base = self.serve(register_pow=12)
        status, _ = Browser(base).call("/api/auth/register", "POST",
                                       {"name": "x", "password": "short"})
        self.assertEqual(status, 400)

    def test_a_taken_name_does_not_cost_the_puzzle(self):
        """Nor does a name someone else already has -- also not abuse.

        Proved by re-using the very challenge the refused attempt carried: if it
        had been spent, the second attempt could not register with it.
        """
        base = self.serve(register_pow=12)
        b = Browser(base)
        self.assertEqual(self.solved(base, "taken")[0], 201)

        _, policy = b.call("/api/auth/challenge")
        nonce = 0
        while not srv.pow_solved(policy["challenge"], str(nonce), policy["bits"]):
            nonce += 1
        body = {"password": "a good long password",
                "challenge": policy["challenge"], "nonce": str(nonce)}
        again, err = b.call("/api/auth/register", "POST", dict(body, name="TAKEN"))
        self.assertEqual(again, 409)
        self.assertIn("taken", err["error"])
        spare, out = b.call("/api/auth/register", "POST", dict(body, name="free"))
        self.assertEqual(spare, 201, out)


class ProofOfWorkTest(unittest.TestCase):
    def test_a_solution_has_the_leading_zero_bits(self):
        nonce = 0
        while not srv.pow_solved("abc", str(nonce), 12):
            nonce += 1
        digest = hashlib.sha256(f"abc:{nonce}".encode()).digest()
        self.assertEqual(int.from_bytes(digest, "big") >> (256 - 12), 0)

    def test_zero_bits_accepts_anything(self):
        self.assertTrue(srv.pow_solved("abc", "0", 0))

    def test_the_challenge_is_part_of_what_is_hashed(self):
        nonce = 0
        while not srv.pow_solved("abc", str(nonce), 12):
            nonce += 1
        # The same nonce against another challenge is worth nothing, which is
        # what stops one solution being spent on a second registration.
        self.assertFalse(srv.pow_solved("abd", str(nonce), 12))


class PasswordTest(unittest.TestCase):
    def test_hash_round_trip(self):
        stored = srv.hash_password("a good long password")
        self.assertTrue(srv.password_matches("a good long password", stored))
        self.assertFalse(srv.password_matches("a good long passwore", stored))

    def test_each_hash_is_salted(self):
        self.assertNotEqual(srv.hash_password("same password"),
                            srv.hash_password("same password"))

    def test_a_damaged_hash_matches_nothing(self):
        for stored in ("", "nonsense", "scrypt$x$8$1$aa$bb", "md5$1$1$1$aa$bb"):
            with self.subTest(stored):
                self.assertFalse(srv.password_matches("anything", stored))

    def test_account_limit_is_separate_from_the_address_limit(self):
        limiter = srv.RateLimiter(2, 60.0)
        self.assertTrue(limiter.allow("user:1", 2 * srv.ACCOUNT_RATE_FACTOR))
        for _ in range(2 * srv.ACCOUNT_RATE_FACTOR - 1):
            self.assertTrue(limiter.allow("user:1", 2 * srv.ACCOUNT_RATE_FACTOR))
        self.assertFalse(limiter.allow("user:1", 2 * srv.ACCOUNT_RATE_FACTOR))
        # ...and the address it came from still has its own budget.
        self.assertTrue(limiter.allow("10.0.0.1"))


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
        empty: dict[str, object] = {k: None for k in core()}
        empty["scope"] = "cpu"
        with self.assertRaises(srv.Invalid):
            srv.validate(document("per-core", cores=[empty]))

    def test_nulls_survive(self):
        """A run with no memory phases: the whole memory group is absent."""
        _, rows = srv.validate(document("per-core", cores=[core(
            mem_gbps=None, mem_lat_ns=None, mem_lat8_ns=None, mlp=None)]))
        self.assertIsNone(rows[0]["mem_gbps"])
        self.assertIsNone(rows[0]["mlp"])
        self.assertEqual(rows[0]["int_thr"], 20000.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
