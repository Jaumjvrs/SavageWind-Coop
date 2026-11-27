// Part of runtime/coop.c (included there, same translation unit): our arrows hitting the other player (protocol 35).
//
// Facts (2026-11-12 probe, `coop arrowtrace`, ): a shot arrow IS an actor named
// after its type (NormalArrow, FireArrow, IceArrow, ElectricArrow, BombArrow_A, AncientArrow; the pouch item has the
// same name). It is created in Link's hand when the bow is drawn (state 1, ~0.3-1 m from Link), stays there while he
// aims, then flies ~2.6 m per frame and stays where it stuck. (The protocol 31 note "flying arrows are not actors" came
// from a test kit whose arrows were named Obj_ArrowNormal: no arrows in the pouch, nothing was ever shot.)
//
// Like a thrown weapon (drops.inc), the SHOOTER's game decides: our arrow is followed every frame; when its path from
// the last frame to this one passes through the other player's avatar (the same capsule as melee: feet + 0.25..1.5 m,
// radius ARROW_BODY_R), or it stops dead right at the avatar (stuck in / bounced off its collision body), a reliable
// COOP_EV_AREAFX kind 1 tells the other game, whose player takes the hit by the friendly damage rules (pvp.inc: the
// bow's power, at most PVP_DMG_MAX, never lethal, a shield held toward the shot blocks it, flinch along the flight).
// Bomb arrows knock down. Fire, ice and shock use native player status (SAVAGEWIND_COOP_ELEMARROW=0: ordinary hit).
// Ancient arrows use ordinary damage. (0.1.1) An enemy's elemental arrow: its hit plus the status (FOE_ELEM).
// SAVAGEWIND_COOP_ARROWS=0 turns it off (as does SAVAGEWIND_COOP_PVP=0); SAVAGEWIND_COOP_ARROWTRACE=1 logs every
// frame of every followed arrow.
#define ARROW_MAX 16
#define ARROW_BODY_R 0.6f     // the arrow's path this close to the avatar's body axis = a hit
#define ARROW_STOP_R 1.0f     // an arrow that stopped dead this close to the axis (stuck in / off its body) = a hit
#define ARROW_OURS_M 2.5f     // a new arrow this close to our Link (on the bow string) is ours
#define ARROW_FLY_MPS 15.0f   // faster than this (and away from Link): flying
#define ARROW_LEN 1.2f        // the tip this far ahead of the arrow's origin
#define ARROW_FORGET_NS 3000000000ull
static const char *const arrow_names[COOP_ARROW_TYPES] = {"NormalArrow", "BombArrow_A", "AncientArrow",
                                                          "FireArrow",   "IceArrow",    "ElectricArrow"};
