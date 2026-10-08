// Host shim of the Arduino core - just enough for the watch UI code and
// Arduino_GFX to compile and run on Linux (tools/watch_sim).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <algorithm>
#include <string>

#define PROGMEM
#define PGM_P const char *
#define F(x) (x)
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#define pgm_read_ptr(a) (*(void *const *)(a))
#define pgm_read_float(a) (*(const float *)(a))
#define memcpy_P memcpy
#define strlen_P strlen
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define SERIAL_8N1 0

typedef uint8_t byte;
typedef bool boolean;

using std::min;
using std::max;
template <class T, class L, class H> static inline T constrain(T x, L lo, H hi) { return x < lo ? lo : (x > hi ? hi : x); }
template <class T> static inline T sq(T x) { return x * x; }
static inline long map(long x, long a, long b, long c, long d) { return (x - a) * (d - c) / (b - a) + c; }
#ifndef abs
#endif

uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
static inline void yield() {}
static inline void pinMode(int, int) {}
static inline void digitalWrite(int, int) {}
static inline int digitalRead(int) { return 0; }
static inline long random(long hi) { return hi > 0 ? rand() % hi : 0; }
static inline long random(long lo, long hi) { return hi > lo ? lo + rand() % (hi - lo) : lo; }
static inline void randomSeed(unsigned long s) { srand(s); }
static inline void *ps_malloc(size_t n) { return malloc(n); }
static inline void *ps_calloc(size_t n, size_t m) { return calloc(n, m); }
static inline uint32_t esp_random() { return (uint32_t)rand(); }

class __FlashStringHelper;
class String;

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *b, size_t n) { size_t k = 0; while (n--) k += write(*b++); return k; }
    size_t write(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
    size_t print(const char *s) { return write(s); }
    size_t print(const __FlashStringHelper *s) { return write((const char *)s); }
    size_t print(const String &s);
    size_t println(const String &s);
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); return write(b); }
    size_t print(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); return write(b); }
    size_t print(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return write(b); }
    size_t print(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); return write(b); }
    size_t print(double v, int d = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", d, v); return write(b); }
    size_t println(const char *s = "") { size_t n = write(s); return n + write((uint8_t)'\n'); }
    size_t printf(const char *fmt, ...) {
        char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); return write(b);
    }
};

class HostSerial : public Print {
public:
    size_t write(uint8_t c) override { return fputc(c, stderr) == EOF ? 0 : 1; }
    using Print::write;
    void begin(unsigned long, int = 0) {}
    int available() { return 0; }
    int read() { return -1; }
    operator bool() { return true; }
};
extern HostSerial Serial;

class String {
public:
    std::string s;
    String() {}
    String(const char *c) : s(c ? c : "") {}
    String(const std::string &x) : s(x) {}
    String(int v) : s(std::to_string(v)) {}
    String(unsigned v) : s(std::to_string(v)) {}
    String(long v) : s(std::to_string(v)) {}
    String(unsigned long v) : s(std::to_string(v)) {}
    String(float v, int d = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", d, v); s = b; }
    const char *c_str() const { return s.c_str(); }
    size_t length() const { return s.size(); }
    bool isEmpty() const { return s.empty(); }
    String &operator+=(const String &o) { s += o.s; return *this; }
    String &operator+=(const char *o) { s += o; return *this; }
    String &operator+=(char c) { s += c; return *this; }
    friend String operator+(const String &a, const String &b) { return String(a.s + b.s); }
    friend String operator+(const String &a, const char *b) { return String(a.s + b); }
    bool operator==(const char *o) const { return s == o; }
    bool operator==(const String &o) const { return s == o.s; }
    char operator[](size_t i) const { return s[i]; }
    int toInt() const { return atoi(s.c_str()); }
    float toFloat() const { return atof(s.c_str()); }
    bool startsWith(const char *p) const { return s.rfind(p, 0) == 0; }
    int indexOf(char c, int from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const char *c, int from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    void trim() { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n"); s = a == std::string::npos ? "" : s.substr(a, b - a + 1); }
    int lastIndexOf(const char *c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
    bool endsWith(const char *p) const { size_t n = strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; }
    String substring(size_t a, size_t b = std::string::npos) const { return String(s.substr(a, b == std::string::npos ? b : b - a)); }
};

inline size_t Print::print(const String &s) { return write(s.c_str()); }
inline size_t Print::println(const String &s) { return println(s.c_str()); }
