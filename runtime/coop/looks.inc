// (coop-looks, protocol 65, 2026-11-18) Telling the Links apart (colour the clothes differently,
// and an option in the coop menu to show the other players' names above them).
//
// Tunic colour: each player picks one in Options > Co-op (settings coop_tint); it travels with the player names
// (coop_net.c, the name entry's tint byte). The other games show that player's avatar in it with the game's own
// model colour uniform: the one the red hurt flash uses (uk_damage_color through 0x700124d110, avatar.inc
// model_damage_color), set per material on the avatar's body and armor puppets (coop-looks2: the colour should
// only affect the clothes, not the skin): materials named for skin, hair, face or eyes (looks_mat_is_skin) get no
// colour; on the bare body that leaves the underwear and belts. hurt_flash_frame applies it between flashes (the red
// flash itself still covers the whole model). SAVAGEWIND_COOP_TINT=0: every avatar keeps the game's colours.
// Dev: `coop looks mats` logs each material and whether it was coloured.
//
// Names above heads: once a frame (after rav_update) each other player's avatar head is projected with the game's view
// camera (hle/freecam.c freecam_game_view: eye and look point from the game's LookAtCamera) and the world projection
// (rt_world_projection: tan(fovy / 2), aspect); the overlay (ui/menu.mm coop_tags_draw) draws the names there.
// Hidden while loading, in an event / cutscene, paused / in a menu, with the free camera, behind the camera or past
// LOOKS_TAG_FAR metres (fading from LOOKS_TAG_FADE). Options > Co-op "Names above players"; SAVAGEWIND_COOP_NAMETAGS=0
// turns it off whatever the setting.

extern bool freecam_game_view(float eye[3], float at[3]);
extern bool freecam_active(void);
void rt_world_projection(float out[4]);  // (nvn/output_scale.c)

static int looks_tint_c = -1, looks_tags_c = -1, own_tint_c = -1, looks_tag_occlude_c = -1;
static unsigned looks_mats_seq;
static bool looks_tint_on(void) { return env_on("SAVAGEWIND_COOP_TINT", &looks_tint_c); }
static bool looks_tags_on(void) { return env_on("SAVAGEWIND_COOP_NAMETAGS", &looks_tags_c); }
// On again (2026-11-18 dyefix): it was off because dyeing the own Link also recoloured the partners' avatars; the
// puppets now load their own colour's textures (avatar.inc puppet_dye_holder). Off: SAVAGEWIND_COOP_OWNTINT=0.
static bool own_tint_on(void) {
    if (own_tint_c < 0) {
        const char *v = getenv("SAVAGEWIND_COOP_OWNTINT");
        own_tint_c = !(v && !strcmp(v, "0"));
    }
    return own_tint_c;
}
static bool looks_tag_occlude_on(void) { return env_on("SAVAGEWIND_COOP_TAGOCCLUDE", &looks_tag_occlude_c); }

// (botw 1.9.0 native dye) Kochi Dye Shop StainColor frame indices for dyeable armor (*_ftp / *_fsp in the model's
// FMAA anim list): 0=original, 1=blue, 2=red, 3=yellow, 4=white, 5=black, 6=purple, 7=green.
static int looks_stain_color(int tint) {
    static const int map[COOP_TINT_COUNT] = {0, 2, 1, 7, 3, 6, 5, 4};
    if (!looks_tint_on() || tint <= 0 || tint >= COOP_TINT_COUNT) return 0;
    return map[tint];
}
// Fallback colour (rgb) and strength (a) for undyeable armors (no *_ftp) or bare Link's underwear (uk_damage_color);
// softer alpha preserves fabric weave, shadows and shading instead of washing everything out into flat solid colour.
// (SAVAGEWIND_COOP_TINT_ALPHA=<0..1>: test override of the strength)
static const float looks_tints[COOP_TINT_COUNT][3] = {
    {0, 0, 0}, {0.82f, 0.10f, 0.08f}, {0.10f, 0.28f, 0.90f}, {0.08f, 0.62f, 0.16f},
    {0.92f, 0.78f, 0.08f}, {0.52f, 0.14f, 0.82f}, {0.02f, 0.02f, 0.03f}, {0.95f, 0.95f, 0.95f},
};
static float looks_tint_alpha(int tint) {
    static float a = -1;
    if (a < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_TINT_ALPHA");
        a = e && *e ? (float)atof(e) : 0.30f;
        if (!(a >= 0 && a <= 1)) a = 0.30f;
    }
    return tint == COOP_TINT_BLACK ? a * 1.25f > 1 ? 1 : a * 1.25f : a;
}
static bool looks_tint_color(int tint, float col[4]) {
    if (tint <= 0 || tint >= COOP_TINT_COUNT || !looks_tint_on()) return false;
    col[0] = looks_tints[tint][0], col[1] = looks_tints[tint][1], col[2] = looks_tints[tint][2];
    col[3] = looks_tint_alpha(tint);
    return true;
}

