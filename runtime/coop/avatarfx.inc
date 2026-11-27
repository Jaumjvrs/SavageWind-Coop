// Part of runtime/coop.c (included from coop/avatar.inc after astree.inc): the player's effects on the avatar.
// (2026-11-14: no swim ripples / splashes on the other player's Link)
//
// How the game shows the player's effects (RE 2026-11-14, Ghidra + ELink2DB):
// - Every actor with an effect set has an XLink handle at actor+0x568: +0x48 the ELink (effects) user instance, +0x50
//   the SLink (sounds) one. The player's effect set is the ELink user "Player_GameRom" (ActorInfo elink): 18 local
//   properties (ground material, water depth "深さ(Rea)", water material, wetness, speeds, state...), 606 asset keys,
//   3 action slots and 74 action triggers. The action slots are generic: FUN_710158d644 names an actor's slots "AS[0]",
//   "AS[1]", "Skl[n]", "AI/Act" for ANY actor, so the AS the actor plays start the action-triggered effects by AS name
//   (SwimDash, SwimAttack, Jump, FallLand, Nml_Move_Dash...). Which variant of an effect plays (water / lava / mud,
//   big / small splash...) is chosen by the properties.
// - The avatar plays the player's own AS (tree mode) but had the effect set "Dummy": nothing showed. Now the avatar
//   actor Dm_CoopLinkF (Co-op/build_overlay.py) has Player_GameRom, and the sender's 18 property values come with
//   the tree packet (protocol 39, CoopAsTree.fx) and are written into the avatar's effect set (values array
//   ui+0x70, dirty bits ui+0x68: what the game's setPropertyValue FUN_7100f515ec does; property defs *(ui+0x30) +0x44
//   count, +0x48 defs: def+0x10 name, def+0x60 type, 2 = float). Names are matched on each side, so the index order
//   of either game does not matter. SAVAGEWIND_COOP_FXPROPS=0: no properties written.
// - Some effects are not started by an AS but by the player's code with the key name, through the XLink event call
//   FUN_710159f00c(actor, key, 0 = effects / 1 = sounds / 2 = both, handle or 0). Entering the water ("ReactionWater_In",
//   called from player code at 0x7100cd446c) is one: the avatar fires it itself when the other player starts
//   swimming. SAVAGEWIND_COOP_SPLASH=0: off.
// Dev: `coop fx <key>` fires an effect key on the avatar; `coop fx props` logs the avatar's and the player's properties.
static const char *const coop_fx_prop_names[COOP_FX_PROPS] = {
    "サブマテリアル", "パラセイル中の高度", "マテリアル", "モデル表示状態", "上昇気流", "受けている風の上向きの強さ",
    "受ける風の強さ", "地面の濡れ具合", "垂直速度", "壁登り角度", "水マテリアル", "水平速度", "深さ(Rea)", "滝の中",
    "濡れ具合", "状態", "盾サーフィン時の盾角度", "速度"};
#define XL_TYPE_FLOAT 2

struct FxMap { uint64_t ui; int idx[COOP_FX_PROPS]; int type[COOP_FX_PROPS]; int found; };
static struct FxMap fx_player_map, fx_avatar_map;

// The actor's XLink user instance: XL_ELINK (+0x48) effects, XL_SLINK (+0x50) sounds (coop/avatarsnd.inc).
#define XL_ELINK 0x48
#define XL_SLINK 0x50
static uint64_t xl_user(uint64_t actor, unsigned off) {
    uint64_t lh = in_guest(actor) ? LD64(actor + 0x568) : 0, ui = in_guest(lh) ? LD64(lh + off) : 0;
    return in_guest(ui) && in_guest(LD64(ui + 0x30)) && in_guest(LD64(ui + 0x70)) ? ui : 0;
}
static uint64_t fx_user(uint64_t actor) { return xl_user(actor, XL_ELINK); }
// The property indices of our names on this actor's effect (or sound) set (cached per user instance); false: none.
static bool xl_map(uint64_t ui, struct FxMap *m, const char *who) {
    if (!ui) return false;
    if (m->ui == ui) return m->found > 0;
    memset(m, 0, sizeof *m);
    m->ui = ui;
    uint64_t tb = LD64(ui + 0x30), defs = LD64(tb + 0x48);
    unsigned n = LD32(tb + 0x44) & 0xffff;
    for (int i = 0; i < COOP_FX_PROPS; i++) {
        m->idx[i] = -1;
        for (unsigned j = 0; j < n && j < 256 && in_guest(defs); j++) {
            uint64_t d = LD64(defs + j * 8), nm = in_guest(d) ? LD64(d + 0x10) : 0;
            if (in_guest(nm) && !strcmp((const char *)(uintptr_t)nm, coop_fx_prop_names[i])) {
                m->idx[i] = (int)j, m->type[i] = (int)LD32(d + 0x60), m->found++;
                break;
            }
        }
    }
    LOG("[coop] fx: %s set %llx: %d of %d properties found (%u defined)\n", who, (unsigned long long)ui, m->found,
        COOP_FX_PROPS, n);
    for (int i = 0; i < COOP_FX_PROPS && coop_look_log(); i++)
        if (m->idx[i] < 0) LOG("[coop] fx: %s set lacks property %s\n", who, coop_fx_prop_names[i]);
    return m->found > 0;
}
static bool fx_map(uint64_t actor, struct FxMap *m, const char *who) { return xl_map(fx_user(actor), m, who); }

// Sender (from astree_build): our player's property values into the tree.
static void fx_fill(uint64_t pa, CoopAsTree *t) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_FXPROPS");
    if (!on || !fx_map(pa, &fx_player_map, "player")) return;
    uint64_t vals = LD64(fx_player_map.ui + 0x70);
    for (int i = 0; i < COOP_FX_PROPS; i++) {
        int k = fx_player_map.idx[i];
        if (k < 0) continue;
        uint32_t v = LD32(vals + (uint64_t)k * 4);
        if (fx_player_map.type[i] == XL_TYPE_FLOAT) {
            float f;
            memcpy(&f, &v, 4);
            if (!isfinite(f) || fabsf(f) > 1000000.0f) v = 0;
        } else if ((int32_t)v < -100000 || (int32_t)v > 100000) {
            v = 0;
        }
        t->fx[i] = v;
        t->fx_mask |= 1u << i;
    }
    t->flags |= COOP_ASTREE_FX;
}

