// Part of runtime/coop.c (included from coop/avatar.inc after avatarfx.inc): the other player's sounds on the avatar,
// and the status effects (ice, electric shock) the player's code fires by key. (2026-11-14, user: the partner's Link
// makes no sounds: footsteps, sword swings, voice. )
//
// How the player's sounds work (RE 2026-11-14, SLink2DB / SoundInfo / Player .bas files, not yet seen in game):
// - Sounds are the XLink "SLink" half: the actor's XLink handle actor+0x568, +0x50 = the sound user instance (same
//   layout as the effect one at +0x48: property defs *(ui+0x30), values ui+0x70, dirty bits ui+0x68). The player's
//   sound user is "GameROMPlayer" (ActorInfo slink; ActorLink SlinkUser goes through SoundInfo.sbyml
//   SoundSettingUserName): 17 local properties, 1287 keys (voices PV000.., ElectricShock, Frozen, Frozen_End,
//   FrozenStart, Climb*, Swim*, Surf*, Parashawl*, EquipNoise_*...), 6 action slots (AS by name, like the effects).
// - Voices: the player's Voice* AS carry AS events TypeIndex 28 = an SLink key (Player_VoiceJump -> "PV405"). The
//   avatar already plays them (renamed CoopX<name>, same .bas, slot 3: coop/astree.inc), so with the sound set on the
//   avatar (Dm_CoopLinkS, Co-op/build_overlay.py) its voice and AS-driven action sounds should play by themselves.
//   9 of the 17 sound properties have the same names as effect properties already sent (protocol 39 fx[]: ground
//   material, water material, depth, speeds, altitude, waterfall, surf shield angle) and are written by name into the
//   avatar's sound set (snd_write). The other 8 (ragdoll speeds, acceleration, smoothed wind, wall-climb direction,
//   forward amount, shield material, riding state) are not sent. SAVAGEWIND_COOP_SOUNDS=0: no sound set (avatar
//   Dm_CoopLinkF), nothing in this file runs except the status effects.
// - Footsteps are NOT in GameROMPlayer: a separate user "PlayerFootStep" (keys PlayerFootStep, PlayerFootStepNaked,
//   PlayerLand, PlayerLadderStep, PlayerLandNaked; props スケールタイプ, スピード, リアクションサブタイプ, 勾配,
//   歩行前進度合, 深さ(Rea), 足の濡れ具合) driven by the player's foot-step code (XLinkInfo "Player"
//   FootStepProcTypeName). The avatar gets a model-less helper Dm_CoopStep (sound set PlayerFootStep only) that is
//   put on the foot that lands and fires "PlayerFootStep" (bare feet: "PlayerFootStepNaked"); a foot lands when its
//   ankle bone comes down near its lowest height after a lift. Landings fire "PlayerLand". Only within STEP_NEAR_M of
//   our Link. SAVAGEWIND_COOP_STEPS=0: off.
// - Sword swings: each weapon's own sound set (ActorInfo slink, e.g. Weapon_Sword_Metal / Twig) has
//   PlayerAttackNormal1/2/3, PlayerAttackJump, PlayerAttackFinish (fired by the player's weapon code on the attack
//   events). The weapon puppets Dm_CoopW_* now keep their real weapon's sound set (overlay) and fire the key when the
//   avatar starts a Cut* AS. SAVAGEWIND_COOP_SWINGSND=0: off.
// - Status effects: the player's code fires "ElectricShock" (both sets: sparks + sound) when shocked, "FrozenStart"
//   (sound) / "Frozen" (the ice, ELink) when frozen and "Frozen_End" when breaking out. The avatar fires them itself
//   when the other player's AS becomes DamageShock / Freeze / FreezeBreak. Friendly elemental arrows also carry the
//   native status flags. They show a tracked Chemical_Fire or Frozen effect while the status is active. For other
//   sources of ice, SAVAGEWIND_COOP_FROZENFX=1 enables the tracked Frozen effect. SAVAGEWIND_COOP_STATUSFX=0: off.
// Dev: `coop snd <key>` (sound key on the avatar), `coop snd both <key>`, `coop snd props`, `coop snd step`,
// `coop snd swing <key>` (on the weapon puppet).

