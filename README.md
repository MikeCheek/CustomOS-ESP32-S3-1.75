# AmoledSmartWatchOS

A full smartwatch OS for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466 round AMOLED), with an Android companion app.
Watch faces, notifications, calls, music, maps, calendar, voice memos with on-device AI, games, and over-the-air updates.
Built on Arduino + Arduino_GFX, no LVGL.

[![build](https://github.com/MikeCheek/CustomOS-ESP32-S3-1.75/actions/workflows/build.yml/badge.svg)](https://github.com/MikeCheek/CustomOS-ESP32-S3-1.75/actions/workflows/build.yml)

![AmoledSmartWatchOS screens](docs/screenshots/hero.png)

## Screenshots

| | | | |
|:-:|:-:|:-:|:-:|
| ![](docs/screenshots/watchface.png) | ![](docs/screenshots/watchface_minimal.png) | ![](docs/screenshots/menu.png) | ![](docs/screenshots/quick_panel.png) |
| Watch face + complications | Minimal face | App menu | Quick panel |
| ![](docs/screenshots/notifications.png) | ![](docs/screenshots/call.png) | ![](docs/screenshots/music.png) | ![](docs/screenshots/navigation.png) |
| Notifications | Incoming call | Phone's music | Turn-by-turn |
| ![](docs/screenshots/calendar.png) | ![](docs/screenshots/recorder_live.png) | ![](docs/screenshots/game_flappy.png) | ![](docs/screenshots/game_fruitninja.png) |
| Calendar | Voice recorder | Flappy | Fruit Slice |
| ![](docs/screenshots/settings.png) | ![](docs/screenshots/battery.png) | ![](docs/screenshots/update_prompt.png) | ![](docs/screenshots/software_update.png) |
| Settings | Battery | Update found on Wi-Fi | Software update |
| ![](docs/screenshots/aod.png) | ![](docs/screenshots/piano_menu.png) | ![](docs/screenshots/piano_pick.png) | ![](docs/screenshots/piano_play.png) |
| Always-on display | Piano Tiles: genres | Song and difficulty | Playing |
| ![](docs/screenshots/assistant_listen.png) | ![](docs/screenshots/assistant_answer.png) | ![](docs/screenshots/fall_alert.png) | ![](docs/screenshots/sleep_app.png) |
| Assistant listening | Assistant answer | Fall detection | Sleep and smart alarm |
| ![](docs/screenshots/draw_open.png) | ![](docs/screenshots/activity.png) | ![](docs/screenshots/smart_stack.png) | ![](docs/screenshots/face_switcher.png) |
| Draw to open | Activity | Smart Stack | Face switcher |
| ![](docs/screenshots/menu_folder.png) | ![](docs/screenshots/activity_rings.png) | ![](docs/screenshots/rings_closed.png) | ![](docs/screenshots/workout_live.png) |
| The Games folder | Activity rings | A ring closed | Workout |
| ![](docs/screenshots/timer_running.png) | ![](docs/screenshots/stopwatch.png) | ![](docs/screenshots/alarms.png) | ![](docs/screenshots/world_clock.png) |
| Timer | Stopwatch with laps | Alarms | World clock |
| ![](docs/screenshots/weather.png) | ![](docs/screenshots/reminders.png) | ![](docs/screenshots/workout_types.png) | |
| Weather | Reminders (memo to-dos) | Workout types | |
| ![](docs/screenshots/notifications_grouped.png) | ![](docs/screenshots/notifications_swipe.png) | ![](docs/screenshots/power_sos.png) | ![](docs/screenshots/sos_countdown.png) |
| Notifications stacked by app | Swipe left to clear | Hold PWR: power off or SOS | Emergency SOS |
| ![](docs/screenshots/life.png) | ![](docs/screenshots/life_meals.png) | ![](docs/screenshots/breathe.png) | ![](docs/screenshots/stretch.png) |
| Life: today's habits | Meal ideas | Breathe | Stretch break |
| ![](docs/screenshots/tips_swipe.png) | ![](docs/screenshots/tips_hold.png) | ![](docs/screenshots/tips_button.png) | |
| Tips: swipes | Tips: touch and hold | Tips: buttons | |

### Getting around

Every gesture on the face can be changed - on the watch (Settings > Home gestures) or in the app (Faces > Gestures and Tiles), one by one or with a style: Standard, Apple-like, Wear OS-like, Galaxy-like or Classic. Standard:

| On the watch face | |
|---|---|
| Tap | Apps: one honeycomb, bigger in the middle, the name of the app in the middle at the bottom; games (and developer tools) in folders; tap to zoom an app open (Settings > Display > Apps as a list for A-Z) |
| Double-tap | Play / pause the phone's music |
| Swipe up | Smart Stack: a workout or timer running, music playing, the next event or alarm, weather, the rings, reminders, messages, battery |
| Swipe in from the right | Tiles: full-screen pages to flick through - Activity, Weather, Up next, Music with its buttons, Timer (1/3/5/10 min in a tap), Life, World clock, Alarm, Reminders, Battery; which and in what order is set in the app |
| Swipe in from the left | Notifications |
| Pull down, or BOOT | Control Center (the quick panel) |
| Touch and hold | Face switcher: swipe through the faces, tap to wear one, Edit for its complications |

| | | | |
|:-:|:-:|:-:|:-:|
| ![](docs/screenshots/tile_activity.png) | ![](docs/screenshots/tile_music.png) | ![](docs/screenshots/tile_timer.png) | ![](docs/screenshots/settings_home.png) |
| Tiles: Activity | Tiles: Music | Tiles: Timer | Settings > Home gestures |

Settings > System > Gesture tips (also shown once after an update) walks through all of these with little animations. Hold PWR for power off or Emergency SOS (5 s countdown, then the phone texts your emergency contact with its location). A red dot at the top of a face means unread notifications. Elsewhere BOOT is back (hold: home, twice: the assistant). Tests and diagnostics are under Settings > System > Developer mode.

### In motion

| | | | | |
|:-:|:-:|:-:|:-:|:-:|
| ![](docs/screenshots/anim_boot.gif) | ![](docs/screenshots/anim_unlock.gif) | ![](docs/screenshots/anim_lock.gif) | ![](docs/screenshots/anim_music.gif) | ![](docs/screenshots/anim_charging.gif) |
| Power on | Unlock | Lock | 3D spectrum ring | Charging |

<sub>Rendered from the real UI code by the [host simulator](tools/watch_sim) - what you see is what the watch draws.</sub>

## On the watch

- **Faces** - Default, Minimal and custom faces designed in the app; complication slots for steps, next event,
  weather, battery, phone battery, messages, next turn, memo to-dos and seconds. A little pet lives on the face.
- **Photo face with depth** - pick a photo in the app (Photo face): the phone finds its subject (Google ML
  Kit subject segmentation, on the phone) and the watch draws the time behind it, so a person, pet or building
  stands in front of the clock. Framing, time position, size, weight and colour in the app; the photo, the
  subject mask and the digits go to the SD card as one file (`firmware/app_photo_watchface.h`). Up to 20
  photos: tap the face for the next one, or let it change every hour or every day; the app lists, shows and
  deletes them
- **Smooth UI** - slide transitions, edge-swipe back, kinetic lists, a pull-down quick panel (brightness,
  volume, radios, DND, torch, battery modes), smooth fonts with accents, auto-rotate.
- **3D effects** - software-rendered on the watch: a particle power-on sequence, lock/unlock that tilt the real
  screen into the distance and switch it off like a CRT, a 3D spectrum ring round the music controls, and a
  spinning 3D bolt when charging starts.
- **Phone link** - notifications with app icons and quick and voice replies, incoming calls (answer/decline), music remote with album art,
  Google Maps turn-by-turn, calendar with offline reminders, weather, Do Not Disturb synced both ways, find my phone.
- **Smart replies** - for each message you can answer, the on-phone LLM writes three replies; the watch
  shows them first, before your quick replies and voice dictation.
- **Catch me up and translate** - on the watch's Notifications (or the app's Messages): the on-phone LLM sums up
  the last day's notifications, what needs an answer first, and translates a message into your language.
- **Watch emulator** - the app runs the watch's own firmware code (screens, apps, games) on the phone:
  swipe and tap it like the watch, BOOT and PWR buttons below (Android; `tools/watch_sim/emu_main.cpp`).
  With no real watch connected it's linked to the app like one: notifications, music, weather, assistant
  answers, smart replies and Catch me up reach it; its settings are kept between runs. The phone's
  accelerometer is the watch's (tilt games, fall detection, sleep) and its beeps, effects and piano play here.
  Its SD card is a folder in the app; it records with the phone's microphone (recorder, assistant, voice replies).
  "Keep it as my watch" keeps it running and linked in the background and after a restart, until a real
  watch connects.
- **Look** - a photo, and the on-phone vision model (FastVLM or Gemma 3n) says what it is, reads its text or
  translates it; the answer also shows on the watch and can be read aloud.
- **Sound alerts** - the phone listens and the watch shows what it heard: doorbell or knocking, smoke alarm,
  baby crying, alarm clock, phone, siren, dog, glass breaking (sherpa-onnx audio tagging with CED, on the phone).
- **Everything from the phone too** - the app has the same Assistant (answers out loud with the installed
  voice), Messages (catch me up, translate, smart replies you can send), voice memos recorded on the phone,
  who-said-what and Piano Tiles songs.
- **Assistant** - ask out loud (app menu, or press BOOT twice): timers on the watch, reminders and alarms into
  your calendar, music, weather, the agenda, Do Not Disturb, finding the phone, questions about your memos, or
  anything else for the on-phone language model. Whisper and the LLM run on the phone, nothing goes online.
  With a Piper voice installed (English, Italian) it also says the answer on the watch speaker.
- **AI models** (app: Apps or Settings > AI models) - everything runs on the phone, and every model is a
  download you choose: transcription (sherpa-onnx Whisper, Moonshine and SenseVoice from GitHub, or whisper.cpp),
  summaries and answers (Qwen3, Gemma), who said what (four speaker models), search by meaning, 17 Piper voices in
  12 languages, Look (FastVLM, Gemma 3n) and sound alerts (three CED sizes). Downloads go through the system, so
  they keep going with the app in the background or closed, resume where they stopped, retry, show a notification
  and try mirrors for Hugging Face files (your own mirror, then hf-mirror.com); one that finishes while the app
  is closed is set up when it next opens. **Set up in one go** downloads a pack - Essentials, Without Hugging
  Face, Everything, Light - or your own choice, and makes each the model its feature uses
- **Wake word** - say "Alexa", "Jarvis", "Hi ESP"... and the assistant opens (Espressif ESP-SR WakeNet on both
  microphones, models on the SD card, installed from the app: AI models > Wake word). Off by default;
  Settings > Sound > Voice turns it on, picks the word and whether it also listens with the screen off. The same
  page has **Voice cleanup**: noise suppression and auto gain on assistant questions and voice replies, and
  **Skip silences**: the Recorder leaves out long pauses
- **Offline voice commands** - with the command model installed too (MultiNet7 English, 2.7 MB, same app
  page), the wake word listens a few seconds for a command and the watch runs it on its own, no phone or
  network: "what time is it", "next song", "pause music", "volume up", "set a timer for five minutes",
  "cancel the timer", "find my phone", "turn on do not disturb", "open settings", "show notifications",
  "open the recorder", "go home"... Anything else is a question: what you said goes to the phone's assistant
  as it is (any language Whisper knows), so you don't say it twice. The phrases and their phonemes are in
  `tools/voice_cmd_phrases.py` (Settings > Sound > Voice > Commands)
- **Apps** - Recorder, Music and Media player (MP3/AAC, video), Gallery, Files, Contacts, Phone, Battery,
  3D printer status (Bambu), sensor tests.
- **Games** - Piano Tiles (42 public-domain songs in four genres, timing judged against a ring for
  accuracy-based stars, combos and hold bonuses, Easy / Normal / Hard on any song, more from the SD card or
  your own: hum a tune or pick a MIDI file in the app's Piano Tiles songs, or `tools/midi_to_piano.py`), Flappy, Fruit Slice, Breakout, Snake, Runner, Crystal Cavern, Maze Raider, Ninja Dungeon, Blackjack,
  Simon, Dodger, Plane, Reaction - played by touch, tilt, the phone as a gamepad, or an ESP-NOW joystick.
- **Health** - hardware pedometer with daily history, GPS (on the `-G` board); fall detection (free fall, impact, lying still: "Did you fall?" for 30 s, then the phone rings
  and texts your emergency contact with the location).
- **Draw to open** - write a letter with one stroke (A assistant, C calendar, M music, N notifications, P phone,
  S settings, V recorder, Z sleep): the $1 unistroke recognizer, on the watch.
- **Activity** - still / walking / running / moving from the accelerometer's rhythm, minutes of each today, a
  rep counter for exercises, and "flick to go back" (two quick wrist flicks; Settings > Wake & quiet).
- **Sleep** - tracked from wrist movement at night (the arm-angle method of van Hees et al.): time asleep, how
  much of it was still, minutes awake, the night as a chart; a smart alarm rings up to 30 minutes early when
  you stir, at the set time at the latest.
- **Power** - light sleep with the phone still connected, an always-on display, battery modes
  (Balanced / Saver / Ultra), wake on wrist raise or tap, auto-dim.
- **System** - encrypted pairing, crash reports, signed firmware updates over Bluetooth or Wi-Fi with automatic
  rollback, USB modes, guided first-boot setup.

## In the companion app (Android)

- Pair, reconnect automatically and keep the link alive in the background; home-screen widget.
- Forward notifications (per app), calls, media, calendar, navigation and weather to the watch.
- **Voice memos** - sync recordings (Bluetooth or fast Wi-Fi), or record one on the spot with the phone's mic
  (it's copied to the watch too); transcribe with Whisper and summarize with Gemma / Qwen, all on the phone. Topics, to-dos, people, insights, search and "Ask AI" over your memos - also by meaning
  with an on-phone embedding model (EmbeddingGemma), so "the trip" finds "holiday in Sicily". Conversations are
  split by speaker ("who said what", sherpa-onnx on the phone; name the voices, tap a line to play from there).
- Watch face designer and library, complication slots, quick replies, DND and bedtime.
- Activity history, contacts and files to the watch, the phone as a gamepad, watch Wi-Fi setup, diagnostics and crash reports.
- **Updates** - checks GitHub for a new app and watch firmware, and installs both.

## Getting started

1. **Flash the watch once over USB** - build and upload from the Arduino IDE (see [Building](#building)).
   From then on, updates arrive over the air.
2. **Install the app** - `AmoledWatch-x.y.z.apk` from the
   [latest release](https://github.com/MikeCheek/CustomOS-ESP32-S3-1.75/releases/latest).
3. **Pair** - open the app, connect, and type the 6-digit code the watch shows.

## Updates

One release (tag `vX.Y.Z`) carries both the firmware and the app.

- **Watch:** checks once a day on Wi-Fi and asks before installing; Settings › Software update checks now.
- **App:** checks when it opens and offers the new APK and the new watch firmware (installed over Bluetooth);
  Settings › Software updates checks now.
- Failed updates leave the current version in place; a firmware that doesn't survive its first minute rolls back.

<details>
<summary>Publishing and signing</summary>

- Bump `FW_VERSION` in `firmware/diag.h`, tag `vX.Y.Z` and push - CI builds and publishes the release.
- **Signed firmware:** `python scripts/ota_keygen.py` once, flash that build over USB, and put `keys/ota_private.pem`
  in the `OTA_PRIVATE_KEY` repository secret. CI then also publishes a `.signed.bin`, the only kind the watch accepts.
- **App signing:** Android only installs an update signed with the same key. Set `ANDROID_KEYSTORE_BASE64`,
  `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD` so every release uses one keystore.
- The watch's update source is `FW_UPDATE_REPO` in `firmware/config.h`; the app's is `UpdateService.repo`.
</details>

## Building

| | |
|---|---|
| **Firmware** | **PlatformIO** (recommended, what releases use): `pio run -t upload` - same sources, plus ESP-IDF power management and Bluetooth modem sleep, so the watch light-sleeps with the phone connected. The first build compiles the core's libraries and takes a while. |
| **Arduino IDE** | Also builds, without the power management: core **esp32 3.3.10**, board *ESP32S3 Dev Module*, PSRAM *OPI*, flash 16 MB, partitions *Custom* (it uses [`firmware/partitions.csv`](firmware/partitions.csv): two 6 MB app slots), USB CDC on boot. Libraries pinned in [`scripts/arduino-libraries.txt`](scripts/arduino-libraries.txt); three more are vendored in `firmware/libraries/`. |
| **CLI** | `arduino-cli compile -b esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc --libraries firmware/libraries firmware` |
| **Partitions** | Since 3.5 the app slots are 6 MB (were 3 MB). A watch on the old table needs **one USB flash** of a 3.5+ build (PlatformIO `pio run -t upload`, or the IDE with partitions *Custom*): an over-the-air update can't change the table, and once the firmware outgrows 3 MB the watch says it doesn't fit. Settings are kept (NVS doesn't move). |
| **App** | `cd companion_app && flutter pub get && flutter run` |
| **iPhone app** | On a Mac with Xcode: `cd companion_app && flutter build ios` (or `flutter run` with an iPhone plugged in). iOS 16+; photo-face subjects need iOS 17. The Podfile fetches the watch emulator's two libraries itself (`ios/WatchEmu/fetch_libs.sh`). Signing needs your Apple developer team in Xcode. CI builds it unsigned on every push. On iPhone, iOS doesn't let an app read other apps' notifications, change Do Not Disturb, control other apps' music or send texts by itself, so those parts of the phone link are Android-only (their screens say so and what works instead); a fall alert opens Messages with the text and location ready, or shows a notification that does. The watch link, memos, the assistant, all the on-phone AI, photo faces, Look, the emulator, calendar, contacts and weather work. |
| **Screenshots** | `tools/watch_sim/run.sh` - compiles the UI for the PC and renders `docs/screenshots/` |
| **CI** | every push builds the firmware (PlatformIO, and the Arduino IDE build as a check) and the APK; a `v*` tag publishes a release |

**Layout:** `firmware/` the Arduino sketch · `companion_app/` the Flutter app · `scripts/` signing and setup helpers ·
`tools/watch_sim/` the screenshot simulator · [`COMPANION_PROTOCOL.md`](COMPANION_PROTOCOL.md) the watch ↔ app protocol ·
[`ROADMAP.md`](ROADMAP.md) what's done and next.

<details>
<summary>Hardware and under the hood</summary>

- **Board:** ESP32-S3 (16 MB flash, 8 MB PSRAM), CO5300 AMOLED over QSPI, CST9217 touch, QMI8658 IMU, PCF85063 RTC,
  AXP2101 PMU, ES8311 speaker + ES7210 mic array, TCA9554 IO expander, microSD, LC76G GPS (`-G` variant).
  Pins live in `firmware/board_pins.h`.
- **Rendering:** frames are drawn into PSRAM buffers on one core and sent by a task on the other, diffed against
  what the panel shows so only changed rectangles go over QSPI (idea borrowed from Meta's MUSE firmware).
- **Screens:** a `Screen` struct per app (`ui.h`) on a stack with slide transitions; HAL modules (`hal_*.cpp`)
  behind feature flags in `config.h`.
- **Link:** a paired, encrypted BLE connection carrying small JSON messages, plus Wi-Fi for big transfers.
</details>
