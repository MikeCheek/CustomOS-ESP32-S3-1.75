#include "diag.h"
#include "config.h"
#include "hal_sd.h"
#include "hal_ble.h"
#include "hal_wifi.h"
#include "hal_power.h"
#include "ui.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_system.h>
#include <esp_core_dump.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <string.h>

// An OTA-installed image starts in "pending verify": if it crashes before
// diag_loop() marks it valid, the bootloader goes back to the previous
// firmware on the next reset. (Arduino's default marks it valid at once.)
extern "C" bool verifyRollbackLater() { return true; }

#define CRASH_MAX 900

static char     s_crash[CRASH_MAX] = "";
static uint32_t s_crash_id = 0;
static uint32_t s_boots = 0;
static uint32_t s_crashes = 0;
static const char *s_reason = "unknown";
static bool     s_was_crash = false;
static bool     s_new_crash = false;   // captured on this boot - copy to SD + toast
static bool     s_marked_valid = false;

static const char *reason_name(esp_reset_reason_t r, bool *crash) {
    *crash = false;
    switch (r) {
        case ESP_RST_POWERON:   return "power on";
        case ESP_RST_EXT:       return "external reset";
        case ESP_RST_SW:        return "restart";
        case ESP_RST_PANIC:     *crash = true; return "crash (panic)";
        case ESP_RST_INT_WDT:   *crash = true; return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  *crash = true; return "task watchdog";
        case ESP_RST_WDT:       *crash = true; return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        case ESP_RST_BROWNOUT:  *crash = true; return "brownout (low voltage)";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB reset";
        case ESP_RST_JTAG:      return "JTAG";
        case ESP_RST_EFUSE:     *crash = true; return "efuse error";
        case ESP_RST_PWR_GLITCH: *crash = true; return "power glitch";
        case ESP_RST_CPU_LOCKUP: *crash = true; return "CPU lockup";
        default:                return "unknown";
    }
}

static const char *exc_name(uint32_t cause) {
    switch (cause) {
        case 0:  return "IllegalInstruction";
        case 2:  return "InstructionFetchError";
        case 3:  return "LoadStoreError";
        case 6:  return "IntegerDivideByZero";
        case 9:  return "LoadStoreAlignment";
        case 20: return "InstFetchProhibited";
        case 28: return "LoadProhibited";
        case 29: return "StoreProhibited";
        default: return "Exception";
    }
}

// Builds s_crash from the core dump, if there is one. Returns true when a
// dump was found (and erased).
static bool read_core_dump(const char *reason) {
    if (esp_core_dump_image_check() != ESP_OK) return false;
    int n = 0;
    n += snprintf(s_crash + n, CRASH_MAX - n, "FW %s (%s)\nReset: %s\n", FW_VERSION, diag_build_date(), reason);
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t *sum = (esp_core_dump_summary_t *)heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_SPIRAM);
    if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
        sum->exc_task[sizeof(sum->exc_task) - 1] = 0;
        n += snprintf(s_crash + n, CRASH_MAX - n, "Task: %s\nPC: 0x%08lx\n", sum->exc_task, (unsigned long)sum->exc_pc);
        if (n < CRASH_MAX)
            n += snprintf(s_crash + n, CRASH_MAX - n, "Cause: %s (%lu) at 0x%08lx\n",
                          exc_name(sum->ex_info.exc_cause), (unsigned long)sum->ex_info.exc_cause,
                          (unsigned long)sum->ex_info.exc_vaddr);
        if (n < CRASH_MAX) n += snprintf(s_crash + n, CRASH_MAX - n, "Backtrace:");
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16 && n < CRASH_MAX; i++)
            n += snprintf(s_crash + n, CRASH_MAX - n, " 0x%08lx", (unsigned long)sum->exc_bt_info.bt[i]);
        if (n < CRASH_MAX && sum->exc_bt_info.corrupted) n += snprintf(s_crash + n, CRASH_MAX - n, " |<-CORRUPTED");
        if (n < CRASH_MAX) n += snprintf(s_crash + n, CRASH_MAX - n, "\nELF: %.16s\n", (const char *)sum->app_elf_sha256);
    }
    if (sum) heap_caps_free(sum);
    char panic[160];
    if (n < CRASH_MAX && esp_core_dump_get_panic_reason(panic, sizeof(panic)) == ESP_OK) {
        panic[sizeof(panic) - 1] = 0;
        n += snprintf(s_crash + n, CRASH_MAX - n, "Panic: %s\n", panic);
    }
