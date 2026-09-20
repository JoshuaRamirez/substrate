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
grep -qE "CNT_VERSION +[0-9]+u" counter.h && ok "segment carries magic + version" \
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

echo "8. the database can be moved while everything is running -- admin only"
rm -f counter.db /tmp/moved.db
./bin/dbd >/tmp/dbd4.log 2>&1 & DP=$!; sleep 0.4
./bin/webd --join --port 8103 >/dev/null 2>&1 & W3=$!; sleep 0.4
curl -s localhost:8103/bump >/dev/null            # -> 1, into ./counter.db
sleep 0.3
TOK=$(./bin/top --token)
AUTH="X-Admin-Token: $TOK"
MOVE="localhost:8103/admin/dbfile?path=%2Ftmp%2Fmoved.db"
code(){ curl -s -o /dev/null -w '%{http_code}' "$@"; }
okf(){ curl -s -X POST -H "$AUTH" "$1" | grep -o '"ok":[a-z]*'; }

check "the token is 128 bits of hex" "$(printf %s "$TOK" | wc -c | tr -d ' ')" "32"
check "no token is 401"    "$(code -X POST "$MOVE")" "401"
check "wrong token is 401" "$(code -X POST -H 'X-Admin-Token: 00000000000000000000000000000000' "$MOVE")" "401"
check "GET is 405 even with the token" "$(code -H "$AUTH" "$MOVE")" "405"
check "the ungated route is gone" "$(code 'localhost:8103/dbfile?path=%2Ftmp%2Fmoved.db')" "404"
check "nothing moved while unauthorized" "$(cat /tmp/moved.db 2>/dev/null)" ""

D=$(curl -s localhost:8103/status | sed -n 3p | sed 's/.*= //')
check "webd reports the db path" "$(basename "$D")" "counter.db"
check "POST with the token moves it" "$(okf "$MOVE")" '"ok":true'
sleep 0.4
check "the new file has the count" "$(cat /tmp/moved.db 2>/dev/null)" "1"
curl -s localhost:8103/bump >/dev/null            # -> 2, into /tmp/moved.db
sleep 0.4
check "new bumps land in the new file" "$(cat /tmp/moved.db 2>/dev/null)" "2"
check "the old file kept its last value" "$(cat counter.db 2>/dev/null)" "1"
D=$(curl -s localhost:8103/status | sed -n 3p | sed 's/.*= //')
check "the path is visible to peers" "$D" "/tmp/moved.db"
check "a relative path is refused" \
      "$(okf 'localhost:8103/admin/dbfile?path=relative.db')" '"ok":false'
check "an unwritable path is accepted then reverted" \
      "$(okf 'localhost:8103/admin/dbfile?path=%2Fno%2Fsuch%2Fdir%2Fx.db')" '"ok":true'
sleep 0.4
D=$(curl -s localhost:8103/status | sed -n 3p | sed 's/.*= //')
check "dbd stayed on the working file" "$D" "/tmp/moved.db"
check "the page carries the token for same-origin js" \
      "$(curl -s localhost:8103/ | grep -c "const TOK='$TOK'")" "1"
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

echo "11. the owner cannot be displaced, and peers survive its restart"
rm -f counter.db
./bin/dbd >/tmp/dbdA.log 2>&1 & DA=$!; sleep 0.5
./bin/dbd >/tmp/dbdB.log 2>&1; RC=$?
check "a second dbd refuses to start" "$RC" "3"
grep -q "owner already running" /tmp/dbdB.log && ok "and says why" || bad "and says why"
T=$(./bin/top --selftest 2>&1 | head -1)
check "the first owner is untouched" "$T" "top selftest: peers=1 count=0"

./bin/webd --join --port 8105 >/dev/null 2>&1 & W5=$!; sleep 0.5
curl -s localhost:8105/bump >/dev/null; curl -s localhost:8105/bump >/dev/null
M=$(curl -s localhost:8105/status | sed -n 2p | tr -d ' ')
check "webd is joined" "$M" "mode=joined"

kill -TERM $DA 2>/dev/null; wait $DA 2>/dev/null; sleep 0.5
M=$(curl -s localhost:8105/status | sed -n 2p | tr -d ' ')
check "webd notices the owner left" "$M" "mode=detached"

./bin/dbd >/tmp/dbdC.log 2>&1 & DC=$!; sleep 0.7
curl -s localhost:8105/status >/dev/null            # one request to revalidate
M=$(curl -s localhost:8105/status | sed -n 2p | tr -d ' ')
check "and re-attaches when one returns" "$M" "mode=joined"
T=$(./bin/top --selftest 2>&1 | head -1)
check "the new owner sees the old peer" "$T" "top selftest: peers=2 count=2"
kill $W5 2>/dev/null; wait $W5 2>/dev/null
kill -TERM $DC 2>/dev/null; wait $DC 2>/dev/null

