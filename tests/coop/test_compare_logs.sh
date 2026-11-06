#!/bin/bash
# Scorer self-test: compare_logs.py must PASS a good pair and FAIL every bad one.
P="$(dirname "$(readlink -f "$0")")/compare_logs.py"
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
fails=0
check() { # name expected log0 log1
  d=$S/$1; mkdir -p $d/copy0 $d/copy1
  printf '%s\n' "$3" > $d/copy0/coop.log; [ "$4" != "-" ] && printf '%s\n' "$4" > $d/copy1/coop.log
  got=$(python3 -I "$P" $d 2 100 | tail -1)
  if [ "$got" = "RESULT: $2" ]; then echo "ok   $1 -> $2"; else echo "BAD  $1: wanted $2, got $got"; fails=$((fails+1)); fi
}
END='[0.3 f100 e1] SUMMARY frames=100 desyncs=0 stalls=0 players=2 catchup=0 resyncs=0 reason=exit_after reached'
G="[0.1 f30 e1] HASH all=aa rng=1 time=1
[0.2 f60 e1] HASH all=bb rng=1 time=1
$END"
check good PASS "$G" "$G"
check diverge FAIL "$G" "[0.1 f30 e1] HASH all=aa rng=1 time=1
[0.2 f60 e1] HASH all=bc rng=2 time=1
$END"
check nosummary FAIL "$G" "[0.1 f30 e1] HASH all=aa rng=1 time=1"
check timeout_summary FAIL "$G" "[0.1 f30 e1] HASH all=aa rng=1 time=1
[0.3 f50 e1] SUMMARY frames=50 desyncs=0 stalls=0 players=2 catchup=0 resyncs=0 reason=timeout"
check desyncline FAIL "$G" "$G
[0.4 f60 e1] DESYNC frame=60 slot=1"
check nohashes FAIL "$END" "$END"
check missing FAIL "$G" "-"
# Same frame number in two epochs: only same-epoch frames are compared.
E2="[0.1 f30 e1] HASH all=aa rng=1 time=1
[0.5 f30 e2] HASH all=cc rng=1 time=1
$END"
check epochs PASS "$E2" "$E2"
check epoch_diverge FAIL "$E2" "[0.1 f30 e1] HASH all=aa rng=1 time=1
[0.5 f30 e2] HASH all=cd rng=1 time=1
$END"
echo "SCORER pass=$((9 - fails)) fail=$fails"
[ $fails = 0 ]
