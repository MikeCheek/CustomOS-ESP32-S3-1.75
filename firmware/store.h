/*
 * store.h
 * Files that outlive a restart: histories, the last weather, notification
 * history, app icons. On the SD card when one is in at boot (/data), else
 * on the watch's own 3.9 MB flash partition ("ffat", wear-levelled), so
 * nothing depends on a card. Settings stay in NVS (small, changed often).
 *
 * Writes are whole files, made safe against a power cut: the new data goes
 * to "<name>.tmp", then replaces the old file.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

void store_begin();                       // setup(), after sd_init()
bool store_ok();                          // somewhere to keep files
const char *store_where();                // "SD card", "watch", "nowhere"
// Reads up to max bytes of <name>; returns how many (-1: no such file).
int  store_read(const char *name, void *buf, size_t max);
int  store_size(const char *name);        // -1: no such file
bool store_write(const char *name, const void *buf, size_t len);
bool store_remove(const char *name);
bool store_exists(const char *name);
