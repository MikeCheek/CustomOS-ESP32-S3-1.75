/*
 * voice_sr.cpp - wake word and voice cleanup with ESP-SR (see voice_sr.h)
 *
 * Wake word: two tasks that live from the first time it's switched on.
 *   wake_ctl   loads/unloads the front end when the setting or the word
 *              changes, then blocks on fetch() and flags a detection
 *   wake_feed  reads both microphones (audio_mic_read()) and feeds them
 * The front end ("MM": two mics, speech-recognition type, low-cost mode)
 * separates the two microphone signals and runs WakeNet on them; its
 * buffers go to PSRAM. The main loop (voice_update()) decides when it
 * listens: not while recording or playing a file, not on the assistant
 * itself, and with the screen off only if that's switched on too.
 *
 * Models load from the SD card at runtime (srmodel_sdcard_init(): the
 * engine reads <base>/<name>/wn9_data itself), so there is no model
 * partition and nothing in flash. ESP-SR keeps that list for the whole
 * run - a word installed later is added to it here.
 *
 * Offline commands: with MultiNet7 (English, /srmodels/mn7_en, ~2.7 MB)
 * installed too, the wake word doesn't open the assistant straight away.
 * The same front end's output goes to MultiNet for a few seconds ("next
 * song", "set a timer for five minutes", "find my phone"...); a command
 * runs here on the watch, with no phone and no network. Nothing it knows
 * in that time goes to the assistant: what was said is kept as it's
 * heard (voice_capture.h), recording goes on while you're still talking,
 * and it's sent to the phone as the question - no need to say it again.
 * Silence after the word opens the assistant to listen, as before.
 */
#include "voice_sr.h"
#include "config.h"
#include "app_settings_state.h"
#include "hal_nvs.h"
#include <Arduino.h>
#include <string.h>

#if FEATURE_AUDIO && FEATURE_SD_CARD && defined(CONFIG_IDF_TARGET_ESP32S3) && __has_include("esp_afe_sr_models.h")
#define VOICE_SR 1
#endif

#if VOICE_SR
#include <SD.h>
#include <esp_heap_caps.h>
#include "hal_audio.h"
#include "hal_sd.h"
#include "hal_sleep.h"
#include "hal_usb.h"
#include "ui.h"
#include "app_assistant.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_wn_iface.h"
#include "esp_mn_iface.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "voice_capture.h"
#include "apps.h"
#include "app_recorder.h"
#include "app_sleep.h"
#include "phone_link.h"
#include "dnd.h"
#include "hal_rtc.h"
#include "hal_vibrate.h"
#include "clock_fmt.h"

// Both in libespressif__esp-sr.a, not in its headers.
extern "C" srmodel_list_t *srmodel_sdcard_init(const char *base_path);
extern "C" char *get_model_info(char *data, int size);
// MultiNet7 itself: esp_mn_handle_from_name() would link every MultiNet
// version (~700 KB of code), more than the app partition has left.
extern "C" const esp_mn_iface_t esp_sr_multinet7_quantized;

// esp_mn_commands_add() turns text into phonemes on the watch with flite,
// ~620 KB of code. The phonemes come precomputed instead
// (voice_cmd_phrases.h, esp_mn_commands_phoneme_add()), and this keeps
// flite out of the build: nothing here calls esp_mn_commands_add().
extern "C" char *flite_g2p(const char *, int) { return nullptr; }

static const char *SD_DIR = "/srmodels";              // as the SD library sees it
static const char *VFS_DIR = "/sd/srmodels";          // as fopen() sees it (ESP-SR keeps this pointer)
static const int MAX_MODELS = 12;

// ---- installed models ----------------------------------------------------------------
struct WakeModel { char dir[24]; char word[20]; };
static WakeModel s_models[MAX_MODELS];
static int s_model_n = 0;
static bool s_scanned = false;
static char s_mn_dir[24] = "";              // the command model's folder, "" = none

