#!/bin/sh
# Three single-file executables. No package manager. No dependency tree.
set -e
cd "$(dirname "$0")"
mkdir -p bin
CFLAGS="-O2 -Wall -Wextra -std=c11"

clang $CFLAGS                 dbd.c  -o bin/dbd
clang $CFLAGS                 webd.c -o bin/webd
clang -O2 -Wall -Wextra -fobjc-arc -framework Cocoa gui.m -o bin/gui

echo "built:"
ls -lh bin/dbd bin/webd bin/gui | awk '{printf "  %-10s %s\n", $9, $5}'
