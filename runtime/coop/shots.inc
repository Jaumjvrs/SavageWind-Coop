// Part of runtime/coop.c (included there, same translation unit): enemy shots hitting the other player (protocol 51).
//
// (2026-11-15: enemy projectiles) Rocks, fireballs, ice and shock balls that enemies shoot or throw are actors
// of the game's "Bullet" profile (ActorInfo: Octarock_Bullet, Rock_Weapon, FireRodLv1Fire, Lizalfos_IceBall, ...). Like
// enemy arrows (arrows.inc): only the game that runs an enemy has its shots; when the enemy aims at the avatar (the
// partner), the shot flies at the avatar and would never hurt the partner. Here, a shot of a known kind that appears
// within SHOT_FOE_M of an enemy we run (and not right at our Link) is followed every frame; when its path passes through
// the avatar's body capsule, or it bursts / vanishes right at it (they break on the avatar's collision body), a
// reliable COOP_EV_AREAFX kind 5 tells the other game, whose Link takes the hit by the game's rules there: a shield held
// toward it blocks it (with the game's block sparkles), damage = the shot's own Attack Power where its
// file gives a real one (pw: Octorok rock 2 - a real hit on the host cost 2 -, boulder 8, bomb 30, ...), else the
// shooter's (coop_enemy_power.txt; 4 when unknown), big rocks / bombs knock down, it can take the last heart. Fire / ice / shock shots also burn / freeze / shock (FOE_ELEM, elemarrow.inc; the class
// is sent for that). The following copy of the enemy in the other game shoots nothing (its AI is off).
// SAVAGEWIND_COOP_FOE_SHOTS=0 turns it off; `coop shottrace <s>` (dev) logs every frame of every followed shot.
#define FSHOT_MAX 16
#define SHOT_FOE_M 5.0f      // a new shot this close to an enemy we run (feet + 1 m) is that enemy's
#define SHOT_OURS_M 2.0f     // ... but not this close to our Link (our own rod's shot)
#define SHOT_FLY_MPS 3.0f    // faster than this: flying (a rock in a hand before the throw is not a hit)
#define SHOT_BODY_R 0.5f     // the avatar's body radius, plus the shot's own
#define SHOT_IDLE_NS 6000000000ull
#define SHOT_STOP_R 0.4f     // a flight that ends < SHOT_STOP_NS after passing this much beyond R: it hit the body
#define SHOT_STOP_NS 300000000ull  // (test_shots run 13: rocks bounce off the avatar's collision body 0.75-1.2 m out)
#define SHOT_JUMP_MPS 150.0f  // faster than this: put back (a reused shot), not flying (rocks fly up to ~80 m/s)
static const struct ShotKind { const char *name; uint8_t cls, knock; float r; int16_t pw; } shot_kinds[] = {
    {"Octarock_Bullet", COOP_SHOT_ROCK, 0, 0.35f, 2},     {"Octarock_Bullet_L", COOP_SHOT_ROCK, 0, 0.5f},
    {"Octarock_SnowBullet", COOP_SHOT_ROCK, 0, 0.35f, 2}, {"OctObj_Stone_01", COOP_SHOT_ROCK, 0, 0.4f},
    {"Rock_Weapon", COOP_SHOT_ROCK, 0, 0.4f},          {"Rock_Moriblin", COOP_SHOT_ROCK, 0, 0.5f},
    {"Giant_Rock_Weapon", COOP_SHOT_ROCK, 1, 1.2f, 8},    {"Giant_Bomb_Weapon", COOP_SHOT_BOMB, 1, 1.5f, 30},
    {"Lizalfos_IceBall", COOP_SHOT_ICE, 0, 0.5f},      {"Lizalfos_ElectricWaterBall", COOP_SHOT_SHOCK, 0, 0.5f, 40},
    {"Lizalfos_Lava", COOP_SHOT_FIRE, 0, 0.5f},        {"Lizalfos_Water", COOP_SHOT_ROCK, 0, 0.5f},
    {"LizalfosFlame", COOP_SHOT_FIRE, 0, 0.6f, 24},        {"LynelFlame", COOP_SHOT_FIRE, 0, 0.8f},
    {"FireRodLv1Fire", COOP_SHOT_FIRE, 0, 0.6f},       {"FireRodLv2Fire", COOP_SHOT_FIRE, 0, 0.6f},
    {"IceRodLv1Ice", COOP_SHOT_ICE, 0, 0.6f},          {"IceRodLv2Ice", COOP_SHOT_ICE, 0, 0.6f},
    {"ThunderRodLv1Thunder", COOP_SHOT_SHOCK, 0, 0.6f}, {"ThunderRodLv2Thunder", COOP_SHOT_SHOCK, 0, 0.6f},
    {"Wizzrobe_WeatherIceBall", COOP_SHOT_ICE, 1, 1.0f},
};
#define SHOT_KINDS ((int)(sizeof shot_kinds / sizeof shot_kinds[0]))
static const char *const shot_class_names[COOP_SHOT_TYPES] = {"rock", "fire", "ice", "shock", "bomb"};
static int foe_shots_c = -1;
static bool foe_shots_on(void) { return env_on("SAVAGEWIND_COOP_FOE_SHOTS", &foe_shots_c); }
static int foe_shot_hits;  // (tests: hits told the other game since start)
static volatile uint64_t shottrace_until_ns;  // `coop shottrace <s>` (dev)
static struct FoeShot {
    uint64_t actor, unit, foe;
    uint32_t id;
    int8_t kind;
    bool flew, hit;  // (hit: this flight was already told)
    int power;
    float prev[3], vel[3], top_v;
    float app_vel[3];  // (velocity before a bounce)
    // (n5: per remote avatar, by player id) this flight's nearest pass within SHOT_STOP_R of that body axis, when,
    // and the straight path's distance there
    float near_line[COOP_MAX_PLAYERS], near_d[COOP_MAX_PLAYERS];
    uint64_t near_ns[COOP_MAX_PLAYERS];
    uint64_t last_ns, born_ns;
    char foe_name[48];
} fshots[FSHOT_MAX];
static uint32_t shot_done_ids[32];
static unsigned shot_done_n;

