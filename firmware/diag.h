/*
 * diag.h
 * Crash reports and diagnostics.
 *
 * At boot, diag_boot() looks at why the chip reset and whether the panic
 * handler left a core dump in the "coredump" flash partition. If it did,
 * the useful part (task, PC, exception, backtrace) becomes a short text
 * report, kept in NVS (and /crash/crash_<n>.txt on the SD card), and the core
 * dump is erased so the next crash can be captured.
 *
 * The report is shown in Settings > Diagnostics and sent to the companion
 * app on request (phone link "crash"), so crashes can be read without a
 * serial cable. Decode the backtrace with:
 *   xtensa-esp32s3-elf-addr2line -pfiaC -e AmoledSmartWatchOS.ino.elf <addresses>
 */
#pragma once
#include <stdint.h>

// Bump on every release; the companion app shows it and OTA compares it.
#define FW_VERSION "3.3.0"

void diag_boot();            // early in setup(), after nvs_init()
void diag_after_storage();   // after sd_init() and ui_init(): SD copy + toast
void diag_loop();            // marks an OTA image good after 20 s of running
void diag_mark_good();       // ...or now (before a deliberate power-off/restart)

const char *diag_fw_version();
const char *diag_build_date();
const char *diag_reset_reason();    // human readable reason for the last reset
bool        diag_last_reset_was_crash();
uint32_t    diag_boot_count();
uint32_t    diag_crash_count();

// Last saved crash report ("" if none). Survives reboots until cleared.
const char *diag_crash_text();
uint32_t    diag_crash_id();        // changes with every new crash report
void        diag_clear_crash();

// Writes a multi-line status summary (version, uptime, memory, radios,
// battery) into out.
void diag_status_text(char *out, int cap);
