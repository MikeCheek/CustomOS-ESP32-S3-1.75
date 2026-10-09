/*
 * hal_display.cpp
 * See hal_display.h for the pipeline overview.
 *
 * Why a separate task at all: Arduino_GFX's QSPI bus sends with
 * spi_device_polling_*() - the calling CPU byte-swaps each 1024-pixel
 * chunk and then spins until it's on the wire. A full 466x466 frame at
 * the CO5300's 40 MHz is ~22 ms of wire time plus the swapping, and the
 * old Arduino_Canvas::flush() did all of that on the UI core, inside
 * loop(), every frame - touch, gestures and app logic all stood still
 * for it. Here that work runs on core 0 while core 1 draws the next
 * frame, and only the rows/columns that actually changed are sent.
 */
#include "hal_display.h"
#include "board_pins.h"
#include "config.h"

#include <Arduino_GFX_Library.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>

#define FB_W        LCD_WIDTH
#define FB_H        LCD_HEIGHT
#define FB_BYTES    ((size_t)FB_W * FB_H * 2)
#define POOL_MAX    4

#define SEND_CORE   0      // loop() and all drawing run on core 1
#define SEND_PRIO   2      // below WiFi/BLE (they preempt it), above idle
#define SEND_STACK  6144   // printf with floats in the stats line needs headroom

// Partial-width rectangles are staged through this many pixels of
// internal RAM so each band is one contiguous draw16bitRGBBitmap()
// (one address window) rather than one window per row.
#define SCRATCH_PX  (FB_W * 16)

// Two dirty runs closer than this are sent as one rectangle - a window
// setup costs about as much as a few rows of pixels.
#define MERGE_GAP_ROWS 8
#define MAX_RECTS      16
// Past this share of the screen, one full-frame send beats many windows.
#define FULL_FRAME_PCT 60

// ---- Fast pixel path -----------------------------------------------------------
// Arduino_ESP32QSPI::writePixels() byte-swaps a 1024-pixel chunk, then
// spins until it's on the wire, then swaps the next: the CPU and the SPI
// DMA never overlap, so a full frame took ~100 ms for ~22 ms of wire time.
// Here a second device on the same bus streams the pixels with two DMA
// buffers in flight: chunk N+1 is swapped while chunk N is being sent.
// The library still sets the window (CASET/PASET/RAMWR); this only sends
// the pixel data that follows, framed exactly like writePixels() does
// (QSPI cmd 0x32, address 0x003C00 = "memory write continue").
#define FAST_CHUNK_PX 2048
static spi_device_handle_t s_fast = nullptr;
static uint32_t *s_fast_buf[2] = { nullptr, nullptr };
static spi_transaction_ext_t s_fast_t[2];

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *gfx = nullptr;
static bool s_display_ok = false;

static volatile uint8_t s_brightness = DEFAULT_BRIGHTNESS;
static volatile bool s_brightness_dirty = false;

// ---- Buffer pool -----------------------------------------------------------
enum FbState : uint8_t { FB_FREE, FB_RENDER, FB_QUEUED, FB_SENDING, FB_SHOWN };
static uint16_t *s_fb[POOL_MAX];
static volatile FbState s_state[POOL_MAX];
static volatile int s_pool_n = 0;
static volatile int s_queued = -1;
static volatile int s_shown = -1;
static volatile int s_last_submitted = -1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t s_task = nullptr;
static SemaphoreHandle_t s_freed = nullptr;     // a buffer became free
static SemaphoreHandle_t s_init_done = nullptr;
static SemaphoreHandle_t s_cmd_lock = nullptr;
static SemaphoreHandle_t s_cmd_done = nullptr;

enum PanelCmd : uint8_t { CMD_NONE, CMD_SLEEP, CMD_WAKE };
static volatile PanelCmd s_cmd = CMD_NONE;
static volatile bool s_force_full = true;
static bool s_panel_asleep = false;           // sender task only

static uint16_t *s_scratch = nullptr;
static int s_scratch_px = 0;

static DisplayStats s_stats = {};

