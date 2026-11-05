/*
 * low_power.h
 * Low Power Mode, like watchOS's: one switch that stretches the battery -
 * the Saver battery mode (dimmer, shorter screen-on, no Wi-Fi/GPS, no
 * raise-to-wake), and the always-on display, the weather animation and
 * the wake word off. Turning it off puts all of that back as it was.
 * The battery readout turns yellow while it's on.
 *
 * At 10% (not charging) the watch offers it once per discharge; charging
 * past 80% turns it off again.
 */
#pragma once

void low_power_set(bool on);
bool low_power_on();
void low_power_update();   // loop(): the 10% offer, off again at 80% charging
