#!/bin/sh
# A shim. The Makefile is the build; this is here because test.sh, run.sh and
# a decade of muscle memory all reach for ./build.sh.
#   ./build.sh          release      (= make)
#   ./build.sh --san    ASan+UBSan   (= make san)
set -e
cd "$(dirname "$0")"
case "$1" in
  --san) exec make san ;;
  "")    exec make ;;
  *)     echo "usage: $0 [--san]" >&2; exit 2 ;;
esac
