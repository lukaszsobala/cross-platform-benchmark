#!/usr/bin/env python3
"""Tell a hub which binaries a release published.

    python3 web/verified.py --db runs.sqlite3 verified-builds.json
    python3 web/verified.py --db runs.sqlite3 \
        https://github.com/OWNER/REPO/releases/download/v0.1.0/verified-builds.json
    python3 web/verified.py --db runs.sqlite3 --list
    python3 web/verified.py --db runs.sqlite3 --forget v0.1.0

The manifest is the `verified-builds.json` that .github/workflows/release.yml
attaches to a release, next to the binaries it describes. Loading it stores the
SHA-256 of each published binary; a run whose `build.binary_sha256` matches one
is labelled with that release on the board -- in grey, not as a badge, because
the digest is a field the submitter wrote. It says which binary the run claims,
which is the comparability question and not the honesty one. What a badge takes
is an attestation; see web/README.md.

Runs already in the database are verified retroactively, because the match is a
join rather than a stamp. So loading a manifest late costs nothing, and there is
no ordering to get right between publishing a release and collecting results.

Nothing here needs a credential. The manifest is a public release asset fetched
over plain HTTPS, and this command touches the database directly rather than the
running server -- there is no upload endpoint for it and no token to leak. Run it
where the database file is.

Re-loading a release replaces exactly what that release published, so a
corrected manifest is this command again, not a database to repair by hand.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import server as srv                                        # noqa: E402

MAX_MANIFEST = 1 << 20      # a hundred builds of metadata, generously


def read_manifest(source: str) -> object:
    """The manifest at a path or an http(s) URL."""
    if source.startswith(("http://", "https://")):
        # No credentials, no redirects to anything but http(s): a release asset
        # is public, so anything asking for a token here is not one.
        req = urllib.request.Request(source, headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            if not resp.url.startswith(("http://", "https://")):
                raise SystemExit(f"refusing redirect to {resp.url!r}")
            raw = resp.read(MAX_MANIFEST + 1)
    else:
        raw = Path(source).read_bytes()
    if len(raw) > MAX_MANIFEST:
        raise SystemExit("manifest is implausibly large; refusing it")
    return json.loads(raw.decode("utf-8"))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest", nargs="?",
                    help="path or https:// URL of a verified-builds.json")
    ap.add_argument("--db", default=str(HERE / "runs.sqlite3"),
                    help="the hub's SQLite database (created if absent)")
    ap.add_argument("--list", action="store_true",
                    help="show what this hub currently trusts, and stop")
    ap.add_argument("--forget", metavar="RELEASE",
                    help="drop a release's builds; its runs stop being verified")
    args = ap.parse_args(argv)

    store = srv.Store(args.db)

    if args.list:
        builds = store.verified_builds()
        if not builds:
            print("no verified builds loaded; runs will all show as unverified")
            return 0
        width = max(len(b["filename"] or "") for b in builds)
        for b in builds:
            print(f"{b['sha256']}  {(b['filename'] or ''):<{width}}  "
                  f"{b['release']}  {b['march'] or ''}")
        print(f"\n{len(builds)} build(s) from "
              f"{len({b['release'] for b in builds})} release(s)", file=sys.stderr)
        return 0

    if args.forget:
        gone = store.forget_release(args.forget)
        print(f"forgot {gone} build(s) from {args.forget}", file=sys.stderr)
        return 0

    if not args.manifest:
        ap.error("give a manifest to load, or --list / --forget")

    try:
        stored, release = store.add_verified_builds(read_manifest(args.manifest))
    except srv.Invalid as e:
        raise SystemExit(f"bad manifest: {e}")
    except (OSError, ValueError) as e:
        raise SystemExit(f"could not read {args.manifest}: {e}")

    # How many runs this just turned verified, which is the number that says
    # whether the manifest was the one meant.
    with store.connect() as db:
        now = db.execute(
            "SELECT COUNT(*) AS n FROM runs r JOIN verified_builds vb "
            "ON vb.sha256 = r.binary_sha256 WHERE vb.release = ?",
            (release,)).fetchone()["n"]
    print(f"{release}: {stored} build(s) loaded, {now} run(s) now verified",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
