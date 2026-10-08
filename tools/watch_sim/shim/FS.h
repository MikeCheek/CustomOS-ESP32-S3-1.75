#pragma once
#include "Arduino.h"
// Host shim: a file system with nothing on it.
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
class File : public Print {
public:
    File() {}
    operator bool() const { return false; }
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t *, size_t) override { return 0; }
    using Print::write;
    int available() { return 0; }
    int read() { return -1; }
    size_t read(uint8_t *, size_t) { return 0; }
    size_t readBytes(char *, size_t) { return 0; }
    int peek() { return -1; }
    bool seek(uint32_t) { return false; }
    size_t position() { return 0; }
    size_t size() { return 0; }
    void close() {}
    void flush() {}
    bool isDirectory() { return false; }
    File openNextFile() { return File(); }
    void rewindDirectory() {}
    const char *name() { return ""; }
    const char *path() { return ""; }
    String readStringUntil(char) { return String(); }
    time_t getLastWrite() { return 0; }
};
namespace fs { using ::File; class FS {
public:
    File open(const char *, const char * = FILE_READ, bool = false) { return File(); }
    File open(const String &p, const char *m = FILE_READ, bool c = false) { return open(p.c_str(), m, c); }
    bool exists(const char *) { return false; }
    bool exists(const String &) { return false; }
    bool remove(const char *) { return false; }
    bool remove(const String &) { return false; }
    bool rename(const char *, const char *) { return false; }
    bool mkdir(const char *) { return false; }
    bool rmdir(const char *) { return false; }
}; }
