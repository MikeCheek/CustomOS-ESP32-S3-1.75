# AmoledSmartWatchOS

A smartwatch-style firmware for the **Waveshare ESP32-S3-Touch-AMOLED-1.75-G**
(466x466 round AMOLED, CO5300 display driver, CST9217 touch, QMI8658 IMU,
PCF85063 RTC, AXP2101 PMU, TCA9554 IO expander, ES7210 mic array, LC76G GPS).

Watch face -> tap anywhere -> scrollable app grid with 7 sensor test
screens and 4 games.

## Before you flash - read this

I could not verify the exact GPIO numbers for this board against a
schematic, only the **signal names** (`LCD_CS`, `LCD_SDIO0-3`,
`IIC_SDA/SCL`, `TP_INT`, etc.) from Waveshare's public docs. Every pin
number in this project lives in **`board_pins.h`**, and every value in
there that needs checking is tagged `// VERIFY`.

To get the real numbers:
1. Go to Waveshare's docs page for this board -> Resources tab ->
   "ESP32-S3-Touch-AMOLED-1.75 Example (GitHub)".
2. Open the `01_HelloWorld` (or similar) Arduino example in that
   package - it has a pin-definition header (often named `Mylibrary.h`
   or `pins_config.h`).
3. Copy the real values into `board_pins.h`. Nothing else needs to
   change - every other file includes `board_pins.h` rather than
   hard-coding numbers.

This is a ~5 minute diff, not a rewrite, but it's a **required** step -
the values shipped here are my best-effort placeholders, not confirmed
hardware truth.

## Libraries you need

Install via Arduino Library Manager where possible; grab the rest from
Waveshare's example package (`Arduino/libraries/` folder in the zip) if
they're not on the Library Manager:

| Library | Used for | Source |
|---|---|---|
| Arduino_GFX (Waveshare fork) | CO5300 display driver | Waveshare demo package - the stock `moononournation/Arduino_GFX` may not have the `Arduino_CO5300` class yet, check |
| lvgl | UI framework | Library Manager (pin to v8.3.x, see note below) |
| SensorLib | QMI8658 + PCF85063 | Library Manager ("SensorLib" by lewisxhe) or demo package |
| XPowersLib | AXP2101 | Library Manager ("XPowersLib" by lewisxhe) or demo package |
| TinyGPSPlus | GPS NMEA parsing | Library Manager ("TinyGPSPlus" by Mikal Hart) |
| SD | microSD card | bundled with arduino-esp32 core |

**LVGL version**: this code targets the LVGL v8.3 API
(`lv_disp_drv_t`, `lv_indev_drv_t`, `lv_scr_load_anim`, etc.). Waveshare's
docs mention driver/LVGL versions are tightly coupled - if their demo
package pins a different major version, match that version rather than
whatever Library Manager offers by default, or you'll get compile
errors in `hal_display.cpp` / `hal_touch.cpp` (LVGL v9 renamed several
of these types).

**lv_conf.h**: not included here (it's a build-level config file, not
project code) - copy the one from Waveshare's demo package into your
Arduino `libraries` folder alongside lvgl, or generate one from
`lv_conf_template.h`. Make sure `LV_COLOR_DEPTH` is set to `16`.

## Arduino IDE settings

- Board: your ESP32-S3 variant for this board (check Waveshare's board
  definition instructions - it may need a specific "ESP32S3 Dev Module"
  config, not a dedicated board entry)
- USB CDC On Boot: **Enabled**
- PSRAM: **OPI PSRAM** (board has 8MB PSRAM, and the LVGL draw buffers
  in `hal_display.cpp` are allocated from it)
- Partition Scheme: pick one with enough app space for LVGL + your
  libraries (start with a default "Huge APP" scheme; go to a 16MB
  scheme only if you add image/font assets later)

## What's implemented

- **Watch face**: time/date (PCF85063), battery ring (AXP2101), live
  step count (QMI8658 peak-detection pedometer - good enough to prove
  the sensor works, not production-grade)
- **App menu**: scrollable grid, tap a tile to open
- **Sensor tests**: IMU (live accel/gyro + rolling chart), RTC
  (live clock + a "set test time" button to prove writes work), Power
  (battery voltage/%, charge/USB status), Touch (draws where you tap),
  GPS (fix status, lat/lon/speed/satellites - `-G` variant only), SD
  card (capacity, root listing, write/read round-trip test), Mic
  (live level meter + speaker beep button)
- **Games**: Snake (swipe to steer), Reaction speed test, Dodger
  (tilt to fly, dodge falling obstacles), Simon (color/tone memory
  game)
