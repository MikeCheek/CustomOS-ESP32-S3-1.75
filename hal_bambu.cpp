#include "hal_bambu.h"
#include "config.h"
#include "hal_wifi.h"
#include "hal_nvs.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ctype.h>
#include <ArduinoJson.h>
#include <string.h>

#if FEATURE_WIFI

#define SSDP_PORT 2021
#define MQTT_PORT 8883
#define CAMERA_PORT 6000
#define FTP_PORT 990
#define MQTT_RECONNECT_INTERVAL_MS 5000

static bool s_enabled = false;
static WiFiUDP s_udp;

static bool s_printer_found = false;
static char s_printer_model[24] = "";
static char s_printer_model_display[24] = ""; // human-readable, derived below
static bool s_camera_supported = false;

// DevModel.bambu.com is an opaque vendor code, not a human-readable name -
// confirmed against several independent SSDP captures (a P1S reports "C12",
// an A1 mini reports "N1", an X1 Carbon reports the legacy string
// "3DPrinter-X1-Carbon"). This also decides camera support: P1/A1-family
// printers expose the TCP+TLS "Bambu Tunnel" snapshot stream this file
// implements; X1-family and H2-family use RTSP instead, which isn't
// implemented here. An unrecognized code is treated as camera-unsupported
// rather than guessed, since a wrong guess here would just hang a
// connection attempt against a stream that was never going to answer.
static void model_code_to_display() {
    struct { const char *code; const char *name; bool cam; } TABLE[] = {
        { "C11", "P1P", true },
        { "C12", "P1S", true },
        { "N1", "A1 mini", true },
        { "N2S", "A1", true },
        { "C13", "X1E", false },
        { "3DPrinter-X1-Carbon", "X1 Carbon", false },
        { "3DPrinter-X1", "X1", false },
    };
    for (auto &t : TABLE) {
        if (strcmp(s_printer_model, t.code) == 0) {
            snprintf(s_printer_model_display, sizeof(s_printer_model_display), "%s", t.name);
            s_camera_supported = t.cam;
            return;
        }
    }
    // Unknown code (new/uncommon model) - show it verbatim rather than a
    // blank, and default the camera off rather than guess a protocol.
    snprintf(s_printer_model_display, sizeof(s_printer_model_display), "%s", s_printer_model);
    s_camera_supported = false;
}
static char s_printer_ip_str[24] = "";
static IPAddress s_printer_ip;
static char s_printer_serial[24] = "";
static bool s_dev_mode_on = false;

static char s_access_code[16] = "";
static bool s_has_access_code = false;

static WiFiClientSecure s_tls;
static PubSubClient s_mqtt(s_tls);
static uint32_t s_last_mqtt_attempt_ms = 0;
static uint32_t s_next_pushall_ms = 0;
static uint32_t s_last_ssdp_ms = 0;
static int s_last_rc = 0;

static BambuPrintState s_state;

// A powered-off printer must not look "found" forever: every reconnect
// attempt to a dead IP is a blocking TLS connect, which freezes the whole
// UI for its duration. Discovery expires if the printer stops announcing
// itself (only while MQTT isn't up - a live connection proves it's there).
#define SSDP_EXPIRY_MS 180000UL

static void reset_state() {
    memset(&s_state, 0, sizeof(s_state));
    s_state.stage = -1;
    s_state.fan_part = s_state.fan_aux = s_state.fan_chamber = s_state.fan_heatbreak = -1;
    s_state.active_tray = 255;
    for (auto &t : s_state.trays) t.remain = -1;
    s_state.ext_spool.remain = -1;
}

// ---- SSDP discovery -------------------------------------------------------
// The printer periodically broadcasts its own SSDP-style NOTIFY on UDP
// 2021 - this only ever listens, never queries. Payload is plain
// "Key: Value" lines, not XML - a handful of fields matter here:
// DevModel.bambu.com, DevConnect.bambu.com ("lan" once Developer Mode
// is actually on, "cloud" otherwise), and USN (the serial number).
static void parse_ssdp_line(const char *line) {
    const char *colon = strchr(line, ':');
    if (!colon) return;
    int key_len = colon - line;
    const char *val = colon + 1;
    while (*val == ' ') val++;
    // Trim trailing CR/LF/whitespace from val in place isn't possible
    // (const), so bound copies below by explicit length instead.
    int val_len = (int)strlen(val);
    while (val_len > 0 && (val[val_len - 1] == '\r' || val[val_len - 1] == '\n' || val[val_len - 1] == ' ')) val_len--;

    auto starts_with = [&](const char *k) { return (int)strlen(k) == key_len && strncmp(line, k, key_len) == 0; };

    if (starts_with("DevModel.bambu.com")) {
        int n = val_len < (int)sizeof(s_printer_model) - 1 ? val_len : (int)sizeof(s_printer_model) - 1;
        memcpy(s_printer_model, val, n); s_printer_model[n] = 0;
        model_code_to_display();
    } else if (starts_with("DevConnect.bambu.com")) {
        s_dev_mode_on = (val_len == 3 && strncmp(val, "lan", 3) == 0);
    } else if (starts_with("USN")) {
        int n = val_len < (int)sizeof(s_printer_serial) - 1 ? val_len : (int)sizeof(s_printer_serial) - 1;
        memcpy(s_printer_serial, val, n); s_printer_serial[n] = 0;
    }
}