static bool model_dir_name_ok(const char *n) {
    int l = strlen(n);
    if (l < 5 || l >= (int)sizeof(WakeModel::dir) || (strncmp(n, "wn9", 3) && strncmp(n, "mn7", 3))) return false;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

// "wakenet9l_tts1h8_Hey,Willow_3_0.644_0.650" -> "Hey Willow"
static void word_from_info(const char *info, char *out, int n) {
    out[0] = 0;
    const char *p = info;
    // comment lines start with '#'
    while (*p == '#') { p = strchr(p, '\n'); if (!p) return; p++; }
    for (int skip = 0; skip < 2 && p; skip++) { p = strchr(p, '_'); if (p) p++; }
    if (!p) return;
    int i = 0;
    for (; *p && *p != '_' && *p != '\n' && i < n - 1; p++) out[i++] = (*p == ',') ? ' ' : *p;
    out[i] = 0;
}

static String model_path(const char *dir, const char *file) {
    return String(SD_DIR) + "/" + dir + "/" + file;
}

static bool read_info(const char *dir, char *buf, int n) {
    File f = SD.open(model_path(dir, "_MODEL_INFO_"));
    if (!f) return false;
    int len = f.read((uint8_t *)buf, n - 1);
    f.close();
    if (len <= 0) return false;
    buf[len] = 0;
    return true;
}

static void scan_models() {
    s_scanned = true;
    s_model_n = 0;
    s_mn_dir[0] = 0;
    if (!sd_is_mounted() || usb_msc_host_active()) return;
    File d = SD.open(SD_DIR);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f && s_model_n < MAX_MODELS; f = d.openNextFile()) {
        bool is_dir = f.isDirectory();
        String name = f.name();
        f.close();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!is_dir || !model_dir_name_ok(name.c_str())) continue;
        if (!strncmp(name.c_str(), "mn7", 3)) {
            // English only: the commands below are English phrases.
            if (!strstr(name.c_str(), "_en")) continue;
            if (SD.exists(model_path(name.c_str(), "mn7_data")) && SD.exists(model_path(name.c_str(), "mn7_index")) &&
                SD.exists(model_path(name.c_str(), "_MODEL_INFO_")))
                strlcpy(s_mn_dir, name.c_str(), sizeof(s_mn_dir));
            continue;
        }
        if (!SD.exists(model_path(name.c_str(), "wn9_data")) || !SD.exists(model_path(name.c_str(), "wn9_index"))) continue;
        char info[256];
        if (!read_info(name.c_str(), info, sizeof(info))) continue;
        WakeModel &m = s_models[s_model_n++];
        strlcpy(m.dir, name.c_str(), sizeof(m.dir));
        word_from_info(info, m.word, sizeof(m.word));
        if (!m.word[0]) strlcpy(m.word, m.dir, sizeof(m.word));
    }
    d.close();
}

static int chosen_model() {
    if (!s_model_n) return -1;
    for (int i = 0; i < s_model_n; i++)
        if (!strcmp(s_models[i].dir, g_app_settings.wake_model)) return i;
    return 0;
}

// ---- ESP-SR's model list ---------------------------------------------------------
static srmodel_list_t *s_list = nullptr;

// A model installed since the list was read (ESP-SR never re-reads it).
static bool list_add(const char *dir) {
    if (esp_srmodel_exists(s_list, (char *)dir) >= 0) return true;
    char raw[256];
    if (!read_info(dir, raw, sizeof(raw))) return true;
    int n = s_list->num;
    char **names = (char **)realloc(s_list->model_name, (n + 1) * sizeof(char *));
    if (!names) return false;
    s_list->model_name = names;
    char **infos = (char **)realloc(s_list->model_info, (n + 1) * sizeof(char *));
    if (!infos) return false;
    s_list->model_info = infos;
    names[n] = (char *)calloc(MODEL_NAME_MAX_LENGTH, 1);
    if (!names[n]) return false;
    strlcpy(names[n], dir, MODEL_NAME_MAX_LENGTH);
    infos[n] = get_model_info(raw, strlen(raw));
    s_list->num = n + 1;
    return true;
}

static srmodel_list_t *model_list() {
    if (!s_list) s_list = srmodel_sdcard_init(VFS_DIR);
    if (!s_list) return nullptr;
    for (int i = 0; i < s_model_n; i++)
        if (!list_add(s_models[i].dir)) return s_list;
    if (s_mn_dir[0]) list_add(s_mn_dir);
    return s_list;
}

