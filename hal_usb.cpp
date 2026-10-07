#include "hal_usb.h"
#include "config.h"
#include "app_settings_state.h"
#include "hal_nvs.h"
#include "hal_sd.h"
#include "hal_power.h"
#include "ui.h"

#include <Arduino.h>
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "esp_system.h"

#if FEATURE_USB_OTG_MODES
#include <USB.h>
#include <USBMSC.h>
#include <USBHIDConsumerControl.h>
#endif
#if FEATURE_SD_CARD
#include <SD.h>
#endif

// ARDUINO_USB_MODE: 1 = "Hardware CDC and JTAG" build (Serial is the
// fixed-function USB-Serial/JTAG, TinyUSB unused until we start it),
// 0 = "USB-OTG (TinyUSB)" build (the core already started TinyUSB).
#ifndef ARDUINO_USB_MODE
#define ARDUINO_USB_MODE 1
#endif
#define USB_RUNTIME_OTG_OK (FEATURE_USB_OTG_MODES && ARDUINO_USB_MODE)

#if !ARDUINO_USB_MODE
extern "C" bool tud_connect(void);
extern "C" bool tud_disconnect(void);
#include "esp32-hal-tinyusb.h"
#endif

static UsbMode  s_active = USB_MODE_FIRMWARE;
static bool     s_cable = false;
static uint32_t s_cable_poll_ms = 0;
static bool     s_otg_started = false;   // TinyUSB owns the PHY now

static inline bool is_otg_mode(UsbMode m) { return m == USB_MODE_STORAGE || m == USB_MODE_REMOTE; }

// ---- USB-Serial/JTAG pad control ------------------------------------------
// USB_PAD_ENABLE connects the Serial/JTAG controller (and its D+ pull-up)
// to the pins. Clearing it makes the watch vanish from the PC exactly as
// if the data lines were cut - charging (VBUS -> AXP2101) is unaffected.
static void usj_pad(bool on) {
    if (on) SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
    else    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
}

// Hands the PHY back from USB-OTG to USB-Serial/JTAG (same register
// sequence as the core's usb_switch_to_cdc_jtag(), which is static) and
// drops the pull-up long enough for the PC to see a disconnect, so the
// next enumeration - the ROM bootloader's or the next boot's - starts
// clean. RTC_CNTL_USB_CONF lives in the RTC domain and survives
// esp_restart(), so without this a restart would come back up with the
// PHY still routed to the (now idle) OTG controller: no serial, no
// auto-reset upload.
static void phy_back_to_usj() {
    usj_pad(false);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
    delay(250);   // > host attach debounce (100 ms)
    usj_pad(true);
}

// ---- File storage (MSC) ---------------------------------------------------
#if USB_RUNTIME_OTG_OK
static USBMSC  *s_msc = nullptr;
static uint32_t s_msc_sectors = 0;
static volatile bool     s_msc_ejected = false;
static volatile uint32_t s_msc_activity_ms = 0;
static bool     s_msc_was_active = false;
static bool     s_was_mounted = false;
static const uint16_t SECTOR_SIZE = 512;

// SD.readRAW()/writeRAW() move one 512-byte sector per call; hosts ask
// for several at once, so loop.
static int32_t msc_on_write(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)offset;
    s_msc_activity_ms = millis();
    for (uint32_t i = 0; i < bufsize / SECTOR_SIZE; i++)
        if (!SD.writeRAW(buffer + i * SECTOR_SIZE, lba + i)) return -1;
    return bufsize;
}

static int32_t msc_on_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)offset;
    s_msc_activity_ms = millis();
    uint8_t *buf = (uint8_t *)buffer;
    for (uint32_t i = 0; i < bufsize / SECTOR_SIZE; i++)
        if (!SD.readRAW(buf + i * SECTOR_SIZE, lba + i)) return -1;
    return bufsize;
}

// Not every host sends START on mount (Windows usually doesn't), so the
// drive counts as "in use" from enumeration until an eject - only the
// eject is acted on here.
static bool msc_on_start_stop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    if (!start && load_eject) s_msc_ejected = true;
    return true;
}

