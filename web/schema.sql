-- cpu-bench results hub.
--
-- Two tables: one row per uploaded run, one row per core/thread record inside
-- it. The records are flattened out of the JSON so the leaderboard can sort and
-- filter in SQL; the original document is kept verbatim in runs.raw so nothing
-- an upload contained is ever lost to the flattening.

CREATE TABLE IF NOT EXISTS runs (
    id            INTEGER PRIMARY KEY,
    created_at    TEXT    NOT NULL,
    delete_token  TEXT    NOT NULL,
    label         TEXT,
    notes         TEXT,

    mode          TEXT    NOT NULL,   -- threads | per-core | full
    compiler      TEXT,               -- "gcc 15.2.0"
    compiler_version TEXT,            -- the toolchain's own version banner
    cc            TEXT,               -- driver invoked, e.g. riscv64-linux-gnu-gcc
    build_flags   TEXT,               -- the exact CFLAGS the binary was built with
    target        TEXT,               -- x86_64 | aarch64 | riscv64
    vectorize     INTEGER,            -- build flags: results are only
    fma           INTEGER,            -- comparable when these two match

    sysname       TEXT,
    os_release    TEXT,
    machine       TEXT,
    cpus          INTEGER,
    cpu_models    TEXT,

    threads       INTEGER,
    seconds       REAL,
    reps          INTEGER,
    warmup        REAL,
    mem_bytes     INTEGER,
    pin           INTEGER,
    clock         TEXT,
    seed          INTEGER,

    dram_name     TEXT,
    dram_mhz_min  REAL,
    dram_mhz_max  REAL,

    checksum      TEXT,
    raw           TEXT    NOT NULL
);

CREATE TABLE IF NOT EXISTS cores (
    id              INTEGER PRIMARY KEY,
    run_id          INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    scope           TEXT    NOT NULL,   -- cpu | thread | total
    cpu             INTEGER,
    mhz             REAL,
    mhz_src         TEXT,

    int_lat         REAL,
    int_thr         REAL,
    ilp             REAL,
    mul_thr         REAL,
    fp_lat          REAL,
    fp_thr          REAL,
    filp            REAL,
    mem_gbps        REAL,
    mem_lat_ns      REAL,
    mem_lat8_ns     REAL,
    mlp             REAL,
    disp_thr        REAL,
    disp_cap        REAL,
    disp_prediction TEXT,
    disp_gain       REAL,
    disp_span       REAL,
    score           REAL
);

CREATE INDEX IF NOT EXISTS cores_run   ON cores(run_id);
CREATE INDEX IF NOT EXISTS cores_scope ON cores(scope, score);
CREATE INDEX IF NOT EXISTS runs_target ON runs(target, vectorize, fma);
CREATE INDEX IF NOT EXISTS runs_time   ON runs(created_at);
