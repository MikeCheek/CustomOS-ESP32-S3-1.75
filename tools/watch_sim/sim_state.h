#pragma once
#include <stdint.h>
// What the stubbed hardware reports - set per scenario.
struct SimState {
    int hour = 10, minute = 9, second = 30;
    int battery = 82;
    bool charging = false;
    bool phone = true;     // Bluetooth link to the companion app
    bool wifi = false;
    uint32_t steps = 6482;
    uint8_t fwup = 0;      // FwupState shown by the Software update screen
};
extern SimState g_sim;
