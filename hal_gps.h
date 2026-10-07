/*
 * hal_gps.h
 * LC76G GNSS module (onboard, -G variant only) over UART, NMEA parsed
 * with TinyGPS++. Only compiled in if FEATURE_GPS is set in config.h.
 */

#pragma once
#include <stdint.h>

struct GpsFix {
    bool has_fix;
    double latitude, longitude;
    float altitude_m;
    float speed_kmh;
    int satellites;
    float hdop;
    int hour, minute, second; // UTC, from GNSS
};

void gps_init();
void gps_deinit(); // for runtime enable/disable - see gps_enable()/gps_disable()
// Call frequently from loop() - feeds NMEA bytes into the parser.
void gps_poll();
GpsFix gps_get_fix();

// Runtime on/off, mirroring hal_wifi.h's wifi_enable()/wifi_disable()
// pattern. Confirmed via the board's actual schematic: the LC76G
// module's RESET pin (pin 9) nets to GPS_RST, which ties directly to
// EXIO7 on the TCA9554 I/O expander (see EXPANDER_PIN_GPS_RST in
// board_pins.h). gps_disable() holds the module in reset - a real
// reduction in the module's own draw, not just silencing the UART on
// the ESP32 side. GPS's VCC/VCC_RF share the board's main 3.3V rail
// with everything else (not an independently switchable PMU rail per
// the schematic), so reset is the actual lever available here, not a
// full power-rail cutoff.
void gps_enable();
void gps_disable();
bool gps_is_enabled();

// ---- Is a GPS module fitted at all? ------------------------------------------
// The LC76G is only on the board's "-G" variant. gps_probe_begin() (setup)
// releases it from reset and listens for valid NMEA sentences for a few
// seconds, without blocking; gps_probe_update() (loop) decides. Until then
// the presence is GPS_UNKNOWN. When it's GPS_ABSENT, gps_enable() does
// nothing and Settings / the top panel show GPS as unavailable.
enum GpsPresence : uint8_t { GPS_UNKNOWN, GPS_PRESENT, GPS_ABSENT };
void gps_probe_begin();
void gps_probe_update();
GpsPresence gps_presence();
inline bool gps_available() { return gps_presence() != GPS_ABSENT; }
