#!/bin/sh
# The worker-node setup, against a stand-in: a "VM" that is a temp HOME with
# its own segment and net folder on this machine, driven by join.sh through a
# local executor instead of SSH, the way test-net.sh runs two substrates on
# 127.0.0.1. Then the same join again, which must change nothing. A fake `gh`
# on PATH records the deploy-key calls, so nothing here reaches GitHub. Run
# alone (./test-worker.sh) or as part of ./test.sh.
cd "$(dirname "$0")"
fail=0
ok()   { printf "  PASS  %s\n" "$1"; }
bad()  { printf "  FAIL  %s\n" "$1"; fail=1; }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1 (want '$3', got '$2')"; }
skip() { printf "  SKIP  %s\n" "$1"; }
W=scripts/worker-node

echo "25. the worker node: install, identity and trust, serve, and the join"

if command -v shellcheck >/dev/null 2>&1; then
  shellcheck "$W"/*.sh >/dev/null 2>&1
  check "shellcheck passes every worker-node script" "$?" "0"
else
  skip "shellcheck not installed"
fi

T=$(mktemp -d /tmp/subwork.XXXXXX)
VM="$T/vm" MAC="$T/mac"
mkdir -p "$VM" "$MAC" "$T/fakebin"
SHV=/sub.wv.$$ PORT=$((40000 + $$ % 20000))
REF=$(git rev-parse HEAD)
cleanup(){
  for p in net dbd; do
    f="$VM/.local/share/substrate/$p.pid"
    [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null
  done
  SUB_SHM=$SHV ./bin/dbd --stop >/dev/null 2>&1
  case "$T" in /tmp/subwork.*) rm -rf "$T" ;; esac
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# the stand-in's executor: one argument, a command line, run as the "VM"
cat > "$T/vm-exec" <<EOF
#!/bin/sh
exec env HOME="$VM" SUB_SHM="$SHV" SUB_LANE="$VM/lane" sh -c "\$1"
EOF
chmod +x "$T/vm-exec"
# a gh that records what it was asked, and remembers the keys it was given
cat > "$T/fakebin/gh" <<EOF
#!/bin/sh
echo "\$*" >> "$T/gh.calls"
case "\$3" in
  list) cat "$T/gh.keys" 2>/dev/null ;;
  add)  for a in "\$@"; do [ "\$prev" = "-t" ] && echo "1	\$a	read-only" >> "$T/gh.keys"; prev=\$a; done ;;
esac
EOF
chmod +x "$T/fakebin/gh"

join(){
  env PATH="$T/fakebin:$PATH" SUB_NET_DIR="$MAC" SUB_WORKER_EXEC="$T/vm-exec" \
      SUB_WORKER_REPO="$(pwd)" SUB_WORKER_REF="$REF" SUB_WORKER_NOAPT=1 SUB_WORKER_SERVICE=direct \
      "$W/join.sh" vm 127.0.0.1 /nonexistent-key-path --port "$PORT" --deploy-repo JoshuaRamirez/Desk-Workers "$@"
}
# what a re-run must leave exactly as it was
state(){
  ( cd "$T" && shasum vm/.local/bin/sub vm/.local/bin/sub-net vm/.local/bin/sub-dbd \
      vm/.ssh/substrate-worker-github_ed25519 vm/.ssh/substrate-worker-github_ed25519.pub vm/.ssh/config \
      vm/.config/systemd/user/substrate-dbd.service vm/.config/systemd/user/substrate-net.service \
      "vm/Library/Application Support/substrate/net/key" "vm/Library/Application Support/substrate/net/peers" \
      vm/.local/share/substrate/net.pid vm/.local/share/substrate/dbd.pid \
      mac/key mac/peers mac/workers/vm/key.pub mac/workers/vm/github.pub 2>&1 )
}

# -- the first join --------------------------------------------------------------
join > "$T/join1.out" 2>&1
check "the join runs all three steps and succeeds" "$?" "0"
check "the proof: sub-net ps vm, from the Mac, lists the worker's registry owner" \
      "$(awk '$2=="dbd" && $3=="owner"{n++} END{print n+0}' "$T/join1.out")" "1"
check "install put sub, sub-net and sub-dbd in the worker's ~/.local/bin" \
      "$(ls "$VM/.local/bin" | tr '\n' ' ')" "sub sub-dbd sub-net "
check "install built the commit asked for" \
      "$(git -C "$VM/.local/src/substrate" rev-parse HEAD)" "$REF"
check "the worker has its own GitHub key, readable only by it" \
      "$(stat -c %a "$VM/.ssh/substrate-worker-github_ed25519" 2>/dev/null || stat -f %Lp "$VM/.ssh/substrate-worker-github_ed25519")" "600"
check "and uses it for github.com" "$(grep -c 'IdentityFile .*substrate-worker-github_ed25519' "$VM/.ssh/config")" "1"
check "the worker trusts the Mac, which calls in only" \
      "$(env HOME="$VM" "$VM/.local/bin/sub-net" peers | awk '$1=="mac"{print $2,$3,$4}')" "(calls in only)"
check "the Mac trusts the worker, at the address it serves on" \
      "$(SUB_NET_DIR="$MAC" ./bin/net peers | awk '$1=="vm"{print $2}')" "127.0.0.1:$PORT"
check "the serve unit binds that one address, and comes back" \
      "$(grep -c -e "ExecStart=%h/.local/bin/sub-net serve --bind 127.0.0.1:$PORT\$" -e '^Restart=always$' "$VM/.config/systemd/user/substrate-net.service")" "2"
check "the registry has a unit too" "$(grep -c '^ExecStart=%h/.local/bin/sub-dbd ' "$VM/.config/systemd/user/substrate-dbd.service")" "1"
check "sub ps on the worker lists its registry" \
      "$("$T/vm-exec" '~/.local/bin/sub ps' | awk '$2=="dbd"{n++} END{print n+0}')" "1"
check "the worker's GitHub key went to the named repo, read-only" \
      "$(grep -c -e '^repo deploy-key add .*/workers/vm/github.pub -R JoshuaRamirez/Desk-Workers -t substrate-worker vm --allow-write=false$' "$T/gh.calls")" "1"

