#pragma once
#include "Arduino.h"
#include <string>
#include <string.h>
// Host shim for the ESP32's NVS. The screenshot simulator keeps it empty
// (defaults everywhere, nothing saved). The app's emulator turns on a real
// store (sim_prefs_enable(), sim_stubs.cpp): values kept per namespace and
// saved to a file, so settings survive a restart like on the watch.
bool sim_prefs_live();
bool sim_prefs_get(const std::string &key, std::string &out);
void sim_prefs_put(const std::string &key, const void *v, size_t n);
void sim_prefs_erase(const std::string &key);
void sim_prefs_clear_ns(const std::string &ns);
void sim_prefs_save();

class Preferences {
    std::string ns_;
    bool open_ = false, dirty_ = false;
    std::string k(const char *key) const { return ns_ + "/" + key; }
    template <class T> T get(const char *key, T d) {
        std::string v;
        if (!open_ || !sim_prefs_get(k(key), v) || v.size() != sizeof(T)) return d;
        T out;
        memcpy(&out, v.data(), sizeof(T));
        return out;
    }
    template <class T> size_t set(const char *key, T v) {
        if (!open_) return sizeof(T);
        sim_prefs_put(k(key), &v, sizeof(T));
        dirty_ = true;
        return sizeof(T);
    }
public:
    ~Preferences() { end(); }
    bool begin(const char *ns, bool = false) {
        if (!sim_prefs_live()) return false;
        ns_ = ns;
        open_ = true;
        return true;
    }
    void end() {
        if (open_ && dirty_) sim_prefs_save();
        open_ = dirty_ = false;
    }
    bool isKey(const char *key) { std::string v; return open_ && sim_prefs_get(k(key), v); }
    bool remove(const char *key) { if (open_) { sim_prefs_erase(k(key)); dirty_ = true; } return true; }
    bool clear() { if (open_) { sim_prefs_clear_ns(ns_); dirty_ = true; } return true; }
    bool getBool(const char *key, bool d = false) { return get<uint8_t>(key, d ? 1 : 0) != 0; }
    uint8_t getUChar(const char *key, uint8_t d = 0) { return get(key, d); }
    int8_t getChar(const char *key, int8_t d = 0) { return get(key, d); }
    uint16_t getUShort(const char *key, uint16_t d = 0) { return get(key, d); }
    int16_t getShort(const char *key, int16_t d = 0) { return get(key, d); }
    int32_t getInt(const char *key, int32_t d = 0) { return get(key, d); }
    uint32_t getUInt(const char *key, uint32_t d = 0) { return get(key, d); }
    int32_t getLong(const char *key, int32_t d = 0) { return get(key, d); }
    uint32_t getULong(const char *key, uint32_t d = 0) { return get(key, d); }
    int64_t getLong64(const char *key, int64_t d = 0) { return get(key, d); }
    uint64_t getULong64(const char *key, uint64_t d = 0) { return get(key, d); }
    float getFloat(const char *key, float d = 0) { return get(key, d); }
    size_t getString(const char *key, char *v, size_t n) {
        std::string s;
        if (!open_ || !sim_prefs_get(k(key), s)) { if (n) v[0] = 0; return 0; }
        size_t c = s.size() < n ? s.size() : (n ? n - 1 : 0);
        memcpy(v, s.data(), c);
        if (n) v[c] = 0;
        return c;
    }
    String getString(const char *key, String d = String()) {
        std::string s;
        return open_ && sim_prefs_get(k(key), s) ? String(s.c_str()) : d;
    }
    size_t getBytesLength(const char *key) { std::string s; return open_ && sim_prefs_get(k(key), s) ? s.size() : 0; }
    size_t getBytes(const char *key, void *out, size_t n) {
        std::string s;
        if (!open_ || !sim_prefs_get(k(key), s)) return 0;
        size_t c = s.size() < n ? s.size() : n;
        memcpy(out, s.data(), c);
        return c;
    }
    size_t putBool(const char *key, bool v) { return set<uint8_t>(key, v ? 1 : 0); }
    size_t putUChar(const char *key, uint8_t v) { return set(key, v); }
    size_t putChar(const char *key, int8_t v) { return set(key, v); }
    size_t putUShort(const char *key, uint16_t v) { return set(key, v); }
    size_t putShort(const char *key, int16_t v) { return set(key, v); }
    size_t putInt(const char *key, int32_t v) { return set(key, v); }
    size_t putUInt(const char *key, uint32_t v) { return set(key, v); }
    size_t putLong(const char *key, int32_t v) { return set(key, v); }
    size_t putULong(const char *key, uint32_t v) { return set(key, v); }
    size_t putLong64(const char *key, int64_t v) { return set(key, v); }
    size_t putULong64(const char *key, uint64_t v) { return set(key, v); }
    size_t putFloat(const char *key, float v) { return set(key, v); }
    size_t putString(const char *key, const char *v) {
        size_t n = strlen(v);
        if (open_) { sim_prefs_put(k(key), v, n); dirty_ = true; }
        return n;
    }
    size_t putString(const char *key, const String &v) { return putString(key, v.c_str()); }
    size_t putBytes(const char *key, const void *v, size_t n) {
        if (open_) { sim_prefs_put(k(key), v, n); dirty_ = true; }
        return n;
    }
};
