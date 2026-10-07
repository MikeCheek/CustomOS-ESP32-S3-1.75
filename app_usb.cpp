#include "app_usb.h"
#include "config.h"
#include "ui_font.h"
#include "board_pins.h"
#include "hal_usb.h"
#include "hal_sd.h"
#include <Arduino_GFX_Library.h>
#include <string.h>

// Consumer-control usages (HID usage table 0x0C) - same values as
// USBHIDConsumerControl.h, repeated so this file doesn't need the USB
// headers (only hal_usb.cpp talks to TinyUSB).
static const uint16_t CC_NEXT = 0x00B5, CC_PREV = 0x00B6, CC_PLAY_PAUSE = 0x00CD,
                      CC_MUTE = 0x00E2, CC_VOL_UP = 0x00E9, CC_VOL_DOWN = 0x00EA;

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

// =========================================================================
//  USB Mode - a list of radio rows (one per mode) under a live status
//  card, plus actions. Anything with a consequence the user might not
//  expect (serial stops, PC loses the watch, a restart) opens a sheet
//  that says so before doing it.
// =========================================================================

enum ItemKind : uint8_t { IT_STATUS, IT_MODE, IT_RESTART, IT_OPEN_REMOTE, IT_FLASH };
struct Item { ItemKind kind; UsbMode mode; int y, h; };

static const char *const MODE_DESC[USB_MODE_COUNT] = {
    "Battery charges, the PC sees nothing",
    "Serial monitor, IDE / idf.py upload, JTAG",
    "SD card as a USB drive on the PC",
    "Play/pause, tracks & volume for the PC",
};

static const int ROW_W = 340, ROW_X = (LCD_WIDTH - ROW_W) / 2;
static const int TOP = 80, GAP = 8, STATUS_H = 64, MODE_H = 80, ACTION_H = 62;
static const int BOTTOM_PAD = 110;

static Item     s_items[USB_MODE_COUNT + 4];
static int      s_item_count = 0;
static int      s_content_h = 0;
static UiScroll s_scroll;
static bool     s_back_armed = false;

// Sheet (in-screen confirmation with room for a real explanation)
enum SheetKind : uint8_t { SH_NONE, SH_SWITCH, SH_RESTART, SH_FLASH };
static SheetKind s_sheet = SH_NONE;
static UsbMode   s_sheet_mode = USB_MODE_FIRMWARE;
static const char *s_sheet_title = "";
static const char *s_sheet_lines[6];
static int       s_sheet_nlines = 0;
static const char *s_sheet_ok = "OK";
static uint16_t  s_sheet_ok_color = COLOR_ACCENT;
static bool      s_swallow = false;      // eat the rest of the touch that closed a sheet
static bool      s_prev_pressed = false;
static uint32_t  s_flash_at = 0;         // >0: rebooting to flash mode at this millis()

static void build_items() {
    s_item_count = 0;
    int y = TOP;
    auto add = [&](ItemKind k, UsbMode m, int h) {
        s_items[s_item_count++] = { k, m, y, h };
        y += h + GAP;
    };
    add(IT_STATUS, USB_MODE_FIRMWARE, STATUS_H);
    for (int m = 0; m < USB_MODE_COUNT; m++) add(IT_MODE, (UsbMode)m, MODE_H);
    if (usb_mode_restart_pending()) add(IT_RESTART, usb_mode_saved(), ACTION_H);
    if (usb_mode_active() == USB_MODE_REMOTE && !usb_mode_restart_pending())
        add(IT_OPEN_REMOTE, USB_MODE_REMOTE, ACTION_H);
    add(IT_FLASH, USB_MODE_FIRMWARE, ACTION_H);
    s_content_h = y - GAP + BOTTOM_PAD;
}

