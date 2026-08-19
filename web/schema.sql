-- cpcpub results hub.
--
-- Four tables: one row per uploaded run, one row per core/thread record inside
-- it, and the accounts a run may be attributed to with their live sessions. The
-- records are flattened out of the JSON so the leaderboard can sort and filter
-- in SQL; the original document is kept verbatim in runs.raw so nothing an
-- upload contained is ever lost to the flattening.

-- An account is a name, a password and an upload token. Nothing else is asked
-- for: no email is collected, so there is no address to leak and no password
-- reset to implement -- a forgotten password is a new account.
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY,
    name          TEXT    NOT NULL,   -- as typed; unique case-insensitively
    password_hash TEXT    NOT NULL,   -- scrypt$n$r$p$salt$hash, see server.py
    -- Readable, because the account page has to be able to show it again --
    -- it is what the submit command carries. It grants uploading and deleting
    -- as this account and nothing else, and rotating it is one click.
    api_token     TEXT    NOT NULL UNIQUE,
    created_at    TEXT    NOT NULL
);

-- "someone@ThisBox" and "someone@thisbox" must not be two accounts: the board
-- shows the name, and two spellings of one name is an impersonation.
CREATE UNIQUE INDEX IF NOT EXISTS users_name ON users(lower(name));

-- One row per signed-in browser. The cookie holds the token; this holds its
-- SHA-256, so a copy of the database does not hand over live sessions.
CREATE TABLE IF NOT EXISTS sessions (
    token_hash  TEXT    PRIMARY KEY,
    user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at  TEXT    NOT NULL,
    expires_at  TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);

CREATE TABLE IF NOT EXISTS runs (
    id            INTEGER PRIMARY KEY,
    created_at    TEXT    NOT NULL,
    delete_token  TEXT    NOT NULL,
    -- Who uploaded it, when anyone did. NULL is an anonymous upload: the board
    -- keeps taking those, and the delete token stays the only way to withdraw
    -- one. Plain column rather than a foreign key, because closing an account
    -- takes its runs with it in one explicit transaction, and because an
    -- existing database gains this column by ALTER TABLE, which cannot add a
    -- reference in SQLite.
    user_id       INTEGER,
    label         TEXT,
    notes         TEXT,

    mode          TEXT    NOT NULL,   -- threads | per-core | full
    -- SHA-256 of the binary that produced the run, when it could take one.
    -- Matched against verified_builds; see the note on that table.
    binary_sha256 TEXT,

    -- Vestigial, and kept only so a database written by an older build still
    -- opens and still holds what it recorded. The hub once accepted a GitHub
    -- OIDC token signed over an uploaded result and stored what it claimed
    -- here. It no longer does: earning one meant running the benchmark from a
    -- CI job, on GitHub's own hardware or on a build agent installed on your
    -- own, which is not a benchmark anybody runs with one command -- and the
    -- architectures this project exists to compare have no build agent at all.
    -- Nothing reads or writes these five columns.
    attest_tier     TEXT,
    attest_repo     TEXT,
    attest_workflow TEXT,
    attest_run_url  TEXT,
    attest_at       TEXT,
    compiler      TEXT,               -- "gcc 15.2.0"
    compiler_version TEXT,            -- the toolchain's own version banner
    cc            TEXT,               -- driver invoked, e.g. riscv64-linux-gnu-gcc
    build_flags   TEXT,               -- the exact CFLAGS the binary was built with
    target        TEXT,               -- x86_64 | aarch64 | riscv64 | loongarch64 | ppc64le | s390x
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

-- The digests of the binaries a release published, loaded by web/verified.py
-- from the manifest the release workflow attaches. A run whose binary_sha256 is
-- in here is shown as *verified*: it was produced by a build whose compiler,
-- flags and source everyone can see, rather than by a local compile with an
-- -march nobody wrote down.
--
-- Matched by join rather than stamped onto the run at upload time, deliberately.
-- A hub that loads a manifest after the fact then verifies the runs that were
-- already waiting, instead of leaving them permanently unverified for having
-- arrived first.
--
-- It is not proof and does not pretend to be: the digest is self-reported like
-- every other field in an upload. What it establishes is sameness of code, not
-- honesty of submitter.
--
-- One row per (binary, release) pair, not one per binary. A release only
-- changes the bytes of the targets whose build actually changed, so the same
-- digest is usually published again, unchanged, by every release after the one
-- that introduced it -- and a table keyed on the digest alone can then hold
-- only one of those answers, whichever manifest was loaded last. Keeping every
-- pair is what lets a read pick the *newest* release a binary appears in, which
-- is the one a reader wants: it is the version those bytes are current in, not
-- the archaeology of when they were first compiled.
CREATE TABLE IF NOT EXISTS verified_builds (
    sha256      TEXT    NOT NULL,
    release     TEXT    NOT NULL,  -- the tag it was published under
    -- Sortable form of `release`, written by server.release_rank(). Tags are
    -- text and text does not sort: "v0.10.0" precedes "v0.9.0" in every string
    -- comparison there is, and picking a newest release by ORDER BY needs a key
    -- where it does not.
    release_rank TEXT   NOT NULL,
    release_url TEXT,
    filename    TEXT,              -- the asset name, e.g. cpcpub-riscv64-rva23
    target      TEXT,              -- x86_64 | aarch64 | riscv64 | loongarch64 | ppc64le | s390x
    march       TEXT,              -- the ISA baseline it was built for
    added_at    TEXT    NOT NULL,
    PRIMARY KEY (sha256, release)
);

-- The lookup every board row makes: digest in hand, which releases published
-- it. The primary key's own index leads with sha256 and would serve, but it is
-- named here so a rebuilt table cannot lose it silently.
CREATE INDEX IF NOT EXISTS verified_builds_sha ON verified_builds(sha256);

CREATE INDEX IF NOT EXISTS cores_run   ON cores(run_id);
CREATE INDEX IF NOT EXISTS cores_scope ON cores(scope, score);
CREATE INDEX IF NOT EXISTS runs_target ON runs(target, vectorize, fma);
CREATE INDEX IF NOT EXISTS runs_time   ON runs(created_at);