// (protocol 52) The model the other game shows for a flying shot (COOP_SHOTV_*; fire / ice / shock balls are effects
// with no model: not shown yet).
static int shot_view_kind(const char *nm) {
    if (!strcmp(nm, "Octarock_Bullet") || !strcmp(nm, "Octarock_Bullet_L")) return COOP_SHOTV_OCT_ROCK;
    if (!strcmp(nm, "Octarock_SnowBullet")) return COOP_SHOTV_OCT_SNOW;
    if (!strcmp(nm, "Rock_Weapon") || !strcmp(nm, "Rock_Moriblin")) return COOP_SHOTV_ROCK;
    if (!strcmp(nm, "Giant_Rock_Weapon")) return COOP_SHOTV_GIANT_ROCK;
    return COOP_SHOTV_NONE;
}
// The overlay puppet for each (build_overlay.py carried_props: the shot's own model, physics-free, moved by writing
// its matrix).
static const char *const shot_view_pup[COOP_SHOTV_KINDS] = {"", "Dm_CoopW_OctRock", "Dm_CoopW_OctRockSnow",
                                                             "Dm_CoopW_RockWeapon", "Dm_CoopW_GiantRock"};

static bool fshot_alive(const struct FoeShot *s) {
    return in_guest(s->actor) && in_guest(s->unit) && LD32(s->unit + UNIT_ID) == s->id &&
           LD64(s->unit + UNIT_ACTOR) == s->actor && LD8(s->actor + PROC_STATE) != 3;
}
// (a shot let go can be taken up again: reused shots come back. The ids seen are kept only to log each one once)
static void fshot_retire(struct FoeShot *s) { memset(s, 0, sizeof *s); }
static int shot_kind(const char *nm) {
    for (int k = 0; k < SHOT_KINDS; k++)
        if (!strcmp(nm, shot_kinds[k].name)) return k;
    return -1;
}
// The enemy we run nearest to p within SHOT_FOE_M of its body (feet + 1 m), or NULL.
static struct Enemy *shot_foe(const float p[3]) {
    struct Enemy *best = NULL;
    float bd = SHOT_FOE_M;
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (!e->actor || !e->mine || e->following || e->last <= 0) continue;
        const float *m = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
        float q[3] = {m[3], m[7] + 1.0f, m[11]}, d = dist3(p, q);
        if (d < bd) bd = d, best = e;
    }
    return best;
}
// (n5, 2026-11-17: rocks and fireballs too) The avatars a shot can hit: the partner's, and with
// MULTISHOTS every extra player's drawn here (rav: multi.inc) whose state is fresh and in our area. Each followed
// shot keeps its near-pass state per avatar (by player id), and a hit on an extra player's avatar goes to that player
// only (coop_net_send_event_to, protocol 60, as arrows). SAVAGEWIND_COOP_MULTISHOTS=0: the partner's avatar only.
static int multishots_c = -1;
static bool multishots_on(void) { return env_on("SAVAGEWIND_COOP_MULTISHOTS", &multishots_c); }
struct ShotTarget { const struct RemoteAv *ra; float lo[3], hi[3], mid[3]; };
static int shot_targets(struct ShotTarget *t) {
    int n = 0;
    char area[96];
    uint32_t here = 0;
    bool have_area = false;
    for (int i = 0; i < rav_n; i++) {
        const struct RemoteAv *ra = &rav[i];
        if (!ra->partner) {
            if (!multishots_on() || !ra->fresh || !(ra->rs.flags & COOP_FLAG_IN_WORLD)) continue;
            if (!have_area) here = local_area(area, sizeof area), have_area = true;
            if (ra->rs.area != here) continue;
        }
        if (ra->pid < 0 || ra->pid >= COOP_MAX_PLAYERS) continue;
        const float *am = (const float *)(uintptr_t)(ra->actor + ACTOR_MTX);
        struct ShotTarget *s = &t[n++];
        s->ra = ra;
        s->lo[0] = s->hi[0] = s->mid[0] = am[3], s->lo[2] = s->hi[2] = s->mid[2] = am[11];
        s->lo[1] = am[7] + PVP_BODY_LO, s->hi[1] = am[7] + PVP_BODY_HI, s->mid[1] = am[7] + 1.0f;
    }
    return n;
}
// Is an enemy we run within 50 m of an avatar? (then new shots are looked for every 2nd frame)
static bool shots_fight_near(const struct ShotTarget *t, int nt) {
    for (int i = 0; i < enemy_count; i++) {
        const struct Enemy *e = &enemies[i];
        if (!e->actor || !e->mine || e->following || e->last <= 0) continue;
        const float *m = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
        for (int k = 0; k < nt; k++) {
            float dx = m[3] - t[k].mid[0], dz = m[11] - t[k].mid[2];
            if (dx * dx + dz * dz < 50.0f * 50.0f) return true;
        }
    }
    return false;
}

