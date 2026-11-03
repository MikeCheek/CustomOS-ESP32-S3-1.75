#!/usr/bin/env bash
# Builds the in-app emulator's library (emu_main.cpp) for the PC with clang
# and drives it like the app does (emu_host_test.cpp): frames as PNGs.
#   tools/watch_sim/emu_host_test.sh [out dir]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LIBS="${ARDUINO_LIBS:-$HOME/Arduino/libraries}"
OUT="${1:-$HERE/build/emu}"
B="$HERE/build/emu_obj"
rm -rf "$B"; mkdir -p "$B" "$OUT"
CXX="${CXX:-clang++}"
FLAGS=(-std=gnu++17 -O1 -fPIC -w -DARDUINOJSON_ENABLE_ARDUINO_STRING=0 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0
       -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -I"$HERE/shim" -I"$HERE" -I"$ROOT/firmware" -I"$LIBS/Arduino_GFX/src" -I"$LIBS/ArduinoJson/src")
# shellcheck disable=SC1091
source "$HERE/emu_sources.sh"
pids=()
for src in "$ROOT"/firmware/*.cpp; do
    f="$(basename "$src" .cpp)"
    [[ " ${EMU_SKIP[*]} " == *" $f "* ]] && continue
    "$CXX" "${FLAGS[@]}" -c "$src" -o "$B/fw_$f.o" & pids+=($!)
done
for f in "${EMU_GFX[@]}"; do "$CXX" "${FLAGS[@]}" -c "$LIBS/Arduino_GFX/src/$f.cpp" -o "$B/gfx_$(basename "$f").o" & pids+=($!); done
for f in sim_stubs emu_main; do "$CXX" "${FLAGS[@]}" -c "$HERE/$f.cpp" -o "$B/$f.o" & pids+=($!); done
for p in "${pids[@]}"; do wait "$p"; done
"$CXX" -shared -Wl,--no-undefined "$B"/*.o -o "$B/libwatchemu.so"
"$CXX" -std=c++17 "$HERE/emu_host_test.cpp" -L"$B" -lwatchemu -Wl,-rpath,"$B" -o "$B/emu_test"
"$B/emu_test" "$OUT"
python3 - "$OUT" <<'PY'
import sys, glob
from PIL import Image
for p in glob.glob(sys.argv[1] + "/*.ppm"):
    Image.open(p).save(p[:-4] + ".png")
PY
echo "Frames in $OUT"