static bool arrows_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_ARROWS") && !strcmp(getenv("SAVAGEWIND_COOP_ARROWS"), "0"));
    return on;
}
static struct Arrow {
    uint64_t actor, unit;
    uint32_t id;
    uint8_t type;
    bool flew, done;
    bool fly_sent, fly_stopped;  // (protocol 74: COOP_EV_ARROW_FLY SHOT / STOP sent)
    uint32_t fly_area;
    int power;
    float prev[3], top_v, fly_dir[3], dir3[3];
    uint64_t last_ns, still_since_ns;
    char bow[32];
    uint64_t foe;          // (protocol 50) an enemy's arrow: the enemy (that we run) that shot it; 0: ours
    float src[3];          // (foe) where its shooter was last seen (+1 m)
    char foe_name[48];
} arrows[ARROW_MAX];
// (protocol 74, 2026-11-19) Partner's arrows visible in flight: when our arrow leaves the bow, COOP_EV_ARROW_FLY
// sends start pos, velocity, gravity and arrow kind; when it stops, hits or vanishes, a matching STOP event retires
// it. Receivers fly a physics-free Dm_CoopW_*Arrow puppet along the ballistic arc (never deals damage or doubles the
// hit). SAVAGEWIND_COOP_ARROWFLY=0 turns it off.
static int arrow_fly_c = -1;
static bool arrow_fly_on(void) { return env_on("SAVAGEWIND_COOP_ARROWFLY", &arrow_fly_c); }
static float arrow_bow_gravity(const char *bow) {
    if (!strcmp(bow, "Weapon_Bow_014")) return -7.0f;  // Phrenic Bow
    if (!strcmp(bow, "Weapon_Bow_023")) return -2.8f;  // Ancient Bow
    if (!strcmp(bow, "Weapon_Bow_071") || !strcmp(bow, "Weapon_Bow_072")) return -5.0f;  // Bow of Light / Twilight Bow
    return -9.8f;
}
static void arrow_fly_send_shot(struct Arrow *r, const float pos[3], const float vel_in[3]) {
    if (!arrow_fly_on() || r->foe || r->fly_sent || !r->id) return;
    char abuf[96];
    uint32_t area = local_area(abuf, sizeof abuf);
    if (!area) return;
    float vel[3] = {vel_in[0], vel_in[1], vel_in[2]};
    float spd = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    if (!isfinite(spd) || spd <= 1.0f) return;
    if (spd > 250.0f) {
        float sc = 250.0f / spd;
        vel[0] *= sc, vel[1] *= sc, vel[2] *= sc, spd = 250.0f;
    }
    CoopArrowFly e = {r->id, area, r->type, COOP_ARROW_FLY_SHOT, {0, 0},
                      {pos[0], pos[1], pos[2]}, {vel[0], vel[1], vel[2]}, arrow_bow_gravity(r->bow)};
    bool ok = coop_arrow_fly_ok(&e) && coop_net_send_event(COOP_EV_ARROW_FLY, &e, sizeof e);
    r->fly_sent = true, r->fly_area = area;
    static unsigned said;
    if (said++ < 60)
        LOG("[coop] arrows: fly shot %s (%x) at %.1f %.1f %.1f, %.0f m/s (g %.1f): %s\n", arrow_names[r->type], r->id,
            pos[0], pos[1], pos[2], spd, e.gravity, ok ? "sent" : "NOT sent");
}
static void arrow_fly_send_stop(struct Arrow *r, const float pos[3]) {
    if (!r->fly_sent || r->fly_stopped || !r->id || !r->fly_area) return;
    r->fly_stopped = true;
    CoopArrowFly e = {r->id, r->fly_area, r->type, COOP_ARROW_FLY_STOP, {0, 0},
                      {pos[0], pos[1], pos[2]}, {0, 0, 0}, 0.0f};
    if (coop_arrow_fly_ok(&e)) coop_net_send_event(COOP_EV_ARROW_FLY, &e, sizeof e);
}
// ---- enemy arrows (2026-11-15, protocol 50, hits on the partner). In the game that runs an enemy, the
// other player is the avatar; an enemy that aims at it (enemies.inc, threat score) shoots arrows that exist only in
// that game. An arrow that appears within ARROW_FOE_M of an enemy we run (and not near our Link with our bow out) is
// followed like ours; when it passes through / stops at the avatar, COOP_EV_AREAFX kind 4 tells the other game, whose
// Link takes the hit by the game's rules there: a shield held toward it blocks it, the damage is the enemy bow's power
// (the nearest Weapon_Bow_ actor to the arrow when it was found; 4 = one heart without one), and it can take the last
// heart (an enemy's hit, not friendly fire). The following copy of the enemy in the other game shoots nothing (its AI
// is off; only melee attacks come from its played AS). SAVAGEWIND_COOP_FOE_ARROWS=0 turns it off.
#define ARROW_FOE_M 3.0f
#define ARROW_FOE_DMG 4
#define ARROW_FOE_IDLE_NS 6000000000ull  // an enemy's arrow that has not flown after this long is let go
static int foe_arrows_c = -1;
static bool foe_arrows_on(void) { return env_on("SAVAGEWIND_COOP_FOE_ARROWS", &foe_arrows_c); }
static int foe_arrow_hits;  // (tests)
static int arrows_hits;  // (tests: hits told the other game since start)

static int arrow_type(const char *name) {
    for (int t = 0; t < COOP_ARROW_TYPES; t++)
        if (!strcmp(name, arrow_names[t])) return t;
    return -1;
}
static bool arrow_alive(const struct Arrow *a) {
    return in_guest(a->actor) && in_guest(a->unit) && LD32(a->unit + UNIT_ID) == a->id &&
           LD64(a->unit + UNIT_ACTOR) == a->actor && LD8(a->actor + PROC_STATE) != 3;
}
// Arrows finished with (hit told, or lying still): their slot is freed and their process id remembered, so a stuck
// arrow next to us is not taken up again as a new one.
#define ARROW_DONE_IDS 64
static uint32_t arrow_done_ids[ARROW_DONE_IDS];
static unsigned arrow_done_n;
static bool arrow_was_done(uint32_t id) {
    for (int i = 0; i < ARROW_DONE_IDS; i++)
        if (arrow_done_ids[i] == id) return true;
    return false;
}
static void arrow_retire(struct Arrow *r) {
    arrow_done_ids[arrow_done_n++ % ARROW_DONE_IDS] = r->id;
    memset(r, 0, sizeof *r);
}