static bool start_storage() {
    if (!sd_is_mounted()) return false;
    s_msc_sectors = SD.numSectors();
    if (s_msc_sectors == 0) return false;
    s_msc = new USBMSC();   // constructing registers the MSC interface - only in this mode
    s_msc->vendorID("Waveshare");
    s_msc->productID("Watch SD");
    s_msc->productRevision("1.0");
    s_msc->onRead(msc_on_read);
    s_msc->onWrite(msc_on_write);
    s_msc->onStartStop(msc_on_start_stop);
    s_msc->mediaPresent(true);
    if (!s_msc->begin(s_msc_sectors, SECTOR_SIZE)) {
        DEBUG_PRINTF("[usb] MSC begin failed\n");
        return false;
    }
    USB.manufacturerName("Waveshare");
    USB.productName("AMOLED Watch (SD card)");
    DEBUG_PRINTF("[usb] file storage: %lu sectors (%lu MB) - serial monitor ends here\n",
                 (unsigned long)s_msc_sectors, (unsigned long)(s_msc_sectors / 2048));
    Serial.flush();
    s_otg_started = USB.begin();
    return s_otg_started;
}
#endif

// ---- Media remote (HID consumer control) ----------------------------------
#if USB_RUNTIME_OTG_OK
static USBHIDConsumerControl *s_cc = nullptr;

static bool start_remote() {
    s_cc = new USBHIDConsumerControl();  // registers the HID interface - only in this mode
    s_cc->begin();
    USB.manufacturerName("Waveshare");
    USB.productName("AMOLED Watch Remote");
    DEBUG_PRINTF("[usb] media remote - serial monitor ends here\n");
    Serial.flush();
    s_otg_started = USB.begin();
    return s_otg_started;
}
#endif

// ---------------------------------------------------------------------------

static bool apply_live(UsbMode m) {
    switch (m) {
    case USB_MODE_CHARGE_ONLY:
#if ARDUINO_USB_MODE
        usj_pad(false);
#else
        tud_disconnect();
#endif
        return true;
    case USB_MODE_FIRMWARE:
#if ARDUINO_USB_MODE
        usj_pad(true);
#else
        tud_connect();
#endif
        return true;
#if USB_RUNTIME_OTG_OK
    case USB_MODE_STORAGE: return start_storage();
    case USB_MODE_REMOTE:  return start_remote();
#endif
    default: return false;
    }
}

void usb_mode_init() {
    UsbMode want = (UsbMode)g_app_settings.usb_mode;
    if (want >= USB_MODE_COUNT) want = USB_MODE_FIRMWARE;
    s_cable = power_vbus_present();
    if (want == USB_MODE_FIRMWARE) { s_active = want; return; }  // nothing to do - the boot default
#if !USB_RUNTIME_OTG_OK
    if (is_otg_mode(want)) {
        DEBUG_PRINTF("[usb] %s needs a \"Hardware CDC and JTAG\" build - staying in Firmware mode\n",
                     usb_mode_name(want));
        s_active = USB_MODE_FIRMWARE;
        return;
    }
#endif
    if (apply_live(want)) {
        s_active = want;
    } else {
        DEBUG_PRINTF("[usb] %s unavailable at boot (no SD card?) - Firmware mode\n", usb_mode_name(want));
        s_active = USB_MODE_FIRMWARE;
    }
}

void usb_mode_update() {
    uint32_t now = millis();
    if (now - s_cable_poll_ms < 500) return;
    s_cable_poll_ms = now;
    s_cable = power_vbus_present();

#if USB_RUNTIME_OTG_OK
    if (s_active == USB_MODE_STORAGE && s_msc) {
        bool mounted = (bool)USB;
        if (mounted && !s_was_mounted && s_msc_ejected) {
            // Re-plugged after an eject: offer the card to the PC again.
            s_msc_ejected = false;
        }
        s_msc->mediaPresent(!s_msc_ejected);
        s_was_mounted = mounted;

        bool active = usb_msc_host_active();
        if (active && !s_msc_was_active) {
            ui_show_toast("SD card is in use by the PC");
        } else if (!active && s_msc_was_active) {
            // The PC is done (eject or unplug): drop the watch's stale
            // FAT cache so it sees whatever the PC changed.
            sd_remount();
            ui_show_toast(s_cable ? "Drive ejected - SD back on watch" : "USB unplugged - SD back on watch");
        }
        s_msc_was_active = active;
    }
#endif
}

UsbMode usb_mode_active() { return s_active; }
UsbMode usb_mode_saved() {
    UsbMode m = (UsbMode)g_app_settings.usb_mode;
    return m < USB_MODE_COUNT ? m : USB_MODE_FIRMWARE;
}
bool usb_mode_restart_pending() { return usb_mode_saved() != s_active; }

const char *usb_mode_name(UsbMode m) {
    switch (m) {
    case USB_MODE_CHARGE_ONLY: return "Charging only";
    case USB_MODE_FIRMWARE:    return "Firmware & Debug";
    case USB_MODE_STORAGE:     return "File storage";
    case USB_MODE_REMOTE:      return "Media remote";
    default:                   return "?";
    }
}