# -- the second join: nothing changes ------------------------------------------------
BEFORE=$(state)
join > "$T/join2.out" 2>&1
check "the second join succeeds" "$?" "0"
check "each step on the worker changes nothing, and says so" \
      "$(grep -c ': changed: nothing$' "$T/join2.out")" "3"
check "the Mac's trust is left as it was" "$(grep -c 'already trusting vm' "$T/join2.out")" "1"
check "no second deploy key" "$(grep -c 'deploy-key add' "$T/gh.calls") $(grep -c 'deploy key already on JoshuaRamirez/Desk-Workers' "$T/join2.out")" "1 1"
check "no file the setup owns, on either side, was touched" "$(state)" "$BEFORE"
check "and the proof still holds" "$(awk '$2=="dbd" && $3=="owner"{n++} END{print n+0}' "$T/join2.out")" "1"

# -- key paths, never key values -------------------------------------------------------
LEAK=0
for k in "$VM/.ssh/substrate-worker-github_ed25519" "$VM/Library/Application Support/substrate/net/key" "$MAC/key"; do
  # the secret's body lines, one at a time: none may appear in anything the join said
  grep -v -e '^-----' "$k" | while IFS= read -r l; do grep -qsF -- "$l" "$T/join1.out" "$T/join2.out" && echo leak; done | grep -q leak && LEAK=1
done
check "no secret key appears in what the join printed" "$LEAK" "0"
check "nor a public key's value: only its path" \
      "$(grep -c -F -e "$(cut -d' ' -f2 "$VM/.ssh/substrate-worker-github_ed25519.pub")" -e "$(cat "$MAC/key.pub")" "$T/join1.out" "$T/join2.out" | awk -F: '{s+=$NF} END{print s+0}')" "0"

# -- the undo ---------------------------------------------------------------------------
"$T/vm-exec" "SUB_WORKER_SERVICE=direct sh -s -- --undo" < "$W/serve.sh" > "$T/undo.out" 2>&1
check "serve.sh --undo stops the worker's adapter" \
      "$(kill -0 "$(cat "$VM/.local/share/substrate/net.pid.stopped")" 2>/dev/null && echo running || echo stopped)" "stopped"
SUB_NET_DIR="$MAC" "$W/join.sh" --undo vm > "$T/undo2.out" 2>&1
check "join.sh --undo: the Mac no longer trusts it" "$(SUB_NET_DIR="$MAC" ./bin/net peers | awk '$1=="vm"' | wc -l | tr -d ' ')" "0"

cleanup; trap - EXIT
[ $fail -eq 0 ] && exit 0 || exit 1
