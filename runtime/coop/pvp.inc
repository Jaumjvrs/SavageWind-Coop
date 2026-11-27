// Part of runtime/coop.c (included there, same translation unit): friendly damage between the two players.
//
// ---- friendly damage (2026-11-11: friendly damage and the like). SAVAGEWIND_COOP_PVP=0 turns it off.
// The player has the same received-hit object as enemies at *(player actor + 0x840) (probe 2026-11-11, `coop watch`
// during a real Bokoblin hit on the plateau): for ONE frame the game finds +0x40 a reaction level (1: AS DamageM,
// 2: AS DamageL), +0x44 the damage taken (quarter hearts), +0x48 the attack's power, +0x50 0, +0x54 a kind (0x11 /
// 0x15 seen), +0x5c 2, +0x6c 0, +0x7c 2, +0x8c flags (0x14 / 0x10); the game resets them the next frame. Writing the
// same fields at our frame hook makes the game take the hearts and play the hit reaction itself (player_hit).
//
// Melee: decided in the VICTIM's game, with no packet: while the other player's avatar plays a sword / spear /
// two-handed attack (the classified state, one hit per action instance `anim_seq`), the blade of its weapon (the
// avatar's Weapon_R bone times the weapon's matrix relative to that bone, as the gear puppet is drawn) is swept from
// the last frame to this one and tested against our Link's body (a capsule). What the victim sees hit them is what
// hurts them; the attacker's lag does not matter.
#define PVP_RECEIVER 0x840
#define PVP_LIFE 0x848
static bool pvp_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_PVP") && !strcmp(getenv("SAVAGEWIND_COOP_PVP"), "0"));
    return on;
}

// Inject a hit on our player (game thread, at the frame hook). level: 1 = medium reaction (AS DamageM, ~1.5 m push),
// 2 = large (AS DamageL, knocked down ~4.5 m away). Never lethal: the damage stops one quarter heart short of 0
// (friendly fire must not kill a partner).
// Direction (2026-11-11, Ghidra): the damage states turn Link by atan2 of the vector the received-hit object's vfunc
// +0xf0 returns (FUN_7100c96ed4 / c8a1b0: angle -> player+0x1c68) and the reaction pushes him along it. With our
// melee-like source (+0x5c 2) that vector comes from a contact record we do not have: zero, so every injected hit
// pushed toward +z (probe: 1.45 m +z whatever way Link faced). That getter (0x7000b8c654) returns the normalized
// vector at +0xcc/+0xd0/+0xd4 when +0x5c is 7 and bit 6 of the u16 at +0x216 is set: the hit is injected that way,
// with `dir` = the way the hit travels (attacker -> us), and bit 6 is cleared again a few frames later (hit_restore).
// (2026-11-15) player_hit_ex(..., lethal = true): an enemy's hit (an enemy arrow that hit the avatar in the other game)
// may take the last heart, as it would in the game.
static struct { uint64_t r; int frames; } hit_restore;
static int player_hit_ex(uint64_t pa, int dmg, int level, int kind, int flags, const float dir[3], bool lethal) {
    uint64_t r = in_guest(pa) ? LD64(pa + PVP_RECEIVER) : 0;
    if (!in_guest(r)) return -1;
    int life = (int)LD32(pa + PVP_LIFE);
    if (!lethal && dmg > life - 1) dmg = life - 1;
    if (dmg > life) dmg = life;
    if (dmg < 0) dmg = 0;
    bool aimed = dir && isfinite(dir[0]) && isfinite(dir[1]) && isfinite(dir[2]) &&
                 dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] > 1e-4f;
    ST32(r + 0x40, (uint32_t)level);
    ST32(r + 0x44, (uint32_t)dmg);
    ST32(r + 0x48, (uint32_t)(dmg > 0 ? dmg : 1));
    ST32(r + 0x50, 0);
    ST32(r + 0x54, (uint32_t)kind);
    ST32(r + 0x5c, aimed ? 7 : 2);
    ST32(r + 0x6c, 0);
    ST32(r + 0x7c, 2);
    ST32(r + 0x8c, (uint32_t)flags);
    if (aimed) {
        memcpy((void *)(uintptr_t)(r + 0xcc), dir, 12);
        ST16(r + 0x216, (uint16_t)(LD16(r + 0x216) | 0x40));
        hit_restore.r = r, hit_restore.frames = 30;
    }
    return dmg;
}
static int player_hit(uint64_t pa, int dmg, int level, int kind, int flags, const float dir[3]) {
    return player_hit_ex(pa, dmg, level, kind, flags, dir, false);
}
static void hit_restore_frame(void) {
    if (hit_restore.frames <= 0 || --hit_restore.frames > 0) return;
    if (in_guest(hit_restore.r)) ST16(hit_restore.r + 0x216, (uint16_t)(LD16(hit_restore.r + 0x216) & ~0x40));
}
static struct { volatile int pending; int dmg, level, kind, flags; float dir[3]; } hit_dev;  // `coop hit <dmg> [level kind flags [dx dz]]`
static struct { volatile int pending; int cls, dmg; } foeshot_dev;  // `coop foeshot <class 0-4> <dmg>` (dev)

// Weapon attack power (quarter hearts), from the player's own game files: build_overlay.py writes
// <overlay>/coop_weapon_power.txt ("Weapon_Sword_001 5" per line). Without it (old overlay): a class table.
#define PVP_POWER_MAX 400
static struct { char name[32]; int power; } pvp_power[PVP_POWER_MAX];
static int pvp_power_n = -1;
static int weapon_power(const char *name) {
    if (pvp_power_n < 0) {
        pvp_power_n = 0;
        const char *dir = getenv("SAVAGEWIND_ROMFS_OVERLAY");
        char path[600];
        snprintf(path, sizeof path, "%s/coop_weapon_power.txt", dir ? dir : "");
        FILE *f = dir && *dir ? fopen(path, "r") : NULL;
        char line[128];
        while (f && pvp_power_n < PVP_POWER_MAX && fgets(line, sizeof line, f)) {
            char n[64];
            int p;
            if (sscanf(line, "%63s %d", n, &p) == 2 && strlen(n) < 32 && p >= 0 && p < 10000)
                snprintf(pvp_power[pvp_power_n].name, 32, "%s", n), pvp_power[pvp_power_n++].power = p;
        }
        if (f) fclose(f);
        LOG("[coop] pvp: %d weapon powers read%s\n", pvp_power_n, f ? "" : " (no coop_weapon_power.txt: class table)");
    }
    for (int i = 0; i < pvp_power_n; i++)
        if (!strcmp(pvp_power[i].name, name)) return pvp_power[i].power;
    if (!strncmp(name, "Weapon_Lsword_", 14)) return 10;
    if (!strncmp(name, "Weapon_Spear_", 13)) return 7;
    return 5;
}
// (2026-11-15) An enemy's own Attack Power (its unarmed blows), from the player's files: build_overlay.py writes
// <overlay>/coop_enemy_power.txt ("Enemy_Bokoblin_Junior 4" per line). -1: not listed (old overlay).
#define ENEMY_POWER_MAX 700
static struct { char name[56]; int power; } enemy_pw[ENEMY_POWER_MAX];
static int enemy_pw_n = -1;
static int enemy_power(const char *name) {
    if (enemy_pw_n < 0) {
        enemy_pw_n = 0;
        const char *dir = getenv("SAVAGEWIND_ROMFS_OVERLAY");
        char path[600], line[160];
        snprintf(path, sizeof path, "%s/coop_enemy_power.txt", dir ? dir : "");
        FILE *f = dir && *dir ? fopen(path, "r") : NULL;
        while (f && enemy_pw_n < ENEMY_POWER_MAX && fgets(line, sizeof line, f)) {
            char n[80];
            int p;
            if (sscanf(line, "%79s %d", n, &p) == 2 && strlen(n) < 56 && p >= 0 && p < 10000)
                snprintf(enemy_pw[enemy_pw_n].name, 56, "%s", n), enemy_pw[enemy_pw_n++].power = p;
        }
        if (f) fclose(f);
        LOG("[coop] foe melee: %d enemy attack powers read%s\n", enemy_pw_n, f ? "" : " (no coop_enemy_power.txt)");
    }
    for (int i = 0; i < enemy_pw_n; i++)
        if (!strcmp(enemy_pw[i].name, name)) return enemy_pw[i].power;
    return -1;
}
static void guard_fx(Cpu *c, uint64_t pa, const float from[3]);  // (below: the game's guard sparkles)
// Blade length (m from the grip) by weapon class.
static float weapon_reach(const char *name) {
    if (!strcmp(name, "Weapon_Lsword_056")) return 1.3f;  // (the Korok Leaf)
    if (!strncmp(name, "Weapon_Lsword_", 14)) return 1.5f;
    if (!strncmp(name, "Weapon_Spear_", 13)) return 1.9f;
    return 1.05f;
}

// The attacks whose blade can hit: the sword combo / dash / spin and the jump / fall attacks.
static bool pvp_attack_anim(int a) {
    return coop_anim_is_cut(a) || a == COOP_ANIM_CUT_JUMP_ST || a == COOP_ANIM_CUT_JUMP_LOOP ||
           a == COOP_ANIM_CUT_JUMP_LAND || a == COOP_ANIM_CUT_FALL || a == COOP_ANIM_CUT_FALL_LAND;
}