// The player the avatar being updated now shows: a swapped context's (multi.inc), else the partner's
static int looks_dev_tint = -1;  // (dev: coop looks show <n>) every avatar in that colour (-1: each player's own)
static int multi_in_pid(void) { return multi_in ? multi_cur : coop_net_partner_id(); }
static int looks_cur_tint(void) {
    if (looks_dev_tint >= 0) return looks_dev_tint < COOP_TINT_COUNT ? looks_dev_tint : 0;
    int pid = multi_in_pid();
    return pid >= 0 ? coop_net_player_tint(pid) : 0;
}

// Ours, to the others: Options > Co-op's choice (dev: coop looks tint <n>)
static void looks_publish(void) {
    static int sent = -1;
    int t = g_settings.coop_tint >= 0 && g_settings.coop_tint < COOP_TINT_COUNT ? g_settings.coop_tint : 0;
    if (t == sent) return;
    sent = t;
    coop_net_set_tint(t);
    LOG("[coop] looks: our tunic colour %d\n", t);
}

// ---- local player's own tunic colour (ownlook)
// Applies Options > Co-op's "Your tunic colour" (`g_settings.coop_tint`) to the local Link (`GameROMPlayer`) and its
// equipped armor actors (`pa + PLAYER_EQUIP`) using the same native StainColor anims and cloth-only `uk_damage_color`
// fallback as the remote avatars. Leaves Link completely untouched while `coop_tint == 0` unless restoring from a
// non-zero colour (preserving any real Kochi Dye Shop stain saved in `armor + 0x858`). Pauses `uk_damage_color` updates
// while the game's own hurt flash (`+0x1da4` / `+0xe60`) or status tint (`+0xb94`) is active, and re-applies on end.
#define PLAYER_STATUS_COLOR 0xb94
#define PLAYER_DMG_COLOR_V 0xe60
#define ARMOR_STAIN_COLOR 0x858
static struct {
    uint64_t pa, model;
    uint32_t pa_id;
    int tint, frames, restored;
    unsigned mats_seq;
    bool applied, game_fx, cloth_tinted;
    struct {
        uint64_t actor, model;
        uint32_t id;
        bool native_dyed, cloth_tinted;
    } slot[COOP_ARMOR_SLOTS];
} own_tint;

static bool player_has_game_dmg_color(uint64_t pa) {
    // The timed flash has its own RGBA. The packed colour can retain the neutral black
    // value after the flash, so reserve the cloth uniform for a visible status hue.
    uint32_t status = LD32(pa + PLAYER_STATUS_COLOR);
    return LDF32(pa + PLAYER_DMG_COLOR_T) > 1e-5f ||
           (status >> 8 && (status & 0xff)) ||
           LDF32(pa + PLAYER_DMG_COLOR_V + 12) > 1e-5f;
}

static uint64_t own_tint_model(uint64_t actor) {
    uint64_t m = in_guest(actor) ? LD64(actor + ACTOR_MODEL) : 0;
    return in_guest(m) && in_guest(LD64(m + 0x40)) && (int)LD32(m + 0x38) > 0 ? m : 0;
}

