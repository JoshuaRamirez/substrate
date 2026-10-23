#!/bin/sh
# Step 2 of 3, run ON the worker: its sub-net key pair, made once, and trust in
# the machine that will drive it, from that machine's key.pub (which join.sh
# has already put here). Public halves only cross the link. Idempotent.
#
#   identity.sh [--trust NAME PUBFILE]...
set -eu
NET="$HOME/.local/bin/sub-net"
changed=""
DIR=$("$NET" dir)
if [ ! -f "$DIR/key" ]; then
  "$NET" keygen >/dev/null
  changed=" keygen"
fi
while [ $# -gt 0 ]; do
  case "$1" in
    --trust)
      [ $# -ge 3 ] || { echo "identity: --trust NAME PUBFILE" >&2; exit 2; }
      out=$("$NET" trust "$2" "$3")
      case "$out" in *"already trusting"*) ;; *) changed="$changed trust:$2" ;; esac
      shift 3 ;;
    *) echo "identity: unknown option $1" >&2; exit 2 ;;
  esac
done
echo "identity: key pair in $DIR (public half $DIR/key.pub)"
echo "identity: changed:${changed:- nothing}"
