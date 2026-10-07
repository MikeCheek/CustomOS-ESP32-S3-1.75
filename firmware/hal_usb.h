/*
 * hal_usb.h
 * USB connection modes - what the watch looks like to a PC on the cable.
 *
 *   Charging only     - no data at all: the PC doesn't see a device, the
 *                       battery just charges. Also stops a serial monitor
 *                       left open on the PC from resetting the watch.
 *   Firmware & Debug  - (default) the ESP32-S3's built-in USB-Serial/JTAG:
 *                       serial monitor, one-click upload from the Arduino
 *                       IDE / idf.py / esptool (auto-reset), JTAG debugging.
 *                       Auto light sleep is held off while the cable is in,
 *                       so uploads never hit a sleeping chip.
 *   File storage      - the SD card shows up on the PC as a USB drive
 *                       (TinyUSB mass storage).
 *   Media remote      - the watch is a USB media keyboard: play/pause,
 *                       next/previous, volume and mute for the PC.
 *
 * Plus a one-shot action: reboot straight into the ROM download (flash)
 * mode, for when the running firmware can't be auto-reset by the IDE.
 *
 * How the switching works: the S3 has two USB controllers sharing one
 * PHY - USB-Serial/JTAG (fixed function, what "Hardware CDC and JTAG"
 * builds use) and USB-OTG (TinyUSB, programmable). Starting TinyUSB at
 * runtime moves the PHY over to OTG, which is how File storage and Media
 * remote are entered without a restart, from an ordinary "Hardware CDC
 * and JTAG" build. Moving back to Serial/JTAG is done with a restart
 * (TinyUSB can't be torn down cleanly at runtime); the chosen mode is
 * saved, so the watch boots straight into it.
 *
 * Build with Tools > USB Mode > "Hardware CDC and JTAG" (the board
 * default). With "USB-OTG (TinyUSB)" the core starts TinyUSB itself at
 * boot, before this module can add its interfaces, so only Charging only
 * and Firmware & Debug are available in that build.
 */

#pragma once
#include <stdint.h>

enum UsbMode : uint8_t {
    USB_MODE_CHARGE_ONLY = 0,
    USB_MODE_FIRMWARE    = 1,
    USB_MODE_STORAGE     = 2,
    USB_MODE_REMOTE      = 3,
    USB_MODE_COUNT
};

enum UsbSwitchResult : uint8_t {
    USB_SWITCH_DONE,            // running in the new mode now
    USB_SWITCH_NEEDS_RESTART,   // saved; takes effect after usb_mode_restart()
    USB_SWITCH_NO_SD,           // File storage without a mounted SD card
    USB_SWITCH_UNSUPPORTED,     // not possible in this build (see header note)
    USB_SWITCH_FAILED,
};

// setup(): after nvs_load_settings() and sd_init().
void usb_mode_init();
// loop(): cheap; polls the cable (VBUS) twice a second and handles the
// PC ejecting the drive (remounts the SD card for the watch's own apps).
void usb_mode_update();

UsbMode usb_mode_active();          // what's running right now
UsbMode usb_mode_saved();           // what the user picked (= active unless a restart is pending)
bool    usb_mode_restart_pending();
const char *usb_mode_name(UsbMode m);
bool usb_mode_supported(UsbMode m); // false: needs FEATURE_USB_OTG_MODES + a HW CDC build
const char *usb_mode_status_text(); // one line for the UI ("PC connected", ...)

// True if `m` needs a restart to be entered from the active mode.
bool usb_mode_switch_needs_restart(UsbMode m);
// Saves `m` and applies it live when possible.
UsbSwitchResult usb_mode_select(UsbMode m);
// Restarts into the saved mode (PHY handed back to Serial/JTAG first).
void usb_mode_restart();
// Reboots into the ROM serial bootloader ("download mode") - never returns.
void usb_enter_download_mode();

bool usb_cable_present();           // VBUS present (any mode)
bool usb_host_connected();          // a PC has enumerated the device
bool usb_mode_blocks_light_sleep(); // see hal_power.cpp

// File storage: true while a PC has the drive mounted (between the host's
// "start" and "eject" SCSI commands). The watch's own SD writers check it
// and refuse while true - two writers on one FAT volume corrupt it.
bool usb_msc_host_active();
uint32_t usb_msc_activity_ms();     // millis() of the last sector read/write

// Media remote: tap a consumer-control usage (CONSUMER_CONTROL_* from
// USBHIDConsumerControl.h). false if not in remote mode / no PC.
bool usb_remote_tap(uint16_t usage);
