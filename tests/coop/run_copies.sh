#!/bin/bash
# Run N copies of FTL with the co-op build on this PC and compare their state hashes.
# Usage: run_copies.sh -s <Hyperspace .so> -g <game dir> -o <out dir> [-n copies] [-f frames] [-b bot]
#                      [-m ffa|owncrew] [-e seed] [-d x11|headless] [-t timeout s] [-p port]
# Copies are hard-linked from <game dir>, each with its own save folder and logs under <out dir>/copyN.
set -u
SO=""; GAME=""; OUT=""; N=2; FRAMES=1800; BOT=probe; MODE=ffa; SEED=4242; DISP=headless; TIMEOUT=240; PORT=27890
SOLO=0
while getopts "s:g:o:n:f:b:m:e:d:t:p:1" opt; do
  case $opt in
    s) SO=$(readlink -f "$OPTARG");; g) GAME=$(readlink -f "$OPTARG");; o) OUT=$(readlink -m "$OPTARG");;
    n) N=$OPTARG;; f) FRAMES=$OPTARG;; b) BOT=$OPTARG;; m) MODE=$OPTARG;; e) SEED=$OPTARG;;
    d) DISP=$OPTARG;; t) TIMEOUT=$OPTARG;; p) PORT=$OPTARG;; 1) SOLO=1;;
    *) echo "bad option"; exit 2;;
  esac
done
[ -f "$SO" ] && [ -d "$GAME/data" ] && [ -n "$OUT" ] || { echo "need -s .so -g game dir -o out dir"; exit 2; }
mkdir -p "$OUT"
BUILD_ID="test-$(sha256sum "$SO" | cut -c1-12)"

pids=()
for ((i=0; i<N; i++)); do
  C="$OUT/copy$i"
  rm -rf "$C/user" "$C/coop.log"
  mkdir -p "$C/user"
  [ -d "$C/game" ] || cp -al "$GAME" "$C/game"
  if [ $SOLO = 1 ]; then ROLE=solo; elif [ $i = 0 ]; then ROLE=host; else ROLE=guest; fi
  (
    cd "$C/game/data" || exit 3
    export LD_PRELOAD="$SO" SteamAppId=212680 SteamGameId=212680 LC_NUMERIC=C
    export XDG_DATA_HOME="$C/user" FTL_COOP=$ROLE FTL_COOP_PORT=$PORT FTL_COOP_ADDR=127.0.0.1:$PORT
    export FTL_COOP_NAME="bot$i" FTL_COOP_PLAYERS=$N FTL_COOP_MODE=$MODE FTL_COOP_SEED=$SEED FTL_COOP_BOT=$BOT
    export FTL_COOP_USERDIR="$C/user/" FTL_COOP_LOG="$C/coop.log" FTL_COOP_EXIT_AFTER=$FRAMES FTL_COOP_BUILD_ID=$BUILD_ID
    export HYPERSPACE_SKIP_BUG_REPORT=1 HYPERSPACE_FORCE_EXIT_ON_FREEZE=1
    if [ "$DISP" = headless ]; then
      exec timeout -k 5 "$TIMEOUT" gamescope --backend headless -W 1280 -H 720 -w 1280 -h 720 -- ./FTL.amd64 > "$C/stdout.log" 2>&1
    else
      exec timeout -k 5 "$TIMEOUT" ./FTL.amd64 > "$C/stdout.log" 2>&1
    fi
  ) &
  pids+=($!)
  [ $i = 0 ] && sleep 4   # host listens first
done
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=$?; done
echo "copies exited (last nonzero rc=$rc)"
python3 -I "$(dirname "$(readlink -f "$0")")/compare_logs.py" "$OUT" "$N" "$FRAMES"