// ---- the commands ----------------------------------------------------------------
enum VoiceCmd {
    CMD_NONE = 0,
    CMD_TIME, CMD_NEXT, CMD_PREV, CMD_PAUSE, CMD_PLAY, CMD_VOL_UP, CMD_VOL_DOWN,
    CMD_TIMER_CANCEL, CMD_FIND_PHONE, CMD_DND_ON, CMD_DND_OFF,
    CMD_HOME, CMD_SETTINGS, CMD_NOTIFICATIONS, CMD_RECORDER, CMD_SLEEP, CMD_MUSIC, CMD_ASSISTANT,
    CMD_TIMER = 100,                // + minutes
};
struct CmdPhrase { int id; const char *text; const char *phonemes; };
#include "voice_cmd_phrases.h"      // tools/voice_cmd_phrases.py
static const int CMD_LISTEN_MS = 4000;      // after the wake word

// ---- the wake engine ---------------------------------------------------------------
static volatile bool s_want = false;        // main loop: should be loaded
static volatile bool s_pause = true;        // main loop: don't feed it right now
static volatile bool s_feed_on = false;     // wake_ctl -> wake_feed: s_afe is usable
static volatile bool s_feed_busy = false;   // wake_feed is inside s_afe
static volatile bool s_detected = false;
static volatile VoiceWakeState s_state = VW_OFF;
static char s_want_model[24] = "";
static char s_loaded_model[24] = "";
static char s_failed_model[24] = "";        // don't retry a word that didn't load until it changes
static const esp_afe_sr_iface_t *s_ifc = nullptr;
static esp_afe_sr_data_t *s_afe = nullptr;
static int16_t *s_feed_buf = nullptr;
static int s_feed_n = 0, s_feed_ch = 0;
static TaskHandle_t s_ctl_task = nullptr, s_feed_task = nullptr;
// MultiNet, loaded with the wake word when its model is installed
static volatile bool s_want_cmds = false;   // main loop: commands on and installed
static bool s_cmds_loaded = false;          // the engine was loaded with them (or tried)
static volatile bool s_cmds_failed = false;
static const esp_mn_iface_t *s_mn = nullptr;
static model_iface_data_t *s_mnd = nullptr;
static volatile bool s_cmd_listen = false;  // wake_ctl: feeding MultiNet after the word
static volatile int s_cmd_result = CMD_NONE;    // wake_ctl -> main loop, -1 = nothing understood
// What was said after the word (PSRAM, 10 s): the question when it isn't a command.
static const int CAP_SAMPLES = VoiceCapture::RATE * 10;
static int16_t *s_cap_buf = nullptr;
static VoiceCapture s_cap;
static bool s_cap_on = false;               // wake_ctl: capturing this time
static volatile bool s_cap_extend = false;  // no command, still talking: keep recording
static volatile bool s_cap_ready = false;   // s_cap holds a question for the main loop (it owns it until cleared)
// Core 0, priority 1: loop() and all drawing are on core 1 and must not
// wait for WakeNet; on core 0 the radios and the display sender (priority
// 2) still go first. It only needs to keep up with the microphone - the
// I2S DMA buffers hold the rest.
static const int WAKE_CORE = 0;
static const int WAKE_PRIO = 1;

static void mn_unload() {
    s_cmd_listen = false;
    s_cap_extend = false;
    if (s_mnd) {
        esp_mn_commands_free();
        s_mn->destroy(s_mnd);
    }
    s_mnd = nullptr;
    s_mn = nullptr;
}

// After the front end: MultiNet on its output, the commands above.
static bool mn_load(srmodel_list_t *models) {
    if (!s_mn_dir[0]) return false;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 1536 * 1024 || heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 48 * 1024) {
        DEBUG_PRINTF("[voice] not enough memory for the commands\n");
        return false;
    }
    int idx = esp_srmodel_exists(models, s_mn_dir);
    if (idx < 0) return false;
    char *name = models->model_name[idx];
    const char *info = models->model_info ? models->model_info[idx] : nullptr;
    if (info && !strstr(info, "MN7") && !strstr(info, "mn7")) {   // "MN7_v2_english_8_0.9_0.90"
        DEBUG_PRINTF("[voice] %s isn't a MultiNet7 model\n", s_mn_dir);
        return false;
    }
    s_mn = &esp_sr_multinet7_quantized;
    s_mnd = s_mn->create(name, CMD_LISTEN_MS);
    if (!s_mnd) { s_mn = nullptr; return false; }
    if (s_mn->get_samp_chunksize(s_mnd) != s_ifc->get_fetch_chunksize(s_afe)) {
        DEBUG_PRINTF("[voice] command model doesn't fit the front end\n");
        s_mn->destroy(s_mnd);
        s_mnd = nullptr;
        s_mn = nullptr;
        return false;
    }
    esp_mn_commands_alloc(s_mn, s_mnd);
    for (const CmdPhrase &p : CMD_PHRASES) esp_mn_commands_phoneme_add(p.id, p.text, p.phonemes);
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err && err->num > 0) DEBUG_PRINTF("[voice] %d command phrases not accepted\n", err->num);
    if (!s_cap_buf)
        s_cap_buf = (int16_t *)heap_caps_malloc(CAP_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    DEBUG_PRINTF("[voice] commands ready (%s)\n", s_mn_dir);
    return true;
}