// ---- Fast pixel path (see top) --------------------------------------------------
static void fast_init() {
    for (int i = 0; i < 2; i++) {
        s_fast_buf[i] = (uint32_t *)heap_caps_aligned_alloc(16, FAST_CHUNK_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_fast_buf[i]) {
            DEBUG_PRINTF("[display] fast path: no DMA memory, using the library's writes\n");
            return;
        }
    }
    spi_device_interface_config_t dev = {};
    dev.command_bits = 8;
    dev.address_bits = 24;
    dev.mode = 0;
    dev.clock_source = SPI_CLK_SRC_DEFAULT;
    dev.clock_speed_hz = DISPLAY_QSPI_HZ;
    dev.spics_io_num = -1;              // CS is ours, like the library's
    dev.flags = SPI_DEVICE_HALFDUPLEX;
    dev.queue_size = 2;
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_fast) != ESP_OK) {
        s_fast = nullptr;
        DEBUG_PRINTF("[display] fast path: couldn't add the SPI device\n");
    }
}

// Native RGB565 pixels -> the panel's big-endian order, two at a time.
static inline uint32_t swap565x2(uint32_t w) {
    return ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
}

// Sends the w x h block at (x, y) of fb (row pitch FB_W). x and w even.
static void fast_rect(const uint16_t *fb, int x, int y, int w, int h) {
    gfx->startWrite();
    ((Arduino_CO5300 *)gfx)->writeAddrWindow(x, y, w, h);
    gfx->endWrite();

    spi_device_acquire_bus(s_fast, portMAX_DELAY);
    gpio_set_level((gpio_num_t)PIN_LCD_CS, 0);
    int in_flight = 0, cur = 0, row = 0, col = 0;
    bool first = true;
    uint32_t left = (uint32_t)w * h;
    while (left) {
        uint32_t n = left > FAST_CHUNK_PX ? FAST_CHUNK_PX : left;
        if (in_flight == 2) {                       // reuse the oldest buffer
            spi_transaction_t *done;
            spi_device_get_trans_result(s_fast, &done, portMAX_DELAY);
            in_flight--;
        }
        // Fill: row segments of the rect, 2 pixels per 32-bit word.
        uint32_t *dst = s_fast_buf[cur];
        for (uint32_t k = 0; k < n; ) {
            const uint32_t *src = (const uint32_t *)(fb + (size_t)(y + row) * FB_W + x + col);
            uint32_t take = (uint32_t)(w - col);
            if (take > n - k) take = n - k;
            for (uint32_t i = 0; i < take / 2; i++) *dst++ = swap565x2(src[i]);
            k += take;
            col += take;
            if (col >= w) { col = 0; row++; }
        }
        spi_transaction_ext_t &t = s_fast_t[cur];
        memset(&t, 0, sizeof(t));
        if (first) {
            t.base.flags = SPI_TRANS_MODE_QIO;
            t.base.cmd = 0x32;
            t.base.addr = 0x003C00;
            first = false;
        } else {
            t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                           SPI_TRANS_VARIABLE_DUMMY;
        }
        t.base.tx_buffer = s_fast_buf[cur];
        t.base.length = n * 16;
        spi_device_queue_trans(s_fast, &t.base, portMAX_DELAY);
        in_flight++;
        cur ^= 1;
        left -= n;
    }
    while (in_flight--) {
        spi_transaction_t *done;
        spi_device_get_trans_result(s_fast, &done, portMAX_DELAY);
    }
    gpio_set_level((gpio_num_t)PIN_LCD_CS, 1);
    spi_device_release_bus(s_fast);
}

