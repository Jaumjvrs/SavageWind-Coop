// Part of runtime/coop.c: the Champion abilities seen on the other players' avatars (protocol 89, coop-champ;
// audit A5). SAVAGEWIND_COOP_CHAMP=0: off (nothing sent, nothing shown).
//
// How the game shows them (RE 2026-11-19, Ghidra + disassembly of 1.9.0; offsets on the player actor):
// - All four abilities are player code. The visible effects are the player's own ELink keys (effect set
//   Player_GameRom, which the avatar Dm_CoopLinkF has too: coop/avatarfx.inc), fired with FUN_7101100bd0(out handle,
//   player, key) and the sound with FUN_7101395354. The Champion apparitions are separate actors
//   (Dm_Npc_<race>_HeroSoul_Kago, TitleBG.pack) the player moves to itself through actor links at +0x2c58 (Urbosa),
//   +0x2c68 (Daruk), +0x2c78 (Revali), +0x2c88 (Mipha); not copied here (yet).
// - Persistent effects, kept alive by the player's per-frame update (0x7100cd39f0..): +0x2350 "Kago_Shield" and
//   +0x2360 "Kago_ShieldCount_<n>" (n = Daruk's shield count +0x1cd4) while vtable+0xae0 says Daruk's shield is up
//   (guarding with the ability ready); +0x2380 "Kago_Ready"; +0x2370 "Kago_Range" (+0xc54 bit 6). Each handle is
//   {effect*, generation}: live when effect+0x20 == the stored generation (elink_live_effect).
// - One-shots: Urbosa's Fury FUN_7100cdde1c: count +0x1cd0 - 1 (at 0: refilled, recharge timer +0x1e10 = s * 30),
//   then "Kago_Lightning". Daruk's shield taking a hit FUN_7100cddf48: "Kago_ShieldBrokenCount_<count>", count +0x1cd4
//   - 1 (at 0: refilled, timer +0x1e04). Mipha's Grace FUN_7100cead24: timer +0x1e1c = s * 30, "Kago_Heal". Revali's
//   Gale: entering the state (0x7100c973e0) takes count +0x1ccc - 1 (at 0: refilled, timer +0x1df8); its update sets
//   +0x1cbe = 1 at the launch and fires "Kago_Wind" (0x7100c97a90..0x7100c97afc).
// The sender watches those counters / timers / handles and sends them in the tree packet (CoopAsTree.champ). The
// receiver keeps the same persistent keys alive on the avatar (own handles) and fires the one-shots (effect + sound).
// Charges, cooldowns and damage stay each player's own: Urbosa's strike hurts enemies in the attacker's game, and its
// hits reach shared enemies through the normal enemy damage sync (counted once).
// Dev: `coop champ probe` logs our counters; `coop champ fake <shield|count N|range|ready|urbosa|mipha|revali|break N|
// off> [ms]` sends a fake (no ability needed); `coop champ native urbosa` runs the game's own Urbosa's Fury call.

#define A_CHAMP_URBOSA_FIRE 0x7000cdde1cull  // FUN_7100cdde1c (dev `coop champ native urbosa`)
#define CHAMP_P_URB_N 0x1cd0
#define CHAMP_P_DAR_N 0x1cd4
#define CHAMP_P_REV_N 0x1ccc
#define CHAMP_P_REV_LAUNCH 0x1cbe
#define CHAMP_P_URB_T 0x1e10
#define CHAMP_P_DAR_T 0x1e04
#define CHAMP_P_REV_T 0x1df8
#define CHAMP_P_MIP_T 0x1e1c
static const unsigned champ_held_off[4] = {0x2350, 0x2360, 0x2370, 0x2380};
static const char *const champ_use_name[COOP_CHAMP_USES] = {"Urbosa's Fury", "Mipha's Grace", "Revali's Gale",
                                                            "Daruk's shield hit"};

static bool champ_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_CHAMP");
    return on;
}