// SAVAGEWIND_COOP_AVSTEPS=0: use legacy Dm_CoopStep + property set (off switch for partner footstep fix).
#define STEP_ACTOR "Dm_CoopStep"
#define STEP_ACTOR_VANILLA "PlayerFootStep"
#define STEP_NEAR_M 40.0f  // footsteps further from our Link than this are not played
#define RSUB_PL_FS_L 14
#define RSUB_PL_FS_R 15
#define RSUB_PL_LAND_FIRST 24
#define SCALE_TYPE_NORMAL 2
static struct Pup step_pup;
static bool avsteps_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_AVSTEPS");
    return on;
}
static struct FxMap snd_avatar_map;
static struct { volatile int pending; int what; char key[64]; } snd_dev;  // `coop snd ...` (dev)
enum { SND_DEV_KEY, SND_DEV_BOTH, SND_DEV_PROPS, SND_DEV_STEP, SND_DEV_SWING };
// (dev, 2026-11-18 sounds worker: which of the partner's actors makes the scraping sound) `coop snd mute <mask>`:
// 1 = the collision body parked, 2 = no footsteps, 4 = no swing sounds. Logged with the time (plat_now_ns) so a
// SAVAGEWIND_AUDIO_WAV recording can be cut into the phases.
static volatile unsigned snd_mute_dev, snd_mute_seq, snd_mute_said = ~0u;
static void snd_mute_log(void) {
    if (snd_mute_said == snd_mute_seq) return;
    snd_mute_said = snd_mute_seq;
    if (snd_mute_seq) LOG("[coop] snd: dev mute %u at %.3f s (command %u)\n", snd_mute_dev, plat_now_ns() / 1e9, snd_mute_seq);
}

static bool snd_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_SOUNDS");
    return on;
}
// Receiver: the sent effect properties whose names the avatar's sound set has too (each frame, and after the NPC's
// own update: astree_npc_hook).
// (2026-11-18: Link's walking sound on the partner sounded like a rock moving) The sent values are the
// sender's EFFECT set's (ELink). Its whole-number properties are enums, and the sound set (SLink) numbers the same
// names differently: seen live on grass, our player's sound set had マテリアル 9 / サブマテリアル 33 while the
// partner's effect set, on the same grass, sent 22 / 53 - written as they were, the avatar's sounds (and the step
// helper's, which got no material at all: 0) played another ground's sounds. So each whole-number value is turned
// into the sound set's number of the same thing, learned from our own player, whose two sets hold both numbers of
// the ground he stands on every frame (snd_learn). A value our player never had: the ground materials take our
// player's own (the partner is near: a footstep within STEP_NEAR_M), other enums are not written.
// SAVAGEWIND_COOP_SNDENUM=0: off (raw values, as before).
#define SND_ENUM_MAX 48
static struct { int n; int32_t e[SND_ENUM_MAX], s[SND_ENUM_MAX]; } snd_enum[COOP_FX_PROPS];
static struct FxMap snd_player_map, snd_step_map;
static bool snd_enum_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_SNDENUM"), LOG("[coop] snd: effect -> sound enum values: %s\n", on ? "translated" : "raw");
    return on;
}
static bool snd_ground_prop(int i) {
    const char *n = coop_fx_prop_names[i];
    return !strcmp(n, "マテリアル") || !strcmp(n, "サブマテリアル") || !strcmp(n, "水マテリアル");
}
static void snd_learn(uint64_t pa) {
    if (!snd_enum_on() || !in_guest(pa) || !fx_map(pa, &fx_player_map, "player") ||
        !xl_map(xl_user(pa, XL_SLINK), &snd_player_map, "player sound"))
        return;
    uint64_t ev = LD64(fx_player_map.ui + 0x70), sv = LD64(snd_player_map.ui + 0x70);
    for (int i = 0; i < COOP_FX_PROPS; i++) {
        int ke = fx_player_map.idx[i], ks = snd_player_map.idx[i];
        if (ke < 0 || ks < 0 || fx_player_map.type[i] == XL_TYPE_FLOAT || snd_player_map.type[i] == XL_TYPE_FLOAT) continue;
        int32_t e = (int32_t)LD32(ev + (uint64_t)ke * 4), s = (int32_t)LD32(sv + (uint64_t)ks * 4);
        int j;
        for (j = 0; j < snd_enum[i].n && snd_enum[i].e[j] != e; j++) {}
        if (j < snd_enum[i].n) {
            if (snd_enum[i].s[j] != s) snd_enum[i].s[j] = s;  // (both sets update in the same frame: last pair wins)
            continue;
        }
        if (j >= SND_ENUM_MAX) continue;
        snd_enum[i].e[j] = e, snd_enum[i].s[j] = s, snd_enum[i].n++;
        static unsigned said;
        if (said++ < 40 || coop_look_log())
            LOG("[coop] snd: %s: effect value %d = sound value %d (learned from our player)\n", coop_fx_prop_names[i], e, s);
    }
}
// The tree's values as the sound set `m` numbers them (whole-number properties translated, see above).
static void snd_translate(const struct FxMap *m, const CoopAsTree *t, CoopAsTree *out) {
    *out = *t;
    if (!snd_enum_on()) return;
    uint64_t sv = in_guest(snd_player_map.ui) ? LD64(snd_player_map.ui + 0x70) : 0;
    if (!in_guest(sv)) sv = 0;
    for (int i = 0; i < COOP_FX_PROPS; i++) {
        if (m->idx[i] < 0 || !(t->fx_mask >> i & 1)) continue;
        bool sent_float = fx_avatar_map.ui ? fx_avatar_map.type[i] == XL_TYPE_FLOAT : m->type[i] == XL_TYPE_FLOAT;
        bool want_float = m->type[i] == XL_TYPE_FLOAT;
        if (sent_float && want_float) continue;
        if (sent_float != want_float) {  // (float in one set, whole number in the other)
            float f;
            if (sent_float) memcpy(&f, &t->fx[i], 4), out->fx[i] = (uint32_t)(int32_t)(isfinite(f) && fabsf(f) <= 1000000.0f ? f : 0);
            else f = (float)(int32_t)t->fx[i], memcpy(&out->fx[i], &f, 4);
            continue;
        }
        int32_t e = (int32_t)t->fx[i];
        int j;
        for (j = 0; j < snd_enum[i].n && snd_enum[i].e[j] != e; j++) {}
        if (j < snd_enum[i].n) out->fx[i] = (uint32_t)snd_enum[i].s[j];
        else if (snd_ground_prop(i) && sv && snd_player_map.idx[i] >= 0)
            out->fx[i] = LD32(sv + (uint64_t)snd_player_map.idx[i] * 4);
        else out->fx_mask &= ~(1u << i);
    }
}
static void snd_write(uint64_t actor, const CoopAsTree *t) {
    if (!snd_on() || !(t->flags & COOP_ASTREE_FX) || !xl_map(xl_user(actor, XL_SLINK), &snd_avatar_map, "avatar sound"))
        return;
    CoopAsTree tt;
    snd_translate(&snd_avatar_map, t, &tt);
    xl_write(&snd_avatar_map, &tt);
}

