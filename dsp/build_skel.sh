#!/bin/bash
# Builds the DSP side of PulseX Quake: libpxquake_skel.so (Hexagon v73, HVX 128 B) in THIS folder, and copies it next
# to bin\pulsex-quake.exe, where the host points ADSP_LIBRARY_PATH. Touches no DSP. Run from Git Bash.
#
# Needs (environment variables, with defaults):
#   HEXAGON_SDK_ROOT    Hexagon SDK 5.x                       (default: /c/Qualcomm/Hexagon_SDK/5.5.7.0)
#   HEXAGON_TOOLS_ROOT  Hexagon tools 8.7.x (hexagon-clang)   (default: $HEXAGON_SDK_ROOT/tools/HEXAGON_Tools/8.7.06/Tools)
#
# Windows loads a DSP module only with a signed catalog (libpxquake_skel.cat) beside it - see README.md, "The NPU
# module and signing". SIGN=1 runs sign_catalog.ps1 afterwards (a catalog signed with YOUR own certificate).
#   bash dsp/build_skel.sh            or   SIGN=1 bash dsp/build_skel.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
SDK="${HEXAGON_SDK_ROOT:-/c/Qualcomm/Hexagon_SDK/5.5.7.0}"
SDK="$(cygpath -u "$SDK" 2>/dev/null || echo "$SDK")"
TOOLS="${HEXAGON_TOOLS_ROOT:-$SDK/tools/HEXAGON_Tools/8.7.06/Tools}"
TOOLS="$(cygpath -u "$TOOLS" 2>/dev/null || echo "$TOOLS")"
CLANG="$TOOLS/bin/hexagon-clang.exe"
READELF="$TOOLS/bin/hexagon-llvm-readelf.exe"
PIC="$TOOLS/target/hexagon/lib/v73/G0/pic"
BIN="$HERE/../bin"
[ -d "$SDK/incs" ] || { echo "!! Hexagon SDK missing: $SDK (set HEXAGON_SDK_ROOT)"; exit 2; }
[ -x "$CLANG" ]   || { echo "!! hexagon-clang missing: $CLANG (set HEXAGON_TOOLS_ROOT)"; exit 2; }
[ -x "$READELF" ] || { echo "!! readelf missing: $READELF"; exit 2; }
[ -d "$PIC" ]     || { echo "!! PIC libraries missing: $PIC"; exit 2; }
CF="-mv73 -G0 -fpic -mhvx -mhvx-length=128B -O3 -funroll-loops -fno-builtin -Wall -Wno-unused-function"
CF="$CF ${PQ_CF:-}"
INC="-I$SDK/incs -I$SDK/incs/stddef -I$SDK/rtos/qurt/computev73/include/qurt -I$SDK/rtos/qurt/computev73/include/posix -I$SDK/ipc/fastrpc/incs -I."
fail(){ echo "  BUILD FAILED: $1"; exit 1; }
sz(){ stat -c%s "$1" 2>/dev/null || echo 0; }
for f in pxquake.h pxquake_skel.c pxquake_stub.c; do
  [ -s "$f" ] || fail "$f missing - run gen_glue.sh"
done
rm -f pxquake_imp.o pxquake_skel.o libpxquake_skel.so
echo "== compile imp =="
"$CLANG" $CF $INC -c pxquake_imp.c -o pxquake_imp.o
echo "== compile skel glue =="
"$CLANG" $CF $INC -Wno-unused-variable -c pxquake_skel.c -o pxquake_skel.o
echo "== link =="
"$CLANG" -mv73 -mhvx -shared -o libpxquake_skel.so -Wl,--no-threads -Wl,-Bsymbolic -Wl,-L"$PIC" \
  -Wl,--start-group pxquake_imp.o pxquake_skel.o -Wl,--end-group
[ "$(sz libpxquake_skel.so)" -ge 4000 ] || fail "libpxquake_skel.so = $(sz libpxquake_skel.so) B"
"$READELF" --dyn-symbols libpxquake_skel.so > _dynsyms.txt
grep -q " pxquake_skel_handle_invoke$" _dynsyms.txt || fail "pxquake_skel_handle_invoke is not exported"
# every import must exist on the DSP: in the SDK's dynsymbols.lst or in this list (checked on the target)
KNOWN="HAP_power_get HAP_mem_get_stats qurt_thread_exit HAP_mmap_get HAP_mmap_put qurt_mem_cache_clean qurt_sleep dspqueue_import dspqueue_read dspqueue_read_noblock dspqueue_write dspqueue_close qurt_thread_get_priority qurt_thread_get_id compute_resource_attr_set_release_callback compute_resource_attr_init compute_resource_attr_set_vtcm_param compute_resource_attr_get_vtcm_ptr compute_resource_acquire compute_resource_release qurt_signal_clear qurt_signal_destroy qurt_signal_init qurt_signal_set qurt_signal_wait qurt_thread_create qurt_thread_join malloc HAP_power_set calloc free __cxa_finalize __register_frame_info_bases __deregister_frame_info_bases"
UND=$(grep " UND " _dynsyms.txt | awk '{print $8}' | grep -v '^$' | sort -u)
for s in $UND; do
  if grep -qx "$s;" "$SDK/incs/dynsymbols.lst" 2>/dev/null; then continue; fi
  case " $KNOWN " in *" $s "*) continue;; esac
  fail "import '$s' is neither in the SDK's dynsymbols.lst nor in the known-good list"
done
rm -f _dynsyms.txt pxquake_imp.o pxquake_skel.o
if [ "${SIGN:-0}" = "1" ]; then
  echo "== sign (your own certificate) =="
  rm -f libpxquake_skel.cat
  powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$HERE/sign_catalog.ps1")" || fail "sign_catalog.ps1"
  [ -s libpxquake_skel.cat ] || fail "no libpxquake_skel.cat"
fi
echo "== copy next to the exe =="
mkdir -p "$BIN"
cp -f libpxquake_skel.so "$BIN/"
[ -s libpxquake_skel.cat ] && cp -f libpxquake_skel.cat "$BIN/"
echo "== done ==  skel=$(sz libpxquake_skel.so) B  -> bin/"