// Closest distance between segments p0-p1 and q0-q1.
static float seg_seg(const float p0[3], const float p1[3], const float q0[3], const float q1[3]) {
    float d1[3], d2[3], r[3];
    for (int k = 0; k < 3; k++) d1[k] = p1[k] - p0[k], d2[k] = q1[k] - q0[k], r[k] = p0[k] - q0[k];
    float a = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2], e = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
    float f = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2], s = 0, t = 0;
    if (a < 1e-8f && e < 1e-8f) s = t = 0;
    else if (a < 1e-8f) s = 0, t = f / e;
    else {
        float c = d1[0] * r[0] + d1[1] * r[1] + d1[2] * r[2];
        if (e < 1e-8f) t = 0, s = -c / a;
        else {
            float b = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2], den = a * e - b * b;
            s = den > 1e-8f ? (b * f - c * e) / den : 0;
            if (s < 0) s = 0;
            if (s > 1) s = 1;
            t = (b * s + f) / e;
            if (t < 0) t = 0, s = -c / a;
            else if (t > 1) t = 1, s = (b - c) / a;
        }
    }
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float d = 0;
    for (int k = 0; k < 3; k++) {
        float x = p0[k] + d1[k] * s - (q0[k] + d2[k] * t);
        d += x * x;
    }
    return sqrtf(d);
}

#define PVP_BODY_R 0.45f    // our Link's body capsule radius (+ the blade's thickness)
#define PVP_BODY_LO 0.25f   // capsule axis from feet + LO to feet + HI
#define PVP_BODY_HI 1.50f
#define PVP_DMG_MAX 8       // one hit takes at most 2 hearts (a strong weapon must not wipe a 3-heart partner)
static float LD32F(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }
static struct {
    uint8_t seq_cur, seq_done;
    bool have_prev, armed;
    float base_prev[3], tip_prev[3];
    uint64_t start_ns;
    int hits;
} pvp;
static struct { volatile uint64_t until_ns; } pvptrace_dev;  // `coop pvptrace <s>` (dev): blade vs body, per frame

// Is our Link able to take a hit now (in the world, alive, no cutscene)?
static bool pvp_victim_ok(Cpu *c, uint64_t pa, char *as, size_t cap) {
    if (!local_in_world() || !in_guest(pa)) return false;
    uint64_t mgr = g_site.event_mgr ? LD64(g_site.event_mgr) : 0;
    if (in_guest(mgr) && LD64(mgr + 0x1d2b8)) return false;  // (an event / cutscene runs here)
    player_as_name(c, pa, as, cap);
    if (!as[0] || starts(as, "Dead") || starts(as, "Die") || starts(as, "Demo")) return false;
    return (int)LD32(pa + PVP_LIFE) > 0;
}

// Our Link guards toward the attacker (shield up, facing within ~75 degrees of them): the hit is blocked.
static bool pvp_guarded(Cpu *c, uint64_t pa, const float from[3]) {
    char all[256];
    as_list(c, pa, all, sizeof all);
    if (!strstr(all, "Guard") && !strstr(all, "AttentionUpper")) return false;
    if (strstr(all, "GuardBreak")) return false;
    const float *m = (const float *)(uintptr_t)(pa + ACTOR_MTX);
    float dx = from[0] - m[3], dz = from[2] - m[11], d = sqrtf(dx * dx + dz * dz);
    if (d < 0.01f) return true;
    return (m[2] * dx + m[10] * dz) / d > 0.26f;  // (facing = the matrix's z column)
}

// `coop hitwatch <addr> <len> [addr2 len2]` (dev, RE): copies the regions every frame; in the frame our player's
// received-hit object holds a hit (+0x44 != 0) and the two after it, every changed word is logged.
#define HITWATCH_MAX 0x4000
static struct { volatile int on; uint64_t addr[2]; uint32_t len[2]; uint8_t prev[2][HITWATCH_MAX]; int left, lines; bool fresh; } hitwatch;
static void hitwatch_frame(uint64_t pa) {
    if (!hitwatch.on) return;
    uint64_t r = in_guest(pa) ? LD64(pa + PVP_RECEIVER) : 0;
    bool hit = in_guest(r) && LD32(r + 0x44);
    if (hit && hitwatch.left <= 0) hitwatch.left = 4;
    for (int w = 0; w < 2; w++) {
        if (!hitwatch.len[w] || !in_guest(hitwatch.addr[w])) continue;
        const uint8_t *now = (const uint8_t *)(uintptr_t)hitwatch.addr[w];
        if (hitwatch.left > 0 && !hitwatch.fresh)
            for (uint32_t o = 0; o + 4 <= hitwatch.len[w] && hitwatch.lines < 3000; o += 4)
                if (memcmp(now + o, hitwatch.prev[w] + o, 4)) {
                    uint32_t a, b;
                    float f;
                    memcpy(&a, hitwatch.prev[w] + o, 4), memcpy(&b, now + o, 4), memcpy(&f, &b, 4);
                    LOG("[coop] hitwatch %d left %d +0x%x: %08x -> %08x (%g)\n", w, hitwatch.left, o, a, b, f);
                    hitwatch.lines++;
                }
        memcpy(hitwatch.prev[w], now, hitwatch.len[w]);
    }
    hitwatch.fresh = false;
    if (hitwatch.left > 0) hitwatch.left--;
}

// -- melee (victim side): the avatar's blade against our body
static void pvp_melee(Cpu *c, uint64_t pa, const CoopState *rs, uint64_t now, bool trace) {
    const CoopGear *g = &rs->gear[COOP_GEAR_WEAPON];
    // (the Korok Leaf blows wind instead: pvp_wind)
    if (!pvp_on() || !pvp_attack_anim(rs->anim) || !g->name[0] || g->bone != COOP_BONE_WEAPON_R ||
        !strcmp(g->name, "Weapon_Lsword_056")) {
        pvp.have_prev = false;
        return;
    }
    if (rs->anim_seq == pvp.seq_done) return;  // (this swing already hit us)
    if (rs->anim_seq != pvp.seq_cur) pvp.seq_cur = rs->anim_seq, pvp.start_ns = now, pvp.have_prev = false;
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    const float *am = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
    float apos[3] = {am[3], am[7], am[11]};
    if (dist3(apos, me) > 5.0f && !trace) { pvp.have_prev = false; return; }
    float b[12], m[12];
    if (!bone_matrix(c, avatar.actor, "Weapon_R", b)) return;
    // (the bone is from the last model update, before this frame's move of the avatar: carried along, gear_frame)
    float inv[12], delta[12], bn[12];
    if (avatar.have_prev && mtx_inv(avatar.mtx_prev, inv)) {
        mtx_mul(am, inv, delta);
        mtx_mul(delta, b, bn);
        memcpy(b, bn, sizeof b);
    }
    mtx_mul(b, g->rel, m);
    float reach = weapon_reach(g->name);  // (the blade runs along the weapon's y axis: pvptrace 2026-11-11)
    float base[3] = {m[3], m[7], m[11]}, tip[3] = {m[3] + m[1] * reach, m[7] + m[5] * reach, m[11] + m[9] * reach};
    float lo[3] = {me[0], me[1] + PVP_BODY_LO, me[2]}, hi[3] = {me[0], me[1] + PVP_BODY_HI, me[2]};
    float best = 1e9f;
    for (int i = 1; i <= 4; i++) {  // (swept from the last frame's blade: a swing moves the tip ~0.3 m a frame)
        float t = pvp.have_prev ? i / 4.0f : 1.0f, bb[3], tt[3];
        for (int k = 0; k < 3; k++) {
            bb[k] = pvp.have_prev ? pvp.base_prev[k] + (base[k] - pvp.base_prev[k]) * t : base[k];
            tt[k] = pvp.have_prev ? pvp.tip_prev[k] + (tip[k] - pvp.tip_prev[k]) * t : tip[k];
        }
        float d = seg_seg(bb, tt, lo, hi);
        if (d < best) best = d;
    }
    memcpy(pvp.base_prev, base, 12), memcpy(pvp.tip_prev, tip, 12), pvp.have_prev = true;
    if (trace)
        LOG("[coop] pvptrace: %s seq %u %.0f ms grip %.2f %.2f %.2f tip %.2f %.2f %.2f me %.2f %.2f %.2f avatar %.2f %.2f "
            "%.2f blade-body %.2f m\n", coop_anim_as[rs->anim], rs->anim_seq, (now - pvp.start_ns) / 1e6, base[0],
            base[1], base[2], tip[0], tip[1], tip[2], me[0], me[1], me[2], apos[0], apos[1], apos[2], best);
    if (best > PVP_BODY_R) return;
    char as[64];
    if (!pvp_victim_ok(c, pa, as, sizeof as)) {
        static unsigned said;
        if (said++ < 20) LOG("[coop] pvp: the blade touches us but we can't be hit now (AS %s)\n", as);
        return;
    }
    pvp.seq_done = rs->anim_seq;
    if (pvp_guarded(c, pa, apos)) {
        LOG("[coop] pvp: blocked the other player's %s with the shield (%s)\n", coop_anim_as[rs->anim], as);
        guard_fx(c, pa, apos);
        return;
    }
    int power = weapon_power(g->name), dmg = power > PVP_DMG_MAX ? PVP_DMG_MAX : power;
    bool big = rs->anim == COOP_ANIM_CUT_SPIN || rs->anim == COOP_ANIM_CUT_FINISH || rs->anim == COOP_ANIM_CUT_JUMP_LAND ||
               rs->anim == COOP_ANIM_CUT_FALL_LAND || !strncmp(g->name, "Weapon_Lsword_", 14);
    int life = (int)LD32(pa + PVP_LIFE);
    float away[3] = {me[0] - apos[0], 0, me[2] - apos[2]};
    int got = player_hit(pa, dmg, big ? 2 : 1, big ? 0x15 : 0x11, big ? 0x10 : 0x14, away);
    pvp.hits++;
    LOG("[coop] pvp: hit by the other player's %s (%s, power %d): -%d life (%d -> %d), %s reaction\n",
        coop_anim_as[rs->anim], g->name, power, got, life, life - got, big ? "large" : "medium");
}

