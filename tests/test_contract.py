#!/usr/bin/env python3
"""The contract between the benchmark and the results hub.

    make check                          # against the binary just built
    python3 tests/test_contract.py      # against testdata/ alone

The two halves of this repo share no code. They meet at exactly one place: the
JSON document `cpu-bench --json` writes and the hub ingests. So nothing but this
test notices when one side grows a field the other never reads -- and the hub's
own tests cannot notice, because their fixture is hand-written and would go on
agreeing with itself forever.

Runs against the committed sample in testdata/, and additionally against a live
run of $CPU_BENCH_BIN when that is set (which is what `make check` does, so the
binary under test is the one just built rather than whatever produced the
sample).

The key sets below are declared, not derived -- the hub reads its fields by
name, so there is nothing to introspect. That is the point: a field added to
either side fails this test until someone writes it down here and in
schema/cpu-bench-1.md, which is the moment to notice the other side needs it
too.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "web"))

import server as srv                                        # noqa: E402

SAMPLE = ROOT / "testdata" / "full-run.json"

# Top-level keys the benchmark may emit, split by whether the hub reads them.
CONSUMED_TOP = {"schema", "mode", "build", "system", "config", "dram",
                "cores", "threads", "total", "checksum"}
# Emitted and deliberately dropped on ingest. `core_spread` is a summary of the
# per-core array, recomputable from the records the hub already stores.
UNCONSUMED_TOP = {"core_spread"}

# Keys within each sub-object. Everything here is optional in a given document:
# `system` is only as complete as uname(2) was, `dram` appears only on machines
# exposing a DRAM devfreq node.
SECTION_KEYS = {
    "build":  {"compiler", "compiler_version", "cc", "flags", "target",
               "vectorize", "fma"},
    "system": {"sysname", "release", "machine", "cpus", "cpu_models"},
    "config": {"threads", "seconds_per_phase", "reps", "warmup_seconds",
               "mem_bytes_per_thread", "pin", "clock", "seed"},
    "dram":   {"name", "mhz_min", "mhz_max"},
}

# Sub-object key -> the runs column it lands in, for the round-trip check.
SECTION_COLUMNS = {
    ("build", "compiler"):            "compiler",
    ("build", "compiler_version"):    "compiler_version",
    ("build", "cc"):                  "cc",
    ("build", "flags"):               "build_flags",
    ("build", "target"):              "target",
    ("build", "vectorize"):           "vectorize",
    ("build", "fma"):                 "fma",
    ("system", "sysname"):            "sysname",
    ("system", "release"):            "os_release",
    ("system", "machine"):            "machine",
    ("system", "cpus"):               "cpus",
    ("system", "cpu_models"):         "cpu_models",
    ("config", "threads"):            "threads",
    ("config", "seconds_per_phase"):  "seconds",
    ("config", "reps"):               "reps",
    ("config", "warmup_seconds"):     "warmup",
    ("config", "mem_bytes_per_thread"): "mem_bytes",
    ("config", "pin"):                "pin",
    ("config", "clock"):              "clock",
    ("config", "seed"):               "seed",
    ("dram", "name"):                 "dram_name",
    ("dram", "mhz_min"):              "dram_mhz_min",
    ("dram", "mhz_max"):              "dram_mhz_max",
}

# A short but complete run: both halves of --full, one CPU, so `make check`
# costs a second or two rather than sweeping every core on the machine.
LIVE_ARGS = ["--full", "--cpus", "0", "--threads", "1",
             "--time", "0.05", "--reps", "1", "--warmup", "0.02", "--json"]


def documents():
    """Every cpu-bench document this run can test, as (name, doc)."""
    out = []
    if SAMPLE.exists():
        out.append(("testdata/full-run.json", json.loads(SAMPLE.read_text())))
    binary = os.environ.get("CPU_BENCH_BIN")
    if binary:
        proc = subprocess.run([binary] + LIVE_ARGS, check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        out.append((f"live: {Path(binary).name}", json.loads(proc.stdout)))
    return out


DOCUMENTS = documents()


def records_of(doc):
    """Result records in the order validate() flattens them."""
    out = [("cpu", r) for r in doc.get("cores") or []]
    out += [("thread", r) for r in doc.get("threads") or []]
    if doc.get("total") is not None:
        out.append(("total", doc["total"]))
    return out


class ContractTest(unittest.TestCase):
    """Checks that hold for every document the benchmark can produce."""

    def setUp(self):
        if not DOCUMENTS:
            self.skipTest("no testdata/full-run.json and no $CPU_BENCH_BIN; "
                          "run `make testdata` or `make check`")

    def test_schema_id(self):
        for name, doc in DOCUMENTS:
            with self.subTest(name):
                self.assertEqual(doc.get("schema"), srv.SCHEMA_ID)

    def test_mode_is_accepted(self):
        for name, doc in DOCUMENTS:
            with self.subTest(name):
                self.assertIn(doc.get("mode"), srv.MODES)

    def test_top_level_keys_are_classified(self):
        self.assertEqual(CONSUMED_TOP & UNCONSUMED_TOP, set(),
                         "a key cannot be both read and dropped")
        for name, doc in DOCUMENTS:
            with self.subTest(name):
                unknown = set(doc) - CONSUMED_TOP - UNCONSUMED_TOP
                self.assertEqual(unknown, set(),
                                 "the benchmark emits a top-level key this "
                                 "contract does not describe; add it to "
                                 "CONSUMED_TOP (and to the hub) or to "
                                 "UNCONSUMED_TOP")

    def test_section_keys_are_classified(self):
        for name, doc in DOCUMENTS:
            for section, allowed in SECTION_KEYS.items():
                if section not in doc:
                    continue
                with self.subTest(f"{name}: {section}"):
                    self.assertEqual(set(doc[section]) - allowed, set(),
                                     f"unknown key in '{section}'")

    def test_records_carry_exactly_the_ingested_fields(self):
        """Neither side may add or rename a record field alone.

        A field the benchmark emits and CORE_FIELDS omits is measured, uploaded
        and silently dropped; a field CORE_FIELDS names and the benchmark no
        longer emits stores NULL for every future upload. Both are invisible
        without this.
        """
        expected = {jkey for jkey, _, _ in srv.CORE_FIELDS} | {"scope"}
        for name, doc in DOCUMENTS:
            for i, (scope, rec) in enumerate(records_of(doc)):
                with self.subTest(f"{name}: {scope}[{i}]"):
                    self.assertEqual(set(rec), expected)

    def test_every_metric_has_a_column(self):
        """/api/metrics may only advertise metrics the leaderboard can sort on."""
        self.assertEqual(set(srv.METRIC_KEYS) - set(srv.CORE_COLUMNS), set(),
                         "METRICS names a key with no cores column")

    def test_the_headline_set_is_small_and_led_by_the_score(self):
        """The columns a table shows before anyone asks for more.

        The front end draws these plus whatever it is sorted on, so a headline
        flag added without thought is a column added to every table at once.
        """
        headline = [m["key"] for m in srv.METRICS if m.get("headline")]
        self.assertEqual(headline[0], "score", "the geomean leads")
        self.assertLessEqual(len(headline), 4, "this is a summary, not a dump")
        self.assertTrue(all(m["better"] != "none" for m in srv.METRICS
                            if m.get("headline")),
                        "a metric that is not a ranking cannot be a headline")

    def test_declared_columns_exist_in_the_database(self):
        with tempfile.TemporaryDirectory() as d:
            store = srv.Store(str(Path(d) / "t.sqlite3"))
            with store.connect() as db:
                runs = {r["name"] for r in db.execute("PRAGMA table_info(runs)")}
                cores = {r["name"] for r in db.execute("PRAGMA table_info(cores)")}
        self.assertEqual(set(srv.RUN_COLUMNS) - runs, set())
        self.assertEqual(set(srv.CORE_COLUMNS) - cores, set())
        # Columns insert() writes that RUN_COLUMNS does not read back.
        self.assertEqual({"delete_token", "raw"} - runs, set())

    def test_document_survives_a_round_trip(self):
        """Every value the benchmark measured comes back out of the database."""
        for name, doc in DOCUMENTS:
            with self.subTest(name):
                run, rows = srv.validate(doc)
                with tempfile.TemporaryDirectory() as d:
                    store = srv.Store(str(Path(d) / "t.sqlite3"))
                    run_id, _ = store.insert(run, rows, json.dumps(doc),
                                             "contract", None)
                    got = store.run(run_id)
                    raw = store.raw(run_id)

                self.assertIsNotNone(got)
                self.assertEqual(json.loads(raw), doc,
                                 "the verbatim copy must be the document that "
                                 "was uploaded, not the flattened rows")

                for (section, key), col in SECTION_COLUMNS.items():
                    value = (doc.get(section) or {}).get(key)
                    if value is None:
                        continue
                    if isinstance(value, bool):
                        value = int(value)
                    with self.subTest(f"{name}: {section}.{key}"):
                        self.assertEqual(got[col], value)

                emitted = records_of(doc)
                self.assertEqual(len(got["cores"]), len(emitted))
                for (scope, rec), stored in zip(emitted, got["cores"]):
                    self.assertEqual(stored["scope"], rec.get("scope", scope))
                    for jkey, col, _kind in srv.CORE_FIELDS:
                        with self.subTest(f"{name}: {scope}.{jkey}"):
                            self.assertEqual(stored[col], rec[jkey])

    def test_a_disp_sweep_dump_is_refused(self):
        """The diagnostic dump shares the schema id but carries no metrics."""
        for name, doc in DOCUMENTS:
            with self.subTest(name):
                sweep = dict(doc, mode="disp-sweep")
                with self.assertRaises(srv.Invalid):
                    srv.validate(sweep)


class SampleTest(unittest.TestCase):
    """Checks specific to the committed fixture."""

    def test_sample_exists(self):
        self.assertTrue(SAMPLE.exists(),
                        "testdata/full-run.json is missing; `make testdata`")

    def test_sample_is_a_full_run(self):
        if not SAMPLE.exists():
            self.skipTest("no sample")
        doc = json.loads(SAMPLE.read_text())
        self.assertEqual(doc["mode"], "full")
        self.assertTrue(doc.get("cores"), "a full run carries per-core records")
        self.assertTrue(doc.get("threads"), "a full run carries thread records")

    def test_sample_is_within_the_upload_limits(self):
        if not SAMPLE.exists():
            self.skipTest("no sample")
        self.assertLess(SAMPLE.stat().st_size, srv.MAX_BODY)

    def test_sample_carries_no_local_paths(self):
        """`make testdata` commits real output, and the flags are baked in.

        `build.flags` is whatever CFLAGS the binary was compiled with, verbatim.
        A cross-compile (`--sysroot=/home/you/toolchain`) or a local include
        path therefore ends up in the committed sample -- and in every result
        uploaded to a public hub. Catch it here rather than in a push.
        """
        if not SAMPLE.exists():
            self.skipTest("no sample")
        text = SAMPLE.read_text()
        for pattern in ("/home/", "/Users/", "/root/", "/tmp/", "C:\\\\"):
            with self.subTest(pattern):
                self.assertNotIn(pattern, text,
                                 f"{pattern!r} in testdata/full-run.json -- "
                                 "rebuild with clean CFLAGS and re-run "
                                 "`make testdata`")


if __name__ == "__main__":
    for name, _ in DOCUMENTS:
        print(f"contract: testing {name}", file=sys.stderr)
    unittest.main(verbosity=2)