// New arrows near our Link (the process ring walked natively: names only, no game calls).
// The enemy we run nearest to p within ARROW_FOE_M of its body (feet + 1 m), or NULL.
static struct Enemy *arrow_foe(const float p[3]) {
    struct Enemy *best = NULL;
    float bd = ARROW_FOE_M;
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (!e->actor || !e->mine || e->following || e->last <= 0) continue;
        const float *m = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
        float q[3] = {m[3], m[7] + 1.0f, m[11]}, d = dist3(p, q);
        if (d < bd) bd = d, best = e;
    }
    return best;
}
static void arrows_scan(const float me[3], uint64_t now, bool ours_ok, bool foes_ok) {
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t start = in_guest(pa) ? LD64(pa + PROC_UNIT) : 0;
    if (!in_guest(start)) return;
    struct { float p[3]; const char *name; } bows[16];  // (foe arrows: the bow nearest to it gives the power)
    int nbows = 0, new_foe[ARROW_MAX], nnew = 0;
    uint64_t u = start, unit_vt = LD64(start);
    for (unsigned step = 0; step < 16384; step++) {
        uint64_t next = LD64(u + UNIT_NODE + 8);
        uint64_t a = LD64(u) == unit_vt ? LD64(u + UNIT_ACTOR) : 0;
        if (in_guest(a) && in_guest(a + ACTOR_MTX + 0x30) && LD32(u + UNIT_ID) == LD32(a + PROC_ID) &&
            LD8(a + PROC_STATE) != 3) {
            const char *nm = (const char *)(uintptr_t)(a + 0x1c);
            if (foes_ok && nbows < 16 && !strncmp(nm, "Weapon_Bow_", 11) && strnlen(nm, 32) < 32) {
                const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
                bows[nbows].p[0] = m[3], bows[nbows].p[1] = m[7], bows[nbows].p[2] = m[11], bows[nbows++].name = nm;
            }
            // (cheap pre-filter: every arrow type's name has "Arrow" at 4..8)
            if (strstr(nm, "Arrow") && strnlen(nm, 32) < 32) {
                int t = arrow_type(nm), free_i = -1;
                bool known = false;
                for (int i = 0; i < ARROW_MAX && t >= 0; i++) {
                    if (arrows[i].actor == a && arrows[i].id == LD32(a + PROC_ID)) known = true;
                    if (free_i < 0 && (!arrows[i].actor || !arrow_alive(&arrows[i]))) free_i = i;
                }
                const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
                float p[3] = {m[3], m[7], m[11]};
                bool fresh = t >= 0 && !known && free_i >= 0 && !arrow_was_done(LD32(a + PROC_ID));
                bool ours = fresh && ours_ok && dist3(p, me) < ARROW_OURS_M;
                struct Enemy *foe = fresh && !ours && foes_ok ? arrow_foe(p) : NULL;
                if (foe) {
                    struct Arrow *r = &arrows[free_i];
                    memset(r, 0, sizeof *r);
                    r->actor = a, r->unit = u, r->id = LD32(a + PROC_ID), r->type = (uint8_t)t;
                    memcpy(r->prev, p, 12);
                    r->last_ns = r->still_since_ns = now;  // (still_since: when it was found, for ARROW_FOE_IDLE_NS)
                    r->foe = foe->actor;
                    snprintf(r->foe_name, sizeof r->foe_name, "%s", foe->name);
                    const float *fm = (const float *)(uintptr_t)(foe->actor + ACTOR_MTX);
                    r->src[0] = fm[3], r->src[1] = fm[7] + 1.0f, r->src[2] = fm[11];
                    if (nnew < ARROW_MAX) new_foe[nnew++] = free_i;
                } else if (ours) {
                    struct Arrow *r = &arrows[free_i];
                    memset(r, 0, sizeof *r);
                    r->actor = a, r->unit = u, r->id = LD32(a + PROC_ID), r->type = (uint8_t)t;
                    memcpy(r->prev, p, 12);
                    r->last_ns = now;
                    const struct Held *b = &held[COOP_GEAR_BOW];
                    snprintf(r->bow, sizeof r->bow, "%s", !strncmp(b->name, "Weapon_Bow_", 11) ? b->name : "");
                    r->power = r->bow[0] ? weapon_power(r->bow) : 5;
                    static unsigned said;
                    if (said++ < 60)
                        LOG("[coop] arrows: our %s on the string (%llx, bow %s, power %d)\n", nm, (unsigned long long)a,
                            r->bow[0] ? r->bow : "?", r->power);
                }
            }
        }
        if (!in_guest(next)) break;
        u = next - UNIT_NODE;
        if (u == start) break;
    }
    for (int k = 0; k < nnew; k++) {  // (foe arrows found now: the power of the bow nearest to them)
        struct Arrow *r = &arrows[new_foe[k]];
        int bi = -1;
        float bd = 2.5f;
        for (int b = 0; b < nbows; b++)
            if (dist3(bows[b].p, r->prev) < bd) bd = dist3(bows[b].p, r->prev), bi = b;
        if (bi >= 0) snprintf(r->bow, sizeof r->bow, "%s", bows[bi].name);
        r->power = bi >= 0 ? weapon_power(r->bow) : ARROW_FOE_DMG;
        static unsigned said;
        if (said++ < 60)
            LOG("[coop] arrows: %s's %s on the string (%llx, bow %s, power %d)\n", r->foe_name, arrow_names[r->type],
                (unsigned long long)r->actor, r->bow[0] ? r->bow : "?", r->power);
    }
}