#endif
    s_crash[CRASH_MAX - 1] = 0;
    esp_core_dump_image_erase();
    return true;
}

void diag_boot() {
    bool crash = false;
    s_reason = reason_name(esp_reset_reason(), &crash);
    s_was_crash = crash;

    Preferences p;
    if (!p.begin("diag", false)) return;
    s_boots = p.getULong("boots", 0) + 1;
    p.putULong("boots", s_boots);
    s_crashes = p.getULong("crashes", 0);
    s_crash_id = p.getULong("crash_id", 0);
    p.getString("crash", s_crash, sizeof(s_crash));

    bool have_dump = read_core_dump(s_reason);
    if (!have_dump && crash) {
        // Watchdog / brownout: no core dump, but still worth recording.
        snprintf(s_crash, sizeof(s_crash), "FW %s (%s)\nReset: %s\n(no core dump)\n", FW_VERSION, diag_build_date(), s_reason);
    }
    if (have_dump || crash) {
        s_crashes++;
        s_crash_id = s_boots;
        s_new_crash = true;
        p.putULong("crashes", s_crashes);
        p.putULong("crash_id", s_crash_id);
        p.putString("crash", s_crash);
        DEBUG_PRINTF("[diag] last reset: %s - crash report:\n%s\n", s_reason, s_crash);
    } else {
        DEBUG_PRINTF("[diag] FW %s, boot #%lu, last reset: %s\n", FW_VERSION, (unsigned long)s_boots, s_reason);
    }
    p.end();
}

void diag_after_storage() {
    if (!s_new_crash) return;
    if (sd_is_mounted()) {
        SD.mkdir("/crash");
        char path[40];
        snprintf(path, sizeof(path), "/crash/crash_%lu.txt", (unsigned long)s_crash_id);
        File f = SD.open(path, FILE_WRITE);
        if (f) { f.print(s_crash); f.close(); }
    }
    ui_show_toast("Recovered from a crash - see Settings > Diagnostics", 4000);
}

void diag_mark_good() {
    if (s_marked_valid) return;
    s_marked_valid = true;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        DEBUG_PRINTF("[diag] new firmware marked good\n");
    }
}

// 20 s of normal running: long enough to catch a boot loop or an early
// crash, short enough that a normal power-off / restart by the user rarely
// lands inside the window (those mark it good themselves anyway).
void diag_loop() {
    if (!s_marked_valid && millis() > 20000) diag_mark_good();
}

// The companion app finds this marker in a .bin before flashing it, to show
// the version and refuse files that aren't this firmware.
static const char FW_MARKER[] = "AMOLEDWATCH_FW=" FW_VERSION;   // NUL-terminated
const char *diag_fw_version() { return FW_MARKER + 15; }
const char *diag_build_date() { return __DATE__ " " __TIME__; }
const char *diag_reset_reason() { return s_reason; }
bool diag_last_reset_was_crash() { return s_was_crash; }
uint32_t diag_boot_count() { return s_boots; }
uint32_t diag_crash_count() { return s_crashes; }
const char *diag_crash_text() { return s_crash; }
uint32_t diag_crash_id() { return s_crash[0] ? s_crash_id : 0; }

void diag_clear_crash() {
    s_crash[0] = 0;
    Preferences p;
    if (p.begin("diag", false)) { p.remove("crash"); p.end(); }
}

void diag_status_text(char *out, int cap) {
    uint32_t up = millis() / 1000;
    PowerStatus ps = power_read();
    int n = snprintf(out, cap,
        "Firmware %s\nBuilt %s\nUptime %luh %02lum\nLast reset: %s\nBoots %lu, crashes %lu\n"
        "RAM free %u KB (block %u KB)\nPSRAM free %u KB\n",
        FW_VERSION, diag_build_date(), (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), s_reason,
        (unsigned long)s_boots, (unsigned long)s_crashes,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    if (n < cap && ps.valid)
        n += snprintf(out + n, cap - n, "Battery %d%% %.2f V%s\n", ps.battery_percent, ps.battery_voltage_v,
                      ps.is_charging ? " charging" : "");
    if (n < cap)
        n += snprintf(out + n, cap - n, "Bluetooth %s\nWiFi %s\nSD card %s\n",
                      !ble_is_enabled() ? "off" : ble_is_connected() ? "connected" : "on",
                      !wifi_is_enabled() ? "off" : wifi_is_connected() ? "connected" : "on",
                      sd_is_mounted() ? "mounted" : "none");
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (n < cap && run) snprintf(out + n, cap - n, "Slot %s", run->label);
}