// ---- the sender (game thread, from astree_publish)
static struct {
    uint64_t pa;
    unsigned frames;  // frames with the same player actor
    int32_t urb_n, dar_n, rev_n;
    float urb_t, dar_t, rev_t, mip_t;
    uint8_t launch;
    uint64_t rev_start_ns;
    uint8_t use[COOP_CHAMP_USES];
    uint8_t break_lv, held_was;
    unsigned said;
} champ_tx;
static struct {
    volatile int probe, native;
    volatile uint16_t held;  // fake persistent bits (COOP_CHAMP_HELD | count << shift)
    volatile uint64_t until_ns;
    volatile int use;        // fake use: kind + 1
    volatile uint8_t lv;
} champ_dev;

static void champ_use(int k, uint8_t lv) {
    champ_tx.use[k] = (uint8_t)((champ_tx.use[k] + 1) & 3);
    if (k == COOP_CHAMP_BREAK) champ_tx.break_lv = lv;
    if (champ_tx.said++ < 60) LOG("[coop] champ: we used %s%s (use %u)\n", champ_use_name[k],
                                  k == COOP_CHAMP_BREAK ? (lv == 3 ? " (count 3)" : lv == 2 ? " (count 2)" : " (count 1)") : "",
                                  champ_tx.use[k]);
}
// A counter use: count down by one, or the timer set (the last charge used: count refilled, recharge started).
static bool champ_counted(int32_t n_was, int32_t n, float t_was, float t) {
    return (n_was > 0 && n == n_was - 1) || (isfinite(t) && isfinite(t_was) && t > t_was + 30.0f);
}

static void champ_publish(Cpu *c, uint64_t pa, CoopAsTree *t) {
    t->champ = 0;
    if (!champ_on() || !in_guest(pa) || !(t->flags & COOP_ASTREE_VALID)) { champ_tx.pa = 0; return; }
    uint64_t now = plat_now_ns();
    if (champ_dev.native) {
        champ_dev.native = 0;
        Cpu saved = *c;
        c->x[0] = pa;
        guest_call(c, A_CHAMP_URBOSA_FIRE);
        *c = saved;
        LOG("[coop] champ: dev: the game's own Urbosa's Fury call done (count now %d)\n", (int32_t)LD32(pa + CHAMP_P_URB_N));
    }
    int32_t urb_n = (int32_t)LD32(pa + CHAMP_P_URB_N), dar_n = (int32_t)LD32(pa + CHAMP_P_DAR_N),
            rev_n = (int32_t)LD32(pa + CHAMP_P_REV_N);
    float urb_t = LDF32(pa + CHAMP_P_URB_T), dar_t = LDF32(pa + CHAMP_P_DAR_T), rev_t = LDF32(pa + CHAMP_P_REV_T),
          mip_t = LDF32(pa + CHAMP_P_MIP_T);
    uint8_t launch = LD8(pa + CHAMP_P_REV_LAUNCH);
    if (champ_dev.probe) {
        champ_dev.probe = 0;
        LOG("[coop] champ: probe: Urbosa %d (timer %.0f) Daruk %d (timer %.0f) Revali %d (timer %.0f, launch %u) "
            "Mipha timer %.0f; shown:%s%s%s%s\n", urb_n, urb_t, dar_n, dar_t, rev_n, rev_t, launch, mip_t,
            elink_live_effect(pa + 0x2350) ? " Kago_Shield" : "", elink_live_effect(pa + 0x2360) ? " Kago_ShieldCount" : "",
            elink_live_effect(pa + 0x2370) ? " Kago_Range" : "", elink_live_effect(pa + 0x2380) ? " Kago_Ready" : "");
    }
    if (pa != champ_tx.pa) champ_tx.pa = pa, champ_tx.frames = 0, champ_tx.rev_start_ns = 0;
    // (the first 2 s with a player: its values come from the save at creation, not from a use)
    if (champ_tx.frames++ > 120) {
        if (champ_counted(champ_tx.urb_n, urb_n, champ_tx.urb_t, urb_t)) champ_use(COOP_CHAMP_URBOSA, 0);
        if (champ_counted(champ_tx.dar_n, dar_n, champ_tx.dar_t, dar_t))
            champ_use(COOP_CHAMP_BREAK, (uint8_t)(champ_tx.dar_n >= 1 && champ_tx.dar_n <= 3 ? champ_tx.dar_n : 1));
        if (isfinite(mip_t) && isfinite(champ_tx.mip_t) && mip_t > champ_tx.mip_t + 30.0f) champ_use(COOP_CHAMP_MIPHA, 0);
        if (champ_counted(champ_tx.rev_n, rev_n, champ_tx.rev_t, rev_t)) {
            champ_tx.rev_start_ns = now;
            LOG("[coop] champ: Revali's Gale starts (count %d -> %d)\n", champ_tx.rev_n, rev_n);
        }
        // The launch: +0x1cbe set in the Gale state; without it in 2 s (another use of that byte?), launch anyway.
        if (champ_tx.rev_start_ns && ((launch && !champ_tx.launch) || now - champ_tx.rev_start_ns > 2000000000ull)) {
            if (!(launch && !champ_tx.launch)) LOG("[coop] champ: Revali's Gale: no launch flag seen in 2 s\n");
            champ_tx.rev_start_ns = 0;
            champ_use(COOP_CHAMP_REVALI, 0);
        }
    }
    champ_tx.urb_n = urb_n, champ_tx.dar_n = dar_n, champ_tx.rev_n = rev_n;
    champ_tx.urb_t = urb_t, champ_tx.dar_t = dar_t, champ_tx.rev_t = rev_t, champ_tx.mip_t = mip_t;
    champ_tx.launch = launch;
    if (champ_dev.use) {
        int k = champ_dev.use - 1;
        champ_dev.use = 0;
        if (k >= 0 && k < COOP_CHAMP_USES) LOG("[coop] champ: dev: fake use\n"), champ_use(k, champ_dev.lv);
    }
    uint16_t v = 0;
    for (int i = 0; i < 4; i++)
        if (elink_live_effect(pa + champ_held_off[i])) v |= (uint16_t)(1u << i);
    if (v & COOP_CHAMP_COUNT) {
        if (dar_n >= 1 && dar_n <= 3) v |= (uint16_t)(dar_n << COOP_CHAMP_COUNT_SHIFT);
        else v &= (uint16_t)~COOP_CHAMP_COUNT;
    }
    if (champ_dev.held && now < champ_dev.until_ns) v = champ_dev.held;
    if ((v & COOP_CHAMP_HELD) != champ_tx.held_was && champ_tx.said++ < 60)
        LOG("[coop] champ: our persistent effects now %x (shield count %u)\n", v & COOP_CHAMP_HELD,
            (v & COOP_CHAMP_COUNT_MASK) >> COOP_CHAMP_COUNT_SHIFT);
    champ_tx.held_was = v & COOP_CHAMP_HELD;
    v |= (uint16_t)((champ_tx.break_lv & 3) << COOP_CHAMP_BREAK_SHIFT);
    for (int k = 0; k < COOP_CHAMP_USES; k++) v |= (uint16_t)((champ_tx.use[k] & 3) << COOP_CHAMP_USE_SHIFT(k));
    t->champ = coop_champ_ok(v, t->flags) ? v : 0;
}