static void xl_write(const struct FxMap *m, const CoopAsTree *t);
// Receiver: the other player's values into the avatar's effect set (each frame, and again after the NPC's own update:
// astree_npc_hook).
static void fx_write(uint64_t actor, const CoopAsTree *t) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_FXPROPS");
    if (!on || !(t->flags & COOP_ASTREE_FX) || !fx_map(actor, &fx_avatar_map, "avatar effect")) return;
    xl_write(&fx_avatar_map, t);
}
// (2026-11-15) While the partner swims, the avatar's InWater ring reads モデル表示状態 (index 3) and 深さ(Rea) (12) all
// the time (watched Switch containers): every writer must put the SAME value there, or the ring flips between assets
// each frame. avl.fx_swim_fix: model shown 1.0, depth the sender's when it is a swimming depth (> 0.35) else 1.7.
static bool rip_tracing;  // (ripple trace window: log every changed avatar value)
static uint32_t fx_fixed(int i, uint32_t v) {
    if (!avl.fx_swim_fix || (i != 3 && i != 12)) return v;
    float f;
    memcpy(&f, &v, 4);
    if (i == 3) f = 1.0f;
    else if (!(isfinite(f) && f > 0.35f && f < 100.0f)) f = 1.7f;
    memcpy(&v, &f, 4);
    return v;
}
// The sender's values into one mapped set (by name).
static void xl_write(const struct FxMap *m, const CoopAsTree *t) {
    uint64_t ui = m->ui, vals = LD64(ui + 0x70);
    uint64_t dirty = 0;
    for (int i = 0; i < COOP_FX_PROPS; i++) {
        int k = m->idx[i];
        if (k < 0 || !(t->fx_mask >> i & 1)) continue;
        uint32_t v = m == &fx_avatar_map && m->type[i] == XL_TYPE_FLOAT ? fx_fixed(i, t->fx[i]) : t->fx[i];
        if (m->type[i] == XL_TYPE_FLOAT) {
            float f;
            memcpy(&f, &v, 4);
            if (!isfinite(f)) continue;
        }
        if (LD32(vals + (uint64_t)k * 4) != v) {
            if (m == &fx_avatar_map && rip_tracing)
                LOG("[coop] fx: ripple trace %s write %s (idx %d): %08x (%.3f) -> %08x (%.3f)\n", xl_who,
                    coop_fx_prop_names[i], k, LD32(vals + (uint64_t)k * 4), *(const float *)(uintptr_t)(vals + (uint64_t)k * 4), v,
                    *(const float *)&v);
            ST32(vals + (uint64_t)k * 4, v);
            dirty |= 1ull << (k & 63);
        }
    }
    if (dirty) ST64(ui + 0x68, LD64(ui + 0x68) | dirty);
}

// Fire one key on an actor (FUN_710159f00c, no handle: a one-shot). mode XL_EV_*: which set(s) it goes to.
#define XL_EV_ELINK 0
#define XL_EV_SLINK 1
#define XL_EV_BOTH 2
static uint64_t fx_key_mem;
static bool xl_event_h(Cpu *c, uint64_t actor, const char *key, int mode, uint64_t handle);
static bool xl_event(Cpu *c, uint64_t actor, const char *key, int mode) { return xl_event_h(c, actor, key, mode, 0); }
static bool xl_event_h(Cpu *c, uint64_t actor, const char *key, int mode, uint64_t handle) {
    if (!fx_key_mem) fx_key_mem = (uint64_t)(uintptr_t)hle_alloc(0x80);
    if (!fx_key_mem || !in_guest(actor) || strlen(key) >= 0x80) return false;
    bool e = fx_user(actor) != 0, snd = xl_user(actor, XL_SLINK) != 0;
    if (mode == XL_EV_ELINK ? !e : mode == XL_EV_SLINK ? !snd : !(e || snd)) return false;
    if (mode == XL_EV_BOTH && !(e && snd)) mode = e ? XL_EV_ELINK : XL_EV_SLINK;
    memcpy((void *)(uintptr_t)fx_key_mem, key, strlen(key) + 1);
    Cpu saved = *c;
    c->x[0] = actor, c->x[1] = fx_key_mem, c->x[2] = (uint64_t)mode, c->x[3] = handle;
    guest_call(c, A_XLINK_EVENT);
    *c = saved;
    return true;
}
static bool fx_emit(Cpu *c, uint64_t actor, const char *key) { return xl_event(c, actor, key, XL_EV_ELINK); }

// Both handles (native and ours) carry a generation from the ELink effect pool.
static uint64_t elink_live_effect(uint64_t handle) {
    if (!handle) return 0;
    uint64_t fx = LD64(handle);
    return in_guest(fx) && in_guest(fx + 0x194) && LD32(fx + 0x20) == LD32(handle + 8) ? fx : 0;
}

// (protocol 47, 2026-11-14: the little blue charge sparkles did not appear on the other
// screen) The game fires "ChargeLvUp" (Player_GameRom, emitter set ChargeLvUp on Weapon_R) by code at each charge
// level up; the avatar has the same effect user (Dm_CoopLinkF): fire it when the partner's level rises.
// SAVAGEWIND_COOP_CHARGEFX=0: off.
static bool charge_fx_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_CHARGEFX");
    return on;
}

