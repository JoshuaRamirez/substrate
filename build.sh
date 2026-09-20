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

# shellcheck disable=SC2086
clang $OPT $WARN $HARD -std=c11 dbd.c  -o $OUT/dbd
clang $OPT $WARN $HARD -std=c11 webd.c -o $OUT/webd
clang $OPT $WARN $HARD -std=c11 wedge.c -o $OUT/wedge
clang $OPT -Wall -Wextra -Wshadow $HARD -fobjc-arc -framework Cocoa gui.m -o $OUT/gui
clang $OPT -Wall -Wextra -Wshadow $HARD -fobjc-arc -framework Cocoa top.m -o $OUT/top

echo "built${1:+ ($1)}:"
ls -lh $OUT/dbd $OUT/webd $OUT/gui $OUT/top | awk '{printf "  %-10s %s\n", $9, $5}'