// ---- enemy melee on the partner (2026-11-15: enemies must really attack the second player).
// Seen (a two-game test + tgttrace): the side that runs an enemy points it at the avatar and it swings at it (96 of 248
// samples aimed at the avatar were attack AS), but the copy in the partner's game (AI off, AS copied) almost never
// takes the partner's hearts: host life 76 -> 76 in 2 of 3 runs. So, victim side, no packet (as pvp_melee): while a
// following enemy copy plays an attack AS, the blade of the weapon it holds (the nearest Weapon_ actor to its hand,
// blade along the weapon's y axis) is swept from the last frame to this one against our Link's body; a fast-moving
// blade that touches it is a hit by the game's rules (shield toward it blocks; may take the last heart). One hit per
// swing (FOE_MELEE_GAP_NS apart, so DoubleAttack can hit twice). A hit the game itself gave us in the last 0.5 s
// counts as that swing's hit. SAVAGEWIND_COOP_FOE_MELEE=0: off.
#define FOE_MELEE_MAX 6
#define FOE_MELEE_GAP_NS 600000000ull
#define FOE_MELEE_SPEED 3.0f  // m/s: the tip moves at least this fast (a swing, not a weapon held still against us)
#define FOE_MELEE_DMG 4       // without a weapon power (unknown weapon): one heart
#define FOE_MELEE_BODY_R 0.6f  // our body + the club's thickness + a little for the copy lagging the real enemy
static bool foe_melee_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_FOE_MELEE") && !strcmp(getenv("SAVAGEWIND_COOP_FOE_MELEE"), "0"));
    return on;
}
static struct FoeMelee {
    uint64_t enemy, weapon, wunit;
    uint32_t wid;
    char wname[32], as[32];
    bool have_prev;
    float base_prev[3], tip_prev[3];
    uint64_t prev_ns, hit_ns, scan_ns;
    // (unarmed) the hands (Weapon_R / Weapon_L bones, when the enemy has them) and the body, last frame
    float pt_prev[3][3];
    bool have_pts;
    int8_t has_bone[2];  // -1 unknown, 0 no such bone, 1 yes
    bool said_unarmed;
} foe_melee[FOE_MELEE_MAX];
static struct { int life; uint64_t game_hit_ns; uint64_t own_hit_ns; int hits; } foe_melee_g;
static struct { uint64_t enemy; const float *hand; float best; uint64_t a; char name[32]; } foe_wsearch;
static bool foe_weapon_name(const char *n) {
    return !strncmp(n, "Weapon_", 7) && strncmp(n, "Weapon_Bow_", 11) && strncmp(n, "Weapon_Shield_", 14) &&
           strnlen(n, 32) < 32;
}
static void foe_weapon_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)ctx;
    if (a == held[COOP_GEAR_WEAPON].actor) return;  // (our own sword)
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    float p[3] = {m[3], m[7], m[11]}, d = dist3(p, foe_wsearch.hand);
    if (d < foe_wsearch.best) foe_wsearch.best = d, foe_wsearch.a = a, snprintf(foe_wsearch.name, 32, "%s", name);
}
// ---- guard sparkles (2026-11-15: blocking the other player's attack lacked the sparkles seen when
// you block an enemy attack). Ghidra: the game's hit reactions are drawn by the ReactionMgr (DAT_7101e3f2b0, built by
// FUN_71011f1270 with the effect set "ReactionHit"); its hit-effect part (*(mgr+0xa0), FUN_71011f3708) emits the key
// "Hit_Guard" for a guarded hit through FUN_7101100bd0(handle out, *(part+0x38) = the actor holding the ReactionHit
// effect user, key, ...: the rest only feeds a debug print), then places the effect itself: event +0xc0..+0xef = a
// 3x4 matrix (rows, translation in the 4th column), +0xb4/+0xb8/+0xbc = scale, +0x180 |= 8 (use that matrix); the
// handle is {event, id at +8} and is good while *(event+0x20) == id. We do the same at our shield (or in front of
// our Link) when our guard blocks the other player, an enemy copy's swing or an arrow. SAVAGEWIND_COOP_GUARDFX=0: off.
// No sound: the ReactionHit sound set has no guard key (the clank is the shield's own).
#define G_REACTION_MGR 0x7001e3f2b0ull
#define S_HIT_GUARD 0x7001663758ull  // ("Hit_Guard" in the game's own strings; checked before use)
static uint64_t guardfx_mem;
static volatile int guardfx_dev;  // `coop guardfx` (dev)
static void guard_fx(Cpu *c, uint64_t pa, const float from[3]) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_GUARDFX") && !strcmp(getenv("SAVAGEWIND_COOP_GUARDFX"), "0"));
    static unsigned said;
    if (!on || !in_guest(pa) || memcmp((const void *)(uintptr_t)S_HIT_GUARD, "Hit_Guard", 10)) return;
    uint64_t mgr = LD64(G_REACTION_MGR), part = in_guest(mgr) ? LD64(mgr + 0xa0) : 0;
    uint64_t act = in_guest(part) ? LD64(part + 0x38) : 0;
    if (!in_guest(act) || !in_guest(LD64(act + 0x568))) {
        if (said++ < 5) LOG("[coop] guard fx: no hit-effect emitter (mgr %llx)\n", (unsigned long long)mgr);
        return;
    }
    if (!guardfx_mem) guardfx_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
    if (!guardfx_mem) return;
    static bool named;
    if (!named) {
        named = true;
        char nm[40] = "";
        const char *src = (const char *)(uintptr_t)(act + 0x1c);
        for (int i = 0; i < 39 && src[i] >= 0x20 && src[i] < 0x7f; i++) nm[i] = src[i], nm[i + 1] = 0;
        LOG("[coop] guard fx: emitter %llx '%s', effect user %llx\n", (unsigned long long)act, nm,
            (unsigned long long)fx_user(act));
    }
    if (!fx_user(act)) return;
    // (2026-11-15: calling FUN_7101100bd0 directly crashed the game (access to 0); the game's own event call with a
    // handle (xl_event_h, the path the avatar's effects already use) emits the same key and fills the same handle)
    memset((void *)(uintptr_t)guardfx_mem, 0, 0x40);
    if (!xl_event_h(c, act, "Hit_Guard", XL_EV_ELINK, guardfx_mem)) return;
    uint64_t ev = LD64(guardfx_mem);
    uint32_t id = LD32(guardfx_mem + 8);
    if (!in_guest(ev) || LD32(ev + 0x20) != id) {
        if (said++ < 5) LOG("[coop] guard fx: Hit_Guard was not emitted\n");
        return;
    }
    // where: our shield if we hold one, else 0.5 m in front of our Link's chest, toward the attacker
    const float *pm = (const float *)(uintptr_t)(pa + ACTOR_MTX);
    float at[3] = {pm[3], pm[7] + 1.0f, pm[11]}, dx = from[0] - at[0], dz = from[2] - at[2];
    float d = sqrtf(dx * dx + dz * dz);
    if (d > 0.01f) at[0] += dx / d * 0.5f, at[2] += dz / d * 0.5f;
    const struct Held *sh = &held[COOP_GEAR_SHIELD];
    if (held_alive(sh)) {
        const float *sm = (const float *)(uintptr_t)(sh->actor + ACTOR_MTX);
        float sp[3] = {sm[3], sm[7], sm[11]};
        if (dist3(sp, at) < 1.5f) memcpy(at, sp, 12);
    }
    float m[12];
    memcpy(m, pm, 48);
    m[3] = at[0], m[7] = at[1], m[11] = at[2];
    memcpy((void *)(uintptr_t)(ev + 0xc0), m, 48);
    float one = 1.0f;
    memcpy((void *)(uintptr_t)(ev + 0xb4), &one, 4), memcpy((void *)(uintptr_t)(ev + 0xb8), &one, 4);
    memcpy((void *)(uintptr_t)(ev + 0xbc), &one, 4);
    ST32(ev + 0x180, LD32(ev + 0x180) | 8);
    if (said++ < 40) LOG("[coop] guard fx: sparkles at %.2f %.2f %.2f\n", at[0], at[1], at[2]);
}

// One enemy copy's hit landed on our body (foe_melee_frame): blocked, skipped or taken.
static void foe_melee_apply(Cpu *c, uint64_t pa, struct Enemy *e, struct FoeMelee *f, const char *as,
                            const float ep[3], const float me[3], int life, uint64_t now, int power, const char *what,
                            float speed) {
    f->hit_ns = now;
    if (now - foe_melee_g.game_hit_ns < 500000000ull) {
        LOG("[coop] foe melee: %s %s touched us, but the game already hurt us (no second hit)\n", e->name, as);
        return;
    }
    if (pvp_guarded(c, pa, ep)) {
        LOG("[coop] foe melee: blocked %s's %s with the shield\n", e->name, as);
        guard_fx(c, pa, ep);
        return;
    }
    bool big = strstr(as, "Jump") || strstr(as, "Charge") || strstr(as, "Strong") || strstr(as, "Spin");
    float away[3] = {me[0] - ep[0], 0, me[2] - ep[2]};
    int got = player_hit_ex(pa, power, big ? 2 : 1, big ? 0x15 : 0x11, big ? 0x10 : 0x14, away, true);
    foe_melee_g.own_hit_ns = now, foe_melee_g.hits++;
    LOG("[coop] foe melee: hit by %s %u's %s (%s, power %d, %.2f m/s): -%d life (%d -> %d)\n", e->name, e->hash, as,
        what, power, speed, got, life, life - got);
}