static void charge_fx(Cpu *c, unsigned lv, const char *as) {
    // (coop-multi: static unsigned last; -> per avatar, avl)
    static unsigned said;
    if (!charge_fx_on() || !avatar.actor || !avatar_alive()) return;
    // CutChargeWait can start before the game clears the previous attack's level. Wait for the reset
    // before playing flashes. CutChargeLswordSpin never enters this hold.
    bool holding = charge_hold_as(as);
    if (holding && !lv) avl.cf_armed = true;
    // If release and a level-up landed in the same packet interval, the release still carries the level.
    if (avl.cf_armed && lv > avl.cf_last && (holding || !strcmp(as, "CutCharge") || !strcmp(as, "CutChargeF"))) {
        bool ok = xl_event(c, avatar.actor, "ChargeLvUp", XL_EV_ELINK);
        if (said++ < 40) LOG("[coop] fx: the other player's charge level %u -> %u: ChargeLvUp %s\n", avl.cf_last, lv, ok ? "fired" : "not fired (no effect set)");
    }
    avl.cf_armed &= holding;
    avl.cf_last = holding ? lv : 0;
    // (2026-11-15: the sparkles showed but the other effect did not: the white-blue swirl of the released
    // spin.) Disassembly of FUN_7100c6c6e4 (the spin state's effect call, from 0x7100c6d064): key "Kaitengiri_%d" or
    // "Kaitengiri_%d_L" (when player+0x17f1, the two-handed spin) with %d = charge level player+0x1f94 + 1, fired as
    // ELink (mode 0) on the player with a handle in the state (+0x68). Same key on the avatar when the partner's spin
    // starts (their level rides in charge_lv: the spin AS also starts with CutCharge).
    // (coop-multi: static uint8_t seq_was; -> per avatar, avl)
    // (coop-multi: static uint64_t h; -> per avatar, avl)
    CoopState rs;
    double age = 99;
    if (!coop_net_get_remote(&rs, &age) || age > 0.5) return;
    bool fresh = rs.anim == COOP_ANIM_CUT_SPIN && rs.anim_seq != avl.cf_seq_was;
    if (rs.anim == COOP_ANIM_CUT_SPIN) avl.cf_seq_was = rs.anim_seq;
    if (!fresh) return;
    // (2026-11-15, RE: the game fires the swirl only for one-handed swords (weapon type
    // player+0xd24 == 0); the "_L" key follows player+0x17f1, not a two-handed sword. Two-handers and spears: none.)
    if (strncmp(rs.gear[COOP_GEAR_WEAPON].name, "Weapon_Sword_", 13)) return;
    if (!avl.cf_h) avl.cf_h = (uint64_t)(uintptr_t)hle_alloc(0x20);
    if (!avl.cf_h) return;
    if (elink_live_effect(avl.cf_h)) {  // (the last spin's swirl, if the game still has it: gone first)
        Cpu saved = *c;
        c->x[0] = avl.cf_h;
        guest_call(c, 0x700056d010ull);  // (A_FX_KILL below: checks the id itself)
        *c = saved;
    }
    memset((void *)(uintptr_t)avl.cf_h, 0, 0x20);
    char key[32];
    snprintf(key, sizeof key, "Kaitengiri_%u", (lv > 2 ? 2 : lv) + 1);
    bool ok = xl_event_h(c, avatar.actor, key, XL_EV_ELINK, avl.cf_h);
    if (said++ < 40) LOG("[coop] fx: the other player's spin (seq %u): %s %s\n", (unsigned)rs.anim_seq, key, ok ? "fired" : "not fired");
}

