/*
 * pio/tinyusb_sdkconfig.h
 * Force-included into every file of the PlatformIO build (platformio.ini).
 *
 * The core rebuild for custom_sdkconfig has no arduino_tinyusb component
 * (it only exists in Espressif's lib-builder), so its Kconfig drops every
 * CONFIG_TINYUSB_* from the new sdkconfig.h. The rebuild only replaces the
 * archives it compiled, though: the stock prebuilt libarduino_tinyusb.a and
 * its headers are still in the package and on the link line. Restoring the
 * settings it was built with - exactly the stock esp32s3 values below -
 * brings back the core's USB library (USB File storage / Media remote).
 */
#pragma once
#ifndef CONFIG_TINYUSB_ENABLED
#define CONFIG_TINYUSB_ENABLED 1
#define CONFIG_TINYUSB_CDC_ENABLED 1
#define CONFIG_TINYUSB_DESC_CDC_STRING "Espressif CDC Device"
#define CONFIG_TINYUSB_CDC_RX_BUFSIZE 64
#define CONFIG_TINYUSB_CDC_TX_BUFSIZE 64
#define CONFIG_TINYUSB_CDC_MAX_PORTS 2
#define CONFIG_TINYUSB_MSC_ENABLED 1
#define CONFIG_TINYUSB_DESC_MSC_STRING "Espressif MSC Device"
#define CONFIG_TINYUSB_MSC_BUFSIZE 4096
#define CONFIG_TINYUSB_HID_ENABLED 1
#define CONFIG_TINYUSB_DESC_HID_STRING "Espressif HID Device"
#define CONFIG_TINYUSB_HID_BUFSIZE 64
#define CONFIG_TINYUSB_MIDI_ENABLED 1
#define CONFIG_TINYUSB_DESC_MIDI_STRING "Espressif MIDI Device"
#define CONFIG_TINYUSB_MIDI_RX_BUFSIZE 64
#define CONFIG_TINYUSB_MIDI_TX_BUFSIZE 64
#define CONFIG_TINYUSB_AUDIO_ENABLED 1
#define CONFIG_TINYUSB_DESC_AUDIO_STRING "Espressif AUDIO Device"
#define CONFIG_TINYUSB_VIDEO_ENABLED 1
#define CONFIG_TINYUSB_DESC_VIDEO_STRING "Espressif VIDEO Device"
#define CONFIG_TINYUSB_VIDEO_STREAMING_BUFSIZE 64
#define CONFIG_TINYUSB_VIDEO_STREAMING_IFS 1
#define CONFIG_TINYUSB_DFU_RT_ENABLED 1
#define CONFIG_TINYUSB_DESC_DFU_RT_STRING "Espressif DFU_RT Device"
#define CONFIG_TINYUSB_DFU_ENABLED 1
#define CONFIG_TINYUSB_DESC_DFU_STRING "Espressif DFU Device"
#define CONFIG_TINYUSB_DFU_BUFSIZE 4096
#define CONFIG_TINYUSB_VENDOR_ENABLED 1
#define CONFIG_TINYUSB_DESC_VENDOR_STRING "Espressif VENDOR Device"
#define CONFIG_TINYUSB_VENDOR_RX_BUFSIZE 64
#define CONFIG_TINYUSB_VENDOR_TX_BUFSIZE 64
#define CONFIG_TINYUSB_NCM_ENABLED 1
#define CONFIG_TINYUSB_DEBUG_LEVEL 0
#endif
