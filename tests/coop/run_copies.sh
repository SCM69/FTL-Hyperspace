#!/bin/bash
# Run N copies of FTL with the co-op build on this PC and compare their state hashes.
# Usage: run_copies.sh -s <Hyperspace .so> -g <game dir> -o <out dir> [-n copies] [-f frames] [-b bot]
#                      [-m ffa|owncrew] [-e seed] [-d x11|headless] [-t timeout s] [-p port] [-J late-join seconds]
# LD_PRELOAD goes to FTL only (gamescope must not load Hyperspace).
# Copies are hard-linked from <game dir>, each with its own save folder and logs under <out dir>/copyN.
# Windows build under Proton: -s Hyperspace.dll and -g a Windows 1.6.9 game folder (FTLGame.exe at its top level).
# Needs Proton (PROTON_DIR, default: Proton - Experimental) and the Steam Linux Runtime sniper (SNIPER_DIR); the Wine
# prefix is WINE_PFX (default <game dir>/../pfx).
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
WIN=0; [ -f "$GAME/FTLGame.exe" ] && WIN=1
[ -f "$SO" ] && { [ -d "$GAME/data" ] || [ $WIN = 1 ]; } && [ -n "$OUT" ] || { echo "need -s .so -g game dir -o out dir"; exit 2; }
if [ $WIN = 1 ]; then
  STEAM_DIR=${STEAM_DIR:-$HOME/.local/share/Steam}
  find_tool() { for l in "$STEAM_DIR" $(sed -n 's/.*"path"[[:space:]]*"\(.*\)".*/\1/p' "$STEAM_DIR/config/libraryfolders.vdf" 2>/dev/null); do
    [ -d "$l/steamapps/common/$1" ] && { echo "$l/steamapps/common/$1"; return; }; done; }
  PROTON_DIR=${PROTON_DIR:-$(find_tool "Proton - Experimental")}
  SNIPER_DIR=${SNIPER_DIR:-$(find_tool SteamLinuxRuntime_sniper)}
  WINE_PFX=${WINE_PFX:-$(readlink -m "$GAME/../pfx")}
  [ -x "$PROTON_DIR/proton" ] && [ -x "$SNIPER_DIR/_v2-entry-point" ] || { echo "Proton or SteamLinuxRuntime_sniper not found"; exit 2; }
  mkdir -p "$WINE_PFX"
fi
mkdir -p "$OUT"
BUILD_ID="test-$(sha256sum "$SO" | cut -c1-12)"
# COOP_NET=tcp (default) connects the copies directly; COOP_NET=fakesteam runs them over the Steam transport with
# tests/coop/fakesteam_hub.py standing in for Steam (lobby, friends' game info, P2P packets).
COOP_NET=${COOP_NET:-tcp}
HUB_PID=
if [ "$COOP_NET" = fakesteam ]; then
  HUB_PORT=$((PORT + 1))
  python3 -I "$(dirname "$(readlink -f "$0")")/fakesteam_hub.py" "$HUB_PORT" ${HUB_LAG_MS:+--lag-ms $HUB_LAG_MS} > "$OUT/hub.log" 2>&1 &
  HUB_PID=$!
  sleep 1
  export FTL_COOP_NET=steam FTL_COOP_FAKE_STEAM=127.0.0.1:$HUB_PORT
else
  export FTL_COOP_NET=tcp
fi

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
  if [ $WIN = 1 ]; then
    rm -f "$C/game/Hyperspace.dll" "$C/game/xinput1_4.dll"
    cp "$SO" "$C/game/Hyperspace.dll"; cp "$(dirname "$0")/../../Release Files/Windows/mod/xinput1_4.dll" "$C/game/"
    # Windows FTL reads settings.ini from its own folder (seen in a Wine file trace), not from the user folder.
    rm -f "$C/game/settings.ini"; cp "$C/user/settings.ini" "$C/game/settings.ini"
  fi
  if [ $SOLO = 1 ]; then ROLE=solo; elif [ $i = 0 ]; then ROLE=host; else ROLE=guest; fi
  (
    if [ $WIN = 1 ]; then cd "$C/game" || exit 3; else cd "$C/game/data" || exit 3; fi
    export SteamAppId=212680 SteamGameId=212680 LC_NUMERIC=C
    export XDG_DATA_HOME="$C/user" FTL_COOP=$ROLE FTL_COOP_PORT=$PORT FTL_COOP_ADDR=127.0.0.1:$PORT
    PLAYERS=$N; [ "$LATE" != 0 ] && PLAYERS=$((N - 1))
    export FTL_COOP_NAME="bot$i" FTL_COOP_PLAYERS=$PLAYERS FTL_COOP_MODE=$MODE FTL_COOP_SEED=$SEED FTL_COOP_BOT=$BOT
    export FTL_COOP_USERDIR="$C/user/" FTL_COOP_LOG="$C/coop.log" FTL_COOP_EXIT_AFTER=$FRAMES FTL_COOP_BUILD_ID=$BUILD_ID
    export HYPERSPACE_SKIP_BUG_REPORT=1 HYPERSPACE_FORCE_EXIT_ON_FREEZE=1 HYPERSPACE_NO_DIALOGS=1
    if [ $WIN = 1 ]; then
      # Wine sees the Linux file system as drive Z:.
      export FTL_COOP_USERDIR="Z:$C/user/" FTL_COOP_LOG="Z:$C/coop.log" WINEDLLOVERRIDES="${WINEDLLOVERRIDES-xinput1_4=n,b}"
      # One prefix per copy (each copy runs in its own runtime container with its own wineserver), cloned from WINE_PFX.
      [ -d "$C/pfx" ] || cp -a --reflink=auto "$WINE_PFX" "$C/pfx"
      export STEAM_COMPAT_DATA_PATH="$C/pfx" STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_DIR" PROTON_LOG=${PROTON_LOG:-0}
      exec timeout -k 5 "$TIMEOUT" gamescope --backend headless -W 1280 -H 720 -w 1280 -h 720 -- \
        "$SNIPER_DIR/_v2-entry-point" --verb=waitforexitandrun -- "$PROTON_DIR/proton" waitforexitandrun ./FTLGame.exe > "$C/stdout.log" 2>&1
    elif [ -n "${FTL_COOP_GDB_COPY:-}" ] && [ "$FTL_COOP_GDB_COPY" = "$i" ]; then
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
[ -n "$HUB_PID" ] && kill "$HUB_PID" 2>/dev/null
echo "copies exited (last nonzero rc=$rc)"
python3 -I "$(dirname "$(readlink -f "$0")")/compare_logs.py" "$OUT" "$N" "$FRAMES"