static struct { volatile int pending; char key[64]; } fx_dev;  // `coop fx <key>|props` (dev)
static bool fx_swimming(const CoopState *rs) {
    if (astree.active && astree.as[0] && astree.as[0] < COOP_AS_NAME_COUNT) {
        const char *n = coop_as_names[astree.as[0]];
        return starts(n, "Swim") && !strstr(n, "Dead");
    }
    int a = rs->anim;
    return a == COOP_ANIM_SWIM_WAIT || a == COOP_ANIM_SWIM || a == COOP_ANIM_SWIM_FAST || a == COOP_ANIM_SWIM_ATTACK ||
           a == COOP_ANIM_SWIM_JUMP;
}
// ---- swim ripples (2026-11-14: the small ripple effects when swimming still did not work).
// The player's swim code fires the looping ELink key "InWater" WITH a handle
// {ptr, id} once in the water and every frame rewrites that effect's transform: scale (1,1,1) at ptr+0xb4, a 3x4
// matrix at ptr+0xc0/+0xd0/+0xe0 (rows (side, up, fwd, pos)) with fwd = the horizontal swim direction, and
// ptr+0x180 |= 8; on leaving the water FUN_710056d010(&handle) kills it. NPC-class avatar: never run, so we do the
// same. Off: SAVAGEWIND_COOP_RIPPLE=0.
#define A_FX_KILL 0x700056d010ull
static struct { uint64_t h, actor; bool on; float prev[3], fwd[2]; unsigned said; } rip;
static void ripple_stop(Cpu *c) {
    if (rip.on && elink_live_effect(rip.h)) {
        Cpu saved = *c;
        c->x[0] = rip.h;
        guest_call(c, A_FX_KILL);  // (checks the id itself)
        *c = saved;
    }
    if (rip.h) memset((void *)(uintptr_t)rip.h, 0, 0x20);
    rip.on = false;
}
static float RIPF(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }
static unsigned rip_fires, rip_trace;
static uint64_t rip_water_set;  // (the avatar we marked as in the water)
static void ripple_frame(Cpu *c, uint64_t a, bool swim) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_RIPPLE") ? (getenv("SAVAGEWIND_COOP_RIPPLE") && !strcmp(getenv("SAVAGEWIND_COOP_RIPPLE"), "2") ? 2 : 1) : 0;
    if (rip.actor != a) rip.on = false, rip.actor = a;  // (a new avatar: the old effect went with the old actor)
    if (!on || !swim || !a || !fx_user(a)) {
        if (rip_water_set && a && rip_water_set == a) ST8(a + 0x68f, 0);  // (our in-water mark off again)
        rip_water_set = 0;
        avl.fx_swim_fix = false;
        ripple_stop(c);
        return;
    }
    // (2026-11-15, attempt 10, probe: the trace showed the avatar's depth set to 0 every 6 frames by the game's contact
    // effect code (FUN_71011f5ab4: depth = actor+0x6f0 (water surface y) - actor+0x3b4 (y) when actor+0x68f (in water)
    // is set, else 0); each 0 re-picks InWater's asset as "nothing" and kills the ring. `coop waterobj` found the water
    // state on the actor itself: our swimming Link has +0x68f 1 / +0x6f0 the surface y, the avatar 0 / 0. Mark the avatar
    // as in the water at the sender's depth while it swims. SAVAGEWIND_COOP_RIPPLE_WATER=0: off.)
    {
        static int wat = -1;
        if (wat < 0) wat = look_on("SAVAGEWIND_COOP_RIPPLE_WATER");
        float d;
        memcpy(&d, &astree.rx.fx[12], 4);
        if (!(isfinite(d) && d > 0.35f && d < 100.0f)) d = 1.7f;
        if (wat) {
            float wy = RIPF(a + 0x3b4) + d;
            ST8(a + 0x68f, 1);
            memcpy((void *)(uintptr_t)(a + 0x6f0), &wy, 4);
            rip_water_set = a;
        }
    }
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    if (!rip.h) rip.h = (uint64_t)(uintptr_t)hle_alloc(0x20);
    if (!rip.h) return;
    // (2026-11-15, attempt 10 probe: the apply log showed the handle's emitter-set list holding a set on the first frame
    // and empty from the next one on (ours keeps its set). Trace the first frames after each fire (3 fires): list
    // heads, the set's dirty bits and the watched values, at frame start (after fx_write).)
    rip_tracing = rip.on && rip_fires <= 3 && rip_trace < 16;
    if (rip.on && rip_fires <= 3 && rip_trace < 16) {
        uint64_t p0 = LD64(rip.h), ui0 = fx_avatar_map.ui;
        bool live = in_guest(p0) && LD32(p0 + 0x20) == LD32(rip.h + 8);
        uint64_t v0 = in_guest(ui0) ? LD64(ui0 + 0x70) : 0;
        struct FxMap *fm0 = &fx_avatar_map;
        LOG("[coop] fx: ripple trace fire %u frame %u: live %d list60 %s list78 %s dirty %llx | model %.2f depth %.2f hspd %.3f spd %.3f mat %d\n",
            rip_fires, rip_trace, live,
            live ? (LD64(p0 + 0x68) == p0 + 0x60 ? "empty" : "SET") : "-",
            live ? (LD64(p0 + 0x80) == p0 + 0x78 ? "empty" : "SET") : "-",
            in_guest(ui0) ? (unsigned long long)LD64(ui0 + 0x68) : 0ull,
            v0 && fm0->idx[3] >= 0 ? RIPF(v0 + (uint64_t)fm0->idx[3] * 4) : -9.0f,
            v0 && fm0->idx[12] >= 0 ? RIPF(v0 + (uint64_t)fm0->idx[12] * 4) : -9.0f,
            v0 && fm0->idx[11] >= 0 ? RIPF(v0 + (uint64_t)fm0->idx[11] * 4) : -9.0f,
            v0 && fm0->idx[17] >= 0 ? RIPF(v0 + (uint64_t)fm0->idx[17] * 4) : -9.0f,
            v0 && fm0->idx[10] >= 0 ? (int)LD32(v0 + (uint64_t)fm0->idx[10] * 4) : -9);
        rip_trace++;
    }
    // (2026-11-15: InWater's asset tree is a chain of Switch containers, picked when it
    // fires: モデル表示状態 must be exactly 1.0, then 深さ(Rea) > 1.6 gives InWater_Still (the MainRipple ring the report
    // means: the little circle wave that shows on his body); a no-match leaves the handle live with nothing in it,
    // which is what we saw. The NPC's own update or a not-yet-deep sent depth can leave other values there: force the
    // two while the partner swims, before the fire and every frame.)
    {
        struct FxMap *fm = &fx_avatar_map;
        avl.fx_swim_fix = true;
        if (fx_map(a, fm, "avatar effect")) {
            uint64_t vals = LD64(fm->ui + 0x70), dirty = 0;
            static const int want_i[2] = {3, 12};  // (coop_fx_prop_names: モデル表示状態, 深さ(Rea))
            // (what is there now, before we write: the NPC's own update rewriting these would show here)
            static uint64_t seen_ns;
            static unsigned seen_n;
            if (seen_n < 30 && plat_now_ns() >= seen_ns && fm->idx[3] >= 0 && fm->idx[12] >= 0 && fm->idx[11] >= 0) {
                seen_ns = plat_now_ns() + 1000000000ull, seen_n++;
                LOG("[coop] fx: ripple props seen: model-shown %.2f depth %.2f hspeed %.3f (sent depth %.2f hspeed %.3f)\n",
                    RIPF(vals + (uint64_t)fm->idx[3] * 4), RIPF(vals + (uint64_t)fm->idx[12] * 4),
                    RIPF(vals + (uint64_t)fm->idx[11] * 4), RIPF((uint64_t)(uintptr_t)&astree.rx.fx[12]),
                    RIPF((uint64_t)(uintptr_t)&astree.rx.fx[11]));
            }
            for (int j = 0; j < 2; j++) {
                int k = fm->idx[want_i[j]];
                if (k < 0 || fm->type[want_i[j]] != XL_TYPE_FLOAT) continue;
                uint64_t at = vals + (uint64_t)k * 4;
                uint32_t want = fx_fixed(want_i[j], want_i[j] == 12 ? astree.rx.fx[12] : 0);
                if (LD32(at) != want) ST32(at, want), dirty |= 1ull << (k & 63);
            }
            if (dirty) ST64(fm->ui + 0x68, LD64(fm->ui + 0x68) | dirty);
            if (!rip.on && rip.said < 40)
                LOG("[coop] fx: ripple props: model-shown idx %d type %d = %.2f, depth idx %d type %d = %.2f\n", fm->idx[3],
                    fm->type[3], fm->idx[3] >= 0 ? RIPF(vals + (uint64_t)fm->idx[3] * 4) : -1.0f, fm->idx[12], fm->type[12],
                    fm->idx[12] >= 0 ? RIPF(vals + (uint64_t)fm->idx[12] * 4) : -1.0f);
        }
    }
    // (coop-multi: static uint64_t refire_ns; -> per avatar, avl)
    if (!rip.on && plat_now_ns() < avl.rp_refire_ns) return;
    if (!rip.on) {
        memset((void *)(uintptr_t)rip.h, 0, 0x20);
        uint64_t host = a;  // (dev experiment, RIPPLE=2: fire it on our own Link's full effect set, moved to the avatar)
        if (on == 2) {
            uint64_t pinfo = player_info_ptr(), pl = pinfo ? LD64(pinfo + 0x60) : 0;
            if (in_guest(pl) && fx_user(pl)) host = pl;
        }
        bool ok = xl_event_h(c, host, "InWater", XL_EV_ELINK, rip.h);
        rip.on = true;
        rip_fires++, rip_trace = 0;
        {
            uint64_t p0 = LD64(rip.h);
            if (rip_fires <= 3 && in_guest(p0))
                LOG("[coop] fx: ripple trace fire %u at fire: list60 %s list78 %s\n", rip_fires,
                    LD64(p0 + 0x68) == p0 + 0x60 ? "empty" : "SET", LD64(p0 + 0x80) == p0 + 0x78 ? "empty" : "SET");
        }
        rip.prev[0] = m[3], rip.prev[1] = m[7], rip.prev[2] = m[11];
        float fl = sqrtf(m[2] * m[2] + m[10] * m[10]);
        rip.fwd[0] = fl > 1e-3f ? m[2] / fl : 0, rip.fwd[1] = fl > 1e-3f ? m[10] / fl : 1;
        if (rip.said++ < 40) LOG("[coop] fx: the other player swims: ripples %s (handle %llx id %x)\n", ok ? "on" : "failed",
                                 (unsigned long long)LD64(rip.h), LD32(rip.h + 8));
    }
    uint64_t p = LD64(rip.h);
    {
        static uint64_t dbg_ns;
        uint64_t now = plat_now_ns();
        if (coop_look_log() && now >= dbg_ns) {
            dbg_ns = now + 1000000000ull;
            LOG("[coop] fx: ripple handle %llx id %x live %d flags %x pos %.2f %.2f %.2f (avatar %.2f %.2f %.2f)\n",
                (unsigned long long)p, LD32(rip.h + 8), in_guest(p) ? (int)LD32(p + 0x20) : -1,
                in_guest(p) ? LD32(p + 0x180) : 0, in_guest(p) ? RIPF(p + 0xcc) : 0.0f, in_guest(p) ? RIPF(p + 0xdc) : 0.0f,
                in_guest(p) ? RIPF(p + 0xec) : 0.0f, m[3], m[7], m[11]);
        }
    }
    if (!in_guest(p) || LD32(p + 0x20) != LD32(rip.h + 8)) {  // (the effect ended by itself: start it again next frame)
        rip.on = false;
        avl.rp_refire_ns = plat_now_ns() + 500000000ull;
        return;
    }
    // (2026-11-15, attempt 8: the deep compare showed the object behind our Link's ring handle (+0x10) with +0x10 = 3
    // and the avatar's with 382: our ring is new, the avatar's one old. G: +0x10 counts frames since the fire, and the
    // ring's emitters only emit for a while after it; the player's code starts it again (state changes). So: kill and
    // fire the avatar's ring again every SAVAGEWIND_COOP_RIPPLE_REFIRE frames (default 20; 0 = never). Logged once a
    // second for a while: both counters, to see the player's own cycle.)
    {
        static int every = -1;
        if (every < 0) {
            const char *e = getenv("SAVAGEWIND_COOP_RIPPLE_REFIRE");
            every = e ? atoi(e) : 0;  // (attempt 9: default off; +0x10 is no age, it re-fired every frame)
            if (every < 0 || every > 100000) every = 0;
        }
        uint64_t o = LD64(p + 0x10);
        unsigned age = in_guest(o) ? LD32(o + 0x10) : 0;
        static uint64_t age_ns;
        static unsigned age_said;
        if (age_said < 40 && plat_now_ns() >= age_ns) {
            age_ns = plat_now_ns() + 1000000000ull, age_said++;
            uint64_t pinfo = player_info_ptr(), pl = pinfo ? LD64(pinfo + 0x60) : 0;
            uint64_t ph = in_guest(pl) ? LD64(pl + 0x2340) : 0, po = in_guest(ph) && LD32(ph + 0x20) == LD32(pl + 0x2348) ? LD64(ph + 0x10) : 0;
            LOG("[coop] fx: ripple age: avatar's %u, ours %d (id %x)\n", age, in_guest(po) ? (int)LD32(po + 0x10) : -1,
                in_guest(pl) ? LD32(pl + 0x2348) : 0);
        }
        if (every > 0 && age >= (unsigned)every) {
            Cpu saved = *c;
            c->x[0] = rip.h;
            guest_call(c, A_FX_KILL);
            *c = saved;
            memset((void *)(uintptr_t)rip.h, 0, 0x20);
            rip.on = false;  // (fired again next frame, with the values forced first)
            return;
        }
    }
    float dx = m[3] - rip.prev[0], dz = m[11] - rip.prev[2], dl = sqrtf(dx * dx + dz * dz);
    if (dl > 0.01f && dl < 2.0f) rip.fwd[0] = dx / dl, rip.fwd[1] = dz / dl;  // (moving: its heading)
    else if (dl <= 0.01f) {
        float fl = sqrtf(m[2] * m[2] + m[10] * m[10]);
        if (fl > 1e-3f) rip.fwd[0] = m[2] / fl, rip.fwd[1] = m[10] / fl;  // (still: its facing)
    }
    rip.prev[0] = m[3], rip.prev[1] = m[7], rip.prev[2] = m[11];
    float fx = rip.fwd[0], fz = rip.fwd[1];
    // (2026-11-15, user: still no ring when standing / swimming slowly, with model shown 1 and depth 2 forced and the
    // handle live: the asset choice is ruled out.) Height check against OUR Link's own ring (player+0x2340 handle,
    // +0x2348 id) when we swim too: its matrix y minus our actor's y is the offset the game uses;
    // the avatar gets the same (SAVAGEWIND_COOP_RIPPLE_DY=<m> forces one). Once, both handles are dumped side by side.
    static float dy = 0;
    static int dy_src = -1;  // -1 none, 0 env, 1 measured
    if (dy_src < 0 && getenv("SAVAGEWIND_COOP_RIPPLE_DY")) dy = (float)atof(getenv("SAVAGEWIND_COOP_RIPPLE_DY")), dy_src = 0;
    {
        uint64_t pinfo = player_info_ptr(), pl = pinfo ? LD64(pinfo + 0x60) : 0;
        uint64_t ph = in_guest(pl) ? LD64(pl + 0x2340) : 0;
        if (in_guest(ph) && LD32(ph + 0x20) == LD32(pl + 0x2348)) {
            const float *pm = (const float *)(uintptr_t)(pl + ACTOR_MTX);
            float d = RIPF(ph + 0xdc) - pm[7];
            if (dy_src != 0 && isfinite(d) && fabsf(d) < 3.0f) dy = d, dy_src = 1;
            static int dumped;
            if (!dumped) {
                dumped = 1;
                LOG("[coop] fx: ripple compare: our ring y %.2f, our Link y %.2f, our water y %.2f -> offset %.2f; avatar y %.2f\n",
                    RIPF(ph + 0xdc), pm[7], RIPF(pl + 0x20d4), d, m[7]);
                for (unsigned o = 0; o < 0x200; o += 4)
                    if (LD32(ph + o) != LD32(p + o))
                        LOG("[coop] fx: ripple compare +%03x: ours %08x (%.3f) avatar's %08x (%.3f)\n", o, LD32(ph + o),
                            RIPF(ph + o), LD32(p + o), RIPF(p + o));
            }
        }
    }
    // (2026-11-15, attempt 7: values steady and right (log "ripple props seen"), handle live, still invisible. The
    // Kaitengiri swirl, also fired with a handle but never moved by us, DOES show: leave the ring on its own Neck
    // attachment (from the asset) by default. SAVAGEWIND_COOP_RIPPLE_MTX=1: move it like the player code does.)
    static int mtx_on = -1;
    // (attempt 7 result: without our writes the handle kept scale 0 and no matrix: the writes stay, default on.)
    if (mtx_on < 0) mtx_on = !(getenv("SAVAGEWIND_COOP_RIPPLE_MTX") && getenv("SAVAGEWIND_COOP_RIPPLE_MTX")[0] == '0');
    {
        static int deep;  // (once: the objects behind the two handles, side by side, for the next look)
        uint64_t pinfo = player_info_ptr(), pl = pinfo ? LD64(pinfo + 0x60) : 0;
        uint64_t ph = in_guest(pl) ? LD64(pl + 0x2340) : 0;
        if (!deep && in_guest(ph) && LD32(ph + 0x20) == LD32(pl + 0x2348)) {
            deep = 1;
            static const unsigned offs[] = {0x10, 0x58, 0x60, 0x78};
            for (unsigned j = 0; j < 4; j++) {
                uint64_t a1 = LD64(ph + offs[j]), a2 = LD64(p + offs[j]);
                if (!in_guest(a1) || !in_guest(a2)) continue;
                unsigned nd = 0;
                for (unsigned o = 0; o < 0x180 && nd < 40; o += 4)
                    if (LD32(a1 + o) != LD32(a2 + o)) {
                        nd++;
                        LOG("[coop] fx: ripple deep +%02x->+%03x: ours %08x (%.3f) avatar's %08x (%.3f)\n", offs[j], o,
                            LD32(a1 + o), RIPF(a1 + o), LD32(a2 + o), RIPF(a2 + o));
                    }
            }
        }
    }
    if (!mtx_on) return;
    float ry = m[7] + (dy_src >= 0 ? dy : 0);
    float row[12] = {fz, 0, fx, m[3], 0, 1, 0, ry, -fx, 0, fz, m[11]};  // (side = up x fwd = (fz, 0, -fx))
    float one = 1.0f;
    for (int k = 0; k < 3; k++) memcpy((void *)(uintptr_t)(p + 0xb4 + 4 * k), &one, 4);
    for (int r = 0; r < 3; r++) memcpy((void *)(uintptr_t)(p + 0xc0 + 0x10 * r), &row[r * 4], 16);
    ST32(p + 0x180, LD32(p + 0x180) | 8);
    // (2026-11-15, attempt 9: the deep compare showed the emitter set behind our Link's handle (+0x60 list) with a
    // full matrix and flags 0x28, the avatar's with an all-zero matrix (scale 0: invisible) and our request flag still
    // pending at handle+0x180. The game's handle method at Ghidra 0x7100f3b104 (a virtual, vtable data 0x7101d0b3b0)
    // is what hands +0x180 requests and the transform at +0x98.. down to the emitter sets (+0x60 / +0x78 lists) and
    // clears +0x180; for the player the game's per-frame update calls it, for the NPC-class avatar nothing does. Call
    // it ourselves after the writes, only once it is found in the handle's own vtable. SAVAGEWIND_COOP_RIPPLE_APPLY=0 off.)
    static int apply_on = -1, slot = -2;
    if (apply_on < 0) apply_on = look_on("SAVAGEWIND_COOP_RIPPLE_APPLY");
    if (!apply_on) return;
    // (attempt 9 never matched and 9b crashed ("no function at 700f3b104"): the address had a digit missing; the
    // runtime one is 0x7000f3b104, found at vtable 7001d0b380 +0x30. Call only when the handle's vtable holds it.)
    uint64_t vt = LD64(p);
    if (slot == -2) {
        slot = in_guest(vt) && LD64(vt + 0x30) == 0x7000f3b104ull ? 6 : -1;
        LOG("[coop] fx: ripple apply method: vtable %llx slot %d (entry %llx)\n", (unsigned long long)vt, slot,
            in_guest(vt) ? (unsigned long long)LD64(vt + 0x30) : 0ull);
    }
    if (slot < 0 || !in_guest(vt) || LD64(vt + 0x30) != 0x7000f3b104ull) return;
    Cpu saved = *c;
    c->x[0] = p;
    guest_call(c, 0x7000f3b104ull);
    *c = saved;
    static unsigned ap_said;
    if (ap_said < 5) {
        uint64_t es = LD64(p + 0x68);  // (first emitter set in the +0x60 list, as the method walks it)
        ap_said++;
        LOG("[coop] fx: ripple applied: flags now %x / %x, first set %llx\n", LD32(p + 0x180), LD32(p + 0x184),
            (unsigned long long)es);
    }
}