// New shots (the process ring walked natively: names only, no game calls).
static void shots_scan(const float me[3], uint64_t now, bool trace) {
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t start = in_guest(pa) ? LD64(pa + PROC_UNIT) : 0;
    if (!in_guest(start)) return;
    uint64_t u = start, unit_vt = LD64(start);
    for (unsigned step = 0; step < 16384; step++) {
        uint64_t next = LD64(u + UNIT_NODE + 8);
        uint64_t a = LD64(u) == unit_vt ? LD64(u + UNIT_ACTOR) : 0;
        if (in_guest(a) && in_guest(a + ACTOR_MTX + 0x30) && LD32(u + UNIT_ID) == LD32(a + PROC_ID) &&
            LD8(a + PROC_STATE) != 3) {
            const char *nm = (const char *)(uintptr_t)(a + 0x1c);
            int k = strnlen(nm, 48) < 48 ? shot_kind(nm) : -1;
            uint32_t id = LD32(a + PROC_ID);
            if (k >= 0) {
                int free_i = -1;
                bool known = false;
                for (int i = 0; i < FSHOT_MAX; i++) {
                    if (fshots[i].actor == a && fshots[i].id == id) known = true;
                    if (free_i < 0 && (!fshots[i].actor || !fshot_alive(&fshots[i]))) free_i = i;
                }
                bool seen = false;
                for (int i = 0; i < 32; i++)
                    if (shot_done_ids[i] == id) seen = true;
                const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
                float p[3] = {m[3], m[7], m[11]};
                struct Enemy *foe = !known && free_i >= 0 && dist3(p, me) > SHOT_OURS_M ? shot_foe(p) : NULL;
                if (foe) {
                    struct FoeShot *s = &fshots[free_i];
                    memset(s, 0, sizeof *s);
                    s->actor = a, s->unit = u, s->id = id, s->kind = (int8_t)k, s->foe = foe->actor;
                    memcpy(s->prev, p, 12);
                    s->last_ns = s->born_ns = now;
                    snprintf(s->foe_name, sizeof s->foe_name, "%s", foe->name);
                    int pw = shot_kinds[k].pw > 0 ? shot_kinds[k].pw : enemy_power(foe->name);
                    s->power = pw > 0 ? pw : 4;
                    if (!seen) shot_done_ids[shot_done_n++ % 32] = id;
                    static unsigned said;
                    if (!seen && said++ < 60)
                        LOG("[coop] shots: %s's %s (%llx, power %d)\n", foe->name, nm, (unsigned long long)a, s->power);
                } else if (!known && !seen && trace) {
                    static unsigned said;
                    if (said++ < 40)
                        LOG("[coop] shots: %s at %.1f %.1f %.1f not taken (no enemy we run within %.0f m / ours)\n", nm,
                            p[0], p[1], p[2], SHOT_FOE_M);
                }
            }
        }
        if (!in_guest(next)) break;
        u = next - UNIT_NODE;
        if (u == start) break;
    }
}

