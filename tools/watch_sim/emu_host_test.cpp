// Drives libwatchemu (emu_main.cpp) on a PC the way the app does: init,
// steps at ~60 Hz, a swipe and a tap, frames saved as PPM.
//   tools/watch_sim/emu_host_test.sh
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <ctime>
#include <vector>

extern "C" {
int emu_init(const char *);
void emu_step();
uint32_t emu_frame_seq();
void emu_frame_rgba(uint8_t *);
int emu_width();
void emu_photo_face(const char *);
int emu_ask_recorded(int ms);
int emu_height();
void emu_touch(int, int, int);
void emu_button(int, int);
void emu_set_connected(int);
void emu_link_push(const char *);
int emu_link_pop(char *, int);
void emu_set_data(int, const char *);
void emu_set_accel(float, float, float);
int emu_take_sounds(int32_t *, int);
void emu_set_option(const char *, int);
int emu_take_event(char *, int);
void emu_set_mic_level(int);
}

static void wait_ms(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}
static void run(int ms) {
    for (int t = 0; t < ms; t += 16) { emu_step(); wait_ms(16); }
}
static void save(const char *path) {
    std::vector<uint8_t> px(466 * 466 * 4);
    emu_frame_rgba(px.data());
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 466 466 255\n");
    for (size_t i = 0; i < 466 * 466; i++) fwrite(&px[i * 4], 1, 3, f);
    fclose(f);
    printf("saved %s (frame %u)\n", path, emu_frame_seq());
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : ".";
    char p[512];
    emu_init(out);
    run(1200);
    snprintf(p, sizeof(p), "%s/emu_face.ppm", out); save(p);
    // swipe left on the face: the app menu
    for (int x = 400; x >= 120; x -= 20) { emu_touch(x, 233, 1); run(16); }
    emu_touch(120, 233, 0);
    run(900);
    snprintf(p, sizeof(p), "%s/emu_menu.ppm", out); save(p);
    emu_button(0, 3);   // BOOT long: home
    run(600);
    snprintf(p, sizeof(p), "%s/emu_home.ppm", out); save(p);

    // phase 2: the app as the watch's phone, in-process
    emu_set_connected(1);
    emu_set_data(2, "{\"tempC\":23,\"condition\":\"Clear\",\"humidity\":40}");
    emu_link_push("{\"t\":\"hi\"}");
    emu_link_push("{\"t\":\"ntf\",\"id\":7,\"ap\":\"Chat\",\"ti\":\"Luca\",\"tx\":\"Dinner at 8?\",\"rp\":1}");
    run(500);
    char buf[2048];
    int got = 0, n;
    while ((n = emu_link_pop(buf, sizeof(buf))) > 0) { printf("watch -> app: %s\n", buf); got++; }
    snprintf(p, sizeof(p), "%s/emu_link.ppm", out); save(p);
    if (!got) { printf("FAIL: no message from the watch\n"); return 1; }

    // the app's Diagnostics asks how the watch is used: three parts come back
    emu_link_push("{\"t\":\"usage\"}");
    run(200);
    int parts = 0;
    while ((n = emu_link_pop(buf, sizeof(buf))) > 0) {
        if (strstr(buf, "\"e\":\"usage\"")) { parts++; printf("usage part: %.90s...\n", buf); }
    }
    printf("usage parts: %d\n", parts);
    if (parts != 4) { printf("FAIL: usage report\n"); return 1; }   // battery, hours, apps, what kept it awake

    // phase 3: a fall on the phone's accelerometer (62.5 Hz), then the alert's beeps
    auto acc = [](float x, float y, float z, float secs) {
        for (int i = 0; i < (int)(secs * 62.5f); i++) { emu_set_accel(x, y, z); if (i % 4 == 0) emu_step(); wait_ms(16); }
    };
    // (fall detection is off by default: turn it on as Settings would)
    emu_set_option("fall", 1);
    acc(-1, 0, 0, 2);
    acc(-0.2f, 0.1f, 0.1f, 0.3f);
    acc(2.0f, 2.2f, 1.5f, 0.05f);
    acc(0.3f, 0.4f, 1.3f, 1.0f);
    acc(0.05f, 0.1f, 0.98f, 3.2f);
    run(1500);
    int32_t snd[64 * 3];
    int ns = emu_take_sounds(snd, 64);
    printf("sounds after the fall: %d%s\n", ns, ns ? "" : " (none)");
    snprintf(p, sizeof(p), "%s/emu_fall.ppm", out); save(p);
    emu_button(1, 2);   // PWR double: back from the alert
    emu_button(0, 3);   // BOOT hold: home
    run(400);

    // phase 4: BOOT twice -> the assistant records (the app's microphone), the
    // voice stops, and the watch asks the app about the recording
    emu_button(0, 2);
    run(300);
    char ev[1024];
    bool rec = false;
    while (emu_take_event(ev, sizeof(ev)) > 0) { printf("event: %s\n", ev); rec |= strstr(ev, "\"rec\",\"on\":1") != nullptr; }
    emu_set_mic_level(5); run(400);     // the noise floor
    emu_set_mic_level(60); run(1200);   // speech
    emu_set_mic_level(5); run(1800);    // a pause: done
    bool asked = false;
    while (emu_take_event(ev, sizeof(ev)) > 0) printf("event: %s\n", ev);
    while ((n = emu_link_pop(buf, sizeof(buf))) > 0) { printf("watch -> app: %s\n", buf); asked |= strstr(buf, "\"ask\"") != nullptr; }
    snprintf(p, sizeof(p), "%s/emu_ask.ppm", out); save(p);
    if (!rec || !asked) { printf("FAIL: no recording (%d) or no ask (%d)\n", rec, asked); return 1; }

    // What the wake word heard that wasn't a command: sent as the question
    // as it is - no new recording, the file is exactly what was heard.
    while (emu_take_event(ev, sizeof(ev)) > 0) {}
    if (!emu_ask_recorded(1500)) { printf("FAIL: ask_recorded refused\n"); return 1; }
    run(300);
    bool rec2 = false, asked2 = false;
    while (emu_take_event(ev, sizeof(ev)) > 0) rec2 |= strstr(ev, "\"rec\",\"on\":1") != nullptr;
    while ((n = emu_link_pop(buf, sizeof(buf))) > 0) asked2 |= strstr(buf, "\"ask\"") && strstr(buf, "_ask.wav");
    struct stat ast;
    snprintf(p, sizeof(p), "%s/sd/Recordings/_ask.wav", out);
    long want = 44 + 1500L * 16 * 2;
    bool sized = stat(p, &ast) == 0 && ast.st_size == want;
    printf("recorded question: asked %d, no new recording %d, file %ld bytes (want %ld)\n", asked2, !rec2,
           (long)(sized ? ast.st_size : -1), want);
    if (!asked2 || rec2 || !sized) { printf("FAIL: recorded question\n"); return 1; }

    // The photo face (make_test_photoface.py wrote <out>/sd/faces/photo.pwf):
    // the red disc (the subject) in front of the white time, the time around it.
    emu_photo_face("test");
    run(1200);
    snprintf(p, sizeof(p), "%s/emu_photo.ppm", out); save(p);
    {
        int w = emu_width(), h = emu_height();
        static uint8_t f[466 * 466 * 4];
        emu_frame_rgba(f);
        const uint8_t *c = f + (200 * w + 233) * 4;
        bool subject_front = c[0] > 240 && c[1] < 16 && c[2] < 16;
        int white = 0;
        for (int y = 150; y < 250 && y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *q = f + (y * w + x) * 4;
                if (q[0] > 245 && q[1] > 245 && q[2] > 245) white++;
            }
        printf("photo face: subject in front %d, time pixels %d\n", subject_front, white);
        if (!subject_front || white < 300) { printf("FAIL: photo face\n"); return 1; }
        // and the app heard about it: {"e":"pf","l":["test"],...}
        bool listed = false;
        char lb[1024];
        while (emu_link_pop(lb, sizeof(lb)) > 0) listed |= strstr(lb, "\"e\":\"pf\"") && strstr(lb, "\"test\"");
        if (!listed) { printf("FAIL: no photo face list for the app\n"); return 1; }
        // with the tap set to the face's own (home gestures), a tap on the
        // Photo face shows the next photo (the green subject)
        emu_link_push("{\"t\":\"home\",\"a\":{\"tap\":\"face\"}}"); run(20);
        while (emu_link_pop(lb, sizeof(lb)) > 0) {}
        emu_touch(120, 380, 1); run(60); emu_touch(120, 380, 0);
        run(400);
        emu_frame_rgba(f);
        const uint8_t *g2 = f + (200 * w + 233) * 4;
        bool next = g2[1] > 240 && g2[0] < 16;
        printf("photo face: tap shows the next photo %d\n", next);
        snprintf(p, sizeof(p), "%s/emu_photo_next.ppm", out); save(p);
        if (!next) { printf("FAIL: tap didn't change the photo\n"); return 1; }
    }
    // 12-hour clock: "9:05" and AM/PM beside it
    emu_set_option("clock24", 0);
    run(1200);
    snprintf(p, sizeof(p), "%s/emu_photo_12h.ppm", out); save(p);
    emu_set_option("clock24", 1);
    return emu_frame_seq() > 0 ? 0 : 1;
}