echo "12. a killed writer cannot wedge the path"
rm -f counter.db
./bin/dbd >/tmp/dbdD.log 2>&1 & DD=$!; sleep 0.5
./bin/wedge >/dev/null 2>&1                          # takes path_seq odd, then dies
sleep 0.6
grep -q "repaired a path seqlock" /tmp/dbdD.log && ok "dbd repaired the seqlock" \
                                                || bad "dbd repaired the seqlock"
./bin/webd --join --port 8106 >/dev/null 2>&1 & W6=$!; sleep 0.5
TOK=$(./bin/top --token)
R=$(curl -s -X POST -H "X-Admin-Token: $TOK" \
        "localhost:8106/admin/dbfile?path=%2Ftmp%2Fafter.db" | grep -o '"ok":[a-z]*')
check "the path still works afterwards" "$R" '"ok":true'
kill $W6 2>/dev/null; wait $W6 2>/dev/null
kill -TERM $DD 2>/dev/null; wait $DD 2>/dev/null
rm -f /tmp/after.db

echo "13. persistence is crash-safe"
rm -f counter.db counter.db.tmp
./bin/dbd >/tmp/dbdE.log 2>&1 & DE=$!; sleep 0.4
./bin/gui --selftest --join >/dev/null 2>&1; sleep 0.4
check "no temp file is left behind" "$(ls counter.db.tmp 2>/dev/null)" ""
check "the count is there in full" "$(cat counter.db)" "1"
kill -TERM $DE 2>/dev/null; wait $DE 2>/dev/null

echo "14. reserve: the verb whose answer the caller waits for"
rm -f counter.db
./bin/webd --port 8140 >/dev/null 2>&1 & WA=$!; sleep 0.4
check "alone: first range starts at 1" \
      "$(curl -s 'localhost:8140/reserve?n=10' | sed -n 2p | tr -d ' ')" "base=1"
check "alone: the next range follows it" \
      "$(curl -s 'localhost:8140/reserve?n=5' | sed -n 2p | tr -d ' ')" "base=11"
check "alone: mode is still alone" \
      "$(curl -s 'localhost:8140/reserve?n=1' | sed -n 4p | tr -d ' ')" "mode=alone"
kill $WA 2>/dev/null; wait $WA 2>/dev/null

./bin/dbd >/tmp/dbdF.log 2>&1 & DF=$!; sleep 0.5
./bin/webd --join --port 8141 >/dev/null 2>&1 & W7=$!
./bin/webd --join --port 8142 >/dev/null 2>&1 & W8=$!; sleep 0.6
check "joined: same call site, same answers" \
      "$(curl -s 'localhost:8141/reserve?n=10' | sed -n 2p | tr -d ' ')" "base=1"
check "joined: mode is joined" \
      "$(curl -s 'localhost:8141/reserve?n=1' | sed -n 4p | tr -d ' ')" "mode=joined"
check "two separate processes share one id space" \
      "$(curl -s 'localhost:8142/reserve?n=4' | sed -n 2p | tr -d ' ')" "base=12"
check "and the range is contiguous" \
      "$(curl -s 'localhost:8142/reserve?n=4' | sed -n 3p | tr -d ' ')" "last=19"
kill $W7 $W8 2>/dev/null; wait $W7 2>/dev/null; wait $W8 2>/dev/null
kill -TERM $DF 2>/dev/null; wait $DF 2>/dev/null

echo "15. the ring survives its callers"
rm -f counter.db
./bin/dbd >/tmp/dbdG.log 2>&1 & DG=$!; sleep 0.5
./bin/hog >/dev/null 2>&1                            # claims every slot, then dies
sleep 0.6
./bin/webd --join --port 8143 >/dev/null 2>&1 & W9=$!; sleep 0.5
check "a dead caller's slots are reclaimed" \
      "$(curl -s 'localhost:8143/reserve?n=1' | head -1)" "reserved 1 ids"
kill $W9 2>/dev/null; wait $W9 2>/dev/null

./bin/webd --join --port 8144 >/dev/null 2>&1 & WB=$!; sleep 0.5
kill -TERM $DG 2>/dev/null; wait $DG 2>/dev/null; sleep 0.5
R=$(curl -s 'localhost:8144/reserve?n=1' | tail -1 | tr -d ' ')
check "reserve falls back to local ids when detached" "$R" "mode=detached"
kill $WB 2>/dev/null; wait $WB 2>/dev/null

rm -f counter.db
[ $fail -eq 0 ] && echo "\nALL PASS" || echo "\nFAILURES"
exit $fail