// Unarmed (2026-11-15: unarmed enemies should hit the other player too): no weapon in its hand, so its
// hands (Weapon_R / Weapon_L bones: punches, slaps) and its body (lunges, Chuchu jumps, Keese dives) are what hits.
// Each is swept from the last frame to this one; a hand within 0.55 m of our body, or the body within its own size
// + 0.45 m, moving at FOE_MELEE_SPEED or more during an attack AS, is a hit of the enemy's own Attack Power (from
// the player's files, coop_enemy_power.txt; Bokoblin 4, small Chuchu 2), else FOE_MELEE_DMG.
static float foe_body_r(const char *n) {
    if (strstr(n, "Chuchu")) return strstr(n, "Senior") ? 1.1f : strstr(n, "Middle") ? 0.7f : 0.35f;
    if (strstr(n, "Keese")) return 0.3f;
    if (strstr(n, "Moblin") || strstr(n, "Lynel") || strstr(n, "Giant") || strstr(n, "Golem")) return 0.9f;
    return 0.45f;
}
static void foe_unarmed(Cpu *c, uint64_t pa, struct Enemy *e, struct FoeMelee *f, const char *as, const float ep[3],
                        const float me[3], int life, uint64_t now) {
    static const char *const bone[2] = {"Weapon_R", "Weapon_L"};
    float pt[3][3], r[3] = {0.55f, 0.55f, foe_body_r(e->name) + 0.45f};
    bool ok[3] = {false, false, true};
    for (int b = 0; b < 2; b++) {
        if (!f->has_bone[b]) continue;
        float m[12];
        ok[b] = bone_matrix(c, e->actor, bone[b], m);
        if (f->has_bone[b] < 0) f->has_bone[b] = ok[b] ? 1 : 0;
        if (ok[b]) pt[b][0] = m[3], pt[b][1] = m[7], pt[b][2] = m[11];
    }
    float lift = foe_body_r(e->name);  // (the body's centre: its size above its feet)
    pt[2][0] = ep[0], pt[2][1] = ep[1] + lift, pt[2][2] = ep[2];
    if (!f->said_unarmed) {
        f->said_unarmed = true;
        LOG("[coop] foe melee: %s %u has no weapon: its hands (%s%s) and body hit (power %d)\n", e->name, e->hash,
            f->has_bone[0] ? "R" : "-", f->has_bone[1] ? "L" : "-", enemy_power(e->name));
    }
    bool prev = f->have_pts;
    float dt = prev && now > f->prev_ns ? (float)((now - f->prev_ns) / 1e9) : 0;
    float lo[3] = {me[0], me[1] + PVP_BODY_LO, me[2]}, hi[3] = {me[0], me[1] + PVP_BODY_HI, me[2]};
    int hit = -1;
    float hit_speed = 0;
    for (int i = 0; i < 3 && prev; i++) {
        if (!ok[i]) continue;
        float speed = dt > 0 ? dist3(pt[i], f->pt_prev[i]) / dt : 0;
        if (speed < FOE_MELEE_SPEED || speed > 60.0f) continue;  // (> 60 m/s: a jump of the copy, not a blow)
        if (seg_seg(f->pt_prev[i], pt[i], lo, hi) <= r[i]) { hit = i, hit_speed = speed; break; }
    }
    memcpy(f->pt_prev, pt, sizeof pt), f->have_pts = true, f->prev_ns = now;
    if (hit < 0 || (f->hit_ns && now - f->hit_ns < FOE_MELEE_GAP_NS)) return;
    int power = enemy_power(e->name);
    if (power == 0) return;  // (listed with no attack power, e.g. Octoroks: only their rocks hurt)
    foe_melee_apply(c, pa, e, f, as, ep, me, life, now, power > 0 ? power : FOE_MELEE_DMG,
                    hit == 2 ? "its body" : hit == 0 ? "its right hand" : "its left hand", hit_speed);
}

static void foe_melee_frame(Cpu *c, uint64_t pa, uint64_t now) {
    if (!foe_melee_on() || !pvp_victim_ok(c, pa, (char[64]){0}, 64)) {
        for (int i = 0; i < FOE_MELEE_MAX; i++) foe_melee[i].have_prev = false;
        foe_melee_g.life = in_guest(pa) ? (int)LD32(pa + PVP_LIFE) : 0;
        return;
    }
    int life = (int)LD32(pa + PVP_LIFE);
    if (foe_melee_g.life && life < foe_melee_g.life && now - foe_melee_g.own_hit_ns > 300000000ull)
        foe_melee_g.game_hit_ns = now;  // (the game hurt us itself: e.g. the copy's own weapon did land)
    foe_melee_g.life = life;
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    bool used[FOE_MELEE_MAX] = {0};
    for (int k = 0; k < enemy_count; k++) {
        struct Enemy *e = &enemies[k];
        if (!e->following || !e->actor || !enemy_alive(e) || e->last <= 0) continue;
        const float *em = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
        float ep[3] = {em[3], em[7], em[11]};
        if (dist3(ep, me) > 8.0f) continue;
        char as[32];
        player_as_name(c, e->actor, as, sizeof as);
        int s = -1, free_s = -1;
        for (int i = 0; i < FOE_MELEE_MAX; i++) {
            if (foe_melee[i].enemy == e->actor) s = i;
            else if (free_s < 0 && !used[i] && (!foe_melee[i].enemy || now - foe_melee[i].prev_ns > 2000000000ull))
                free_s = i;
        }
        if (s < 0) {
            if (free_s < 0) continue;
            s = free_s;
            memset(&foe_melee[s], 0, sizeof foe_melee[s]);
            foe_melee[s].enemy = e->actor;
            foe_melee[s].has_bone[0] = foe_melee[s].has_bone[1] = -1;
        }
        struct FoeMelee *f = &foe_melee[s];
        used[s] = true;
        if (strcmp(as, f->as))
            snprintf(f->as, sizeof f->as, "%s", as), f->hit_ns = 0, f->have_prev = false, f->have_pts = false;
        if (!strstr(as, "Attack")) { f->have_prev = false, f->have_pts = false, f->prev_ns = now; continue; }
        // its weapon: kept while it lives, looked for again (at most every 0.5 s) when lost
        bool walive = in_guest(f->weapon) && in_guest(f->wunit) && LD32(f->wunit + UNIT_ID) == f->wid &&
                      LD64(f->wunit + UNIT_ACTOR) == f->weapon && LD8(f->weapon + PROC_STATE) != 3;
        if (!walive && now - f->scan_ns > 500000000ull) {
            f->scan_ns = now, f->weapon = 0;
            float hb[12];
            float hand[3] = {ep[0], ep[1] + 1.0f, ep[2]};
            if (bone_matrix(c, e->actor, "Weapon_R", hb)) hand[0] = hb[3], hand[1] = hb[7], hand[2] = hb[11];
            foe_wsearch.enemy = e->actor, foe_wsearch.hand = hand, foe_wsearch.best = 0.4f, foe_wsearch.a = 0;  // (held: 0.00 m from the bone; one lying near: not its)
            walk_actors(c, foe_weapon_name, foe_weapon_seen, NULL);
            if (foe_wsearch.a) {
                f->weapon = foe_wsearch.a, f->wunit = LD64(f->weapon + PROC_UNIT), f->wid = LD32(f->weapon + PROC_ID);
                snprintf(f->wname, sizeof f->wname, "%s", foe_wsearch.name);
                LOG("[coop] foe melee: %s %u holds %s (%.2f m from its hand)\n", e->name, e->hash, f->wname,
                    foe_wsearch.best);
            }
            walive = f->weapon != 0;
        }
        if (!walive) {  // (none found: unarmed, or it was knocked out of its hand)
            f->have_prev = false;
            if (f->weapon == 0 && f->scan_ns) foe_unarmed(c, pa, e, f, as, ep, me, life, now);
            continue;
        }
        f->have_pts = false;
        const float *m = (const float *)(uintptr_t)(f->weapon + ACTOR_MTX);
        float reach = weapon_reach(f->wname);
        float base[3] = {m[3], m[7], m[11]}, tip[3] = {m[3] + m[1] * reach, m[7] + m[5] * reach, m[11] + m[9] * reach};
        float speed = 0;
        if (f->have_prev && now > f->prev_ns) speed = dist3(tip, f->tip_prev) / (float)((now - f->prev_ns) / 1e9);
        float lo[3] = {me[0], me[1] + PVP_BODY_LO, me[2]}, hi[3] = {me[0], me[1] + PVP_BODY_HI, me[2]};
        // (traced 2026-11-15: with two games on one PC each runs ~12-20 fps, the blade turns up to ~180 degrees between
        // frames; a straight line between the old and new tip cut the arc short and missed. So the blade's direction
        // is turned from the old one to the new one around the moving grip (normalized blend), 8 steps.)
        float best = 1e9f, dn[3] = {m[1], m[5], m[9]}, dp[3];
        if (f->have_prev)
            for (int q = 0; q < 3; q++) dp[q] = (f->tip_prev[q] - f->base_prev[q]) / reach;
        for (int i = 1; i <= 8; i++) {
            float t = f->have_prev ? i / 8.0f : 1.0f, bb[3], dd[3], tt[3];
            for (int q = 0; q < 3; q++) {
                bb[q] = f->have_prev ? f->base_prev[q] + (base[q] - f->base_prev[q]) * t : base[q];
                dd[q] = f->have_prev ? dp[q] + (dn[q] - dp[q]) * t : dn[q];
            }
            float len = sqrtf(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2]);
            if (len < 0.2f) len = 0.2f;  // (a blade turned right round: the blend passes near the grip)
            for (int q = 0; q < 3; q++) tt[q] = bb[q] + dd[q] / len * reach;
            float d = seg_seg(bb, tt, lo, hi);
            if (d < best) best = d;
        }
        bool prev = f->have_prev;
        static int trace = -1;  // SAVAGEWIND_COOP_FOE_MELEE_TRACE=1 (dev): blade vs body every attack frame
        if (trace < 0) trace = getenv("SAVAGEWIND_COOP_FOE_MELEE_TRACE") && *getenv("SAVAGEWIND_COOP_FOE_MELEE_TRACE") == '1';
        if (trace) {
            float ax[3];  // (distance from our body axis to the blade if it ran along x / y / z: which axis is right)
            for (int q = 0; q < 3; q++) {
                float t2[3] = {m[3] + m[q] * reach, m[7] + m[4 + q] * reach, m[11] + m[8 + q] * reach};
                float t3[3] = {m[3] - m[q] * reach, m[7] - m[4 + q] * reach, m[11] - m[8 + q] * reach};
                float d1 = seg_seg(base, t2, lo, hi), d2 = seg_seg(base, t3, lo, hi);
                ax[q] = d1 < d2 ? d1 : -d2;
            }
            LOG("[coop] foe melee trace: %s enemy %.2f m from us, grip %.2f m, swept blade-body %.2f m, tip %.1f m/s, "
                "axis x %.2f y %.2f z %.2f (- = negative axis)\n", as, dist3(ep, me), dist3(base, me), best, speed,
                ax[0], ax[1], ax[2]);
        }
        memcpy(f->base_prev, base, 12), memcpy(f->tip_prev, tip, 12), f->have_prev = true, f->prev_ns = now;
        if (!prev || best > FOE_MELEE_BODY_R || speed < FOE_MELEE_SPEED) continue;
        if (f->hit_ns && now - f->hit_ns < FOE_MELEE_GAP_NS) continue;  // (this swing already hit us)
        int power = weapon_power(f->wname);
        foe_melee_apply(c, pa, e, f, as, ep, me, life, now, power > 0 ? power : FOE_MELEE_DMG, f->wname, speed);
    }
}

