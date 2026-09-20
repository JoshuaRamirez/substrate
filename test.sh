#!/bin/sh
# Runs the falsification criteria from the concept. Exits non-zero on failure.
cd "$(dirname "$0")"
PORT=8099
fail=0
ok()   { printf "  PASS  %s\n" "$1"; }
bad()  { printf "  FAIL  %s\n" "$1"; fail=1; }
check(){ [ "$2" = "$3" ] && ok "$1" || bad "$1 (want '$3', got '$2')"; }

# webd is 38MB (it carries the embedded table), so its FIRST launch after a
# build reads it cold off disk. Wait for the port rather than guessing a sleep.
waitport(){
  for _ in $(seq 1 300); do
    if curl -s -o /dev/null --max-time 2 "localhost:$1/status" 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  return 1
}


pkill -f 'bin/(dbd|webd)' 2>/dev/null
rm -f counter.db; sleep 0.3

echo "1. each binary runs ALONE, self-contained"
./bin/webd --port $PORT >/dev/null 2>&1 &
WP=$!; waitport $PORT
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
./bin/webd --join --port $PORT >/dev/null 2>&1 & WP=$!; waitport $PORT
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
./bin/webd --join --port 8102 >/dev/null 2>&1 & W2=$!; waitport 8102
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
./bin/webd --join --port 8103 >/dev/null 2>&1 & W3=$!; waitport 8103
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

./bin/webd --join --port 8105 >/dev/null 2>&1 & W5=$!; waitport 8105
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
./bin/webd --join --port 8106 >/dev/null 2>&1 & W6=$!; waitport 8106
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
./bin/webd --port 8140 >/dev/null 2>&1 & WA=$!; waitport 8140
check "alone: first range starts at 1" \
      "$(curl -s 'localhost:8140/reserve?n=10' | sed -n 2p | tr -d ' ')" "base=1"
check "alone: the next range follows it" \
      "$(curl -s 'localhost:8140/reserve?n=5' | sed -n 2p | tr -d ' ')" "base=11"
check "alone: mode is still alone" \
      "$(curl -s 'localhost:8140/reserve?n=1' | sed -n 4p | tr -d ' ')" "mode=alone"
kill $WA 2>/dev/null; wait $WA 2>/dev/null

./bin/dbd >/tmp/dbdF.log 2>&1 & DF=$!; sleep 0.5
./bin/webd --join --port 8141 >/dev/null 2>&1 & W7=$!
./bin/webd --join --port 8142 >/dev/null 2>&1 & W8=$!; waitport 8142
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
./bin/webd --join --port 8143 >/dev/null 2>&1 & W9=$!; waitport 8143
check "a dead caller's slots are reclaimed" \
      "$(curl -s 'localhost:8143/reserve?n=1' | head -1)" "reserved 1 ids"
kill $W9 2>/dev/null; wait $W9 2>/dev/null

./bin/webd --join --port 8144 >/dev/null 2>&1 & WB=$!; waitport 8144
kill -TERM $DG 2>/dev/null; wait $DG 2>/dev/null; sleep 0.5
R=$(curl -s 'localhost:8144/reserve?n=1' | tail -1 | tr -d ' ')
check "reserve falls back to local ids when detached" "$R" "mode=detached"
kill $WB 2>/dev/null; wait $WB 2>/dev/null

echo "16. the database that lives inside the executable"
check "look answers with no owner, no files, no network" \
      "$(cd /tmp && "$OLDPWD/bin/look" k000000042)" "111486301962"
check "and the last record too" \
      "$(cd /tmp && "$OLDPWD/bin/look" k000999999)" "2654433106564239"
check "a miss is a miss" "$(./bin/look nosuchkey 2>&1)" "miss"
check "it reports a million records" \
      "$(./bin/look --stat | head -1 | tr -d ' ')" "records=1000000"

rm -f counter.db
./bin/webd --port 8145 >/dev/null 2>&1 & WC=$!; waitport 8145
check "webd carries the same table, alone" \
      "$(curl -s 'localhost:8145/lookup?k=k000000042' | sed -n 3p | tr -d ' ')" "value=111486301962"
check "and it needs no owner to answer" \
      "$(curl -s 'localhost:8145/lookup?k=k000000042' | head -1)" "hit"
kill $WC 2>/dev/null; wait $WC 2>/dev/null

./bin/dbd >/tmp/dbdH.log 2>&1 & DH=$!; sleep 0.5
./bin/webd --join --port 8146 >/dev/null 2>&1 & WD=$!; waitport 8146
check "one process, two databases: the private one" \
      "$(curl -s 'localhost:8146/lookup?k=k000000007' | head -1)" "hit"
