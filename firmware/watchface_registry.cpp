#include "watchface_registry.h"
#include "config.h"
#include "app_watchface.h"
#include "app_custom_watchface.h"
#include "ui.h"

#if FEATURE_NVS
#include "hal_nvs.h"
#endif

// Forward declarations
extern Screen watchface_minimal_screen;

static const WatchfaceEntry s_registry[] = {
    { "Eyes",     &watchface_screen,         app_watchface_tick },
    { "Minimal",  &watchface_minimal_screen, nullptr },
    { "Custom",   &custom_watchface_screen,  nullptr },
};
static const int s_count = sizeof(s_registry) / sizeof(s_registry[0]);

static int s_current = 0;

int watchface_count() { return s_count; }

const WatchfaceEntry *watchface_get(int index) {
    if (index < 0 || index >= s_count) return nullptr;
    return &s_registry[index];
}

int watchface_current() { return s_current; }

void watchface_set_current(int index) {
    if (index < 0 || index >= s_count) return;
    s_current = index;
#if FEATURE_NVS
    nvs_save_watchface_index(index);
#endif
}

void watchface_registry_init() {
#if FEATURE_NVS
    uint8_t saved = nvs_load_watchface_index();
    if (saved < s_count) s_current = saved; // saved is unsigned, always >= 0
#endif
}

void watchface_cycle_next() {
    int next = (s_current + 1) % s_count;
    watchface_set_current(next);
    const WatchfaceEntry *wf = watchface_get(s_current);
    if (wf && wf->screen) {
        ui_go_home(); // pop back to root, then push new watchface
        ui_push(wf->screen);
    }
}