// (2026-11-15, ripple attempt 10, dev `coop waterobj`) The ring dies because the game's contact-effect code
// (Ghidra FUN_71011f5ab4, from FUN_71011f3708) writes the avatar's 深さ(Rea) (index 5) = W+0x6f0 - W+0x3b4 when the
// water object W (contact info +0xb8) has +0x68f set, else 0. Find W on our swimming Link: a pointer one or two levels
// below the actor with +0x68f == 1 and +0x6f0 = the water surface y (player +0x20d4); then the same path on the avatar.
static volatile int waterobj_dev;
static bool wobj_ok(uint64_t q, float wy) {
    if (!in_guest(q) || !in_guest(q + 0x700)) return false;
    return LD8(q + 0x68f) == 1 && fabsf(RIPF(q + 0x6f0) - wy) < 0.05f;
}
static void waterobj_scan(void) {
    uint64_t pinfo = g_site.player_info ? LD64(g_site.player_info) : 0, pl = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t av = avatar.actor;
    if (!in_guest(pl)) return;
    float wy = RIPF(pl + 0x20d4);
    LOG("[coop] waterobj: player %llx water y %.2f y %.2f; avatar %llx\n", (unsigned long long)pl, wy,
        ((const float *)(uintptr_t)(pl + ACTOR_MTX))[7], (unsigned long long)av);
    unsigned found = 0;
    for (unsigned o = 0; o < 0x3000 && found < 12; o += 8) {
        uint64_t p = LD64(pl + o);
        if (!in_guest(p)) continue;
        if (wobj_ok(p, wy)) {
            found++;
            uint64_t ap = in_guest(av) ? LD64(av + o) : 0;
            LOG("[coop] waterobj: player+%x -> %llx (+3b4 %.2f); avatar+%x -> %llx: 68f %d 6f0 %.2f 3b4 %.2f\n", o,
                (unsigned long long)p, RIPF(p + 0x3b4), o, (unsigned long long)ap, in_guest(ap + 0x700) ? LD8(ap + 0x68f) : -1,
                in_guest(ap + 0x700) ? RIPF(ap + 0x6f0) : 0.0f, in_guest(ap + 0x700) ? RIPF(ap + 0x3b4) : 0.0f);
        }
        for (unsigned o2 = 0; o2 < 0x800 && found < 12; o2 += 8) {
            if (!in_guest(p + o2 + 8)) break;
            uint64_t q = LD64(p + o2);
            if (!wobj_ok(q, wy)) continue;
            found++;
            uint64_t ap = in_guest(av) ? LD64(av + o) : 0, aq = in_guest(ap) ? LD64(ap + o2) : 0;
            LOG("[coop] waterobj: player+%x+%x -> %llx (+3b4 %.2f); avatar -> %llx: 68f %d 6f0 %.2f 3b4 %.2f\n", o, o2,
                (unsigned long long)q, RIPF(q + 0x3b4), (unsigned long long)aq, in_guest(aq + 0x700) ? LD8(aq + 0x68f) : -1,
                in_guest(aq + 0x700) ? RIPF(aq + 0x6f0) : 0.0f, in_guest(aq + 0x700) ? RIPF(aq + 0x3b4) : 0.0f);
        }
    }
    LOG("[coop] waterobj: %u found\n", found);
}