static void own_tint_frame(Cpu *c) {
    // A temporary pause in placement/model updates need not replace the actor or its armor.
    if (!own_tint_on() || !local_in_world() || framepace_loading_ui()) return;
    uint64_t pinfo = player_info_ptr();
    uint64_t pa = pinfo ? LD64(pinfo + 0x60) : 0;
    if (!in_guest(pa) || !in_guest(pa + PLAYER_EQUIP + 0x40) || LD8(pa + PROC_STATE) == 3) return;
    uint64_t model = own_tint_model(pa);
    if (!model) return;
    uint32_t pa_id = LD32(pa + PROC_ID);
    bool pa_changed = pa != own_tint.pa || pa_id != own_tint.pa_id;
    bool model_changed = model != own_tint.model;
    if (pa_changed) memset(&own_tint, 0, sizeof own_tint);
    if (model_changed) own_tint.cloth_tinted = false;
    float tcol[4] = {0, 0, 0, 0};
    int want = g_settings.coop_tint >= 0 && g_settings.coop_tint < COOP_TINT_COUNT ? g_settings.coop_tint : 0;
    int tint = looks_tint_color(want, tcol) ? want : 0;
    if (!tint && !own_tint.applied) return;  // (Default: never touch the local Link or Kochi Dye Shop colors)

    uint64_t armors[COOP_ARMOR_SLOTS] = {0};
    uint64_t models[COOP_ARMOR_SLOTS] = {0};
    uint32_t armor_ids[COOP_ARMOR_SLOTS] = {0};
    char anames[COOP_ARMOR_SLOTS][32];
    memset(anames, 0, sizeof anames);
    uint64_t equip = LD64(pa + PLAYER_EQUIP);
    if (!in_guest(equip)) return;
    if (!outfit_out) outfit_out = (uint64_t)(uintptr_t)hle_alloc(0x20);
    if (!outfit_out) return;
    {
        Cpu saved = *c;
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
            memset((void *)(uintptr_t)outfit_out, 0, 0x20);
            *c = saved;
            c->x[0] = equip + 0x10 + (uint64_t)i * 0x10;
            c->x[1] = outfit_out;
            guest_call(c, A_EQUIP_GET);
            uint64_t it = LD64(outfit_out + 8);
            *c = saved;
            c->x[0] = outfit_out;
            guest_call(c, A_HANDLE_RELEASE);
            *c = saved;
            if (!in_guest(it) || !in_guest(it + 0x900) || LD8(it + PROC_STATE) == 3) continue;
            const char *in = (const char *)(uintptr_t)(it + 0x1c);
            if (strncmp(in, "Armor_", 6) || !strncmp(in, "Armor_Default", 13)) continue;
            armors[i] = it;
            models[i] = own_tint_model(it);
            armor_ids[i] = LD32(it + PROC_ID);
            snprintf(anames[i], sizeof anames[i], "%.31s", in);
        }
    }

    static const float zero[4] = {0, 0, 0, 0};
    bool game_fx = player_has_game_dmg_color(pa);
    if (!tint) {
        bool pending = false;
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
            if (!armors[i] || own_tint.slot[i].actor != armors[i] || own_tint.slot[i].id != armor_ids[i]) continue;
            if (!models[i]) {
                pending = true;
                continue;
            }
            if (own_tint.slot[i].model != models[i]) continue;
            if (own_tint.slot[i].native_dyed) {
                // The co-op dye only changes the model. The actor still holds the game's real dye.
                int st = (int)LD32(armors[i] + ARMOR_STAIN_COLOR);
                if (st >= 0 && st <= 15 && armor_native_dye(c, armors[i], i, st, anames[i], st)) {
                    if (coop_look_log())
                        LOG("[coop] looks: own %s restored StainColor %d\n", anames[i], st);
                    own_tint.restored++;
                    own_tint.slot[i].native_dyed = false;
                } else pending = true;
            }
            if (own_tint.slot[i].cloth_tinted && !game_fx) {
                model_damage_color(c, armors[i], zero, false, anames[i]);
                own_tint.slot[i].cloth_tinted = false;
            }
            pending |= own_tint.slot[i].cloth_tinted;
        }
        if (own_tint.cloth_tinted && !game_fx) {
            model_damage_color(c, pa, zero, false, "ownbody");
            own_tint.cloth_tinted = false;
        }
        if (!pending && !own_tint.cloth_tinted) {
            LOG("[coop] looks: own Link back to default (restored %d native-dyed)\n", own_tint.restored);
            memset(&own_tint, 0, sizeof own_tint);
        }
        own_tint.tint = 0;
        own_tint.game_fx = game_fx;
        return;
    }

    bool armor_changed = false;
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
        if (armors[i] && !models[i]) continue;
        if (armors[i] != own_tint.slot[i].actor || armor_ids[i] != own_tint.slot[i].id ||
            models[i] != own_tint.slot[i].model) {
            armor_changed = true;
            own_tint.slot[i].actor = armors[i];
            own_tint.slot[i].id = armor_ids[i];
            own_tint.slot[i].model = models[i];
            own_tint.slot[i].native_dyed = false;
            own_tint.slot[i].cloth_tinted = false;
        }
    }
    bool mats_req = plat_now_ns() < looks_mats_until && own_tint.mats_seq != looks_mats_seq;
    bool need_dye = tint != own_tint.tint || model_changed || armor_changed || mats_req || own_tint.frames >= TINT_REFRESH;
    bool need_cloth = !game_fx && (need_dye || own_tint.game_fx);
    if (!need_dye && !need_cloth) {
        own_tint.frames++;
        own_tint.game_fx = game_fx;
        return;
    }

    int stain = looks_stain_color(tint), native_n = 0, cloth_n = 0;
    if (need_dye) {
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
            if (!models[i]) continue;
            int original = (int)LD32(armors[i] + ARMOR_STAIN_COLOR);
            if (original >= 0 && original <= 15 &&
                armor_native_dye(c, armors[i], i, stain, anames[i], original))
                own_tint.slot[i].native_dyed = true;
            if (own_tint.slot[i].native_dyed) native_n++;
        }
    } else {
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
            if (!models[i]) continue;
            if (own_tint.slot[i].native_dyed) native_n++;
        }
    }
    if (need_cloth) {
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
            if (!models[i]) continue;
            if (own_tint.slot[i].native_dyed) {
                if (own_tint.slot[i].cloth_tinted) model_damage_color(c, armors[i], zero, false, anames[i]);
                own_tint.slot[i].cloth_tinted = false;
            } else if ((int)LD32(armors[i] + ARMOR_STAIN_COLOR) < 0) {
                model_damage_color(c, armors[i], tcol, true, anames[i]);
                own_tint.slot[i].cloth_tinted = true;
                cloth_n++;
            }
        }
        model_damage_color(c, pa, tcol, true, "ownbody");
        own_tint.cloth_tinted = true;
    }
    if (tint != own_tint.tint || pa_changed || armor_changed)
        LOG("[coop] looks: own Link in colour %d (stain %d: %d native-dyed + %d cloth-tinted, body)\n", tint, stain,
            native_n, cloth_n);
    own_tint.pa = pa;
    own_tint.model = model;
    own_tint.pa_id = pa_id;
    own_tint.tint = tint;
    own_tint.frames = 0;
    own_tint.restored = 0;
    own_tint.applied = true;
    own_tint.game_fx = game_fx;
    if (mats_req) own_tint.mats_seq = looks_mats_seq;
}