// One property of a set by name: its index (-1: none) and type.
static int xl_prop(uint64_t ui, const char *name, int *type) {
    if (!ui) return -1;
    uint64_t tb = LD64(ui + 0x30), defs = in_guest(tb) ? LD64(tb + 0x48) : 0;
    unsigned n = in_guest(tb) ? LD32(tb + 0x44) & 0xffff : 0;
    for (unsigned j = 0; j < n && j < 64 && in_guest(defs); j++) {
        uint64_t d = LD64(defs + j * 8), nm = in_guest(d) ? LD64(d + 0x10) : 0;
        if (in_guest(nm) && !strcmp((const char *)(uintptr_t)nm, name)) {
            if (type) *type = (int)LD32(d + 0x60);
            return (int)j;
        }
    }
    return -1;
}
static void xl_set_raw(uint64_t ui, int k, uint32_t v) {
    if (!ui || k < 0) return;
    uint64_t vals = LD64(ui + 0x70);
    if (LD32(vals + (uint64_t)k * 4) == v) return;
    ST32(vals + (uint64_t)k * 4, v);
    ST64(ui + 0x68, LD64(ui + 0x68) | 1ull << (k & 63));
}
static void xl_set_float(uint64_t ui, const char *name, float f) {
    int type = -1, k = xl_prop(ui, name, &type);
    if (k < 0 || !isfinite(f)) return;
    uint32_t v;
    if (type == XL_TYPE_FLOAT) memcpy(&v, &f, 4);
    else v = (uint32_t)(int32_t)f;
    xl_set_raw(ui, k, v);
}
static void xl_set_int(uint64_t ui, const char *name, int32_t val) {
    int type = -1, k = xl_prop(ui, name, &type);
    if (k < 0) return;
    uint32_t v = (uint32_t)val;
    if (type == XL_TYPE_FLOAT) {
        float f = (float)val;
        memcpy(&v, &f, 4);
    }
    xl_set_raw(ui, k, v);
}
// The sent effect property `fx_name` as a float (false: not sent, or not a float on our side).
static bool rx_fx_float(const char *fx_name, float *out) {
    if (!astree.active || !(astree.rx.flags & COOP_ASTREE_FX)) return false;
    for (int i = 0; i < COOP_FX_PROPS; i++) {
        if (strcmp(coop_fx_prop_names[i], fx_name)) continue;
        if (!(astree.rx.fx_mask >> i & 1) || fx_avatar_map.idx[i] < 0 || fx_avatar_map.type[i] != XL_TYPE_FLOAT) return false;
        memcpy(out, &astree.rx.fx[i], 4);
        return isfinite(*out);
    }
    return false;
}
// The sent whole-number effect property `fx_name` translated to sound enum values when learned from our player.
static bool rx_fx_snd_enum(const char *fx_name, int32_t *out) {
    if (!astree.active || !(astree.rx.flags & COOP_ASTREE_FX)) return false;
    for (int i = 0; i < COOP_FX_PROPS; i++) {
        if (strcmp(coop_fx_prop_names[i], fx_name)) continue;
        if (!(astree.rx.fx_mask >> i & 1) || fx_avatar_map.idx[i] < 0 || fx_avatar_map.type[i] == XL_TYPE_FLOAT) return false;
        int32_t e = (int32_t)astree.rx.fx[i];
        if (!snd_enum_on()) { *out = e; return true; }
        for (int j = 0; j < snd_enum[i].n; j++)
            if (snd_enum[i].e[j] == e) { *out = snd_enum[i].s[j]; return true; }
        return false;
    }
    return false;
}

