#!/bin/bash
# FTL Co-op installer for Linux (Steam, FTL 1.6.13 native). Run it yourself; it changes only files in the FTL folder.
#   ./coop-install.sh [FTL folder]          install (or update)
#   ./coop-install.sh --uninstall [folder]  remove it again
# It copies Hyperspace.1.6.13.amd64.so (from next to this script) into <FTL folder>/data, keeps a backup of an
# existing one, writes data/coop.cfg if there is none, and changes the game's start script data/FTL so that only the
# game itself loads the library (the way Hyperspace's own install notes describe; backup data/FTL.before-coop).
# A Steam update or "verify game files" restores data/FTL: run this again afterwards.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SO_NAME=Hyperspace.1.6.13.amd64.so
WANT_SHA=23897b7e7b538da7b5858e86aed87bd2a1e567d33d12d956761099628f557d80   # FTL.amd64, Steam 1.6.13

UNINSTALL=0
if [ "${1:-}" = "--uninstall" ]; then UNINSTALL=1; shift; fi

find_ftl() {
  local libs=("$HOME/.local/share/Steam" "$HOME/.steam/steam")
  for vdf in "$HOME/.local/share/Steam/config/libraryfolders.vdf" "$HOME/.steam/steam/steamapps/libraryfolders.vdf"; do
    [ -f "$vdf" ] && while read -r p; do libs+=("$p"); done < <(sed -n 's/.*"path"[[:space:]]*"\(.*\)".*/\1/p' "$vdf")
  done
  for l in "${libs[@]}"; do
    [ -x "$l/steamapps/common/FTL Faster Than Light/data/FTL.amd64" ] && { echo "$l/steamapps/common/FTL Faster Than Light"; return; }
  done
}

FTL=${1:-$(find_ftl)}
[ -n "$FTL" ] && [ -f "$FTL/data/FTL.amd64" ] || { echo "FTL not found. Run: $0 \"/path/to/FTL Faster Than Light\""; exit 1; }
DATA="$FTL/data"

if [ $UNINSTALL = 1 ]; then
  rm -f "$DATA/$SO_NAME"
  [ -f "$DATA/$SO_NAME.before-coop" ] && mv "$DATA/$SO_NAME.before-coop" "$DATA/$SO_NAME"
  [ -f "$DATA/FTL.before-coop" ] && mv "$DATA/FTL.before-coop" "$DATA/FTL"
  [ -f "$DATA/coop.cfg" ] && mv "$DATA/coop.cfg" "$DATA/coop.cfg.removed"
  echo "Removed (data/FTL restored, coop.cfg kept as coop.cfg.removed)."
  exit 0
fi

HAVE_SHA=$(sha256sum "$DATA/FTL.amd64" | cut -d' ' -f1)
if [ "$HAVE_SHA" != "$WANT_SHA" ]; then
  echo "This FTL.amd64 is not the tested Steam 1.6.13 build (SHA-256 $HAVE_SHA). Not installing."
  exit 1
fi
[ -f "$HERE/$SO_NAME" ] || { echo "$SO_NAME is missing next to this script."; exit 1; }

if [ -f "$DATA/$SO_NAME" ] && [ ! -f "$DATA/$SO_NAME.before-coop" ] && ! cmp -s "$HERE/$SO_NAME" "$DATA/$SO_NAME"; then
  cp -p "$DATA/$SO_NAME" "$DATA/$SO_NAME.before-coop"
fi
cp "$HERE/$SO_NAME" "$DATA/$SO_NAME"

if [ ! -f "$DATA/coop.cfg" ]; then
  cat > "$DATA/coop.cfg" <<'CFG'
# FTL Co-op settings. Lines starting with # are ignored.
# role: host (you run the game others join) or guest (you join a host). Remove the line to play alone.
role=host
# guest: the host's address and port, for example 203.0.113.7:27890 (the host must forward TCP port 27890,
# or use a VPN such as a LAN tunnel)
address=127.0.0.1:27890
port=27890
name=Captain
# host: start automatically when this many players (you included) are connected; 0 = press F9
players=2
# host: ffa (anyone controls anything) or owncrew (everyone commands their own crew); F11 switches in a run
mode=ffa
# host: new (new run) or continue (your saved run)
start=new
CFG
fi

# Start script: preload only for the final exec of the game binary.
if ! grep -q "$SO_NAME" "$DATA/FTL"; then
  if ! grep -q '^exec "\$here/\$command.\$arch" "\$@"$' "$DATA/FTL"; then
    echo "data/FTL looks different from the Steam 1.6.13 script; not changing it."; exit 1
  fi
  cp -p "$DATA/FTL" "$DATA/FTL.before-coop"
  sed -i 's|^exec "\$here/\$command.\$arch" "\$@"$|LD_PRELOAD="$here/'"$SO_NAME"'" exec "$here/$command.$arch" "$@"|' "$DATA/FTL"
fi

echo "Installed into: $DATA"
echo "Edit $DATA/coop.cfg (role, address, players), then start FTL from Steam as usual."
echo "Remove with: $0 --uninstall \"$FTL\""