// (2026-11-14: the arrow did not show) Our arrow sitting on the string (not shot yet), or 0: sent as the
// carried prop on the right hand (avatar.inc), the other game shows Dm_CoopW_<NormalArrow...> there.
static uint64_t arrow_on_string(void) {
    for (int i = 0; i < ARROW_MAX; i++)
        if (arrows[i].actor && !arrows[i].foe && !arrows[i].flew && !arrows[i].done && arrow_alive(&arrows[i]))
            return arrows[i].actor;
    return 0;
}

// (n4, 2026-11-17) Arrows hit every remote avatar, not only the partner's: each flying arrow is tested against the
// avatar nearest to it, and a hit on an extra player's avatar goes to that player only (coop_net_send_event_to,
// protocol 60). SAVAGEWIND_COOP_MULTIHITS=0: only the partner's avatar is hit (as before).
static int multihits_c = -1;
static bool multihits_on(void) { return env_on("SAVAGEWIND_COOP_MULTIHITS", &multihits_c); }
static const struct RemoteAv *arrow_target;  // (arrows_frame) the avatar the arrow is tested against now

static void arrow_hit_send(struct Arrow *r, const float p[3], const float dir_in[3], float dist, float v, const char *how) {
    r->done = true;
    arrow_fly_send_stop(r, p);
    float dir[3] = {dir_in[0], 0, dir_in[2]};
    if (r->fly_dir[0] || r->fly_dir[2]) dir[0] = r->fly_dir[0], dir[2] = r->fly_dir[2];
    int power = r->power > 400 ? 400 : r->power < 0 ? 0 : r->power;
    CoopAreaFx e = {r->foe ? COOP_AREAFX_FOE_ARROW : COOP_AREAFX_ARROW, r->type, (int16_t)power, {p[0], p[1], p[2]},
                    {dir[0], 0, dir[2]}, 0};
    bool extra = arrow_target && !arrow_target->partner;
    bool ok = coop_areafx_ok(&e) && (extra ? coop_net_send_event_to(arrow_target->pid, COOP_EV_AREAFX, &e, sizeof e)
                                           : coop_net_send_event(COOP_EV_AREAFX, &e, sizeof e));
    if (ok) r->foe ? foe_arrow_hits++ : arrows_hits++;
    LOG("[coop] arrows: %s%s %s hit %s %d (%s, %.2f m from the body axis, %.1f m/s, power %d): %s\n",
        r->foe ? r->foe_name : "our", r->foe ? "'s" : "", arrow_names[r->type], extra ? "extra player" : "the other player",
        arrow_target ? arrow_target->pid : -1, how, dist, v, power, ok ? "told them" : "NOT sent");
}

