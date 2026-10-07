#include "hal_rtc.h"
#include "board_pins.h"
#include "config.h"

// SensorLib - install via Library Manager ("SensorLib" by lewisxhe) or
// from the Waveshare demo package.
#include <SensorPCF85063.hpp>

static SensorPCF85063 s_rtc;
static bool s_ok = false;

bool rtc_init() {
    // Drop the I2C address, the library already knows it
    s_ok = s_rtc.begin(Wire, PIN_IIC_SDA, PIN_IIC_SCL);
    if (!s_ok) {
        DEBUG_PRINTF("[rtc] PCF85063 init FAILED at 0x%02X\n", RTC_I2C_ADDR);
        return false;
    }

    // If the RTC has never been set (e.g. first boot, dead backup cell),
    // seed it with a sane default so the UI doesn't show garbage. Replace
    // with real time sync (NTP over Wi-Fi, or the companion app) once
    // you wire that up.
    RTC_DateTime rtc_time = s_rtc.getDateTime();
    struct tm t = {0};
    t.tm_year = rtc_time.getYear() - 1900; 
    t.tm_mon  = rtc_time.getMonth() - 1;   
    t.tm_mday = rtc_time.getDay();
    t.tm_hour = rtc_time.getHour();
    t.tm_min  = rtc_time.getMinute();
    t.tm_sec  = rtc_time.getSecond();
    if (t.tm_year < (2024 - 1900)) {
        DEBUG_PRINTF("[rtc] time looks unset, seeding default\n");
        rtc_set(2026, 1, 1, 0, 0, 0);
    }

    DEBUG_PRINTF("[rtc] PCF85063 init OK\n");
    return true;
}

WatchTime rtc_now() {
    WatchTime wt{};
    if (!s_ok) { wt.valid = false; return wt; }

    RTC_DateTime rtc_time = s_rtc.getDateTime();
    wt.valid = true;
    // NOT "+ 1900" / "+ 1" - see rtc_set() below and the reasoning
    // there. getDateTime()/setDateTime() share the same RTC_DateTime
    // type (SensorPCF85063.hpp declares both via
    // "using SensorRTC::setDateTime; using SensorRTC::getDateTime;"),
    // and multiple official examples (including Waveshare's own wiki
    // for a closely related board) call setDateTime() with a full
    // conventional year (e.g. 2025) and a conventional 1-12 month, not
    // a "years since 1900" or 0-indexed value - a getter/setter pair
    // on the same object would be expected to agree on units. Adding
    // 1900 on top of an already-full year would silently produce
    // something like year 3924; adding 1 to an already-1-indexed month
    // would overflow past 12 for anything set in December. This was
    // the actual bug behind "time syncs but date doesn't" - time
    // fields (hour/minute/second) have no such convention mismatch to
    // go wrong, only year/month did.
    //
    // Honesty note: I could not find the getter's own source directly
    // (SensorLib's file layout has moved more than once and the exact
    // current path wasn't fetchable) - this fix rests on the setter's
    // confirmed convention plus the strong assumption that the getter
    // is symmetric with it, not on having read the getter's
    // implementation line for line. If dates are still wrong after
    // this, that symmetry assumption would be the next thing to
    // question.
    wt.year = rtc_time.getYear();
    wt.month = rtc_time.getMonth();
    wt.day = rtc_time.getDay();
    wt.hour = rtc_time.getHour();
    wt.minute = rtc_time.getMinute();
    wt.second = rtc_time.getSecond();
    wt.weekday = rtc_time.getWeek();
    return wt;
}

void rtc_set(int year, int month, int day, int hour, int minute, int second) {
    if (!s_ok) return;
    s_rtc.setDateTime(year, month, day, hour, minute, second);
}