static void open_sheet(SheetKind k, UsbMode m) {
    s_sheet = k;
    s_sheet_mode = m;
    s_sheet_nlines = 0;
    auto line = [](const char *t) { if (s_sheet_nlines < 6) s_sheet_lines[s_sheet_nlines++] = t; };
    s_sheet_ok_color = COLOR_ACCENT;
    switch (k) {
    case SH_SWITCH:
        s_sheet_title = usb_mode_name(m);
        if (m == USB_MODE_CHARGE_ONLY) {
            line("The PC won't see the watch");
            line("at all - no serial, no");
            line("upload. To flash again use");
            line("Flash mode below or switch");
            line("back to Firmware & Debug.");
            s_sheet_ok = "Switch";
        } else if (m == USB_MODE_STORAGE) {
            line("The SD card becomes a USB");
            line("drive on the PC. Serial and");
            line("IDE upload stop until you");
            line("switch back (restart).");
            line("Eject on the PC first!");
            s_sheet_ok = "Start";
        } else {  // remote
            line("The watch becomes a media");
            line("keyboard for the PC. Serial");
            line("and IDE upload stop until");
            line("you switch back (restart).");
            s_sheet_ok = "Start";
        }
        break;
    case SH_RESTART:
        s_sheet_title = "Restart needed";
        line("Leaving this mode restarts");
        line("the watch's USB. Restart");
        line("now? (Later = on next boot)");
        if (usb_msc_host_active()) {
            line("");
            line("Eject the drive on the PC!");
            s_sheet_ok_color = COLOR_WARN;
        }
        s_sheet_ok = "Restart";
        break;
    case SH_FLASH:
        s_sheet_title = "Flash mode";
        line("Reboots into the ESP32-S3");
        line("ROM bootloader: upload from");
        line("Arduino IDE / idf.py /");
        line("esptool without buttons.");
        line("Press RESET to leave.");
        s_sheet_ok = "Reboot";
        s_sheet_ok_color = COLOR_WARN;
        break;
    default: break;
    }
}

static void choose_mode(UsbMode m) {
    if (m == usb_mode_saved() && !usb_mode_restart_pending()) {
        if (m == USB_MODE_REMOTE) ui_push(&usb_remote_screen);
        return;
    }
    if (!usb_mode_supported(m)) { ui_show_toast("Disabled in this build"); return; }
    if (m == USB_MODE_STORAGE && !sd_is_mounted()) { ui_show_toast("No SD card inserted"); return; }
    if (usb_mode_switch_needs_restart(m) || m == usb_mode_active()) {
        // Leaving an OTG mode (or undoing a pending change): just record
        // the choice; the sheet offers the restart.
        usb_mode_select(m);
        build_items();
        if (usb_mode_restart_pending()) open_sheet(SH_RESTART, m);
        return;
    }
    if (m == USB_MODE_FIRMWARE) {          // harmless: apply straight away
        usb_mode_select(m);
        ui_show_toast("Firmware & Debug on");
        build_items();
        return;
    }
    open_sheet(SH_SWITCH, m);
}

static void sheet_confirm() {
    SheetKind k = s_sheet;
    UsbMode m = s_sheet_mode;
    s_sheet = SH_NONE;
    switch (k) {
    case SH_SWITCH: {
        UsbSwitchResult r = usb_mode_select(m);
        if (r == USB_SWITCH_DONE) {
            if (m == USB_MODE_REMOTE) ui_push(&usb_remote_screen);
            else ui_show_toast(m == USB_MODE_STORAGE ? "SD card shared over USB" : "Charging only");
        } else if (r == USB_SWITCH_UNSUPPORTED) {
            ui_show_toast("Build with USB Mode: HW CDC+JTAG");
        } else if (r == USB_SWITCH_NO_SD) {
            ui_show_toast("No SD card inserted");
        } else if (r == USB_SWITCH_NEEDS_RESTART) {
            open_sheet(SH_RESTART, m);
        } else {
            ui_show_toast("USB switch failed");
        }
        break;
    }
    case SH_RESTART: usb_mode_restart(); break;  // doesn't return
    case SH_FLASH:   s_flash_at = millis() + 450; break;  // let the "ready" frame reach the panel first
    default: break;
    }
    build_items();
}

