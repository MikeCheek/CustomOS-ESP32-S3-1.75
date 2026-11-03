// Drives libwatchemu (emu_main.cpp) on a PC the way the app does: init,
// steps at ~60 Hz, a swipe and a tap, frames saved as PPM.
//   tools/watch_sim/emu_host_test.sh
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

extern "C" {
int emu_init(const char *);
void emu_step();
uint32_t emu_frame_seq();
void emu_frame_rgba(uint8_t *);
void emu_touch(int, int, int);
void emu_button(int, int);
void emu_set_connected(int);
void emu_link_push(const char *);
int emu_link_pop(char *, int);
void emu_set_data(int, const char *);
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
    return emu_frame_seq() > 0 ? 0 : 1;
}
