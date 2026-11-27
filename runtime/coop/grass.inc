// Part of runtime/coop.c (included after coop/pvp.inc, same translation unit): grass the other player cuts.
//
// ---- (2026-11-14: grass cut in one game did not show as cut in the other, nor did it have the cutting
// animation). The field grass is the terrain system's (ksys::tera Grass), not
// actors, so there is nothing to sync: each game cuts its own. While the other player's avatar swings a sword /
// spear / two-handed weapon (the same attacks and blade as the friendly-damage test, coop/pvp.inc pvp_melee), we cut
// the grass along its blade in OUR world with the game's own call, which also plays the cut effect (flying blades)
// and the cut sound. No packet: what the avatar's blade sweeps is what gets cut. SAVAGEWIND_COOP_GRASS=0 turns it off.
//
// Ghidra (2026-11-14): FUN_7101494184(s0 radius, s1 falloff, x0 grass, x1 float pos[3], w2 1, w3 effect+sound,
// w4 skip-check) cuts the grass in a circle around pos (only where pos.y is within -0.5..+1.5 m of the ground) and,
// where grass was standing, FUN_710149467c -> FUN_71011f1b34 spawns the "GrassCut" effect (+ "FlowerPetal_x_x").
// The callers (bomb / explosion / AI actions, e.g. FUN_710078ab34, FUN_7100653e58) get `grass` as
// FUN_710148e620(mgr) = *(*(mgr + 0x360) + 0x138) with mgr = DAT_7101e57bd8, only when (mgr + 0xa58) bit 1 is set.
#define A_GRASS_CUT 0x7001494184ull
#define A_GRASS_SWAY 0x7001493b68ull  // (Ghidra 2026-11-19: FUN_7101493b68(s0 radius, s1 inner=0, x0 grass, x1 pos[3]))
#define A_GRASS_MGR 0x7001e57bd8ull   // (DAT_7101e57bd8: the terrain manager)
#define A_GRASS_ON 0x7001e3f528ull    // (DAT_7101e3f528: the cut function returns at once when 0)

static bool grass_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_GRASS") && !strcmp(getenv("SAVAGEWIND_COOP_GRASS"), "0"));
    return on;
}
static bool grass_spear_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_GRASSSPEAR") && !strcmp(getenv("SAVAGEWIND_COOP_GRASSSPEAR"), "0"));
    return on;
}
static bool grass_rustle_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_GRASSRUSTLE") && !strcmp(getenv("SAVAGEWIND_COOP_GRASSRUSTLE"), "0"));
    return on;
}

static uint64_t grass_mem;  // guest buffer: the position (3 floats)
static struct {
    uint8_t seq;
    bool have_prev;
    float tip_prev[3];
    unsigned cuts, said, rustles, rustle_said;
    char no_cut_said[32];
} grass;
static volatile float grass_dev_r, grass_dev_rustle_r;  // `coop grasscut/grassrustle <radius>` (dev)

// The game's grass, or 0 when the terrain has none loaded now.
static uint64_t grass_obj(void) {
    uint64_t mgr = LD64(A_GRASS_MGR);
    if (!in_guest(mgr) || !((LD8(mgr + 0xa58) >> 1) & 1) || !LD8(A_GRASS_ON)) return 0;
    uint64_t t = LD64(mgr + 0x360);
    uint64_t g = in_guest(t) ? LD64(t + 0x138) : 0;
    return in_guest(g) ? g : 0;
}

// Cut the grass in a circle around p (radius m). Returns the game's result (1: there was terrain there).
static int grass_cut(Cpu *c, uint64_t g, const float p[3], float radius, float falloff) {
    if (!grass_mem) grass_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
    if (!grass_mem) return 0;
    memcpy((void *)(uintptr_t)grass_mem, p, 12);
    Cpu saved = *c;
    memset(&c->v[0], 0, 16), memset(&c->v[1], 0, 16);
    memcpy(&c->v[0], &radius, 4), memcpy(&c->v[1], &falloff, 4);
    c->x[0] = g; c->x[1] = grass_mem; c->x[2] = 1; c->x[3] = 1; c->x[4] = 1;
    guest_call(c, A_GRASS_CUT);
    int got = (int)(c->x[0] & 1);
    *c = saved;
    return got;
}

