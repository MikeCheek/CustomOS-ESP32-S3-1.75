#!/bin/bash
# Gets the watch emulator's sources ready for WatchEmu.podspec (the
# Podfile runs this on every `pod install`): links to the firmware and
# the PC simulator (CocoaPods only takes files under the pod's folder),
# and fetches the Arduino libraries the screen code uses (and JPEGDEC), at the
# versions the firmware is built with - the same as the Android build
# (android/app/src/main/cpp/CMakeLists.txt).
set -e
cd "$(dirname "$0")"
mkdir -p src third_party
ln -sfn ../../../../firmware src/firmware
ln -sfn ../../../../tools/watch_sim src/sim
get() {   # name url tag
  if [ ! -f "third_party/$1/.tag" ] || [ "$(cat "third_party/$1/.tag")" != "$3" ]; then
    rm -rf "third_party/$1"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "third_party/$1"
    echo "$3" > "third_party/$1/.tag"
  fi
}
get Arduino_GFX https://github.com/moononournation/Arduino_GFX.git v1.6.8
get ArduinoJson https://github.com/bblanchon/ArduinoJson.git v7.4.3
get JPEGDEC https://github.com/bitbank2/JPEGDEC.git 1.8.4
