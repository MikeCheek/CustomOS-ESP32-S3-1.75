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
#include "model_path.h"

// Both in libespressif__esp-sr.a, not in its headers.
extern "C" srmodel_list_t *srmodel_sdcard_init(const char *base_path);
extern "C" char *get_model_info(char *data, int size);

static const char *SD_DIR = "/srmodels";              // as the SD library sees it
static const char *VFS_DIR = "/sd/srmodels";          // as fopen() sees it (ESP-SR keeps this pointer)
static const int MAX_MODELS = 12;

// ---- installed models ----------------------------------------------------------------
struct WakeModel { char dir[24]; char word[20]; };
static WakeModel s_models[MAX_MODELS];
static int s_model_n = 0;
static bool s_scanned = false;

static bool model_dir_name_ok(const char *n) {
    int l = strlen(n);
    if (l < 5 || l >= (int)sizeof(WakeModel::dir) || strncmp(n, "wn9", 3)) return false;
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

static srmodel_list_t *model_list() {
    if (!s_list) s_list = srmodel_sdcard_init(VFS_DIR);
    if (!s_list) return nullptr;
    // Words installed since the list was read (ESP-SR never re-reads it).
    for (int i = 0; i < s_model_n; i++) {
        if (esp_srmodel_exists(s_list, s_models[i].dir) >= 0) continue;
        char raw[256];
        if (!read_info(s_models[i].dir, raw, sizeof(raw))) continue;
        int n = s_list->num;
        char **names = (char **)realloc(s_list->model_name, (n + 1) * sizeof(char *));
        if (!names) break;
        s_list->model_name = names;
        char **infos = (char **)realloc(s_list->model_info, (n + 1) * sizeof(char *));
        if (!infos) break;
        s_list->model_info = infos;
        names[n] = (char *)calloc(MODEL_NAME_MAX_LENGTH, 1);
        if (!names[n]) break;
        strlcpy(names[n], s_models[i].dir, MODEL_NAME_MAX_LENGTH);
        infos[n] = get_model_info(raw, strlen(raw));
        s_list->num = n + 1;
    }
    return s_list;
}

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
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 4;
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
    DEBUG_PRINTF("[voice] wake word '%s' loaded: %d samples x %d ch per feed\n", s_loaded_model, s_feed_n, s_feed_ch);
    return true;
}

static void engine_unload() {
    s_feed_on = false;
    while (s_feed_busy) vTaskDelay(pdMS_TO_TICKS(5));
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
        if (s_afe && (!want || strcmp(s_loaded_model, s_want_model))) engine_unload();
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
        afe_fetch_result_t *r = s_ifc->fetch_with_delay(s_afe, pdMS_TO_TICKS(100));
        if (r && r->ret_value != ESP_FAIL && r->wakeup_state == WAKENET_DETECTED) {
            DEBUG_PRINTF("[voice] wake word heard\n");
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
    s_ctl_task = xTaskCreateStaticPinnedToCore(wake_ctl, "wake_ctl", 12 * 1024, nullptr, 4, ctl_stack, &ctl_tcb, 1);
    s_feed_task = xTaskCreateStaticPinnedToCore(wake_feed, "wake_feed", 6 * 1024, nullptr, 5, feed_stack, &feed_tcb, 1);
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
            ui_push(&assistant_screen);     // starts listening for the question
        }
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

bool voice_model_file(const char *name, const uint8_t *data, int len) {
    // "<model>/<file>", only the three files a WakeNet model has.
    const char *slash = strchr(name, '/');
    if (!slash || slash - name >= (int)sizeof(WakeModel::dir)) return false;
    char dir[sizeof(WakeModel::dir)];
    memcpy(dir, name, slash - name);
    dir[slash - name] = 0;
    const char *file = slash + 1;
    if (!model_dir_name_ok(dir)) return false;
    if (strcmp(file, "_MODEL_INFO_") && strcmp(file, "wn9_data") && strcmp(file, "wn9_index")) return false;
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

#endif