static void shot_hit_send(struct FoeShot *s, const struct ShotTarget *t, const float p[3], float dist, float v, const char *how) {
    const struct ShotKind *k = &shot_kinds[s->kind];
    float dir[3] = {s->vel[0], 0, s->vel[2]}, l = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    if (l > 1e-3f) dir[0] /= l, dir[2] /= l;
    else dir[0] = 0, dir[2] = 1;
    int power = s->power > 400 ? 400 : s->power < 0 ? 0 : s->power;
    CoopAreaFx e = {COOP_AREAFX_FOE_SHOT, (uint8_t)(k->cls | (k->knock ? COOP_SHOT_KNOCK : 0)), (int16_t)power,
                    {p[0], p[1], p[2]}, {dir[0], 0, dir[2]}, 0};
    bool extra = !t->ra->partner;  // (n5: an extra player's avatar: to that player only)
    bool ok = coop_areafx_ok(&e) && (extra ? coop_net_send_event_to(t->ra->pid, COOP_EV_AREAFX, &e, sizeof e)
                                           : coop_net_send_event(COOP_EV_AREAFX, &e, sizeof e));
    if (ok) foe_shot_hits++;
    LOG("[coop] shots: %s's %s hit %s %d (%s, %.2f m from the body axis, %.1f m/s, power %d): %s\n", s->foe_name,
        k->name, extra ? "extra player" : "the other player", t->ra->pid, how, dist, v, power, ok ? "told them" : "NOT sent");
    s->hit = true;
}

static void shots_frame(Cpu *c) {
    (void)c;
    if (!foe_shots_on()) return;
    uint64_t now = plat_now_ns();
    struct ShotTarget tg[COOP_MAX_PLAYERS];
    int nt = local_in_world() && player_info_ptr() ? shot_targets(tg) : 0;
    if (!nt) {
        memset(fshots, 0, sizeof fshots);
        coop_net_set_shot_views(NULL, 0);
        return;
    }
    bool trace = shottrace_until_ns && now < shottrace_until_ns;
    float me[3];
    drop_me(me);
    me[1] += 1.0f;
    static unsigned frame;
    frame++;
    if (frame % 10 == 0 || (frame % 2 == 0 && shots_fight_near(tg, nt))) shots_scan(me, now, trace);
    for (int i = 0; i < FSHOT_MAX; i++) {
        struct FoeShot *s = &fshots[i];
        if (!s->actor) continue;
        const struct ShotKind *k = &shot_kinds[s->kind];
        float R = SHOT_BODY_R + k->r;
        bool alive = fshot_alive(s);
        float p[3], v = 0, prev[3];
        memcpy(prev, s->prev, 12);
        if (alive) {
            const float *m = (const float *)(uintptr_t)(s->actor + ACTOR_MTX);
            p[0] = m[3], p[1] = m[7], p[2] = m[11];
            double dt = now > s->last_ns ? (now - s->last_ns) / 1e9 : 0;
            if (dt < 0.001) continue;  // (the same game frame twice)
            s->last_ns = now;
            if (dt < 0.3) v = (float)(dist3(p, prev) / dt);
        }
        float pv = sqrtf(s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] + s->vel[2] * s->vel[2]);
        // (2026-11-15 test: an Octorok's rocks are kept and reused, waiting inside it between shots, state 2: the same
        // actor flies again and again. After a hit it is put back at once (a jump far faster than a rock) or stops)
        bool jump = alive && v > SHOT_JUMP_MPS;
        bool ended = s->flew && (!alive || jump || v < 0.5f);
        bool near = false;
        for (int t = 0; t < nt && !near; t++) near = dist3(alive ? p : prev, tg[t].mid) < 40.0f;
        if (ended) {
            // (the flight ended: it burst on an avatar's collision body, the ground, or was put back. One more frame
            // along its last velocity from where it was last seen: through a body = a hit. n5: the avatar it came
            // nearest to of those it qualifies for)
            float ahead[3] = {prev[0] + s->vel[0] * 0.08f, prev[1] + s->vel[1] * 0.08f, prev[2] + s->vel[2] * 0.08f};
            int best = -1;
            float bd = 1e9f, bhow = 0;
            for (int t = 0; t < nt; t++) {
                int pid = tg[t].ra->pid;
                float d = seg_seg(prev, ahead, tg[t].lo, tg[t].hi);
                if (trace && near)
                    LOG("[coop] shottrace %s %llx flight ended (%s), %.2f m from player %d's body axis\n", k->name,
                        (unsigned long long)s->actor, !alive ? "gone" : jump ? "put back" : "stopped", d, pid);
                bool stopped_at = s->near_ns[pid] && now - s->near_ns[pid] < SHOT_STOP_NS && s->near_d[pid] < R + SHOT_STOP_R &&
                                  seg_seg(prev, prev, tg[t].lo, tg[t].hi) < R + SHOT_STOP_R + 0.6f &&  // (it stopped right there)
                                  s->near_line[pid] < R;  // (and was coming straight at the body)
                if ((d < R + 0.3f || stopped_at) && d < bd) bd = d, best = t, bhow = d < R + 0.3f ? d : s->near_d[pid];
            }
            if (!s->hit && best >= 0) shot_hit_send(s, &tg[best], prev, bhow, pv, "burst at the body");
            s->flew = s->hit = false, s->top_v = 0;
            memset(s->near_ns, 0, sizeof s->near_ns);
        }
        if (!alive) { fshot_retire(s); continue; }
        memcpy(s->prev, p, 12);
        if (jump) { memset(s->vel, 0, sizeof s->vel); continue; }
        float dt = v > 0 ? dist3(p, prev) / v : 0;
        if (dt > 0) for (int j = 0; j < 3; j++) s->vel[j] = (p[j] - prev[j]) / dt;
        else memset(s->vel, 0, sizeof s->vel);
        if (v > SHOT_FLY_MPS) s->flew = true, s->born_ns = now;
        if (v > s->top_v) s->top_v = v;
        if (v > SHOT_FLY_MPS && v >= 0.6f * s->top_v) memcpy(s->app_vel, s->vel, 12);  // (before any bounce)
        int through = -1;
        float through_d = 1e9f;
        for (int t = 0; t < nt; t++) {
            int pid = tg[t].ra->pid;
            float d = seg_seg(prev, p, tg[t].lo, tg[t].hi);
            if (trace && near && (s->flew || ended))
                LOG("[coop] shottrace %s %llx: at %.2f %.2f %.2f, %.1f m/s, path %.2f m from player %d's body axis\n",
                    k->name, (unsigned long long)s->actor, p[0], p[1], p[2], v, d, pid);
            if (s->flew && !s->hit && v > SHOT_FLY_MPS && d < R && d < through_d) through = t, through_d = d;
            if (s->flew && d < R + SHOT_STOP_R && (!s->near_ns[pid] || d < s->near_d[pid])) {
                s->near_d[pid] = d;
                // (test_shots run 16: the avatar's collision body stops rocks ~1 m from its axis, wider than a Link: a
                // stop there is a hit only when the straight path it came on would have gone through a Link's body)
                float a0[3] = {prev[0], prev[1], prev[2]};
                float a1[3] = {prev[0] + s->app_vel[0] * 0.15f, prev[1] + s->app_vel[1] * 0.15f, prev[2] + s->app_vel[2] * 0.15f};
                s->near_line[pid] = seg_seg(a0, a1, tg[t].lo, tg[t].hi);
            }
            if (s->flew && d < R + SHOT_STOP_R) s->near_ns[pid] = now;
        }
        if (through >= 0) shot_hit_send(s, &tg[through], p, through_d, v, "through the body");
        if (!s->flew && !near && now - s->born_ns > SHOT_IDLE_NS) fshot_retire(s);  // (far away and resting: let go)
    }
    // (protocol 52) the shots flying now go to the other game, which shows them (shots_view)
    CoopShotView sv[COOP_SHOT_VIEWS];
    int n = 0;
    for (int i = 0; i < FSHOT_MAX && n < COOP_SHOT_VIEWS; i++) {
        struct FoeShot *s = &fshots[i];
        int vk = s->actor && s->flew ? shot_view_kind(shot_kinds[s->kind].name) : COOP_SHOTV_NONE;
        if (vk == COOP_SHOTV_NONE) continue;
        sv[n] = (CoopShotView){.kind = (uint8_t)vk};
        memcpy(sv[n].pos, s->prev, 12), memcpy(sv[n].vel, s->vel, 12);
        n++;
    }
    coop_net_set_shot_views(sv, n);
}