// Rustle / sway the grass around p (radius m) without cutting it: the game's own call right after the cut branch in
// FUN_71007b1848 (FUN_7101493b68 -> FUN_710139c344 -> FUN_71013a012c grass bend wave). Used for blunt weapons, spears,
// and the outer edge of cutting swings. SAVAGEWIND_COOP_GRASSRUSTLE=0 turns it off.
static void grass_rustle(Cpu *c, uint64_t g, const float p[3], float radius) {
    if (!grass_mem) grass_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
    if (!grass_mem) return;
    memcpy((void *)(uintptr_t)grass_mem, p, 12);
    Cpu saved = *c;
    float zero = 0.0f;
    memset(&c->v[0], 0, 16), memset(&c->v[1], 0, 16);
    memcpy(&c->v[0], &radius, 4), memcpy(&c->v[1], &zero, 4);
    c->x[0] = g; c->x[1] = grass_mem;
    guest_call(c, A_GRASS_SWAY);
    *c = saved;
}

// (2026-11-18: all weapons were cutting grass, though a hammer, for example, does not.) The game's own
// rule (Ghidra, the weapon's per-frame grass cut FUN_71007b1848): no cut when the weapon's GeneralParamList
// WeaponCommon.IsBlunt is set (the byte at +0x130 of that object; FUN_71014e0bbc declares "IsBlunt" there) or the
// attack kind (weapon +0xcf8) is 4 ("Stab", parsed from the AS HoldEvent TypeIndex 3 string by FUN_7100c39ae0).
// Every Player spear attack animation in GameROMPlayer.sbactorpack (CutS1..4, CutSF, CutDash, CutCharge, CutHorseRide,
// CutJumpEd, flurry rush, etc.) emits "Stab" (4), and thrown spears are also skipped explicitly by vfunc +0x748.
// The partner's weapon is only a model puppet here (no params), so the 34 IsBlunt weapons are listed by name, and
// Weapon_Spear_* skips cutting (rustling only) when SAVAGEWIND_COOP_GRASSSPEAR is on.
// SAVAGEWIND_COOP_GRASSBLUNT=0: blunt weapons cut again; SAVAGEWIND_COOP_GRASSSPEAR=0: spears cut again.
static bool weapon_is_spear(const char *name) { return !strncmp(name, "Weapon_Spear_", 13); }
static bool grass_weapon_bone(int b) { return b == COOP_BONE_WEAPON_R || b == COOP_BONE_ARM_R; }
static bool grass_attack_anim(int a) { return pvp_attack_anim(a) || a == COOP_ANIM_CUT_HORSE; }
static bool weapon_blunt(const char *name) {
    static const char *const blunt[] = {
        "Lsword_004", "Lsword_005", "Lsword_006", "Lsword_010", "Lsword_011", "Lsword_012", "Lsword_016", "Lsword_017",
        "Lsword_018", "Lsword_019", "Lsword_031", "Lsword_036", "Lsword_037", "Lsword_038", "Lsword_045", "Lsword_054",
        "Lsword_056", "Spear_031",  "Spear_036",  "Sword_004",  "Sword_005",  "Sword_006",  "Sword_019",  "Sword_020",
        "Sword_022",  "Sword_040",  "Sword_043",  "Sword_044",  "Sword_048",  "Sword_049",  "Sword_050",  "Sword_060",
        "Sword_061",  "Sword_062"};
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_GRASSBLUNT") && !strcmp(getenv("SAVAGEWIND_COOP_GRASSBLUNT"), "0"));
    if (strncmp(name, "Weapon_", 7)) return false;
    if (!on) return !strcmp(name, "Weapon_Lsword_056");  // (the Korok Leaf blows wind instead: never cut)
    for (size_t i = 0; i < sizeof blunt / sizeof *blunt; i++)
        if (!strcmp(name + 7, blunt[i])) return true;
    return false;
}

