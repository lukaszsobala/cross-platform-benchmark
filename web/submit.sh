#!/bin/sh
# Upload a cpu-bench JSON result to a results hub.
#
#   bench/cpu-bench --full --json | web/submit.sh -l "workstation, quiet"
#   web/submit.sh -u https://hub.example -l "sbc" < run.json
#
# Reads the result document on stdin. Prints the server's reply, which contains
# the run id and the delete token -- keep the token to withdraw the run later.

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

query=""
[ -n "$label" ] && query="?label=$(encode "$label")"
if [ -n "$notes" ]; then
    [ -n "$query" ] && query="$query&notes=$(encode "$notes")" \
                    || query="?notes=$(encode "$notes")"
fi

exec curl -fsS -X POST "$url/api/runs$query" \
    -H 'Content-Type: application/json' \
    --data-binary @-