// (protocol 52: show the rock) The other game's enemy shots flying, shown here: our copy of the enemy shoots
// nothing (its AI is off), so without this the partner sees the Octorok spit and then gets hurt by nothing. Each slot
// is a puppet with the shot's model, put where the newest sample (20 a second) says, moved on by its velocity for the
// sample's age. A puppet not needed is parked SHOT_PARK_DROP m under our Link and kept SHOT_KEEP_NS (the next rock
// of the burst then needs no creation), then deleted. SAVAGEWIND_COOP_SHOTVIEW=0: not shown.
#define SHOT_PARK_DROP 40.0f
#define SHOT_KEEP_NS 20000000000ull
#define SHOT_LEAD_MAX_S 0.15f
// (n5) Every other player's shots (each player's state packet carries its own; the host relays them): puppet slots
// per player id. With SAVAGEWIND_COOP_MULTISHOTS=0 only the partner's are shown (as before).
static struct ShotPup { struct Pup p; uint64_t used_ns; bool shown; } shot_pup[COOP_MAX_PLAYERS][COOP_SHOT_VIEWS];
static int shot_view_c = -1;
static void shots_view(Cpu *c) {
    uint64_t now = plat_now_ns();
    float me[3] = {0, 0, 0};
    if (player_info_ptr()) memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    static int have[COOP_SHOTV_KINDS];  // (0 unknown, 1 in the overlay, -1 not: an old overlay)
    int my_id = coop_net_my_id(), partner = coop_net_partner_id();
    uint32_t heard = coop_net_players(5.0);
    for (int pl = 0; pl < COOP_MAX_PLAYERS; pl++) {
    CoopState rs;
    CoopShotView sv[COOP_SHOT_VIEWS];
    double age = 99, sage = 99;
    char area[96];
    bool who = pl != my_id && (heard >> pl & 1) && (pl == partner || multishots_on());
    if (!who) {
        bool any = false;
        for (int i = 0; i < COOP_SHOT_VIEWS; i++) any |= shot_pup[pl][i].p.actor != 0;
        if (!any) continue;  // (nothing of that player's to park or drop)
    }
    coop_net_view(who ? pl : COOP_VIEW_NONE);
    bool show = who && env_on("SAVAGEWIND_COOP_SHOTVIEW", &shot_view_c) && local_in_world() && player_info_ptr() &&
                coop_net_get_remote(&rs, &age) && age < 1.0 && (rs.flags & COOP_FLAG_IN_WORLD) &&
                rs.area == local_area(area, sizeof area) && coop_net_remote_shot_views(sv, &sage) && sage < 0.5;
    coop_net_view(-1);
    for (int i = 0; i < COOP_SHOT_VIEWS; i++) {
        struct ShotPup *sp = &shot_pup[pl][i];
        if (sp->p.actor && (!local_in_world() || !pup_alive(&sp->p))) sp->p.actor = 0, sp->p.name[0] = 0;
        const CoopShotView *v = show && sv[i].kind != COOP_SHOTV_NONE && dist3(sv[i].pos, me) < 150.0f ? &sv[i] : NULL;
        const char *name = v ? shot_view_pup[v->kind] : "";
        if (v && !have[v->kind]) {
            have[v->kind] = pack_exists(name) ? 1 : -1;
            if (have[v->kind] < 0) LOG("[coop] shots: no %s in the overlay (old overlay): their shots are not shown\n", name);
        }
        if (v && have[v->kind] < 0) v = NULL, name = "";
        if (!v) {  // (park it, or let it go after a while)
            if (!sp->p.actor) continue;
            if (!local_in_world() || now - sp->used_ns > SHOT_KEEP_NS) { pup_drop(c, &sp->p); sp->shown = false; continue; }
            static int hold = -1;  // (tests: SAVAGEWIND_COOP_SHOTVIEW_HOLD=<s> leaves it that long where it was last shown, for a screenshot)
            if (hold < 0) hold = getenv("SAVAGEWIND_COOP_SHOTVIEW_HOLD") ? atoi(getenv("SAVAGEWIND_COOP_SHOTVIEW_HOLD")) : 0;
            if (hold > 0 && now - sp->used_ns < (uint64_t)hold * 1000000000ull) continue;
            if (sp->shown) LOG("[coop] shots: their %s is gone (parked)\n", sp->p.name);
            sp->shown = false;
            float m[12] = {1, 0, 0, me[0], 0, 1, 0, me[1] - SHOT_PARK_DROP, 0, 0, 1, me[2]};
            memcpy((void *)(uintptr_t)(sp->p.actor + ACTOR_MTX), m, sizeof m);
            memcpy((void *)(uintptr_t)(sp->p.actor + ACTOR_MTX2), m, sizeof m);
            continue;
        }
        float lead = (float)sage + 1.0f / 60;
        if (lead > SHOT_LEAD_MAX_S) lead = SHOT_LEAD_MAX_S;
        float at[3];
        for (int k = 0; k < 3; k++) at[k] = v->pos[k] + v->vel[k] * lead;
        if (sp->p.actor && strcmp(sp->p.name, name)) pup_drop(c, &sp->p);
        if (!sp->p.actor) {
            if (now < sp->p.retry_ns || spawn_hold()) continue;
            sp->p.retry_ns = now + 2000000000ull;
            sp->p.actor = spawn_now(c, name, at);
            if (!sp->p.actor) {
                if (!sp->p.said) sp->p.said = true, LOG("[coop] shots: could not create %s\n", name);
                continue;
            }
            sp->p.unit = LD64(sp->p.actor + PROC_UNIT), sp->p.id = LD32(sp->p.actor + PROC_ID);
            snprintf(sp->p.name, sizeof sp->p.name, "%s", name);
            LOG("[coop] shots: made %s (%llx) for their shot\n", name, (unsigned long long)sp->p.actor);
        }
        if (!sp->shown)
            LOG("[coop] shots: their %s shown at %.1f %.1f %.1f, %.0f m/s (sample %.0f ms old, player %d)\n", name, at[0],
                at[1], at[2], sqrtf(v->vel[0] * v->vel[0] + v->vel[1] * v->vel[1] + v->vel[2] * v->vel[2]), sage * 1000, pl);
        sp->shown = true, sp->used_ns = now;
        float m[12] = {1, 0, 0, at[0], 0, 1, 0, at[1], 0, 0, 1, at[2]};
        memcpy((void *)(uintptr_t)(sp->p.actor + ACTOR_MTX), m, sizeof m);
        memcpy((void *)(uintptr_t)(sp->p.actor + ACTOR_MTX2), m, sizeof m);
    }
    }
}