// ---- drawing --------------------------------------------------------------

static void draw_status(Arduino_GFX *g, int y) {
    g->fillRoundRect(ROW_X, y, ROW_W, STATUS_H, 18, COLOR_PANEL);
    bool cable = usb_cable_present(), host = usb_host_connected();
    bool busy = usb_mode_active() == USB_MODE_STORAGE && usb_msc_host_active() &&
                millis() - usb_msc_activity_ms() < 400;
    uint16_t dot = !cable ? COLOR_TEXT_DIM : host ? COLOR_GOOD : COLOR_WARN;
    if (busy && (millis() / 120) % 2) dot = COLOR_ACCENT;
    g->fillCircle(ROW_X + 30, y + STATUS_H / 2, 9, dot);
    const char *head = busy ? "Transferring..." : !cable ? "Not connected" : host ? "Connected" : "Cable in";
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    ui_print(ROW_X + 54, y + 13, 2, COLOR_TEXT, head);
    g->setTextSize(1);
    g->setTextColor(COLOR_TEXT_DIM);
    ui_print(ROW_X + 54, y + 40, 1, COLOR_TEXT_DIM, usb_mode_status_text());
}

static void draw_mode_row(Arduino_GFX *g, int y, UsbMode m) {
    bool saved = usb_mode_saved() == m, active = usb_mode_active() == m;
    bool pending = usb_mode_restart_pending();
    uint16_t border = saved ? COLOR_ACCENT : COLOR_TEXT_DIM;
    g->fillRoundRect(ROW_X, y, ROW_W, MODE_H, 18, border);
    g->fillRoundRect(ROW_X + 2, y + 2, ROW_W - 4, MODE_H - 4, 16, COLOR_PANEL);

    int rx = ROW_X + 30, ry = y + MODE_H / 2;
    g->fillCircle(rx, ry, 13, border);
    g->fillCircle(rx, ry, 10, COLOR_PANEL);
    if (saved) g->fillCircle(rx, ry, 6, COLOR_ACCENT);

    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    ui_print(ROW_X + 54, y + 12, 2, COLOR_TEXT, usb_mode_name(m));
    g->setTextSize(1);
    g->setTextColor(COLOR_TEXT_DIM);
    ui_print(ROW_X + 54, y + 38, 1, COLOR_TEXT_DIM, MODE_DESC[m]);

    const char *tag = nullptr;
    uint16_t tag_c = COLOR_GOOD;
    if (!usb_mode_supported(m))        { tag = "NOT IN THIS BUILD (see config.h)"; tag_c = COLOR_TEXT_DIM; }
    else if (m == USB_MODE_STORAGE && !sd_is_mounted()) { tag = "NO SD CARD"; tag_c = COLOR_BAD; }
    else if (saved && active)          tag = "IN USE";
    else if (saved && pending)         { tag = "AFTER RESTART"; tag_c = COLOR_WARN; }
    else if (active && pending)        { tag = "IN USE UNTIL RESTART"; tag_c = COLOR_TEXT_DIM; }
    if (tag) {
        g->setTextColor(tag_c);
        ui_print(ROW_X + 54, y + 56, 1, tag_c, tag);
    }
}

static void draw_action(Arduino_GFX *g, int y, const char *label, const char *desc, uint16_t fill) {
    // Dark ink on the coloured action pills (white on yellow is unreadable).
    bool plain = fill == COLOR_PANEL;
    uint16_t ink = plain ? COLOR_TEXT : COLOR_BG;
    g->fillRoundRect(ROW_X, y, ROW_W, ACTION_H, ACTION_H / 2, fill);
    g->setTextSize(2);
    g->setTextColor(ink);
    ui_print(ROW_X + 28, y + 12, 2, ink, label);
    g->setTextSize(1);
    g->setTextColor(plain ? COLOR_TEXT_DIM : ink);
    ui_print(ROW_X + 28, y + 38, 1, plain ? COLOR_TEXT_DIM : ink, desc);
    int cx = ROW_X + ROW_W - 30, cy = y + ACTION_H / 2;
    g->fillTriangle(cx - 4, cy - 9, cx - 4, cy + 9, cx + 8, cy, ink);
}