static void poll_ssdp() {
    int len = s_udp.parsePacket();
    if (len <= 0) return;
    static char buf[512];
    int n = s_udp.read(buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;

    s_printer_ip = s_udp.remoteIP();
    snprintf(s_printer_ip_str, sizeof(s_printer_ip_str), "%d.%d.%d.%d",
             s_printer_ip[0], s_printer_ip[1], s_printer_ip[2], s_printer_ip[3]);

    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next = 0;
        parse_ssdp_line(line);
        line = next ? next + 1 : nullptr;
    }
    s_printer_found = (s_printer_serial[0] != '\0');
    s_last_ssdp_ms = millis();
}

// ---- MQTT -----------------------------------------------------------------

// X1-series printers publish their FULL status object every time (large -
// AMS, HMS, camera, upgrade state...); P1/A1-series publish only what
// changed since the last report. Either way this parses with an
// ArduinoJson filter so only the handful of fields the UI shows are ever
// stored - the unfiltered report can exceed any sensible RAM budget - and
// every field is only overwritten if it's actually present, so a partial
// P1 update never blanks out values it simply didn't mention.
static StaticJsonDocument<1536> s_filter;
static bool s_filter_built = false;

static void build_filter() {
    if (s_filter_built) return;
    static const char *const KEYS[] = {
        "nozzle_temper", "nozzle_target_temper", "bed_temper", "bed_target_temper",
        "chamber_temper", "mc_percent", "mc_remaining_time", "layer_num",
        "total_layer_num", "gcode_state", "subtask_name", "stg_cur", "print_error",
        "spd_lvl", "spd_mag", "cooling_fan_speed", "big_fan1_speed", "big_fan2_speed",
        "heatbreak_fan_speed", "wifi_signal", "nozzle_diameter",
    };
    JsonObject p = s_filter.createNestedObject("print");
    for (const char *k : KEYS) p[k] = true;
    p["hms"][0]["attr"] = true;
    p["lights_report"][0] = true;
    p["ams"]["tray_now"] = true;
    p["ams"]["ams"][0]["id"] = true;
    p["ams"]["ams"][0]["tray"][0]["id"] = true;
    p["ams"]["ams"][0]["tray"][0]["tray_type"] = true;
    p["ams"]["ams"][0]["tray"][0]["tray_color"] = true;
    p["ams"]["ams"][0]["tray"][0]["remain"] = true;
    // Sentinel only: an empty slot arrives as just {"id":"N"}; keeping one
    // always-present-when-loaded key lets us tell "empty" from "partial update".
    p["ams"]["ams"][0]["tray"][0]["tray_info_idx"] = true;
    p["vt_tray"]["tray_type"] = true;
    p["vt_tray"]["tray_color"] = true;
    p["vt_tray"]["remain"] = true;
    s_filter_built = true;
}

// Bambu sends many numbers as strings ("15", "-45dBm"); accept both.
static int json_int(JsonVariantConst v, int fallback) {
    if (v.isNull()) return fallback;
    if (v.is<const char *>()) {
        const char *str = v.as<const char *>();
        return str ? atoi(str) : fallback;
    }
    return v.as<int>();
}

// Fans report a raw 0-15 step value (checked against ha-bambulab).
static int fan_pct(int raw) {
    if (raw < 0) return -1;
    int pct = (raw * 100 + 7) / 15;
    return pct > 100 ? 100 : pct;
}