static void snd_dump(const char *tag, uint64_t ui) {
    if (!ui) { LOG("[coop] snd %s: no sound set\n", tag); return; }
    uint64_t tb = LD64(ui + 0x30), defs = LD64(tb + 0x48), vals = LD64(ui + 0x70);
    unsigned n = LD32(tb + 0x44) & 0xffff;
    LOG("[coop] snd %s: sound set %llx, %u properties\n", tag, (unsigned long long)ui, n);
    for (unsigned j = 0; j < n && j < 64 && in_guest(defs); j++) {
        uint64_t d = LD64(defs + j * 8), nm = in_guest(d) ? LD64(d + 0x10) : 0;
        uint32_t v = LD32(vals + j * 4);
        float f;
        memcpy(&f, &v, 4);
        LOG("[coop] snd %s:  %2u %s type %u = %g / %d\n", tag, j, in_guest(nm) ? (const char *)(uintptr_t)nm : "?",
            in_guest(d) ? LD32(d + 0x60) : 0, (double)f, (int)v);
    }
}

// The AS the avatar plays now in the body slot (tree mode), or "" (classified states: by COOP_ANIM_*).
static const char *snd_body_as(void) {
    return astree.active && astree.as[0] && astree.as[0] < COOP_AS_NAME_COUNT ? coop_as_names[astree.as[0]] : "";
}
static bool snd_feet_on_ground(const CoopState *rs) {
    const char *n = snd_body_as();
    if (n[0]) {
        static const char *const air[] = {"Swim", "Climb", "Ladder", "Parashawl", "Para", "Fall", "Jump", "Wall",
                                          "Surf", "Motorcycle", "Dead", "Die", "Freeze", "Ice", "Slipping", "Sand",
                                          "Wakeboard", "Tree", "Demo"};
        for (size_t i = 0; i < sizeof air / sizeof air[0]; i++)
            if (starts(n, air[i])) return false;
        return !strstr(n, "Horse") && !strstr(n, "ShieldRide") && !strstr(n, "Jump") && !strstr(n, "Fall");
    }
    int a = rs->anim;
    return a == COOP_ANIM_WALK || a == COOP_ANIM_RUN || a == COOP_ANIM_DASH || a == COOP_ANIM_SQUAT_MOVE ||
           a == COOP_ANIM_WAIT;
}
static float snd_dist_local(void) {
    if (!last_local.have) return 1e9f;
    float d2 = 0;
    for (int i = 0; i < 3; i++) d2 += (avatar.pos[i] - last_local.pos[i]) * (avatar.pos[i] - last_local.pos[i]);
    return sqrtf(d2);
}

