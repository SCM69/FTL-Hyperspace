#!/bin/bash
# Builds the Linux co-op zip from a finished Linux build: package.sh <Hyperspace.1.6.13.amd64.so> <out.zip>
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
SO=$(realpath "$1"); OUT=$(realpath -m "$2")
TMP=$(mktemp -d); trap 'rm -r "$TMP"' EXIT
cp "$SO" "$TMP/Hyperspace.1.6.13.amd64.so"
cp "$HERE/coop-install.sh" "$HERE/README-COOP-Linux.txt" "$ROOT/LICENSE" "$ROOT/COPYRIGHT.md" "$TMP/"
rm -f "$OUT"
python3 -I -c 'import sys, zipfile, os
d, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for n in sorted(os.listdir(d)):
        i = zipfile.ZipInfo(n); i.compress_type = zipfile.ZIP_DEFLATED
        i.external_attr = (0o755 if n.endswith(".sh") else 0o644) << 16
        z.writestr(i, open(os.path.join(d, n), "rb").read()); print(n)' "$TMP" "$OUT"