static void arrows_frame(Cpu *c) {
    (void)c;
    static bool trace_read, trace;
    if (!trace_read) trace_read = true, trace = getenv("SAVAGEWIND_COOP_ARROWTRACE") && strcmp(getenv("SAVAGEWIND_COOP_ARROWTRACE"), "0");
    bool ours_hit = arrows_on() && pvp_on(), ours_fly = arrow_fly_on();
    bool ours_track = ours_hit || ours_fly, foes_ok = foe_arrows_on();
    if (!ours_track && !foes_ok) return;
    uint64_t now = plat_now_ns();
    if (!local_in_world() || !player_info_ptr()) { memset(arrows, 0, sizeof arrows); return; }
    float me[3];
    drop_me(me);
    me[1] += 1.0f;
    // Look for new arrows while our bow is out (and a little after), else every 10th frame (an arrow sits on the
    // string ~0.3 s or more before it flies). (foe arrows: only while the avatar is here to be hit)
    static unsigned frame;
    frame++;
    // (n4: the avatars arrows can hit - the partner's, and with MULTIHITS every extra player's; rav: multi.inc)
    float pp[3];
    bool partner = drop_partner(pp) && avatar.actor && avatar_alive() && avatar.frames >= 6;
    int targets = 0;
    for (int t = 0; t < rav_n; t++)
        if (rav[t].partner ? partner : multihits_on() && rav[t].fresh && (rav[t].rs.flags & COOP_FLAG_IN_WORLD)) targets++;
    foes_ok = foes_ok && targets;
    if (now - arrows_bow_ns < 4000000000ull || frame % 10 == 0) arrows_scan(me, now, ours_track, foes_ok);
    for (int i = 0; i < ARROW_MAX; i++) {
        struct Arrow *r = &arrows[i];
        if (!r->actor) continue;
        if (!arrow_alive(r)) {
            arrow_fly_send_stop(r, r->prev);
            if (trace && r->flew && !r->done) LOG("[coop] arrowtrace %s %llx gone\n", arrow_names[r->type], (unsigned long long)r->actor);
            memset(r, 0, sizeof *r);
            continue;
        }
        const float *m = (const float *)(uintptr_t)(r->actor + ACTOR_MTX);
        float p[3] = {m[3], m[7], m[11]};
        double dt = now > r->last_ns ? (now - r->last_ns) / 1e9 : 0;
        if (dt < 0.001) continue;  // (the same game frame twice)
        float v = dt < 0.25 ? (float)(dist3(p, r->prev) / dt) : 0;
        float prev[3];
        memcpy(prev, r->prev, 12);
        memcpy(r->prev, p, 12);
        r->last_ns = now;
        // (an arrow on the string jumps ~1 m in one frame when it is put on: 37-50 m/s seen; and the arrow put on
        // after a shot can stay on the string, not moving, for as long as the bow stays out: only a fast arrow away
        // from Link has been shot, and only a shot arrow is let go when it lies still)
        if (r->foe && !r->flew) {  // (where its shooter is: the arrow is on its string until it flies)
            for (int k = 0; k < enemy_count; k++)
                if (enemies[k].actor == r->foe) {
                    const float *fm = (const float *)(uintptr_t)(r->foe + ACTOR_MTX);
                    r->src[0] = fm[3], r->src[1] = fm[7] + 1.0f, r->src[2] = fm[11];
                }
        }
        float from_me = dist3(p, r->foe ? r->src : me);
        bool was_flying = r->flew;
        if (v > ARROW_FLY_MPS && from_me > 1.5f) r->flew = true, r->still_since_ns = 0;
        if (r->flew && !r->fly_sent && !r->foe && ours_fly && dt >= 0.004 && dt < 0.25 && v > ARROW_FLY_MPS) {
            float vel[3] = {(float)((p[0] - prev[0]) / dt), (float)((p[1] - prev[1]) / dt),
                            (float)((p[2] - prev[2]) / dt)};
            const float *start = dist3(p, prev) < 10.0f ? prev : p;
            arrow_fly_send_shot(r, start, vel);
        }
        if (r->flew && !was_flying && r->foe && trace)
            LOG("[coop] arrowtrace %s's %s %llx flies\n", r->foe_name, arrow_names[r->type], (unsigned long long)r->actor);
        if (r->flew && v > r->top_v) r->top_v = v;
        if (r->flew && v > 2 * ARROW_FLY_MPS && dist3(p, prev) > 1e-3f) {
            float d = dist3(p, prev);
            for (int k = 0; k < 3; k++) r->dir3[k] = (p[k] - prev[k]) / d;
        }
        if (r->flew && v < 1.0f && !r->still_since_ns) {
            r->still_since_ns = now;
            arrow_fly_send_stop(r, p);
        }
        if (r->done || (r->flew && r->still_since_ns && now - r->still_since_ns > ARROW_FORGET_NS) ||
            (!r->flew && from_me > 4.0f) || (r->foe && !r->flew && now - r->still_since_ns > ARROW_FOE_IDLE_NS) ||
            (r->foe ? !foes_ok : !ours_track)) {
            arrow_fly_send_stop(r, p);
            arrow_retire(r);
            continue;
        }
        if (!r->flew || (!r->foe && !ours_hit)) continue;
        arrow_target = NULL;  // (n4) the nearest avatar it can hit
        float best = 1e9f;
        for (int t = 0; t < rav_n; t++) {
            const struct RemoteAv *ra = &rav[t];
            if (!(ra->partner ? partner : multihits_on() && ra->fresh && (ra->rs.flags & COOP_FLAG_IN_WORLD))) continue;
            const float *tm = (const float *)(uintptr_t)(ra->actor + ACTOR_MTX);
            float q[3] = {tm[3], tm[7] + 1.0f, tm[11]}, d = dist3(p, q);
            if (d < best) best = d, arrow_target = ra;
        }
        if (!arrow_target) continue;
        const float *am = (const float *)(uintptr_t)(arrow_target->actor + ACTOR_MTX);
        float lo[3] = {am[3], am[7] + PVP_BODY_LO, am[11]}, hi[3] = {am[3], am[7] + PVP_BODY_HI, am[11]};
        // (the arrow's origin is ~1.2 m behind its tip: test 2026-11-12, an arrow at the avatar stopped / bounced back
        // with its origin 1.5-1.6 m from the body axis (Dm_CoopBody r 0.4). The path is tested up to where the tip
        // was, the way it flew last.)
        float tip[3] = {p[0] + r->dir3[0] * ARROW_LEN, p[1] + r->dir3[1] * ARROW_LEN, p[2] + r->dir3[2] * ARROW_LEN};
        float dist = seg_seg(prev, tip, lo, hi);
        float dir[3] = {p[0] - prev[0], 0, p[2] - prev[2]}, l = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
        if (l > 1e-4f) dir[0] /= l, dir[2] /= l;
        else dir[0] = 0, dir[2] = 1;
        // (the way it flew AT the avatar: the frame of the hit may already be a bounce)
        float to[3] = {am[3] - prev[0], 0, am[11] - prev[2]}, tl = sqrtf(to[0] * to[0] + to[2] * to[2]);
        if (v > ARROW_FLY_MPS && tl > 1e-3f && (dir[0] * to[0] + dir[2] * to[2]) / tl > 0.3f) memcpy(r->fly_dir, dir, 12);
        float hx = p[0] - am[3], hz = p[2] - am[11], axis = sqrtf(hx * hx + hz * hz), up = p[1] - am[7];
        if (trace)
            LOG("[coop] arrowtrace %s %llx: at %.2f %.2f %.2f, %.1f m/s, path %.2f m from the body axis (point %.2f m, "
                "%.2f m up)\n", arrow_names[r->type], (unsigned long long)r->actor, p[0], p[1], p[2], v, dist, axis, up);
        if (v > 1.0f && dist < ARROW_BODY_R) {
            arrow_hit_send(r, p, dir, dist, v, "through the body");
        } else if (r->top_v > 2 * ARROW_FLY_MPS && v < 3.0f && axis < ARROW_STOP_R && up > 0.3f && up < 2.1f) {
            // (stopped dead at the avatar: stuck in or bounced off its collision body, Dm_CoopBody r 0.4)
            arrow_hit_send(r, p, dir, axis, r->top_v, "stopped at the body");
        }
    }
}

