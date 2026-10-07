# Updating to firmware 3.2.0 / app 3.2.0

All files are already in this folder (141 changed or new, verified by checksum).
Previous versions: `_backup_before_ui_pipeline/before_v32.tar`. List: `_backup_before_ui_pipeline/CHANGED_FILES_v32.txt`.

## 1. Flash the watch (USB)
1. Open `firmware\firmware.ino` in the Arduino IDE (same board settings as before: ESP32S3 Dev Module, OPI PSRAM,
   16 MB flash, "16M Flash (3MB APP/9.9MB FATFS)", USB CDC On Boot enabled). No new libraries to install.
2. On the watch: Settings > USB Mode > **Firmware & Debug** (or **Reboot to flash**). Close the Serial Monitor.
3. Upload. If "Write timeout": hold BOOT, press RESET, release BOOT, pick the new COM port, upload, then press RESET.
4. Serial log to check: `FW 3.2.0`, `[audio] ... speaker=1, mic=1` (or the `I2C devices answering` line),
   `[ble] fw 3.0: old bonds cleared`.

## 2. Build and install the app
1. `cd companion_app` → `flutter pub get` → `flutter analyze` (send me any errors) → `flutter run` (or build the APK).
2. No new packages; the manifest gains the Do Not Disturb permission.

## 3. Pair again (new in 3.0 - required once)
1. In the app: Settings > Forget this watch. On Android also remove "AmoledWatch" in the phone's Bluetooth settings.
2. Connect from the app. The watch shows a 6-digit code; type it in Android's pairing prompt (within ~30 s).
3. Later, to unpair: watch Settings > BLE Status > "Unpair all phones" (tap twice).

## 4. Set up the new features
- **Do Not Disturb**: app Settings > Do Not Disturb & quick replies > allow DND access; optionally Bedtime + hours.
- **Quick replies**: same screen; edit, drag to reorder, up to 10.
- **Watchface slots**: app Settings > Watchface slots (or watch Settings > Watchface slots). Custom faces: add the
  ring widgets (Steps ring, Next event, Memo to-dos...) in the watchface designer.
- **Watch Wi-Fi** (2.9): Home > More > Watch Wi-Fi.
- **Microphone** (2.8.2): watch Settings > Microphone.

## 5. Optional: git + automatic builds
1. Install git, create an empty GitHub repo named `AmoledSmartWatchOS`.
2. In this folder (PowerShell): `.\scripts\init_repo.ps1 -Remote https://github.com/<you>/AmoledSmartWatchOS.git`
3. Every push builds the firmware and the APK (Actions tab). Tag `v3.2.0` to publish a release.

## 6. Optional: signed firmware updates
1. `pip install cryptography`, then `python scripts/ota_keygen.py` (creates `keys/ota_private.pem` - back it up,
   never commit it - and writes your public key into `ota_pubkey.h`).
2. Flash that build once over USB.
3. For every Bluetooth update: `python scripts/sign_firmware.py <build>/firmware.ino.bin`, send the
   `.signed.bin` from the app. Unsigned or older images are refused; USB flashing always works.

## 7. What to test and report back
- Pairing on first connect; reconnect without a code afterwards.
- Recording volume / playback, Wi-Fi setup from the app.
- DND both ways, bedtime, quick replies on a message reply.
- Complications on the Default and Minimal faces.