// Footsteps: the helper on the landing foot fires the footstep key.
static struct {
    float floor[2];  // per foot: its lowest ankle height above the avatar's root lately
    bool up[2];      // lifted since its last step
    uint64_t last_ns[2], land_ns;
    float prev_pos[3];
    uint64_t prev_ns;
    unsigned fired;
} steps;
static bool step_fire(Cpu *c, const float at[3], const char *key, float speed_ms, int subtype, float fwd) {
    if (!step_pup.actor || step_pup.frames < 2 || (snd_mute_dev & 2)) return false;
    float m[12];
    memcpy(m, (void *)(uintptr_t)(avatar.actor + ACTOR_MTX), sizeof m);
    m[3] = at[0], m[7] = at[1], m[11] = at[2];
    memcpy((void *)(uintptr_t)(step_pup.actor + ACTOR_MTX), m, sizeof m);
    memcpy((void *)(uintptr_t)(step_pup.actor + ACTOR_MTX2), m, sizeof m);
    uint64_t ui = xl_user(step_pup.actor, XL_SLINK);
    if (!ui) return false;
    bool fix = avsteps_on();
    float hs, v, depth = 0.0f;
    bool has_depth = rx_fx_float("深さ(Rea)", &depth);
    float spd30 = rx_fx_float("水平速度", &hs) ? hs : (fix ? speed_ms / 30.0f : speed_ms);
    // Speed thresholds in PlayerFootStep (0.07, 0.15, 0.22) are in metres/30Hz-frame; ELink "水平速度" already is.
    xl_set_float(ui, "スピード", spd30);
    if (has_depth) xl_set_float(ui, "深さ(Rea)", depth);
    if (rx_fx_float("濡れ具合", &v)) {
        xl_set_float(ui, "足の濡れ具合", v);
        if (fix) xl_set_float(ui, "濡れ具合", v);
    }
    if (fix) {
        xl_set_int(ui, "リアクションサブタイプ", subtype);
        xl_set_int(ui, "スケールタイプ", SCALE_TYPE_NORMAL);
        xl_set_float(ui, "歩行前進度合", fwd);
        xl_set_float(ui, "勾配", 0.0f);
    }
    // (2026-11-18) The ground under the partner's feet (the helper's set had 0 there: some other ground's steps).
    if (snd_enum_on() && astree.active && (astree.rx.flags & COOP_ASTREE_FX) && xl_map(ui, &snd_step_map, "step helper sound")) {
        CoopAsTree g;
        snd_translate(&snd_step_map, &astree.rx, &g);
        uint32_t keep = 0;
        for (int i = 0; i < COOP_FX_PROPS; i++)
            if (snd_ground_prop(i)) keep |= 1u << i;
        g.fx_mask &= keep;
        xl_write(&snd_step_map, &g);
        static unsigned said;
        if (said++ < 6 || coop_look_log())
            for (int i = 0; i < COOP_FX_PROPS; i++)
                if (snd_ground_prop(i) && snd_step_map.idx[i] >= 0)
                    LOG("[coop] snd: step %s: sent %d -> written %d%s\n", coop_fx_prop_names[i], (int)astree.rx.fx[i],
                        (int)LD32(LD64(ui + 0x70) + (uint64_t)snd_step_map.idx[i] * 4), g.fx_mask >> i & 1 ? "" : " (not written)");
    }
    bool step_ok = xl_event(c, step_pup.actor, key, XL_EV_SLINK);
    // Vanilla Player (FUN_71011f5fd8 -> FUN_71011f87cc) also fires WaterTrack* on GameROMPlayer when wading;
    // PlayerFootStep produces 0 samples when マテリアル is Water (8). BOTW zeroes 深さ(Rea) on the avatar
    // between ticks, so set 深さ(Rea) and 速度 on the avatar sound set right before firing WaterTrack*.
    bool wok = false;
    if (!strcmp(key, "PlayerFootStep") || !strcmp(key, "PlayerFootStepNaked")) {
        int32_t smat = -1, swmat = -1;
        rx_fx_snd_enum("マテリアル", &smat);
        rx_fx_snd_enum("水マテリアル", &swmat);
        if ((has_depth && depth > 0.005f) || smat == 8 || swmat == 8 ||
            smat == 11 || swmat == 11 || smat == 12 || swmat == 12 || smat == 35 || swmat == 35) {
            uint64_t av_ui = avatar.actor && avatar_alive() && avatar.frames >= 30 ? xl_user(avatar.actor, XL_SLINK) : 0;
            if (av_ui) {
                xl_set_float(av_ui, "深さ(Rea)", depth > 0.005f ? depth : 0.02f);
                xl_set_float(av_ui, "速度", spd30);
                const char *wt = (swmat == 35 || smat == 35) ? "WaterTrackGrudge"
                               : (swmat == 12 || smat == 12) ? "WaterTrackBog"
                               : (swmat == 11 || smat == 11) ? "WaterTrackLava"
                                                             : "WaterTrack";
                wok = xl_event(c, avatar.actor, wt, XL_EV_SLINK);
                if (steps.fired < 30 || coop_look_log())
                    LOG("[coop] snd: water step %s (depth %.2f, spd %.3f) -> %s\n", wt, depth, spd30,
                        wok ? "fired" : "no sound set");
            }
        }
    }
    return step_ok || wok;
}
static void steps_frame(Cpu *c, const CoopState *rs, bool have) {
    static int on = -1;
    const char *want = avsteps_on() ? STEP_ACTOR_VANILLA : STEP_ACTOR;
    if (on < 0) {
        on = snd_on() && look_on("SAVAGEWIND_COOP_STEPS") && (avsteps_on() || pack_exists(STEP_ACTOR));
        LOG("[coop] snd: the other player's footsteps: %s (%s)\n", on ? "on" : "off (switched off, or no Dm_CoopStep in the overlay)",
            want);
    }
    bool av = avatar.actor && avatar_alive() && avatar.frames >= 30;
    bool near = av && have && snd_dist_local() < STEP_NEAR_M;
    if (!pup_need(c, &step_pup, on && near ? want : "")) {
        steps.prev_ns = 0;
        return;
    }
    step_pup.frames++;
    uint64_t now = plat_now_ns();
    float speed = 0, fwd = 1.0f;  // avatar's horizontal speed (m/s) and forward alignment in [-1, 1]
    if (steps.prev_ns && now > steps.prev_ns) {
        float dx = avatar.pos[0] - steps.prev_pos[0], dz = avatar.pos[2] - steps.prev_pos[2];
        float d = sqrtf(dx * dx + dz * dz);
        speed = d / (float)((now - steps.prev_ns) / 1e9);
        if (!isfinite(speed) || speed > 40) speed = 0;
        else if (d > 1e-4f) {
            const float *am = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
            fwd = (dx * am[2] + dz * am[10]) / d;
            if (!isfinite(fwd)) fwd = 1.0f;
            else if (fwd < -1.0f) fwd = -1.0f;
            else if (fwd > 1.0f) fwd = 1.0f;
        }
    }
    memcpy(steps.prev_pos, avatar.pos, sizeof steps.prev_pos);
    steps.prev_ns = now;
    // Bare feet: no lower armor on the other player.
    bool naked = have && !rs->armor[2][0];
    // Landings (the body AS becomes a landing).
    // (coop-multi: static uint16_t was_as; -> per avatar, avl)
    // (coop-multi: static uint8_t was_seq; -> per avatar, avl)
    uint16_t as0 = astree.active ? astree.as[0] : 0;
    if (as0 && (as0 != avl.st_was_as || astree.seq[0] != avl.st_was_seq)) {
        const char *n = coop_as_names[as0];
        if (!strcmp(n, "Land") || starts(n, "LandDamage") || !strcmp(n, "CutJumpLand") || !strcmp(n, "CutFallLand") ||
            !strcmp(n, "LadderLand")) {
            float at[3] = {avatar.pos[0], avatar.pos[1], avatar.pos[2]};
            bool ok = step_fire(c, at, naked ? "PlayerLandNaked" : "PlayerLand", speed, RSUB_PL_LAND_FIRST, 1.0f);
            steps.land_ns = now;
            if (steps.fired++ < 30 || coop_look_log()) LOG("[coop] snd: the other player lands (%s): %s\n", n, ok ? "fired" : "no sound set");
        }
    }
    avl.st_was_as = as0, avl.st_was_seq = astree.seq[0];
    if (!snd_feet_on_ground(rs) || speed < 0.4f) {
        steps.up[0] = steps.up[1] = false;
        return;
    }
    static const char *const ankle[2] = {"Ankle_L", "Ankle_R"};
    float root = ((const float *)(uintptr_t)(avatar.actor + ACTOR_MTX))[7];
    for (int f = 0; f < 2; f++) {
        float b[12];
        if (!bone_matrix(c, avatar.actor, ankle[f], b)) continue;
        float h = b[7] - root;
        if (!isfinite(h) || fabsf(h) > 3) continue;
        if (!steps.floor[f] || h < steps.floor[f]) steps.floor[f] = h;
        else steps.floor[f] += 0.002f;  // (slowly up again: slopes, stairs)
        if (h > steps.floor[f] + 0.06f) steps.up[f] = true;
        else if (steps.up[f] && h < steps.floor[f] + 0.025f && now - steps.last_ns[f] > 180000000ull &&
                 now - steps.land_ns > 300000000ull) {
            steps.up[f] = false;
            steps.last_ns[f] = now;
            float at[3] = {b[3], b[7], b[11]};
            bool ok = step_fire(c, at, naked ? "PlayerFootStepNaked" : "PlayerFootStep", speed,
                                f == 0 ? RSUB_PL_FS_L : RSUB_PL_FS_R, fwd);
            if (steps.fired++ < 30 || coop_look_log())
                LOG("[coop] snd: footstep %s (%.1f m/s, %s) -> %s\n", ankle[f] + 6, speed, naked ? "bare" : "shod",
                    ok ? "fired" : "no sound set");
        }
    }
}