// The other game's enemy shot hit our Link there (game thread, from pvp_areafx_ev).
static void shots_ev(Cpu *c, const CoopAreaFx *e) {
    uint64_t pa = player_info_ptr() ? LD64(player_info_ptr() + 0x60) : 0;
    char as[64] = "", area[96];
    float me[3];
    drop_me(me);
    float body[3] = {me[0], me[1] + 0.9f, me[2]}, dist = dist3(body, e->pos);
    CoopState rs;
    double age = 99;
    bool same_area = coop_net_get_remote(&rs, &age) && rs.area == local_area(area, sizeof area);
    int cls = e->flags & COOP_SHOT_CLASS;
    bool knock = (e->flags & COOP_SHOT_KNOCK) != 0;
    const char *what = cls < COOP_SHOT_TYPES ? shot_class_names[cls] : "shot";
    bool ok = foe_shots_on() && same_area && dist < 3.0f && pvp_victim_ok(c, pa, as, sizeof as);
    LOG("[coop] shots: an enemy's %s shot hit us there (%.1f m from us here): %s\n", what, dist,
        ok ? "hit" : !foe_shots_on() ? "enemy shots off" : "out of reach / can't be hit now");
    if (!ok) return;
    float from[3] = {e->pos[0] - e->dir[0] * 2, e->pos[1], e->pos[2] - e->dir[2] * 2};
    if (pvp_guarded(c, pa, from)) {
        LOG("[coop] shots: blocked the %s shot with the shield (%s)\n", what, as);
        guard_fx(c, pa, from);
        return;
    }
    int life = (int)LD32(pa + PVP_LIFE), level = knock ? 2 : 1;
    float dir[3] = {e->dir[0], 0, e->dir[2]};
    int got = player_hit_ex(pa, e->damage, level, level == 2 ? 0x15 : 0x11, level == 2 ? 0x10 : 0x14, dir, true);
    LOG("[coop] shots: %s shot: -%d life (%d -> %d), %s\n", what, got, life, life - got,
        level == 2 ? "knocked down" : "medium reaction");
    // (FOE_ELEM, elemarrow.inc: the class order matches the arrows')
    if (got < life && cls >= COOP_SHOT_FIRE && cls <= COOP_SHOT_SHOCK)
        foe_elem(c, pa, COOP_ARROW_FIRE + (unsigned)(cls - COOP_SHOT_FIRE));
}

