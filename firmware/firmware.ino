/*
 * AmoledSmartWatchOS.ino
 * Smartwatch OS for the Waveshare ESP32-S3-Touch-AMOLED-1.75-G.
 *
 * Direct Arduino_GFX drawing — no LVGL.
 */

#include <Wire.h>
#include "complications.h"
#include "dnd.h"
#include <esp_pm.h>
#include "config.h"
#include "board_pins.h"

#include "hal_display.h"
#include "hal_touch.h"
#include "hal_expander.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "hal_imu.h"
#include "hal_gps.h"
#include "hal_sd.h"
#include "hal_audio.h"
#include "hal_buttons.h"
#include "hal_sleep.h"
#include "hal_nvs.h"
#include "hal_vibrate.h"
#include "hal_wifi.h"
#include "hal_bambu.h"
#include "hal_ntp.h"
#include "hal_ble.h"
#include "hal_espnow.h"
#include "hal_sd.h"
#include "hal_usb.h"
#include "ui.h"
#include "app_watchface.h"
#include "app_custom_watchface.h"
#include "watchface_registry.h"
#include "app_settings_state.h"
#include "app_notifications.h"
#include "anim_lock.h"
#include "app_onboarding.h"
#include "app_bambu_printer.h"
#include "app_poweroff.h"
#include "app_charging_anim.h"
#include "app_findme.h"
#include "phone_link.h"
#include "ui_font.h"
#include "diag.h"
#include "hal_ota.h"
#include "hal_wifi_xfer.h"
#include "wifi_cfg.h"
void pairing_update();       // app_pairing.cpp
#include "app_calendar.h"
#include <SD.h>
#include <esp_heap_caps.h>

// The whole UI (every screen's draw/tick, image and video decoding, the
// Gallery's thumbnail work) runs on Arduino's loopTask, whose default
// stack is 8 KB. A raw-MJPEG thumbnail overflowed it ("Stack canary
// watchpoint triggered (loopTask)"); that code is fixed, and this gives
// headroom for the rest. Costs 8 KB of internal RAM.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static uint32_t s_last_second_tick = 0;
static bool s_ble_was_connected = false;
static bool s_file_modal_shown = false;
static bool s_was_recording = false;
static void recordings_update_index();

static bool is_audio_name(const char *n) {
    int l = strlen(n);
    return l > 4 && (!strcasecmp(n + l - 4, ".wav") || !strcasecmp(n + l - 4, ".mp3"));
}

static void on_file_confirm(bool accepted) {
    if (accepted) {
        ble_file_approve();
    } else {
        ble_file_deny();
        s_file_modal_shown = false;
    }
}

static void check_file_transfer() {
    BleFileState st = ble_file_state();

    // Show modal when a new file transfer is pending
    if (st == BLE_FILE_PENDING && !s_file_modal_shown) {
        s_file_modal_shown = true;
        char body[128];
        int total = ble_file_total_size();
        const char *fn = ble_file_name();
        if (!strncmp(fn, "rec:", 4)) {
            snprintf(body, sizeof(body), "Add '%s' (%d KB) to your recordings?", fn + 4, total / 1024);
            ui_show_confirm("New recording", body, "Add", "Decline", on_file_confirm);
        } else {
            snprintf(body, sizeof(body), "Receive '%s' (%d bytes)?", fn, total);
            ui_show_confirm("File Transfer", body, "Accept", "Decline", on_file_confirm);
        }
    }

    // Save to SD when complete
    if (st == BLE_FILE_COMPLETE) {
        s_file_modal_shown = false;
#if FEATURE_SD_CARD
        const char *fn = ble_file_name();
        if (sd_is_mounted() && !strncmp(fn, "rec:", 4)) {
            // A recording from the phone: straight into /Recordings.
            const char *n = fn + 4;
            bool ok = false;
            if (usb_msc_host_active()) {
                ui_show_toast("SD card is in use over USB", 2000);
            } else if (sd_is_safe_filename(n) && n[0] != '_' && n[0] != '.' && is_audio_name(n)) {
                SD.mkdir("/Recordings");
                // Never overwrite: name.wav, name_2.wav, ...
                String nm(n), base = nm.substring(0, nm.length() - 4), ext = nm.substring(nm.length() - 4);
                String path = String("/Recordings/") + nm;
                for (int i = 2; SD.exists(path) && i < 100; i++) path = "/Recordings/" + base + "_" + i + ext;
                File f = SD.open(path, FILE_WRITE);
                if (f) {
                    ok = f.write(ble_file_data(), ble_file_bytes_received()) == (size_t)ble_file_bytes_received();
                    f.close();
                    if (!ok) SD.remove(path);
                }
            }
            if (ok) recordings_update_index();
            ui_show_toast(ok ? "Recording added" : "Couldn't save the recording", 2000);
        } else if (sd_is_mounted()) {
            bool ok = sd_save_file(ble_file_name(), ble_file_data(), ble_file_bytes_received());
            if (ok) {
                ui_show_toast("File saved to SD", 2000);
            } else {
                ui_show_toast("Save failed!", 2000);
            }
        } else {
            ui_show_toast("No SD card!", 2000);
        }
#else
        ui_show_toast("File received", 2000);
#endif
        ble_consume_file();
    }

    if (ble_consume_file_too_big()) ui_show_toast("Too big for Bluetooth (max 4 MB) - the app uses Wi-Fi for that", 2500);

    // Reset modal flag when transfer is idle/denied
    if (st == BLE_FILE_IDLE || st == BLE_FILE_DENIED) {
        s_file_modal_shown = false;
    }
}