static bool engine_load() {
    // The engine's own allocations are mostly PSRAM, but some must be
    // internal - and Wi-Fi/Bluetooth need theirs.
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 64 * 1024) {
        DEBUG_PRINTF("[voice] not enough internal RAM for the wake word\n");
        return false;
    }
    srmodel_list_t *models = model_list();
    if (!models) return false;
    int idx = esp_srmodel_exists(models, s_want_model);
    if (idx < 0) return false;

    afe_config_t *cfg = afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) return false;
    cfg->aec_init = false;                  // no speaker reference channel
    cfg->vad_init = false;
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = strdup(models->model_name[idx]);   // its own copy: afe_config_free() may free it
    cfg->wakenet_model_name_2 = nullptr;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = WAKE_CORE;     // its own separation task
    cfg->afe_perferred_priority = WAKE_PRIO;
    cfg = afe_config_check(cfg);
    if (!cfg) return false;
    s_ifc = esp_afe_handle_from_config(cfg);
    s_afe = s_ifc ? s_ifc->create_from_config(cfg) : nullptr;
    afe_config_free(cfg);
    if (!s_afe) return false;

    s_feed_n = s_ifc->get_feed_chunksize(s_afe);
    s_feed_ch = s_ifc->get_feed_channel_num(s_afe);
    s_feed_buf = (int16_t *)heap_caps_malloc((size_t)s_feed_n * s_feed_ch * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_feed_buf || s_feed_n <= 0 || s_feed_ch < 1 || s_feed_ch > 2) {
        s_ifc->destroy(s_afe);
        s_afe = nullptr;
        free(s_feed_buf);
        s_feed_buf = nullptr;
        return false;
    }
    strlcpy(s_loaded_model, s_want_model, sizeof(s_loaded_model));
    s_cmds_loaded = s_want_cmds;
    s_cmds_failed = s_cmds_loaded && !mn_load(models);
    DEBUG_PRINTF("[voice] wake word '%s' loaded: %d samples x %d ch per feed\n", s_loaded_model, s_feed_n, s_feed_ch);
    return true;
}

static void engine_unload() {
    s_feed_on = false;
    while (s_feed_busy) vTaskDelay(pdMS_TO_TICKS(5));
    mn_unload();
    if (s_afe) s_ifc->destroy(s_afe);
    s_afe = nullptr;
    free(s_feed_buf);
    s_feed_buf = nullptr;
    s_loaded_model[0] = 0;
    DEBUG_PRINTF("[voice] wake word unloaded\n");
}

