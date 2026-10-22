#!/bin/sh
# Two substrates on one machine, joined by the network adapter: two segments,
# two owners, two `sub-net serve`s on 127.0.0.1, two throwaway key pairs. One
# reads the other's registry, starts and stops a process there, and is told of
# its events as they happen -- over the same authenticated, encrypted
# connection a second machine would use. Run alone (./test-net.sh) or as part
# of ./test.sh.
#
# Everything lives in a temp folder: the keys, the trust lists, the change
# lane, the logs. The machine's own segment, keys and lane are never touched,
# and no port is opened on anything but loopback.
cd "$(dirname "$0")"
fail=0
ok()   { printf "  PASS  %s\n" "$1"; }
bad()  { printf "  FAIL  %s\n" "$1"; fail=1; }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1 (want '$3', got '$2')"; }
NET="$(pwd)/bin/net" SUB="$(pwd)/bin/sub" DBD="$(pwd)/bin/dbd" PEER="$(pwd)/bin/peer"

T=$(mktemp -d /tmp/subnet.XXXXXX)
export SUB_LANE="$T/lane" DESK_WHOAMI="net-tester"
A="$T/a" B="$T/b" C="$T/c"
mkdir -p "$A" "$B" "$C"
SHA=/sub.na.$$ SHB=/sub.nb.$$
PA=$((20000 + $$ % 20000)) PB=$((20001 + $$ % 20000))
# run a command as machine A or machine B: its own segment, its own net folder.
# A function would put a background job in a subshell, and $! would name the
# subshell rather than the process, so cleanup would miss it: env execs.
asA(){ env SUB_SHM=$SHA SUB_NET_DIR="$A" "$@"; }
asB(){ env SUB_SHM=$SHB SUB_NET_DIR="$B" "$@"; }
EA="env SUB_SHM=$SHA SUB_NET_DIR=$A" EB="env SUB_SHM=$SHB SUB_NET_DIR=$B"
# wait until a file holds a line matching a pattern (3 s at most)
waitline(){ for _ in $(seq 1 60); do grep -qs -- "$2" "$1" && return 0; sleep 0.05; done; return 1; }
PIDS=""
# On every way out, pass or fail: stop what the test started, then take its
# folder with it -- the throwaway key pairs are the test's, and stay its.
cleanup(){
  for p in $PIDS; do kill "$p" 2>/dev/null; done
  asA "$DBD" --stop >/dev/null 2>&1; asB "$DBD" --stop >/dev/null 2>&1
  case "$T" in /tmp/subnet.*) rm -rf "$T" ;; esac
}
trap cleanup EXIT
trap 'exit 130' INT TERM

echo "24. the network adapter: two substrates, one connection, live"

"$NET" selftest > "$T/selftest.out" 2>&1
check "every primitive matches its RFC vectors" "$? $(grep -c '^  ok' "$T/selftest.out")" "0 14"

# -- keys ---------------------------------------------------------------------
KG=$(SUB_NET_DIR="$A" "$NET" keygen 2>&1); SUB_NET_DIR="$B" "$NET" keygen >/dev/null 2>&1
SUB_NET_DIR="$C" "$NET" keygen >/dev/null 2>&1
check "keygen makes a secret half only its owner can read" "$(stat -c %a "$A/key" 2>/dev/null || stat -f %Lp "$A/key")" "600"
check "keygen prints no key" "$(printf '%s' "$KG" | grep -c -F -e "$(cat "$A/key")" -e "$(cat "$A/key.pub")")" "0"
SUB_NET_DIR="$A" "$NET" keygen >/dev/null 2>&1
check "keygen will not overwrite a key pair unasked" "$?" "2"

SUB_NET_DIR="$A" "$NET" trust b "$B/key.pub" --at 127.0.0.1:$PB >/dev/null
SUB_NET_DIR="$B" "$NET" trust a "$A/key.pub" --at 127.0.0.1:$PA >/dev/null
SUB_NET_DIR="$C" "$NET" trust b "$B/key.pub" --at 127.0.0.1:$PB >/dev/null   # C trusts B; B does not trust C

