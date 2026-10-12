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
# so its buffers can live in PSRAM. This rewrites the archive with every
# .bss section renamed to .ext_ram.bss, which the linker script puts in
# PSRAM when CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set
# (custom_sdkconfig). It's done in place in the libs package (the stock
# file kept as libarduino_tinyusb.a.stock), so whichever library path
# the link searches finds it.
#
# Only when that linker script really has the PSRAM .bss section: in the
# platform's own IDF build of the core (before the sections exist), or if
# anything goes wrong, the stock archive is put back. What happened is
# written to .pio/tinyusb_to_psram.log (CI prints it).

import os
import re
import shutil
import subprocess
from pathlib import Path

Import("env")  # noqa: F821 - provided by PlatformIO


_LOG = []


def _log(msg):
    print("[tinyusb->psram] " + msg)
    _LOG.append(msg)


def _write_log():
    try:
        d = Path(env.subst("$PROJECT_DIR")) / ".pio"  # noqa: F821
        d.mkdir(exist_ok=True)
        with open(d / "tinyusb_to_psram.log", "a") as f:
            for m in _LOG:
                f.write(m + "\n")
    except OSError:
        pass


def _bss_sections(objdump, path, tool_env):
    listing = subprocess.run([objdump, "-h", str(path)], env=tool_env, check=True,
                             capture_output=True, text=True).stdout
    return (sorted(set(re.findall(r"^\s*\d+\s+(\.bss(?:\.\S+)?)\s", listing, re.M))),
            ".ext_ram.bss" in listing)


def main():
    platform = env.PioPlatform()  # noqa: F821
    libs_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
    if not libs_dir:
        return
    mcu = env.BoardConfig().get("build.mcu", "esp32s3")  # noqa: F821
    lib = Path(libs_dir) / mcu / "lib" / "libarduino_tinyusb.a"
    stock = lib.with_name(lib.name + ".stock")
    ld = Path(libs_dir) / mcu / "ld" / "sections.ld"
    if not lib.is_file():
        _log("no %s" % lib)
        return
    if not stock.is_file():
        shutil.copy2(lib, stock)
    has_ext = ld.is_file() and ".ext_ram.bss" in ld.read_text(errors="ignore")
    if not has_ext:
        # not (yet) a linker script with PSRAM .bss: the stock archive
        shutil.copy2(stock, lib)
        _log("%s has no .ext_ram.bss section - stock archive" % ld)
        return

    tool_env = dict(os.environ)
    tool_env.update(env["ENV"])  # noqa: F821 - the toolchain's PATH
    # The toolchain's binutils, named after its gcc ($OBJCOPY is esptool
    # in this platform, not objcopy)
    cc = env.subst("$CC")  # noqa: F821
    if not cc.endswith("gcc"):
        _log("unexpected compiler name %r - left as is" % cc)
        return
    objcopy, objdump = cc[:-3] + "objcopy", cc[:-3] + "objdump"
    try:
        names, _ = _bss_sections(objdump, stock, tool_env)
        if not names:
            _log("no .bss sections in the archive - left as is")
            return
        _, done = _bss_sections(objdump, lib, tool_env)
        if done:
            _log("archive already in PSRAM (%d .bss sections)" % len(names))
            return
        args = [objcopy]
        for n in names:
            args += ["--rename-section", "%s=.ext_ram%s" % (n, n)]
        tmp = lib.with_name(lib.name + ".tmp")
        subprocess.run(args + [str(stock), str(tmp)], env=tool_env, check=True,
                       capture_output=True, text=True)
        os.replace(tmp, lib)
        _log("%d .bss sections moved to PSRAM (%s)" % (len(names), lib))
    except Exception as e:  # never break the build over this
        shutil.copy2(stock, lib)
        _log("failed (%s) - stock archive" % e)


try:
    main()
finally:
    _write_log()