// The other game's arrow hit our Link there (game thread, from pvp_areafx_ev).
static void arrows_ev(Cpu *c, const CoopAreaFx *e) {
    uint64_t pa = player_info_ptr() ? LD64(player_info_ptr() + 0x60) : 0;
    char as[64] = "", area[96];
    float me[3];
    drop_me(me);
    float body[3] = {me[0], me[1] + 0.9f, me[2]}, dist = dist3(body, e->pos);
    CoopState rs;
    double age = 99;
    // (n4: an extra player's arrow, sent to us only: world_event_from has the remote state viewed as that player's)
    bool same_area = coop_net_get_remote(&rs, &age) && rs.area == local_area(area, sizeof area);
    const char *what = e->flags < COOP_ARROW_TYPES ? arrow_names[e->flags] : "arrow";
    bool foe = e->kind == COOP_AREAFX_FOE_ARROW, on = foe ? foe_arrows_on() : pvp_on() && arrows_on();
    // (where the arrow met the avatar there vs. where we are here: a few tenths of a second of lag apart)
    bool ok = on && same_area && dist < 3.0f && pvp_victim_ok(c, pa, as, sizeof as);
    LOG("[coop] pvp: %s %s hit us there (%.1f m from us here): %s\n", foe ? "an enemy's" : "the other player's", what,
        dist, ok ? "hit" : !on ? "arrow damage off" : "out of reach / can't be hit now");
    if (!ok) return;
    float from[3] = {e->pos[0] - e->dir[0] * 2, e->pos[1], e->pos[2] - e->dir[2] * 2};
    if (pvp_guarded(c, pa, from)) {
        LOG("[coop] pvp: blocked the %s with the shield (%s)\n", what, as);
        guard_fx(c, pa, from);
        return;
    }
    if (!foe && elemarrow_hit(c, pa, e->flags)) return;
    {
        char all[256];
        as_list(c, pa, all, sizeof all);
        const float *m = (const float *)(uintptr_t)(pa + ACTOR_MTX);
        LOG("[coop] pvp: not blocked (facing %.2f %.2f, the shot from %.2f %.2f; AS %s)\n", m[2], m[10], from[0] - m[3],
            from[2] - m[11], all);
    }
    int dmg = !foe && e->damage > PVP_DMG_MAX ? PVP_DMG_MAX : e->damage, life = (int)LD32(pa + PVP_LIFE);
    if (e->flags == COOP_ARROW_BOMB && dmg < 4) dmg = 4;  // (the blast: at least a heart)
    int level = e->flags == COOP_ARROW_BOMB ? 2 : 1;
    float dir[3] = {e->dir[0], 0, e->dir[2]};
    int got = player_hit_ex(pa, dmg, level, level == 2 ? 0x15 : 0x11, level == 2 ? 0x10 : 0x14, dir, foe);
    LOG("[coop] pvp: %s: -%d life (%d -> %d), %s\n", what, got, life, life - got,
        level == 2 ? "knocked down" : "medium reaction");
    if (foe && got < life && e->flags >= COOP_ARROW_FIRE && e->flags <= COOP_ARROW_SHOCK) foe_elem(c, pa, e->flags);
}

// ---- (protocol 74, 2026-11-19) Partner's arrows visible in flight (receiver side).
// Uses the physics-free Dm_CoopW_*Arrow puppets already in the overlay (build_overlay.py): no physics body, no attack
// sensor, no AI — can never hurt anything or double the hit from COOP_EV_AREAFX.
#define ARROW_FLY_MAX 8
#define ARROW_FLY_MAX_NS 4000000000ull
#define ARROW_FLY_STOP_CAP_NS 500000000ull
#define ARROW_FLY_KEEP_NS 20000000000ull
#define ARROW_FLY_PARK_DROP 40.0f
#define ARROW_FLY_RANGE_M 250.0f
static const char *const arrow_fly_pup_names[COOP_ARROW_TYPES] = {
    "Dm_CoopW_NormalArrow", "Dm_CoopW_BombArrow_A", "Dm_CoopW_AncientArrow",
    "Dm_CoopW_FireArrow",   "Dm_CoopW_IceArrow",    "Dm_CoopW_ElectricArrow",
};
static struct ArrowFlySlot {
    struct Pup p;
    bool active, has_stop, shown;
    int origin;
    uint32_t id, area;
    uint8_t kind;
    float pos[3], vel[3], gravity, stop_pos[3];
    uint64_t born_ns, last_ns, stop_ns, used_ns;
} arrow_fly[ARROW_FLY_MAX];