static void draw_sheet(Arduino_GFX *g) {
    g->fillScreen(COLOR_BG);
    ui_edge_ring(0, 360, ui_dim(s_sheet_ok_color, 0.35f), 6, 4, false);
    ui_text_center(CX, 112, COLOR_TEXT, s_sheet_title, 3);
    for (int i = 0; i < s_sheet_nlines; i++)
        ui_text_center(CX, 166 + i * 26, COLOR_TEXT_DIM, s_sheet_lines[i], 2);
    int by = 330, bw = 132, bh = 56;
    g->fillRoundRect(CX - 8 - bw, by, bw, bh, bh / 2, COLOR_PANEL);
    ui_text_center(CX - 8 - bw / 2, by + bh / 2, COLOR_TEXT, s_sheet == SH_RESTART ? "Later" : "Cancel", 2);
    g->fillRoundRect(CX + 8, by, bw, bh, bh / 2, s_sheet_ok_color);
    ui_text_center(CX + 8 + bw / 2, by + bh / 2, COLOR_BG, s_sheet_ok, 2);
}

static void draw_flash_ready(Arduino_GFX *g) {
    g->fillScreen(COLOR_BG);
    ui_edge_ring(0, 360, COLOR_WARN, 10, 4, false);
    // Chip glyph
    g->fillRoundRect(CX - 34, 92, 68, 68, 10, COLOR_WARN);
    g->fillRoundRect(CX - 24, 102, 48, 48, 6, COLOR_BG);
    for (int i = 0; i < 4; i++) {
        int o = -24 + 6 + i * 13;
        g->fillRect(CX + o - 2, 82, 5, 10, COLOR_WARN);
        g->fillRect(CX + o - 2, 160, 5, 10, COLOR_WARN);
        g->fillRect(CX - 44, 102 + i * 13 - 2 + 4, 10, 5, COLOR_WARN);
        g->fillRect(CX + 34, 102 + i * 13 - 2 + 4, 10, 5, COLOR_WARN);
    }
    ui_text_center(CX, 214, COLOR_TEXT, "FLASH MODE", 3);
    ui_text_center(CX, 262, COLOR_TEXT_DIM, "Ready for upload", 2);
    ui_text_center(CX, 296, COLOR_TEXT_DIM, "Arduino IDE / idf.py", 2);
    ui_text_center(CX, 340, COLOR_TEXT_DIM, "RESET or an upload", 2);
    ui_text_center(CX, 364, COLOR_TEXT_DIM, "brings the watch back", 2);
}

static void usb_create() {
    build_items();
    ui_scroll_reset(&s_scroll, s_content_h - LCD_HEIGHT, 0);
    s_back_armed = false;
    s_sheet = SH_NONE;
    s_swallow = false;
    s_prev_pressed = false;
    s_flash_at = 0;
}

static void usb_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_flash_at) { draw_flash_ready(g); return; }
    if (s_sheet != SH_NONE) { draw_sheet(g); return; }

    build_items();  // status/pending can change while the screen is open
    ui_scroll_set_max(&s_scroll, s_content_h - LCD_HEIGHT);
    int off = -ui_scroll_offset(&s_scroll);
    for (int i = 0; i < s_item_count; i++) {
        const Item &it = s_items[i];
        int y = it.y + off;
        if (y + it.h < 0 || y > LCD_HEIGHT) continue;
        switch (it.kind) {
        case IT_STATUS:      draw_status(g, y); break;
        case IT_MODE:        draw_mode_row(g, y, it.mode); break;
        case IT_RESTART:     draw_action(g, y, "Restart now", "Switch to the selected mode", COLOR_WARN); break;
        case IT_OPEN_REMOTE: draw_action(g, y, "Open remote", "Media buttons for the PC", COLOR_ACCENT); break;
        case IT_FLASH:       draw_action(g, y, "Flash mode", "Reboot into the ROM bootloader", COLOR_PANEL); break;
        }
    }
}