// ---- Panel bring-up (runs on the sender task) ---------------------------------
static void panel_init() {
    // Shared: the library takes the bus per write instead of keeping it,
    // so the fast pixel path below can use it in between.
    bus = new Arduino_ESP32QSPI(
        PIN_LCD_CS, PIN_LCD_SCLK,
        PIN_LCD_SDIO0, PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3, true);

    gfx = new Arduino_CO5300(bus, PIN_LCD_RST, LCD_ROTATION, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

    // Speed passed explicitly: the ESP32QSPI_* #defines that used to sit
    // in config.h never reached the library (it's compiled as its own
    // translation unit), so the bus was always on the library default.
    if (!gfx->begin(DISPLAY_QSPI_HZ)) {
        DEBUG_PRINTF("[display] gfx->begin() FAILED\n");
        return;
    }
    s_display_ok = true;
    ((Arduino_CO5300 *)gfx)->setBrightness(s_brightness);
    gfx->fillScreen(COLOR_BG);

    fast_init();
    if (!s_fast) {
        // Fallback: library writes, partial-width rects staged in scratch.
        s_scratch = (uint16_t *)heap_caps_malloc(SCRATCH_PX * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_scratch) s_scratch = (uint16_t *)heap_caps_malloc(SCRATCH_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_scratch_px = s_scratch ? SCRATCH_PX : 0;
    }

    DEBUG_PRINTF("[display] init done (%dx%d) QSPI %d MHz, sender on core %d\n",
                 LCD_WIDTH, LCD_HEIGHT, DISPLAY_QSPI_HZ / 1000000, xPortGetCoreID());
}

// ---- Sending ----------------------------------------------------------------
// x1/x2 inclusive. The CO5300 needs even-aligned windows (start on an
// even column/row, even width/height) - same rule MUSE's round_area()
// enforces - so the rect is widened to that here.
static void send_rect(const uint16_t *fb, int x1, int y1, int x2, int y2) {
    x1 &= ~1; y1 &= ~1;
    x2 |= 1;  y2 |= 1;
    if (x2 >= FB_W) x2 = FB_W - 1;
    if (y2 >= FB_H) y2 = FB_H - 1;
    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w <= 0 || h <= 0) return;

    if (s_fast) {
        fast_rect(fb, x1, y1, w, h);
        s_stats.px_sent += (uint32_t)w * h;
        return;
    }
    if (w == FB_W || !s_scratch) {
        // Full-width rows are already contiguous in the framebuffer.
        if (w == FB_W) {
            gfx->draw16bitRGBBitmap(0, y1, (uint16_t *)(fb + (size_t)y1 * FB_W), FB_W, h);
        } else {
            for (int y = y1; y <= y2; y++)
                gfx->draw16bitRGBBitmap(x1, y, (uint16_t *)(fb + (size_t)y * FB_W + x1), w, 1);
        }
        s_stats.px_sent += (uint32_t)w * h;
        return;
    }

    int rows_per = (s_scratch_px / w) & ~1;
    if (rows_per < 2) rows_per = 2;
    for (int y = y1; y <= y2; y += rows_per) {
        int hb = y2 - y + 1;
        if (hb > rows_per) hb = rows_per;
        for (int r = 0; r < hb; r++)
            memcpy(s_scratch + (size_t)r * w, fb + (size_t)(y + r) * FB_W + x1, (size_t)w * 2);
        gfx->draw16bitRGBBitmap(x1, y, s_scratch, w, hb);
    }
    s_stats.px_sent += (uint32_t)w * h;
}

// Widens [l, r] to cover every pixel of row y that differs between a
// and b. Compares two pixels per 32-bit word (rows are 932 bytes, so
// every row starts 4-byte aligned) - first from the left, then from the
// right, so a changed row only reads as far as its outermost changes.
static inline void row_span(const uint16_t *a16, const uint16_t *b16, int y, int &l, int &r) {
    const int NW = FB_W / 2;
    const uint32_t *a = (const uint32_t *)(a16 + (size_t)y * FB_W);
    const uint32_t *b = (const uint32_t *)(b16 + (size_t)y * FB_W);
    int i = 0;
    while (i < NW && a[i] == b[i]) i++;
    if (i == NW) return;
    int j = NW - 1;
    while (j > i && a[j] == b[j]) j--;
    if (2 * i < l) l = 2 * i;
    if (2 * j + 1 > r) r = 2 * j + 1;
}

struct DirtyRect { int16_t x1, y1, x2, y2; };

// Returns true if anything was sent.
static bool send_frame(const uint16_t *fb, const uint16_t *ref) {
    if (!ref) {
        send_rect(fb, 0, 0, FB_W - 1, FB_H - 1);
        return true;
    }

    DirtyRect rects[MAX_RECTS];
    int n = 0, open = -1, gap = 0;
    for (int y = 0; y < FB_H; y += 2) {
        int l = FB_W, r = -1;
        row_span(fb, ref, y, l, r);
        if (y + 1 < FB_H) row_span(fb, ref, y + 1, l, r);
        if (r < 0) {
            if (open >= 0) {
                gap += 2;
                if (gap > MERGE_GAP_ROWS) open = -1;
            }
            continue;
        }
        gap = 0;
        if (open < 0) {
            if (n < MAX_RECTS) {
                rects[n] = { (int16_t)l, (int16_t)y, (int16_t)r, (int16_t)(y + 1) };
                open = n++;
                continue;
            }
            open = n - 1; // out of slots - grow the last one
        }
        DirtyRect &c = rects[open];
        c.y2 = y + 1;
        if (l < c.x1) c.x1 = l;
        if (r > c.x2) c.x2 = r;
    }
    if (n == 0) return false;

    uint32_t area = 0;
    for (int i = 0; i < n; i++)
        area += (uint32_t)(rects[i].x2 - rects[i].x1 + 1) * (rects[i].y2 - rects[i].y1 + 1);
    if (area * 100 > (uint32_t)FB_W * FB_H * FULL_FRAME_PCT) {
        send_rect(fb, 0, 0, FB_W - 1, FB_H - 1);
        return true;
    }
    for (int i = 0; i < n; i++) send_rect(fb, rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2);
    return true;
}

static void service_commands() {
    PanelCmd cmd = s_cmd;
    if (cmd == CMD_SLEEP) {
        if (gfx) gfx->displayOff();
        s_panel_asleep = true;
        s_cmd = CMD_NONE;
        xSemaphoreGive(s_cmd_done);
    } else if (cmd == CMD_WAKE) {
        if (gfx) {
            // Coming out of the always-on clock the panel never slept:
            // skip DISPON/SLPOUT (and their 240 ms of delays).
            if (s_panel_asleep) gfx->displayOn();
            ((Arduino_CO5300 *)gfx)->setBrightness(s_brightness);
        }
        s_brightness_dirty = false;
        if (s_panel_asleep) s_force_full = true; // don't trust GRAM across sleep
        s_panel_asleep = false;
        s_cmd = CMD_NONE;
        xSemaphoreGive(s_cmd_done);
    }
    if (s_brightness_dirty && !s_panel_asleep && gfx) {
        s_brightness_dirty = false;
        ((Arduino_CO5300 *)gfx)->setBrightness(s_brightness);
    }
}

static void sender_task(void *) {
    panel_init();
    xSemaphoreGive(s_init_done);

    uint32_t stats_t0 = millis();
    DisplayStats last = {};
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        service_commands();
        if (s_pool_n == 0 || !s_display_ok) continue;

        int idx;
        portENTER_CRITICAL(&s_mux);
        idx = s_queued;
        if (idx >= 0) {
            s_queued = -1;
            s_state[idx] = FB_SENDING;
        }
        portEXIT_CRITICAL(&s_mux);
        if (idx < 0) continue;

        bool sent = false;
        uint32_t t0 = millis();
        if (!s_panel_asleep) {
            const uint16_t *ref = nullptr;
            if (!s_force_full && s_shown >= 0 && s_shown != idx) ref = s_fb[s_shown];
            s_force_full = false;
            sent = send_frame(s_fb[idx], ref);
        } else {
            s_force_full = true;
        }
        uint32_t dt = millis() - t0;

        portENTER_CRITICAL(&s_mux);
        int old = s_shown;
        if (!s_panel_asleep) {
            s_shown = idx;
            s_state[idx] = FB_SHOWN;
            if (old >= 0 && old != idx) s_state[old] = FB_FREE;
        } else {
            s_state[idx] = FB_FREE;
        }
        if (sent) { s_stats.frames_sent++; s_stats.send_ms_total += dt; }
        else s_stats.frames_skipped++;
        portEXIT_CRITICAL(&s_mux);
        xSemaphoreGive(s_freed);

        uint32_t now = millis();
        if (now - stats_t0 >= 5000) {
            DisplayStats s = s_stats;
            uint32_t fs = s.frames_sent - last.frames_sent;
            DEBUG_PRINTF("[lcd] %.1f fps sent, %u skipped, %u dropped, avg %u ms/frame, %u%% of screen/frame\n",
                         fs * 1000.0f / (now - stats_t0),
                         (unsigned)(s.frames_skipped - last.frames_skipped),
                         (unsigned)(s.frames_dropped - last.frames_dropped),
                         fs ? (unsigned)((s.send_ms_total - last.send_ms_total) / fs) : 0u,
                         fs ? (unsigned)((uint64_t)(s.px_sent - last.px_sent) * 100 / ((uint64_t)fs * FB_W * FB_H)) : 0u);
            last = s;
            stats_t0 = now;
        }

        // Let core 0's idle task (and the task watchdog that watches it)
        // run between frames even when frames arrive back to back.
        vTaskDelay(1);
    }
}