// ---- wind and blasts (SAVAGEWIND_COOP_AREAFX=0 turns them off).
// The Korok Leaf's wind is no actor (Ghidra: a physics wind volume + an effect built by the weapon's own AI action,
// EquipedDefaultWindWeapon: WindRadius 2, WindFlyingDist 8), so the other game cannot recreate it by name. Victim
// side, no packet: when the avatar swings the Leaf (Weapon_Lsword_056 in its hand, a new attack instance) and our
// Link stands in the gust in front of it (a cone 8 m long, 2 m wide at the avatar, wider further out), our Link is
// pushed away from it - an injected hit with no damage, aimed along the gust (player_hit: the game's own stagger,
// DamageM ~1.5 m; closer than 3 m: DamageL, knocked down ~4.5 m). Objects / enemies in our world are not blown.
// (Since protocol 34: DamageL for everyone in the gust + an extra slide: see WIND_PUSH_DEFAULT below.)
#define WIND_LEN 8.0f
#define WIND_RADIUS 2.0f
#define WIND_DELAY_NS 150000000ull  // (the gust leaves the leaf a moment after the swing starts)
static bool areafx_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_AREAFX") && !strcmp(getenv("SAVAGEWIND_COOP_AREAFX"), "0"));
    return on;
}
static struct { uint8_t seq_seen, seq_done; uint64_t start_ns; int gusts; } wind;

