// JPEGDEC for the emulator (EMU_JPEGDEC in sim_stubs.cpp). On arm64 the
// library turns on NEON by itself, which needs its arm_neon.h include, so
// SIMD stays on there; elsewhere (x86 PCs and simulators, 32-bit ARM) it's
// plain C, with no CPU-feature flags to get right.
#if !(defined(__aarch64__) || defined(__arm64__))
#define NO_SIMD
#endif
#include "JPEGDEC.cpp"