// Every frame (after pvp_frame): while the avatar swings, cut and/or rustle along its weapon path.
// (0.1.1, playtest 2026-11-26: grass reacts to Link walking through it, but not to the other Link) The game
// bends grass around moving bodies of the chemical system (Link's
// TgtChemical cylinder; the avatar has none): FUN_71012832a4 -> ... -> FUN_710140ebb4 -> FUN_710139bfd8 each frame
// per body, which pushes a directional wave FUN_710139c3dc(s0 r, s1 inner, x0 grassSys = *(grass + 0x28), x1 pos[3],
// x2 velocity[3]) (RE). We make that call for the avatar's feet while the partner
// walks / runs / sneaks on the ground. SAVAGEWIND_COOP_GRASSBEND=0: off; =ripple: the plain wave FUN_7101493b68
// instead (no direction, with the game's own ground-height check); GRASSBEND_R=<m> (dev): the radius.
#define A_GRASS_WAVE_DIR 0x700139c3dcull
static int grass_bend_mode(void) {  // 0 off, 1 directional (default), 2 ripple
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_GRASSBEND");
        m = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "ripple") ? 2 : 1;
    }
    return m;
}
static void grass_walk(Cpu *c, const CoopState *rs) {
    static float last[3];
    static uint64_t last_ns, last_av;
    static unsigned said;
    int mode = grass_bend_mode();
    if (!mode) return;
    const float *am = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
    float pos[3] = {am[3], am[7], am[11]};
    uint64_t now = plat_now_ns();
    bool cont = last_av == avatar.actor && last_ns && now - last_ns < 200000000ull;
    float dt = cont ? (float)((now - last_ns) / 1e9) : 0;
    float v[3] = {0, 0, 0};
    if (dt > 1e-3f) v[0] = (pos[0] - last[0]) / dt, v[2] = (pos[2] - last[2]) / dt;
    memcpy(last, pos, 12), last_ns = now, last_av = avatar.actor;
    bool ground = rs->anim == COOP_ANIM_WALK || rs->anim == COOP_ANIM_RUN || rs->anim == COOP_ANIM_DASH ||
                  rs->anim == COOP_ANIM_SQUAT_MOVE;
    float speed = sqrtf(v[0] * v[0] + v[2] * v[2]);
    if (!ground || !cont || speed < 0.5f || speed > 30.0f) return;
    uint64_t g = grass_obj();
    if (!g) return;
    static float r = -1;
    if (r < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_GRASSBEND_R");
        r = e ? (float)atof(e) : 1.0f;
        if (!(r >= 0.2f && r <= 5.0f)) r = 1.0f;
    }
    if (mode == 2) {
        grass_rustle(c, g, pos, r);
    } else {
        uint64_t sys = LD64(g + 0x28);
        if (!in_guest(sys)) return;
        if (!grass_mem) grass_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
        if (!grass_mem) return;
        memcpy((void *)(uintptr_t)(grass_mem + 0x20), pos, 12);
        memcpy((void *)(uintptr_t)(grass_mem + 0x30), v, 12);
        Cpu saved = *c;
        float zero = 0.0f;
        memset(&c->v[0], 0, 16), memset(&c->v[1], 0, 16);
        memcpy(&c->v[0], &r, 4), memcpy(&c->v[1], &zero, 4);
        c->x[0] = sys; c->x[1] = grass_mem + 0x20; c->x[2] = grass_mem + 0x30;
        guest_call(c, A_GRASS_WAVE_DIR);
        *c = saved;
    }
    if (said < 5) {
        said++;
        LOG("[coop] grass: the other player's %s bends the grass (%s, r %.1f, %.1f m/s) at %.1f %.1f %.1f\n",
            coop_anim_as[rs->anim], mode == 2 ? "ripple" : "directional", r, speed, pos[0], pos[1], pos[2]);
    }
}
static void grass_frame(Cpu *c) {
    if (grass_dev_r > 0) {
        float r = grass_dev_r;
        grass_dev_r = 0;
        uint64_t pi = player_info_ptr(), g = grass_obj();
        float p[3] = {0, 0, 0};
        if (pi) memcpy(p, (const void *)(uintptr_t)(pi + 0x88), 12);
        p[1] += 0.5f;
        int got = g ? grass_cut(c, g, p, r, 4.0f) : -1;
        LOG("[coop] grass: dev cut r %.1f at %.2f %.2f %.2f: grass %llx, game %d\n", r, p[0], p[1], p[2],
            (unsigned long long)g, got);
    }
    if (grass_dev_rustle_r > 0) {
        float r = grass_dev_rustle_r;
        grass_dev_rustle_r = 0;
        uint64_t pi = player_info_ptr(), g = grass_obj();
        float p[3] = {0, 0, 0};
        if (pi) memcpy(p, (const void *)(uintptr_t)(pi + 0x88), 12);
        p[1] += 0.5f;
        if (g) grass_rustle(c, g, p, r);
        LOG("[coop] grass: dev rustle r %.1f at %.2f %.2f %.2f: grass %llx\n", r, p[0], p[1], p[2],
            (unsigned long long)g);
    }
    if (!grass_on()) return;
    CoopState rs;
    double age = 99;
    if (!coop_net_get_remote(&rs, &age) || age > 0.5 || !avatar.actor || !avatar_alive() || avatar.frames < 6 ||
        !local_in_world()) {
        grass.have_prev = false;
        return;
    }
    grass_walk(c, &rs);
    const CoopGear *gear = &rs.gear[COOP_GEAR_WEAPON];
    // (HOLDARM sends Weapon_Lsword_* on COOP_BONE_ARM_R instead of COOP_BONE_WEAPON_R: accept both held-weapon bones)
    if (!grass_attack_anim(rs.anim) || !gear->name[0] || !grass_weapon_bone(gear->bone)) {
        grass.have_prev = false;
        return;
    }
    bool blunt = weapon_blunt(gear->name);
    bool is_spear = weapon_is_spear(gear->name);
    bool spear_skip = is_spear && grass_spear_on();
    bool no_cut = blunt || spear_skip;
    if (no_cut) {
        if (strncmp(grass.no_cut_said, gear->name, sizeof grass.no_cut_said - 1)) {
            snprintf(grass.no_cut_said, sizeof grass.no_cut_said, "%.31s", gear->name);
            LOG("[coop] grass: the other player's %s %s: no grass cut\n", gear->name,
                blunt ? "is blunt" : "is a spear");
        }
        if (!grass_rustle_on()) {
            grass.have_prev = false;
            return;
        }
    } else {
        grass.no_cut_said[0] = 0;
    }
    bool fresh = rs.anim_seq != grass.seq;
    if (fresh) grass.seq = rs.anim_seq, grass.have_prev = false;
    uint64_t g = grass_obj();
    if (!g) return;
    // (2026-11-14: the charged sword move on one screen cut much more grass than on the other;
    // the play logs had no spin cut at all, only slashes: the spin's few frames in the state packet left the
    // per-frame cut a small piece.) The real spin sweeps a full circle (one blade out from Link, R = blade * 2.7,
    // hard edge): cut that whole ring once when the partner's spin starts, then the per-frame cut below as usual.
    if (!no_cut && fresh && rs.anim == COOP_ANIM_CUT_SPIN) {
        // (2026-11-15, after the ring of blade * 2.7 (cut ~2.5 m): the real spin cuts way more grass than that.
        // The game's per-attack scale S was never found; the spin's blast is sized
        // by eye: a cut radius of 4 m + 0.75 m per charge level, SAVAGEWIND_COOP_SPINCUT=<metres> to tune. The game's
        // cut covers about R / 2: centre R = 2 * radius, plus 12 overlapping circles at 60% for a solid disc.)
        const float *a0 = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
        static float base = -1;
        if (base < 0) {
            const char *e = getenv("SAVAGEWIND_COOP_SPINCUT");
            base = e ? (float)atof(e) : 4.0f;
            if (!(base >= 1.0f && base <= 15.0f)) base = 4.0f;
        }
        unsigned lv = astree_charge_lv;
        float eff = base + 0.75f * (float)(lv > 2 ? 2 : lv), r = eff * 2.0f;
        float ctr[3] = {a0[3], a0[7] + 0.3f, a0[11]};
        int got = grass_cut(c, g, ctr, r, 8.0f);
        for (int k = 0; k < 12; k++) {
            float an = (float)k * (6.2831853f / 12.0f);
            float p[3] = {ctr[0] + sinf(an) * eff * 0.6f, ctr[1], ctr[2] + cosf(an) * eff * 0.6f};
            grass_cut(c, g, p, eff * 0.9f, 8.0f);
        }
        LOG("[coop] grass: the other player's spin (seq %u) cuts the full disc r %.2f around %.2f %.2f %.2f (game %d)\n",
            (unsigned)rs.anim_seq, r, ctr[0], ctr[1], ctr[2], got);
    }
    // The blade, as pvp_melee draws it: the weapon's bone (Weapon_R, or Arm_2_R for Weapon_Lsword_* with HOLDARM)
    // carried by this frame's move of the avatar, times the weapon's matrix relative to that bone; the blade runs
    // along the weapon's y axis.
    const float *am = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
    float b[12], m[12], inv[12], delta[12], bn[12];
    if (!bone_matrix(c, avatar.actor, coop_bone_name[gear->bone], b)) return;
    if (avatar.have_prev && mtx_inv(avatar.mtx_prev, inv)) {
        mtx_mul(am, inv, delta);
        mtx_mul(delta, b, bn);
        memcpy(b, bn, sizeof b);
    }
    mtx_mul(b, gear->rel, m);
    float reach = weapon_reach(gear->name);
    // (2026-11-14: the grass cut radius was always the same, it should scale with the attack; the charged
    // one cut a big circle in one game and only a small piece in the other). The game's own cut,
    // RE: the WEAPON cuts one circle a frame at its body centre (about mid-blade) with
    // R = blade * 1.4, falloff 4 (the cut covers about R / 2); the charged spin uses R = blade * 2.7, falloff 8 (hard
    // edge), centred one blade length out from Link along the blade at Link's height. Jump / fall finishers: bigger
    // than a slash (blade * 2.0, a guess: their per-attack scale was not found).)
    float tip[3] = {m[3] + m[1] * reach * 0.5f, m[7] + m[5] * reach * 0.5f, m[11] + m[9] * reach * 0.5f};
    float rad = reach * 1.4f, fo = 4.0f;
    float spin_bx = 0, spin_bz = 0;
    if (rs.anim == COOP_ANIM_CUT_SPIN) {
        float bx = m[1], bz = m[9], bl = sqrtf(bx * bx + bz * bz);
        if (bl > 1e-3f) bx /= bl, bz /= bl;
        else bx = am[2], bz = am[10];
        spin_bx = bx, spin_bz = bz;
        tip[0] = am[3] + bx * reach, tip[1] = am[7] + 0.3f, tip[2] = am[11] + bz * reach;
        rad = reach * 2.7f, fo = 8.0f;
    } else if (rs.anim == COOP_ANIM_CUT_JUMP_LAND || rs.anim == COOP_ANIM_CUT_FALL_LAND)
        rad = reach * 2.0f;
    if (rad < 1.2f) rad = 1.2f;
    if (!no_cut) {
        int got = grass_cut(c, g, tip, rad, fo);
        if (grass.have_prev) {  // (a swing moves the blade ~0.3 m a frame: also halfway from the last frame's centre)
            float h[3] = {(tip[0] + grass.tip_prev[0]) * 0.5f, (tip[1] + grass.tip_prev[1]) * 0.5f,
                          (tip[2] + grass.tip_prev[2]) * 0.5f};
            grass_cut(c, g, h, rad, fo);
        }
        memcpy(grass.tip_prev, tip, 12), grass.have_prev = true;
        grass.cuts++;
        if (grass.said < 60) {
            grass.said++;
            LOG("[coop] grass: the other player's %s (seq %u) cuts r %.2f at %.2f %.2f %.2f (game %d)\n",
                coop_anim_as[rs.anim], (unsigned)rs.anim_seq, rad, tip[0], tip[1], tip[2], got);
        }
    }
    if (grass_rustle_on()) {
        // Matches FUN_71007b1848's unconditional sway call: spears push R=3.0 m around the tip (+0.5 m past AABB half);
        // other weapons advance forward (+0.4 m normal, +1.0 m spin) with R = reach * (spin ? 3.7 : 1.8).
        float sp[3], srad;
        if (is_spear) {
            float off = reach + 0.3f;
            sp[0] = m[3] + m[1] * off, sp[1] = m[7] + m[5] * off, sp[2] = m[11] + m[9] * off;
            srad = 3.0f;
        } else if (rs.anim == COOP_ANIM_CUT_SPIN) {
            sp[0] = tip[0] + spin_bx * 1.0f, sp[1] = tip[1], sp[2] = tip[2] + spin_bz * 1.0f;
            srad = reach * 3.7f;
        } else {
            sp[0] = tip[0] + m[1] * 0.4f, sp[1] = tip[1] + m[5] * 0.4f, sp[2] = tip[2] + m[9] * 0.4f;
            srad = reach * (rs.anim == COOP_ANIM_CUT_JUMP_LAND || rs.anim == COOP_ANIM_CUT_FALL_LAND ? 2.5f : 1.8f);
        }
        if (srad < 1.8f) srad = 1.8f;
        if (no_cut && fresh && rs.anim == COOP_ANIM_CUT_SPIN && !is_spear) {
            // Charged blunt spin (e.g. hammer): rustle the full ring around the avatar on spin start.
            float ctr[3] = {am[3], am[7] + 0.3f, am[11]};
            grass_rustle(c, g, ctr, srad);
            for (int k = 0; k < 8; k++) {
                float an = (float)k * (6.2831853f / 8.0f);
                float p[3] = {ctr[0] + sinf(an) * reach * 1.5f, ctr[1], ctr[2] + cosf(an) * reach * 1.5f};
                grass_rustle(c, g, p, srad * 0.7f);
            }
        }
        grass_rustle(c, g, sp, srad);
        if (no_cut) {
            if (grass.have_prev) {
                float h[3] = {(sp[0] + grass.tip_prev[0]) * 0.5f, (sp[1] + grass.tip_prev[1]) * 0.5f,
                              (sp[2] + grass.tip_prev[2]) * 0.5f};
                grass_rustle(c, g, h, srad);
            }
            memcpy(grass.tip_prev, sp, 12), grass.have_prev = true;
            grass.rustles++;
            if (grass.rustle_said < 60) {
                grass.rustle_said++;
                LOG("[coop] grass: the other player's %s (%s, seq %u) rustles r %.2f at %.2f %.2f %.2f\n",
                    coop_anim_as[rs.anim], gear->name, (unsigned)rs.anim_seq, srad, sp[0], sp[1], sp[2]);
            }
        }
    }
}
