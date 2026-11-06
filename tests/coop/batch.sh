#!/bin/bash
# Run a batch of multi-copy co-op tests. Each argument: "copies bot mode seed frames".
# Usage: batch.sh <out prefix> "2 fight ffa 31 10800" "4 chaos owncrew 7 3600" ...
# Prints one line per test and a final tally; exit code 1 if any test failed.
HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(readlink -f "$HERE/../..")
GAME=$(readlink -f "$ROOT/../game-test")
SO="$ROOT/build-linux-1.6.13-release/Hyperspace.1.6.13.amd64.so"
PREFIX=$1; shift
pass=0; fail=0; port=28200
for t in "$@"; do
  read -r n bot mode seed frames <<< "$t"
  port=$((port + 1))
  out="$PREFIX-$n-$bot-$mode-$seed"
  timeout $((frames / 20 + 200)) "$HERE/run_copies.sh" -s "$SO" -g "$GAME" -o "$out" -n "$n" -f "$frames" -b "$bot" -m "$mode" -e "$seed" -t $((frames / 25 + 150)) -p $port > "$out.txt" 2>&1
  r=$(grep -o "RESULT: [A-Z]*" "$out.txt" | tail -1)
  L="$out/copy0/coop.log"
  [ "$r" = "RESULT: PASS" ] && pass=$((pass + 1)) || fail=$((fail + 1))
  echo "$t: ${r:-RESULT: NONE} cmds=$(grep -c ' APPLY ' "$L" 2>/dev/null) jumps=$(grep -c 'jump to' "$L" 2>/dev/null) last: $(grep 'BOT status' "$L" 2>/dev/null | tail -1 | cut -c30-150)"
done
echo "BATCH pass=$pass fail=$fail"
[ $fail = 0 ]
