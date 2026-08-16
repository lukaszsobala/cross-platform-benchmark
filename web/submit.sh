#!/bin/sh
# Upload a cpcpub JSON result to a results hub.
#
#   web/submit.sh --save -u https://hub.example -t YOUR-TOKEN   # once per machine
#   web/submit.sh -l "workstation, quiet"                       # runs the bench
#   bench/cpcpub --full --json | web/submit.sh -l "sbc"      # or feed it one
#   web/submit.sh -u https://hub.example -l "sbc" < run.json
#
# With nothing on stdin it runs the benchmark itself, so submitting a result is
# one command rather than three. Otherwise it reads the result document on
# stdin. Either way it prints the server's reply, which carries the run id and
# -- for an anonymous upload -- the delete token that is the only way to
# withdraw the run later.
#
# The hub URL and the account's upload token are remembered in
# $XDG_CONFIG_HOME/cpcpub/hub.conf (or ~/.config/cpcpub/hub.conf) by
# `--save`, so later runs need no flags at all. The token is what puts the run
# on your account; without one the upload is anonymous, which the hub accepts
# just the same. Get one from the hub's Account tab.
#
# A --variants run writes an array of documents, one per build variant. The hub
# takes one document per POST and keys a run on the variant's two flags, so the
# array is posted as separate runs -- which is what makes them comparable in the
# leaderboard. Splitting it needs a JSON parser, so that path requires python3;
# a single document does not.

set -eu

# shellcheck disable=SC1007  # `CDPATH= cd` is an assignment prefix, not a typo
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
config_dir=${XDG_CONFIG_HOME:-$HOME/.config}/cpcpub
config=$config_dir/hub.conf

url=
token=
label=
notes=
save=0

usage() {
    cat >&2 <<EOF
usage: $0 [-u URL] [-t TOKEN] [-l LABEL] [-n NOTES] [--save] [< result.json]
  -u URL    hub base URL (default: saved, then \$CPCPUB_HUB,
            then http://127.0.0.1:8080)
  -t TOKEN  upload token, so the run lands on your account
            (default: saved, then \$CPCPUB_TOKEN; without one it is anonymous)
  -l LABEL  short label for the run, e.g. the machine's name
  -n NOTES  longer notes: cooling, governor, anything explaining the numbers
  --save    write URL and token to $config and exit; a saved token is kept
            unless -t is given, so -t '' is how you go back to anonymous
  -h        this

With no result on stdin, runs $here/../bench/cpcpub --full --json itself.
EOF
    exit 2
}

# The saved settings, if any. Read line by line rather than sourced: this file
# is data, and a config file that can run commands is a config file that will.
if [ -f "$config" ]; then
    while IFS='=' read -r key value; do
        case "$key" in
            url)   url=$value ;;
            token) token=$value ;;
        esac
    done < "$config"
fi

# Environment beats the file, flags beat the environment.
url=${CPCPUB_HUB:-$url}
token=${CPCPUB_TOKEN:-$token}

# --save is a long option; getopts does not do those, so it is lifted out of the
# argument list before getopts sees it. Rotating the positional parameters one
# at a time rather than re-splitting them, so `-l "my box"` survives.
for arg do
    shift
    if [ "$arg" = "--save" ]; then
        save=1
    else
        set -- "$@" "$arg"
    fi
done

while getopts "u:t:l:n:h" opt; do
    case "$opt" in
        u) url=$OPTARG ;;
        t) token=$OPTARG ;;
        l) label=$OPTARG ;;
        n) notes=$OPTARG ;;
        *) usage ;;
    esac
done

url=${url:-http://127.0.0.1:8080}

if [ "$save" = 1 ]; then
    mkdir -p "$config_dir"
    # Written with the umask closed: the token is a credential, and the rest of
    # the machine has no business reading it.
    (umask 077; printf 'url=%s\ntoken=%s\n' "$url" "$token" > "$config")
    echo "saved $config"
    echo "  url   $url"
    if [ -n "$token" ]; then
        echo "  token set -- uploads from this machine will be on your account"
    else
        echo "  token unset -- uploads will be anonymous; -t TOKEN to change that"
    fi
    exit 0
fi

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
    if [ -n "$token" ]; then
        curl -fsS -X POST "$url/api/runs$(query_for "$2")" \
            -H "Authorization: Bearer $token" \
            -H 'Content-Type: application/json' \
            --data-binary @"$1"
    else
        curl -fsS -X POST "$url/api/runs$(query_for "$2")" \
            -H 'Content-Type: application/json' \
            --data-binary @"$1"
    fi
}

# stdin is read once and kept, because deciding what it is means looking at it.
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cpcpub-submit.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

if [ -t 0 ]; then
    # Nothing piped in: measure this machine rather than printing usage at
    # someone who has just told us where to send the result.
    bench=$here/../bench/cpcpub
    # Git Bash: the same build, one suffix different.
    [ -x "$bench" ] || [ ! -x "$bench.exe" ] || bench=$bench.exe
    [ -x "$bench" ] || {
        echo "$0: nothing on stdin and no benchmark at $bench" >&2
        echo "     build it with 'make', or pipe a result document in" >&2
        exit 1
    }
    echo "running $bench --full --json (a minute or so)..." >&2
    "$bench" --full --json > "$tmp/in.json"
else
    cat > "$tmp/in.json"
fi

[ -s "$tmp/in.json" ] || { echo "$0: empty result document" >&2; exit 1; }

if [ -z "$token" ]; then
    echo "note: uploading anonymously; keep the delete_token below to withdraw" >&2
    echo "      this run. '$0 --save -t TOKEN' puts it on your account instead." >&2
fi

# A leading '[' is an array of per-variant documents; anything else is one
# document and goes up untouched, with no interpreter in the way.
if [ "$(tr -d ' \t\r\n' < "$tmp/in.json" | cut -c1)" != "[" ]; then
    post "$tmp/in.json" "$label"
    echo
    exit 0
fi

command -v python3 >/dev/null 2>&1 || {
    echo "$0: a --variants array needs python3 to split; upload one variant at" >&2
    echo "     a time instead: cpcpub --variant NAME --json | $0 ..." >&2
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