// `coop shotprobe <s>` (dev, RE 2026-11-15): every frame, every shot-kind actor within 40 m of our Link: its matrix
// position, process state and both candidate physics bodies' positions (no distance check), to see what moves when an
// Octorok's rock flies.
static volatile uint64_t shotprobe_until_ns;
static void shotprobe_frame(void) {
    uint64_t now = plat_now_ns();
    if (!shotprobe_until_ns || now > shotprobe_until_ns || !local_in_world() || !player_info_ptr()) return;
    float me[3];
    drop_me(me);
    uint64_t pa = LD64(player_info_ptr() + 0x60), start = in_guest(pa) ? LD64(pa + PROC_UNIT) : 0;
    if (!in_guest(start)) return;
    static int life_was = -1;
    int life = (int)LD32(pa + PVP_LIFE);
    if (life != life_was) LOG("[coop] shotprobe life %d -> %d\n", life_was, life), life_was = life;
    uint64_t u = start, unit_vt = LD64(start);
    for (unsigned step = 0; step < 16384; step++) {
        uint64_t next = LD64(u + UNIT_NODE + 8);
        uint64_t a = LD64(u) == unit_vt ? LD64(u + UNIT_ACTOR) : 0;
        if (in_guest(a) && in_guest(a + ACTOR_MTX + 0x30)) {
            const char *nm = (const char *)(uintptr_t)(a + 0x1c);
            static uint64_t seen_a[8192];
            static unsigned seen_n;
            bool any_new = true;
            for (unsigned i = 0; i < seen_n; i++)
                if (seen_a[i] == a) any_new = false;
            if (any_new && seen_n < 8192 && strnlen(nm, 48) < 48) {  // (every actor new within 40 m, once: what appears when it shoots)
                const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
                float p[3] = {m[3], m[7], m[11]};
                if (dist3(p, me) < 40.0f) seen_a[seen_n++] = a;  // (once it is near: spawned actors start elsewhere)
                if (dist3(p, me) < 40.0f)
                    LOG("[coop] shotprobe new %s %llx st %d at %.2f %.2f %.2f (%.1f m from us)\n", nm,
                        (unsigned long long)a, LD8(a + PROC_STATE), p[0], p[1], p[2], dist3(p, me));
            }
            if (strnlen(nm, 48) < 48 && (shot_kind(nm) >= 0 || strstr(nm, "Bullet"))) {
                const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
                float p[3] = {m[3], m[7], m[11]};
                static float last[8][3];
                static uint64_t last_a[8];
                int li = -1;
                for (int i = 0; i < 8; i++)
                    if (last_a[i] == a || (li < 0 && !last_a[i])) li = i;
                bool moved = li < 0 || last_a[li] != a || dist3(last[li], p) > 0.01f;
                if (li >= 0) last_a[li] = a, memcpy(last[li], p, 12);
                if (dist3(p, me) < 40.0f && moved) {
                    uint64_t p1 = LD64(a + 0x578), q1 = in_guest(p1) ? LD64(p1 + 0x80) : 0,
                             b1 = in_guest(q1) ? LD64(q1 + 0x410) : 0, p2 = LD64(a + 0x198),
                             b2 = in_guest(p2) ? LD64(p2 + 0x920) : 0;
                    float z[3] = {0, 0, 0};
                    const float *bp1 = in_guest(b1) && LD64(b1) == RIGID_BODY_VT ? (const float *)(uintptr_t)(b1 + 0x1a0) : z;
                    const float *bp2 = in_guest(b2) && LD64(b2) == RIGID_BODY_VT ? (const float *)(uintptr_t)(b2 + 0x1a0) : z;
                    LOG("[coop] shotprobe %s %llx st %d id %x: mtx %.2f %.2f %.2f | body1 %.2f %.2f %.2f | body2 %.2f %.2f "
                        "%.2f\n", nm, (unsigned long long)a, LD8(a + PROC_STATE), LD32(a + PROC_ID), p[0], p[1], p[2],
                        bp1[0], bp1[1], bp1[2], bp2[0], bp2[1], bp2[2]);
                }
            }
        }
        if (!in_guest(next)) break;
        u = next - UNIT_NODE;
        if (u == start) break;
    }
}