// Bumps whenever the recordings change (the Recorder screen re-reads then).
static uint32_t s_rec_rev = 0;
uint32_t recordings_revision() { return s_rec_rev; }

// Scan /Recordings/ and write recordings.csv index (name,size per line)
static void recordings_update_index() {
    s_rec_rev++;
    if (!sd_is_mounted()) return;

    String csv = "";
    File root = SD.open("/Recordings");
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                const char *name = entry.name();
                int nlen = strlen(name);
                // "_" files are internal (voice-reply dictation), not memos
                if (nlen > 4 && name[0] != '_' && name[0] != '.' && is_audio_name(name)) {
                    csv += name;
                    csv += ",";
                    csv += String(entry.size());
                    csv += "\n";
                }
            }
            entry.close();
            entry = root.openNextFile();
        }
        root.close();
    }

    // Overwrite the index file
    SD.remove("/Recordings/recordings.csv");
    File idx = SD.open("/Recordings/recordings.csv", FILE_WRITE);
    if (idx) {
        idx.print(csv);
        idx.close();
        DEBUG_PRINTF("[main] recordings index updated: %d bytes\n", csv.length());
    }
}

// --- Non-blocking file download state ---
static File     s_dl_file;
static int     s_dl_chunkIdx = 0;
static int     s_dl_totalChunks = 0;
static int     s_dl_fileSize = 0;
static bool    s_dl_active = false;
static uint32_t s_dl_last_chunk_ms = 0; // timeout tracking
static int     s_dl_chunk = 480;        // payload bytes per chunk, fits the link's MTU

static char    s_dl_path[80] = "";
static volatile bool s_dl_cancel_req = false;   // set by the BLE task on disconnect

bool notes_download_active() { return s_dl_active; }
void notes_request_cancel() { s_dl_cancel_req = true; }

// Is this SD path being sent right now (Bluetooth or Wi-Fi)? Deleting or
// overwriting an open file corrupts both it and the card's free space.
bool recordings_file_busy(const char *path) {
    if (s_dl_active && !strcmp(path, s_dl_path)) return true;
    return wifi_xfer_file_busy(path);
}

// Returns false if the chunk couldn't be queued (BLE out of buffers): try
// again on the next loop.
static bool notes_send_one_chunk() {
    if (!s_dl_active) return true;
    const int CHUNK_SIZE = s_dl_chunk;
    uint8_t buf[480];
    if (s_dl_file.available() && s_dl_chunkIdx < s_dl_totalChunks) {
        int toRead = CHUNK_SIZE;
        int remaining = s_dl_fileSize - (s_dl_chunkIdx * CHUNK_SIZE);
        if (toRead > remaining) toRead = remaining;
        uint32_t pos = s_dl_file.position();
        int got = s_dl_file.read(buf, toRead);
        if (got > 0) {
            if (!ble_notes_send_file_chunk(s_dl_chunkIdx, s_dl_totalChunks, buf, got)) {
                s_dl_file.seek(pos);   // resend this one
                return false;
            }
            s_dl_chunkIdx++;
            // The last one: finish now, not on the next credit (the next
            // download's window would be spent on this one's "done").
            if (s_dl_chunkIdx < s_dl_totalChunks) return true;
        }
    }
    // Done — all chunks sent
    s_dl_file.close();
    s_dl_active = false;
    s_dl_path[0] = 0;
    ble_notes_clear_chunk_credits();
    delay(50); // let last chunk flush
    ble_notes_send_transfer_complete();
    DEBUG_PRINTF("[main] notes: sent file (%d bytes, %d chunks)\n",
                 s_dl_fileSize, s_dl_chunkIdx);
    return true;
}

