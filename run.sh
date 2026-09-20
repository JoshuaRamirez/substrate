#!/bin/sh
# The demo.
cd "$(dirname "$0")"
[ -x bin/top ] || ./build.sh

case "$1" in
alone)
  echo "Each binary is complete by itself. No owner, no segment, no peers."
  echo "  ./bin/webd            # then: curl localhost:8080/bump"
  echo "  ./bin/gui             # then: click bump"
  ;;
top)
  ./bin/top                     # dashboard alone; tolerates an absent owner
  ;;
*)
  trap 'kill 0' EXIT INT TERM
  ./bin/dbd &           sleep 0.5
  ./bin/webd --join &   sleep 0.3
  ./bin/gui  --join &   sleep 0.3
  echo ""
  echo "  curl localhost:8080/bump   -> the number moves in the gui AND in top"
  echo "  click stop in top          -> that peer's row goes grey, then vanishes"
  echo "  ctrl-c here                -> stops everything"
  echo ""
  ./bin/top
  ;;
esac