static void arrow_fly_reset(void) {
    memset(arrows, 0, sizeof arrows);
    memset(arrow_fly, 0, sizeof arrow_fly);
}

// Build a 3x4 world matrix with local +Z (arrow shaft -> tip, verified in Arrow.bphysics) along vel.
static void arrow_fly_mtx(float m[12], const float pos[3], const float vel[3]) {
    float vl = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    float f[3] = {0, 0, 1};
    if (vl > 1e-4f) f[0] = vel[0] / vl, f[1] = vel[1] / vl, f[2] = vel[2] / vl;
    float u[3] = {0, 1, 0};
    if (fabsf(f[1]) > 0.98f) u[0] = 1, u[1] = 0, u[2] = 0;
    float r[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl > 1e-5f) r[0] /= rl, r[1] /= rl, r[2] /= rl;
    float up[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    m[0] = r[0];  m[1] = up[0];  m[2]  = f[0];  m[3]  = pos[0];
    m[4] = r[1];  m[5] = up[1];  m[6]  = f[1];  m[7]  = pos[1];
    m[8] = r[2];  m[9] = up[2];  m[10] = f[2];  m[11] = pos[2];
}

// Reliable COOP_EV_ARROW_FLY event from another player (game thread, from world_event).
static void arrow_fly_ev(const uint8_t *data, int len) {
    if (!arrow_fly_on() || len != (int)sizeof(CoopArrowFly)) return;
    CoopArrowFly e;
    memcpy(&e, data, sizeof e);
    if (!coop_arrow_fly_ok(&e)) return;
    if (!local_in_world() || !player_info_ptr()) return;
    char abuf[96];
    uint32_t here = local_area(abuf, sizeof abuf);
    if (!here || e.area != here) return;
    int origin = world_ev_origin >= 0 ? world_ev_origin : 0;
    uint64_t now = plat_now_ns();
    if (e.flags == COOP_ARROW_FLY_STOP) {
        for (int i = 0; i < ARROW_FLY_MAX; i++) {
            struct ArrowFlySlot *s = &arrow_fly[i];
            if (!s->active || s->origin != origin || s->id != e.id) continue;
            float vl = sqrtf(s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] + s->vel[2] * s->vel[2]);
            float dx = e.pos[0] - s->pos[0], dy = e.pos[1] - s->pos[1], dz = e.pos[2] - s->pos[2];
            float along = vl > 1e-3f ? (dx * s->vel[0] + dy * s->vel[1] + dz * s->vel[2]) / vl : 0.0f;
            if (along <= 0.5f) {
                s->active = false;
            } else {
                // Short-range shot where SHOT and STOP arrived together / early: let the visual finish reaching stop_pos.
                s->has_stop = true;
                memcpy(s->stop_pos, e.pos, 12);
                s->stop_ns = now;
            }
            return;
        }
        return;
    }
    float me[3];
    drop_me(me);
    if (dist3(e.pos, me) > ARROW_FLY_RANGE_M) return;
    const char *want = arrow_fly_pup_names[e.kind];
    int pick = -1;
    // Prefer an inactive slot that already owns a parked puppet of the same arrow kind (instant frame-0 reuse).
    for (int i = 0; i < ARROW_FLY_MAX && pick < 0; i++)
        if (!arrow_fly[i].active && arrow_fly[i].p.actor && !strcmp(arrow_fly[i].p.name, want)) pick = i;
    for (int i = 0; i < ARROW_FLY_MAX && pick < 0; i++)
        if (!arrow_fly[i].active && !arrow_fly[i].p.actor) pick = i;
    for (int i = 0; i < ARROW_FLY_MAX && pick < 0; i++)
        if (!arrow_fly[i].active) pick = i;
    if (pick < 0) {
        pick = 0;
        for (int i = 1; i < ARROW_FLY_MAX; i++)
            if (arrow_fly[i].born_ns < arrow_fly[pick].born_ns) pick = i;
    }
    struct ArrowFlySlot *s = &arrow_fly[pick];
    s->active = true, s->has_stop = false, s->shown = false;
    s->origin = origin, s->id = e.id, s->area = e.area, s->kind = e.kind;
    memcpy(s->pos, e.pos, 12);
    memcpy(s->vel, e.vel, 12);
    s->gravity = e.gravity;
    s->born_ns = s->last_ns = s->used_ns = now;
    s->stop_ns = 0;
}

