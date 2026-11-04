/*
 * app_wallet.h
 * Wallet: boarding passes, tickets, loyalty and membership cards - their
 * QR code, Aztec, PDF417, Data Matrix or barcode shown big and bright on
 * the watch, ready to scan. The app makes them (from a photo or screenshot
 * of the code, or typed in) and turns each into its modules; the watch
 * only draws black and white squares, whatever the kind. Kept on the SD
 * card (/wallet), or in memory until a restart without one - the app
 * sends them again when it connects.
 *
 *   phone -> watch  {"t":"wal","op":"put","i":0,"n":"Ryanair","s":"FR 1234 - seat 12A","k":"aztec",
 *                    "w":45,"h":45,"r":10,"crc":12345,"tot":254,"o":0,"b":"<base64>"}
 *                   the modules row by row, 1 bit each (MSB first), in parts ("o" = byte offset);
 *                   "r": a module's height in tenths of its width (PDF417: 30, the rest 10);
 *                   a 1D barcode has h = 1
 *                   {"t":"wal","op":"del","i":3}      {"t":"wal","op":"ask"}
 *   watch -> phone  {"e":"wal","l":[["Ryanair",12345],...]}   on "hi", "ask" and after each change
 */
#pragma once
#include "ui.h"
#include <ArduinoJson.h>

#define WALLET_MAX 8

extern Screen wallet_screen;
void wallet_init();                     // setup(): what's on the SD card
void wallet_on_link(JsonDocument &doc); // phone link "wal"
void wallet_send();
void wallet_open_next(int slot);         // the next time it opens, show this pass