bool usb_mode_supported(UsbMode m) {
#if USB_RUNTIME_OTG_OK
    return m < USB_MODE_COUNT;
#else
    return m < USB_MODE_COUNT && !is_otg_mode(m);
#endif
}

bool usb_cable_present() { return s_cable; }

bool usb_host_connected() {
    if (!s_cable) return false;
    switch (s_active) {
    case USB_MODE_CHARGE_ONLY: return false;
    case USB_MODE_FIRMWARE:
#if ARDUINO_USB_MODE
        return HWCDC::isPlugged();
#else
        return (bool)USB;
#endif
#if USB_RUNTIME_OTG_OK
    case USB_MODE_STORAGE:
    case USB_MODE_REMOTE: return (bool)USB;
#endif
    default: return false;
    }
}

const char *usb_mode_status_text() {
    if (!s_cable) return "No cable connected";
    switch (s_active) {
    case USB_MODE_CHARGE_ONLY: return "Charging - no data link";
    case USB_MODE_FIRMWARE:    return usb_host_connected() ? "PC connected - ready to flash" : "Power only - no PC seen";
#if USB_RUNTIME_OTG_OK
    case USB_MODE_STORAGE:
        if (s_msc_ejected) return "Ejected - replug to mount again";
        return usb_msc_host_active() ? "SD card mounted on the PC" : "Waiting for the PC...";
    case USB_MODE_REMOTE:      return usb_host_connected() ? "PC connected - remote ready" : "Waiting for the PC...";
#endif
    default: return "";
    }
}

bool usb_mode_switch_needs_restart(UsbMode m) {
    if (m == s_active) return false;
    // TinyUSB can't be stopped at runtime: leaving an OTG mode means a
    // restart, entering one from Charging/Firmware doesn't.
    return is_otg_mode(s_active);
}

UsbSwitchResult usb_mode_select(UsbMode m) {
    if (m >= USB_MODE_COUNT) return USB_SWITCH_FAILED;
#if !USB_RUNTIME_OTG_OK
    if (is_otg_mode(m)) return USB_SWITCH_UNSUPPORTED;
#endif
    if (m == USB_MODE_STORAGE && !sd_is_mounted()) return USB_SWITCH_NO_SD;

    g_app_settings.usb_mode = (uint8_t)m;
    nvs_save_settings(g_app_settings);

    if (m == s_active) return USB_SWITCH_DONE;   // e.g. undoing a pending change
    if (usb_mode_switch_needs_restart(m)) return USB_SWITCH_NEEDS_RESTART;
    if (!apply_live(m)) {
        g_app_settings.usb_mode = (uint8_t)s_active;
        nvs_save_settings(g_app_settings);
        return USB_SWITCH_FAILED;
    }
    s_active = m;
    return USB_SWITCH_DONE;
}

void usb_mode_restart() {
    DEBUG_PRINTF("[usb] restarting into %s\n", usb_mode_name(usb_mode_saved()));
#if FEATURE_SD_CARD
    SD.end();
#endif
    if (s_otg_started) phy_back_to_usj();
    esp_restart();
}

void usb_enter_download_mode() {
    DEBUG_PRINTF("[usb] rebooting into ROM download mode\n");
    Serial.flush();
#if FEATURE_SD_CARD
    SD.end();
#endif
#if ARDUINO_USB_MODE
    // ROM bootloader talks over USB-Serial/JTAG: make sure the PHY and
    // pads are back on it (OTG modes / Charging only), then latch the
    // force-download strap override and reset.
    if (s_otg_started || s_active == USB_MODE_CHARGE_ONLY) phy_back_to_usj();
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
#else
    usb_persist_restart(RESTART_BOOTLOADER);
#endif
}

bool usb_mode_blocks_light_sleep() {
    // Automatic light sleep gates the USB controllers' clocks: an upload
    // or serial session would stall, a drive transfer would time out.
    // So: no light sleep while a data mode has a cable in.
    return s_cable && s_active != USB_MODE_CHARGE_ONLY;
}

bool usb_msc_host_active() {
#if USB_RUNTIME_OTG_OK
    return s_active == USB_MODE_STORAGE && s_msc && !s_msc_ejected && (bool)USB;
#else
    return false;
#endif
}

uint32_t usb_msc_activity_ms() {
#if USB_RUNTIME_OTG_OK
    return s_msc_activity_ms;
#else
    return 0;
#endif
}

bool usb_remote_tap(uint16_t usage) {
#if USB_RUNTIME_OTG_OK
    if (s_active != USB_MODE_REMOTE || !s_cc || !(bool)USB) return false;
    s_cc->press(usage);
    s_cc->release();
    return true;
#else
    (void)usage;
    return false;
#endif
}