- **Settings**: brightness slider, about screen

## Known rough edges (by design, flagged rather than faked)

- **Audio** (`hal_audio.cpp`): the ES7210 register init is a
  best-effort default sequence, not verified against this exact
  board's firmware revision. If the mic level meter reads flat or the
  beep is silent/distorted, diff against Waveshare's `08_ES8311` demo
  and adjust `es7210_register_init()` / the I2S pin config.
- **Touch driver** (`hal_touch.cpp`): implements a generic CST9xx-style
  register read. If Waveshare's package ships a dedicated CST9217
  class, prefer that - it'll match the exact register map for your
  firmware revision instead of my generic reconstruction.
- **Time sync**: RTC has no NTP/BLE sync wired up yet - it just seeds a
  default if it looks unset. Wire up Wi-Fi + NTP or a companion-app
  time-set command if you want it to self-correct.
- **Step counter**: simple accel-magnitude peak detector, resets on
  reboot (no persistence). Fine as a "sensor works" demo, not a
  fitness-tracker-grade pedometer.

## Extending it

- New sensor test or game: create `app_test_foo.h/.cpp` (or
  `game_foo.h/.cpp`) following the pattern in any existing one -
  `ui_create_app_screen("Title")` for the container,
  `ui_get_current_app_screen()` for the return value - then add one
  line to the `MENU_ENTRIES[]` table and forward-declaration list in
  `app_menu.cpp`.
- New sensor/peripheral: add pins to `board_pins.h`, write
  `hal_foo.h/.cpp` following the existing HAL files' shape (`_init()`
  returns bool, a `_read()`/getter struct, everything gated behind a
  `FEATURE_FOO` flag in `config.h` if it's optional hardware).

## Rendering pipeline and UI building blocks

(Techniques borrowed from Meta's open-source MUSE firmware, which runs on
this same board, re-implemented for this project's Arduino_GFX renderer.)

- **Background panel sender** (`hal_display.cpp`): frames are drawn on
  core 1 into a pool of PSRAM framebuffers and handed to an `lcd_send`
  task on core 0, which sends them to the CO5300 while the next frame is
  drawn. Before sending, each frame is diffed against what the panel
  already shows, and only the changed rectangles (even-aligned, as the
  CO5300 requires) go over QSPI; an unchanged frame sends nothing. A
  newer frame replaces one still waiting, so the panel never lags behind.
  Brightness and sleep/wake also go through that task - nothing else may
  touch `display_gfx()` directly. A `[lcd]` line on the serial log every
  5 s shows fps, skipped/dropped frames and how much of the screen each
  frame actually sent.
- **Screen transitions** (`ui.cpp`): `ui_push()` slides the new screen in
  from the right, `ui_pop_screen()`/`ui_go_home()` slide the old one off
  to the right. On EDGE-mode screens, a drag from the left edge moves the
  screen with your finger, with the previous screen sliding in
  underneath; let go past a third of the way (or flick) to go back.
  Opt out per screen with the `no_transition` / `no_drag_back` fields at
  the end of `Screen`.
- **Kinetic lists** (`UiScroll` in `ui.h`, `ui_scroll.cpp`): drag, fling
  with momentum, rubber-band ends, settle on a whole row. Used by
  Settings and the WiFi network list.
- **T9 keypad** (`ui_keypad.h/.cpp`): a 12-key phone keypad sized for the
  round screen (multi-tap, hold for digit, 123/#+= modes, Show/Hide for
  passwords). Used by onboarding (name) and WiFi setup.

## Building and CI

- **Arduino IDE:** board "ESP32S3 Dev Module", core esp32 3.3.10, PSRAM "OPI PSRAM", Flash 16 MB, partition
  "16M Flash (3MB APP/9.9MB FATFS)", USB CDC On Boot "Enabled". Library versions are pinned in
  `scripts/arduino-libraries.txt`; `minimp3`, `libhelix` and `ESP_H264_Decoder` are not in the Library Manager and are
  vendored in `libraries/` (copy them to your Arduino `libraries` folder if the IDE can't find them).
- **arduino-cli:** `arduino-cli compile -b esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc --libraries libraries .`
- **Git + GitHub Actions:** run `scripts/init_repo.ps1 -Remote <url>` once. `.github/workflows/build.yml` then compiles the
  firmware and runs `flutter analyze` + a debug APK build on every push; a tag `vX.Y.Z` (matching `FW_VERSION` in
  `diag.h`) publishes a release with the `.bin` and the `.apk`.
