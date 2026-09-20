#!/bin/sh
# Runs the falsification criteria from the concept. Exits non-zero on failure.
cd "$(dirname "$0")"
PORT=8099
fail=0
ok()   { printf "  PASS  %s\n" "$1"; }
bad()  { printf "  FAIL  %s\n" "$1"; fail=1; }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1 (want '$3', got '$2')"; }

pkill -f 'bin/(dbd|webd)' 2>/dev/null
rm -f counter.db; sleep 0.3

echo "1. each binary runs ALONE, self-contained"
./bin/webd --port $PORT >/dev/null 2>&1 &
WP=$!; sleep 0.4
curl -s localhost:$PORT/bump >/dev/null; curl -s localhost:$PORT/bump >/dev/null
R=$(curl -s localhost:$PORT/status | head -1 | tr -d ' ')
check "webd alone counts" "$R" "count=2"
M=$(curl -s localhost:$PORT/status | sed -n 2p | tr -d ' ')
check "webd alone reports mode" "$M" "mode=alone"
kill $WP 2>/dev/null; wait $WP 2>/dev/null

G=$(./bin/gui --selftest 2>&1)
check "gui alone counts" "$G" "gui selftest: mode=alone count=1"

echo "2. joining without an owner is refused, not silently faked"
./bin/gui --selftest --join >/dev/null 2>&1
check "gui --join exits 2 with no dbd" "$?" "2"

echo "3. the pipeline: one cell, three processes"
./bin/dbd >/tmp/dbd.log 2>&1 & DP=$!; sleep 0.4
./bin/webd --join --port $PORT >/dev/null 2>&1 & WP=$!; sleep 0.4
curl -s localhost:$PORT/bump >/dev/null      # webd writes  -> 1
curl -s localhost:$PORT/bump >/dev/null      # webd writes  -> 2
G=$(./bin/gui --selftest --join 2>&1)        # gui writes   -> 3
check "gui sees webd's writes" "$G" "gui selftest: mode=joined count=3"
R=$(curl -s localhost:$PORT/status | head -1 | tr -d ' ')
check "webd sees gui's write" "$R" "count=3"
M=$(curl -s localhost:$PORT/status | sed -n 2p | tr -d ' ')
check "webd reports joined" "$M" "mode=joined"
sleep 0.4
grep -q "count=3" /tmp/dbd.log && ok "dbd observed count=3" || bad "dbd observed count=3"

echo "4. the owner persists state across its own restart"
kill -TERM $DP 2>/dev/null; wait $DP 2>/dev/null
kill $WP 2>/dev/null; wait $WP 2>/dev/null; sleep 0.3
./bin/dbd >/tmp/dbd2.log 2>&1 & DP=$!; sleep 0.4
G=$(./bin/gui --selftest --join 2>&1)
check "count survived dbd restart" "$G" "gui selftest: mode=joined count=4"
kill -TERM $DP 2>/dev/null; wait $DP 2>/dev/null

echo "5. the format is pinned, not the binary"
grep -q "CNT_VERSION   3" counter.h && ok "segment carries magic + version" \
                                   || bad "segment carries magic + version"

echo "6. the registry: the dashboard reads shm, not ps"
rm -f counter.db   # section 4 left a persisted count; start this one clean
./bin/dbd >/tmp/dbd3.log 2>&1 & DP=$!; sleep 0.4
./bin/webd --join --port 8101 >/dev/null 2>&1 & W1=$!
./bin/webd --join --port 8102 >/dev/null 2>&1 & W2=$!
sleep 0.5
T=$(./bin/top --selftest 2>&1 | head -1)
check "top sees dbd + 2 webd" "$T" "top selftest: peers=3 count=0"
curl -s localhost:8101/bump >/dev/null
T=$(./bin/top --selftest 2>&1 | head -1)
check "top sees the shared count" "$T" "top selftest: peers=3 count=1"

echo "7. dead peers are reaped by the owner"
kill $W2 2>/dev/null; wait $W2 2>/dev/null; sleep 0.6
T=$(./bin/top --selftest 2>&1 | head -1)
check "peer table shrank after a kill" "$T" "top selftest: peers=2 count=1"
kill $W1 2>/dev/null; wait $W1 2>/dev/null
kill -TERM $DP 2>/dev/null; wait $DP 2>/dev/null

echo "8. the database can be moved while everything is running"
rm -f counter.db /tmp/moved.db
./bin/dbd >/tmp/dbd4.log 2>&1 & DP=$!; sleep 0.4
./bin/webd --join --port 8103 >/dev/null 2>&1 & W3=$!; sleep 0.4
curl -s localhost:8103/bump >/dev/null            # -> 1, into ./counter.db
sleep 0.3
D=$(curl -s localhost:8103/status | sed -n 3p | sed 's/.*= //')
check "webd reports the db path" "$(basename "$D")" "counter.db"
R=$(curl -s "localhost:8103/dbfile?path=%2Ftmp%2Fmoved.db" | head -1)
check "move accepted" "$R" "asked"
sleep 0.4
check "the new file has the count" "$(cat /tmp/moved.db 2>/dev/null)" "1"
curl -s localhost:8103/bump >/dev/null            # -> 2, into /tmp/moved.db
sleep 0.4
check "new bumps land in the new file" "$(cat /tmp/moved.db 2>/dev/null)" "2"
check "the old file kept its last value" "$(cat counter.db 2>/dev/null)" "1"
D=$(curl -s localhost:8103/status | sed -n 3p | sed 's/.*= //')
check "the path is visible to peers" "$D" "/tmp/moved.db"
R=$(curl -s "localhost:8103/dbfile?path=relative.db" | head -1)
check "a relative path is refused" "$R" "refused"
R=$(curl -s "localhost:8103/dbfile?path=%2Fno%2Fsuch%2Fdir%2Fx.db" | head -1)
check "an unwritable path is accepted then reverted" "$R" "asked"
sleep 0.4
D=$(curl -s localhost:8103/status | sed -n 3p | sed 's/.*= //')
check "dbd stayed on the working file" "$D" "/tmp/moved.db"
kill $W3 2>/dev/null; wait $W3 2>/dev/null
kill -TERM $DP 2>/dev/null; wait $DP 2>/dev/null
rm -f /tmp/moved.db

echo "9. the persisted file is chosen at startup too"
rm -f counter.db /tmp/start.db; echo 41 > /tmp/start.db
./bin/dbd /tmp/start.db >/tmp/dbd5.log 2>&1 & DP=$!; sleep 0.4
G=$(./bin/gui --selftest --join 2>&1)
check "dbd restored from an argv path" "$G" "gui selftest: mode=joined count=42"
kill -TERM $DP 2>/dev/null; wait $DP 2>/dev/null; sleep 0.3
check "and persisted back to it" "$(cat /tmp/start.db)" "42"
check "without touching the default" "$(cat counter.db 2>/dev/null)" ""
rm -f /tmp/start.db

echo "10. the dashboard tolerates an absent owner"
./bin/top --selftest >/dev/null 2>&1
check "top --selftest exits 2 with no dbd" "$?" "2"

rm -f counter.db
[ $fail -eq 0 ] && echo "\nALL PASS" || echo "\nFAILURES"
exit $fail