// ---- weapon effects on the partner's weapon puppet (2026-11-16: all animations should show on the partner).
// Weapon puppets Dm_CoopW_<Sword|Lsword|Spear>_* now keep their real weapon's effect set (build_overlay.py:
// ElinkUser WeaponSpear / WeaponLsword / Weapon_Sword_001...). The game starts a spear's "SpearAttack" by code on the
// weapon while the owner's AS is in a thrust window (FUN_7101169664) and the blade trail "Blur_Default" from the weapon
// user. For charged attacks follow the avatar's active hold events so a spear flurry can play separate thrusts.
// SAVAGEWIND_COOP_WEAPONFX=0 disables all weapon-puppet effects.
enum { WFX_BLUR, WFX_SPEAR };
static struct { uint64_t h[2], actor; uint16_t as; uint8_t seq, active; bool charged; unsigned said; } wfx;
static uint64_t wfx_windows;

static uint64_t weapon_fx_trail_data(uint64_t actor) {
    // The game reads the wielder's AnimationInfo sword-blur table unconditionally (0x700152ff4c).
    // Older overlays lack it. Only bind the avatar as wielder when the table loaded.
    uint64_t link = LD64(actor + 0x570);
    uint64_t res = in_guest(link) ? LD64(link + 0x148) : 0;
    uint64_t data = in_guest(res) ? LD64(res + 0x2c0) : 0;
    return in_guest(data) && LD32(data + 8) > 0 ? data : 0;
}

