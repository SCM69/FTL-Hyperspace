#!/bin/bash
# Run N copies of FTL with the co-op build on this PC and compare their state hashes.
# Usage: run_copies.sh -s <Hyperspace .so> -g <game dir> -o <out dir> [-n copies] [-f frames] [-b bot]
#                      [-m ffa|owncrew] [-e seed] [-d x11|headless] [-t timeout s] [-p port] [-J late-join seconds]
# LD_PRELOAD goes to FTL only (gamescope must not load Hyperspace).
# Copies are hard-linked from <game dir>, each with its own save folder and logs under <out dir>/copyN.
set -u
SO=""; GAME=""; OUT=""; N=2; FRAMES=1800; BOT=probe; MODE=ffa; SEED=4242; DISP=headless; TIMEOUT=240; PORT=27890
SOLO=0; LATE=0
while getopts "s:g:o:n:f:b:m:e:d:t:p:1J:" opt; do
  case $opt in
    s) SO=$(readlink -f "$OPTARG");; g) GAME=$(readlink -f "$OPTARG");; o) OUT=$(readlink -m "$OPTARG");;
    n) N=$OPTARG;; f) FRAMES=$OPTARG;; b) BOT=$OPTARG;; m) MODE=$OPTARG;; e) SEED=$OPTARG;;
    d) DISP=$OPTARG;; t) TIMEOUT=$OPTARG;; p) PORT=$OPTARG;; 1) SOLO=1;; J) LATE=$OPTARG;;
    *) echo "bad option"; exit 2;;
  esac
done
[ -f "$SO" ] && [ -d "$GAME/data" ] && [ -n "$OUT" ] || { echo "need -s .so -g game dir -o out dir"; exit 2; }
mkdir -p "$OUT"
BUILD_ID="test-$(sha256sum "$SO" | cut -c1-12)"

pids=()
for ((i=0; i<N; i++)); do
  C="$OUT/copy$i"
  # FTL_COOP_KEEP_USER=1 keeps the save folder from the previous run (continue tests).
  if [ "${FTL_COOP_KEEP_USER:-0}" = 1 ]; then rm -f "$C/coop.log"; else rm -rf "$C/user" "$C/coop.log"; fi
  mkdir -p "$C/user/FasterThanLight"
  # Skip the first-launch language chooser; sound off; 60 FPS frame limit; Advanced Edition on; normal difficulty.
  for f in "$C/user/settings.ini" "$C/user/FasterThanLight/settings.ini"; do
    printf '%s\n' last_fullscreen=0 screen_x=1280 screen_y=720 music=0 sound=0 altpause=0 dialog_keys=1 \
      showpaths=1 lowend=0 logging=0 ach_pop=0 vsync=0 framelimit=1 colorblind=0 dlcEnabled=1 difficulty=1 \
      language=en windowed=1 stretched=0 > "$f"
  done
  [ -d "$C/game" ] || cp -al "$GAME" "$C/game"
  if [ $SOLO = 1 ]; then ROLE=solo; elif [ $i = 0 ]; then ROLE=host; else ROLE=guest; fi
  (
    cd "$C/game/data" || exit 3
    export SteamAppId=212680 SteamGameId=212680 LC_NUMERIC=C
    export XDG_DATA_HOME="$C/user" FTL_COOP=$ROLE FTL_COOP_PORT=$PORT FTL_COOP_ADDR=127.0.0.1:$PORT
    PLAYERS=$N; [ "$LATE" != 0 ] && PLAYERS=$((N - 1))
    export FTL_COOP_NAME="bot$i" FTL_COOP_PLAYERS=$PLAYERS FTL_COOP_MODE=$MODE FTL_COOP_SEED=$SEED FTL_COOP_BOT=$BOT
    export FTL_COOP_USERDIR="$C/user/" FTL_COOP_LOG="$C/coop.log" FTL_COOP_EXIT_AFTER=$FRAMES FTL_COOP_BUILD_ID=$BUILD_ID
    export HYPERSPACE_SKIP_BUG_REPORT=1 HYPERSPACE_FORCE_EXIT_ON_FREEZE=1 HYPERSPACE_NO_DIALOGS=1
    if [ -n "${FTL_COOP_GDB_COPY:-}" ] && [ "$FTL_COOP_GDB_COPY" = "$i" ]; then
      # Debug: this copy runs under gdb; the timeout's SIGINT stops it and gdb prints every thread's stack.
      exec timeout -k 10 -s INT "$TIMEOUT" gamescope --backend headless -W 1280 -H 720 -w 1280 -h 720 -- gdb -batch \
        -ex "set startup-with-shell off" -ex "set env LD_PRELOAD=$SO" -ex "handle SIGPIPE nostop noprint" -ex run \
        -ex "thread apply all bt 25" --args ./FTL.amd64 > "$C/stdout.log" 2>&1
    elif [ "$DISP" = headless ]; then
      exec timeout -k 5 "$TIMEOUT" gamescope --backend headless -W 1280 -H 720 -w 1280 -h 720 -- env LD_PRELOAD="$SO" ./FTL.amd64 > "$C/stdout.log" 2>&1
    else
      exec timeout -k 5 "$TIMEOUT" env LD_PRELOAD="$SO" ./FTL.amd64 > "$C/stdout.log" 2>&1
    fi
  ) &
  pids+=($!)
  [ $i = 0 ] && sleep 4   # host listens first
  # -J S: the last copy joins S seconds after the others (late join).
  [ "$LATE" != 0 ] && [ $i = $((N - 2)) ] && sleep "$LATE"
done
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=$?; done
echo "copies exited (last nonzero rc=$rc)"
python3 -I "$(dirname "$(readlink -f "$0")")/compare_logs.py" "$OUT" "$N" "$FRAMES"