void notes_cancel_download() {
    ble_notes_clear_chunk_credits();
    if (s_dl_active) {
        s_dl_file.close();
        s_dl_active = false;
        s_dl_path[0] = 0;
        DEBUG_PRINTF("[main] notes: download cancelled\n");
    }
}

// Handle notes sync commands from the companion app
static void check_notes_sync() {
    if (s_dl_cancel_req) {
        s_dl_cancel_req = false;
        notes_cancel_download();
    }
    // A new download request replaces one still running (the app gave up on
    // it) instead of waiting for its 15 s timeout.
    if (s_dl_active && ble_notes_get_cmd() == BLE_NOTES_CMD_DOWNLOAD_FILE) notes_cancel_download();
    // Flow-controlled download: send one chunk per REQUEST_CHUNK from companion
    if (s_dl_active) {
        // Timeout: if no chunk request in 15s, companion probably gave up
        if (millis() - s_dl_last_chunk_ms > 15000) {
            notes_cancel_download();
            DEBUG_PRINTF("[main] notes: download timed out (no chunk request in 15s)\n");
        } else {
            // Send what the app has room for (it keeps a window of chunks
            // in flight instead of asking for each one).
            int credits = ble_notes_take_chunk_credits();
            int sent = 0;
            while (sent < credits && s_dl_active) {
                if (!notes_send_one_chunk()) break;
                sent++;
            }
            if (sent < credits && s_dl_active) ble_notes_return_chunk_credits(credits - sent);
            if (sent) s_dl_last_chunk_ms = millis(); // reset timeout after sending
        }
        return;
    }

    BleNotesCmd cmd = ble_notes_get_cmd();
    if (cmd == BLE_NOTES_CMD_NONE) return;

    // Consume command + save args BEFORE any blocking work.
    // ble_notes_send_file_list() sends many BLE notifications (blocking with
    // delay(10) each). During that time, the companion can receive the list
    // and send a new DOWNLOAD_FILE command (0x02). If we don't consume the
    // LIST_FILES command first, ble_notes_consume_cmd() at the bottom would
    // clear the newly-arrived DOWNLOAD_FILE command — losing it forever.
    char savedArg[64] = {0};
    if (cmd == BLE_NOTES_CMD_DOWNLOAD_FILE || cmd == BLE_NOTES_CMD_DELETE_FILE) {
        const char *arg = ble_notes_cmd_arg();
        if (arg) strncpy(savedArg, arg, sizeof(savedArg) - 1);
    }
    ble_notes_consume_cmd();

    if (cmd == BLE_NOTES_CMD_LIST_FILES) {
        // Read from cached CSV index if available, else scan + regenerate
        recordings_update_index();

        File idx = SD.open("/Recordings/recordings.csv", FILE_READ);
        if (idx) {
            // CSV is small — read it all and send as JSON array
            String csvContent = "";
            while (idx.available()) {
                char buf[256];
                int got = idx.read((uint8_t*)buf, sizeof(buf) - 1);
                if (got <= 0) break;
                buf[got] = '\0';
                csvContent += buf;
            }
            idx.close();

            // Convert CSV (name,size\n) to JSON array
            String json = "[";
            bool first = true;
            int pos = 0;
            while (pos < csvContent.length()) {
                int eol = csvContent.indexOf('\n', pos);
                if (eol < 0) eol = csvContent.length();
                String line = csvContent.substring(pos, eol);
                pos = eol + 1;
                if (line.length() == 0) continue;

                int comma = line.indexOf(',');
                if (comma < 0) continue;
                String name = line.substring(0, comma);
                String sz = line.substring(comma + 1);
                name.trim();
                sz.trim();
                if (name.length() == 0) continue;

                if (!first) json += ",";
                first = false;
                json += "{\"name\":\"" + name + "\",\"size\":" + sz + "}";
            }
            json += "]";
            if (!ble_notes_send_file_list(json.c_str())) {
                DEBUG_PRINTF("[main] notes: failed to send file list (disconnected?)\n");
            }
            DEBUG_PRINTF("[main] notes: sent file list from CSV index (%d bytes)\n", json.length());
        } else {
            // Fallback: scan directory (shouldn't happen after first recording)
            String json = "[";
            File root = SD.open("/Recordings");
            if (root && root.isDirectory()) {
                bool first = true;
                File entry = root.openNextFile();
                while (entry) {
                    if (!entry.isDirectory()) {
                        const char *name = entry.name();
                        int nlen = strlen(name);
                        if (nlen > 4 && name[0] != '_' && name[0] != '.' && is_audio_name(name)) {
                            if (!first) json += ",";
                            first = false;
                            json += "{\"name\":\"";
                            json += name;
                            json += "\",\"size\":";
                            json += String(entry.size());
                            json += "}";
                        }
                    }
                    entry.close();
                    entry = root.openNextFile();
                }
                root.close();
            }
            json += "]";
            if (!ble_notes_send_file_list(json.c_str())) {
                DEBUG_PRINTF("[main] notes: failed to send file list (disconnected?)\n");
            }
            DEBUG_PRINTF("[main] notes: sent file list by scan (%d bytes)\n", json.length());
        }
    }

    if (cmd == BLE_NOTES_CMD_DOWNLOAD_FILE) {
        if (!sd_is_safe_filename(savedArg)) {
            DEBUG_PRINTF("[main] notes: rejected unsafe download filename '%s'\n", savedArg);
        } else if (usb_msc_host_active()) {
            DEBUG_PRINTF("[main] notes: download deferred, SD card is mounted over USB\n");
        } else {
        String path = String("/Recordings/") + savedArg;
        File f = SD.open(path, FILE_READ);
        if (f) {
            s_dl_fileSize = f.size();
            // 5 header bytes per notification; anything past the MTU
            // would be cut off silently (iOS typically negotiates ~185).
            s_dl_chunk = ble_notify_payload() - 5;
            if (s_dl_chunk > 480) s_dl_chunk = 480;
            if (s_dl_chunk < 16) s_dl_chunk = 16;
            s_dl_totalChunks = (s_dl_fileSize + s_dl_chunk - 1) / s_dl_chunk;
            s_dl_chunkIdx = 0;
            s_dl_file = f; // move file handle to static
            snprintf(s_dl_path, sizeof(s_dl_path), "%s", path.c_str());
            s_dl_active = true;
            s_dl_last_chunk_ms = millis(); // start timeout tracking
            DEBUG_PRINTF("[main] notes: starting download '%s' (%d bytes, %d chunks)\n",
                         savedArg, s_dl_fileSize, s_dl_totalChunks);
            // Push chunk 0 right away rather than waiting for the app's
            // first REQUEST_CHUNK - see comment above for why waiting
            // for it is racy.
            notes_send_one_chunk();
        } else {
            DEBUG_PRINTF("[main] notes: file not found: %s\n", path.c_str());
        }
        }
    }

    if (cmd == BLE_NOTES_CMD_DELETE_FILE) {
        if (!sd_is_safe_filename(savedArg)) {
            DEBUG_PRINTF("[main] notes: rejected unsafe delete filename '%s'\n", savedArg);
        } else {
        String path = String("/Recordings/") + savedArg;
        if (recordings_file_busy(path.c_str())) {
            DEBUG_PRINTF("[main] notes: %s is being transferred, not deleted\n", savedArg);
        } else if (SD.exists(path)) {
            SD.remove(path);
            recordings_update_index();
            DEBUG_PRINTF("[main] notes: deleted %s\n", savedArg);
        }
        }
    }

    if (cmd == BLE_NOTES_CMD_TRANSCRIPT) {
        const char *transcript = ble_notes_get_transcript();
        if (transcript) {
            // "<recording name>\n<text>" -> /Recordings/<name minus .wav>.txt
            const char *nl = strchr(transcript, '\n');
            char name[64] = {0};
            if (nl && nl - transcript < (int)sizeof(name)) memcpy(name, transcript, nl - transcript);
            char *dot = strrchr(name, '.');
            if (dot && !strcasecmp(dot, ".wav")) *dot = 0;
            if (name[0] && sd_is_safe_filename(name) && sd_is_mounted() && !usb_msc_host_active()) {
                String path = String("/Recordings/") + name + ".txt";
                SD.remove(path);
                File tf = SD.open(path, FILE_WRITE);
                if (tf) {
                    tf.print(nl + 1);
                    tf.close();
                    ui_show_toast("Transcript saved", 1500);
                }
                DEBUG_PRINTF("[main] notes: transcript -> %s (%d bytes)\n", path.c_str(), (int)strlen(nl + 1));
            } else {
                DEBUG_PRINTF("[main] notes: transcript ignored (bad name or no SD)\n");
            }
        }
        ble_notes_consume_transcript();
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);
    DEBUG_PRINTF("\n=== ESP32-S3-Touch-AMOLED Smart Watch OS (Arduino_GFX) ===\n");

    // Dynamic frequency scaling (80-240MHz) plus automatic light
    // sleep when it's safe to - see hal_power.cpp's
    // power_update_sleep_policy() for the full reasoning. That call
    // happens later in setup(), after BLE/WiFi/ESP-NOW have been
    // conditionally started from saved settings, so its first
    // decision already reflects real post-boot radio state rather
    // than guessing at boot time before any of them have run.

    randomSeed(esp_random());
    Wire.begin(PIN_IIC_SDA, PIN_IIC_SCL, IIC_CLOCK_HZ);

#if FEATURE_NVS
    nvs_init();
    nvs_load_settings(g_app_settings);
#endif
    diag_boot();   // reset reason + crash report from the last run

#if FEATURE_IO_EXPANDER
    expander_init();
#endif

    display_init();
    display_set_brightness(g_app_settings.brightness);
    ui_fonts_set_smooth(g_app_settings.smooth_fonts);

    // Init screen manager with the display
    ui_init(display_gfx());

    touch_init();

    power_init();
    rtc_init();
    imu_init();

#if FEATURE_SD_CARD
    sd_init();
#endif
    // USB Mode from Settings (charging only / firmware / file storage /
    // media remote) - after sd_init(), File storage serves that card.
    usb_mode_init();
#if FEATURE_AUDIO
    // Audio before the GPS probe: the codecs get the I2C bus and power
    // rails to themselves while they're configured.
    quick_replies_load();                                  // dnd.cpp
    comp_load();                                           // watchface slots
    audio_set_mic_sensitivity(g_app_settings.mic_sens);   // used by the codec init below
    audio_init();
    audio_set_volume(g_app_settings.volume);
#endif
#if FEATURE_GPS
    // Probe for the module (only fitted on the -G board) - non-blocking,
    // decided in loop() by gps_probe_update(); keeps running if gps_on.
    gps_probe_begin();
#endif

    buttons_init();
    sleep_init();
    vibrate_init();

#if FEATURE_BLE
    if (g_app_settings.ble_on && !ble_init()) {
        // Don't keep retrying (and failing) on every boot.
        g_app_settings.ble_on = false;
        nvs_save_settings(g_app_settings);
    }
    if (g_app_settings.espnow_on) {
        espnow_enable();
    }
#endif

#if FEATURE_WIFI
    if (g_app_settings.wifi_on) {
        wifi_enable();
        ntp_init();
        ntp_sync(); // ntp_init() alone only resets state to idle - this is the actual trigger, previously missing entirely, which meant WiFi connected at boot and then simply stayed connected forever with no time sync ever happening
    }
#endif

    // Now that BLE/WiFi/ESP-NOW have been conditionally started from
    // saved settings (or not), this first call sets the actual
    // correct policy for however the watch is really configured,
    // rather than a boot-time guess.
    power_update_sleep_policy();

    // Boot into the watchface
    watchface_registry_init();

    // If we have a persisted custom watchface but aren't on it, switch
    if (watchface_current() != 2) {
#if FEATURE_NVS
        if (nvs_has_watchface_json()) {
            DEBUG_PRINTF("[boot] NVS has custom watchface, switching to index 2\n");
            watchface_set_current(2);
        }
#endif
    }

    const WatchfaceEntry *wf = watchface_get(watchface_current());
    if (wf && wf->screen) ui_push(wf->screen);

    if (!g_app_settings.onboarding_complete) {
        // First-ever boot (or after an NVS wipe) - the welcome/setup
        // flow replaces the usual power-on animation below, pushed on
        // top of the fresh watchface the same way that animation
        // normally is, so finishing onboarding's own ui_pop_screen()
        // reveals the watchface underneath exactly like the power-on
        // animation's self-pop already does on every other boot.
        ui_push(&onboarding_screen);
    } else {
        // Power-on animation - reuses the same particle-burst effect the
        // lock screen already plays on wake (see anim_lock.cpp), pushed on
        // top of the fresh watchface so it plays once at boot then
        // self-pops to reveal it underneath. Genuinely reused, not a
        // separate new animation system, since it's the same visual and
        // there's no reason to duplicate it.
        lock_anim_set_mode(true, false);
        ui_push(&lock_anim_screen);
    }

    diag_after_storage();
    DEBUG_PRINTF("=== boot complete ===\n\n");
    DEBUG_PRINTF("[mem] internal free %u (largest block %u), PSRAM free %u\n",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() {
    uint32_t now = millis();

    buttons_update();
    ButtonEvent boot_evt = buttons_get_event(BTN_BOOT);
    ButtonEvent pwr_evt = buttons_get_event(BTN_PWR);

    bool was_asleep = sleep_is_asleep();

    if (boot_evt != BTN_EVENT_NONE || pwr_evt != BTN_EVENT_NONE) {
        sleep_register_activity();
    }

    bool asleep = sleep_update();
    diag_loop();
    ota_update();                // firmware update from the phone (flash writes)
    wifi_xfer_update();          // recordings over Wi-Fi, when the app asks
    wifi_cfg_update();           // Wi-Fi setup / check from the app
    pairing_update();            // shows the pairing code when a phone pairs
    if (wifi_xfer_take_changes()) {
        recordings_update_index();
        ui_show_toast("Recording added from phone", 2000);
    }
    if (int nf = wifi_xfer_take_files_received()) {
        char t[40];
        snprintf(t, sizeof(t), nf == 1 ? "File received" : "%d files received", nf);
        ui_show_toast(t, 2000);
    }
    imu_steps_poll();            // hardware pedometer + midnight rollover
    calendar_update();           // event reminders (works without the phone)
#if FEATURE_AUDIO
    power_set_screen_off(asleep && !audio_is_playing());   // MP3 decode needs the clock
#else
    power_set_screen_off(asleep);
#endif
#if FEATURE_BLE
    ble_set_low_power(asleep);
#endif
    usb_mode_update();           // cable poll (2 Hz) + drive eject handling
    power_update_sleep_policy(); // cheap no-op unless a radio's on/off state actually changed since the last check

    if (!was_asleep && !asleep) {
        if (boot_evt == BTN_EVENT_SHORT_PRESS) {
            // BOOT = back: closes the top panel first if it's down.
            if (ui_top_panel_is_open()) ui_top_panel_close();
            else ui_pop_screen();
        } else if (boot_evt == BTN_EVENT_LONG_PRESS) {
            if (ui_top_panel_is_open()) ui_top_panel_close();
            ui_go_home();
        }
        if (pwr_evt == BTN_EVENT_SHORT_PRESS) {
            // Play the "locking" particle animation first; it calls
            // sleep_force_sleep() itself once it finishes, so the
            // display doesn't go dark until the animation has actually
            // been seen (see anim_lock.cpp).
            lock_anim_set_mode(false, true);
            ui_push(&lock_anim_screen);
        } else if (pwr_evt == BTN_EVENT_DOUBLE_PRESS) {
            ui_pop_screen();
        } else if (pwr_evt == BTN_EVENT_LONG_PRESS) {
            ui_push(&poweroff_screen);
        }
    } else if (was_asleep && !asleep) {
        // Just woke up - play the "unlocking" particle animation on
        // top of whatever screen was showing before sleep.
        lock_anim_set_mode(true, false);
        ui_push(&lock_anim_screen);
    }

    if (asleep) {
        // Keep BLE alive during sleep — NimBLE has its own task but we
        // still need to process received data (time sync, watchface, etc.)
#if FEATURE_BLE
        ble_update();
        notifications_update();
        phone_link_update();
        check_notes_sync();
#endif
#if FEATURE_WIFI
        bambu_tick(); // internally a no-op unless enabled and WiFi is connected, same as the NTP poll above
#endif
#if FEATURE_BLE

        if (ble_consume_findme_request()) {
            // Wake the display first - a "find me" response nobody can
            // see because the screen is still off defeats the purpose.
            sleep_register_activity();
            ui_push(&findme_screen);
        }

        int y, mo, d, h, mi, s;
        if (ble_get_phone_time(y, mo, d, h, mi, s)) {
            rtc_set(y, mo, d, h, mi, s);
        }

        // Auto-switch to custom watchface even during sleep
        if (ble_watchface_ready() && watchface_current() != 2) {
            watchface_set_current(2);
        }
#endif
#if FEATURE_WIFI
        // If a sync was already in progress when the screen locked
        // (WiFi just got toggled on, then the user locked before the
        // few-second sync finished), this keeps it moving to
        // completion instead of leaving the radio connected - or even
        // just stuck mid-connect, never reaching its own timeout -
        // indefinitely with nothing left to ever call wifi_disable().
        NtpStatus ntp_st = ntp_get_status();
        if (ntp_st == NTP_CONNECTING || ntp_st == NTP_SYNCING) {
            ntp_update();
        }
#endif
#if FEATURE_AUDIO
        audio_idle_power();
#endif
        // Slower poll cadence than the awake path (was 50ms) - locked
        // state has nothing time-critical happening, so there's no
        // reason to re-check touch/motion/BLE this often. 150ms is
        // still imperceptible as wake latency for a wrist-raise/tap
        // gesture, but cuts the I2C touch/PMU polling and BLE
        // processing rate to roughly a third of what it was.
        // With all radios off it light-sleeps through the wait instead
        // (hal_power.cpp) - unless audio, GPS or an update needs the chip.
        bool keep_awake = ota_active();
#if FEATURE_AUDIO
        keep_awake = keep_awake || audio_is_playing() || audio_is_recording();
#endif
#if FEATURE_GPS
        keep_awake = keep_awake || gps_is_enabled();
#endif
        if (notes_download_active() || wifi_xfer_active()) delay(4);   // a transfer is running: keep it moving
        else if (keep_awake || !power_light_sleep(150)) delay(150);
        return;
    }

    // Auto-dim while awake but idle (before the longer sleep_timeout
    // eventually locks the screen entirely) - AMOLED power draw scales
    // with how bright/how many pixels are lit, unlike an LCD backlight
    // which draws roughly the same regardless of content, so this is
    // one of the most directly effective levers available for the
    // "idle but not locked" state specifically. Restores full
    // brightness immediately on any activity - sleep_register_activity()
    // already gets called from every real input path (touch, buttons,
    // controller), so this piggybacks on that rather than tracking
    // its own separate timer.
#define AUTO_DIM_AFTER_MS 8000
#define AUTO_DIM_LEVEL 50
    {
        static bool s_auto_dimmed = false;
        bool should_dim = !ui_current_screen_suppresses_idle() &&
                          sleep_ms_since_activity() >= AUTO_DIM_AFTER_MS;
        if (should_dim && !s_auto_dimmed) {
            s_auto_dimmed = true;
            // Never "dim" to something brighter than the user's own level
            // (Ultra saver runs at 40, below the fixed AUTO_DIM_LEVEL).
            uint8_t dim = g_app_settings.brightness * 2 / 5;
            if (dim > AUTO_DIM_LEVEL) dim = AUTO_DIM_LEVEL;
            if (dim < 8) dim = 8;
            display_set_brightness(dim);
        } else if (!should_dim && s_auto_dimmed) {
            s_auto_dimmed = false;
            display_set_brightness(g_app_settings.brightness);
        }
    }

    // Read touch and route to active screen
    TouchPoint tp = touch_read();
    static bool s_was_touched = false;
    if (tp.touched) {
        ui_handle_touch(tp.x, tp.y, true);
    } else if (s_was_touched) {
        ui_handle_touch(0, 0, false);
    }
    s_was_touched = tp.touched;

    // Draw the active screen
    ui_update();

    // Poll swipe gestures and route to active screen
    ui_poll_gestures();

    // Touch feedback for sleep timer
    if (touch_is_currently_touched()) {
        sleep_register_activity();
    }

    // 1-second tick
    if (now - s_last_second_tick >= 1000) {
        s_last_second_tick = now;
        // NOTE: per-screen on_tick (game logic) now runs every render
        // frame from ui_update() in ui.cpp, not here - it used to be
        // gated behind this once-a-second block via ui_tick(), which
        // silently capped every game's simulation to ~1 update/sec
        // (each game's own internal millis() sub-stepping never got a
        // chance to fire more than once). This block now only drives
        // the once-a-second watchface readout (clock/battery/steps).

        const WatchfaceEntry *wf = watchface_get(watchface_current());
        if (wf && wf->tick) wf->tick();
    }

    vibrate_update();

#if FEATURE_AUDIO
    audio_update();
    // Rebuild CSV index when recording stops
    if (s_was_recording && !audio_is_recording()) {
        s_was_recording = false;
        recordings_update_index();
    }
    if (audio_is_recording()) s_was_recording = true;
#endif

#if FEATURE_GPS
    gps_poll();
    gps_probe_update();
#endif

#if FEATURE_WIFI
    {
        // Checking the actual NTP state machine here, not just
        // wifi_is_connected() - a sync stuck waiting for the initial
        // connection (bad saved credentials, AP out of range) would
        // never see wifi_is_connected() become true, which meant its
        // own 15-second timeout inside ntp_update() never actually
        // got a chance to run, leaving the radio attempting to
        // connect indefinitely instead of giving up and disabling.
        NtpStatus ntp_st = ntp_get_status();
        if (ntp_st == NTP_CONNECTING || ntp_st == NTP_SYNCING) {
            ntp_update();
        }
    }
#endif

#if FEATURE_BLE
    ble_update();
    notifications_update();
    phone_link_update();
#endif
#if FEATURE_WIFI
    bambu_tick(); // internally a no-op unless enabled and WiFi is connected, same as the NTP poll below
#endif
#if FEATURE_BLE

    if (ble_consume_findme_request()) {
        ui_push(&findme_screen);
    }

    // Auto-sync time on first connect
    if (ble_is_connected() && !s_ble_was_connected) {
        s_ble_was_connected = true;
        int y, mo, d, h, mi, s;
        if (ble_get_phone_time(y, mo, d, h, mi, s)) {
            rtc_set(y, mo, d, h, mi, s);
            DEBUG_PRINTF("[main] time set from BLE phone (auto-sync)\n");
        }
    }
    if (!ble_is_connected()) {
        s_ble_was_connected = false;
    }

    int y, mo, d, h, mi, s;
    if (ble_get_phone_time(y, mo, d, h, mi, s)) {
        rtc_set(y, mo, d, h, mi, s);
        DEBUG_PRINTF("[main] time set from BLE phone\n");
    }

    // Check for file transfer events (modal + SD save)
    check_file_transfer();

    // Check for notes sync commands (file list, download, etc.)
    check_notes_sync();

    // Check for watchface data — auto-switch to custom watchface
    if (ble_watchface_ready() && watchface_current() != 2) {
        DEBUG_PRINTF("[main] watchface data received, switching to custom\n");
        watchface_set_current(2);
        ui_go_home();
        const WatchfaceEntry *wf = watchface_get(2);
        if (wf && wf->screen) ui_push(wf->screen);
    }

    static uint32_t s_last_ble_bat_ms = 0;
    if (millis() - s_last_ble_bat_ms >= 30000) {
        s_last_ble_bat_ms = millis();
        int bat = power_get_battery_percent();
        if (bat >= 0) ble_update_battery((uint8_t)bat);
    }
#endif

    // Charging-started animation - checked on its own 1s throttle
    // (not every loop iteration) since power_is_charging() is a fresh
    // PMU I2C read each call, not a cached value, and this doesn't
    // need faster-than-1s responsiveness to feel prompt.
    static uint32_t s_last_charge_check_ms = 0;
    static bool s_was_charging = false;
    if (millis() - s_last_charge_check_ms >= 1000) {
        s_last_charge_check_ms = millis();
        bool charging_now = power_is_charging();
        if (charging_now && !s_was_charging) {
            ui_push(&charging_anim_screen);
        }
        s_was_charging = charging_now;
    }

    delay(5);
}
