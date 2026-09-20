#!/bin/sh
# The two-minute demo.
cd "$(dirname "$0")"
[ -x bin/dbd ] || ./build.sh

case "$1" in
alone)
  echo "Each binary is complete by itself. No owner, no segment, no peers."
  echo "  ./bin/webd            # then: curl localhost:8080/bump"
  echo "  ./bin/gui             # then: click bump"
  echo "Their counts are private to each process."
  ;;
*)
  trap 'kill 0' EXIT INT TERM
  ./bin/dbd &                 sleep 0.5
  ./bin/webd --join &         sleep 0.4
  echo ""
  echo "  curl localhost:8080/bump   -> watch the number change in the window"
  echo "  click bump in the window   -> watch dbd print the new count here"
  echo "  ctrl-c to stop everything"
  echo ""
  ./bin/gui --join
  ;;
esac