static uint16_t hex_to_565(const char *hex) { // "RRGGBBAA" or "RRGGBB"
    if (!hex || strlen(hex) < 6) return 0;
    uint8_t c[3];
    char pair[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        pair[0] = hex[i * 2];
        pair[1] = hex[i * 2 + 1];
        c[i] = (uint8_t)strtol(pair, nullptr, 16);
    }
    return (uint16_t)(((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3));
}

static void apply_tray(BambuTray &tr, JsonObject t) {
    if (t.size() <= 1) { // only {"id":"N"} = empty slot
        tr.present = false;
        tr.type[0] = 0;
        tr.remain = -1;
        return;
    }
    if (t.containsKey("tray_type")) snprintf(tr.type, sizeof(tr.type), "%s", (const char *)(t["tray_type"] | ""));
    if (t.containsKey("tray_color")) tr.color565 = hex_to_565(t["tray_color"] | "");
    if (t.containsKey("remain")) {
        int r = json_int(t["remain"], -1);
        tr.remain = (r < 0 || r > 100) ? -1 : (int8_t)r;
    }
    tr.present = (tr.type[0] != 0);
}

static void on_mqtt_message(char *topic, byte *payload, unsigned int length) {
    (void)topic;
    build_filter();
    static DynamicJsonDocument doc(6144); // holds only the filtered subset, so this is generous
    doc.clear();
    DeserializationError err = deserializeJson(doc, payload, length, DeserializationOption::Filter(s_filter));
    if (err) {
        DEBUG_PRINTF("[bambu] JSON parse failed: %s (len %u)\n", err.c_str(), length);
        return;
    }

    JsonObject print = doc["print"];
    if (print.isNull()) return;

    if (print.containsKey("nozzle_temper")) s_state.nozzle_temp = print["nozzle_temper"].as<float>();
    if (print.containsKey("nozzle_target_temper")) s_state.nozzle_target = print["nozzle_target_temper"].as<float>();
    if (print.containsKey("bed_temper")) s_state.bed_temp = print["bed_temper"].as<float>();
    if (print.containsKey("bed_target_temper")) s_state.bed_target = print["bed_target_temper"].as<float>();
    if (print.containsKey("chamber_temper")) s_state.chamber_temp = print["chamber_temper"].as<float>();
    if (print.containsKey("mc_percent")) s_state.percent = json_int(print["mc_percent"], 0);
    if (print.containsKey("mc_remaining_time")) s_state.remaining_min = json_int(print["mc_remaining_time"], 0);
    if (print.containsKey("layer_num")) s_state.layer_num = json_int(print["layer_num"], 0);
    if (print.containsKey("total_layer_num")) s_state.total_layer_num = json_int(print["total_layer_num"], 0);
    if (print.containsKey("gcode_state")) {
        const char *gs = print["gcode_state"];
        snprintf(s_state.gcode_state, sizeof(s_state.gcode_state), "%s", gs ? gs : "");
    }
    if (print.containsKey("subtask_name")) {
        const char *fn = print["subtask_name"];
        snprintf(s_state.filename, sizeof(s_state.filename), "%s", fn ? fn : "");
    }
    if (print.containsKey("stg_cur")) s_state.stage = json_int(print["stg_cur"], -1);
    if (print.containsKey("print_error")) s_state.print_error = json_int(print["print_error"], 0);
    if (print.containsKey("hms")) s_state.hms_count = print["hms"].size();
    if (print.containsKey("spd_lvl")) s_state.speed_level = json_int(print["spd_lvl"], 0);
    if (print.containsKey("spd_mag")) s_state.speed_pct = json_int(print["spd_mag"], 0);
    if (print.containsKey("cooling_fan_speed")) s_state.fan_part = fan_pct(json_int(print["cooling_fan_speed"], -1));
    if (print.containsKey("big_fan1_speed")) s_state.fan_aux = fan_pct(json_int(print["big_fan1_speed"], -1));
    if (print.containsKey("big_fan2_speed")) s_state.fan_chamber = fan_pct(json_int(print["big_fan2_speed"], -1));
    if (print.containsKey("heatbreak_fan_speed")) s_state.fan_heatbreak = fan_pct(json_int(print["heatbreak_fan_speed"], -1));
    if (print.containsKey("wifi_signal")) s_state.wifi_dbm = json_int(print["wifi_signal"], 0);
    if (print.containsKey("nozzle_diameter")) {
        JsonVariant nd = print["nozzle_diameter"];
        if (nd.is<const char *>()) snprintf(s_state.nozzle_diameter, sizeof(s_state.nozzle_diameter), "%s", nd.as<const char *>());
        else snprintf(s_state.nozzle_diameter, sizeof(s_state.nozzle_diameter), "%.1f", nd.as<float>());
    }
    if (print.containsKey("lights_report")) {
        for (JsonObject l : print["lights_report"].as<JsonArray>()) {
            const char *node = l["node"];
            const char *mode = l["mode"];
            if (node && mode && strcmp(node, "chamber_light") == 0) {
                s_state.light_known = true;
                s_state.chamber_light = (strcmp(mode, "on") == 0);
            }
        }
    }

    JsonObject ams = print["ams"];
    if (!ams.isNull()) {
        if (ams.containsKey("tray_now")) s_state.active_tray = json_int(ams["tray_now"], 255);
        for (JsonObject u : ams["ams"].as<JsonArray>()) {
            int uid = json_int(u["id"], -1);
            if (uid < 0 || uid >= BAMBU_MAX_AMS) continue;
            if (uid + 1 > s_state.ams_units) s_state.ams_units = uid + 1;
            for (JsonObject t : u["tray"].as<JsonArray>()) {
                int tid = json_int(t["id"], -1);
                if (tid < 0 || tid > 3) continue;
                apply_tray(s_state.trays[uid * 4 + tid], t);
            }
        }
    }
    JsonObject vt = print["vt_tray"];
    if (!vt.isNull()) apply_tray(s_state.ext_spool, vt);

    s_state.last_msg_ms = millis();
    s_state.valid = true;
}

// P1-series printers only report what changed, so without this the
// static fields (total layers, filename, AMS trays...) may never arrive.
static void request_pushall() {
    char topic[48];
    snprintf(topic, sizeof(topic), "device/%s/request", s_printer_serial);
    static const char msg[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
    s_mqtt.publish(topic, msg);
}

static void try_mqtt_connect() {
    if (!s_printer_found || !s_has_access_code) return;
    char topic[48];
    snprintf(topic, sizeof(topic), "device/%s/report", s_printer_serial);

    s_mqtt.setServer(s_printer_ip, MQTT_PORT);
    s_mqtt.setCallback(on_mqtt_message);

    char client_id[24];
    snprintf(client_id, sizeof(client_id), "amoledwatch_%lu", (unsigned long)millis());
    if (s_mqtt.connect(client_id, "bblp", s_access_code)) {
        s_last_rc = 0;
        s_mqtt.subscribe(topic);
        s_next_pushall_ms = millis(); // ask for the full state right away
        DEBUG_PRINTF("[bambu] MQTT connected, subscribed to %s\n", topic);
    } else {
        s_last_rc = s_mqtt.state();
        DEBUG_PRINTF("[bambu] MQTT connect failed, rc=%d\n", s_last_rc);
    }
}

// ---- Public API -------------------------------------------------------

// Everything that touches the network stack (UDP socket, TLS/MQTT setup)
// is deferred until WiFi is actually connected. With WiFi off the TCP/IP
// stack has never been started, and using it then - even just s_udp.begin()
// - asserts inside FreeRTOS (xQueueSemaphoreTake on a lock that doesn't
// exist yet) and reboots the watch. So bambu_enable() only flips flags.
static bool s_net_started = false;

static void start_network() {
    // Full X1 reports are large; PubSubClient silently DROPS any message
    // bigger than its buffer, so ask for as much as memory allows.
    if (!s_mqtt.setBufferSize(16384) && !s_mqtt.setBufferSize(8192)) s_mqtt.setBufferSize(4096);
    // Bound each blocking connect - default is 15 s, which would freeze the
    // whole UI that long per attempt when the printer is unreachable.
    s_mqtt.setSocketTimeout(5);
    s_tls.setTimeout(5);
    s_tls.setInsecure(); // Bambu printers use a private/self-signed cert not practical to embed - accepted standard practice for this LAN-only connection, same approach every third-party Bambu integration uses
    s_udp.begin(SSDP_PORT);
    s_net_started = true;
    DEBUG_PRINTF("[bambu] network up, listening for printer broadcast on UDP %d\n", SSDP_PORT);
}

static void stop_network() {
    if (s_mqtt.connected()) s_mqtt.disconnect();
    s_udp.stop();
    s_net_started = false;
}

void bambu_enable() {
    // NOT an "only the first time" guard on the reset below: this is
    // called again every time the user (re-)enters an access code -
    // touch_enter_code()'s OK handler and bambu_printer_create() both
    // call it - and a stale s_printer_found/serial/model from an earlier
    // attempt must not be left standing in the way of a fresh one. Only
    // the one-time log line is actually gated on "was this already on".
    bool was_enabled = s_enabled;
    s_enabled = true;
    s_printer_found = false;
    s_printer_model[0] = 0;
    s_printer_model_display[0] = 0;
    s_printer_serial[0] = 0;
    reset_state();
    s_last_rc = 0;
    s_last_mqtt_attempt_ms = 0; // don't wait out the old retry timer before the first attempt with the new state
    if (!was_enabled) DEBUG_PRINTF("[bambu] enabled (network starts once WiFi is connected)\n");
}

void bambu_disable() {
    if (!s_enabled) return;
    s_enabled = false;
    if (s_net_started) stop_network();
    s_printer_found = false;
    reset_state();
    DEBUG_PRINTF("[bambu] disabled\n");
}

bool bambu_is_enabled() { return s_enabled; }

void bambu_tick() {
    if (!s_enabled) return;
    if (!wifi_is_connected()) {
        if (s_net_started) stop_network(); // WiFi dropped/off: release the socket, rebind on reconnect
        return;
    }
    if (!s_net_started) start_network();
    poll_ssdp();
    if (s_mqtt.connected()) {
        s_mqtt.loop();
        if ((int32_t)(millis() - s_next_pushall_ms) >= 0) {
            request_pushall();
            // Retry quickly until the first report lands, then only every
            // 5 minutes - the P1 lags if asked more often (per the protocol docs).
            s_next_pushall_ms = millis() + (s_state.valid ? 300000UL : 10000UL);
        }
    } else {
        if (s_printer_found && millis() - s_last_ssdp_ms > SSDP_EXPIRY_MS) {
            s_printer_found = false; // stopped announcing itself - treat as gone
            s_printer_serial[0] = 0;
        }
        if (s_printer_found && s_has_access_code && s_dev_mode_on &&
            millis() - s_last_mqtt_attempt_ms > MQTT_RECONNECT_INTERVAL_MS) {
            s_last_mqtt_attempt_ms = millis();
            try_mqtt_connect();
        }
    }
}

void bambu_set_access_code(const char *code) {
    snprintf(s_access_code, sizeof(s_access_code), "%s", code);
    s_has_access_code = (s_access_code[0] != '\0');
    nvs_save_bambu_code(code);
    // A changed code must take effect now, not after the next retry timer.
    if (s_mqtt.connected()) s_mqtt.disconnect();
    s_last_mqtt_attempt_ms = 0;
    s_last_rc = 0;
}

bool bambu_has_access_code() {
    if (s_has_access_code) return true;
    if (nvs_load_bambu_code(s_access_code, sizeof(s_access_code))) {
        s_has_access_code = (s_access_code[0] != '\0');
    }
    return s_has_access_code;
}

bool bambu_printer_found() { return s_printer_found; }
const char *bambu_printer_model() { return s_printer_model_display; }
const char *bambu_printer_ip_str() { return s_printer_ip_str; }
const char *bambu_printer_serial() { return s_printer_serial; }
bool bambu_printer_dev_mode_on() { return s_dev_mode_on; }
bool bambu_mqtt_connected() { return s_mqtt.connected(); }
int bambu_mqtt_last_error() { return s_last_rc; }

// ---- Controls ---------------------------------------------------------

static void mqtt_publish(const char *json) {
    if (!s_mqtt.connected()) return;
    char topic[48];
    snprintf(topic, sizeof(topic), "device/%s/request", s_printer_serial);
    s_mqtt.publish(topic, json);
}

void bambu_set_light(bool on) {
    char msg[220];
    snprintf(msg, sizeof(msg),
        "{\"system\":{\"sequence_id\":\"0\",\"command\":\"ledctrl\",\"led_node\":\"chamber_light\",\"led_mode\":\"%s\","
        "\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":1,\"interval_time\":1000}}",
        on ? "on" : "off");
    mqtt_publish(msg);
}

void bambu_set_speed(int level) {
    if (level < 1 || level > 4) return;
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"print\":{\"sequence_id\":\"0\",\"command\":\"print_speed\",\"param\":\"%d\"}}", level);
    mqtt_publish(msg);
}

void bambu_pause() { mqtt_publish("{\"print\":{\"sequence_id\":\"0\",\"command\":\"pause\",\"param\":\"\"}}"); }
void bambu_resume() { mqtt_publish("{\"print\":{\"sequence_id\":\"0\",\"command\":\"resume\",\"param\":\"\"}}"); }
void bambu_stop() { mqtt_publish("{\"print\":{\"sequence_id\":\"0\",\"command\":\"stop\",\"param\":\"\"}}"); }

void bambu_print_file(const char *path) {
    size_t len = strlen(path);
    bool is_3mf = (len > 4 && strcasecmp(path + len - 4, ".3mf") == 0);
    if (!is_3mf) return; // plain .gcode needs an absolute on-printer path this HAL doesn't know reliably - see hal_bambu.h
    char msg[360];
    // project_file's documented form: "url" is the SD-card-relative FTP
    // path to the .3mf itself, "param" is the plate's gcode inside it.
    // Multi-plate 3MFs are assumed to want plate 1 - there's no way to
    // know which plate without parsing the 3MF's own metadata, which
    // this HAL doesn't do, so a multi-plate file may print the wrong plate.
    snprintf(msg, sizeof(msg),
        "{\"print\":{\"sequence_id\":\"0\",\"command\":\"project_file\",\"param\":\"Metadata/plate_1.gcode\","
        "\"project_id\":\"0\",\"profile_id\":\"0\",\"task_id\":\"0\",\"subtask_id\":\"0\",\"subtask_name\":\"\","
        "\"url\":\"ftp:///%s\",\"md5\":\"\",\"timelapse\":false,\"bed_type\":\"auto\","
        "\"bed_levelling\":true,\"flow_cali\":true,\"vibration_cali\":true,\"layer_inspect\":true,\"use_ams\":false}}",
        path);
    mqtt_publish(msg);
}

// ---- Camera -------------------------------------------------------------
// TCP+TLS "Bambu Tunnel" protocol (P1/A1-family only - see
// model_code_to_display() for why X1/H2-family are excluded): connect,
// send an 80-byte auth packet, then read a continuous stream of
// (16-byte header, JPEG payload) frames. Header's first u32 (native
// little-endian, matching this LE MCU) is the JPEG length. Verified
// against two independent reference implementations.

static WiFiClientSecure s_cam_tls;
static bool s_cam_open = false;
static uint8_t s_cam_header[16];
static int s_cam_header_read = 0;
static uint32_t s_cam_frame_len = 0;
static int s_cam_frame_read = 0;
static bool s_cam_have_header = false;

bool bambu_camera_supported() { return s_camera_supported; }

void bambu_camera_open() {
    if (s_cam_open || !s_printer_found || !s_has_access_code) return;
    s_cam_tls.setInsecure();
    s_cam_tls.setTimeout(3);
    if (!s_cam_tls.connect(s_printer_ip, CAMERA_PORT)) {
        DEBUG_PRINTF("[bambu] camera connect failed\n");
        return;
    }
    uint8_t pkt[80];
    memset(pkt, 0, sizeof(pkt));
    uint32_t h0 = 0x40, h1 = 0x3000; // rest of the 16-byte header is zero
    memcpy(pkt + 0, &h0, 4);
    memcpy(pkt + 4, &h1, 4);
    memcpy(pkt + 16, "bblp", 4);
    memcpy(pkt + 48, s_access_code, strlen(s_access_code));
    s_cam_tls.write(pkt, sizeof(pkt));
    s_cam_open = true;
    s_cam_header_read = 0;
    s_cam_have_header = false;
    DEBUG_PRINTF("[bambu] camera stream opened\n");
}

void bambu_camera_close() {
    if (s_cam_open) s_cam_tls.stop();
    s_cam_open = false;
    s_cam_have_header = false;
}

bool bambu_camera_is_open() { return s_cam_open; }

bool bambu_camera_poll_frame(uint8_t *buf, size_t buf_cap, size_t *out_len) {
    if (!s_cam_open) return false;
    if (!s_cam_tls.connected()) { bambu_camera_close(); return false; }

    if (!s_cam_have_header) {
        if (s_cam_tls.available() <= 0) return false;
        int got = s_cam_tls.read(s_cam_header + s_cam_header_read, 16 - s_cam_header_read);
        if (got <= 0) return false;
        s_cam_header_read += got;
        if (s_cam_header_read < 16) return false;
        uint32_t len;
        memcpy(&len, s_cam_header, 4);
        s_cam_header_read = 0;
        if (len == 0 || len > buf_cap) {
            // Bogus or too-large frame for our buffer - drop this header
            // and wait for the next one rather than trying to skip a
            // length we can't buffer, which risks never resyncing.
            DEBUG_PRINTF("[bambu] camera frame len %lu out of range, resyncing\n", (unsigned long)len);
            return false;
        }
        s_cam_frame_len = len;
        s_cam_frame_read = 0;
        s_cam_have_header = true;
    }

    if (s_cam_tls.available() <= 0) return false;
    int got = s_cam_tls.read(buf + s_cam_frame_read, s_cam_frame_len - s_cam_frame_read);
    if (got <= 0) return false;
    s_cam_frame_read += got;
    if ((uint32_t)s_cam_frame_read < s_cam_frame_len) return false;

    *out_len = s_cam_frame_len;
    s_cam_have_header = false;
    return true;
}

// ---- File listing (FTPS) -------------------------------------------------
// Port 990, implicit TLS on both the control connection and the PASV data
// connection - standard FTP (RFC 959) plus RFC 4217-style implicit TLS,
// not a Bambu-specific protocol. Blocking; only called when the file
// browser view opens, never from tick().

static bool ftp_read_line(WiFiClientSecure &c, char *buf, size_t buf_cap, uint32_t timeout_ms) {
    size_t len = 0;
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        if (c.available() > 0) {
            int ch = c.read();
            if (ch < 0) continue;
            if (ch == '\n') { buf[len] = 0; return true; }
            if (ch != '\r' && len < buf_cap - 1) buf[len++] = (char)ch;
        } else if (!c.connected()) {
            break;
        }
    }
    buf[len] = 0;
    return len > 0;
}

