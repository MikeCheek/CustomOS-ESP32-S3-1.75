#pragma once
#include "Arduino.h"
// Host shim: an NVS namespace that's always empty (defaults everywhere).
class Preferences {
public:
    bool begin(const char *, bool = false) { return false; }
    void end() {}
    bool isKey(const char *) { return false; }
    bool remove(const char *) { return true; }
    bool clear() { return true; }
    bool getBool(const char *, bool d = false) { return d; }
    uint8_t getUChar(const char *, uint8_t d = 0) { return d; }
    int8_t getChar(const char *, int8_t d = 0) { return d; }
    uint16_t getUShort(const char *, uint16_t d = 0) { return d; }
    int16_t getShort(const char *, int16_t d = 0) { return d; }
    int32_t getInt(const char *, int32_t d = 0) { return d; }
    uint32_t getUInt(const char *, uint32_t d = 0) { return d; }
    int32_t getLong(const char *, int32_t d = 0) { return d; }
    uint32_t getULong(const char *, uint32_t d = 0) { return d; }
    int64_t getLong64(const char *, int64_t d = 0) { return d; }
    uint64_t getULong64(const char *, uint64_t d = 0) { return d; }
    float getFloat(const char *, float d = 0) { return d; }
    size_t getString(const char *, char *v, size_t n) { if (n) v[0] = 0; return 0; }
    String getString(const char *, String d = String()) { return d; }
    size_t getBytesLength(const char *) { return 0; }
    size_t getBytes(const char *, void *, size_t) { return 0; }
    template <class T> size_t put(const char *, T) { return 1; }
    size_t putBool(const char *, bool) { return 1; }
    size_t putUChar(const char *, uint8_t) { return 1; }
    size_t putChar(const char *, int8_t) { return 1; }
    size_t putUShort(const char *, uint16_t) { return 2; }
    size_t putShort(const char *, int16_t) { return 2; }
    size_t putInt(const char *, int32_t) { return 4; }
    size_t putUInt(const char *, uint32_t) { return 4; }
    size_t putLong(const char *, int32_t) { return 4; }
    size_t putULong(const char *, uint32_t) { return 4; }
    size_t putLong64(const char *, int64_t) { return 8; }
    size_t putULong64(const char *, uint64_t) { return 8; }
    size_t putFloat(const char *, float) { return 4; }
    size_t putString(const char *, const char *v) { return strlen(v); }
    size_t putString(const char *, const String &v) { return v.length(); }
    size_t putBytes(const char *, const void *, size_t n) { return n; }
};
