// The screenshots: each scenario sets up the watch, runs the UI for a
// moment and saves the frame on screen.
#include <Arduino.h>
#include "config.h"
#include "ui.h"
#include "app_face_nav.h"
extern Screen settings_body_screen;
#define settings_body_screen_ref settings_body_screen
extern Screen contacts_screen;
#define contacts_screen_ref contacts_screen
extern Screen phone_screen;
#define phone_screen_ref phone_screen
extern Screen gallery_screen;
#define gallery_screen_ref gallery_screen
extern Screen media_screen;
#define media_screen_ref media_screen
extern Screen pet_screen;
#define pet_screen_ref pet_screen
extern Screen diagnostics_screen;
#define diagnostics_screen_ref diagnostics_screen
extern Screen snake_screen;
#define snake_screen_ref snake_screen
extern Screen breakout_screen;
#define breakout_screen_ref breakout_screen
#include "app_life.h"
#include "app_tips.h"
extern Screen life_meal_screen;
static Screen &life_meal_screen_ref() { return life_meal_screen; }
#include "app_menu.h"
#include "app_sos.h"
#include "app_poweroff.h"
#include "app_activity.h"
#include "fitness.h"
#include "app_workout.h"
#include "app_reminders.h"
#include "app_weather.h"
#include "app_clock.h"
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
    flappy_screen, fruitninja_screen, battery_screen, recorder_screen, piano_screen, assistant_screen,
    fall_alert_screen, sleep_screen, draw_screen, activity_screen;