// Sword swings: the weapon puppet fires its weapon's attack sound when the avatar starts an attack AS.
static const char *swing_key(const char *as) {
    if (!strcmp(as, "CutS2")) return "PlayerAttackNormal2";
    if (!strcmp(as, "CutS3") || !strcmp(as, "CutS4")) return "PlayerAttackNormal3";
    if (!strcmp(as, "CutSF") || !strcmp(as, "CutJustFinish") || !strcmp(as, "CutRodeoFinish") || !strcmp(as, "CutChargeF") ||
        !strcmp(as, "CutChargeLswordSpin"))
        return "PlayerAttackFinish";
    if (!strcmp(as, "CutJumpSt") || !strcmp(as, "CutFall") || !strcmp(as, "CutJumpHorseRide")) return "PlayerAttackJump";
    if (!strcmp(as, "CutS1") || !strcmp(as, "CutDash") || !strcmp(as, "CutHorseRide") || !strcmp(as, "CutAfterJust") ||
        !strcmp(as, "CutJustRush") || !strcmp(as, "CutRodeoRush") || !strcmp(as, "CutNinja") || !strcmp(as, "CutReverse") ||
        !strcmp(as, "CutShieldRide") || !strcmp(as, "CutZoraRide") || !strcmp(as, "CutAfterJump"))
        return "PlayerAttackNormal1";
    return NULL;
}
static const char *swing_key_anim(int a) {
    switch (a) {
    case COOP_ANIM_CUT1: case COOP_ANIM_CUT_DASH: return "PlayerAttackNormal1";
    case COOP_ANIM_CUT2: return "PlayerAttackNormal2";
    case COOP_ANIM_CUT3: return "PlayerAttackNormal3";
    case COOP_ANIM_CUT_FINISH: case COOP_ANIM_CUT_SPIN: return "PlayerAttackFinish";
    case COOP_ANIM_CUT_JUMP_ST: return "PlayerAttackJump";
    default: return NULL;
    }
}
static bool swing_fire(Cpu *c, const char *key) {
    struct Pup *p = &gear_pup[COOP_GEAR_WEAPON];
    if (!p->actor || !pup_alive(p) || strncmp(p->name, "Dm_CoopW_", 9) || (snd_mute_dev & 4)) return false;
    return xl_event(c, p->actor, key, XL_EV_SLINK);
}
static void swings_frame(Cpu *c, const CoopState *rs, bool have) {
    static int on = -1;
    if (on < 0) on = snd_on() && look_on("SAVAGEWIND_COOP_SWINGSND"),
                LOG("[coop] snd: sword swing sounds on the weapon puppet: %s\n", on ? "on" : "off");
    // (coop-multi: static uint16_t was[2]; -> per avatar, avl)
    // (coop-multi: static uint8_t was_seq[2]; -> per avatar, avl)
    // (coop-multi: static int was_anim = -1; -> per avatar, avl)
    // (coop-multi: static uint8_t was_anim_seq; -> per avatar, avl)
    static unsigned said;
    if (!on || !avatar.actor || !avatar_alive() || !local_in_world() || !have) return;
    const char *key = NULL, *why = "";
    if (astree.active) {
        static const int idx[2] = {0, 1 * COOP_AS_LAYERS};  // body slot 0.0, upper body 1.0
        // (the same AS starting in both slots in one frame is one swing: the first slot that starts one wins)
        for (int k = 0; k < 2; k++) {
            uint16_t a = astree.as[idx[k]];
            uint8_t s = astree.seq[idx[k]];
            if (!key && a && a < COOP_AS_NAME_COUNT && (a != avl.sw_was[k] || s != avl.sw_was_seq[k]))
                key = swing_key(coop_as_names[a]), why = coop_as_names[a];
            avl.sw_was[k] = a, avl.sw_was_seq[k] = s;
        }
        avl.sw_was_anim = -1;
    } else {
        if (rs->anim != avl.sw_was_anim || rs->anim_seq != avl.sw_was_anim_seq) key = swing_key_anim(rs->anim), why = "classified";
        avl.sw_was_anim = rs->anim, avl.sw_was_anim_seq = rs->anim_seq;
    }
    if (!key) return;
    bool ok = swing_fire(c, key);
    if (said++ < 40 || coop_look_log()) LOG("[coop] snd: swing %s (%s) -> %s\n", key, why, ok ? "fired" : "no weapon puppet with sounds");
}