// ---- the gust's push (2026-11-11 evening: the Leaf's push looked wrong / not fluid). The player has no
// wind reaction AS (none of the player's AS is a wind / blown one), and the injected 0-damage hit
// made Link play the DamageM / DamageL stagger. Now the gust slides our Link along the wind for WIND_PUSH_NS, fast at
// first and slowing down, moved by his own physics body so walls and slopes still stop him (no animation of its own:
// whatever Link does, he keeps doing it while being carried). SAVAGEWIND_COOP_WINDPUSH picks how (dev; probed with
// `coop push <mode> <m/s> <ms>`): 0 = the old stagger hit, 1 = the body's velocity, 2 = the actor's velocity (+0x400),
// 3 = the body moved a step each frame (setPosition). Seen 2026-11-11 (a probe, two instances): mode 3, the
// avatar's Leaf swing 3.4 m in front -> our Link slid 3.07 m / 2.91 m along the gust, upright, no damage. Modes 1 and 2
// never really ran in that probe (a timing bug ended the dev push on its first frame; fixed after): unknown.
// 2026-11-11 night (play-test 3): the slide is NOT good; the protocol 31 hit needed to throw whoever it
// hits strongly, just like an enemy. So the default is the hit again (mode 0), stronger than protocol 31: everyone in
// the gust is knocked down (DamageL, pushed away along the gust like a strong enemy hit, 0 damage), and on top of it a
// short extra slide along the gust, longer the closer to the Leaf (wind_extra: ~2.5 m at the Leaf, ~0.6 m at the gust's
// end; SAVAGEWIND_COOP_WINDEXTRA=0: only the knock-down). SAVAGEWIND_COOP_WINDPUSH=1..3: the slide alone (protocol 33).
// 2026-11-12 (play-test 4: the Leaf throw still looked odd; a probe, both views): the game's own DamageL
// already throws the victim into a tumble along the ground (SlippingDown, ~10 m/s) and then the get-up; the extra slide
// only stretched that tumble with a pushed-along body. So the extra slide is OFF by default now (WINDEXTRA=1: back on).
#define WIND_PUSH_NS 600000000ull
#define WIND_EXTRA_NS 500000000ull
#define WIND_PUSH_DEFAULT 0
static int wind_push_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_WINDPUSH");
        m = e && *e >= '0' && *e <= '3' ? *e - '0' : WIND_PUSH_DEFAULT;
    }
    return m;
}
static struct {
    bool on;
    int mode;
    float dir[3], speed, start[3];
    uint64_t start_ns, last_ns, dur_ns;
    uint64_t rb, rb_actor;
    int frames;
} wpush;
static struct { volatile int pending; int mode; float speed; int ms; } push_dev;  // `coop push <mode> <m/s> <ms>`
static uint64_t player_rb(uint64_t pa) {
    if (wpush.rb_actor == pa && in_guest(wpush.rb) && in_guest(LD64(wpush.rb + 0x78))) return wpush.rb;
    bool fast = objects_fast_only;
    objects_fast_only = false;
    wpush.rb = actor_rb(pa), wpush.rb_actor = pa;
    objects_fast_only = fast;
    LOG("[coop] wind: our Link's physics body %llx\n", (unsigned long long)wpush.rb);
    return wpush.rb;
}
static void wind_push_start(uint64_t pa, const float dir[3], float speed, uint64_t dur_ns, int mode) {
    memset(&wpush.dir, 0, sizeof wpush.dir);
    float l = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    if (l < 1e-4f) return;
    wpush.dir[0] = dir[0] / l, wpush.dir[2] = dir[2] / l;
    wpush.speed = speed, wpush.mode = mode, wpush.dur_ns = dur_ns;
    wpush.start_ns = wpush.last_ns = plat_now_ns(), wpush.frames = 0, wpush.on = true;
    memcpy(wpush.start, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    if (mode != 2) player_rb(pa);
}
static void wind_push_frame(Cpu *c, uint64_t pa, uint64_t now) {
    if (!wpush.on) return;
    if (!in_guest(pa) || !local_in_world()) { wpush.on = false; return; }
    // (started in this frame after `now` was read: unsigned differences would wrap)
    float t = now > wpush.start_ns ? (float)((now - wpush.start_ns) / (double)wpush.dur_ns) : 0.0f;
    float dt = now > wpush.last_ns ? (float)((now - wpush.last_ns) / 1e9) : 0.0f;
    if (now > wpush.last_ns) wpush.last_ns = now;
    if (t >= 1.0f) {
        float me[3];
        memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
        float dx = me[0] - wpush.start[0], dz = me[2] - wpush.start[2];
        LOG("[coop] wind: push over (mode %d, %d frames): moved %.2f m (%.2f m along the wind, %.2f up)\n", wpush.mode,
            wpush.frames, sqrtf(dx * dx + dz * dz), dx * wpush.dir[0] + dz * wpush.dir[2], me[1] - wpush.start[1]);
        // (mode 1: leave the body's own velocity to the game again)
        wpush.on = false;
        return;
    }
    if (dt > 0.1f) dt = 0.1f;
    float v = wpush.speed * (1.0f - t) * (1.0f - t) * 1.5f;  // (eased out; the 1.5 keeps the distance of a linear ramp near)
    float add[3] = {wpush.dir[0] * v, 0, wpush.dir[2] * v};
    wpush.frames++;
    if (wpush.mode == 2) {  // the actor's velocity, metres per (30 fps) frame
        float vel[3];
        memcpy(vel, (const void *)(uintptr_t)(pa + 0x400), 12);
        vel[0] += add[0] * dt, vel[2] += add[2] * dt;
        memcpy((void *)(uintptr_t)(pa + 0x400), vel, 12);
        return;
    }
    uint64_t rb = player_rb(pa), body = in_guest(rb) ? LD64(rb + 0x78) : 0;
    if (!in_guest(body)) return;
    if (wpush.mode == 1) {
        float vel[3];
        memcpy(vel, (const void *)(uintptr_t)(pa + 0x400), 12);  // (the player's own move this frame, m/frame)
        float lv[3] = {vel[0] * 30 + add[0], vel[1] * 30, vel[2] * 30 + add[2]};
        rb_set_linvel(c, rb, lv);
        return;
    }
    // mode 3: a step of the body (the havok body's position at +0x1a0) through the game's setPosition
    float p[3];
    memcpy(p, (const void *)(uintptr_t)(body + 0x1a0), 12);
    p[0] += add[0] * dt, p[2] += add[2] * dt;
    if (!rb_mem) rb_mem = (uint64_t)(uintptr_t)hle_alloc(0x80);
    if (!rb_mem) return;
    memcpy((void *)(uintptr_t)(rb_mem + 0x50), p, 12);
    Cpu saved = *c;
    c->x[0] = rb; c->x[1] = rb_mem + 0x50; c->x[2] = 1;
    guest_call(c, A_RB_SET_POSITION);
    *c = saved;
}

static void pvp_wind(Cpu *c, uint64_t pa, const CoopState *rs, uint64_t now) {
    if (!areafx_on() || !pvp_attack_anim(rs->anim) || strcmp(rs->gear[COOP_GEAR_WEAPON].name, "Weapon_Lsword_056"))
        return;
    if (rs->anim_seq == wind.seq_done) return;
    if (rs->anim_seq != wind.seq_seen) { wind.seq_seen = rs->anim_seq, wind.start_ns = now; return; }
    if (now - wind.start_ns < WIND_DELAY_NS) return;
    wind.seq_done = rs->anim_seq;  // (one gust per swing, whether it reaches us or not)
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    float a = avatar.yaw * 3.14159265f / 180, f[2] = {sinf(a), cosf(a)};  // (yaw 0 = +z, write_mtx)
    float dx = me[0] - avatar.pos[0], dz = me[2] - avatar.pos[2], dy = me[1] - avatar.pos[1];
    float along = dx * f[0] + dz * f[1], side = fabsf(-dx * f[1] + dz * f[0]);
    bool in = along > -0.3f && along < WIND_LEN && side < WIND_RADIUS + along * 0.15f && fabsf(dy) < 3.0f;
    LOG("[coop] wind: the other player swings the Korok Leaf; we are %.1f m in front, %.1f m to the side: %s\n", along,
        side, in ? "blown" : "not reached");
    raft_gust(c, avatar.pos, f);  // (2026-11-21 RAFTGUST: our rafts in the gust, objects.inc)
    if (in && raft_shared_ride(me, avatar.pos)) {
        LOG("[coop] wind: we ride the same raft: the gust goes into the sail, not knocked down (RAFTCALM)\n");
        return;
    }
    char as[64];
    if (!in || !pvp_victim_ok(c, pa, as, sizeof as)) return;
    float dir[3] = {f[0], 0, f[1]};
    bool close = along < 3.0f;
    wind.gusts++;
    int mode = wind_push_mode();
    if (mode == 0) {  // (default) knocked down like by a strong enemy hit, thrown along the gust; 0 damage
        int got = player_hit(pa, 0, 2, 0x15, 0x10, dir);
        static int extra = -1;
        if (extra < 0) {  // (off by default since 2026-11-12: =1 brings the extra slide back)
            const char *e = getenv("SAVAGEWIND_COOP_WINDEXTRA");
            extra = e && *e == '1';
        }
        float metres = 0;
        if (extra && got >= 0) {
            float t = along < 0 ? 0 : along / WIND_LEN;  // (0 at the Leaf .. 1 at the gust's end)
            metres = 2.5f - 1.9f * (t > 1 ? 1 : t);
            char all[256];
            as_list(c, pa, all, sizeof all);
            if (strstr(all, "Parashawl") || strstr(all, "Fall") || strstr(all, "Jump")) metres *= 1.4f;
            wind_push_start(pa, dir, 2.0f * metres / (WIND_EXTRA_NS / 1e9f), WIND_EXTRA_NS, 3);
        }
        LOG("[coop] wind: thrown by the gust: knocked down (DamageL, 0 damage)%s, + %.1f m extra slide along the gust "
            "(%.1f m in front of the Leaf)\n", got < 0 ? " - NO hit object, not applied" : "", metres, along);
        return;
    }
    // Distance like the old reactions (DamageM ~1.5 m, DamageL ~4.5 m), but slid smoothly: ~3.5 m close up, ~1.5 m
    // at the gust's end; more in the air (a glider catches the wind).
    char all[256];
    as_list(c, pa, all, sizeof all);
    bool air = strstr(all, "Parashawl") || strstr(all, "Fall") || strstr(all, "Jump");
    float metres = close ? 3.5f : 3.5f - 2.0f * (along - 3.0f) / (WIND_LEN - 3.0f);
    if (metres < 1.5f) metres = 1.5f;
    if (air) metres *= 1.6f;
    float speed = 2.0f * metres / (WIND_PUSH_NS / 1e9f);  // (start speed of a push slowing to 0: distance = v0 T / 2)
    wind_push_start(pa, dir, speed, WIND_PUSH_NS, mode);
    LOG("[coop] wind: pushed %.1f m along the gust over %.1f s (%s, mode %d)\n", metres, WIND_PUSH_NS / 1e9,
        air ? "in the air" : "on the ground", mode);
}

// Remote bombs are real actors only in the game of the player who used them (RemoteBomb, RemoteBombCube,
// RemoteBomb2, RemoteBombCube2; no map HashId). That game tracks its own: every bomb is first held overhead by our
// Link (the rune pulls it out into the hands: *(player actor+0x88), like a lifted barrel), so it is picked up there
// with no actor walk (a 0.1 s actor walk was tried first; fast_overworld then failed 2 of 2, cause unproven), and followed
// every frame;
// one that disappears (it blew up) within 30 m of the other player sends COOP_AREAFX_BOMB with where it was. The other
// game applies it to its own Link (pvp_areafx_ev): inside BOMB_RADIUS a knock-down away from the blast (AREAFX) and up
// to 2 hearts by distance (PVP). A bomb removed without a blast (switching runes, too far away) counts as a blast too.
// Shot arrows are left out: they are no actors in this game (`coop actors Arrow` lists no shot arrow while one flies,
// 2026-11-11), so the shooter's game has nothing cheap to follow.
#define SHOT_MAX 8
#define BOMB_RADIUS 4.5f
static struct Shot { uint64_t actor, unit; uint32_t id; float pos[3], vel[3]; uint64_t at_ns; uint8_t kind; } shots[SHOT_MAX];
// (protocol 34; play-test 3: the bomb's movement had a slight delay, especially on the throw: it showed a
// little above Link's head before the throw). Causes found in the code: (1) the sender kept the carried prop on the
// avatar for 0.4 s after the release (avatar.inc local_look: "hold prop for ~0.4s so the throw doesn't show empty
// hands", made for barrels / pots) at its last spot over the head, while since protocol 33 the bomb also flies as the
// bombs[] copy: the frozen prop hung over the avatar's head during the throw; (2) the bombs[] copy was created only
// when the bomb left the hands (a new actor in that frame) and (3) eased toward each 20 Hz sample (always a little
// behind). Now: the sender treats a held remote bomb like a shared object (carry_last_shared: no prop after the
// release, the copy takes over in the same packet), sends each bomb's velocity, and the receiver creates the copy
// (parked out of sight) while the avatar still holds the bomb and moves it to the newest sample extrapolated by its
// velocity. SAVAGEWIND_COOP_BOMBSYNC=0: the protocol 33 behaviour (velocity still sent, not used).
static bool bombsync_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_BOMBSYNC");
    return on;
}
#define BOMB_LEAD_MAX_S 0.12f  // (extrapolate at most this far ahead of the newest sample)
#define BOMB_PARK_DROP 40.0f   // (the copy waits this far under the avatar while the bomb is still in its hands)
static void shot_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c; (void)ctx;
    int free_i = -1;
    for (int i = 0; i < SHOT_MAX; i++) {
        if (shots[i].actor == a && LD32(a + PROC_ID) == shots[i].id) return;
        if (!shots[i].actor && free_i < 0) free_i = i;
    }
    if (free_i < 0) return;
    struct Shot *s = &shots[free_i];
    s->actor = a, s->unit = LD64(a + PROC_UNIT), s->id = LD32(a + PROC_ID);
    s->kind = strstr(name, "Cube") ? COOP_BOMB_CUBE : COOP_BOMB_ROUND;
    s->pos[0] = LD32F(a + ACTOR_MTX + 0x0c), s->pos[1] = LD32F(a + ACTOR_MTX + 0x1c), s->pos[2] = LD32F(a + ACTOR_MTX + 0x2c);
    static unsigned said;
    if (said++ < 40) LOG("[coop] pvp: following our %s (%llx)\n", name, (unsigned long long)a);
}
static void pvp_bombs(Cpu *c, const CoopState *rs, bool have, uint64_t now) {
    if (!areafx_on()) return;
    if (!local_in_world()) { memset(shots, 0, sizeof shots); coop_net_set_bombs(NULL, 0); return; }
    (void)now;
    uint64_t pa = player_info_ptr() ? LD64(player_info_ptr() + 0x60) : 0, held_a = in_guest(pa) ? LD64(pa + 0x88) : 0;
    if (in_guest(held_a) && LD8(held_a + PROC_STATE) != 3 && !strncmp((const char *)(uintptr_t)(held_a + 0x1c), "RemoteBomb", 10)) {
        shot_seen(c, held_a, (const char *)(uintptr_t)(held_a + 0x1c), NULL);
        // (protocol 34) after the throw the bomb is shown as its bombs[] copy: no 0.4 s prop hold over the head
        // (avatar.inc local_look reads this flag next frame; objects.inc resets it while something else is held)
        if (bombsync_on()) carry_last_shared = true;
    } else if (in_guest(held_a) && !carry_on() && bombsync_on())
        carry_last_shared = false;  // (objects.inc does not run its reset with SAVAGEWIND_COOP_CARRY=0)
    // (protocol 33) our bombs lying / rolling in the world, for the other game to show: slot 0 the round one, slot 1
    // the cube (the rune allows one of each at a time). The one in Link's hands is the carried gear prop instead.
    CoopBomb out[COOP_BOMBS];
    memset(out, 0, sizeof out);
    for (int i = 0; i < SHOT_MAX; i++) {
        struct Shot *s = &shots[i];
        if (!s->actor) continue;
        bool alive = in_guest(s->unit) && LD32(s->unit + UNIT_ID) == s->id && LD64(s->unit + UNIT_ACTOR) == s->actor &&
                     LD8(s->actor + PROC_STATE) != 3;
        if (alive) {
            const float *m = (const float *)(uintptr_t)(s->actor + ACTOR_MTX);
            float np[3] = {m[3], m[7], m[11]};
            // (protocol 34) velocity from the last frame's position (a paused frame / a first sample: kept / zero)
            double dt = s->at_ns && now > s->at_ns ? (now - s->at_ns) / 1e9 : 0;
            if (dt > 0.001 && dt < 0.25)
                for (int k = 0; k < 3; k++) {
                    float v = (float)((np[k] - s->pos[k]) / dt);
                    s->vel[k] = v > 99 ? 99 : v < -99 ? -99 : v;
                }
            else if (dt >= 0.25) memset(s->vel, 0, sizeof s->vel);
            if (dt > 0.001 || !s->at_ns) s->at_ns = now;
            memcpy(s->pos, np, 12);
            CoopBomb *b = &out[s->kind == COOP_BOMB_CUBE ? 1 : 0];
            if (s->actor != held_a && !b->kind) {
                b->kind = s->kind;
                memcpy(b->pos, s->pos, 12);
                memcpy(b->vel, s->vel, 12);
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++) b->rot[r * 3 + k] = m[r * 4 + k];
                if (!coop_bomb_ok(b)) memset(b, 0, sizeof *b);
            }
            continue;
        }
        if (have && (rs->flags & COOP_FLAG_IN_WORLD) && dist3(s->pos, rs->pos) < 30.0f) {
            CoopAreaFx e = {COOP_AREAFX_BOMB, 0, 8, {s->pos[0], s->pos[1], s->pos[2]}, {0, 0, 0}, BOMB_RADIUS};
            bool sent = coop_net_send_event(COOP_EV_AREAFX, &e, sizeof e);
            LOG("[coop] pvp: our remote bomb blew up at %.1f %.1f %.1f, %.1f m from the other player: %s\n", s->pos[0],
                s->pos[1], s->pos[2], dist3(s->pos, rs->pos), sent ? "told them" : "queue full");
        }
        memset(s, 0, sizeof *s);
    }
    coop_net_set_bombs(out, COOP_BOMBS);
}