static float weapon_fx_trail_frames(uint64_t data) {
    // The AS can open the hold window one frame before the model starts its new skeletal clip.
    // Starting then leaves the emitter with no trail samples. Check the same clips and frame range
    // as the native SwordBlur callback before starting it.
    uint64_t model = LD64(avatar.actor + ACTOR_MODEL);
    uint64_t blend = in_guest(model) ? LD64(model + 0xd0) : 0;
    if (!in_guest(blend)) return 0;
    uint64_t layer = LD64(blend + 0x48), table = LD64(blend + 0x68), rows = LD64(data + 0x10);
    if (!in_guest(layer) || !in_guest(table) || !in_guest(rows)) return 0;
    unsigned count = LD32(data + 8), layers = LD32(blend + 0x40), clips = LD32(blend + 0x60);
    for (unsigned i = 0; i < layers && i < 3; i++, layer += 0xa8) {
        unsigned j = LD32(layer) >> 16;
        uint64_t clip = j < clips ? LD64(table + j * 0x28) : 0;
        uint64_t res = in_guest(clip) ? LD64(clip) : 0;
        uint64_t name = in_guest(res) ? LD64(res + 0x10) : 0;
        uint64_t frame = LD64(layer + 0x20);
        if (!in_guest(name) || !in_guest(frame)) continue;
        float f = RIPF(frame);
        for (unsigned k = 0; k < count; k++) {
            uint64_t row = rows + k * 0x30, key = LD64(row + 0x28);
            float first = (float)LD32(row + 4), last = (float)LD32(row + 8);
            if (f >= first - 1 && f <= last + 1 &&
                in_guest(key) && !strcmp((const char *)(uintptr_t)(name + 2), (const char *)(uintptr_t)key))
                return last - first + 1;
        }
    }
    return 0;
}

static bool weapon_fx_hold(Cpu *c, unsigned kind) {
    uint64_t as = LD64(avatar.actor + 0x560), checker = LD64(0x7001d67388ull);
    if (!in_guest(as) || !in_guest(checker)) return false;
    Cpu saved = *c;
    c->x[0] = as, c->x[1] = kind, c->x[2] = 0, c->x[3] = 0, c->x[4] = 0;
    c->x[5] = checker, c->x[6] = 0, c->x[7] = 1;
    guest_call(c, 0x70014a2ab0ull);  // query an active hold in AS slot 0, as in the game's spear effect helper
    bool active = c->x[0] & 1;
    *c = saved;
    return active;
}

static uint64_t weapon_fx_effect(unsigned i) {
    uint64_t h = wfx.h[i], effect = h ? LD64(h) : 0;
    return in_guest(effect + 0x190) && LD32(effect + 0x20) == LD32(h + 8) ? effect : 0;
}

static uint64_t weapon_fx_trail(void) {
    uint64_t effect = weapon_fx_effect(WFX_BLUR);
    uint64_t trail = effect ? LD64(effect + 0x190) : 0;
    return in_guest(trail) ? trail : 0;
}

static float weapon_fx_blur_lifetime(Cpu *c, float sample_frames) {
    uint64_t as = LD64(avatar.actor + ACTOR_AS);
    if (!wfx_windows) wfx_windows = (uint64_t)(uintptr_t)hle_alloc(0x188);
    if (!wfx_windows || !in_guest(as)) return 0;
    // Native SwordBlur trigger: collect active blur windows, then divide the first window's frames by
    // the playback rate. The emitter's authored life is 1000 frames.
    uint64_t vt = LD64(A_SAFESTRING_VT_SLOT) + 0x10, empty = LD64(0x7001d66218ull);
    memset((void *)(uintptr_t)wfx_windows, 0, 0x188);
    for (unsigned i = 0; i < 16; i++) {
        uint64_t row = wfx_windows + i * 0x18;
        ST64(row, vt);
        ST64(row + 8, empty);
    }
    Cpu saved = *c;
    c->x[0] = as, c->x[1] = wfx_windows, c->x[2] = 48;
    c->x[3] = LD64(0x7001d66510ull), c->x[4] = 0, c->x[5] = 1;
    guest_call(c, 0x70014a2cc4ull);
    float frames = LD32(wfx_windows + 0x180) ? RIPF(wfx_windows + 0x10) : 0;
    *c = saved;
    if (!(frames > 0 && frames <= 200)) frames = sample_frames;
    c->x[0] = as, c->x[1] = 0, c->x[2] = 0, c->x[3] = LD64(0x7001d76ac0ull), c->x[4] = 0;
    guest_call(c, 0x70014a25b0ull);
    float rate;
    memcpy(&rate, &c->v[0], sizeof rate);
    *c = saved;
    return frames / (rate > 0 && isfinite(rate) ? rate : 1) / 1000.0f;
}

static void weapon_fx_stop(Cpu *c, unsigned i, bool fade) {
    uint64_t h = wfx.h[i];
    if (h && elink_live_effect(h)) {
        if (!fade && i == WFX_BLUR) {
            uint64_t trail = weapon_fx_trail();
            if (trail && LD64(trail + 8) == avatar.actor) ST64(trail + 8, 0);
        }
        Cpu saved = *c;
        c->x[0] = h;
        guest_call(c, fade ? 0x7000675e8cull : A_FX_KILL);  // the game's spear/trail stop lets the particles fade
        *c = saved;
        if (!fade) memset((void *)(uintptr_t)h, 0, 0x20);
    } else if (h) {
        memset((void *)(uintptr_t)h, 0, 0x20);
    }
    wfx.active &= ~(1u << i);
}

