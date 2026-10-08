#!/usr/bin/env bash
# Renders README screenshots of the watch UI on a PC (Linux, g++, Python 3
# + Pillow). The real screen code in firmware/ is compiled for the host;
# the hardware layers are replaced by sim_stubs.cpp.
#
#   tools/watch_sim/run.sh [output dir, default docs/screenshots]
#
# Needs the Arduino_GFX and ArduinoJson libraries (scripts/arduino-libraries.txt)
# in ~/Arduino/libraries, or set ARDUINO_LIBS.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LIBS="${ARDUINO_LIBS:-$HOME/Arduino/libraries}"
GFX="$LIBS/Arduino_GFX/src"
JSON="$LIBS/ArduinoJson/src"
OUT="${1:-$ROOT/docs/screenshots}"
B="$HERE/build"
mkdir -p "$B/gfx" "$B/fw" "$B/raw"

CXX="${CXX:-g++}"
FLAGS=(-std=gnu++17 -O1 -g -w
       -DARDUINOJSON_ENABLE_ARDUINO_STRING=0 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0
       -I"$HERE/shim" -I"$HERE" -I"$ROOT/firmware" -I"$GFX" -I"$JSON")

# Hardware drivers and anything needing a radio/SD/codec: replaced by the stubs.
SKIP=(diag hal_audio hal_bambu hal_ble hal_display hal_espnow hal_expander hal_fwupdate hal_gps hal_imu hal_ntp
      hal_ota hal_power hal_rtc hal_sd hal_touch hal_usb hal_wifi hal_wifi_xfer media_decode wifi_cfg
      app_bambu_printer app_espnow_status app_video app_wifi_setup)

for f in Arduino_GFX Arduino_G Arduino_DataBus canvas/Arduino_Canvas; do
    "$CXX" "${FLAGS[@]}" -c "$GFX/$f.cpp" -o "$B/gfx/$(basename "$f").o"
done
for f in "$ROOT"/firmware/*.cpp; do
    n="$(basename "$f" .cpp)"
    [[ " ${SKIP[*]} " == *" $n "* ]] && continue
    "$CXX" "${FLAGS[@]}" -c "$f" -o "$B/fw/$n.o" &
done
wait
for f in sim_main sim_stubs sim_scenarios; do
    "$CXX" "${FLAGS[@]}" -c "$HERE/$f.cpp" -o "$B/$f.o"
done
"$CXX" "$B"/*.o "$B"/fw/*.o "$B"/gfx/*.o -o "$B/watch_sim"

rm -f "$B"/raw/*.rgb565
SIM_OUT="$B/raw" "$B/watch_sim"
python3 "$HERE/to_png.py" "$B/raw" "$OUT"
echo "Screenshots in $OUT"