// ---- Public API ---------------------------------------------------------------
void display_init() {
    s_freed = xSemaphoreCreateBinary();
    s_init_done = xSemaphoreCreateBinary();
    s_cmd_lock = xSemaphoreCreateMutex();
    s_cmd_done = xSemaphoreCreateBinary();
    // The panel is brought up from the sender task itself, so the SPI bus
    // is created on the same core that will always drive it.
    xTaskCreatePinnedToCore(sender_task, "lcd_send", SEND_STACK, nullptr, SEND_PRIO, &s_task, SEND_CORE);
    if (xSemaphoreTake(s_init_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        DEBUG_PRINTF("[display] sender task did not finish panel init\n");
    }
}

int display_pipeline_start(uint16_t *first) {
    if (s_pool_n > 0 || !first) return s_pool_n;
    int n = 0;
    s_fb[n] = first;
    s_state[n++] = FB_FREE;
    while (n < POOL_MAX) {
        uint16_t *p = (uint16_t *)heap_caps_aligned_alloc(16, FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) break;
        memset(p, 0, FB_BYTES);
        s_fb[n] = p;
        s_state[n++] = FB_FREE;
    }
    s_shown = -1;
    s_queued = -1;
    s_force_full = true;
    s_pool_n = n;
    DEBUG_PRINTF("[display] frame pool: %d x %u bytes in PSRAM\n", n, (unsigned)FB_BYTES);
    return n;
}

static int index_of(const uint16_t *fb) {
    for (int i = 0; i < s_pool_n; i++)
        if (s_fb[i] == fb) return i;
    return -1;
}

uint16_t *display_fb_acquire(bool allow_drop) {
    if (s_pool_n == 0) return nullptr;
    uint32_t t0 = millis();
    for (;;) {
        int got = -1;
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < s_pool_n; i++) {
            if (s_state[i] == FB_FREE) { got = i; break; }
        }
        if (got < 0 && allow_drop && s_queued >= 0) {
            got = s_queued;
            s_queued = -1;
            s_stats.frames_dropped++;
        }
        if (got < 0 && s_pool_n == 1 && s_state[0] == FB_SHOWN) {
            got = 0;         // single-buffer fallback: draw over the reference
            s_shown = -1;    // ...so the next send can't diff against it
        }
        if (got >= 0) s_state[got] = FB_RENDER;
        portEXIT_CRITICAL(&s_mux);
        if (got >= 0) return s_fb[got];
        if (millis() - t0 > 250) return nullptr; // caller skips this frame
        xSemaphoreTake(s_freed, pdMS_TO_TICKS(20));
    }
}