# -- listening: one private address, never everything -------------------------
for addr in 0.0.0.0:$PB "[::]:$PB" 8.8.8.8:$PB; do
  asB "$NET" serve --bind "$addr" >/dev/null 2>&1
  check "serve refuses to listen on $addr" "$?" "3"
done
SUB_NET_DIR="$T/nokeys" "$NET" serve --bind 127.0.0.1:$PB >/dev/null 2>&1
check "serve refuses to start without a key pair" "$?" "3"

# -- two substrates ------------------------------------------------------------
( cd "$A" && exec $EA "$DBD" "$A/db" > "$A/dbd.log" 2>&1 ) & PIDS="$PIDS $!"
( cd "$B" && exec $EB "$DBD" "$B/db" > "$B/dbd.log" 2>&1 ) & PIDS="$PIDS $!"
waitline "$A/dbd.log" "owning" && waitline "$B/dbd.log" "owning"
$EA "$NET" serve --bind 127.0.0.1:$PA > "$A/net.log" 2>&1 & PIDS="$PIDS $!"
$EB "$NET" serve --bind 127.0.0.1:$PB > "$B/net.log" 2>&1 & PIDS="$PIDS $!"
waitline "$A/net.log" "serving" && waitline "$B/net.log" "serving"
check "both sides serve on loopback" "$(grep -c serving "$A/net.log") $(grep -c serving "$B/net.log")" "1 1"

# -- the registry, read across ------------------------------------------------
check "A reads B's registry: B's owner is on it" \
      "$(asA "$NET" ps b | awk '$2=="dbd" && $3=="owner"{n++} END{print n+0}')" "1"
check "and the row is B's own (its pid)" \
      "$(asA "$NET" ps b --json | python3 -c 'import json,sys; print([p["pid"] for p in json.load(sys.stdin)["peers"] if p["name"]=="dbd"][0])')" \
      "$(asB "$SUB" ps | awk '$2=="dbd"{print $1}')"
check "B reads A's the same way, over its own connection" \
      "$(asB "$NET" ps a | awk '$2=="dbd"{n++} END{print n+0}')" "1"

# -- events, pushed -----------------------------------------------------------
$EA "$NET" events b > "$T/events.jsonl" 2>"$T/events.err" & EV=$!; PIDS="$PIDS $EV"
# held open: B has one more connect than disconnect while it streams
held(){ echo $(( $(grep -c 'a connected' "$B/net.log") - $(grep -c 'a disconnected' "$B/net.log") )); }
waitline "$T/events.err" "subscribed to b"
check "A is subscribed to B's events" "$(grep -c 'subscribed to b' "$T/events.err")" "1"
check "the event stream is one persistent connection, held open" "$(held)" "1"

# -- process control, across --------------------------------------------------
R=$(asA "$NET" run b --name sleeper --role worker -- sleep 300 2>&1)
check "A starts a process on B" "$(printf '%s' "$R" | grep -c 'sleeper running on b')" "1"
check "it is registered on B" "$(asB "$SUB" ps | awk '$2=="sleeper" && $3=="worker"{n++} END{print n+0}')" "1"
check "and not on A" "$(asA "$SUB" ps | awk '$2=="sleeper"{n++} END{print n+0}')" "0"
SPID=$(asB "$SUB" ps | awk '$2=="sleeper"{print $1}')
waitline "$T/events.jsonl" '"ev":"join".*"name":"sleeper"'
check "B's join is pushed to A, live" "$(grep -c '"ev":"join".*"name":"sleeper"' "$T/events.jsonl")" "1"
check "the pushed event names the service's own pid" \
      "$(grep '"ev":"join".*"name":"sleeper"' "$T/events.jsonl" | python3 -c 'import json,sys; print(json.loads(sys.stdin.readline())["pid"])')" "$SPID"

