/*
 * hal_bambu.h
 * Discovers a Bambu Lab 3D printer on the local network and reads its
 * live print status over MQTT. Two things must be true on the
 * printer itself before this can work at all, neither of which this
 * code can detect or fix - both are printer-side settings:
 *   1. LAN Only Mode + Developer Mode enabled (Settings > LAN Only on
 *      the printer touchscreen). Firmware from January 2025 onward
 *      requires this for ANY third-party local access - without it,
 *      the printer will be discovered (SSDP still announces it) but
 *      MQTT will never connect, no matter how correct the access
 *      code is. bambu_printer_dev_mode_on() reports this directly
 *      from the discovery broadcast so the UI can say so rather than
 *      leave the user guessing at a silent connection failure.
 *   2. The 8-digit Access Code, read off the same LAN Only settings
 *      screen and entered once here (bambu_set_access_code()) -
 *      that's the MQTT password; the username is always "bblp" on
 *      every Bambu Lab printer.
 *
 * Requires WiFi connected (see hal_wifi.h) - this feature is useless
 * without it and does nothing until wifi_is_connected() is true.
 *
 * New library dependency: PubSubClient (by Nick O'Leary) - install
 * via Arduino Library Manager if not already present. TLS itself
 * comes from WiFiClientSecure, already built into the ESP32 Arduino
 * core. JSON parsing reuses ArduinoJson, already a dependency of this
 * project (see app_custom_watchface.cpp).
 */
#pragma once
#include <Arduino.h>

void bambu_enable();
void bambu_disable();
bool bambu_is_enabled();
void bambu_tick(); // services the discovery listener and MQTT client - call every loop()

void bambu_set_access_code(const char *code); // persists to NVS
bool bambu_has_access_code();

// ---- Controls (all no-ops if not connected; each publishes and returns
// immediately - the printer's next status report is what confirms it
// actually took effect, same "verify by reread, not by ACK" approach
// every third-party Bambu integration uses, since the plain MQTT publish
// itself doesn't tell you the command was accepted). ----
void bambu_set_light(bool on);
void bambu_set_speed(int level); // 1 silent, 2 standard, 3 sport, 4 ludicrous
void bambu_pause();
void bambu_resume();
void bambu_stop();
// path is relative to the SD card root as returned by bambu_ftp_list(),
// e.g. "benchy.gcode.3mf" or "timelapse/benchy.gcode.3mf". Chooses the
// right MQTT command from the extension (.3mf -> project_file via the
// ftp:// URL form, else gcode_file) - see hal_bambu.cpp for why.
void bambu_print_file(const char *path);

// ---- Camera (P1/A1-family only - TCP+TLS "Bambu Tunnel" on port 6000;
// X1-series uses RTSP instead, not implemented here, see
// bambu_camera_supported()). One JPEG frame at a time, decoded straight
// to the screen the same way app_media.cpp already decodes JPEGs from
// the SD card (JPEGDEC, no extra library). ----
bool bambu_camera_supported(); // false for X1-family models (RTSP, not this path)
void bambu_camera_open();      // opens the TCP+TLS connection; call once before polling frames
void bambu_camera_close();
// Polls the open connection for one complete frame. Returns true and
// fills *out_len the moment a full JPEG has been read into buf (caller
// then decodes it); false while still waiting or on no connection.
// Never blocks longer than one read() would - safe to call every tick.
bool bambu_camera_poll_frame(uint8_t *buf, size_t buf_cap, size_t *out_len);
bool bambu_camera_is_open();

// ---- File listing (FTPS, port 990 - the only documented way to see
// what's on the printer's storage; MQTT has no such command). Blocking
// (a full connect+list+close), so call it once when the file browser
// opens, not from a tick. Returns the number of entries filled, or -1
// on connection failure. is_dir lets the caller show folders (like
// "timelapse/") as navigable rather than printable. ----
#define BAMBU_MAX_FILES 40
struct BambuFileEntry {
    char name[64]; // relative to the directory just listed, no path
    bool is_dir;
    uint32_t size;
};
int bambu_ftp_list(const char *dir, BambuFileEntry *out, int max_entries);

// Discovery state - the printer announces itself periodically over
// UDP; nothing here is queried or guessed.
bool bambu_printer_found();
const char *bambu_printer_model();
const char *bambu_printer_ip_str();
const char *bambu_printer_serial();
bool bambu_printer_dev_mode_on(); // false = LAN Only/Developer Mode isn't on at the printer - MQTT cannot connect until it is, regardless of access code

bool bambu_mqtt_connected();

// Last MQTT connect failure code (PubSubClient state(): 4 = bad
// credentials, 5 = unauthorized - i.e. wrong Access Code - anything
// else is transport/TLS). 0 = no failure since the last success.
int bambu_mqtt_last_error();

// One filament tray. color565 is already converted from the printer's
// "RRGGBBAA" hex. remain is the spool's remaining % (-1 = unknown -
// third-party spools without an RFID tag don't report it).
struct BambuTray {
    bool present;
    uint16_t color565;
    char type[9]; // "PLA", "PETG", ...
    int8_t remain;
};

#define BAMBU_MAX_AMS 4

struct BambuPrintState {
    float nozzle_temp, nozzle_target;
    float bed_temp, bed_target;
    float chamber_temp;
    int percent;
    int remaining_min;
    int layer_num, total_layer_num;
    char gcode_state[16]; // \"IDLE\",\"PREPARE\",\"RUNNING\",\"PAUSE\",\"FINISH\",\"FAILED\" - whatever the printer's own report sends, copied as-is
    char filename[64];

    int stage;            // stg_cur: what the printer is doing right now (-1/255 = nothing special). See bambu_stage_text().
    int print_error;      // 0 = none
    int hms_count;        // active Health Management System warnings
    int speed_level;      // 1 silent, 2 standard, 3 sport, 4 ludicrous, 0 = unknown
    int speed_pct;        // spd_mag, e.g. 100
    int fan_part, fan_aux, fan_chamber, fan_heatbreak; // percent 0-100, -1 = not reported
    int wifi_dbm;         // printer's own WiFi signal, 0 = unknown
    bool light_known, chamber_light;
    char nozzle_diameter[8];

    int active_tray;      // tray_now: (unit*4 + slot) for AMS, 254 = external spool, 255 = none
    int ams_units;        // highest AMS unit index seen + 1 (0 = no AMS reported)
    BambuTray trays[BAMBU_MAX_AMS * 4];
    BambuTray ext_spool;

    uint32_t last_msg_ms; // millis() of the last parsed report
    bool valid;           // false until the first MQTT report has actually been parsed
};
const BambuPrintState &bambu_get_state();

// Human-readable text for a stg_cur code, or \"\" for printing/idle/unknown
// (nothing worth calling out). Codes verified against two independent
// protocol references (OpenBambuAPI + ha-bambulab).
const char *bambu_stage_text(int stage);
