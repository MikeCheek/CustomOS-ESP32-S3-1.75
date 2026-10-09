# Watch ↔ companion app protocol

One BLE GATT service, `19B10000-E8F2-537E-4F6C-D104768A1214`. The watch advertises as `AmoledWatch`.
Firmware side: `hal_ble.cpp` (transport), `phone_link.cpp` (phone link logic).
App side: `companion_app/lib/services/ble_service.dart`, `ble_protocol.dart`, `providers/companion_provider.dart`.

## Pairing and encryption (firmware 3.0+)

Every writable characteristic (and the battery read) needs an encrypted link: LE Secure Connections with MITM
protection and bonding. The watch is display-only: when a phone pairs it shows a random 6-digit code
(`app_pairing.cpp`) that is typed on the phone. Bonded phones reconnect encrypted without a code. The app pairs
right after service discovery if reading the battery characteristic is refused (older firmware reads fine and
isn't paired). Unpairing: watch Settings > BLE Status > "Unpair all phones", and "Forget this watch" in the app
(which also removes the Android bond).

Signed firmware: `scripts/sign_firmware.py` writes `<name>.signed.bin` = image ‖ signature(64) ‖ `AWSIG001`; the app
strips the last 72 bytes and sends the signature with `0x05`.

## Characteristics

| UUID suffix | Name | Direction | Format |
|---|---|---|---|
| `0001` | Battery | watch → phone (read/notify) | 1 byte, % |
| `0002` | Notification (legacy) | phone → watch | UTF-8 text `[App] Title: text`; the exact string `\x01__FINDME__\x01` triggers Find my watch |
| `0003` | Time | phone → watch | 7 bytes: year-2000, month, day, hour, minute, second, 0 |
| `0004` | Contacts | phone → watch | JSON array `[{"name","phone","email"}]`, framed (see below); a write starting with `[` is a whole list |
| `0005` | Media (legacy) | phone → watch | JSON `{"title","artist","album","playing","position","duration"}` |
| `0006` | File / data | phone → watch | prefix `0x01` watchface JSON (first chunk: `0x01` + total len 2 LE + data, then `0x01` + data), `0x02` weather JSON, `0x03` fitness JSON, otherwise a file transfer: header `nameLen(4 LE) totalSize(4 LE) name`, chunks `len(4 LE) offset(4 LE) data`. Files up to 4 MB, accepted on the watch (bigger: Wi-Fi `/put`). A name `rec:<name>.wav`/`.mp3` (up to 4 MB) is saved into /Recordings |
| `0007` | Notes | both | phone writes `0x01` list, `0x02`+name download, `0x03`+name delete, `0x05` next chunk (`0x05`+n = n more chunks: the app keeps a window of chunks in flight), transcript as `0x06`+total(2 LE)+data then `0x07`+data. Watch notifies `0x10` list header, `0x11` list chunk, `0x12` file chunk, `0x13` done |
| `0008` | Controller | phone → watch | 2 bytes: d-pad bits, button bits |
| `0009` | **Phone link** | both | JSON messages, framed (see below) |
| `000A` | **Firmware update** | both | binary, see "Firmware update" below |

### Framing for long messages (contacts `0004`, phone link `0009`)

`0x01` + total length (2 bytes, little endian) + data starts a message, `0x02` + data continues it until
`total` bytes arrived. A packet starting with `{` (or `[` for contacts) is a complete message by itself.
Each packet is at most MTU − 3 bytes. The watch keeps the last complete message per characteristic
in PSRAM (phone link: queue of 7 messages of up to 1023 bytes).

On the phone link, a packet starting with `0x03` is a frame of an image the watch asked for (fw 3.4+):
`0x03` + kind (`'i'` app icon, `'a'` album cover) + hash (4 bytes LE) + total size (4 LE) + offset (4 LE) +
data. The image is a JPEG (icons 40×40, covers 128×128, up to 24 KB); frames come in order, one image at a time.

## Phone link messages (`0009`)

Phone → watch (`"t"` = type):

| Message | Meaning |
|---|---|
| `{"t":"hi"}` | app connected; the watch answers with `{"e":"hi",...}` |
| `{"t":"bat","l":85,"c":1}` | phone battery %, charging |
| `{"t":"med","ti":"Song","ar":"Artist","pl":1,"po":12,"du":200,"v":7,"vm":15,"ah":456}` | now playing: title, artist, playing, position s, duration s, volume, max volume, album cover hash (optional) |
| `{"t":"ntf","id":123,"ap":"WhatsApp","ti":"Mario","tx":"Ciao!","rp":1,"ic":789}` | notification; `rp` = can be replied to, `ic` = app icon hash (optional). Same `id` again updates it |
| `{"t":"nrm","id":123}` | notification removed on the phone |
| `{"t":"call","st":"ring"\|"active"\|"end","id":456,"n":"Mario"}` | call state; `ring` opens the call screen and wakes the watch |
| `{"t":"find","on":0}` | find-my-phone stopped on the phone side |
| `{"t":"req"}` | send a status report now |
| `{"t":"cal","r":1,"rm":10,"ev":[...]}` … `{"t":"cal","ev":[...],"done":1}` | agenda (replaces the watch's on `done`; `r` starts a new set, `rm` = reminder minutes, 0 = off). Event: `{"id":1,"ti":"Title","s":<start>,"e":<end>,"ad":0,"lo":"Place","c":0xRRGGBB}`; times are **local** epoch seconds (local date/time counted as if UTC). Max 24 events, ~850 bytes per message |
| `{"t":"nav","st":1,"d":"200 m","i":"Turn right onto Via Roma","x":"12 min · 10:42","h":123}` / `{"t":"nav","st":0}` | turn-by-turn step (`h` = icon hash, 0 = none) / navigation ended |
| `{"t":"navi","h":123,"w":48,"b":"<base64>"}` | maneuver icon: 48×48, 1 bit per pixel, rows MSB first; sent once per hash (the watch caches 12) |
| `{"t":"wifi","on":1}` / `{"t":"wifi","on":0}` | start / stop the Wi-Fi transfer server (see below) |
| `{"t":"crash"}` / `{"t":"crash","clr":1}` | send diagnostics (and clear the crash report) |
| `{"t":"dict","id":123,"n":1,"tx":"text"}` / `{"t":"dict","id":123,"n":1,"err":"why"}` | voice reply transcription result (`n` echoes the watch's attempt number) |

Watch → phone (`"e"` = event):

| Message | Meaning |
|---|---|
| `{"e":"hi","bat":80,"chg":0,"st":1234,"fw":"2.5.0","rr":"power on","cr":0,"up":12}` / `{"e":"st",...}` | watch battery, charging, steps today; on connect, then every 5 min. `hi` adds firmware version, last reset reason, crash report id (0 = none) and uptime s |
| `{"e":"wifi","st":"ready","ip":"192.168.1.20","port":8080,"k":"<key>"}` / `{"e":"wifi","st":"err","why":"..."}` / `{"e":"wifi","st":"off"}` | Wi-Fi transfer server state |
| `{"e":"crash","id":7,"txt":"...","sys":"..."}` | diagnostics: last crash report (`txt`, empty if none) and system status text |
| `{"e":"dict","id":123,"n":1,"f":"_reply.wav"}` | voice reply recorded: download `f` with the notes characteristic, transcribe, answer with `t:"dict"` |
| `{"e":"req"}` | please resend battery + now playing |
| `{"e":"img","k":"i"\|"a","h":789}` | send this app icon / album cover (as `0x03` frames); asked once per image, again after 20 s if nothing came |
| `{"e":"med","a":"toggle"\|"next"\|"prev"\|"volUp"\|"volDown"}` | media control |
| `{"e":"find","on":1}` | ring the phone (stops after 60 s or `on:0`) |
| `{"e":"call","a":"answer"\|"decline","id":456}` | answer / decline / hang up |
| `{"e":"ntf","a":"dismiss","id":123}` | dismiss on the phone |
| `{"e":"ntf","a":"reply","id":123,"tx":"On my way"}` | quick reply through the notification's reply action |

Text is UTF-8. The watch keeps Latin-1 letters (its smooth fonts draw them) and folds everything else
(`ble_fold_utf8`).

## Firmware update (`000A`)

App → watch:

| Packet | Meaning |
|---|---|
| `0x01` size(4 LE) [md5 as 32 hex chars] | begin; watch answers `0x81` |
| `0x02` offset(4 LE) data | image data, strictly in order |
| `0x03` | everything sent; watch verifies and answers `0x84` |
| `0x04` | abort |
| `0x05` signature(64) | fw 3.0+: ECDSA P-256 `r‖s` over the SHA-256 of the image, sent before `0x03`. Required when the watch was built with a key in `ota_pubkey.h` |

Watch → app (notify):

| Packet | Meaning |
|---|---|
| `0x81` status | begin: 0 ok, 1 error, 2 too big, 3 battery below 20 % (not charging) |
| `0x82` written(4 LE) | bytes written to flash, every 16 KB and at the end |
| `0x84` status | 0 = image verified, restarting in ~2.5 s; 8 = incomplete, 9 = invalid image, 10 = signature missing or wrong |
| `0x85` code | failed while receiving (4 cancelled, 2 out of order, 3 overflow, 5 disconnected, 6 timeout, 7 flash) |

Flow control: keep at most 32 KB sent beyond the last `0x82` (the watch buffers 64 KB in PSRAM).
The image goes to the inactive OTA slot. A new image that doesn't stay up for a minute is rolled back
by the bootloader on the next reset. A valid `.bin` starts with `0xE9` and contains the string
`AMOLEDWATCH_FW=<version>`.

## Wi-Fi transfers

For big files the app asks for `{"t":"wifi","on":1}`; the watch joins its saved Wi-Fi network and serves
(HTTP, port 8080, every request needs `k`):

| Request | Result |
|---|---|
| `GET /list?k=KEY` | `[{"name":"rec_001.wav","size":123}]` (recordings, wav/mp3) |
| `GET /f?k=KEY&n=NAME` | the file from /Recordings |
| `POST /put` with headers `X-Key`, `X-Name` (body = any file) | saved to the card root like a Bluetooth file transfer (replaces a same-name file); `{"name":"..."}` |
| `POST /up` with headers `X-Key: KEY`, `X-Name: NAME` (body = the file, not multipart) | saved to /Recordings (never overwrites: `name_2.wav`); `{"name":"saved as"}` |

Key and name can always be sent as `X-Key` / `X-Name` headers instead of `k` / `n` (for `/up` they must be:
the server doesn't parse the query string of a body upload). A `rec:` file over Bluetooth is never overwritten either.

The server stops on `{"t":"wifi","on":0}`, after 2 idle minutes, or 15 s after Bluetooth drops; the radio
goes back to how it was. The phone must be on the same network.

## Wi-Fi setup from the app (firmware 2.9+)

Phone → watch on the link characteristic, `{"t":"wcfg","op":...}`:

| `op` | Extra fields | What the watch does |
|---|---|---|
| `status` | – | replies at once with the state below |
| `scan` | – | scans (radio on just for the scan), replies `{"e":"wcfg","op":"scan","ok":1,"l":[{"s":"ssid","r":-55,"o":0}]}` (up to 15, strongest first, `o` = open) |
| `set` | `s` ssid, `p` password | saves them, connects, replies like `test` |
| `test` | – | connects with the saved network, replies `{"e":"wcfg","op":"test","ok":1\|0,"why":"..."}` + state |
| `forget` | – | deletes the saved network, turns Wi-Fi off, replies `status` |

State fields: `saved` (saved SSID or ""), `on` (radio on), `auto` (Wi-Fi on in Settings), `conn`, and when connected
`ssid`, `ip`, `rssi`; `err` = why the radio couldn't start (e.g. "Not enough memory for Wi-Fi"); `ram` = free internal RAM.
Scans and tests run in the main loop; afterwards the radio goes back off unless Wi-Fi is on in Settings or a transfer
uses it. While a Wi-Fi transfer runs, `scan`/`set`/`test` reply `ok:0` with a reason.

## Do Not Disturb and quick replies (firmware 3.1+)

| Direction | Message | Meaning |
|---|---|---|
| phone → watch | `{"t":"dnd","on":0\|1}` | the phone's DND changed (sent on connect too, if DND sync is on in the app) |
| phone → watch | `{"t":"dnd","bed":0\|1,"from":1380,"to":420}` | bedtime window (minutes after midnight; may cross midnight) |
| watch → phone | `{"e":"dnd","on":0\|1}` | DND tile used on the watch; the app applies it with `setInterruptionFilter` if it has DND access |
| phone → watch | `{"t":"qr","l":["OK","On my way",...]}` | quick replies (max 10, 38 chars each); `[]` = built-in presets. "Dictate" always stays first |

While DND (switch or bedtime) is on, the watch shows no notification popups and plays no reminder or navigation
sounds; incoming calls still ring.

## Watchface complications (firmware 3.2+)

| Direction | Message | Meaning |
|---|---|---|
| phone → watch | `{"t":"comp","s":["steps","next_event","battery"]}` | the 3 slots on the Default/Minimal faces, left to right |
| phone → watch | `{"t":"todo","n":4,"top":"Call Marco"}` | open to-dos from the voice memos (for the `todos` complication) |

Keys: `none`, `steps`, `battery`, `phone_battery`, `next_event`, `weather`, `notifications`, `navigation`, `todos`,
`seconds`. Custom watchface JSON: `{"type":"complication","dataSrc":"next_event","x":120,"y":330,"radius":42}`.

Firmware update end status `0x84` 11 = image older than the installed firmware (only when signed updates are on).