static void usb_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (s_flash_at) return;
    if (s_swallow) { if (!pressed) s_swallow = false; return; }

    if (s_sheet != SH_NONE) {
        if (!edge) return;
        int by = 330, bw = 132, bh = 56;
        if (y >= by - 10 && y <= by + bh + 10) {
            if (x >= CX - 8 - bw && x <= CX - 8) { s_sheet = SH_NONE; s_swallow = true; build_items(); }
            else if (x >= CX + 8 && x <= CX + 8 + bw) { s_swallow = true; sheet_confirm(); }
        }
        return;
    }

    if (pressed && !s_scroll.dragging) s_back_armed = (x < 54 && y < 54);
    if (s_back_armed) {
        if (!pressed) { s_back_armed = false; ui_pop_screen(); }
        return;
    }
    if (!ui_scroll_touch(&s_scroll, y, pressed)) return;

    int cy = y + ui_scroll_offset(&s_scroll);
    if (x < ROW_X || x > ROW_X + ROW_W) return;
    for (int i = 0; i < s_item_count; i++) {
        const Item &it = s_items[i];
        if (cy < it.y || cy > it.y + it.h) continue;
        switch (it.kind) {
        case IT_MODE:        choose_mode(it.mode); break;
        case IT_RESTART:     open_sheet(SH_RESTART, it.mode); break;
        case IT_OPEN_REMOTE: ui_push(&usb_remote_screen); break;
        case IT_FLASH:       open_sheet(SH_FLASH, USB_MODE_FIRMWARE); break;
        default: break;
        }
        break;
    }
}

static void usb_tick() {
    ui_scroll_tick(&s_scroll);
    if (s_flash_at && (int32_t)(millis() - s_flash_at) >= 0) usb_enter_download_mode();
}

