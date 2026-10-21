#!/bin/sh
# The system-change lane: `sub change` runs changes to the running system one
# at a time, in arrival order, and logs each one. A queue, not a gate: nothing
# is refused, a stuck change times out and frees the lane, a dead holder's
# lock is cleared. Run alone (./test-change.sh) or as part of ./test.sh.
# Everything here lives in a throwaway lane and a throwaway HOME: the
# machine's lane, log and LaunchAgents are never touched.
cd "$(dirname "$0")"
fail=0
ok()   { printf "  PASS  %s\n" "$1"; }
bad()  { printf "  FAIL  %s\n" "$1"; fail=1; }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1 (want '$3', got '$2')"; }
SUB="$(pwd)/bin/sub"

T=$(mktemp -d /tmp/subchange.XXXXXX)
export SUB_LANE="$T/lane" DESK_WHOAMI="lane-tester"
LOG="$SUB_LANE/changes.jsonl"
# the field of the Nth log line (1-based; -1 the last), by python, so the JSON is really parsed
field(){ python3 -c 'import json,sys; L=[json.loads(x) for x in open(sys.argv[1]) if x.strip()]; v=L[int(sys.argv[2]) if int(sys.argv[2])<0 else int(sys.argv[2])-1].get(sys.argv[3], "<absent>"); print(json.dumps(v) if isinstance(v,(list,dict)) or v is None else v)' "$LOG" "$1" "$2" 2>/dev/null; }
lines(){ [ -f "$LOG" ] && wc -l < "$LOG" | tr -d ' ' || echo 0; }
# wait until the lane shows a change running (the holder's entry carries "started")
# (or give up at once if the change just started in the background has already exited)
running(){ for _ in $(seq 1 100); do grep -qs '"started"' "$SUB_LANE"/queue/*.q && return 0; kill -0 $! 2>/dev/null || return 1; sleep 0.05; done; return 1; }

echo "23. the system-change lane: one change at a time, in arrival order"

# -- strictly one after another, in arrival order ---------------------------
O="$T/order"; : > "$O"
"$SUB" change "first" -- sh -c "echo start-1 >> $O; sleep 1; echo end-1 >> $O" >/dev/null 2>&1 &
running
for n in 2 3 4 5; do
  "$SUB" change "change $n" -- sh -c "echo start-$n >> $O; sleep 0.2; echo end-$n >> $O" >/dev/null 2>&1 &
  sleep 0.15        # arrival order is the order they asked
done
wait
check "concurrent changes never overlap, and run in arrival order" \
      "$(tr '\n' ' ' < "$O")" "start-1 end-1 start-2 end-2 start-3 end-3 start-4 end-4 start-5 end-5 "
check "each queued change is logged once" "$(lines)" "5"
check "the log's order is the arrival order" \
      "$(python3 -c 'import json,sys; print(" ".join(json.loads(x)["what"] for x in open(sys.argv[1])))' "$LOG")" \
      "first change 2 change 3 change 4 change 5"
check "tickets rise with arrival" \
      "$(python3 -c 'import json,sys; t=[json.loads(x)["ticket"] for x in open(sys.argv[1])]; print(t==sorted(t) and len(set(t))==len(t))' "$LOG")" "True"
check "a waiting change started only after the one before it ended" \
      "$(python3 -c 'import json,sys; L=[json.loads(x) for x in open(sys.argv[1])]; print(all(L[i+1]["started_s"]>=L[i]["ended_s"] for i in range(len(L)-1)))' "$LOG")" "True"

# -- many short changes at once: each logged exactly once, none mistaken for dead
#    (a waiter that opened an entry just before its owner unlinked it, and locked
#    it just after the owner closed it, once buried a clean change as holder-died)
N0=$(lines)
for n in $(seq 1 20); do "$SUB" change "burst $n" -- true >/dev/null 2>&1 & done
wait
check "20 changes at once: 20 lines" "$(( $(lines) - N0 ))" "20"
check "and every one ok" "$(grep -c '"what":"burst [0-9]*".*"status":"ok"' "$LOG")" "20"

# -- every field logged -------------------------------------------------------
"$SUB" change "write a setting" --undo "echo undone" --why "the suite asks" --timeout 30 -- sh -c 'exit 3' >/dev/null 2>&1
check "sub change exits with the command's exit code" "$?" "3"
check "logged: what"     "$(field -1 what)"    "write a setting"
check "logged: who (DESK_WHOAMI)" "$(field -1 who)" "lane-tester"
check "logged: why"      "$(field -1 why)"     "the suite asks"
check "logged: undo"     "$(field -1 undo)"    "echo undone"
check "logged: the command, argv as given" "$(field -1 cmd)" '["sh", "-c", "exit 3"]'
check "logged: the exit code" "$(field -1 exit)" "3"
check "logged: status failed on a non-zero exit" "$(field -1 status)" "failed"
check "logged: the timeout" "$(field -1 timeout)" "30"
check "logged: queued, started and ended, as local ISO times" \
      "$(for k in queued started ended; do field -1 $k; done | grep -cE '^20[0-9]{2}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}[+-][0-9]{2}:[0-9]{2}$')" "3"
check "logged: queued <= started <= ended" \
      "$(python3 -c 'import json,sys; L=json.loads(open(sys.argv[1]).readlines()[-1]); print(L["queued_s"]<=L["started_s"]<=L["ended_s"])' "$LOG")" "True"
"$SUB" change "a clean one" -- true >/dev/null 2>&1
check "logged: status ok on exit 0" "$(field -1 status)" "ok"
check "no --why or --undo is logged as empty, not missing" "$(field -1 undo)|$(field -1 why)" "|"
DESK_WHOAMI= HERDR_PANE_ID=p-test PATH="/usr/bin:/bin" "$SUB" change "who without a name" -- true >/dev/null 2>&1
check "who falls back to the herdr pane" "$(field -1 who)" "herdr pane p-test"
"$SUB" change "no such program" -- /nonexistent/prog >/dev/null 2>&1
check "a command that cannot run is logged too, exit 127" "$(field -1 exit)" "127"

# -- a stuck change times out and frees the lane --------------------------------
P="$T/pids"; : > "$P"
"$SUB" change "stuck" --timeout 1 -- sh -c "sleep 60 & echo \$! >> $P; echo \$\$ >> $P; sleep 60" >/dev/null 2>&1 &
running
"$SUB" change "after the stuck one" -- true >/dev/null 2>&1
wait
check "the stuck change is logged timed-out" \
      "$(python3 -c 'import json,sys; L=[json.loads(x) for x in open(sys.argv[1])]; s=[x for x in L if x["what"]=="stuck"][0]; print(s["status"], s["exit"])' "$LOG")" "timed-out 124"
# from the log, the lane's own clock, and as properties rather than tight times
# (on a loaded machine at background priority a process can wait seconds for a
# CPU): the stuck change was stopped long before its own 60s were up, and the
# next one ran after it ended, never beside it
check "and it was stopped at its timeout, not after its own 60s" \
      "$(python3 -c 'import json,sys; L={json.loads(x)["what"]: json.loads(x) for x in open(sys.argv[1])}; s=L["stuck"]; print(s["ended_s"]-s["started_s"] < 20)' "$LOG")" "True"
check "and the next one ran once it was killed, not beside it" \
      "$(python3 -c 'import json,sys; L={json.loads(x)["what"]: json.loads(x) for x in open(sys.argv[1])}; s=L["stuck"]; n=L["after the stuck one"]; print(n["started_s"] >= s["ended_s"])' "$LOG")" "True"
check "the next change is logged ok" "$(field -1 status)" "ok"
alive=0; for p in $(cat "$P"); do kill -0 "$p" 2>/dev/null && alive=$((alive+1)); done
check "its whole process group was killed" "$alive" "0"

# -- a dead holder's lock is cleared ------------------------------------------
: > "$P"
"$SUB" change "holder that dies" -- sh -c "echo \$\$ >> $P; sleep 60" >/dev/null 2>&1 &
HP=$!
running
kill -9 "$HP"; wait "$HP" 2>/dev/null
"$SUB" change "after the dead holder" -- true >/dev/null 2>&1
check "a change behind a dead holder runs, not after the holder's 60s (queued to started under 20s)" \
      "$(python3 -c 'import json,sys; n=[json.loads(x) for x in open(sys.argv[1]) if "\"after the dead holder\"" in x][0]; print(n["started_s"]-n["queued_s"] < 20)' "$LOG")" "True"
check "the dead holder is logged holder-died" \
      "$(python3 -c 'import json,sys; L=[json.loads(x) for x in open(sys.argv[1])]; print([x["status"] for x in L if x["what"]=="holder that dies"])' "$LOG")" "['holder-died']"
sleep 0.2
alive=0; for p in $(cat "$P"); do kill -0 "$p" 2>/dev/null && alive=$((alive+1)); done
check "and what it was running is stopped, so no two changes overlap" "$alive" "0"
# the holder and its group's leader both die, a worker of the group lives on: still stopped
: > "$P"
"$SUB" change "holder and leader die" -- sh -c "sleep 60 & echo \$! >> $P; wait" >/dev/null 2>&1 &
HP=$!
running
LEADER=$(python3 -c 'import json,glob,sys; print([json.loads(open(f).read())["pgid"] for f in glob.glob(sys.argv[1]+"/queue/*.q")][0])' "$SUB_LANE")
kill -9 "$HP" "$LEADER"; wait "$HP" 2>/dev/null
"$SUB" change "after the leaderless group" -- true >/dev/null 2>&1
sleep 0.2
alive=0; for p in $(cat "$P"); do kill -0 "$p" 2>/dev/null && alive=$((alive+1)); done
check "a leaderless group left by a dead holder is stopped too" "$alive" "0"
# an entry left by a process long gone (no lock held on it) is cleared too
mkdir -p "$SUB_LANE/queue"; echo '{"ticket":1,"what":"ghost"}' > "$SUB_LANE/queue/000000000001.q"
"$SUB" change "after a ghost" --timeout 5 -- true >/dev/null 2>&1
check "a stale entry nobody holds does not block the lane" "$(field -1 what) $(field -1 status)" "after a ghost ok"
check "and is cleared" "$(ls "$SUB_LANE/queue" | grep -c ghost; ls "$SUB_LANE/queue" | wc -l | tr -d ' ')" "0
0"

# -- sub changes ------------------------------------------------------------------
check "sub changes --last 2 shows two changes" "$("$SUB" changes --last 2 | grep -c '^#')" "2"
check "sub changes --json --last 1 is the log's last line" "$("$SUB" changes --json --last 1)" "$(tail -1 "$LOG")"
"$SUB" change "long one" -- sleep 4 >/dev/null 2>&1 &
running
"$SUB" change "waiting one" -- true >/dev/null 2>&1 &
for _ in $(seq 1 60); do      # until the waiting one has queued
  R=$("$SUB" changes --running); echo "$R" | grep -q '^waiting.*waiting one' && break; sleep 0.05
done
check "--running shows the current change"    "$(echo "$R" | grep -c '^running.*long one')" "1"
check "--running shows the queue behind it"   "$(echo "$R" | grep -c '^waiting.*waiting one')" "1"
wait
check "--running on a free lane says so" "$("$SUB" changes --running)" "(no change running)"

# -- overhead on a free lane ---------------------------------------------------------
# the lane's own cost, from its own clock: queued to started on a free lane
# (ticket, queue entry, the scan, the fork), best of 7. Wall time against a bare
# spawn is printed beside it; on a loaded machine at background priority the
# scheduler adds seconds to either at random, so it is shown, not judged.
OV=$(python3 - "$SUB" "$LOG" <<'PY'
import subprocess, sys, time, json
a, b = [], []
for i in range(7):
    t = time.time(); subprocess.run(["/usr/bin/true"]); b.append(time.time() - t)
    t = time.time(); subprocess.run([sys.argv[1], "change", f"overhead {i}", "--", "/usr/bin/true"], check=True); a.append(time.time() - t)
L = [json.loads(x) for x in open(sys.argv[2])][-7:]
lane = min(x["started_s"] - x["queued_s"] for x in L)
print(f"{lane:.3f} {max(0.0, min(a) - min(b)):.3f}")
PY
)
set -- $OV
check "overhead on a free lane is under 0.5s (lane ${1}s; wall, above a bare spawn, ${2}s)" "$(python3 -c "print($1 < 0.5)")" "True"

# -- sub restart goes through the lane ------------------------------------------------
TH="$T/home"; mkdir -p "$TH/Library/LaunchAgents"
: > "$TH/Library/LaunchAgents/dev.substrate.subfake.plist"
FL="$T/fake-launchctl"; FO="$T/launchctl.out"
printf '#!/bin/sh\necho "start $*" >> %s\nsleep 0.3\necho "end $*" >> %s\n' "$FO" "$FO" > "$FL"; chmod +x "$FL"
: > "$O"; : > "$FO"
"$SUB" change "hold the lane" -- sh -c "echo start-hold >> $O; sleep 0.8; echo end-hold >> $O" >/dev/null 2>&1 &
running
HOME="$TH" SUB_LAUNCHCTL="$FL" "$SUB" restart subfake >/dev/null 2>&1
check "sub restart exits 0" "$?" "0"
wait
check "sub restart kickstarts the agent" "$(head -1 "$FO")" "start kickstart -k gui/$(id -u)/dev.substrate.subfake"
check "sub restart waited for the change ahead of it" "$(tr '\n' ' ' < "$O")" "start-hold end-hold "
check "sub restart is logged in the lane" "$(field -1 what) $(field -1 status)" "restart subfake ok"
check "and ran after the holder ended" \
      "$(python3 -c 'import json,sys; L=[json.loads(x) for x in open(sys.argv[1])]; print(L[-1]["started_s"]>=L[-2]["ended_s"])' "$LOG")" "True"
HOME="$TH" SUB_LAUNCHCTL="$FL" "$SUB" restart nosuchagent >/dev/null 2>&1
check "restarting what is not enabled says so" "$?" "2"

# -- a change that makes a change: nested, not deadlocked -----------------------------
N0=$(lines)
HOME="$TH" SUB_LAUNCHCTL="$FL" "$SUB" change "outer" --timeout 10 -- "$SUB" restart subfake >/dev/null 2>&1
check "sub restart inside sub change runs (no deadlock)" "$?" "0"
check "both are logged" "$(( $(lines) - N0 ))" "2"
check "the inner one names the change it ran inside" \
      "$(python3 -c 'import json,sys; L=[json.loads(x) for x in open(sys.argv[1])]; print(L[-2]["what"], L[-2]["nested_in"]==L[-1]["ticket"])' "$LOG")" "restart subfake True"

mkdir -p /tmp/x2045/aside && mv "$T" /tmp/x2045/aside/ 2>/dev/null
[ $fail -eq 0 ] && echo "  lane: all pass" || echo "  lane: FAILURES"
exit $fail