// ---- the other player's bombs in our game (protocol 33): a model where theirs lies / rolls (Dm_CoopW_RemoteBomb /
// _Cube, the carried-prop puppets: physics-free, moved by writing their matrix), and the blast as Dm_CoopBoom
// (build_overlay.py: a BarrelBomb with its attack sensor removed, so it hurts nothing; pvp_areafx_ev applies the
// damage as before), set off by an injected hit in its first frames. NOT SEEN WORKING YET: a first version that sent
// its AI straight to the explosion state only stood there as a red barrel (2026-11-11, a probe, no blast, no
// self-delete); the hit route is untested (code only, play-tested later). A barrel that does not go off is deleted after
// BOOM_GIVE_UP_NS. SAVAGEWIND_COOP_BOOM=0: no blast shown. No physics on the bomb model: nothing to kick or stand on.
static struct BombPup { struct Pup p; float shown[3]; bool placed, parked; uint64_t hide_until_ns, last_ns; } bomb_pup[COOP_BOMBS];
// The kind of remote bomb the avatar holds overhead now (its carried prop), or COOP_BOMB_NONE.
static int carried_bomb_kind(const CoopState *rs) {
    const char *n = rs->gear[COOP_GEAR_CARRIED].name;
    if (rs->gear[COOP_GEAR_CARRIED].bone == COOP_BONE_NONE || strncmp(n, "RemoteBomb", 10)) return COOP_BOMB_NONE;
    return strstr(n, "Cube") ? COOP_BOMB_CUBE : COOP_BOMB_ROUND;
}
#define BOOM_MAX 4
static struct { struct Pup p; uint64_t made_ns; int kick; } booms[BOOM_MAX];
#define BOOM_KICK_FRAMES 6
#define BOOM_GIVE_UP_NS 2000000000ull
static void pvp_bomb_view(Cpu *c, const CoopState *rs, bool have, uint64_t now) {
    CoopBomb rb[COOP_BOMBS];
    double age = 99;
    char area[96];
    bool show = areafx_on() && have && local_in_world() && (rs->flags & COOP_FLAG_IN_WORLD) &&
                rs->area == local_area(area, sizeof area) && coop_net_remote_bombs(rb, &age) && age < 1.0;
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    int held_kind = show && bombsync_on() && dist3(rs->pos, me) < 150.0f ? carried_bomb_kind(rs) : COOP_BOMB_NONE;
    for (int i = 0; i < COOP_BOMBS; i++) {
        struct BombPup *bp = &bomb_pup[i];
        const CoopBomb *b = show ? &rb[i] : NULL;
        bool real = b && b->kind != COOP_BOMB_NONE && dist3(b->pos, me) < 150.0f && now >= bp->hide_until_ns;
        // (protocol 34) the bomb is still in the avatar's hands: its copy is made now, parked out of sight, so it is
        // there in the very frame the bomb leaves the hands (slot 0 the round bomb, slot 1 the cube, as sent)
        bool pre = !real && held_kind == (i == 1 ? COOP_BOMB_CUBE : COOP_BOMB_ROUND);
        bool want = real || pre;
        int kind = real ? b->kind : held_kind;
        const char *name = !want ? "" : kind == COOP_BOMB_CUBE ? "Dm_CoopW_RemoteBombCube" : "Dm_CoopW_RemoteBomb";
        float park[3] = {0, 0, 0};
        if (pre) park[0] = rs->pos[0], park[1] = rs->pos[1] - BOMB_PARK_DROP, park[2] = rs->pos[2];
        if (bp->p.actor && (!local_in_world() || !pup_alive(&bp->p))) bp->p.actor = 0, bp->p.name[0] = 0;
        if (bp->p.actor && strcmp(bp->p.name, name)) {
            LOG("[coop] bombs: the other player's %s is gone here\n", bp->p.name);
            pup_drop(c, &bp->p);
        }
        if (!want) { bp->placed = false, bp->parked = false; continue; }
        if (!bp->p.actor) {
            if (now < bp->p.retry_ns || spawn_hold()) continue;
            bp->p.retry_ns = now + 2000000000ull;
            bp->p.actor = spawn_now(c, name, real ? b->pos : park);
            if (!bp->p.actor) {
                if (!bp->p.said) bp->p.said = true, LOG("[coop] bombs: could not create %s\n", name);
                continue;
            }
            bp->p.unit = LD64(bp->p.actor + PROC_UNIT), bp->p.id = LD32(bp->p.actor + PROC_ID);
            snprintf(bp->p.name, sizeof bp->p.name, "%s", name);
            bp->placed = false;
            if (real)
                LOG("[coop] bombs: the other player's bomb shown here (%s %llx) at %.1f %.1f %.1f\n", name,
                    (unsigned long long)bp->p.actor, b->pos[0], b->pos[1], b->pos[2]);
            else
                LOG("[coop] bombs: the other player holds a remote bomb: its copy made ahead (%s %llx), parked until the throw\n",
                    name, (unsigned long long)bp->p.actor);
        }
        if (pre) {  // (out of sight under the avatar until the bomb leaves its hands)
            static const float id3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            float m[12];
            for (int r = 0; r < 3; r++) {
                for (int k = 0; k < 3; k++) m[r * 4 + k] = id3[r * 3 + k];
                m[r * 4 + 3] = park[r];
            }
            memcpy((void *)(uintptr_t)(bp->p.actor + ACTOR_MTX), m, sizeof m);
            memcpy((void *)(uintptr_t)(bp->p.actor + ACTOR_MTX2), m, sizeof m);
            bp->placed = false, bp->parked = true;
            continue;
        }
        if (bp->parked) {
            bp->parked = false;
            LOG("[coop] bombs: the other player's bomb left their hands: shown at %.1f %.1f %.1f (moving %.1f m/s, sample %.0f ms old)\n",
                b->pos[0], b->pos[1], b->pos[2], sqrtf(b->vel[0] * b->vel[0] + b->vel[1] * b->vel[1] + b->vel[2] * b->vel[2]),
                age * 1000);
        }
        // Where it is now: the newest sample (20 a second) moved on by its velocity for the sample's age (protocol 34;
        // BOMBSYNC=0: the sample itself), then followed closely; a jump snaps.
        float target[3];
        float lead = bombsync_on() ? (float)age + 1.0f / 60 : 0;
        if (lead > BOMB_LEAD_MAX_S) lead = BOMB_LEAD_MAX_S;
        for (int k = 0; k < 3; k++) target[k] = b->pos[k] + b->vel[k] * lead;
        double fdt = bp->last_ns && now > bp->last_ns ? (now - bp->last_ns) / 1e9 : 1 / 60.0;
        bp->last_ns = now;
        float follow = bombsync_on() ? (float)(fdt * 30 > 1 ? 1 : fdt * 30) : 0.35f;
        if (!bp->placed || dist3(bp->shown, target) > 3.0f) memcpy(bp->shown, target, 12), bp->placed = true;
        else
            for (int k = 0; k < 3; k++) bp->shown[k] += (target[k] - bp->shown[k]) * follow;
        float m[12];
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) m[r * 4 + k] = b->rot[r * 3 + k];
            m[r * 4 + 3] = bp->shown[r];
        }
        memcpy((void *)(uintptr_t)(bp->p.actor + ACTOR_MTX), m, sizeof m);
        memcpy((void *)(uintptr_t)(bp->p.actor + ACTOR_MTX2), m, sizeof m);
    }
    // Blasts shown: the barrel is set off by a strong hit for its first frames (its own AI then explodes and deletes it);
    // one still there after BOOM_GIVE_UP_NS did not go off: deleted (so no barrel is left standing).
    for (int i = 0; i < BOOM_MAX; i++) {
        if (!booms[i].p.actor) continue;
        if (!local_in_world() || !pup_alive(&booms[i].p)) {
            LOG("[coop] bombs: blast %llx gone by itself after %.2f s\n", (unsigned long long)booms[i].p.actor,
                (now - booms[i].made_ns) / 1e9);
            booms[i].p.actor = 0;
            continue;
        }
        if (booms[i].kick > 0) {
            booms[i].kick--;
            enemy_inject(booms[i].p.actor, 100, 15);
        }
        if (now - booms[i].made_ns > BOOM_GIVE_UP_NS) {
            LOG("[coop] bombs: blast %llx did not go off in %.1f s: deleted\n", (unsigned long long)booms[i].p.actor,
                BOOM_GIVE_UP_NS / 1e9);
            pup_drop(c, &booms[i].p);
        }
    }
}
// Show the other player's bomb blowing up at pos (game thread, from pvp_areafx_ev).
static void pvp_boom(Cpu *c, const float pos[3], uint64_t now) {
    static int have_pack = -1;
    if (have_pack < 0) have_pack = pack_exists("Dm_CoopBoom") && look_on("SAVAGEWIND_COOP_BOOM");  // (=0: no blast shown)
    for (int i = 0; i < COOP_BOMBS; i++) {  // (their bomb's model goes, whatever the next state packet still says)
        struct BombPup *bp = &bomb_pup[i];
        if (bp->p.actor && bp->placed && dist3(bp->shown, pos) < 4.0f) {
            pup_drop(c, &bp->p);
            bp->placed = false;
        }
        if (dist3(bp->shown, pos) < 4.0f) bp->hide_until_ns = now + 800000000ull;
    }
    if (!have_pack) { LOG("[coop] bombs: no Dm_CoopBoom in the overlay (old overlay) or SAVAGEWIND_COOP_BOOM=0: no blast shown\n"); return; }
    int k = 0;
    for (int i = 0; i < BOOM_MAX; i++)
        if (!booms[i].p.actor) { k = i; break; }
    if (booms[k].p.actor) pup_drop(c, &booms[k].p);
    float at[3] = {pos[0], pos[1] - 0.3f, pos[2]};  // (the barrel's origin is its foot; the bomb's is its centre)
    uint64_t a = spawn_now(c, "Dm_CoopBoom", at);
    if (!a) { LOG("[coop] bombs: the game refused to make the blast\n"); return; }
    booms[k].p.actor = a, booms[k].p.unit = LD64(a + PROC_UNIT), booms[k].p.id = LD32(a + PROC_ID);
    snprintf(booms[k].p.name, sizeof booms[k].p.name, "Dm_CoopBoom");
    booms[k].made_ns = now, booms[k].kick = BOOM_KICK_FRAMES;
    LOG("[coop] bombs: blast shown at %.1f %.1f %.1f (%llx)\n", at[0], at[1], at[2], (unsigned long long)a);
}