// Reads one control-channel reply, following FTP's multi-line convention
// ("150-more coming" continues until a line starting "150 " with a space).
static int ftp_read_reply(WiFiClientSecure &c, char *buf, size_t buf_cap, uint32_t timeout_ms = 5000) {
    char line[160];
    int code = 0;
    do {
        if (!ftp_read_line(c, line, sizeof(line), timeout_ms)) return 0;
        if (strlen(line) >= 4 && isdigit((unsigned char)line[0])) code = atoi(line);
        if (buf) snprintf(buf, buf_cap, "%s", line);
    } while (strlen(line) >= 4 && line[3] == '-'); // "150-" = more lines follow; "150 " = final line
    return code;
}

static bool ftp_send(WiFiClientSecure &c, const char *cmd) {
    return c.print(cmd) > 0 && c.print("\r\n") > 0;
}

int bambu_ftp_list(const char *dir, BambuFileEntry *out, int max_entries) {
    if (!s_printer_found || !s_has_access_code || !wifi_is_connected()) return -1;

    WiFiClientSecure ctrl;
    ctrl.setInsecure();
    ctrl.setTimeout(5);
    if (!ctrl.connect(s_printer_ip, FTP_PORT)) {
        DEBUG_PRINTF("[bambu] ftp control connect failed\n");
        return -1;
    }
    char reply[160];
    if (ftp_read_reply(ctrl, reply, sizeof(reply)) != 220) { ctrl.stop(); return -1; }

    char cmd[96];
    ftp_send(ctrl, "USER bblp");
    if (ftp_read_reply(ctrl, reply, sizeof(reply)) != 331) { ctrl.stop(); return -1; }
    snprintf(cmd, sizeof(cmd), "PASS %s", s_access_code);
    ftp_send(ctrl, cmd);
    if (ftp_read_reply(ctrl, reply, sizeof(reply)) != 230) { ctrl.stop(); return -1; } // wrong/stale access code

    ftp_send(ctrl, "TYPE A"); // ASCII for the listing itself
    ftp_read_reply(ctrl, reply, sizeof(reply));

    ftp_send(ctrl, "PASV");
    if (ftp_read_reply(ctrl, reply, sizeof(reply)) != 227) { ctrl.stop(); return -1; }
    // "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)."
    int h1, h2, h3, h4, p1, p2;
    const char *paren = strchr(reply, '(');
    if (!paren || sscanf(paren, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        ctrl.stop();
        return -1;
    }
    IPAddress data_ip(h1, h2, h3, h4);
    uint16_t data_port = (uint16_t)(p1 * 256 + p2);

    WiFiClientSecure data;
    data.setInsecure();
    data.setTimeout(5);
    if (!data.connect(data_ip, data_port)) {
        DEBUG_PRINTF("[bambu] ftp data connect failed\n");
        ctrl.stop();
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "LIST %s", (dir && dir[0]) ? dir : "");
    ftp_send(ctrl, cmd);
    int listing_started = ftp_read_reply(ctrl, reply, sizeof(reply));
    if (listing_started != 150 && listing_started != 125) {
        data.stop();
        ctrl.stop();
        return -1;
    }

    int count = 0;
    char line[192];
    while (count < max_entries && ftp_read_line(data, line, sizeof(line), 4000)) {
        if (line[0] == 0) continue;
        // Standard unix `ls -l` layout: perms links owner group size mon day time-or-year name...
        // Filenames can contain spaces, so only the first 8 fields are
        // tokenized; everything after them, trimmed, is the filename.
        char tmp[192];
        snprintf(tmp, sizeof(tmp), "%s", line);
        char *tok = strtok(tmp, " \t");
        int field = 0;
        uint32_t size = 0;
        bool is_dir = (line[0] == 'd');
        while (tok && field < 8) {
            if (field == 4) size = (uint32_t)atol(tok);
            tok = strtok(nullptr, " \t");
            field++;
        }
        if (!tok || field < 8) continue; // malformed line - skip rather than guess
        // tok points into tmp, which strtok already NUL-terminated at its
        // first internal space - if the filename itself contains a space
        // that would silently truncate it. Recover the full filename from
        // the ORIGINAL, untouched line at the same byte offset instead.
        size_t offset = tok - tmp;
        const char *trimmed = line + offset;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (!trimmed[0] || strcmp(trimmed, ".") == 0 || strcmp(trimmed, "..") == 0) continue;
        snprintf(out[count].name, sizeof(out[count].name), "%s", trimmed);
        out[count].is_dir = is_dir;
        out[count].size = size;
        count++;
    }

    data.stop();
    ftp_read_reply(ctrl, reply, sizeof(reply)); // "226 Transfer complete"
    ftp_send(ctrl, "QUIT");
    ctrl.stop();
    return count;
}

const BambuPrintState &bambu_get_state() { return s_state; }

#else

void bambu_enable() {}
void bambu_disable() {}
bool bambu_is_enabled() { return false; }
void bambu_tick() {}
void bambu_set_access_code(const char *) {}
bool bambu_has_access_code() { return false; }
bool bambu_printer_found() { return false; }
const char *bambu_printer_model() { return ""; }
const char *bambu_printer_ip_str() { return ""; }
const char *bambu_printer_serial() { return ""; }
bool bambu_printer_dev_mode_on() { return false; }
bool bambu_mqtt_connected() { return false; }
int bambu_mqtt_last_error() { return 0; }
void bambu_set_light(bool) {}
void bambu_set_speed(int) {}
void bambu_pause() {}
void bambu_resume() {}
void bambu_stop() {}
void bambu_print_file(const char *) {}
bool bambu_camera_supported() { return false; }
void bambu_camera_open() {}
void bambu_camera_close() {}
bool bambu_camera_is_open() { return false; }
bool bambu_camera_poll_frame(uint8_t *, size_t, size_t *) { return false; }
int bambu_ftp_list(const char *, BambuFileEntry *, int) { return -1; }
static BambuPrintState s_empty_state;
const BambuPrintState &bambu_get_state() { return s_empty_state; }

#endif

const char *bambu_stage_text(int stage) {
    switch (stage) {
        case 1: return "Auto bed leveling";
        case 2: return "Heating bed";
        case 3: return "Vibration compensation";
        case 4: return "Changing filament";
        case 5: return "Paused";
        case 6: return "Paused: filament ran out";
        case 7: return "Heating nozzle";
        case 8: return "Calibrating extrusion";
        case 9: return "Scanning bed surface";
        case 10: return "Inspecting first layer";
        case 11: return "Identifying build plate";
        case 12: return "Calibrating lidar";
        case 13: return "Homing toolhead";
        case 14: return "Cleaning nozzle tip";
        case 15: return "Checking nozzle temp";
        case 16: return "Paused by user";
        case 17: return "Paused: front cover";
        case 18: return "Calibrating lidar";
        case 19: return "Calibrating flow";
        case 20: return "Paused: nozzle temp error";
        case 21: return "Paused: bed temp error";
        case 22: return "Unloading filament";
        case 23: return "Paused: step skipped";
        case 24: return "Loading filament";
        case 25: return "Calibrating motor noise";
        case 26: return "Paused: AMS lost";
        case 27: return "Paused: heatbreak fan";
        case 28: return "Paused: chamber temp error";
        case 29: return "Cooling chamber";
        case 30: return "Paused: custom gcode";
        case 31: return "Motor noise test";
        case 32: return "Paused: nozzle covered";
        case 33: return "Paused: cutter error";
        case 34: return "Paused: first layer error";
        case 35: return "Paused: nozzle clog";
        default: return "";
    }
}
