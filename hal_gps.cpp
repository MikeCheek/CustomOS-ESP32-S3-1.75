#include "hal_gps.h"
#include "board_pins.h"
#include "config.h"
#include "hal_expander.h"
#include "app_settings_state.h"
#include "diag.h"
#include <Preferences.h>

#if FEATURE_GPS

// TinyGPS++ - install via Library Manager ("TinyGPSPlus" by Mikal Hart).
// This is a generic, well-tested NMEA parser and works with any GNSS
// module (including the LC76G) since it just parses standard sentences.
#include <TinyGPS++.h>
#include <HardwareSerial.h>

static TinyGPSPlus s_gps;
static HardwareSerial s_gpsSerial(GPS_UART_NUM);

static bool s_gps_enabled = false;

void gps_init() {
    // Release the module from reset (confirmed via the board's
    // schematic - see EXPANDER_PIN_GPS_RST in board_pins.h) before
    // starting the UART. This is the real power-control lever this
    // project was missing - previously gps_init() only started the
    // UART, which stopped the ESP32 talking to the module but did
    // nothing to reduce the module's own draw while it sat there
    // still actively searching regardless of whether anything was
    // listening.
    expander_set_pin(EXPANDER_PIN_GPS_RST, true);
    delay(100); // let the module come up before the UART starts reading it - a reasonable settle time, not a datasheet-verified minimum
    s_gpsSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    s_gps_enabled = true;
    DEBUG_PRINTF("[gps] LC76G released from reset, UART started at %d baud (rx=%d tx=%d)\n",
                 GPS_BAUD, PIN_GPS_RX, PIN_GPS_TX);
}

void gps_deinit() {
    s_gpsSerial.end();
    // Hold the module in reset - the actual power-saving step, not
    // just silencing the UART.
    expander_set_pin(EXPANDER_PIN_GPS_RST, false);
    s_gps_enabled = false;
    DEBUG_PRINTF("[gps] UART stopped, module held in reset\n");
}

static GpsPresence s_presence = GPS_UNKNOWN;
static bool     s_probing = false;
static uint32_t s_probe_t0 = 0;
static const uint32_t GPS_PROBE_MS = 3000;   // LC76G sends its first NMEA well within 1 s of reset

void gps_enable() { if (!s_gps_enabled && s_presence != GPS_ABSENT) gps_init(); }
void gps_disable() { if (s_gps_enabled) gps_deinit(); }
bool gps_is_enabled() { return s_gps_enabled; }

void gps_poll() {
    if (!s_gps_enabled) return;
    while (s_gpsSerial.available() > 0) {
        s_gps.encode(s_gpsSerial.read());
    }
}

// "No module on this board" is remembered, so boards without GPS (the
// non -G variant) don't power up the GPS reset line and UART on every boot.
// Re-checked every 10th boot in case the first probe was a false negative.
static bool stored_absent() {
    Preferences p;
    if (!p.begin("gps", true)) return false;
    bool a = p.getBool("absent", false);
    p.end();
    return a;
}

static void store_absent(bool absent) {
    Preferences p;
    if (!p.begin("gps", false)) return;
    if (p.getBool("absent", false) != absent) p.putBool("absent", absent);
    p.end();
}

void gps_probe_begin() {
    if (!g_app_settings.gps_on && stored_absent() && diag_boot_count() % 10 != 0) {
        s_presence = GPS_ABSENT;
        DEBUG_PRINTF("[gps] no module on this board (remembered) - not probing\n");
        return;
    }
    s_presence = GPS_UNKNOWN;
    if (!s_gps_enabled) gps_init();
    s_probing = true;
    s_probe_t0 = millis();
}

void gps_probe_update() {
    if (!s_probing) return;
    gps_poll();
    // A sentence with a good checksum can only come from a real module -
    // a floating RX pin produces noise, never valid NMEA.
    if (s_gps.passedChecksum() > 0) {
        s_presence = GPS_PRESENT;
        s_probing = false;
        DEBUG_PRINTF("[gps] module detected (%lu NMEA sentences)\n", (unsigned long)s_gps.passedChecksum());
        store_absent(false);
        if (!g_app_settings.gps_on) gps_deinit();      // was only on for the probe
    } else if (millis() - s_probe_t0 > GPS_PROBE_MS) {
        s_presence = GPS_ABSENT;
        s_probing = false;
        DEBUG_PRINTF("[gps] no module answered (%lu chars, 0 valid sentences) - GPS disabled\n",
                     (unsigned long)s_gps.charsProcessed());
        gps_deinit();
        store_absent(true);
    }
}

GpsPresence gps_presence() { return s_presence; }

GpsFix gps_get_fix() {
    GpsFix fix{};
    fix.has_fix = s_gps.location.isValid() && s_gps.location.age() < 5000;
    if (fix.has_fix) {
        fix.latitude = s_gps.location.lat();
        fix.longitude = s_gps.location.lng();
    }
    fix.altitude_m = s_gps.altitude.isValid() ? s_gps.altitude.meters() : 0;
    fix.speed_kmh = s_gps.speed.isValid() ? s_gps.speed.kmph() : 0;
    fix.satellites = s_gps.satellites.isValid() ? s_gps.satellites.value() : 0;
    fix.hdop = s_gps.hdop.isValid() ? s_gps.hdop.hdop() : 99.9f;
    if (s_gps.time.isValid()) {
        fix.hour = s_gps.time.hour();
        fix.minute = s_gps.time.minute();
        fix.second = s_gps.time.second();
    }
    return fix;
}

#else // FEATURE_GPS disabled

void gps_init() {}
void gps_deinit() {}
void gps_probe_begin() {}
void gps_probe_update() {}
GpsPresence gps_presence() { return GPS_ABSENT; }
void gps_enable() {}
void gps_disable() {}
bool gps_is_enabled() { return false; }
void gps_poll() {}
GpsFix gps_get_fix() { return GpsFix{}; }

#endif