check "one process, two databases: the shared one" \
      "$(curl -s 'localhost:8146/reserve?n=2' | sed -n 2p | tr -d ' ')" "base=1"
kill $WD 2>/dev/null; wait $WD 2>/dev/null
kill -TERM $DH 2>/dev/null; wait $DH 2>/dev/null

echo "17. the format is the contract: three languages, one page"
L=$(./bin/layout)
val(){ echo "$L" | awk -v k="$1" '$1==k{print $2}'; }
segsize=$(echo "$L" | awk '/^size/{print $2}')

# The Swift and Python peers hardcode these. If C moves and they do not, they
# would read garbage and call it agreement. Assert they still line up.
check "swift knows the segment size"  "$(grep -o 'SEG_SIZE = [0-9]*' swiftpeer.swift | grep -o '[0-9]*')" "$segsize"
check "python knows the segment size" "$(grep 'SEG_SIZE' pypeer.py | grep -o '[0-9]*$' | head -1)" "$segsize"
check "swift knows OFF_COUNT"  "$(grep -o 'OFF_COUNT *= *[0-9]*' swiftpeer.swift | grep -o '[0-9]*$')" "$(val count)"
check "python knows OFF_COUNT" "$(grep -o 'OFF_COUNT, OFF_OWNER_PID, OFF_NEXT_ID = [0-9]*' pypeer.py | grep -o '[0-9]*$')" "$(val count)"
check "swift knows OFF_RING"   "$(grep -o 'OFF_RING *= *[0-9]*' swiftpeer.swift | grep -o '[0-9]*$')" "$(val ring)"
check "swift knows OFF_PEERS"  "$(grep -o 'OFF_PEERS *= *[0-9]*' swiftpeer.swift | grep -o '[0-9]*$')" "$(val peers)"
cmagic=$(echo "$L" | awk '/^magic/{print $2; exit}')
check "swift knows the magic"  "$(grep -o '0x434E543[0-9]' swiftpeer.swift | head -1)" "$cmagic"
check "python knows the magic" "$(grep -o '0x434E543[0-9]' pypeer.py | head -1)" "$cmagic"

rm -f counter.db
./bin/dbd >/tmp/dbdI.log 2>&1 & DI=$!; sleep 0.5
./bin/webd --join --port 8147 >/dev/null 2>&1 & WE=$!; waitport 8147
curl -s localhost:8147/bump >/dev/null; curl -s localhost:8147/bump >/dev/null   # C: 2
check "swift joins and agrees"  "$(./bin/swiftpeer --bump 3 | head -1)" "swiftpeer: bumped 3, count=5"
check "python joins and agrees" "$(./pypeer.py --bump 4 | head -1)" "pypeer: bumped 4, count=9"
check "C sees both of them"     "$(curl -s localhost:8147/status | head -1 | tr -d ' ')" "count=9"
check "swift can use the ring"  "$(./bin/swiftpeer --bump 0 --reserve 5 | sed -n 2p)" "swiftpeer: reserved 5, base=1, last=5"
check "python can use the ring" "$(./pypeer.py --bump 0 --reserve 5 | sed -n 2p)" "pypeer: reserved 5, base=6, last=10"
check "C continues the same id space" "$(curl -s 'localhost:8147/reserve?n=2' | sed -n 2p | tr -d ' ')" "base=11"
kill $WE 2>/dev/null; wait $WE 2>/dev/null
kill -TERM $DI 2>/dev/null; wait $DI 2>/dev/null

echo "18. contention at scale, and backpressure that waits instead of losing"
rm -f counter.db
./bin/dbd >/tmp/dbdJ.log 2>&1 & DJ=$!; sleep 0.5
for n in 8 64 128; do
  R=$(./bin/storm $n 500 2>/dev/null)
  E=$?
  j=$(echo "$R" | awk '/joined/{print $4}')
  d=$(echo "$R" | awk '/duplicates/{print $2}')
  g=$(echo "$R" | awk '/gaps/{print $5}')
  p=$(echo "$R" | awk '/dropped/{print $3}')
  check "$n clients all joined"        "$j" "$n"
  check "$n clients: no duplicate ids" "$d" "0"
  check "$n clients: no lost ids"      "$g" "0"
  check "$n clients: nothing dropped"  "$p" "0"
  check "$n clients: exit clean"       "$E" "0"
done
kill -TERM $DJ 2>/dev/null; wait $DJ 2>/dev/null