// ---- name tags
#define LOOKS_TAG_FAR 60.0f
#define LOOKS_TAG_FADE 45.0f
#define LOOKS_TAG_HEAD 2.05f  // metres above the avatar's feet
#define LOOKS_TAG_OCCLUDE_NS 100000000ull  // line-of-sight raycast cadence (~10 Hz per visible player)
// Game 1.9.0 physics RayCast helpers (found via FUN_7100bd78a0 / camera occlusion FUN_71006055fc)
#define A_RAY_CTOR         0x7001306b24ull  // (ray, ignore_group, 0xf)
#define A_RAY_DTOR         0x7001305278ull  // (ray)
#define A_RAY_LAYERS_WORLD 0x70012164b0ull  // (ray): enables layers 8, 10, 9, 2, 0xc (terrain, ground, trees, objects)
#define A_RAY_SET_SEG      0x70013054d0ull  // (ray, from[3], to[3])
#define A_RAY_CAST         0x7001306b80ull  // (ray, 0) -> bool hit
#define A_RAY_HIT_POS      0x70013060b4ull  // (ray, float out[3])
static uint64_t looks_ray_mem;
static struct { bool have, occluded; uint64_t next_ns; } looks_occ[COOP_MAX_PLAYERS];
static unsigned looks_occ_logs;

static bool looks_rb_is_avatar_body(uint64_t rb, int target_pid, bool *is_target) {
    if (is_target) *is_target = false;
    if (!in_guest(rb)) return false;
    int partner = coop_net_partner_id();
    if (rb == col.rb || rb == hcol.rb) {
        if (is_target && target_pid == partner) *is_target = true;
        return true;
    }
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
        const MultiCtx *m = multi_ctx[p];
        if (m && (rb == m->col.rb || rb == m->hcol.rb)) {
            if (is_target && target_pid == p) *is_target = true;
            return true;
        }
    }
    return false;
}