asB "$PEER" --selftest --join >/dev/null 2>&1        # a bump on B's segment
waitline "$T/events.jsonl" '"ev":"count"'
check "a bump on B's segment is pushed as a count event" \
      "$(grep '"ev":"count"' "$T/events.jsonl" | tail -1 | python3 -c 'import json,sys; print(json.loads(sys.stdin.readline())["count"])')" "1"

asA "$NET" stop b sleeper > "$T/stop.out" 2>&1
check "A stops it on B" "$?" "0"
# gone means reaped: until its `sub run` wrapper (which polls every 500 ms)
# collects it, a stopped service is a zombie, and kill -0 still answers
for _ in $(seq 1 60); do kill -0 "$SPID" 2>/dev/null || break; sleep 0.05; done
check "it is gone from B" "$(kill -0 "$SPID" 2>/dev/null && echo running || echo gone)" "gone"
waitline "$T/events.jsonl" '"ev":"leave".*"name":"sleeper"'
check "B's leave is pushed to A" "$(grep -c '"ev":"leave".*"name":"sleeper"' "$T/events.jsonl")" "1"
check "the stop went through B's change lane" \
      "$(python3 -c 'import json,sys; print(sum(1 for l in open(sys.argv[1]) if json.loads(l)["what"]=="stop sleeper"))' "$SUB_LANE/changes.jsonl" 2>/dev/null)" "1"
check "events arrive in order, numbered without gaps" \
      "$(python3 -c '
import json,sys
s=[json.loads(l)["seq"] for l in open(sys.argv[1]) if l.strip()]
print("ok" if s and s==list(range(s[0],s[0]+len(s))) else s)' "$T/events.jsonl")" "ok"

asA "$NET" stop b nosuch >/dev/null 2>&1
check "stopping what is not there says so" "$?" "2"

# -- a named channel (what a Warp Drive cell runs through) ---------------------
cat > "$T/cell.sh" <<'EOF'
#!/bin/sh
while IFS= read -r l; do printf '{"event":"placed","in":%s}\n{"done":true}\n' "$l"; done
EOF
chmod +x "$T/cell.sh"
$EB "$NET" listen warp -- "$T/cell.sh" > "$T/listen.log" 2>&1 & PIDS="$PIDS $!"
waitline "$B/net.log" "listening on channel warp"
OUT=$(echo '{"place":"cell-1"}' | asA "$NET" connect b warp --lines 2 2>&1)
check "a JSON line goes to B's listener and its answers come back" "$OUT" '{"event":"placed","in":{"place":"cell-1"}}
{"done":true}'
asA "$NET" connect b nosuch </dev/null >/dev/null 2>&1
check "connecting to a channel nobody serves is refused" "$?" "3"

# -- who may call ---------------------------------------------------------------
SUB_NET_DIR="$C" "$NET" ps b >/dev/null 2>&1
check "a machine B does not trust is refused" "$?" "3"
waitline "$B/net.log" "not on the trust list"
check "and B says why" "$(grep -c 'not on the trust list' "$B/net.log")" "1"
SUB_NET_DIR="$A" "$NET" trust imposter "$C/key.pub" --at 127.0.0.1:$PB >/dev/null
SUB_NET_DIR="$A" "$NET" ps imposter >/dev/null 2>&1
check "a server that cannot prove the key trusted for its name is refused" "$?" "3"

# -- secrets stay secret ----------------------------------------------------------
LEAK=0
for k in "$A/key" "$B/key" "$C/key"; do
  grep -rqsF "$(cat "$k")" "$A/net.log" "$B/net.log" "$T/events.jsonl" "$T/stop.out" "$T/listen.log" \
       "$A/dbd.log" "$B/dbd.log" && LEAK=1
done
check "no secret key appears in any log or output" "$LEAK" "0"
check "nothing key-shaped was written into the repo" "$(git status --porcelain 2>/dev/null | grep -c -e 'key' -e 'peers')" "0"

[ $fail -eq 0 ] && exit 0 || exit 1
