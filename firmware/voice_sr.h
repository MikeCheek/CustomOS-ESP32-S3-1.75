/*
 * voice_sr.h
 * On-watch speech processing with Espressif's ESP-SR (part of the core):
 *
 *  - Wake word: WakeNet listens to both microphones and opens the
 *    assistant when it hears its word ("Hi ESP", "Alexa", "Jarvis"...).
 *    The models (~290 KB each) live on the SD card in /srmodels/<name>/
 *    (_MODEL_INFO_, wn9_data, wn9_index): the companion app downloads
 *    them from Espressif's esp-sr repository and sends them over
 *    Bluetooth as "wn:<name>/<file>". Off by default - it keeps the
 *    microphone and a CPU core busy all the time.
 *
 *  - Offline commands: with MultiNet7 installed as well (~2.7 MB), the
 *    wake word listens ~4 s for a command ("next song", "set a timer for
 *    five minutes", "find my phone") and runs it on the watch; anything
 *    else said is sent to the phone's assistant as the question
 *    (voice_capture.h, assistant_ask_recorded()), silence opens it.
 *
 *  - Voice cleanup: the mono recordings the assistant and voice replies
 *    make (mic1 + mic2 summed) go through ESP-SR's noise suppression and
 *    automatic gain before they're written. If that front end can't be
 *    created, or produces nothing, recordings are saved as before.
 *
 * The microphone samples come from hal_audio's own I2S reads
 * (audio_mic_read()), not from the core's ESP_SR library, which would
 * install a second I2S driver on the same port.
 */
#pragma once
#include <stdint.h>

enum VoiceWakeState {
    VW_OFF,          // setting off
    VW_NO_MODEL,     // nothing in /srmodels (or no SD card)
    VW_LOADING,
    VW_LISTENING,
    VW_PAUSED,       // the microphone is busy, or the screen is off
    VW_FAILED,       // couldn't create the engine (memory)
    VW_UNSUPPORTED,  // built without ESP-SR
};

void voice_init();            // setup(), after audio_init() and sd_init()
void voice_update();          // loop(), awake or asleep

VoiceWakeState voice_wake_state();
const char *voice_wake_status();      // one line for Settings
const char *voice_wake_word();        // "Alexa", "" with no model
int  voice_wake_model_count();
void voice_wake_next_model();         // Settings: switch to the next installed word
bool voice_wake_keeps_awake();        // listening with the screen off: no light sleep

// Offline commands after the wake word (MultiNet7 English in
// /srmodels/mn7_en, "wn:mn7_en/<file>" from the app): music, timers,
// find my phone, do not disturb, opening apps. Setting voice_commands.
bool voice_commands_installed();
const char *voice_commands_status();  // one line for Settings

// A "wn:<model>/<file>" Bluetooth transfer (name without the "wn:").
// Saves it in /srmodels; true when it's written.
bool voice_model_file(const char *name, const uint8_t *data, int len);
