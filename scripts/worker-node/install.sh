#!/bin/sh
# Step 1 of 3, run ON the worker (join.sh pipes it over SSH): the build tools,
# Substrate built from its repo, `sub`, `sub-net` and `sub-dbd` in
# ~/.local/bin, and a GitHub key pair of the worker's own for the private
# repos it will read. Idempotent: run it again and it changes nothing, and
# says so.
#
#   SUB_WORKER_REPO   where to clone from (default: the public GitHub repo)
#   SUB_WORKER_REF    what to build (default: main)
#   SUB_WORKER_NOAPT  set: never call apt-get (a stand-in with its own tools)
#
# Prints paths, never a key.
set -eu
REPO=${SUB_WORKER_REPO:-https://github.com/JoshuaRamirez/substrate.git}
REF=${SUB_WORKER_REF:-main}
SRC="$HOME/.local/src/substrate"
BIN="$HOME/.local/bin"
GHKEY="$HOME/.ssh/substrate-worker-github_ed25519"
changed=""
note(){ changed="$changed $1"; }

# -- the build tools -----------------------------------------------------------
missing=""
for t in git make clang ld ssh-keygen; do command -v "$t" >/dev/null 2>&1 || missing="$missing $t"; done
if [ -n "$missing" ]; then
  if [ -n "${SUB_WORKER_NOAPT:-}" ]; then
    echo "install: missing$missing, and apt is off here" >&2; exit 3
  fi
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq git make clang binutils openssh-client >/dev/null
  note packages
fi

# -- the source ------------------------------------------------------------------
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$(dirname "$SRC")"
  git clone -q "$REPO" "$SRC"
  note clone
fi
before=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo none)
git -C "$SRC" fetch -q origin
want=$(git -C "$SRC" rev-parse --verify -q "origin/$REF^{commit}" || git -C "$SRC" rev-parse --verify "$REF^{commit}")
if [ "$before" != "$want" ]; then
  git -C "$SRC" checkout -q --detach "$want"
  note "source@$(git -C "$SRC" rev-parse --short HEAD)"
fi

# -- the build, and only what a worker runs ------------------------------------------
make -s -C "$SRC" bin/sub bin/net bin/dbd >/dev/null
mkdir -p "$BIN"
for pair in sub:sub net:sub-net dbd:sub-dbd; do
  from="$SRC/bin/${pair%%:*}" to="$BIN/${pair#*:}"
  if ! cmp -s "$from" "$to"; then
    cp "$from" "$to.new" && chmod 755 "$to.new" && mv "$to.new" "$to"
    note "${pair#*:}"
  fi
done

# -- the worker's own GitHub key ------------------------------------------------------
mkdir -p "$HOME/.ssh" && chmod 700 "$HOME/.ssh"
if [ ! -f "$GHKEY" ]; then
  ssh-keygen -q -t ed25519 -N "" -C "substrate-worker@$(hostname)" -f "$GHKEY" </dev/null >/dev/null
  note github-key
fi
# use it for github.com, once: a marked block, never a second copy
CFG="$HOME/.ssh/config"
if ! grep -qs "^# substrate-worker github key" "$CFG"; then
  {
    echo "# substrate-worker github key"
    echo "Host github.com"
    echo "  IdentityFile $GHKEY"
    echo "  IdentitiesOnly yes"
  } >> "$CFG"
  chmod 600 "$CFG"
  note ssh-config
fi

echo "install: sub, sub-net, sub-dbd in $BIN ($(git -C "$SRC" rev-parse --short HEAD))"
echo "install: github key $GHKEY (public half $GHKEY.pub)"
echo "install: changed:${changed:- nothing}"