static void drops_throw_ev(Cpu *c, const CoopAreaFx *e);  // (drops.inc: their thrown weapon hit us)
static void arrows_ev(Cpu *c, const CoopAreaFx *e);       // (arrows.inc, protocol 35: their arrow hit us)
static void shots_ev(Cpu *c, const CoopAreaFx *e);  // (shots.inc, protocol 51: an enemy's shot hit us there)
// The other game's bomb blast (game thread, from world_event).
static void pvp_areafx_ev(Cpu *c, const uint8_t *data, int len) {
    if (len != (int)sizeof(CoopAreaFx)) return;
    CoopAreaFx e;
    memcpy(&e, data, sizeof e);
    if (!coop_areafx_ok(&e)) { LOG("[coop] pvp: an invalid area event dropped\n"); return; }
    if (e.kind == COOP_AREAFX_THROW) { drops_throw_ev(c, &e); return; }  // (protocol 32, drops.inc)
    if (e.kind == COOP_AREAFX_ARROW || e.kind == COOP_AREAFX_FOE_ARROW) { arrows_ev(c, &e); return; }  // (arrows.inc)
    if (e.kind == COOP_AREAFX_FOE_SHOT) { shots_ev(c, &e); return; }  // (shots.inc)
    uint64_t pa = player_info_ptr() ? LD64(player_info_ptr() + 0x60) : 0;
    char as[64] = "", area[96];
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    float body[3] = {me[0], me[1] + 0.9f, me[2]}, d = dist3(body, e.pos);
    CoopState rs;
    double age = 99;
    bool same_area = coop_net_get_remote(&rs, &age) && rs.area == local_area(area, sizeof area);
    if (areafx_on() && same_area && local_in_world() && d < 150.0f) pvp_boom(c, e.pos, plat_now_ns());  // (protocol 33)
    bool ok = areafx_on() && same_area && d < e.radius && pvp_victim_ok(c, pa, as, sizeof as);
    LOG("[coop] pvp: the other player's bomb blew up %.1f m from us: %s\n", d, ok ? "caught in it" : "out of reach");
    if (!ok) return;
    int dmg = pvp_on() ? (int)(e.damage * (1.0f - 0.5f * d / e.radius) + 0.5f) : 0;
    if (dmg > PVP_DMG_MAX) dmg = PVP_DMG_MAX;
    float dir[3] = {me[0] - e.pos[0], 0, me[2] - e.pos[2]};
    if (dir[0] * dir[0] + dir[2] * dir[2] < 0.01f) dir[0] = 0, dir[2] = 1;
    int life = (int)LD32(pa + PVP_LIFE);
    int got = player_hit(pa, dmg, 2, 0x15, 0x10, dir);
    LOG("[coop] pvp: blast: -%d life (%d -> %d), knocked down\n", got, life, life - got);
}

static void pvp_frame(Cpu *c) {
    uint64_t now = plat_now_ns(), pa = player_info_ptr() ? LD64(player_info_ptr() + 0x60) : 0;
    hitwatch_frame(pa);
    hit_restore_frame();
    if (foeshot_dev.pending && in_guest(pa)) {  // `coop foeshot <class> <dmg>` (dev): as if the other game's enemy shot hit us
        foeshot_dev.pending = 0;
        const float *m = (const float *)(uintptr_t)(pa + ACTOR_MTX);
        CoopAreaFx e = {COOP_AREAFX_FOE_SHOT, (uint8_t)foeshot_dev.cls, (int16_t)foeshot_dev.dmg,
                        {m[3], m[7] + 0.9f, m[11]}, {-m[2], 0, -m[10]}, 0};
        LOG("[coop] pvp: dev foe shot class %d damage %d\n", foeshot_dev.cls, foeshot_dev.dmg);
        shots_ev(c, &e);
    }
    if (hit_dev.pending) {
        hit_dev.pending = 0;
        int got = player_hit(pa, hit_dev.dmg, hit_dev.level, hit_dev.kind, hit_dev.flags, hit_dev.dir);
        LOG("[coop] pvp: dev hit %d (level %d kind 0x%x flags 0x%x) -> %d\n", hit_dev.dmg, hit_dev.level, hit_dev.kind,
            hit_dev.flags, got);
    }
    if (push_dev.pending && in_guest(pa)) {  // `coop push <mode> <m/s> <ms>` (dev): our Link pushed the way he faces
        push_dev.pending = 0;
        const float *m = (const float *)(uintptr_t)(pa + ACTOR_MTX);
        float dir[3] = {m[2], 0, m[10]};
        wind_push_start(pa, dir, push_dev.speed, (uint64_t)push_dev.ms * 1000000ull, push_dev.mode);
        LOG("[coop] wind: dev push mode %d, %.1f m/s for %d ms, facing %.2f %.2f\n", push_dev.mode, push_dev.speed,
            push_dev.ms, dir[0], dir[2]);
    }
    wind_push_frame(c, pa, now);
    CoopState rs;
    double age = 99;
    bool have = coop_net_get_remote(&rs, &age) && age < 0.5;
    pvp_bombs(c, &rs, have, now);
    pvp_bomb_view(c, &rs, have, now);
    if (guardfx_dev && in_guest(pa)) {  // `coop guardfx` (dev)
        guardfx_dev = 0;
        const float *m = (const float *)(uintptr_t)(pa + ACTOR_MTX);
        float from[3] = {m[3] + m[2] * 3, m[7], m[11] + m[10] * 3};
        guard_fx(c, pa, from);
    }
    foe_melee_frame(c, pa, now);
    if (!have ||!avatar.actor || !avatar_alive() || avatar.frames < 6) {
        pvp.have_prev = false;
        return;
    }
    pvp_melee(c, pa, &rs, now, pvptrace_dev.until_ns && now < pvptrace_dev.until_ns);
    pvp_wind(c, pa, &rs, now);
}
