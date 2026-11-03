// PC test for firmware/silence_gate.h:
//   g++ -std=c++17 -Ifirmware tools/silence_test.cpp -o /tmp/silence_test && /tmp/silence_test
#include "silence_gate.h"
#include <cstdio>

int main() {
    SilenceGate g;
    g.reset();
    int kept = 0, total = 0, fails = 0;
    auto run = [&](int ms, int level) { int k = 0; for (int t = 0; t < ms; t += 16) { total++; k += g.keep(level, 16); } kept += k; return k * 16; };
    run(2000, 6);                         // room noise: learn the floor
    int speech1 = run(3000, 40);          // talking
    int pause = run(5000, 7);             // a long pause
    int speech2 = run(2000, 35);          // talking again
    printf("floor %d, kept: speech %d ms + %d ms, pause %d of 5000 ms\n", g.floor(), speech1, speech2, pause);
    if (speech1 < 2990 || speech2 < 1990) { printf("FAIL speech dropped\n"); fails++; }
    if (pause > 700 || pause < 500) { printf("FAIL the pause should keep ~600 ms\n"); fails++; }
    return fails;
}