// (polish2, 2026-11-20) The ice block while frozen: "Frozen" (ELink, looping) fired WITH a handle so it can be
// stopped (A_FX_KILL, avatarfx.inc) on the break / when the state ends. SAVAGEWIND_COOP_FROZENFX=1 turns it on.
static void status_effect_stop(Cpu *c, uint64_t h) {
    if (h && elink_live_effect(h)) {
        Cpu saved = *c;
        c->x[0] = h;
        guest_call(c, A_FX_KILL);  // (checks the id itself)
        *c = saved;
    }
    if (h) memset((void *)(uintptr_t)h, 0, 0x20);
}
static void actor_ice_material(Cpu *c, uint64_t actor, bool show);

// Status effects on the avatar: shock sparks, freezing, breaking out of the ice.
static void status_frame(Cpu *c, const CoopState *rs, bool have) {
    static int on = -1, frozen_fx = -1;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_STATUSFX");
        frozen_fx = getenv("SAVAGEWIND_COOP_FROZENFX") && !strcmp(getenv("SAVAGEWIND_COOP_FROZENFX"), "1");
        LOG("[coop] snd: status effects on the avatar (shock, ice break): %s; ice effect while frozen: %s\n",
            on ? "on" : "off", frozen_fx ? "on" : "off");
    }
    static unsigned said;
    bool in_world = local_in_world();
    uint32_t unit = on && in_world && have && avatar.actor && avatar.frames >= 30 && avatar_alive() ? avatar.unit_id : 0;
    if (unit != avl.status_unit) {
        if (in_world) {
            if (avl.ice_shown && avatar.unit_id == avl.status_unit && avatar_alive())
                actor_ice_material(c, avatar.actor, false);
            status_effect_stop(c, avl.ice_h);
            status_effect_stop(c, avl.fire_h);
        }
        avl.ss_was = 0;
        avl.fire_shown = avl.ice_shown = false;
        avl.status_unit = unit;
    }
    if (!unit) return;
    uint8_t elem = elemarrow_on() ? rs->flags & COOP_FLAG_LINK_ELEM : 0;
    bool fire = elem & COOP_FLAG_LINK_FIRE;
    if (fire != avl.fire_shown) {
        status_effect_stop(c, avl.fire_h);
        if (fire) {
            if (!avl.fire_h) avl.fire_h = (uint64_t)(uintptr_t)hle_alloc(0x20);
            bool ok = xl_event_h(c, avatar.actor, "Chemical_Fire", XL_EV_ELINK, avl.fire_h);
            LOG("[coop] elemarrow: avatar burning %s\n", ok && elink_live_effect(avl.fire_h) ? "started" : "no live effect");
        }
        avl.fire_shown = fire;
    }
    int now = 0;
    const char *n = snd_body_as();
    if (n[0]) now = !strcmp(n, "DamageShock") ? 1 : !strcmp(n, "Freeze") ? 2 : !strcmp(n, "FreezeBreak") ? 3 : 0;
    else now = rs->anim == COOP_ANIM_FREEZE ? 2 : rs->anim == COOP_ANIM_FREEZE_BREAK ? 3 : 0;
    if (!now && (elem & COOP_FLAG_LINK_ICE)) now = 2;
    if (!now && (elem & COOP_FLAG_LINK_SHOCK)) now = 1;
    bool ice = now == 2 && (frozen_fx || (elem & COOP_FLAG_LINK_ICE));
    if (ice || avl.ice_shown) actor_ice_material(c, avatar.actor, ice);
    if (ice != avl.ice_shown) {
        status_effect_stop(c, avl.ice_h);
        if (ice) {
            if (!avl.ice_h) avl.ice_h = (uint64_t)(uintptr_t)hle_alloc(0x20);
            if (elem & COOP_FLAG_LINK_ICE) fx_emit(c, avatar.actor, "Frozen_Start");
            bool ok = xl_event_h(c, avatar.actor, "Frozen", XL_EV_ELINK, avl.ice_h);
            LOG("[coop] snd: status Frozen on the avatar -> %s\n", ok && elink_live_effect(avl.ice_h) ? "started" : "no live effect");
        }
        avl.ice_shown = ice;
    }
    if (now == avl.ss_was) return;
    bool ok = true;
    const char *what = "";
    if (now == 1) what = "ElectricShock", ok = xl_event(c, avatar.actor, "ElectricShock", XL_EV_BOTH);
    if (now == 2) what = "FrozenStart", ok = xl_event(c, avatar.actor, "FrozenStart", XL_EV_SLINK);
    if (now == 3 || (avl.ss_was == 2 && now == 0)) what = "Frozen_End", ok = xl_event(c, avatar.actor, "Frozen_End", XL_EV_BOTH);
    if (what[0] && (said++ < 40 || coop_look_log())) LOG("[coop] snd: status %s on the avatar -> %s\n", what, ok ? "fired" : "failed");
    avl.ss_was = now;
}

