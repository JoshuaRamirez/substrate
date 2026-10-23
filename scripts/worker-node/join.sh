#!/bin/sh
# Run on the Mac: make a Linux machine a Substrate worker this Mac drives.
#
#   join.sh NAME ADDRESS SSH-KEY-PATH [--user U] [--port P] [--self NAME]
#           [--deploy-repo OWNER/REPO]...
#   join.sh --undo NAME              stop trusting it here (its side: serve.sh --undo)
#
# ADDRESS is whatever the private link gives the worker, the same address the
# Mac reaches it on and the one it serves on: join.sh does not care which link
# that is. The three steps run on the worker in order, over SSH: install.sh,
# identity.sh (with this Mac's key.pub, sent over), serve.sh. Then the worker's
# key.pub comes back and this Mac trusts it, its GitHub key's public half is
# registered read-only on each --deploy-repo, and the proof is `sub-net ps NAME`.
#
# Only public halves cross. Nothing is installed or started on this Mac: it is
# an SSH client and this repo's own bin/net. Every step is idempotent, so a
# second join changes nothing and says so.
#
#   SUB_WORKER_EXEC   a local executor instead of SSH (the stand-in test): a
#                     command that runs its one argument as a shell command line
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NET="$ROOT/bin/net"

die(){ echo "join: $*" >&2; exit 2; }
# names, addresses and paths reach a remote shell: keep them to safe characters
safe(){ case "$1" in *[!A-Za-z0-9._:/@%+-]*|'') die "'$1' is not a plain $2" ;; esac; }

if [ "${1:-}" = "--undo" ]; then
  [ $# -ge 2 ] || die "--undo NAME"
  safe "$2" name
  [ -x "$NET" ] || make -s -C "$ROOT" bin/net
  "$NET" untrust "$2"
  echo "join: on $2 itself, run scripts/worker-node/serve.sh --undo"
  exit 0
fi

[ $# -ge 3 ] || die "usage: join.sh NAME ADDRESS SSH-KEY-PATH [--user U] [--port P] [--self NAME] [--deploy-repo OWNER/REPO]..."
NAME=$1 ADDR=$2 SSHKEY=$3; shift 3
USER_AT=azureuser PORT=7341 SELF=mac REPOS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --user) USER_AT=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --self) SELF=$2; shift 2 ;;
    --deploy-repo) safe "$2" repo; REPOS="$REPOS $2"; shift 2 ;;
    *) die "unknown option $1" ;;
  esac
done
for v in "$NAME" "$SELF" "$USER_AT"; do safe "$v" name; done
safe "$ADDR" address; safe "$PORT" port
case "$ADDR" in *:*) BIND="[$ADDR]:$PORT" ;; *) BIND="$ADDR:$PORT" ;; esac

# one way to run a command line on the worker: SSH, or the stand-in's executor
if [ -n "${SUB_WORKER_EXEC:-}" ]; then
  on_worker(){ $SUB_WORKER_EXEC "$1"; }
else
  [ -f "$SSHKEY" ] || die "no SSH key at $SSHKEY"
  on_worker(){
    ssh -i "$SSHKEY" -o IdentitiesOnly=yes -o BatchMode=yes \
        -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 "$USER_AT@$ADDR" "$1"
  }
fi
# pass the stand-in's knobs through; on a real worker they are unset
pass=""
for k in SUB_WORKER_REPO SUB_WORKER_REF SUB_WORKER_NOAPT SUB_WORKER_SERVICE; do
  eval "v=\${$k:-}"
  # shellcheck disable=SC2154  # v is set by the eval above
  [ -n "$v" ] && { safe "$v" "$k value"; pass="$pass $k='$v'"; }
done

[ -x "$NET" ] || make -s -C "$ROOT" bin/net
HERE_DIR=$("$NET" dir)
[ -f "$HERE_DIR/key" ] || "$NET" keygen >/dev/null
KEEP="$HERE_DIR/workers/$NAME"
mkdir -p "$KEEP"

echo "join: $NAME at $ADDR, step 1 of 3: install"
on_worker "env$pass sh -s" < "$HERE/install.sh"

echo "join: $NAME, step 2 of 3: identity and trust"
on_worker "mkdir -p ~/.cache/substrate-worker && cat > ~/.cache/substrate-worker/$SELF.pub" < "$HERE_DIR/key.pub"
on_worker "env$pass sh -s -- --trust '$SELF' ~/.cache/substrate-worker/$SELF.pub" < "$HERE/identity.sh"
# shellcheck disable=SC2016  # expands on the worker, not here
on_worker 'cat "$(~/.local/bin/sub-net dir)/key.pub"' > "$KEEP/key.pub.new"
on_worker 'cat ~/.ssh/substrate-worker-github_ed25519.pub' > "$KEEP/github.pub.new"
# keep a public half only when it is new or changed, so a re-join leaves it be
for f in key.pub github.pub; do
  if cmp -s "$KEEP/$f.new" "$KEEP/$f"; then mv "$KEEP/$f.new" "$KEEP/.$f.fetched"
  else mv "$KEEP/$f.new" "$KEEP/$f"; fi
done
"$NET" trust "$NAME" "$KEEP/key.pub" --at "$BIND"

echo "join: $NAME, step 3 of 3: serve"
on_worker "env$pass sh -s -- '$BIND'" < "$HERE/serve.sh"

# the worker's own read-only access to the private repos it needs
for r in $REPOS; do
  title="substrate-worker $NAME"
  if gh repo deploy-key list -R "$r" 2>/dev/null | grep -qF "$title"; then
    echo "join: deploy key already on $r ($title)"
  else
    gh repo deploy-key add "$KEEP/github.pub" -R "$r" -t "$title" --allow-write=false >/dev/null
    echo "join: deploy key added to $r, read-only ($title, public half $KEEP/github.pub)"
  fi
done

echo "join: the proof, sub-net ps $NAME"
for _ in 1 2 3 4 5 6 7 8 9 10; do "$NET" ps "$NAME" 2>/dev/null && exit 0; sleep 1; done
"$NET" ps "$NAME"
