#!/bin/bash
# Regenerates the FastRPC glue of the pxquake module from pxquake.idl with the Hexagon SDK's qaic:
#   pxquake.h (shared header), pxquake_stub.c (host side), pxquake_skel.c (DSP side). The generated files are in the
# tree, so this is only needed after a change to pxquake.idl. Touches no DSP.
#   HEXAGON_SDK_ROOT (default /c/Qualcomm/Hexagon_SDK/5.5.7.0)        bash dsp/gen_glue.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
SDK="${HEXAGON_SDK_ROOT:-/c/Qualcomm/Hexagon_SDK/5.5.7.0}"
SDK="$(cygpath -u "$SDK" 2>/dev/null || echo "$SDK")"
QAIC="$SDK/ipc/fastrpc/qaic/WinNT/qaic.exe"
[ -x "$QAIC" ] || { echo "!! qaic missing: $QAIC (set HEXAGON_SDK_ROOT)"; exit 2; }
"$QAIC" -mdll -o . -I "$SDK/incs" -I "$SDK/incs/stddef" pxquake.idl
for f in pxquake.h pxquake_stub.c pxquake_skel.c; do
  [ -s "$f" ] || { echo "!! qaic did not write $f"; exit 3; }
done
grep -q "pxquake_skel_handle_invoke" pxquake_skel.c || { echo "!! no handle_invoke in the skel glue"; exit 4; }
echo "== glue generated =="
