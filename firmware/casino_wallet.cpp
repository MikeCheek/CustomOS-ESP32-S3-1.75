// The Casino's shared credit - see casino_wallet.h.
#include "casino_wallet.h"
#include "hal_save.h"

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