static bool looks_ray_blocked(Cpu *c, const float eye[3], const float target[3], int target_pid, float *hit_dist, uint64_t *hit_rb_out) {
    if (hit_rb_out) *hit_rb_out = 0;
    float seg_len = sqrtf((target[0] - eye[0]) * (target[0] - eye[0]) +
                          (target[1] - eye[1]) * (target[1] - eye[1]) +
                          (target[2] - eye[2]) * (target[2] - eye[2]));
    if (seg_len < 0.5f) return false;
    if (!looks_ray_mem) looks_ray_mem = (uint64_t)(uintptr_t)hle_alloc(0x140);
    if (!looks_ray_mem) return false;
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t phys = in_guest(pa) ? LD64(pa + 0x578) : 0;
    uint64_t ignore_grp = in_guest(phys) ? LD64(phys + 0x188) : 0;
    if (!in_guest(ignore_grp)) ignore_grp = 0;

    float from[3] = {eye[0], eye[1], eye[2]};
    float dir[3] = {(target[0] - eye[0]) / seg_len, (target[1] - eye[1]) / seg_len, (target[2] - eye[2]) / seg_len};
    // Step past an intervening unrelated avatar's collision body once so one partner doesn't hide another's name tag.
    for (int step = 0; step < 2; step++) {
        float rem = sqrtf((target[0] - from[0]) * (target[0] - from[0]) +
                          (target[1] - from[1]) * (target[1] - from[1]) +
                          (target[2] - from[2]) * (target[2] - from[2]));
        if (rem < 0.4f) return false;
        memset((void *)(uintptr_t)looks_ray_mem, 0, 0x100);
        memcpy((void *)(uintptr_t)(looks_ray_mem + 0x100), from, 12);
        memcpy((void *)(uintptr_t)(looks_ray_mem + 0x110), target, 12);
        Cpu saved = *c;
        c->x[0] = looks_ray_mem; c->x[1] = ignore_grp; c->x[2] = 0xf;
        guest_call(c, A_RAY_CTOR);
        *c = saved;
        c->x[0] = looks_ray_mem;
        guest_call(c, A_RAY_LAYERS_WORLD);
        *c = saved;
        c->x[0] = looks_ray_mem; c->x[1] = looks_ray_mem + 0x100; c->x[2] = looks_ray_mem + 0x110;
        guest_call(c, A_RAY_SET_SEG);
        *c = saved;
        c->x[0] = looks_ray_mem; c->x[1] = 0;
        guest_call(c, A_RAY_CAST);
        bool hit = (c->x[0] & 1) != 0;
        *c = saved;
        uint64_t hit_rb = hit ? LD64(looks_ray_mem + 0xe0) : 0;
        float hp[3] = {0, 0, 0};
        if (hit) {
            c->x[0] = looks_ray_mem; c->x[1] = looks_ray_mem + 0x120;
            guest_call(c, A_RAY_HIT_POS);
            *c = saved;
            memcpy(hp, (const void *)(uintptr_t)(looks_ray_mem + 0x120), 12);
        }
        c->x[0] = looks_ray_mem;
        guest_call(c, A_RAY_DTOR);
        *c = saved;
        if (!hit) return false;
        bool is_target = false;
        if (looks_rb_is_avatar_body(hit_rb, target_pid, &is_target)) {
            if (is_target) return false;
            from[0] = hp[0] + dir[0] * 0.9f;
            from[1] = hp[1] + dir[1] * 0.9f;
            from[2] = hp[2] + dir[2] * 0.9f;
            continue;
        }
        if (hit_dist)
            *hit_dist = sqrtf((hp[0] - eye[0]) * (hp[0] - eye[0]) +
                              (hp[1] - eye[1]) * (hp[1] - eye[1]) +
                              (hp[2] - eye[2]) * (hp[2] - eye[2]));
        if (hit_rb_out) *hit_rb_out = hit_rb;
        return true;
    }
    return false;
}
static struct {
    volatile int seq;  // odd while written (game thread); the overlay reads between two equal even values
    int n;
    CoopNameTag t[COOP_MAX_PLAYERS];
} looks_tags;
static unsigned looks_tag_logs;

