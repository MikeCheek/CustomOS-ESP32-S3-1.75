#pragma once
#include "Arduino.h"
#include <dirent.h>
#include <stdio.h>
#include <string>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
// Host shim for the SD card. The screenshot simulator has an empty one;
// the app's emulator points it at a folder (sim_sd_root, set by
// sim_sd_enable() in sim_stubs.cpp) and files are real files there.
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

std::string &sim_sd_root();   // "" = no card
inline std::string sim_sd_path(const char *p) {
    std::string r = sim_sd_root();
    if (!p || !*p) return r;
    return r + (p[0] == '/' ? "" : "/") + p;
}

class File : public Print {
    struct Impl {
        FILE *fp = nullptr;
        DIR *dir = nullptr;
        std::string path, name;   // path as the firmware sees it ("/Recordings/a.wav")
        ~Impl() { if (fp) fclose(fp); if (dir) closedir(dir); }
    };
    std::shared_ptr<Impl> d_;
public:
    File() {}
    // opens `path` (firmware view) with an fopen mode; dirs for reading
    static File open_path(const char *path, const char *mode) {
        File f;
        if (sim_sd_root().empty() || !path) return f;
        std::string host = sim_sd_path(path);
        struct stat st;
        bool exists = stat(host.c_str(), &st) == 0;
        auto impl = std::make_shared<Impl>();
        impl->path = path[0] == '/' ? path : std::string("/") + path;
        size_t sl = impl->path.find_last_of('/');
        impl->name = sl == std::string::npos ? impl->path : impl->path.substr(sl + 1);
        if (exists && S_ISDIR(st.st_mode)) {
            impl->dir = opendir(host.c_str());
            if (!impl->dir) return f;
        } else {
            const char *m = (mode && mode[0] == 'w') ? "wb+" : (mode && mode[0] == 'a') ? "ab+" : "rb";
            impl->fp = fopen(host.c_str(), m);
            if (!impl->fp) return f;
        }
        f.d_ = impl;
        return f;
    }
    operator bool() const { return d_ && (d_->fp || d_->dir); }
    size_t write(uint8_t c) override { return d_ && d_->fp ? fwrite(&c, 1, 1, d_->fp) : 0; }
    size_t write(const uint8_t *b, size_t n) override { return d_ && d_->fp ? fwrite(b, 1, n, d_->fp) : 0; }
    using Print::write;
    size_t size() {
        if (!d_ || !d_->fp) return 0;
        long cur = ftell(d_->fp);
        fseek(d_->fp, 0, SEEK_END);
        long n = ftell(d_->fp);
        fseek(d_->fp, cur, SEEK_SET);
        return n < 0 ? 0 : (size_t)n;
    }
    size_t position() { return d_ && d_->fp ? (size_t)ftell(d_->fp) : 0; }
    int available() { return d_ && d_->fp ? (int)(size() - position()) : 0; }
    int read() { if (!d_ || !d_->fp) return -1; int c = fgetc(d_->fp); return c == EOF ? -1 : c; }
    size_t read(uint8_t *b, size_t n) { return d_ && d_->fp ? fread(b, 1, n, d_->fp) : 0; }
    size_t readBytes(char *b, size_t n) { return read((uint8_t *)b, n); }
    int peek() { if (!d_ || !d_->fp) return -1; int c = fgetc(d_->fp); if (c != EOF) ungetc(c, d_->fp); return c == EOF ? -1 : c; }
    bool seek(uint32_t pos) { return d_ && d_->fp && fseek(d_->fp, pos, SEEK_SET) == 0; }
    void close() { d_.reset(); }
    void flush() { if (d_ && d_->fp) fflush(d_->fp); }
    bool isDirectory() { return d_ && d_->dir; }
    File openNextFile() {
        if (!d_ || !d_->dir) return File();
        for (struct dirent *e; (e = readdir(d_->dir));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string p = d_->path == "/" ? "/" + std::string(e->d_name) : d_->path + "/" + e->d_name;
            return open_path(p.c_str(), FILE_READ);
        }
        return File();
    }
    void rewindDirectory() { if (d_ && d_->dir) rewinddir(d_->dir); }
    const char *name() { return d_ ? d_->name.c_str() : ""; }
    const char *path() { return d_ ? d_->path.c_str() : ""; }
    String readStringUntil(char t) {
        String s;
        for (int c; (c = read()) >= 0 && c != t;) s += (char)c;
        return s;
    }
    time_t getLastWrite() {
        struct stat st;
        return d_ && stat(sim_sd_path(d_->path.c_str()).c_str(), &st) == 0 ? st.st_mtime : 0;
    }
};

namespace fs { using ::File; class FS {
public:
    File open(const char *p, const char *m = FILE_READ, bool = false) { return File::open_path(p, m); }
    File open(const String &p, const char *m = FILE_READ, bool c = false) { return open(p.c_str(), m, c); }
    bool exists(const char *p) { struct stat st; return !sim_sd_root().empty() && stat(sim_sd_path(p).c_str(), &st) == 0; }
    bool exists(const String &p) { return exists(p.c_str()); }
    bool remove(const char *p) { return !sim_sd_root().empty() && ::remove(sim_sd_path(p).c_str()) == 0; }
    bool remove(const String &p) { return remove(p.c_str()); }
    bool rename(const char *a, const char *b) { return !sim_sd_root().empty() && ::rename(sim_sd_path(a).c_str(), sim_sd_path(b).c_str()) == 0; }
    bool rename(const String &a, const String &b) { return rename(a.c_str(), b.c_str()); }
    bool mkdir(const char *p) { return !sim_sd_root().empty() && (::mkdir(sim_sd_path(p).c_str(), 0755) == 0 || exists(p)); }
    bool mkdir(const String &p) { return mkdir(p.c_str()); }
    bool rmdir(const char *p) { return !sim_sd_root().empty() && ::rmdir(sim_sd_path(p).c_str()) == 0; }
    bool rmdir(const String &p) { return rmdir(p.c_str()); }
}; }