static void weapon_fx_kill(Cpu *c) {
    for (int i = 0; i < 2; i++) weapon_fx_stop(c, i, false);
    wfx.actor = 0;
}

static void weapon_fx_start(Cpu *c, unsigned i, const char *as, const char *weapon, bool charged) {
    static const char *const keys[] = {"Blur_Default", "SpearAttack"};
    uint64_t data = charged && i == WFX_BLUR ? weapon_fx_trail_data(avatar.actor) : 0;
    float frames = data ? weapon_fx_trail_frames(data) : 0;
    if (data && !frames) return;
    if (!wfx.h[i]) wfx.h[i] = (uint64_t)(uintptr_t)hle_alloc(0x20);
    if (!wfx.h[i]) return;
    weapon_fx_stop(c, i, false);  // a fading instance may still occupy this handle
    memset((void *)(uintptr_t)wfx.h[i], 0, 0x20);
    xl_event_h(c, wfx.actor, keys[i], XL_EV_ELINK, wfx.h[i]);
    uint64_t effect = weapon_fx_effect(i);
    uint64_t trail = data ? weapon_fx_trail() : 0;
    if (trail) {
        float life = weapon_fx_blur_lifetime(c, frames);
        float scale = RIPF(wfx.actor + 0x418) * RIPF(trail + 0x18);
        ST64(trail + 8, avatar.actor);
        memcpy((void *)(uintptr_t)(trail + 0x18), &scale, sizeof scale);
        if (life > 0) {
            memcpy((void *)(uintptr_t)(effect + 0x100), &life, sizeof life);
            ST32(effect + 0x180, LD32(effect + 0x180) | 0x100);
        }
    }
    wfx.active |= 1u << i;
    if (wfx.said++ < 60 || coop_look_log())
        LOG("[coop] fx: the other player's %s with %s: %s %s%s\n", as, weapon, keys[i], effect ? "fired" : "no handle",
            trail ? " (wielder bound)" : "");
}

static void weapon_fx_frame(Cpu *c, const CoopState *rs) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_WEAPONFX");
    if (!on) return;
    struct Pup *p = &gear_pup[COOP_GEAR_WEAPON];
    uint64_t w = p->actor && pup_alive(p) ? p->actor : 0;
    uint16_t as = astree.active ? astree.as[0] : 0;
    uint8_t seq = astree.active ? astree.seq[0] : 0;
    const char *n = as && as < COOP_AS_NAME_COUNT ? coop_as_names[as] : "";
    const char *wn = rs->gear[COOP_GEAR_WEAPON].name;
    bool spear = starts(wn, "Weapon_Spear_"), lsword = starts(wn, "Weapon_Lsword_");
    bool charged = starts(n, "CutCharge");
    bool want = w && (spear || lsword || starts(wn, "Weapon_Sword_")) && starts(n, "Cut") &&
                !charge_hold_as(n) && (!charged || charge_fx_on()) && fx_user(w);
    if (wfx.actor != w) weapon_fx_kill(c);
    if (!want || wfx.as != as || wfx.seq != seq)
        for (unsigned i = 0; i < 2; i++)
            if ((wfx.active >> i) & 1) weapon_fx_stop(c, i, wfx.charged);
    if (!want) return;
    wfx.actor = w, wfx.as = as, wfx.seq = seq, wfx.charged = charged;
    // The claymore release's hold can be reported while its old spin clip is still on the model.
    // Its sampled finishing clip tells weapon_fx_start when the blade trail can actually begin.
    bool lsword_finish = lsword && !strcmp(n, "CutCharge");
    bool active[2] = {!charged || lsword_finish || weapon_fx_hold(c, 48),
                      spear && (!charged || weapon_fx_hold(c, 3))};
    for (unsigned i = 0; i < 2; i++) {
        bool running = (wfx.active >> i) & 1;
        if (active[i] && !running) weapon_fx_start(c, i, n, wn, charged);
        if (!active[i] && running) weapon_fx_stop(c, i, charged);
    }
}

// Each frame from avatar_frame (game thread), once the avatar exists.
static void avatarfx_frame(Cpu *c, const CoopState *rs) {
    if (waterobj_dev) waterobj_dev = 0, waterobj_scan();
    // (coop-multi: static struct { uint64_t actor; bool swim; unsigned splashes; } s; -> per avatar, avl)
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_SPLASH");
    uint64_t a = avatar.actor;
    if (fx_dev.pending) {
        fx_dev.pending = 0;
        if (!strcmp(fx_dev.key, "props")) {
            xprobe_dump("avatar", a);
            uint64_t pinfo = g_site.player_info ? LD64(g_site.player_info) : 0;
            xprobe_dump("player", pinfo ? LD64(pinfo + 0x60) : 0);
        } else if (!strncmp(fx_dev.key, "w:", 2)) {  // (2026-11-16) w:<key>: on the weapon puppet; w:props its properties
            struct Pup *wp = &gear_pup[COOP_GEAR_WEAPON];
            uint64_t w = wp->actor && pup_alive(wp) ? wp->actor : 0;
            if (!strcmp(fx_dev.key + 2, "props")) xprobe_dump("weaponpup", w);
            else LOG("[coop] fx: key %s on the weapon puppet %s -> %s\n", fx_dev.key + 2, wp->name,
                     w && fx_emit(c, w, fx_dev.key + 2) ? "fired" : "no puppet / no effect set");
        } else {
            LOG("[coop] fx: key %s on the avatar -> %s\n", fx_dev.key, fx_emit(c, a, fx_dev.key) ? "fired" : "no effect set");
        }
    }
    if (avl.af_s.actor != a) avl.af_s.actor = a, avl.af_s.swim = false;
    if (!a || avatar.frames < 30) { ripple_frame(c, a, false); return; }
    if (astree.active) fx_write(a, &astree.rx);
    weapon_fx_frame(c, rs);
    bool swim = fx_swimming(rs);
    ripple_frame(c, a, swim);
    if (swim && !avl.af_s.swim && on && fx_user(a)) {
        bool ok = fx_emit(c, a, "ReactionWater_In");
        if (avl.af_s.splashes++ < 50 || coop_look_log()) LOG("[coop] fx: the other player went into the water: splash %s\n", ok ? "fired" : "failed");
    }
    avl.af_s.swim = swim;
}