static void looks_tags_store(const CoopNameTag *t, int n) {
    if (!n && !looks_tags.n) return;
    __atomic_add_fetch(&looks_tags.seq, 1, __ATOMIC_SEQ_CST);
    memcpy(looks_tags.t, t, sizeof(CoopNameTag) * (size_t)n);
    looks_tags.n = n;
    __atomic_add_fetch(&looks_tags.seq, 1, __ATOMIC_SEQ_CST);
}

// (any thread: the overlay) out NULL: only how many
int coop_name_tags(CoopNameTag *out, int max) {
    for (int tries = 0; tries < 4; tries++) {
        int s0 = __atomic_load_n(&looks_tags.seq, __ATOMIC_SEQ_CST);
        if (s0 & 1) continue;
        int n = looks_tags.n;
        if (n > max && out) n = max;
        if (out && n > 0) memcpy(out, looks_tags.t, sizeof(CoopNameTag) * (size_t)n);
        if (__atomic_load_n(&looks_tags.seq, __ATOMIC_SEQ_CST) == s0) return n;
    }
    return 0;
}

// Game thread, after rav_update
static void looks_tags_frame(Cpu *c) {
    CoopNameTag t[COOP_MAX_PLAYERS];
    int n = 0;
    float eye[3] = {0}, at[3] = {0}, proj[4];
    bool show = looks_tags_on() && g_settings.coop_names && local_in_world() && !framepace_in_event() &&
                !framepace_loading_ui() && !framepace_paused() && !framepace_menu_ui() && !freecam_active() &&
                freecam_game_view(eye, at);
    if (show) rt_world_projection(proj);
    if (show && !(proj[0] > 1e-4f && proj[1] > 1e-4f)) show = false;
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]}, r[3], u[3];
    if (show) {
        float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl < 1e-4f) show = false;
        else f[0] /= fl, f[1] /= fl, f[2] /= fl;
        r[0] = -f[2], r[1] = 0, r[2] = f[0];  // (forward x up(0,1,0))
        float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
        if (rl < 1e-4f) show = false;
        else r[0] /= rl, r[2] /= rl;
        u[0] = r[1] * f[2] - r[2] * f[1], u[1] = r[2] * f[0] - r[0] * f[2], u[2] = r[0] * f[1] - r[1] * f[0];
    }
    bool occlude = show && looks_tag_occlude_on();
    if (!occlude) memset(looks_occ, 0, sizeof looks_occ);
    uint64_t now = plat_now_ns();
    uint32_t seen_mask = 0;
    for (int i = 0; show && i < rav_n && n < COOP_MAX_PLAYERS; i++) {
        const struct RemoteAv *ra = &rav[i];
        if (!ra->actor || !ra->fresh || ra->pid < 0 || ra->pid >= COOP_MAX_PLAYERS || !(ra->rs.flags & COOP_FLAG_IN_WORLD))
            continue;
        const float *m = (const float *)(uintptr_t)(ra->actor + ACTOR_MTX);
        float p[3] = {m[3], m[7] + LOOKS_TAG_HEAD, m[11]};
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) continue;
        float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        if (dist > LOOKS_TAG_FAR || z < 0.3f) continue;
        float x = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (z * proj[0] * proj[1]);
        float y = (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / (z * proj[0]);
        if (fabsf(x) > 1.2f || fabsf(y) > 1.2f) continue;
        seen_mask |= 1u << ra->pid;
        char pname[COOP_NAME_LEN];
        if (!coop_net_player_name(ra->pid, pname, sizeof pname)) snprintf(pname, sizeof pname, "Player %d", ra->pid + 1);
        if (occlude) {
            if (!looks_occ[ra->pid].have || now >= looks_occ[ra->pid].next_ns) {
                float hb[12], head[3] = {m[3], m[7] + 1.65f, m[11]};
                if (bone_matrix(c, ra->actor, "Head", hb) && isfinite(hb[3] + hb[7] + hb[11]))
                    head[0] = hb[3], head[1] = hb[7] + 0.12f, head[2] = hb[11];
                float hdist = -1.0f;
                uint64_t hrb = 0;
                bool blocked = looks_ray_blocked(c, eye, head, ra->pid, &hdist, &hrb);
                if ((!looks_occ[ra->pid].have && blocked) ||
                    (looks_occ[ra->pid].have && looks_occ[ra->pid].occluded != blocked)) {
                    if (looks_occ_logs++ < 40 || coop_look_log())
                        LOG("[coop] looks: tag \"%s\" (player %d) %s (%.1f m, hit %.1f m, rb %llx)\n", pname, ra->pid,
                            blocked ? "hidden behind obstacle" : "visible", dist, hdist, (unsigned long long)hrb);
                }
                looks_occ[ra->pid].have = true;
                looks_occ[ra->pid].occluded = blocked;
                looks_occ[ra->pid].next_ns = now + LOOKS_TAG_OCCLUDE_NS;
            }
            if (looks_occ[ra->pid].occluded) continue;
        }
        CoopNameTag *g = &t[n];
        memset(g, 0, sizeof *g);
        g->x = x * 0.5f + 0.5f, g->y = 0.5f - y * 0.5f;
        g->alpha = dist <= LOOKS_TAG_FADE ? 1.0f : 1.0f - (dist - LOOKS_TAG_FADE) / (LOOKS_TAG_FAR - LOOKS_TAG_FADE);
        g->size = dist <= 8 ? 1.0f : dist >= LOOKS_TAG_FAR ? 0.7f : 1.0f - 0.3f * (dist - 8) / (LOOKS_TAG_FAR - 8);
        g->aspect = proj[1];
        g->tint = looks_tint_on() ? coop_net_player_tint(ra->pid) : 0;
        snprintf(g->name, sizeof g->name, "%s", pname);
        if (looks_tag_logs < 6) {
            looks_tag_logs++;
            LOG("[coop] looks: name tag \"%s\" (player %d, colour %d) at %.3f %.3f, %.1f m\n", g->name, ra->pid, g->tint,
                g->x, g->y, dist);
        }
        n++;
    }
    for (int p = 0; p < COOP_MAX_PLAYERS; p++)
        if (!(seen_mask >> p & 1)) looks_occ[p].have = false;
    looks_tags_store(t, n);
}

