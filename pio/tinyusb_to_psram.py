# pio/tinyusb_to_psram.py - PlatformIO extra script (platformio.ini).
#
# Moves TinyUSB's ~30 KB of static buffers from internal RAM to PSRAM.
#
# The stock prebuilt libarduino_tinyusb.a is built with every USB class
# (audio, video, MIDI, DFU, network, host drivers...), and their buffers
# all land in internal RAM even though the watch only ever runs CDC, MSC
# and HID - 30 KB that Wi-Fi then can't have next to Bluetooth.
#
# On the ESP32-S3 TinyUSB runs the USB controller in slave mode (no DMA:
# the CPU copies through the FIFOs) and its interrupt isn't IRAM-resident,
# so its buffers can live in PSRAM. This copies the archive into the build
# folder with every .bss section renamed to .ext_ram.bss, which the linker
# script puts in PSRAM when CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is
# set (custom_sdkconfig), and puts the copy first on the library path.
#
# Only when that linker script really has the PSRAM .bss section: in the
# platform's own IDF build of the core (before the sections exist) or if
# anything goes wrong, the stock archive is linked as before.

import os
import re
import subprocess
from pathlib import Path

Import("env")  # noqa: F821 - provided by PlatformIO


def _log(msg):
    print("[tinyusb->psram] " + msg)


def main():
    platform = env.PioPlatform()  # noqa: F821
    libs_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
    if not libs_dir:
        return
    mcu = env.BoardConfig().get("build.mcu", "esp32s3")  # noqa: F821
    src = Path(libs_dir) / mcu / "lib" / "libarduino_tinyusb.a"
    ld = Path(libs_dir) / mcu / "ld" / "sections.ld"
    if not src.is_file() or not ld.is_file():
        return
    if ".ext_ram.bss" not in ld.read_text(errors="ignore"):
        _log("linker script has no PSRAM .bss (core not rebuilt yet) - stock archive")
        return

    out_dir = Path(env.subst("$BUILD_DIR")) / "psram_libs"  # noqa: F821
    dst = out_dir / "libarduino_tinyusb.a"
    tool_env = dict(os.environ)
    tool_env.update(env["ENV"])  # noqa: F821 - the toolchain's PATH
    objcopy = env.subst("$OBJCOPY")  # noqa: F821
    objdump = re.sub(r"objcopy$", "objdump", objcopy)

    if not dst.is_file() or dst.stat().st_mtime < src.stat().st_mtime:
        try:
            out_dir.mkdir(parents=True, exist_ok=True)
            listing = subprocess.run([objdump, "-h", str(src)], env=tool_env, check=True,
                                     capture_output=True, text=True).stdout
            names = sorted(set(re.findall(r"^\s*\d+\s+(\.bss(?:\.\S+)?)\s", listing, re.M)))
            if not names:
                _log("no .bss sections found - stock archive")
                return
            args = [objcopy]
            for n in names:
                args += ["--rename-section", "%s=.ext_ram%s" % (n, n)]
            tmp = dst.with_suffix(".tmp")
            subprocess.run(args + [str(src), str(tmp)], env=tool_env, check=True,
                           capture_output=True, text=True)
            os.replace(tmp, dst)
            _log("%d .bss sections moved to PSRAM" % len(names))
        except Exception as e:  # never break the build over this
            _log("failed (%s) - stock archive" % e)
            return

    env.Prepend(LIBPATH=[str(out_dir)])  # noqa: F821 - found before the stock copy


main()
