/*
 * hal_fwupdate.h
 * Firmware updates over Wi-Fi from the project's GitHub releases.
 *
 * About once a day (FW_UPDATE_CHECK_HOURS), when the watch is on Wi-Fi -
 * or Wi-Fi is switched on in Settings and a network is saved - it asks
 * the GitHub API for the latest release of FW_UPDATE_REPO. If that is
 * newer than FW_VERSION, the user is asked whether to install it; on
 * "Update" the .bin is downloaded straight into the inactive OTA slot and
 * the watch restarts into it. Settings > Software update checks on demand.
 *
 * Network work runs on its own task so the UI stays alive. The same rules
 * as Bluetooth updates apply (hal_ota.h): with a key in ota_pubkey.h only
 * a validly signed image (the release's *.signed.bin) is installed, and a
 * new image that doesn't survive its first minute is rolled back.
 */
#pragma once
#include <stdint.h>

enum FwupState : uint8_t {
    FWUP_IDLE,
    FWUP_CONNECTING,    // waiting for Wi-Fi
    FWUP_CHECKING,      // asking GitHub for the latest release
    FWUP_UP_TO_DATE,
    FWUP_AVAILABLE,     // fwup_latest_version() is newer - fwup_install() to get it
    FWUP_DOWNLOADING,   // writing to flash
    FWUP_DONE,          // verified - restarting
    FWUP_FAILED,        // fwup_error() says why
};

// Main loop, awake or asleep: automatic checks, the "update available"
// prompt, restarting once installed.
void fwup_update();

void fwup_check();      // check now (Settings > Software update)
void fwup_install();    // download + install the version found by the check
void fwup_cancel();     // stop a download (the current firmware stays)
void fwup_dismiss();    // back to idle after a result was shown

FwupState   fwup_state();
bool        fwup_busy();          // connecting / checking / downloading: keep the chip awake
bool        fwup_installing();    // a download is under way (Bluetooth updates wait)
bool        fwup_failed_installing();   // the FAILED state came from a download, not a check
const char *fwup_latest_version();
const char *fwup_error();
uint32_t    fwup_total();         // download size in bytes
uint32_t    fwup_written();