// Each frame (game thread, coop.c after the gear puppets are placed).
static void avatarsnd_frame(Cpu *c, const CoopState *rs, bool have) {
    snd_mute_log();
    if (snd_dev.pending) {
        snd_dev.pending = 0;
        uint64_t a = avatar.actor && avatar_alive() ? avatar.actor : 0;
        switch (snd_dev.what) {
        case SND_DEV_PROPS: {
            snd_dump("avatar", a ? xl_user(a, XL_SLINK) : 0);
            uint64_t pinfo = g_site.player_info ? LD64(g_site.player_info) : 0, pa = in_guest(pinfo) ? LD64(pinfo + 0x60) : 0;
            snd_dump("player", in_guest(pa) ? xl_user(pa, XL_SLINK) : 0);
            snd_dump("step helper", step_pup.actor ? xl_user(step_pup.actor, XL_SLINK) : 0);
            snd_dump("weapon puppet", gear_pup[COOP_GEAR_WEAPON].actor ? xl_user(gear_pup[COOP_GEAR_WEAPON].actor, XL_SLINK) : 0);
            break;
        }
        case SND_DEV_STEP: {
            float at[3] = {avatar.pos[0], avatar.pos[1], avatar.pos[2]};
            LOG("[coop] snd: dev footstep -> %s\n",
                a && step_fire(c, at, "PlayerFootStep", 3.0f, RSUB_PL_FS_L, 1.0f) ? "fired" : "no step helper (avatar near?)");
            break;
        }
        case SND_DEV_SWING:
            LOG("[coop] snd: dev swing %s -> %s\n", snd_dev.key, swing_fire(c, snd_dev.key) ? "fired" : "no weapon puppet with sounds");
            break;
        default:
            LOG("[coop] snd: dev key %s (%s) on the avatar -> %s\n", snd_dev.key, snd_dev.what == SND_DEV_BOTH ? "both sets" : "sound",
                a && xl_event(c, a, snd_dev.key, snd_dev.what == SND_DEV_BOTH ? XL_EV_BOTH : XL_EV_SLINK) ? "fired" : "no sound set");
        }
    }
    {
        uint64_t pinfo = local_in_world() && g_site.player_info ? LD64(g_site.player_info) : 0;
        uint64_t pa = in_guest(pinfo) ? LD64(pinfo + 0x60) : 0;
        if (in_guest(pa)) snd_learn(pa);
        else snd_player_map.ui = 0;
    }
    if (local_in_world() && avatar.actor && avatar_alive() && avatar.frames >= 30 && astree.active) snd_write(avatar.actor, &astree.rx);
    steps_frame(c, rs, have);
    swings_frame(c, rs, have);
    status_frame(c, rs, have);
}
