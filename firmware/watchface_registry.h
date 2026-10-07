/*
 * watchface_registry.h
 * Simple registry of available watchfaces.
 */

#pragma once
#include "ui.h"

struct WatchfaceEntry {
    const char *name;
    Screen *screen;
    void (*tick)();  // called every second, may be nullptr
};

int watchface_count();
const WatchfaceEntry *watchface_get(int index);
int watchface_current();
void watchface_set_current(int index);
void watchface_cycle_next();
void watchface_registry_init();