echo "19. payload transfer: bytes that cross the boundary without being copied"
rm -f counter.db /tmp/pl.bin /tmp/pl.out
./bin/dbd >/tmp/dbdK.log 2>&1 & DK=$!; sleep 0.5
./bin/webd --join --port 8148 >/dev/null 2>&1 & WF=$!; waitport 8148
head -c 900000 /dev/urandom > /tmp/pl.bin
SHA=$(shasum -a 256 /tmp/pl.bin | cut -c1-16)

R=$(curl -s -X PUT --data-binary @/tmp/pl.bin 'localhost:8148/blob?k=42')
check "a 900KB payload is accepted" "$(echo "$R" | grep -o '\"ok\":true')" '"ok":true'
check "and its length is right"     "$(echo "$R" | grep -o '\"len\":900000')" '"len":900000'
curl -s 'localhost:8148/blob?k=42' -o /tmp/pl.out
check "it comes back byte-identical" "$(cmp -s /tmp/pl.bin /tmp/pl.out && echo same)" "same"
check "python reads the SAME bytes from the arena, zero-copy" \
      "$(./pypeer.py --get 42 | sed -n 2p | grep -o 'sha=[0-9a-f]*' | cut -d= -f2)" "$SHA"
check "a missing blob is a miss" \
      "$(curl -s -o /dev/null -w '%{http_code}' 'localhost:8148/blob?k=999')" "404"

# the arena is finite, and running out is backpressure with a status code
FREE0=$(curl -s -X PUT --data-binary @/tmp/pl.bin 'localhost:8148/blob?k=43' | grep -o '"blocks_free":[0-9]*' | cut -d: -f2)
check "each payload consumes a block" "$([ "$FREE0" -lt 64 ] && echo yes)" "yes"
check "replacing a key frees the old block" \
      "$(curl -s -X PUT --data-binary @/tmp/pl.bin 'localhost:8148/blob?k=43' | grep -o '\"ok\":true')" '"ok":true'
check "an oversized payload is refused, not truncated" \
      "$(head -c 1100000 /dev/urandom | curl -s -X PUT --data-binary @- 'localhost:8148/blob?k=44' \
         -o /dev/null -w '%{http_code}')" "507"
kill $WF 2>/dev/null; wait $WF 2>/dev/null
kill -TERM $DK 2>/dev/null; wait $DK 2>/dev/null
rm -f /tmp/pl.bin /tmp/pl.out

echo "20. another OS, another libc, and another user"
if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
  echo "  SKIP  docker not available (cross-OS checks not run)"
else
  docker build -q -f Dockerfile.linux -t cntpipe:linux . >/dev/null 2>&1
  OUT=$(docker run --rm cntpipe:linux bash -c '
    ./bin/layout | awk "/^size/{print \"SIZE \" \$2} /^magic/ && !m {print \"MAGIC \" \$2; m=1}"
    ./bin/dbd >/dev/null 2>&1 & sleep 1
    echo "LOOK $(./bin/look k000000042)"
    ./bin/storm 64 500 2>/dev/null | awk "/duplicates/{print \"DUP \" \$2} /gaps/{print \"GAP \" \$5} /dropped/{print \"DROP \" \$3} /joined/{print \"JOIN \" \$4}"
    useradd -m intruder 2>/dev/null
    su intruder -c "/src/bin/storm 1 1" >/dev/null 2>&1; echo "OTHERUSER $?"
    stat -c "%a" /dev/shm/cnt.v7 | sed "s/^/MODE /"
  ' 2>/dev/null)
  g(){ echo "$OUT" | awk -v k="$1" '$1==k{print $2}'; }
  check "linux computes the same segment size" "$(g SIZE)"  "$(./bin/layout | awk '/^size/{print $2}')"
  check "linux computes the same magic"        "$(g MAGIC)" "$(./bin/layout | awk '/^magic/{print $2; exit}')"
  check "the embedded table reads the same via ELF" "$(g LOOK)" "111486301962"
  check "64 clients on linux: all joined"      "$(g JOIN)" "64"
  check "64 clients on linux: no duplicates"   "$(g DUP)"  "0"
  check "64 clients on linux: no lost ids"     "$(g GAP)"  "0"
  check "64 clients on linux: nothing dropped" "$(g DROP)" "0"
  check "the segment is owner-only"            "$(g MODE)" "600"
  check "another user cannot join"             "$(g OTHERUSER)" "1"
fi

rm -f counter.db
[ $fail -eq 0 ] && echo "\nALL PASS" || echo "\nFAILURES"
exit $fail
