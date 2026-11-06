#!/bin/bash
# Builds the Windows co-op zip from a finished Windows build: package.sh <Hyperspace.dll> <out.zip>
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd)
DLL=$(realpath "$1"); OUT=$(realpath -m "$2")
TMP=$(mktemp -d); trap 'rm -r "$TMP"' EXIT
cp "$DLL" "$ROOT/Release Files/Windows/mod/xinput1_4.dll" "$HERE/coop-install.bat" "$HERE/README-COOP.txt" "$TMP/"
cp "$ROOT/LICENSE" "$ROOT/COPYRIGHT.md" "$TMP/"
rm -f "$OUT"
python3 -I -c 'import sys, zipfile, os
d, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for n in sorted(os.listdir(d)): z.write(os.path.join(d, n), n); print(n)' "$TMP" "$OUT"
