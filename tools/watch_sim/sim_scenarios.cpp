// The screenshots: each scenario sets up the watch, runs the UI for a
// moment and saves the frame on screen.
#include <Arduino.h>
#include "config.h"
#include "ui.h"
#include "apps.h"
#include "watchface_registry.h"
#include "hal_fwupdate.h"
#include "sim_state.h"
#include "diag.h"
#include "anim_lock.h"
#include "app_charging_anim.h"
#include "hal_sleep.h"
#include "app_settings_state.h"

void sim_run(uint32_t ms);
void sim_save(const char *name);
void sim_tap(int x, int y);
void sim_link(const char *json);

extern Screen menu_screen, watchface_minimal_screen, nowplaying_screen, calendar_screen, fwup_screen,
    flappy_screen, fruitninja_screen, battery_screen, recorder_screen;

// Local epoch seconds of today (2026-10-08) at hh:mm, like calendar_now().
static uint32_t at(int hh, int mm) { return 20734u * 86400u + hh * 3600u + mm * 60u; }

static void phone_data() {
    char b[1024];
    sim_link("{\"t\":\"hi\"}");
    sim_link("{\"t\":\"bat\",\"l\":64,\"c\":0}");
    snprintf(b, sizeof(b),
             "{\"t\":\"cal\",\"r\":1,\"rm\":10,\"done\":1,\"n\":3,\"ev\":["
             "{\"id\":1,\"s\":%u,\"e\":%u,\"c\":4359668,\"ti\":\"Design review\",\"lo\":\"Room 4B\"},"
             "{\"id\":2,\"s\":%u,\"e\":%u,\"c\":16750592,\"ti\":\"Lunch with Sara\",\"lo\":\"Trattoria Milano\"},"
             "{\"id\":3,\"s\":%u,\"e\":%u,\"c\":3066993,\"ti\":\"Gym\",\"lo\":\"\"}]}",
             at(10, 45), at(11, 30), at(13, 0), at(14, 0), at(18, 30), at(19, 30));
    sim_link(b);
    sim_link("{\"t\":\"med\",\"ti\":\"Midnight City\",\"ar\":\"M83\",\"pl\":1,\"po\":96,\"du\":243,\"v\":9,\"vm\":15}");
    sim_link("{\"t\":\"ntf\",\"id\":11,\"ap\":\"Calendar\",\"ti\":\"Design review in 35 min\",\"tx\":\"Room 4B\",\"rp\":0}");
    sim_link("{\"t\":\"ntf\",\"id\":12,\"ap\":\"Gmail\",\"ti\":\"Your order has shipped\",\"tx\":\"Arriving Friday - track your package\",\"rp\":0}");
    sim_link("{\"t\":\"ntf\",\"id\":13,\"ap\":\"WhatsApp\",\"ti\":\"Sara\",\"tx\":\"Still on for lunch at 1? I booked a table\",\"rp\":1}");
    sim_run(4000);
}

// Saves `ms` of animation as numbered frames (one every `step` ms) - to_png.py
// turns <name>_NNN frames into <name>.gif.
static void record(const char *name, uint32_t ms, uint32_t step) {
    char n[64];
    for (uint32_t t = 0, i = 0; t <= ms; t += step, i++) {
        sim_run(step);
        snprintf(n, sizeof(n), "%s_%03u", name, (unsigned)i);
        sim_save(n);
    }
}

static void home() {
    ui_go_home();
    sim_run(1200);
}

static void shot(Screen *s, const char *name, uint32_t ms = 1200) {
    home();
    ui_push(s);
    sim_run(ms);
    sim_save(name);
}

void sim_scenarios() {
    ui_push(watchface_get(0)->screen);
    sim_run(500);
    phone_data();

    home();
    sim_run(6000);   // let the notification pop-ups go by
    sim_save("watchface");

    shot(&watchface_minimal_screen, "watchface_minimal");

    // Always-on clock: what the panel shows while the watch sleeps.
    g_app_settings.aod_on = true;
    sleep_force_sleep();
    sim_save("aod");
    sleep_register_activity();
    g_app_settings.aod_on = false;
    shot(&menu_screen, "menu");
    shot(&notifications_screen, "notifications");
    shot(&nowplaying_screen, "music");
    shot(&calendar_screen, "calendar");
    shot(&settings_screen, "settings");
    g_sim.fwup = FWUP_AVAILABLE;
    shot(&fwup_screen, "software_update");
    g_sim.fwup = FWUP_IDLE;
    shot(&battery_screen, "battery");
    shot(&recorder_screen, "recorder");

    // Games (their title screens - sprites and levels come from the SD card)
    shot(&flappy_screen, "game_flappy");
    shot(&fruitninja_screen, "game_fruitninja");

    home();
    ui_top_panel_open();
    sim_run(1500);
    sim_save("quick_panel");
    ui_top_panel_close();
    sim_run(800);

    // Turn-by-turn from the phone's navigation app
    sim_link("{\"t\":\"nav\",\"st\":1,\"i\":\"Turn right onto Via Dante\",\"d\":\"250 m\",\"x\":\"12 min - 1.4 km\",\"h\":0}");
    sim_run(1500);
    sim_save("navigation");
    sim_link("{\"t\":\"nav\",\"st\":0}");
    home();

    // Incoming call, answered or declined on the wrist
    sim_link("{\"t\":\"call\",\"st\":\"ring\",\"id\":1,\"n\":\"Sara Rossi\"}");
    sim_run(1500);
    sim_save("call");

    // Animations (GIFs)
    sim_link("{\"t\":\"call\",\"st\":\"end\",\"id\":1}");
    home();
    sim_run(500);
    lock_anim_set_boot();
    ui_push(&lock_anim_screen);
    record("anim_boot", 2160, 40);
    sim_run(300);

    home();
    sim_run(300);
    lock_anim_set_mode(false, false);
    ui_push(&lock_anim_screen);
    record("anim_lock", 240, 20);
    sim_run(300);

    lock_anim_set_mode(true, false);
    ui_push(&lock_anim_screen);
    record("anim_unlock", 260, 20);
    sim_run(300);

    home();
    ui_push(&nowplaying_screen);
    sim_run(1200);
    record("anim_music", 2000, 50);

    home();
    sim_run(300);
    g_sim.charging = true;
    ui_push(&charging_anim_screen);
    record("anim_charging", 1040, 40);
    g_sim.charging = false;
    sim_run(300);

    // An automatic check on Wi-Fi found a release
    home();
    ui_show_confirm("Update available", "Firmware 3.5.0 is out (you have " FW_VERSION "). Download and install it now?",
                    "Update", "Later", [](bool) {});
    sim_run(800);
    sim_save("update_prompt");
}