static void usb_gesture(Gesture g) {
    if (s_flash_at) return;
    if (s_sheet != SH_NONE) { s_sheet = SH_NONE; build_items(); return; }
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen usb_mode_screen = {
    "USB Mode", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    usb_create, usb_draw, usb_touch, usb_tick, nullptr, usb_gesture,
    250,   // idle: 4 fps is plenty for the status card
};

// =========================================================================
//  Media remote - five round keys in a cross (volume on the vertical axis,
//  tracks on the horizontal, play/pause in the middle) plus mute, with the
//  screen-edge ring lighting up on the side that was pressed.
// =========================================================================

struct RKey { int x, y, r; uint16_t usage; float ring_deg; };
static const RKey R_KEYS[] = {
    { CX,       CY,       64, CC_PLAY_PAUSE, -1 },
    { CX - 132, CY,       46, CC_PREV,       270 },
    { CX + 132, CY,       46, CC_NEXT,       90 },
    { CX,       CY - 132, 46, CC_VOL_UP,     0 },
    { CX,       CY + 132, 46, CC_VOL_DOWN,   180 },
    { CX + 120, CY + 120, 30, CC_MUTE,       135 },
};
static const int R_KEY_COUNT = sizeof(R_KEYS) / sizeof(R_KEYS[0]);
static int      s_r_lit = -1;
static uint32_t s_r_lit_ms = 0;
static bool     s_r_prev = false;

static void draw_skip(Arduino_GFX *g, int cx, int cy, int dir, uint16_t c) {
    // two triangles + bar, mirrored by dir (+1 next, -1 prev)
    int s = 14;
    g->fillTriangle(cx - dir * s, cy - s, cx - dir * s, cy + s, cx, cy, c);
    g->fillTriangle(cx, cy - s, cx, cy + s, cx + dir * s, cy, c);
    int bx = dir > 0 ? cx + s : cx - s - 4;
    g->fillRect(bx, cy - s, 4, 2 * s, c);
}

static void remote_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    bool host = usb_host_connected();
    uint32_t now = millis();
    bool lit = s_r_lit >= 0 && now - s_r_lit_ms < 220;

    ui_edge_ring(0, 360, ui_dim(host ? COLOR_ACCENT : COLOR_TEXT_DIM, 0.25f), 8, 4, false);
    if (lit && R_KEYS[s_r_lit].ring_deg >= 0)
        ui_edge_ring(R_KEYS[s_r_lit].ring_deg - 30, 60, COLOR_ACCENT, 8, 4);
    else if (lit)
        ui_edge_ring(0, 360, COLOR_ACCENT, 8, 4, false);

    for (int i = 0; i < R_KEY_COUNT; i++) {
        const RKey &k = R_KEYS[i];
        bool on = lit && s_r_lit == i;
        uint16_t fill = i == 0 ? (on ? COLOR_TEXT : COLOR_ACCENT) : (on ? COLOR_ACCENT : COLOR_PANEL);
        uint16_t ink  = i == 0 ? (on ? COLOR_ACCENT : COLOR_TEXT) : COLOR_TEXT;
        g->fillCircle(k.x, k.y, k.r, fill);
        switch (k.usage) {
        case CC_PLAY_PAUSE:
            g->fillTriangle(k.x - 26, k.y - 20, k.x - 26, k.y + 20, k.x + 2, k.y, ink);
            g->fillRect(k.x + 10, k.y - 20, 7, 40, ink);
            g->fillRect(k.x + 22, k.y - 20, 7, 40, ink);
            break;
        case CC_PREV: draw_skip(g, k.x, k.y, -1, ink); break;
        case CC_NEXT: draw_skip(g, k.x, k.y, +1, ink); break;
        case CC_VOL_UP:
            g->fillRect(k.x - 14, k.y - 3, 28, 6, ink);
            g->fillRect(k.x - 3, k.y - 14, 6, 28, ink);
            break;
        case CC_VOL_DOWN:
            g->fillRect(k.x - 14, k.y - 3, 28, 6, ink);
            break;
        case CC_MUTE:
            ui_text_center(k.x, k.y, ink, "M", 2);
            break;
        }
    }
    ui_text_center(CX, CY - 132 + 64, COLOR_TEXT_DIM, "VOL", 1);
    ui_text_center(CX, 46, host ? COLOR_GOOD : COLOR_WARN, host ? "PC connected" : "Waiting for PC", 2);
    ui_text_center(CX, CY + 82, COLOR_TEXT_DIM, "USB media remote", 1);
}

static void remote_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_r_prev;
    s_r_prev = pressed;
    if (!edge) return;
    for (int i = 0; i < R_KEY_COUNT; i++) {
        const RKey &k = R_KEYS[i];
        int dx = x - k.x, dy = y - k.y, rr = k.r + 10;
        if (dx * dx + dy * dy > rr * rr) continue;
        s_r_lit = i;
        s_r_lit_ms = millis();
        if (!usb_remote_tap(k.usage)) {
            ui_show_toast(usb_mode_active() == USB_MODE_REMOTE ? "Connect the watch to a PC"
                                                               : "Pick Media remote in USB Mode");
        }
        return;
    }
}

static void remote_create() { s_r_lit = -1; s_r_prev = false; }

static void remote_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen usb_remote_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    remote_create, remote_draw, remote_touch, nullptr, nullptr, remote_gesture,
    250, false, false, false, false, true /* hide_status: the edge ring is ours */,
};