// (dev, RE 2026-11-15, part of `coop shotprobe`) In the frame our Link is hurt (its attack receiver's hit flag, as
// hitwatch), every 8-byte value in the receiver (0x200 B) and one pointer level below it (0x100 B each) that points
// at an actor (a printable name at +0x1c and a finite matrix) is logged with the actor's name, process state and
// position: which actor hurt us (an Octorok's rock?).
static bool probe_is_actor(uint64_t a, char *nm, size_t cap) {
    if (!in_guest(a) || (a & 7) || !in_guest(a + ACTOR_MTX + 0x30)) return false;
    const char *s = (const char *)(uintptr_t)(a + 0x1c);
    size_t n = 0;
    for (; n < cap - 1 && s[n]; n++)
        if (s[n] < 0x21 || s[n] > 0x7e) return false;
    if (n < 4 || s[n] || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return false;
    memcpy(nm, s, n + 1);
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    for (int i = 0; i < 12; i++)
        if (!isfinite(m[i]) || fabsf(m[i]) > 100000) return false;
    return true;
}
static void hitwho_frame(uint64_t pa) {
    if (!shotprobe_until_ns || plat_now_ns() > shotprobe_until_ns || !in_guest(pa)) return;
    uint64_t r = LD64(pa + PVP_RECEIVER);
    static bool was;
    bool hit = in_guest(r) && LD32(r + 0x44);
    bool rise = hit && !was;
    was = hit;
    if (!rise) return;
    const float *pm = (const float *)(uintptr_t)(pa + ACTOR_MTX);
    LOG("[coop] hitwho: hurt (life %u) at %.2f %.2f %.2f; receiver %llx\n", LD32(pa + PVP_LIFE), pm[3], pm[7], pm[11],
        (unsigned long long)r);
    int lines = 0;
    for (uint32_t o = 0; o < 0x200 && lines < 40; o += 8) {
        uint64_t v = LD64(r + o);
        char nm[48];
        if (probe_is_actor(v, nm, sizeof nm)) {
            const float *m = (const float *)(uintptr_t)(v + ACTOR_MTX);
            LOG("[coop] hitwho: receiver+0x%x -> %s %llx st %d at %.2f %.2f %.2f\n", o, nm, (unsigned long long)v,
                LD8(v + PROC_STATE), m[3], m[7], m[11]);
            lines++;
            continue;
        }
        if (!in_guest(v) || (v & 7)) continue;
        for (uint32_t q = 0; q < 0x100 && lines < 40; q += 8) {
            uint64_t w = in_guest(v + q + 8) ? LD64(v + q) : 0;
            if (w != pa && probe_is_actor(w, nm, sizeof nm)) {
                const float *m = (const float *)(uintptr_t)(w + ACTOR_MTX);
                LOG("[coop] hitwho: receiver+0x%x -> +0x%x -> %s %llx st %d at %.2f %.2f %.2f\n", o, q, nm,
                    (unsigned long long)w, LD8(w + PROC_STATE), m[3], m[7], m[11]);
                lines++;
            }
        }
    }
}