static void wake_ctl(void *) {
    for (;;) {
        bool want = s_want;
        if (s_afe && (!want || strcmp(s_loaded_model, s_want_model) || s_cmds_loaded != s_want_cmds)) engine_unload();
        if (!s_afe && want && strcmp(s_failed_model, s_want_model)) {
            s_state = VW_LOADING;
            if (engine_load()) {
                s_failed_model[0] = 0;
                s_feed_on = true;
            } else {
                strlcpy(s_failed_model, s_want_model, sizeof(s_failed_model));
                DEBUG_PRINTF("[voice] couldn't start the wake word '%s'\n", s_want_model);
            }
        }
        if (!s_afe) {
            s_state = want ? VW_FAILED : VW_OFF;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        s_state = s_pause ? VW_PAUSED : VW_LISTENING;
        if ((s_cmd_listen || s_cap_extend) && s_pause) {
            // Something else took the microphone: no command this time.
            s_cmd_listen = false;
            s_cap_extend = false;
            s_ifc->enable_wakenet(s_afe);
        }
        afe_fetch_result_t *r = s_ifc->fetch_with_delay(s_afe, pdMS_TO_TICKS(100));
        if (!r || r->ret_value == ESP_FAIL) continue;
        if (s_cap_extend) {
            // No command: the rest of the question, until a pause.
            if (r->data && r->data_size > 0) s_cap.add((const int16_t *)r->data, r->data_size / 2);
            if (!s_cap.still_talking() || s_cap.full()) {
                s_cap_extend = false;
                s_ifc->enable_wakenet(s_afe);
                s_cap_ready = true;
                s_cmd_result = -1;
            }
            continue;
        }
        if (s_cmd_listen) {
            if (!r->data || r->data_size <= 0) continue;
            if (s_cap_on) s_cap.add((const int16_t *)r->data, r->data_size / 2);
            esp_mn_state_t st = s_mn->detect(s_mnd, r->data);
            if (st == ESP_MN_STATE_DETECTING) continue;
            int cmd = -1;
            if (st == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *res = s_mn->get_results(s_mnd);
                if (res && res->num > 0) cmd = res->command_id[0];
                DEBUG_PRINTF("[voice] command %d (%s)\n", cmd, res ? res->string : "");
            }
            s_cmd_listen = false;
            if (cmd < 0 && s_cap_on && s_cap.spoke()) {
                if (s_cap.still_talking() && !s_cap.full()) { s_cap_extend = true; continue; }
                s_cap_ready = true;          // the question, all of it
            }
            s_ifc->enable_wakenet(s_afe);
            s_cmd_result = cmd;
        } else if (r->wakeup_state == WAKENET_DETECTED) {
            DEBUG_PRINTF("[voice] wake word heard\n");
            if (s_mnd) {
                s_mn->clean(s_mnd);
                s_ifc->disable_wakenet(s_afe);
                // The main loop may still be saving the last question: then none this time.
                s_cap_on = s_cap_buf && !s_cap_ready;
                if (s_cap_on) s_cap.begin(s_cap_buf, CAP_SAMPLES);
                s_cmd_listen = true;
            }
            s_detected = true;
        }
    }
}

static void wake_feed(void *) {
    static int16_t rd[256 * 2];
    int fill = 0;
    bool was_idle = true;
    for (;;) {
        if (!s_feed_on || s_pause) { was_idle = true; vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        s_feed_busy = true;
        if (!s_feed_on) { s_feed_busy = false; continue; }
        if (was_idle) {
            // Nothing stale from before the pause: that could be heard as the word.
            s_ifc->reset_buffer(s_afe);
            fill = 0;
            was_idle = false;
        }
        int frames = audio_mic_read(rd, 256, 100);
        for (int i = 0; i < frames; i++) {
            if (s_feed_ch == 2) {
                s_feed_buf[fill * 2] = rd[i * 2];
                s_feed_buf[fill * 2 + 1] = rd[i * 2 + 1];
            } else {
                s_feed_buf[fill] = (int16_t)(((int32_t)rd[i * 2] + rd[i * 2 + 1]) / 2);
            }
            if (++fill == s_feed_n) {
                s_ifc->feed(s_afe, s_feed_buf);
                fill = 0;
            }
        }
        s_feed_busy = false;
        if (frames <= 0) vTaskDelay(pdMS_TO_TICKS(20));   // the microphone is someone else's right now
    }
}

static bool start_tasks() {
    if (s_ctl_task) return true;
    // Stacks in PSRAM like rec_task's: internal RAM is what Wi-Fi runs short of.
    static StaticTask_t ctl_tcb, feed_tcb;
    StackType_t *ctl_stack = (StackType_t *)heap_caps_malloc(12 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StackType_t *feed_stack = (StackType_t *)heap_caps_malloc(6 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctl_stack || !feed_stack) { free(ctl_stack); free(feed_stack); return false; }
    s_ctl_task = xTaskCreateStaticPinnedToCore(wake_ctl, "wake_ctl", 12 * 1024, nullptr, WAKE_PRIO, ctl_stack, &ctl_tcb, WAKE_CORE);
    s_feed_task = xTaskCreateStaticPinnedToCore(wake_feed, "wake_feed", 6 * 1024, nullptr, WAKE_PRIO, feed_stack, &feed_tcb, WAKE_CORE);
    return s_ctl_task && s_feed_task;
}

// ---- voice cleanup for the mono recordings ------------------------------------------
static const esp_afe_sr_iface_t *s_vc_ifc = nullptr;
static esp_afe_sr_data_t *s_vc = nullptr;
static int16_t *s_vc_buf = nullptr;
static int s_vc_n = 0, s_vc_fill = 0;
static bool s_vc_broken = false;        // created badly or gave nothing back: plain recordings from now on
static bool s_vc_bypass = false;
static uint32_t s_vc_in = 0, s_vc_out = 0;

static bool vc_create() {
    afe_config_t *cfg = afe_config_init("M", nullptr, AFE_TYPE_VC, AFE_MODE_LOW_COST);
    if (!cfg) return false;
    cfg->aec_init = false;
    cfg->se_init = false;       // one channel: the two microphones are already summed
    cfg->vad_init = false;
    cfg->wakenet_init = false;
    cfg->ns_init = true;        // WebRTC noise suppression
    cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    cfg->agc_init = true;
    cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg = afe_config_check(cfg);
    if (!cfg) return false;
    s_vc_ifc = esp_afe_handle_from_config(cfg);
    s_vc = s_vc_ifc ? s_vc_ifc->create_from_config(cfg) : nullptr;
    afe_config_free(cfg);
    if (!s_vc) return false;
    s_vc_n = s_vc_ifc->get_feed_chunksize(s_vc);
    if (s_vc_n <= 0 || s_vc_ifc->get_feed_channel_num(s_vc) != 1) return false;
    s_vc_buf = (int16_t *)heap_caps_malloc(s_vc_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    DEBUG_PRINTF("[voice] cleanup ready: %d samples per feed\n", s_vc_n);
    return s_vc_buf != nullptr;
}

static bool vc_begin() {
    if (!g_app_settings.voice_clean || s_vc_broken) return false;
    if (!s_vc) {
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 48 * 1024 || !vc_create()) {
            DEBUG_PRINTF("[voice] cleanup unavailable, recording as is\n");
            s_vc_broken = true;
            return false;
        }
    } else {
        s_vc_ifc->reset_buffer(s_vc);
    }
    s_vc_fill = 0;
    s_vc_in = s_vc_out = 0;
    s_vc_bypass = false;
    return true;
}

static int vc_process(const int16_t *in, int n, int16_t *out, int cap) {
    if (s_vc_bypass) {
        int k = n < cap ? n : cap;
        memcpy(out, in, k * sizeof(int16_t));
        return k;
    }
    int produced = 0;
    for (int i = 0; i < n; i++) {
        s_vc_buf[s_vc_fill++] = in[i];
        if (s_vc_fill < s_vc_n) continue;
        s_vc_fill = 0;
        s_vc_ifc->feed(s_vc, s_vc_buf);
        for (;;) {
            afe_fetch_result_t *r = s_vc_ifc->fetch_with_delay(s_vc, 0);
            if (!r || r->ret_value == ESP_FAIL || r->data_size <= 0 || !r->data) break;
            int k = r->data_size / (int)sizeof(int16_t);
            if (k > cap - produced) k = cap - produced;
            memcpy(out + produced, r->data, k * sizeof(int16_t));
            produced += k;
            if (produced >= cap) break;
        }
    }
    s_vc_in += n;
    s_vc_out += produced;
    if (s_vc_in > 16000 && s_vc_out == 0) {
        // A second of voice in and nothing out: don't lose the recording.
        DEBUG_PRINTF("[voice] cleanup gave no audio - recording as is\n");
        s_vc_broken = s_vc_bypass = true;
        int k = n < cap ? n : cap;
        memcpy(out, in, k * sizeof(int16_t));
        return k;
    }
    return produced;
}

static void vc_end() {}

static const AudioMonoFilter s_vc_filter = { vc_begin, vc_process, vc_end };

// ---- running a command (main loop) -----------------------------------------------
static bool need_phone() {
    if (phone_link_active()) return true;
    ui_show_toast("Phone not connected", 2000);
    return false;
}

static void open_screen(Screen *s) {
    if (ui_top_panel_is_open()) ui_top_panel_close();
    if (!ui_screen_on_top(s)) ui_push(s);
}

static void run_command(int cmd) {
    char buf[48];
    vibrate_ms(25);
    if (cmd >= CMD_TIMER && cmd <= CMD_TIMER + 180) {
        int min = cmd - CMD_TIMER;
        assistant_timer_set((uint32_t)min * 60);
        if (min % 60 == 0) snprintf(buf, sizeof(buf), "Timer: %d h", min / 60);
        else if (min > 60) snprintf(buf, sizeof(buf), "Timer: %d h %d min", min / 60, min % 60);
        else snprintf(buf, sizeof(buf), "Timer: %d min", min);
        ui_show_toast(buf, 2500);
        return;
    }
    const PhoneMedia &m = phone_link_media();
    switch (cmd) {
    case CMD_TIME: {
        WatchTime t = rtc_now();
        ui_go_home();
        char hm[16];
        clock_hm_text(hm, sizeof(hm), t.hour, t.minute);
        snprintf(buf, sizeof(buf), "It's %s", hm);
        ui_show_toast(buf, 3000);
        break;
    }
    case CMD_NEXT: if (need_phone()) { phone_link_media_cmd("next"); ui_show_toast("Next song", 1500); } break;
    case CMD_PREV: if (need_phone()) { phone_link_media_cmd("prev"); ui_show_toast("Previous song", 1500); } break;
    case CMD_PAUSE:
        if (need_phone()) {
            if (m.playing) phone_link_media_cmd("toggle");
            ui_show_toast("Paused", 1500);
        }
        break;
    case CMD_PLAY:
        if (need_phone()) {
            if (!m.playing) phone_link_media_cmd("toggle");
            ui_show_toast("Playing", 1500);
        }
        break;
    case CMD_VOL_UP: if (need_phone()) { phone_link_media_cmd("volUp"); ui_show_toast("Volume up", 1200); } break;
    case CMD_VOL_DOWN: if (need_phone()) { phone_link_media_cmd("volDown"); ui_show_toast("Volume down", 1200); } break;
    case CMD_TIMER_CANCEL:
        if (assistant_timer_active()) { assistant_timer_set(0); ui_show_toast("Timer cancelled", 2000); }
        else ui_show_toast("No timer running", 2000);
        break;
    case CMD_FIND_PHONE: if (need_phone()) { phone_link_find_phone(true); ui_show_toast("Ringing your phone", 2000); } break;
    case CMD_DND_ON: dnd_set(true, false); ui_show_toast("Do not disturb on", 2000); break;
    case CMD_DND_OFF: dnd_set(false, false); ui_show_toast("Do not disturb off", 2000); break;
    case CMD_HOME: if (ui_top_panel_is_open()) ui_top_panel_close(); ui_go_home(); break;
    case CMD_SETTINGS: open_screen(&settings_screen); break;
    case CMD_NOTIFICATIONS: open_screen(&notifications_screen); break;
    case CMD_RECORDER: open_screen(&recorder_screen); break;
    case CMD_SLEEP: open_screen(&sleep_screen); break;
    case CMD_MUSIC: open_screen(&media_screen); break;
    case CMD_ASSISTANT:
    default:
        open_screen(&assistant_screen);   // not a command: ask the assistant
        break;
    }
}

// ---- public ------------------------------------------------------------------------
void voice_init() {
    audio_set_mono_filter(&s_vc_filter);
}

void voice_update() {
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (now - last_ms < 100) return;
    last_ms = now;

    bool on = g_app_settings.voice_wake;
    if (on && !s_scanned) scan_models();
    int m = chosen_model();
    bool card_ok = sd_is_mounted() && !usb_msc_host_active();
    bool want = on && m >= 0 && card_ok;
    s_want_cmds = want && g_app_settings.voice_commands && s_mn_dir[0];
    if (want) {
        strlcpy(s_want_model, s_models[m].dir, sizeof(s_want_model));
        if (!start_tasks()) want = false;
    }
    s_want = want;

    bool asleep = sleep_is_asleep();
    bool pause = !want || audio_is_recording() || audio_is_playing() || ui_screen_on_top(&assistant_screen) ||
                 (asleep && !g_app_settings.voice_wake_asleep);
    s_pause = pause;
    audio_mic_hold(want && !pause && s_afe);

    if (s_detected) {
        s_detected = false;
        if (!pause) {
            sleep_register_activity();      // wakes the screen
            if (s_cmd_listen) {
                vibrate_ms(20);
                ui_show_toast("Listening...", CMD_LISTEN_MS);
            } else {
                ui_push(&assistant_screen); // starts listening for the question
            }
        }
    }
    int cmd = s_cmd_result;
    if (cmd != CMD_NONE) {
        s_cmd_result = CMD_NONE;
        if (!pause) {
            sleep_register_activity();
            if (cmd >= 0) {
                run_command(cmd);
            } else if (s_cap_ready && s_cap.samples() > VoiceCapture::RATE / 2) {
                // Not a command, but something was said: that's the question.
                s_cap.normalize();
                if (!assistant_ask_recorded(s_cap.data(), s_cap.samples(), VoiceCapture::RATE))
                    ui_push(&assistant_screen);   // no phone / no card: the assistant says why
            } else {
                ui_push(&assistant_screen);       // nothing said: the assistant listens instead
            }
        }
        s_cap_ready = false;   // wake_ctl may use the buffer again
    }
}

VoiceWakeState voice_wake_state() {
    if (!g_app_settings.voice_wake) return VW_OFF;
    if (!s_scanned) scan_models();
    if (!s_model_n) return VW_NO_MODEL;
    if (!s_want) return s_ctl_task ? VW_PAUSED : VW_LOADING;
    return s_state == VW_OFF ? VW_LOADING : s_state;
}

const char *voice_wake_word() {
    if (!s_scanned) scan_models();
    int m = chosen_model();
    return m >= 0 ? s_models[m].word : "";
}

int voice_wake_model_count() {
    if (!s_scanned) scan_models();
    return s_model_n;
}

void voice_wake_next_model() {
    scan_models();
    if (!s_model_n) return;
    int m = chosen_model();
    m = (m + 1) % s_model_n;
    strlcpy(g_app_settings.wake_model, s_models[m].dir, sizeof(g_app_settings.wake_model));
    nvs_save_settings(g_app_settings);
}

bool voice_wake_keeps_awake() {
    return s_want && !s_pause && g_app_settings.voice_wake_asleep;
}

const char *voice_wake_status() {
    static char buf[48];
    switch (voice_wake_state()) {
    case VW_OFF: return "Opens the assistant (uses more battery)";
    case VW_NO_MODEL: return "Install one from the companion app";
    case VW_LOADING: return "Loading...";
    case VW_FAILED: return "Couldn't start: not enough memory";
    case VW_PAUSED:
        snprintf(buf, sizeof(buf), "Say \"%s\" (paused)", voice_wake_word());
        return buf;
    case VW_LISTENING:
        snprintf(buf, sizeof(buf), "Say \"%s\"", voice_wake_word());
        return buf;
    default: return "";
    }
}

bool voice_commands_installed() {
    if (!s_scanned) scan_models();
    return s_mn_dir[0] != 0;
}

const char *voice_commands_status() {
    if (!voice_commands_installed()) return "Install them from the companion app (2.7 MB)";
    if (!g_app_settings.voice_commands) return "After the wake word: next song, timers...";
    if (!g_app_settings.voice_wake) return "Turn on the wake word to use them";
    if (s_cmds_failed && s_afe) return "Couldn't start: not enough memory";
    return "Say the word, then e.g. \"next song\"";
}

bool voice_model_file(const char *name, const uint8_t *data, int len) {
    // "<model>/<file>", only the three files a WakeNet or MultiNet model has.
    const char *slash = strchr(name, '/');
    if (!slash || slash - name >= (int)sizeof(WakeModel::dir)) return false;
    char dir[sizeof(WakeModel::dir)];
    memcpy(dir, name, slash - name);
    dir[slash - name] = 0;
    const char *file = slash + 1;
    if (!model_dir_name_ok(dir)) return false;
    bool mn = !strncmp(dir, "mn7", 3);
    if (strcmp(file, "_MODEL_INFO_") && strcmp(file, mn ? "mn7_data" : "wn9_data") && strcmp(file, mn ? "mn7_index" : "wn9_index"))
        return false;
    if (!sd_is_mounted() || usb_msc_host_active() || len <= 0) return false;
    SD.mkdir(SD_DIR);
    SD.mkdir(String(SD_DIR) + "/" + dir);
    String path = model_path(dir, file);
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    bool ok = f.write(data, len) == (size_t)len;
    f.close();
    if (!ok) { SD.remove(path); return false; }
    s_scanned = false;      // seen on the next look
    s_failed_model[0] = 0;  // and worth another try
    return true;
}

#else // no ESP-SR in this build -------------------------------------------------------

void voice_init() {}
void voice_update() {}
VoiceWakeState voice_wake_state() { return VW_UNSUPPORTED; }
const char *voice_wake_status() { return "Not in this build"; }
const char *voice_wake_word() { return ""; }
int voice_wake_model_count() { return 0; }
void voice_wake_next_model() {}
bool voice_wake_keeps_awake() { return false; }
bool voice_model_file(const char *, const uint8_t *, int) { return false; }
bool voice_commands_installed() { return false; }
const char *voice_commands_status() { return "Not in this build"; }

#endif
