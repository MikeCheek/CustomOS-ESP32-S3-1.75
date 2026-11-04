# Roadmap

What is done and what is next for AmoledSmartWatchOS (firmware) and the companion app. Each open
item has a short design so it can be picked up directly. Effort: S = an evening, M = a few days, L = a week or more.

## Done

| Version | What |
|---|---|
| fw 2.5 / app 2.1 | Crash reports + Diagnostics (watch and app), guided setup, firmware updates over Bluetooth with rollback, smooth fonts with accents, voice replies, light sleep / BLE power modes / hardware pedometer |
| fw 2.6 / app 2.2 | Calendar (agenda screen, reminders that work offline), turn-by-turn navigation from Google Maps, Android home-screen widget |
| fw 2.7 / app 2.3 | Fast recording transfers (windowed Bluetooth, 2M PHY, Wi-Fi), adding recordings from the phone, Music-style Recorder, app logo |
| fw 2.8 / app 2.4 | Files up to 4 MB over Bluetooth and any size over Wi-Fi (`/put`); memo knowledge: titles, topics, to-dos and people per memo, Insights dashboard (topics, trends, activity, heatmap, importance, length), theme collections, ranked search with highlights, Ask AI over memos, related memos, favorites, one-tap Process everything (optionally after every sync) |
| fw 2.8.1 / app 2.4.1 | Fixes: codecs retried at boot and before play/record (silent speaker, blank recordings), GPS probe no longer runs on boards without GPS, Wi-Fi gets back the 32 KB of RAM it needs with Bluetooth on (USB File storage / Media remote modes off - `FEATURE_USB_OTG_MODES`); memo playback in the app with speed, ±10 s and follow-along transcript |
| fw 2.8.2 / app 2.4.2 | Mic sensitivity (Settings > Microphone, 5 levels, default louder than before, live meter); memo language shown and correctable (re-transcribes and re-summarizes); fixed download links of Qwen3 0.6B and Gemma 3 270M, added Qwen3 0.6B int4 |
| fw 2.9 / app 2.5 | Watch Wi-Fi set up and checked from the app (scan from the watch, password, test, forget); home screen memo brief: urgent memos, to-dos to tick off, today's count, process button |
| fw 3.0 / app 3.0 | Paired, encrypted Bluetooth link (random code per pairing, Secure Connections, unpair); optional signed firmware updates (ECDSA P-256, `scripts/ota_keygen.py`, `scripts/sign_firmware.py`); git + GitHub Actions builds; memo fixes (no-speech memos skipped, MP3 lengths, absolute search scores) |
| fw 3.1 / app 3.1 | Do Not Disturb synced both ways + bedtime schedule on the watch; quick replies edited in the app |
| fw 3.2 / app 3.2 | Watchface complications (steps, battery, phone battery, next event, weather, messages, next turn, memo to-dos, seconds) on Default/Minimal faces and in custom watchfaces; slots set on the watch or in the app; signed-update downgrade protection; pairing hardening (iOS, retries) |
| fw 3.3 / app 3.3 | Updates from GitHub releases: the watch checks on Wi-Fi and installs (signed images when a key is set), the app updates itself and the watch; 3D power-on, lock/unlock, music spectrum ring and charging animations; voice memos recorded on the phone (copied to the watch); Author section; fix: memo sync timing out (BLE notifications sent immediately); CI checks the Android keystore secrets |
| fw 3.4 / app 3.4 | App icons on notifications and album art on Now playing; always-on display; PlatformIO build with power management, tickless idle and Bluetooth modem sleep (light sleep with the phone connected), I2S stopped when silent, drain since unplugged on the Battery screen; animated weather behind the main face (rain, snow, storm, fog, clouds, sun/heat, night sky; Settings > Display); Settings grouped into pages; quick circular lock/unlock and a light charging ripple; ~3x faster screen sends; fixes: music crash on the PlatformIO build, T9 keypad counts quick taps; Recorder: both mics live round the dial (level history on the bezel, waveform ring), screen stays on while recording; USB File storage / Media remote back in the release build, USB Mode lists only what a build supports |
| fw 3.5 (in progress) | Life: healthy days planned in the app (water, stretch breaks, Breathe, training days, meal times with ideas for five diets (written each day by the on-phone LLM from the diet, your food notes such as allergies, the season and training days; a built-in list without a model), a social check-in, wind-down) - the watch nudges at the right moments ("Do it" / "Later"), guides Breathe and a 3-minute stretch, keeps a daily score and streak, and reports each day back; App menu: the centred app's name, Games and Developer folders; app: one Faces tab for the watch's look (the face worn, the Eyes face's colour and parts, slots, brightness, always-on, text size, photo and designed faces - set live over {"t":"look"}), Home with the rings, up next, a quick timer and shortcuts you pick; Smart and feel: the Smart Stack ordered by what's running, the time of day and what you open then (taps learned per quarter of the day); notifications stacked by app, swipe left to clear, Clear all, mute an app for today, a red unread dot on the faces; a cue per event (messages, left/right turns, arrival, goals, start/stop, reminders, SOS) as tone patterns - the board has no motor - or a motor's rhythm when one is fitted; Text size; Low Power Mode (offered at 10%, off at 80% charging, yellow battery readout); Emergency SOS from the hold-PWR screen; Everyday apps: Timer (presets, custom, pause; the assistant's timer), Stopwatch with laps, Alarms (weekdays, snooze, ring asleep), World clock (30 cities, daylight saving on the watch, time zone from the phone), Weather (hours and days), Reminders (the memo to-dos, ticked off on the watch); Fitness: activity rings (Move, Exercise, Stand) with goals, celebrations and stand reminders, a rings complication, the week in the Activity app and the companion app, a Workout app (9 types, GPS distance on the -G board or steps, pace, calories, pause, summary, sent to the phone) with detection of walks and runs and "Still working out?"; Getting around like watchOS: the apps in one honeycomb (bigger in the middle, rubber-band edges, an app zooms open from its icon; or an A-Z list), every icon in one style (a coloured disc, white drawing), a Smart Stack on swipe up (timer, music, next event, weather, activity, messages, battery), touch and hold a face to switch faces with previews and edit complications, BOOT on a face opens Control Center, tests and diagnostics behind Developer mode, Files opens the media browser; battery readout kept inside the round edge, complication labels cut to fit; App: iPhone and iPad (Swift channels, the emulator as a framework, Vision subjects, a side rail on wide screens); AI models: background downloads that outlive the app (background_downloader: resume, retries, notifications, Hugging Face mirrors), a second transcription engine (sherpa-onnx Whisper / Moonshine / SenseVoice from GitHub), more voices, speaker and sound models, packs to set up everything at once; After the wake word, words that aren't a command go to the phone's assistant as the question (no repeating it, any language); several photo faces (tap, hourly, daily; list, show and delete from the app); 6 MB app slots (firmware/partitions.csv; one USB flash from older tables, settings kept); Photo watch face with depth (the time behind the photo's subject: ML Kit subject segmentation and the digits drawn on the phone, blended on the watch); Offline voice commands after the wake word (ESP-SR MultiNet7 English from the SD card, phonemes precomputed so the on-chip pronouncer stays out of the build: music, timers, find phone, DND, opening apps); Recorder: skip silences; Activity app (still / walking / running / moving, rep counter) and flick to go back; Sound alerts (phone listens, CED audio tagging, the watch shows it); Look: describe / read / translate a photo with an on-phone vision model; Watch emulator in the app (the firmware's screen code as an Android library: screen, touch, buttons; linked to the app in-process like a real watch, settings kept, the phone's accelerometer, speaker and microphone, an SD card folder); Memo search and Ask AI by meaning (EmbeddingGemma on the phone, cached vectors); Catch me up (notification digest) and translation with the local LLM, on the watch and in the app's Messages; the Assistant in the app too; Draw to open (one-stroke letters, $1 recognizer); Sleep tracking from wrist movement and a smart alarm (Sleep app); Fall detection (accelerometer FIFO, free fall + impact + lying still, "Did you fall?", the phone texts an emergency contact); Spoken assistant answers (Piper voices via sherpa-onnx on the phone, played on the watch); Smart replies written by the local LLM for each message; Piano Tiles songs from the app: hum a tune (YIN pitch tracking, tempo from the onsets) or pick a MIDI file, sent to /piano/; Who said what in memos (speaker split with sherpa-onnx: pyannote segmentation + CAM++ voices, matched to Whisper's timestamps, named voices, the summary reads the conversation); Voice assistant (watch app + BOOT double-press, or a wake word: ESP-SR WakeNet on both mics, models from the app onto the SD card; ESP-SR noise suppression + auto gain on its recordings; rules for timers, reminders, alarms, music, weather, agenda, DND, find phone, memo questions in English and Italian, the local LLM for everything else); Piano Tiles for the round screen (42 public-domain songs in four genres, timing ring with Perfect / Great / Good, combos, hold bonuses, accuracy-based stars, any difficulty on any song, progress round the edge, SD-card songs, `tools/midi_to_piano.py`) and a piano voice in the audio driver; Wi-Fi works next to Bluetooth and USB again (TinyUSB, the Bluetooth host and two task stacks moved to PSRAM) |

## Next

### 1. App icons and album art — M
**Why:** notifications and the music screen are text only; an icon makes a notification readable at a glance.
- **Phone:** in `NotificationListener`, render the notification's small icon (or the app icon) to 32×32,
  and the media session's album art to 96×96. Encode as RGB565 with a hash; send only on first use.
- **Protocol:** `{"t":"img","h":<hash>,"w":32,"f":"565","b":"<base64>"}`. The 2 KB icon needs several link messages;
  add a chunked variant (`"o":offset,"n":total`) or a dedicated binary characteristic.
- **Watch:** PSRAM cache of about 32 icons keyed by hash (LRU), persisted to `/icons/`. `ntf` and `med` messages carry the hash.
- **Risk:** link throughput while a lot of notifications arrive at once. Album art only when the now-playing screen is open.

### 2. Do Not Disturb sync — S
- **Phone → watch:** observe `NotificationManager.getCurrentInterruptionFilter()`
  (`ACTION_INTERRUPTION_FILTER_CHANGED` broadcast) and send `{"t":"dnd","on":1}`.
- **Watch → phone:** the DND tile sends `{"e":"dnd","on":1}`, and the phone calls `setInterruptionFilter`.
  This needs Notification Policy access, a one-time system screen; add it to setup and Diagnostics.
- **Watch:** add a DND schedule in Settings (bedtime 23:00–7:00) for when the phone isn't connected.

### 3. Custom quick replies — S
- The app edits the list (reorderable) and sends `{"t":"qr","l":["…","…"]}` on connect.
- The watch stores up to 10 in NVS and uses them instead of the built-in presets. "Dictate" stays first.

### 4. Watchface complications — M
**Why:** the data is already on the watch (weather, next event, phone battery, steps, navigation) but only shows inside apps.
- Add a `Complication` API on the watch: `int count(); void draw(slot, rect)`. Data providers:
  - steps ring
  - battery
  - phone battery
  - next event (`calendar_next`)
  - weather
  - unread notification count
  - navigation step
- The default and minimal watchfaces get 2–3 slots. The custom watchface JSON gains `"complication":"next_event"`
  widgets, and the app's designer gets the matching items.

### 5. Always-on display — M
- Reuse the asleep loop: instead of `display_sleep()`, set the panel to its lowest brightness and draw a sparse
  clock (≤5 % of pixels lit, shifting a few pixels every minute against burn-in). Redraw once a minute.
- Wake from light sleep on a 60 s timer instead of 150 ms when nothing else is pending.
- Make it a setting, off by default, auto-disabled below 20 % battery.
- **Check first:** the CO5300's idle-mode current at low brightness, measured with the PMU's battery current.

### 6. Wrist gestures — M
- Use the QMI8658's on-chip **wake-on-motion / tap** engines (`configWakeOnMotion`, `configTap`) on INT1 instead of
  polling the accelerometer. This needs the IMU interrupt pin; the board file currently has `PIN_IMU_INT1 -1`, so check
  the schematic (it may be routed through the IO expander).
- **Raise to wake:** tilt change towards the face within 0.5 s, then hold.
- **Flick to dismiss:** a quick roll out and back while a notification or reminder is on screen.
- **Win:** better wake detection and fewer I²C reads while asleep.

### 7. Sleep tracking — M
- **Watch:** while it's night and the watch is still, log a motion count per minute (the pedometer's activity plus
  accelerometer variance) to `/sleep/yyyymmdd.bin`: 1 byte per minute, about 600 bytes a night.
- **Phone:** download in the morning over the notes channel. Score with a simple actigraphy model (Cole-Kripke), show
  the nights in the app's Activity tab and a summary on the watch.
- **Watch:** an optional smart alarm (vibration needs `FEATURE_VIBRATE`; otherwise a soft tone).

### 8. Workouts — L
- **Watch:** a workout screen (walk / run / bike / free) with timer, steps, cadence, laps and an auto-pause on no motion.
- **G board:** GPS track at 1 Hz to `/workouts/<id>.gpx`; distance and pace live.
- **Phone:** sync the GPX and show a map with stats (no extra package: draw the polyline on a `CustomPaint` tile
  background, or use flutter_map). Optional export to Health Connect.

### 9. Voice memos, part 3 — M
(Search, collections, insights and the one-tap pipeline are done in app 2.4.)
- **App:**
  - share as WAV/TXT/Markdown; export a collection or the to-do list
  - embeddings (a small on-device sentence model) for semantic search beyond shared words
  - background processing (WorkManager) so the pipeline runs without the app open
  - delete on the watch after a confirmed download (setting)
- **Watch:** show the transcript (`/Recordings/*.txt`) in the Recorder.

### 10. Bluetooth security — M
- Replace the fixed passkey (`BLE_PAIRING_PASSKEY`) with a random one shown on the watch at pairing time.
  NimBLE's `onPassKeyDisplay` callback is already supported by the library.
- Require encryption on the sensitive characteristics (`NIMBLE_PROPERTY::READ_ENC | WRITE_ENC` on contacts, link,
  notes, file, OTA).
- **OTA:** accept only images signed with your key. Add an ECDSA signature appended by a build script, checked in
  `hal_ota.cpp` before `Update.end()`. The ESP-IDF secure boot alternative needs eFuses and is irreversible, so it isn't recommended.

### 11. Project hygiene: git + CI — S
- Put the sketch and `companion_app/` in one git repo, with `_backup_*` folders in `.gitignore`.
- **GitHub Actions:**
  - `arduino-cli compile` with the same FQBN as `scratchpad/build.sh` (core 3.3.10, PSRAM=opi,
    FlashSize=16M, PartitionScheme=custom with firmware/partitions.csv, CDCOnBoot=cdc); upload `*.ino.bin` as a release asset.
  - `flutter analyze` and `flutter build apk --debug` for the app.
- Bump `FW_VERSION` (diag.h) per release; the app's firmware update screen then compares versions.

### 12. PlatformIO (pioarduino) with a custom sdkconfig — L
**Why:** this is the biggest remaining battery gain. The prebuilt Arduino core has no `CONFIG_PM_ENABLE` and no BT modem
sleep, so the chip can't light-sleep while Bluetooth is on. With pioarduino's `custom_sdkconfig` you can enable
`CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE` and `CONFIG_BT_CTRL_MODEM_SLEEP` (main clock, or 32 kHz XTAL if fitted).
- **Expected:** idle-connected current from about 20 mA to about 3–5 mA.
- **Steps:**
  1. Reproduce the current build under pioarduino with Arduino 3.3.x.
  2. Enable PM and tickless idle; `power_update_sleep_policy()` already calls `esp_pm_configure()` and will start working.
  3. Enable modem sleep; check BLE connection stability with the slow connection parameters.
  4. Faster incremental builds come as a side benefit.
- **Risk:** the hal_display QSPI DMA and the USB-Serial-JTAG under light sleep need testing; take a GPIO-hold approach for the panel's reset line.

## Smaller ideas
- **Timers and alarms:** timer/stopwatch app; alarms synced from the phone's clock app.
- **Weather:** a 3-day forecast screen.
- **Find my phone:** "last seen" location when the link drops (phone sends its location on disconnect).
- **Phone controls:** camera shutter for the phone (link message → `MediaStore` intent with a countdown).
- **Battery history:** graph from the PMU, logged every 10 min to NVS/SD.
- **Pedometer:** calibration (step length) in the app; distance and calories.