static void arrow_fly_frame(Cpu *c) {
    uint64_t now = plat_now_ns();
    if (!local_in_world() || !player_info_ptr()) {
        for (int i = 0; i < ARROW_FLY_MAX; i++) {
            arrow_fly[i].active = arrow_fly[i].shown = false;
            arrow_fly[i].p.actor = 0, arrow_fly[i].p.name[0] = 0;
        }
        return;
    }
    static int have[COOP_ARROW_TYPES];  // 0 unknown, 1 present in overlay, -1 absent
    float me[3];
    drop_me(me);
    char abuf[96];
    uint32_t here = local_area(abuf, sizeof abuf);
    bool on = arrow_fly_on();
    for (int i = 0; i < ARROW_FLY_MAX; i++) {
        struct ArrowFlySlot *s = &arrow_fly[i];
        if (s->p.actor && !pup_alive(&s->p)) s->p.actor = 0, s->p.name[0] = 0, s->shown = false;
        if (!on) s->active = false;
        if (s->active) {
            if (!here || s->area != here || now - s->born_ns > ARROW_FLY_MAX_NS ||
                dist3(s->pos, me) > ARROW_FLY_RANGE_M || (s->has_stop && now - s->stop_ns > ARROW_FLY_STOP_CAP_NS)) {
                s->active = false;
            } else {
                float dt = now > s->last_ns ? (float)((now - s->last_ns) / 1e9) : 0.0f;
                if (dt > 0.1f) dt = 0.1f;
                s->last_ns = now;
                s->pos[0] += s->vel[0] * dt;
                s->pos[1] += s->vel[1] * dt + 0.5f * s->gravity * dt * dt;
                s->pos[2] += s->vel[2] * dt;
                s->vel[1] += s->gravity * dt;
                if (s->has_stop) {
                    float dx = s->stop_pos[0] - s->pos[0], dy = s->stop_pos[1] - s->pos[1],
                          dz = s->stop_pos[2] - s->pos[2];
                    if (dx * s->vel[0] + dy * s->vel[1] + dz * s->vel[2] <= 0.0f) s->active = false;
                }
            }
        }
        if (!s->active) {
            if (!s->p.actor) continue;
            if (now - s->used_ns > ARROW_FLY_KEEP_NS) {
                pup_drop(c, &s->p);
                s->shown = false;
                continue;
            }
            if (s->shown) {
                static unsigned park_said;
                if (park_said++ < 60)
                    LOG("[coop] arrows: flying %s (%x) done (parked)\n", s->p.name, s->id);
                s->shown = false;
            }
            float m[12] = {1, 0, 0, me[0], 0, 1, 0, me[1] - ARROW_FLY_PARK_DROP, 0, 0, 1, me[2]};
            memcpy((void *)(uintptr_t)(s->p.actor + ACTOR_MTX), m, sizeof m);
            memcpy((void *)(uintptr_t)(s->p.actor + ACTOR_MTX2), m, sizeof m);
            continue;
        }
        const char *name = arrow_fly_pup_names[s->kind];
        if (!have[s->kind]) {
            have[s->kind] = pack_exists(name) ? 1 : -1;
            if (have[s->kind] < 0)
                LOG("[coop] arrows: no %s in the overlay (old overlay): flying arrow not shown\n", name);
        }
        if (have[s->kind] < 0) { s->active = false; continue; }
        float m[12];
        arrow_fly_mtx(m, s->pos, s->vel);
        if (s->p.actor && strcmp(s->p.name, name)) pup_drop(c, &s->p);
        if (!s->p.actor) {
            if (now < s->p.retry_ns || spawn_hold()) continue;
            s->p.retry_ns = now + 2000000000ull;
            s->p.actor = spawn_mtx_now(c, name, m);
            if (!s->p.actor) {
                if (!s->p.said) s->p.said = true, LOG("[coop] arrows: could not create %s for flying arrow\n", name);
                continue;
            }
            s->p.unit = LD64(s->p.actor + PROC_UNIT), s->p.id = LD32(s->p.actor + PROC_ID);
            snprintf(s->p.name, sizeof s->p.name, "%s", name);
        }
        memcpy((void *)(uintptr_t)(s->p.actor + ACTOR_MTX), m, sizeof m);
        memcpy((void *)(uintptr_t)(s->p.actor + ACTOR_MTX2), m, sizeof m);
        s->used_ns = now;
        if (!s->shown) {
            s->shown = true;
            static unsigned show_said;
            if (show_said++ < 60) {
                float spd = sqrtf(s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] + s->vel[2] * s->vel[2]);
                LOG("[coop] arrows: showing %s (%x, %llx) at %.1f %.1f %.1f, %.0f m/s (player %d)\n", name, s->id,
                    (unsigned long long)s->p.actor, s->pos[0], s->pos[1], s->pos[2], spd, s->origin);
            }
        }
    }
}