void sleep_load_demo();
void assistant_on_result(uint32_t, const char *, const char *, const char *, int32_t, const char *);

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
    sim_link("{\"t\":\"tz\",\"o\":120}");
    sim_link("{\"t\":\"wxh\",\"pl\":\"Milan\",\"hi\":21,\"lo\":12,\"h\":[[10,19,61],[11,19,61],[12,20,3],[13,21,2],"
             "[14,21,2],[15,21,1],[16,20,0],[17,19,0],[18,17,0],[19,16,0],[20,15,0],[21,14,0]]}");
    sim_link("{\"t\":\"todos\",\"r\":1,\"done\":1,\"l\":[{\"k\":\"a1\",\"x\":\"Call Marco about the flat\"},"
             "{\"k\":\"a2\",\"x\":\"Send the slides to Giulia before Friday's review\"},"
             "{\"k\":\"a3\",\"x\":\"Buy coffee\"},{\"k\":\"a4\",\"x\":\"Book the dentist\"}]}");
    // a morning's movement for the rings
    for (int i = 0; i < 330; i++) fitness_feed(ACT_WALK, 108);
    for (int i = 0; i < 300; i++) fitness_feed(ACT_MOVING, 0);
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
    menu_open_folder(0);                    // Games
    sim_run(800);
    sim_save("menu_folder");
    home();
    shot(&menu_screen, "menu");
    sim_tap(233, 233);                      // the middle app: zooms open
    sim_run(90);
    sim_save("menu_launch");
    sim_run(600);                            // the app is open
    ui_pop_screen();                         // BOOT: back into its icon
    sim_run(110);
    sim_save("menu_return");
    g_app_settings.menu_list = true;
    shot(&menu_screen, "menu_list");
    g_app_settings.menu_list = false;
    shot(&activity_screen, "activity_rings");
    shot(&rings_celebrate_screen, "rings_closed", 1600);
    shot(&weather_screen, "weather");
    shot(&reminders_screen, "reminders");
    shot(&worldclock_screen, "world_clock");
    shot(&alarms_screen, "alarms_empty");
    sim_tap(233, 96 + 44);                  // + Add alarm
    sim_run(500);
    sim_save("alarm_edit");
    sim_tap(233 + 90, 360 + 28);            // Save (a new one: the middle button)
    sim_tap(233, 388);
    sim_run(600);
    sim_save("alarms");
    shot(&timer_screen, "timer");
    sim_tap(233, 146);                      // 3 min
    sim_run(20000);
    sim_save("timer_running");
    shot(&stopwatch_screen, "stopwatch_idle");
    sim_tap(233 + 80, 346);                 // Start
    sim_run(12340);
    sim_tap(233 - 80, 346);                 // Lap
    sim_run(9870);
    sim_tap(233 - 80, 346);
    sim_run(4210);
    sim_save("stopwatch");
    shot(&workout_screen, "workout_types");
    sim_tap(233, 92 + 92 + 46);             // Outdoor Run
    sim_run(1500);
    sim_save("workout_countdown");
    sim_run(2500);
    sim_run(60000);
    sim_save("workout_live");
    // Life: a plan from the app, a few things done
    sim_link("{\"t\":\"life\",\"w\":{\"on\":1,\"g\":8,\"ev\":90},\"s\":{\"on\":1,\"g\":3},\"tr\":{\"on\":1,\"dy\":127,\"g\":30,\"at\":1080},"
             "\"m\":{\"on\":1,\"g\":3,\"at\":1260},\"so\":{\"on\":1,\"dy\":127,\"at\":1140,\"who\":\"Mum\"},\"wd\":{\"on\":1,\"at\":1350}}");
    sim_link("{\"t\":\"meals\",\"b\":\"Greek yogurt, berries and walnuts\",\"l\":\"Chickpea salad with feta and olive oil\","
             "\"d\":\"Grilled salmon, quinoa and green beans\"}");
    shot(&life_screen, "life_empty");
    for (int i = 0; i < 3; i++) sim_tap(233 + 118, 250 + 42);   // three glasses of water
    sim_run(1600);
    sim_save("life");
    shot(&life_meal_screen_ref(), "life_meals");
    shot(&breathe_screen, "breathe_pick");
    sim_tap(233, 392);                       // Start
    sim_run(3000);
    sim_save("breathe");
    shot(&stretch_screen, "stretch", 4000);
    shot(&settings_body_screen_ref, "body");
    shot(&tips_screen, "tips_swipe", 900);
    for (int i = 0; i < 4; i++) sim_tap(380, 233);    // on to "Touch and hold"
    sim_run(1200);
    sim_save("tips_hold");
    for (int i = 0; i < 2; i++) sim_tap(380, 233);    // "Side button"
    sim_run(1000);
    sim_save("tips_button");
    shot(&contacts_screen_ref, "review_contacts");
    shot(&phone_screen_ref, "review_phone");
    shot(&gallery_screen_ref, "review_gallery");
    shot(&media_screen_ref, "review_files");
    shot(&pet_screen_ref, "review_pet");
    shot(&diagnostics_screen_ref, "review_diagnostics");
    shot(&snake_screen_ref, "review_snake", 600);
    shot(&smart_stack_screen, "smart_stack");
    // more from one app: they stack up
    sim_link("{\"t\":\"ntf\",\"id\":14,\"ap\":\"WhatsApp\",\"ti\":\"Marco\",\"tx\":\"Are you coming tonight?\",\"rp\":1}");
    sim_link("{\"t\":\"ntf\",\"id\":15,\"ap\":\"WhatsApp\",\"ti\":\"Family\",\"tx\":\"Mum: dinner on Sunday?\",\"rp\":1}");
    sim_run(4000);
    home();
    sim_save("watchface_unread");
    shot(&notifications_screen, "notifications_grouped");
    sim_tap(233, 92 + 28 + 50);             // the WhatsApp stack: fans out
    sim_run(400);
    sim_save("notifications_expanded");
    // a row dragged left, halfway through
    ui_handle_touch(380, 92 + 28 + 40 + 8 + 50, true);
    sim_run(40);
    for (int x = 370; x >= 230; x -= 20) { ui_handle_touch(x, 92 + 28 + 40 + 8 + 50, true); sim_run(30); }
    sim_save("notifications_swipe");
    ui_handle_touch(230, 92 + 28 + 40 + 8 + 50, false);
    sim_run(400);
    shot(&poweroff_screen, "power_sos");
    shot(&sos_screen, "sos_countdown", 2300);
    sim_run(4000);
    sim_save("sos_sent");
    g_app_settings.low_power = true;
    shot(&activity_screen, "low_power_status");
    g_app_settings.low_power = false;
    home();
    ui_handle_touch(233, 300, true);        // touch and hold the face
    sim_run(900);
    ui_handle_touch(233, 300, false);
    sim_run(400);
    sim_save("face_switcher");
    home();
    shot(&notifications_screen, "notifications");
    shot(&nowplaying_screen, "music");
    shot(&calendar_screen, "calendar");
    shot(&settings_screen, "settings");
    g_sim.fwup = FWUP_AVAILABLE;
    shot(&fwup_screen, "software_update");
    g_sim.fwup = FWUP_IDLE;
    shot(&battery_screen, "battery");
    shot(&recorder_screen, "recorder");
    sim_tap(233, 110);                     // Record
    sim_run(4200);                         // fill the level history
    sim_save("recorder_live");
    record("anim_recorder", 1200, 40);
    sim_tap(233, 345);                     // Stop
    sim_run(300);

    // Assistant: listening, then an answer that set a timer
    shot(&assistant_screen, "assistant_listen", 900);
    sim_tap(233, 233);
    sim_run(200);
    assistant_on_result(1, "Set a timer for 10 minutes for the pasta", "Timer set for 10 minutes.", "timer", 600, "");
    sim_run(1500);
    sim_save("assistant_answer");
    g_sim.recording = false;

    // Fall detection asking
    shot(&fall_alert_screen, "fall_alert", 8000);
    sleep_load_demo();
    shot(&sleep_screen, "sleep_app");
    g_app_settings.alarm_on = false;
    shot(&draw_screen, "draw_open");
    shot(&activity_screen, "activity");

    // Piano Tiles: song list, a song's card, a song under way, the result
    shot(&piano_screen, "piano_menu");
    sim_tap(233, 156);                     // Ode to Joy
    sim_run(400);
    sim_save("piano_pick");
    sim_tap(233, 352);                     // Play
    sim_run(300);
    sim_tap(330, 136);                     // the first tile (top-right)
    sim_run(500);
    sim_tap(330, 330);                     // the next one (bottom-right)
    sim_run(500);
    sim_save("piano_play");
    record("anim_piano", 1600, 50);
    sim_tap(60, 233); sim_run(100);        // two wrong lanes: the result screen
    sim_tap(60, 233); sim_run(1600);
    sim_save("piano_result");

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
