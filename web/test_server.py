#!/usr/bin/env python3
"""Tests for the cpu-bench results hub.

    python3 web/test_server.py

Runs a real server on an ephemeral port against a temporary database, so the
HTTP layer, the validation and the SQL are all exercised together.
"""

import http.client
import http.cookiejar
import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

import server as srv


def core(scope="cpu", cpu: int | None = 0, **over):
    rec: dict[str, object] = {
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
            return [c for c in b["cores"] if c["run_id"] == out["id"]][0]

        best = row("score", "desc")
        self.assertEqual(best["cpu"], 1)
        # Every column comes from that one core, so the row is a real
        # measurement rather than a per-metric best of several.
        self.assertEqual(best["mem_lat_ns"], 200.0)
        quickest = row("mem_lat_ns", "asc")
        self.assertEqual(quickest["cpu"], 2)
        self.assertEqual(quickest["score"], 5000.0)

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


class AccountTest(unittest.TestCase):
    """Accounts: signing in, what an account may do, and what it may not."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        # Both limiters off: these tests sign in far more often in a second
        # than any person does in fifteen minutes.
        cls.server = srv.build_server(str(Path(cls.tmp.name) / "a.sqlite3"),
                                      "127.0.0.1", 0, rate_limit=0, auth_limit=0)
        cls.base = "http://127.0.0.1:%d" % cls.server.server_address[1]
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
        _, rows = srv.validate(document("per-core",
                                        cores=[core(mem_gbps=None, mlp=None)]))
        self.assertIsNone(rows[0]["mem_gbps"])
        self.assertEqual(rows[0]["int_thr"], 20000.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
