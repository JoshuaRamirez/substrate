#!/bin/sh
# Step 3 of 3, run ON the worker: the registry (sub-dbd) and the network
# adapter (sub-net serve) as systemd user services, the adapter bound to the
# one private address the driving machine reaches this worker on, and linger
# on, so both come back after the nightly shutdown without anyone logging in.
# sub-net refuses a wildcard or public address itself; nothing here listens on
# anything else. Idempotent: an unchanged unit is neither rewritten nor
# restarted.
#
#   serve.sh ADDRESS:PORT          set up and start
#   serve.sh --undo                stop, disable, remove the units
#
#   SUB_WORKER_SERVICE=direct      no systemd (a stand-in): write the units,
#                                  start the two processes directly instead
set -eu
BIN="$HOME/.local/bin"
UNITS="$HOME/.config/systemd/user"
DATA="$HOME/.local/share/substrate"
MODE=${SUB_WORKER_SERVICE:-systemd}
changed=""

if [ "${1:-}" = "--undo" ]; then
  if [ "$MODE" = systemd ]; then
    systemctl --user disable --now substrate-net.service substrate-dbd.service 2>/dev/null || true
  else
    for p in net dbd; do
      [ -f "$DATA/$p.pid" ] && kill "$(cat "$DATA/$p.pid")" 2>/dev/null; mv "$DATA/$p.pid" "$DATA/$p.pid.stopped" 2>/dev/null || true
    done
  fi
  for u in substrate-net substrate-dbd; do
    [ -f "$UNITS/$u.service" ] && mv "$UNITS/$u.service" "$UNITS/$u.service.removed"
  done
  [ "$MODE" = systemd ] && systemctl --user daemon-reload
  echo "serve: stopped and disabled (units moved aside as *.service.removed)"
  exit 0
fi

BIND=${1:?usage: serve.sh ADDRESS:PORT | --undo}
mkdir -p "$UNITS" "$DATA"

# write a unit only when its text differs
unit(){ # name, text
  if [ "$(cat "$UNITS/$1.service" 2>/dev/null)" != "$2" ]; then
    printf '%s\n' "$2" > "$UNITS/$1.service.new" && mv "$UNITS/$1.service.new" "$UNITS/$1.service"
    changed="$changed $1"
  fi
}
unit substrate-dbd "[Unit]
Description=Substrate registry (sub-dbd)

[Service]
ExecStart=%h/.local/bin/sub-dbd %h/.local/share/substrate/substrate.db
Restart=always
RestartSec=2

[Install]
WantedBy=default.target"
# Restart=always is also how it waits for the private address: until the
# link's interface is up, the bind fails, and systemd tries again.
unit substrate-net "[Unit]
Description=Substrate network adapter (sub-net serve on $BIND)
After=substrate-dbd.service network-online.target
Wants=substrate-dbd.service

[Service]
ExecStart=%h/.local/bin/sub-net serve --bind $BIND
Restart=always
RestartSec=2

[Install]
WantedBy=default.target"

if [ "$MODE" = systemd ]; then
  if [ ! -e "/var/lib/systemd/linger/$(id -un)" ]; then
    sudo loginctl enable-linger "$(id -un)"
    changed="$changed linger"
  fi
  systemctl --user daemon-reload
  for u in substrate-dbd substrate-net; do
    if ! systemctl --user is-enabled -q "$u.service" 2>/dev/null; then
      systemctl --user enable -q "$u.service"; changed="$changed enable:$u"
    fi
    case "$changed" in
      *" $u"*) systemctl --user restart "$u.service" ;;                         # its unit changed
      *) systemctl --user is-active -q "$u.service" || { systemctl --user start "$u.service"; changed="$changed start:$u"; } ;;
    esac
  done
else
  # the stand-in: the same two commands the units run, started once
  running(){ [ -f "$DATA/$1.pid" ] && kill -0 "$(cat "$DATA/$1.pid")" 2>/dev/null; }
  if ! running dbd; then
    nohup "$BIN/sub-dbd" "$DATA/substrate.db" > "$DATA/dbd.log" 2>&1 &
    echo $! > "$DATA/dbd.pid"; changed="$changed start:substrate-dbd"
    for _ in $(seq 1 50); do grep -qs owning "$DATA/dbd.log" && break; sleep 0.1; done
  fi
  if ! running net; then
    nohup "$BIN/sub-net" serve --bind "$BIND" > "$DATA/net.log" 2>&1 &
    echo $! > "$DATA/net.pid"; changed="$changed start:substrate-net"
    for _ in $(seq 1 50); do grep -qs serving "$DATA/net.log" && break; sleep 0.1; done
  fi
fi
echo "serve: sub-net serve on $BIND ($MODE)"
echo "serve: changed:${changed:- nothing}"
