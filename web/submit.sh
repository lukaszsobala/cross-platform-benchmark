#!/bin/sh
# Upload a cpu-bench JSON result to a results hub.
#
#   bench/cpu-bench --full --json | web/submit.sh -l "workstation, quiet"
#   bench/cpu-bench --full --variants --json | web/submit.sh -l "workstation"
#   web/submit.sh -u https://hub.example -l "sbc" < run.json
#
# Reads the result document on stdin. Prints the server's reply, which contains
# the run id and the delete token -- keep the token to withdraw the run later.
#
# A --variants run writes an array of documents, one per build variant. The hub
# takes one document per POST and keys a run on the variant's two flags, so the
# array is posted as separate runs -- which is what makes them comparable in the
# leaderboard. Splitting it needs a JSON parser, so that path requires python3;
# a single document does not.

set -eu

url=${CPU_BENCH_HUB:-http://127.0.0.1:8080}
label=
notes=

usage() {
    cat >&2 <<EOF
usage: $0 [-u URL] [-l LABEL] [-n NOTES] < result.json
  -u URL    hub base URL (default \$CPU_BENCH_HUB or http://127.0.0.1:8080)
  -l LABEL  short label for the run, e.g. the machine's name
  -n NOTES  longer notes: cooling, governor, anything explaining the numbers
EOF
    exit 2
}

while getopts "u:l:n:h" opt; do
    case "$opt" in
        u) url=$OPTARG ;;
        l) label=$OPTARG ;;
        n) notes=$OPTARG ;;
        *) usage ;;
    esac
done

command -v curl >/dev/null 2>&1 || { echo "$0: curl is required" >&2; exit 1; }

# Percent-encode the query values so labels may contain spaces and punctuation.
encode() {
    printf '%s' "$1" | od -An -tx1 -v | tr -d '\n ' | sed 's/../%&/g'
}

# Build the query string for one upload. $1 is the label to use.
query_for() {
    q=""
    [ -n "$1" ] && q="?label=$(encode "$1")"
    if [ -n "$notes" ]; then
        [ -n "$q" ] && q="$q&notes=$(encode "$notes")" || q="?notes=$(encode "$notes")"
    fi
    printf '%s' "$q"
}

post() {  # post FILE LABEL
    curl -fsS -X POST "$url/api/runs$(query_for "$2")" \
        -H 'Content-Type: application/json' \
        --data-binary @"$1"
}

# stdin is read once and kept, because deciding what it is means looking at it.
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cpu-bench-submit.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
cat > "$tmp/in.json"

# A leading '[' is an array of per-variant documents; anything else is one
# document and goes up untouched, with no interpreter in the way.
if [ "$(tr -d ' \t\r\n' < "$tmp/in.json" | cut -c1)" != "[" ]; then
    post "$tmp/in.json" "$label"
    exit 0
fi

command -v python3 >/dev/null 2>&1 || {
    echo "$0: a --variants array needs python3 to split; upload one variant at" >&2
    echo "     a time instead: cpu-bench --variant NAME --json | $0 ..." >&2
    exit 1
}

# One file per element, named for the variant so the label can say which is
# which. The name is not in the document -- it is the two build flags that are,
# and they determine it.
python3 - "$tmp" <<'EOF'
import json, os, sys

out = sys.argv[1]
docs = json.load(open(os.path.join(out, "in.json")))
if not isinstance(docs, list) or not docs:
    sys.exit("submit.sh: expected a non-empty array of result documents")
for i, doc in enumerate(docs):
    build = doc.get("build") or {}
    name = "%s-%s" % ("vector" if build.get("vectorize") else "scalar",
                      "fma" if build.get("fma") else "nofma")
    with open(os.path.join(out, "%02d-%s.json" % (i, name)), "w") as f:
        json.dump(doc, f)
EOF

for f in "$tmp"/[0-9][0-9]-*.json; do
    variant=$(basename "$f" .json)
    variant=${variant#??-}
    echo "== $variant"
    post "$f" "${label:+$label }($variant)"
    echo
done
