#!/bin/sh
# Three drivers and an owner. No package manager. No dependency tree.
#   ./build.sh            release
#   ./build.sh --san      ASan + UBSan, for running test.sh against
set -e
cd "$(dirname "$0")"
mkdir -p bin

WARN="-Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings -Wformat=2
      -Wformat-security -Wvla -Wcast-qual -Wmissing-prototypes -Wstrict-prototypes"
HARD="-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-common"

if [ "$1" = "--san" ]; then
  OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
  OUT="bin"
else
  OPT="-O2"
  OUT="bin"
fi

# The embedded table. Generated, not committed: 40 MB of it.
EMDB_N="${EMDB_N:-1000000}"
clang -O2 -std=c11 mkdb.c -o bin/mkdb
[ -f db.blob ] || ./bin/mkdb "$EMDB_N" db.blob
# -sectalign matters: emdb_rec holds a uint64_t, and a section the linker
# placed on an odd address makes every read of it undefined behaviour. UBSan
# caught this; arm64 tolerated it silently, which is exactly the problem.
EMBED="-Wl,-sectcreate,__TEXT,__emdb,db.blob -Wl,-sectalign,__TEXT,__emdb,8"

# shellcheck disable=SC2086
clang $OPT $WARN $HARD -std=c11 dbd.c  -o $OUT/dbd
clang $OPT $WARN $HARD -std=c11 webd.c -o $OUT/webd $EMBED
clang $OPT $WARN $HARD -std=c11 wedge.c -o $OUT/wedge
clang $OPT $WARN $HARD -std=c11 bench.c -o $OUT/bench
clang $OPT $WARN $HARD -std=c11 hog.c   -o $OUT/hog
clang $OPT $WARN $HARD -std=c11 look.c  -o $OUT/look $EMBED
clang $OPT $WARN $HARD -std=c11 layout.c -o $OUT/layout

# A second compiled toolchain, sharing nothing with the above but the format.
if command -v swiftc >/dev/null 2>&1; then
  swiftc -O swiftpeer.swift -o $OUT/swiftpeer 2>/dev/null || \
    echo "  (swiftpeer skipped: swiftc failed)"
fi
clang $OPT -Wall -Wextra -Wshadow $HARD -fobjc-arc -framework Cocoa gui.m -o $OUT/gui
clang $OPT -Wall -Wextra -Wshadow $HARD -fobjc-arc -framework Cocoa top.m -o $OUT/top

echo "built${1:+ ($1)}:"
ls -lh $OUT/dbd $OUT/webd $OUT/gui $OUT/top $OUT/look $OUT/swiftpeer 2>/dev/null | awk '{printf "  %-10s %s\n", $9, $5}'