// ---- the receiver (game thread, per avatar: coop.c and multi_run; `champ` is swapped per avatar: multi.inc)
static struct {
    uint64_t mem;     // 5 effect handles of 0x20 (hle_alloc: never freed, kept over a multi context reset)
    uint64_t actor;
    bool on[4], init;
    uint8_t count;
    uint16_t use_was;
    uint64_t retry_ns[4];
    unsigned said;
} champ;
static const char *champ_held_key(int i, unsigned n, char *buf, size_t cap) {
    static const char *const keys[4] = {"Kago_Shield", NULL, "Kago_Range", "Kago_Ready"};
    if (i != 1) return keys[i];
    snprintf(buf, cap, "Kago_ShieldCount_%02u", n >= 1 && n <= 3 ? n : 1);  // (the game's keys: _01.._03)
    return buf;
}
static void champ_kill(Cpu *c, uint64_t h) {
    if (!elink_live_effect(h)) return;
    Cpu saved = *c;
    c->x[0] = h;
    guest_call(c, A_FX_KILL);  // (checks the id itself)
    *c = saved;
}
static void champ_stop_all(Cpu *c) {
    for (int i = 0; i < 4; i++) {
        if (champ.mem && champ.on[i]) champ_kill(c, champ.mem + (uint64_t)i * 0x20);
        champ.on[i] = false;
    }
}
static void champ_frame(Cpu *c) {
    uint64_t a = avatar.actor;
    bool ok_av = champ_on() && a && avatar_alive() && avatar.frames >= 6 && local_in_world();
    if (!ok_av || a != champ.actor) {
        if (champ.actor) champ_stop_all(c);
        champ.actor = ok_av ? a : 0, champ.init = false;
        if (!ok_av) return;
    }
    if (!champ.mem) champ.mem = (uint64_t)(uintptr_t)hle_alloc(0x20 * 5);
    if (!champ.mem) return;
    CoopAsTree t;
    double age = 99;
    bool have = coop_net_remote_astree(&t, &age), fresh = have && age < 0.5 && (t.flags & COOP_ASTREE_VALID);
    uint16_t v = fresh ? t.champ : 0;
    uint64_t now = plat_now_ns();
    // Persistent keys: kept alive while the sender shows them (restarted at most every 0.5 s if one ends).
    unsigned n = (v & COOP_CHAMP_COUNT_MASK) >> COOP_CHAMP_COUNT_SHIFT;
    for (int i = 0; i < 4; i++) {
        uint64_t h = champ.mem + (uint64_t)i * 0x20;
        bool want = v >> i & 1, live = champ.on[i] && elink_live_effect(h);
        if (!want) {
            if (champ.on[i]) {
                champ_kill(c, h);
                if (champ.said++ < 80) LOG("[coop] champ: the other player's %s off\n", i == 1 ? "shield count" : champ_held_key(i, 0, NULL, 0));
            }
            champ.on[i] = false;
            continue;
        }
        bool recount = i == 1 && live && champ.count != n;
        if ((live && !recount) || now < champ.retry_ns[i]) continue;
        if (live) champ_kill(c, h);
        champ.retry_ns[i] = now + 500000000ull;
        char buf[32];
        const char *key = champ_held_key(i, n, buf, sizeof buf);
        memset((void *)(uintptr_t)h, 0, 0x20);
        bool fired = xl_event_h(c, a, key, XL_EV_ELINK, h);
        champ.on[i] = fired && elink_live_effect(h);
        // (2026-11-19 test: Kago_Shield fires on the avatar but never lives - Kago_ShieldCount_N is the visible
        // orange bubble. A key that does not start is tried again only after 10 s.)
        if (!champ.on[i]) champ.retry_ns[i] = now + 10000000000ull;
        if (i == 1) champ.count = (uint8_t)n;
        if (champ.said++ < 80)
            LOG("[coop] champ: the other player's %s on the avatar: %s\n", key,
                champ.on[i] ? "playing" : fired ? "fired, no live effect (no such key?)" : "not fired (no effect set)");
    }
    // One-shots: each 2-bit use counter that moved since the last fresh tree.
    if (!fresh) {
        if (!have || age > 2.0) champ.init = false;  // (a long gap: take the next counters as they are)
        return;
    }
    uint16_t uses = v >> 8;
    if (!champ.init) { champ.init = true, champ.use_was = uses; return; }
    if (uses == champ.use_was) return;
    for (int k = 0; k < COOP_CHAMP_USES; k++) {
        if (((uses ^ champ.use_was) >> (2 * k) & 3) == 0) continue;
        char buf[40];
        const char *key = k == COOP_CHAMP_URBOSA ? "Kago_Lightning" : k == COOP_CHAMP_MIPHA ? "Kago_Heal" :
                          k == COOP_CHAMP_REVALI ? "Kago_Wind" : buf;
        if (k == COOP_CHAMP_BREAK) {
            unsigned lv = (v & COOP_CHAMP_BREAK_MASK) >> COOP_CHAMP_BREAK_SHIFT;
            snprintf(buf, sizeof buf, "Kago_ShieldBrokenCount_%02u", lv >= 1 && lv <= 3 ? lv : 1);
        }
        uint64_t h = champ.mem + 4 * 0x20;
        memset((void *)(uintptr_t)h, 0, 0x20);
        bool fired = xl_event_h(c, a, key, XL_EV_ELINK, h), live = fired && elink_live_effect(h);
        xl_event(c, a, key, XL_EV_SLINK);
        if (champ.said++ < 80)
            LOG("[coop] champ: the other player used %s: %s on the avatar %s\n", champ_use_name[k], key,
                live ? "playing" : fired ? "fired, no live effect (no such key?)" : "not fired (no effect set)");
    }
    champ.use_was = uses;
}