void display_fb_release(uint16_t *fb) {
    int idx = index_of(fb);
    if (idx < 0) return;
    portENTER_CRITICAL(&s_mux);
    if (s_state[idx] == FB_RENDER) s_state[idx] = FB_FREE;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_freed);
}

void display_fb_submit(uint16_t *fb) {
    int idx = index_of(fb);
    if (idx < 0) return;
    bool dropped = false;
    portENTER_CRITICAL(&s_mux);
    int old = s_queued;
    if (old >= 0 && old != idx) {
        s_state[old] = FB_FREE;   // superseded before it was sent
        s_stats.frames_dropped++;
        dropped = true;
    }
    s_queued = idx;
    s_state[idx] = FB_QUEUED;
    s_last_submitted = idx;
    portEXIT_CRITICAL(&s_mux);
    if (dropped) xSemaphoreGive(s_freed);
    if (s_task) xTaskNotifyGive(s_task);
}

const uint16_t *display_fb_last_submitted() {
    int i = s_last_submitted;
    return i >= 0 ? s_fb[i] : nullptr;
}

void display_force_full_refresh() { s_force_full = true; }

DisplayStats display_get_stats() {
    portENTER_CRITICAL(&s_mux);
    DisplayStats s = s_stats;
    portEXIT_CRITICAL(&s_mux);
    return s;
}

static void run_cmd(PanelCmd cmd) {
    if (!s_task) {
        if (!gfx) return;
        if (cmd == CMD_SLEEP) gfx->displayOff(); else gfx->displayOn();
        return;
    }
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    xSemaphoreTake(s_cmd_done, 0); // clear any stale completion
    s_cmd = cmd;
    xTaskNotifyGive(s_task);
    if (xSemaphoreTake(s_cmd_done, pdMS_TO_TICKS(1500)) != pdTRUE) {
        DEBUG_PRINTF("[display] panel command %d timed out\n", (int)cmd);
    }
    xSemaphoreGive(s_cmd_lock);
}

void display_sleep() { run_cmd(CMD_SLEEP); }
void display_wakeup() { run_cmd(CMD_WAKE); }

void display_set_brightness(uint8_t level) {
    s_brightness = level;
    s_brightness_dirty = true;
    if (s_task) xTaskNotifyGive(s_task);
}

uint8_t display_get_brightness() { return s_brightness; }
bool display_is_ok() { return s_display_ok; }
Arduino_GFX *display_gfx() { return gfx; }
