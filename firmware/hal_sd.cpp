#include "hal_sd.h"
#include "board_pins.h"
#include "config.h"
#include "hal_usb_msc.h"

#if FEATURE_SD_CARD

#include <SPI.h>
#include <SD.h>

static SPIClass s_sdSpi(HSPI);
static bool s_mounted = false;

bool sd_init() {
    s_sdSpi.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    s_mounted = SD.begin(PIN_SD_CS, s_sdSpi);
    if (!s_mounted) {
        DEBUG_PRINTF("[sd] mount FAILED - check board_pins.h SD_* pins / card inserted\n");
        return false;
    }
    DEBUG_PRINTF("[sd] mounted, size=%llu MB\n", sd_card_size_mb());

    // Quick I/O verification: write a test file and read it back
    {
        const char *test_path = "/_io_test.bin";
        const char *test_data = "SDIO0123456789ABCDEF";
        int tlen = (int)strlen(test_data);
        SD.remove(test_path);
        File wf = SD.open(test_path, FILE_WRITE);
        if (wf) {
            wf.write((const uint8_t *)test_data, tlen);
            wf.close();
            File rf = SD.open(test_path, FILE_READ);
            if (rf) {
                char buf[32] = {0};
                int got = rf.read((uint8_t *)buf, tlen);
                rf.close();
                if (got == tlen && memcmp(buf, test_data, tlen) == 0) {
                    DEBUG_PRINTF("[sd] I/O test OK\n");
                } else {
                    DEBUG_PRINTF("[sd] I/O test FAIL: got %d bytes, data mismatch\n", got);
                    DEBUG_PRINTF("[sd] expected: %.20s\n", test_data);
                    DEBUG_PRINTF("[sd] got:      %.20s\n", buf);
                }
            } else {
                DEBUG_PRINTF("[sd] I/O test FAIL: cannot re-open test file\n");
            }
        } else {
            DEBUG_PRINTF("[sd] I/O test FAIL: cannot create test file (write protect?)\n");
        }
        SD.remove(test_path);
    }

    // Read first file found on root to verify file data reads work
    {
        File root = SD.open("/");
        if (root && root.isDirectory()) {
            File entry = root.openNextFile();
            while (entry) {
                if (!entry.isDirectory() && entry.size() > 0) {
                    // Copy the name out BEFORE close() — entry.name()
                    // returns a pointer into the File object's own
                    // internal buffer, not a caller-owned copy. Using it
                    // after close() (as this used to) is a use-after-free:
                    // the underlying buffer can be freed/reused by then,
                    // which is exactly what produced the garbled/truncated
                    // filename and garbage trailing bytes seen in the
                    // serial log ('wm_test.txtxV..%') — and very likely
                    // the actual cause of audio_init() never completing
                    // right after this: the SD library's heap can end up
                    // corrupted, and the next big allocation (I2S DMA
                    // buffers in audio_init()) is what silently hangs/
                    // crashes on it, well before any [audio] log line.
                    char ename[64];
                    strncpy(ename, entry.name(), sizeof(ename) - 1);
                    ename[sizeof(ename) - 1] = '\0';
                    uint32_t esz = entry.size();
                    uint8_t hdr[8] = {0};
                    int got = entry.read(hdr, 8);
                    entry.close();
                    DEBUG_PRINTF("[sd] first file: '%s' (%lu bytes), read %d: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                                 ename, (unsigned long)esz, got,
                                 hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7]);
                    break;
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
        }
    }

    return true;
}

bool sd_is_mounted() { return s_mounted; }

bool sd_remount() {
    SD.end();
    s_mounted = SD.begin(PIN_SD_CS, s_sdSpi);
    DEBUG_PRINTF("[sd] remount %s\n", s_mounted ? "OK" : "FAILED");
    return s_mounted;
}

uint64_t sd_card_size_mb() {
    if (!s_mounted) return 0;
    return SD.cardSize() / (1024 * 1024);
}

uint64_t sd_used_mb() {
    if (!s_mounted) return 0;
    return SD.usedBytes() / (1024 * 1024);
}

String sd_list_root(int max_entries) {
    String out;
    if (!s_mounted) return "SD not mounted";

    File root = SD.open("/");
    if (!root || !root.isDirectory()) return "Failed to open /";

    int count = 0;
    File entry = root.openNextFile();
    while (entry && count < max_entries) {
        out += entry.isDirectory() ? "[DIR] " : "      ";
        out += entry.name();
        if (!entry.isDirectory()) {
            out += "  (" + String(entry.size()) + " B)";
        }
        out += "\n";
        entry.close();
        entry = root.openNextFile();
        count++;
    }
    root.close();
    if (out.length() == 0) out = "(empty)";
    return out;
}

bool sd_write_read_test(const String &contents, String &readback_out) {
    if (!s_mounted) { readback_out = "SD not mounted"; return false; }

    SD.remove("/wm_test.txt");
    File f = SD.open("/wm_test.txt", FILE_WRITE);
    if (!f) { readback_out = "open for write failed"; return false; }
    f.print(contents);
    f.close();

    f = SD.open("/wm_test.txt", FILE_READ);
    if (!f) { readback_out = "open for read failed"; return false; }
    readback_out = f.readString();
    f.close();

    return readback_out == contents;
}

bool sd_is_safe_filename(const char *name) {
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') return false;
    }
    if (strstr(name, "..")) return false;
    return true;
}

bool sd_save_file(const char *filename, const uint8_t *data, int len) {
    if (!s_mounted) return false;
    if (!sd_is_safe_filename(filename)) {
        DEBUG_PRINTF("[sd] save rejected: unsafe filename '%s'\n", filename ? filename : "(null)");
        return false;
    }
#if FEATURE_USB_MSC
    if (usb_msc_host_active()) {
        DEBUG_PRINTF("[sd] save rejected: SD card is mounted over USB\n");
        return false;
    }
#endif
    String path = String("/") + filename;
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        DEBUG_PRINTF("[sd] save failed: %s\n", filename);
        return false;
    }
    f.write(data, len);
    f.close();
    DEBUG_PRINTF("[sd] saved %s (%d bytes)\n", filename, len);
    return true;
}

bool sd_file_exists(const char *filename) {
    if (!s_mounted) return false;
    String path = String("/") + filename;
    return SD.exists(path);
}

#else // FEATURE_SD_CARD disabled

bool sd_init() { return false; }
bool sd_is_mounted() { return false; }
bool sd_remount() { return false; }
uint64_t sd_card_size_mb() { return 0; }
uint64_t sd_used_mb() { return 0; }
String sd_list_root(int) { return "SD feature disabled"; }
bool sd_write_read_test(const String &, String &readback_out) {
    readback_out = "SD feature disabled";
    return false;
}
bool sd_save_file(const char *, const uint8_t *, int) { return false; }
bool sd_file_exists(const char *) { return false; }

#endif
