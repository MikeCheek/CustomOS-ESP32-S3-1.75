// The Casino's shared credit - see casino_wallet.h.
#include "casino_wallet.h"
#include "hal_save.h"
#include <string.h>

#define WALLET_MAGIC 0x43534e31u   // "CSN1"
struct WalletSave { uint32_t magic; int32_t credit; };
// what the games kept before they shared a wallet
struct OldSave { uint32_t magic; int32_t money; };

static bool s_loaded = false;
static int32_t s_credit = CASINO_START;

static void save() {
    WalletSave w = {WALLET_MAGIC, s_credit};
    game_save_blob("casino_wallet", &w, sizeof(w));
}

static void load() {
    if (s_loaded) return;
    s_loaded = true;
    WalletSave w;
    if (game_load_blob("casino_wallet", &w, sizeof(w)) && w.magic == WALLET_MAGIC) {
        s_credit = w.credit;
        return;
    }
    int32_t sum = 0;
    bool any = false;
    OldSave o;
    if (game_load_blob("cavern_money", &o, sizeof(o)) && o.magic == 0x43415633u) { sum += o.money; any = true; }
    if (game_load_blob("blackjack_money", &o, sizeof(o)) && o.magic == 0x424a4b32u) { sum += o.money; any = true; }
    s_credit = any ? (sum < CASINO_TOP_UP ? CASINO_TOP_UP : sum) : CASINO_START;
    save();
}

int32_t casino_credit() { load(); return s_credit; }

void casino_set(int32_t credit) {
    load();
    if (credit == s_credit) return;
    s_credit = credit;
    save();
}

int32_t casino_top_up() {
    load();
    if (s_credit < CASINO_MIN_BET) { s_credit = CASINO_TOP_UP; save(); }
    return s_credit;
}

// ---- stats and the daily bonus ------------------------------------------------------------
#define STATS_MAGIC 0x43535431u   // "CST1"
struct StatsSave { uint32_t magic; CasinoGameStats g[CG_COUNT]; uint8_t best_hand; uint32_t bonus_day; };
static StatsSave s_st;
static bool s_st_loaded = false;

static void st_load() {
    if (s_st_loaded) return;
    s_st_loaded = true;
    if (game_load_blob("casino_stats", &s_st, sizeof(s_st)) && s_st.magic == STATS_MAGIC) return;
    memset(&s_st, 0, sizeof(s_st));
    s_st.magic = STATS_MAGIC;
    s_st.best_hand = 255;
}
static void st_save() { game_save_blob("casino_stats", &s_st, sizeof(s_st)); }

const char *casino_game_name(CasinoGame g) {
    static const char *const N[CG_COUNT] = {"Cavern slots", "Blackjack", "Video poker", "Roulette", "Hi-Lo dice"};
    return g < CG_COUNT ? N[g] : "";
}

void casino_record(CasinoGame g, int32_t net) {
    if (g >= CG_COUNT) return;
    st_load();
    CasinoGameStats &s = s_st.g[g];
    s.rounds++;
    s.net += net;
    if (net > s.best) s.best = net;
    st_save();
}

CasinoGameStats casino_stats(CasinoGame g) {
    st_load();
    return g < CG_COUNT ? s_st.g[g] : CasinoGameStats{0, 0, 0};
}

void casino_record_poker_hand(uint8_t hand) {
    st_load();
    if (hand < s_st.best_hand) { s_st.best_hand = hand; st_save(); }
}
uint8_t casino_best_poker_hand() { st_load(); return s_st.best_hand; }

bool casino_daily_bonus_ready(uint32_t day) { st_load(); return day > 20240000u && s_st.bonus_day != day; }

int32_t casino_daily_bonus(uint32_t day) {
    if (!casino_daily_bonus_ready(day)) return 0;
    s_st.bonus_day = day;
    st_save();
    casino_set(casino_credit() + CASINO_DAILY_BONUS);
    return CASINO_DAILY_BONUS;
}

void casino_stats_reset() {
    st_load();
    memset(&s_st, 0, sizeof(s_st));
    s_st.magic = STATS_MAGIC;
    s_st.best_hand = 255;
    st_save();
}