static void looks_frame(Cpu *c) {
    looks_publish();
    own_tint_frame(c);
    looks_tags_frame(c);
}

static void looks_reset(void) {
    looks_ray_mem = 0;
    memset(&own_tint, 0, sizeof own_tint);
    memset(looks_occ, 0, sizeof looks_occ);
    looks_tags_store(NULL, 0);
}

// (dev) coop looks tint <n> | show <n|-1> | names <0|1> | tags
static int looks_dev_log(void) {
    CoopNameTag t[COOP_MAX_PLAYERS];
    int n = coop_name_tags(t, COOP_MAX_PLAYERS);
    LOG("[coop] looks: our colour %d, names %s, %d tag(s)\n", g_settings.coop_tint, g_settings.coop_names ? "on" : "off", n);
    for (int i = 0; i < n; i++)
        LOG("[coop] looks: tag \"%s\" colour %d at %.3f %.3f alpha %.2f\n", t[i].name, t[i].tint, t[i].x, t[i].y, t[i].alpha);
    return n;
}

// (coop-looks2 + cloth-only fix) Materials that must NOT be fallback-tinted: skin/hair/face/eyes, AND leather/
// metal/straps/accessories/emissives (Belt, WaistBelt, Metal, Glove, Gauntlets, Armcover, Jewel, Armor, UnderShirts,
// kusari, *emm) so undyeable sets (e.g. Classic Tunics) and bare Link only shift their cloth/tunic tonality.
static bool looks_mat_is_skin(const char *name, int len) {
    static const char *const skip[] = {
        "Skin", "Hair", "Hairtop", "Face", "Eye", "Eyeball", "Eyeemm", "Eyelashes",
        "Earring", "Finger", "Lens",
        "Belt", "WaistBelt", "Upperbelt", "Metal", "Uppermetal", "Glove", "Gauntlets",
        "Armcover", "Jewel", "Armor", "UnderShirts", "kusari", "Headeemm", "Upperemm",
    };
    int i = 0, words = 0;
    while (i < len) {
        int j = i;
        while (j < len && name[j] != '_') j++;
        int w = j - i;
        words++;
        if (words > 1) {  // (after "Mt")
            for (unsigned k = 0; k < sizeof skip / sizeof *skip; k++)
                if ((int)strlen(skip[k]) == w && !memcmp(name + i, skip[k], w)) return true;
            if (w == 4 && !memcmp(name + i, "Head", 4) && j >= len) return true;  // (Mt_Head: Link's own head)
        }
        i = j + 1;
    }
    return false;
}
