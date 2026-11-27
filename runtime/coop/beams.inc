// Part of runtime/coop.c (included there, same translation unit): Guardian targeting lasers and beam shots in every
// game (protocol 90, worker "beams", 2026-11-19).
//
// Only the game that runs a Guardian's AI (enemies.inc: e->mine && !e->following, the "owner") aims and fires; the
// other games' copies have their AI off. So the partner saw no red targeting line and no beam, or (in the owner's
// game) the beam flew at the partner's avatar and hurt nobody. Facts used (Ghidra, 1.9.0):
//  - The beam is its own actor "GuardianBeam" (ActorInfo profile Beam, Attack Power 52, ELink user GuardianBeam).
//    Its AI GuardianBeamFire (factory 0x7100b6a138) takes its direction, on enter, from the actor's matrix z column
//    (0x71006a33c0: actor+0x3a0 / 0x3b0 / 0x3c0) and its speed from its Speed parameter (200 m/s). So a GuardianBeam
//    created here with a matrix whose z column points at a player flies at that player, with its own hit, guard,
//    perfect-guard reflect and explosion, all decided in this game.
//  - The targeting laser is drawn by the AI action GuardianAimBeam (vtable 0x7101c05750, run on the Guardian's
//    "GuardianBeamCannon" part for a Stalker) through a helper at action+0x68 (init 0x7100ba44b8, calc 0x7100ba4774,
//    end 0x7100ba4bc8): ELink keys "Target" (handle at action+0x90) and "Laser" (handle at action+0xa0) emitted on
//    that actor with 0x7101100bd0; each frame the Laser effect gets a 3x4 matrix (z column = direction, translation =
//    origin) at fx+0xc0, length at fx+0x110 (fx+0x10c = 1), flags |= 0x408; the Target effect its position at
//    fx+0xa8; the frame counter at action+0x140 rises; the end point is kept at action+0x144. On end both effects are
//    killed (0x700056d010) and "Target_End" is emitted at the end point. Sound: SLink "BeamSightSearch" (loop) while
//    aiming, "BeamSightLocked" at the end.
// Owner: every new GuardianBeam near a Guardian we run is sent once (COOP_BEAM_SHOT: start, direction, the player it
// flies at, as an offset from that player's body); while a GuardianAimBeam's laser is live it is sent (COOP_BEAM_AIM,
// every BEAM_AIM_KEEP_NS and when it moves) and its end (AIM_END). Followers: the laser is drawn from their copy's
// cannon to the same player (their own Link when it is them, else that player's avatar); the beam is created and flies
// at that player. No damage is sent: the beam itself hits the targeted player in their own game (a perfect guard there
// sends it back into the Guardian copy, whose damage the enemy sync gives the owner).
// SAVAGEWIND_COOP_BEAMS=0: all of it off; SAVAGEWIND_COOP_BEAMLASER=0: no laser (beams only);
// SAVAGEWIND_COOP_BEAMSOUND=0: the laser without its sounds; SAVAGEWIND_COOP_BEAMQUIET=0: our copies' own lasers and
// beams are left alone (see "followers: our copies stay quiet").

#define A_FX_EMIT_H 0x7001100bd0ull   // (out x8: handle {fx, gen}, actor, key): ELink emit with a handle
#define A_SND_EMIT_H 0x7001395354ull  // (out x8: handle, actor, key, 0): SLink emit with a handle
#define A_SND_STOP 0x700056cc8cull    // (handle, 0)
#define A_FX_KILL_H 0x700056d010ull   // (handle)
#define VT_GUARDIAN_AIM_BEAM 0x7001c05750ull
#define AIMB_TARGET_H 0x90
#define AIMB_LASER_H 0xa0
#define AIMB_TICK 0x140
#define AIMB_END 0x144
#define BEAM_FOE_M 20.0f          // a new beam this close to a Guardian we run (feet) is that Guardian's
#define BEAM_SPEED 200.0f         // GuardianBeam's Speed parameter
#define BEAM_HIT_R 6.0f           // the player whose body the beam's line passes nearest within this is its target
#define BEAM_COPY_R 4.5f          // a follower's beam starts outside this sphere around the Guardian copy's middle
#define BEAM_AIM_KEEP_NS 300000000ull
#define BEAM_AIM_GAP_NS 150000000ull
#define BEAM_AIM_TIMEOUT_NS 1200000000ull
#define BEAM_BODY_UP 0.8f         // a player's body centre above the feet / position

static int beams_c = -1, beamlaser_c = -1, beamsound_c = -1;
static bool beams_on(void) { return env_on("SAVAGEWIND_COOP_BEAMS", &beams_c); }
static bool beamlaser_on(void) { return beams_on() && env_on("SAVAGEWIND_COOP_BEAMLASER", &beamlaser_c); }
static bool beamsound_on(void) { return env_on("SAVAGEWIND_COOP_BEAMSOUND", &beamsound_c); }
static const char *const beam_kind_names[COOP_BEAM_KINDS] = {"GuardianBeam", "GuardianMiniBeam", "GuardianMiniBeam_Junior"};
static int beam_shots_sent, beam_shots_shown, beam_aims_sent, beam_aims_shown;  // (log / tests)

static bool beam_guardian_name(const char *n) { return !strncmp(n, "Enemy_Guardian", 14); }
static bool beam_live(uint64_t a, uint64_t unit, uint32_t id) {
    return in_guest(a) && in_guest(unit) && LD32(unit + UNIT_ID) == id && LD64(unit + UNIT_ACTOR) == a &&
           LD8(a + PROC_STATE) != 3;
}
static void beam_mpos(uint64_t a, float p[3]) {
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    p[0] = m[3], p[1] = m[7], p[2] = m[11];
}
// Natively walk the process ring (names only, no game calls): fn(actor, unit, name, ctx) for each live actor.
typedef void (*BeamRingFn)(uint64_t a, uint64_t u, const char *nm, void *ctx);
static void beam_ring(BeamRingFn fn, void *ctx) {
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
            if (strnlen(nm, 48) < 48) fn(a, u, nm, ctx);
        }
        if (!in_guest(next)) break;
        u = next - UNIT_NODE;
        if (u == start) break;
    }
}
// The Guardian we run nearest to p (its feet) within r, or NULL.
static struct Enemy *beam_foe(const float p[3], float r) {
    struct Enemy *best = NULL;
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (!e->actor || !e->mine || e->following || e->last <= 0 || !beam_guardian_name(e->name)) continue;
        float q[3];
        beam_mpos(e->actor, q);
        float d = dist3(p, q);
        if (d < r) r = d, best = e;
    }
    return best;
}
static struct Enemy *beam_enemy_by_hash(uint32_t hash) {
    for (int i = 0; i < enemy_count; i++)
        if (enemies[i].hash == hash && hash) return &enemies[i];
    return NULL;
}

// ---- the players' bodies here: our Link (our player id) and every avatar (rav: its player id)
struct BeamBody { int pid; float c[3]; };
static int beam_bodies(struct BeamBody *b) {
    int n = 0, me = coop_net_my_id();
    if (me >= 0 && me < COOP_MAX_PLAYERS && player_info_ptr()) {
        drop_me(b[n].c);
        b[n].c[1] += BEAM_BODY_UP;
        b[n++].pid = me;
    }
    for (int i = 0; i < rav_n && n < COOP_MAX_PLAYERS + 1; i++) {
        const struct RemoteAv *ra = &rav[i];
        if (!in_guest(ra->actor) || ra->pid < 0 || ra->pid >= COOP_MAX_PLAYERS || ra->pid == me) continue;
        beam_mpos(ra->actor, b[n].c);
        b[n].c[1] += BEAM_BODY_UP;
        b[n++].pid = ra->pid;
    }
    return n;
}
// Where a received aim points here: that player's body centre + the offset (our Link when it is us), or the point.
static bool beam_resolve(const CoopBeam *e, float out[3]) {
    if (e->target < 0) { memcpy(out, e->aim, 12); return true; }
    struct BeamBody b[COOP_MAX_PLAYERS + 1];
    int n = beam_bodies(b);
    for (int i = 0; i < n; i++)
        if (b[i].pid == e->target) {
            for (int k = 0; k < 3; k++) out[k] = b[i].c[k] + e->aim[k];
            return true;
        }
    return false;
}
// The target of a point (the laser's end): the body nearest to it within r, as an offset; else the point itself.
static void beam_aim_at_point(const float p[3], float r, CoopBeam *e) {
    struct BeamBody b[COOP_MAX_PLAYERS + 1];
    int n = beam_bodies(b), best = -1;
    for (int i = 0; i < n; i++) {
        float d = dist3(p, b[i].c);
        if (d < r) r = d, best = i;
    }
    e->target = best >= 0 ? (int8_t)b[best].pid : -1;
    for (int k = 0; k < 3; k++) e->aim[k] = best >= 0 ? p[k] - b[best].c[k] : p[k];
    if (best >= 0)
        for (int k = 0; k < 3; k++) e->aim[k] = e->aim[k] > 19.0f ? 19.0f : e->aim[k] < -19.0f ? -19.0f : e->aim[k];
}

// Guest memory: ELink / SLink key names and handles (hle_alloc never frees: allocated once).
static uint64_t beam_keys;  // "Laser" @0, "Target" @0x10, "Target_End" @0x20, "BeamSightSearch" @0x30, "BeamSightLocked" @0x50
static bool beam_mem(void) {
    if (beam_keys) return true;
    uint64_t k = (uint64_t)(uintptr_t)hle_alloc(0x80);
    if (!k) return false;
    memset((void *)(uintptr_t)k, 0, 0x80);
    strcpy((char *)(uintptr_t)k, "Laser");
    strcpy((char *)(uintptr_t)(k + 0x10), "Target");
    strcpy((char *)(uintptr_t)(k + 0x20), "Target_End");
    strcpy((char *)(uintptr_t)(k + 0x30), "BeamSightSearch");
    strcpy((char *)(uintptr_t)(k + 0x50), "BeamSightLocked");
    beam_keys = k;
    return true;
}
static uint64_t beam_emit(Cpu *c, uint64_t fn, uint64_t actor, uint64_t key, uint64_t handle) {
    memset((void *)(uintptr_t)handle, 0, 16);
    Cpu saved = *c;
    c->x[0] = actor, c->x[1] = key, c->x[2] = 0, c->x[8] = handle;  // (AArch64 indirect return)
    guest_call(c, fn);
    *c = saved;
    return fn == A_FX_EMIT_H ? elink_live_effect(handle) : LD64(handle);
}
static void beam_call1(Cpu *c, uint64_t fn, uint64_t handle) {
    Cpu saved = *c;
    c->x[0] = handle, c->x[1] = 0;
    guest_call(c, fn);
    *c = saved;
}
static void beam_fx_pos(uint64_t fx, const float p[3]) {
    static const float one[3] = {1, 1, 1};
    memcpy((void *)(uintptr_t)(fx + 0xa8), p, 12);
    memcpy((void *)(uintptr_t)(fx + 0xb4), one, 12);
    ST32(fx + 0x180, LD32(fx + 0x180) | 4);
}
// A row-major 3x4 whose z column is dir (unit) and translation p (the laser effect's and the beam's creation matrix).
static void beam_basis(const float d[3], const float p[3], float m[12]) {
    float x0 = d[2], x1 = 0, x2 = -d[0];
    if (fabsf(d[1]) > 0.99f) x0 = 0, x1 = -d[2], x2 = d[1];
    float l = sqrtf(x0 * x0 + x1 * x1 + x2 * x2);
    if (l < 1e-4f) x0 = 1, x1 = 0, x2 = 0, l = 1;
    x0 /= l, x1 /= l, x2 /= l;
    float y0 = d[1] * x2 - d[2] * x1, y1 = d[2] * x0 - d[0] * x2, y2 = d[0] * x1 - d[1] * x0;
    float r[12] = {x0, y0, d[0], p[0], x1, y1, d[1], p[1], x2, y2, d[2], p[2]};
    memcpy(m, r, sizeof r);
}

// ======== owner: beams fired by Guardians we run
// (test_beams run 1, 2026-11-19: the ring showed a flying beam as a new process id on each of its 4 frames of flight,
// and after the hit a resting GuardianBeam stayed at the impact point, seen as new ~17 times a second: 80 sends for a
// few shots, and the other game drew a pile of explosions. So flights are followed by their path, not by id: a beam
// is sent once it is seen again ahead on its own line (it flies), each flight once; resting beams are never sent.)
static uint32_t beam_seen_ids[32];  // beams we created (followers): never sent
static unsigned beam_seen_n;
static bool beam_seen(uint32_t id) {
    for (int i = 0; i < 32; i++)
        if (beam_seen_ids[i] == id && id) return true;
    return false;
}
#define BEAM_FLIGHTS 8
static struct BeamFlight {
    float first[3], last[3], dir[3];
    uint64_t first_ns, last_ns;
    bool sent;
    int8_t kind;
} bflight[BEAM_FLIGHTS];
static struct { uint32_t hash; uint64_t ns; } beam_foe_sent[8];  // (a Guardian fires at most once a second)
static int beam_kind(const char *nm) {
    for (int k = 0; k < COOP_BEAM_KINDS; k++)
        if (!strcmp(nm, beam_kind_names[k])) return k;
    return -1;
}
static int beamscout_c = -1;
static bool beam_kind_on(int k) {
    // (2026-11-21) Guardian Scouts' beams (shrine Scouts I-IV, the Junior's) too: the same flight (the beam actor
    // takes its direction from its matrix, its own Speed). SAVAGEWIND_COOP_BEAMSCOUT=0: Stalker / Skywatcher /
    // Turret beams only, as before.
    return k == 0 || (k > 0 && k < COOP_BEAM_KINDS && env_on("SAVAGEWIND_COOP_BEAMSCOUT", &beamscout_c));
}
struct BeamScan { uint32_t area; uint64_t now; };
static void beam_shot_send(const struct BeamFlight *f, uint32_t area, const char *nm, uint64_t now);
static void beam_shot_seen(uint64_t a, uint64_t u, const char *nm, void *ctx) {
    (void)u;
    int k = beam_kind(nm);
    if (k < 0 || !beam_kind_on(k)) return;
    if (beam_seen(LD32(a + PROC_ID))) return;  // (one we created)
    struct BeamScan *sc = ctx;
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    float p[3] = {m[3], m[7], m[11]}, d[3] = {m[2], m[6], m[10]};
    float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dl > 0.5f && dl < 2.0f)) return;
    for (int j = 0; j < 3; j++) d[j] /= dl;
    // the same flight as one followed? (resting where it was, or further on along its line)
    int free_i = -1, oldest = 0;
    for (int i = 0; i < BEAM_FLIGHTS; i++) {
        struct BeamFlight *f = &bflight[i];
        if (!f->last_ns || sc->now - f->last_ns > 500000000ull) { if (free_i < 0) free_i = i; continue; }
        if (f->last_ns < bflight[oldest].last_ns) oldest = i;
        if (f->kind != k) continue;
        float w[3] = {p[0] - f->last[0], p[1] - f->last[1], p[2] - f->last[2]};
        float t = w[0] * f->dir[0] + w[1] * f->dir[1] + w[2] * f->dir[2], w2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
        if (w2 < 0.6f * 0.6f) { f->last_ns = sc->now; return; }  // (resting: not a shot)
        float perp2 = w2 - t * t;
        if (t > 0.5f && t < 25.0f && perp2 < 1.2f * 1.2f) {  // (flew on along its line)
            memcpy(f->last, p, 12);
            f->last_ns = sc->now;
            if (!f->sent) {
                f->sent = true;
                float v[3] = {p[0] - f->first[0], p[1] - f->first[1], p[2] - f->first[2]};
                float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (vl > 0.5f) for (int j = 0; j < 3; j++) f->dir[j] = v[j] / vl;
                beam_shot_send(f, sc->area, nm, sc->now);
            }
            return;
        }
    }
    // a new one: followed if it starts near a Guardian we run (a beam first seen far from the muzzle: not taken)
    struct Enemy *foe = beam_foe(p, BEAM_FOE_M);
    if (!foe) return;
    struct BeamFlight *f = &bflight[free_i >= 0 ? free_i : oldest];
    memset(f, 0, sizeof *f);
    memcpy(f->first, p, 12), memcpy(f->last, p, 12), memcpy(f->dir, d, 12);
    f->first_ns = f->last_ns = sc->now, f->kind = (int8_t)k;
}
static void beam_shot_send(const struct BeamFlight *f, uint32_t area, const char *nm, uint64_t now) {
    int k = f->kind;
    const float *p = f->first, *d = f->dir;
    struct Enemy *foe = beam_foe(p, BEAM_FOE_M);
    if (!foe) return;
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (beam_foe_sent[i].hash == foe->hash) { slot = i; break; }
        if (beam_foe_sent[i].ns < beam_foe_sent[slot].ns) slot = i;
    }
    if (beam_foe_sent[slot].hash == foe->hash && now - beam_foe_sent[slot].ns < 1000000000ull) return;
    beam_foe_sent[slot].hash = foe->hash, beam_foe_sent[slot].ns = now;
    CoopBeam e = {foe->hash, area, COOP_BEAM_SHOT, (uint8_t)k, -1, 0, {p[0], p[1], p[2]}, {0}, BEAM_SPEED};
    // its target: the body whose centre the line passes nearest (ahead of it, within BEAM_HIT_R)
    struct BeamBody b[COOP_MAX_PLAYERS + 1];
    int n = beam_bodies(b), best = -1;
    float bd = BEAM_HIT_R, bt = 0;
    for (int i = 0; i < n; i++) {
        float w[3] = {b[i].c[0] - p[0], b[i].c[1] - p[1], b[i].c[2] - p[2]};
        float t = w[0] * d[0] + w[1] * d[1] + w[2] * d[2];
        if (t < 0.5f) continue;
        float q[3] = {p[0] + d[0] * t, p[1] + d[1] * t, p[2] + d[2] * t}, off = dist3(q, b[i].c);
        if (off < bd) bd = off, best = i, bt = t;
    }
    if (best >= 0) {
        e.target = (int8_t)b[best].pid;
        for (int j = 0; j < 3; j++) e.aim[j] = p[j] + d[j] * bt - b[best].c[j];
    } else {
        for (int j = 0; j < 3; j++) e.aim[j] = p[j] + d[j] * 100.0f;
    }
    bool ok = coop_beam_ok(&e) && coop_net_send_event(COOP_EV_BEAM, &e, sizeof e);
    if (ok) beam_shots_sent++;
    static unsigned said;
    if (said++ < 80)
        LOG("[coop] beams: %s's %s fired at %s %d (%.2f m off the body centre) from %.1f %.1f %.1f: %s\n", foe->name, nm,
            best >= 0 ? "player" : "nobody, aim", best >= 0 ? b[best].pid : -1, best >= 0 ? bd : 0.0f, p[0], p[1], p[2],
            ok ? "sent" : "NOT sent");
}
// Is a Guardian we run within 150 m of us? (only then is the ring walked for beams / lasers)
static bool beam_foes_near(const float me[3]) {
    for (int i = 0; i < enemy_count; i++) {
        const struct Enemy *e = &enemies[i];
        if (!e->actor || !e->mine || e->following || e->last <= 0 || !beam_guardian_name(e->name)) continue;
        float q[3];
        beam_mpos(e->actor, q);
        if (dist3(q, me) < 150.0f) return true;
    }
    return false;
}

// ======== owner: targeting lasers of Guardians we run
#define BEAM_LASERS 6
static struct BeamAimSrc {
    uint64_t actor, unit, action;  // the actor running GuardianAimBeam (cannon or body) and the action
    uint32_t id, hash;
    uint8_t kind;                  // 0 cannon part, 1 the body itself
    uint64_t body;                 // the Guardian's body actor (enemies[])
    bool on;
    uint32_t tick;
    uint64_t tick_ns, sent_ns, next_find_ns;
    CoopBeam last;
} aimsrc[BEAM_LASERS];
static uint64_t aimsrc_scan_ns;
static void aimsrc_seen(uint64_t a, uint64_t u, const char *nm, void *ctx) {
    (void)ctx;
    bool body = beam_guardian_name(nm), cannon = !strncmp(nm, "GuardianBeamCannon", 18);
    if (!body && !cannon) return;
    uint32_t id = LD32(a + PROC_ID);
    int free_i = -1;
    for (int i = 0; i < BEAM_LASERS; i++) {
        if (aimsrc[i].actor == a && aimsrc[i].id == id) return;
        if (free_i < 0 && (!aimsrc[i].actor || !beam_live(aimsrc[i].actor, aimsrc[i].unit, aimsrc[i].id))) free_i = i;
    }
    if (free_i < 0) return;
    float p[3];
    beam_mpos(a, p);
    struct Enemy *foe = NULL;
    if (body) {
        for (int i = 0; i < enemy_count && !foe; i++)
            if (enemies[i].actor == a && enemies[i].mine && !enemies[i].following) foe = &enemies[i];
    } else {
        foe = beam_foe(p, 12.0f);
    }
    if (!foe) return;
    struct BeamAimSrc *s = &aimsrc[free_i];
    memset(s, 0, sizeof *s);
    s->actor = a, s->unit = u, s->id = id, s->hash = foe->hash, s->kind = body ? 1 : 0, s->body = foe->actor;
}
static bool aimsrc_ok(const struct BeamAimSrc *s) {
    if (!s->actor || !beam_live(s->actor, s->unit, s->id) || !in_guest(s->body) || LD8(s->body + PROC_STATE) == 3) return false;
    struct Enemy *e = beam_enemy_by_hash(s->hash);
    return e && e->actor == s->body && e->mine && !e->following;
}
static void beam_aim_send(struct BeamAimSrc *s, uint32_t area, uint8_t what, const float org[3], const float end[3],
                          uint64_t now) {
    CoopBeam e = {s->hash, area, what, s->kind, -1, 0, {0}, {0}, 0};
    float bp[3];
    beam_mpos(s->body, bp);
    if (what == COOP_BEAM_AIM) {
        for (int k = 0; k < 3; k++) e.pos[k] = org[k] - bp[k];
        beam_aim_at_point(end, 5.0f, &e);
    }
    bool ok = coop_beam_ok(&e) && coop_net_send_event(COOP_EV_BEAM, &e, sizeof e);
    if (ok && what == COOP_BEAM_AIM) beam_aims_sent++;
    bool change = what != COOP_BEAM_AIM || !s->last.what || s->last.target != e.target;
    static unsigned said;
    if (change && said++ < 80)
        LOG("[coop] beams: laser of %llx (Guardian %u) %s%s%d: %s\n", (unsigned long long)s->actor, s->hash,
            what == COOP_BEAM_AIM ? "on player " : "off", what == COOP_BEAM_AIM ? "" : " ",
            what == COOP_BEAM_AIM ? e.target : 0, ok ? "sent" : "NOT sent");
    s->last = e, s->sent_ns = now;
}
static void beam_lasers_owner(uint32_t area, uint64_t now, bool scan) {
    if (scan && now >= aimsrc_scan_ns) {
        aimsrc_scan_ns = now + 1000000000ull;
        beam_ring(aimsrc_seen, NULL);
    }
    for (int i = 0; i < BEAM_LASERS; i++) {
        struct BeamAimSrc *s = &aimsrc[i];
        if (!s->actor) continue;
        if (!aimsrc_ok(s)) {
            if (s->on) {
                CoopBeam e = {s->hash, area, COOP_BEAM_AIM_END, s->kind, -1, 0, {0}, {0}, 0};
                if (coop_beam_ok(&e)) coop_net_send_event(COOP_EV_BEAM, &e, sizeof e);
            }
            memset(s, 0, sizeof *s);
            continue;
        }
        if (!s->action || LD64(s->action) != VT_GUARDIAN_AIM_BEAM || LD64(s->action + 8) != s->actor) {
            s->action = 0;
            if (now < s->next_find_ns) continue;
            s->action = wp_find_ai(s->actor, VT_GUARDIAN_AIM_BEAM);
            s->next_find_ns = now + 5000000000ull;
            if (!s->action) continue;
            static unsigned said;
            if (said++ < 20)
                LOG("[coop] beams: %s %llx (Guardian %u) aims with GuardianAimBeam %llx\n", s->kind ? "body" : "cannon",
                    (unsigned long long)s->actor, s->hash, (unsigned long long)s->action);
        }
        uint64_t fx = elink_live_effect(s->action + AIMB_LASER_H);
        uint32_t tick = LD32(s->action + AIMB_TICK);
        if (tick != s->tick) s->tick = tick, s->tick_ns = now;
        bool on = fx && now - s->tick_ns < 120000000ull;
        if (on) {
            float len = LDF32(fx + 0x110);
            float org[3] = {LDF32(fx + 0xcc), LDF32(fx + 0xdc), LDF32(fx + 0xec)};
            float end[3] = {LDF32(s->action + AIMB_END), LDF32(s->action + AIMB_END + 4), LDF32(s->action + AIMB_END + 8)};
            if (!(isfinite(len) && len > 0 && len < 1000.0f)) on = false;
            for (int k = 0; k < 3 && on; k++) on = isfinite(org[k]) && isfinite(end[k]);
            if (on) {
                float bp[3];
                beam_mpos(s->body, bp);
                if (dist3(org, bp) > 30.0f) on = false;
            }
            if (on) {
                CoopBeam probe = {0};
                beam_aim_at_point(end, 5.0f, &probe);
                bool moved = !s->on || probe.target != s->last.target ||
                             fabsf(probe.aim[0] - s->last.aim[0]) + fabsf(probe.aim[1] - s->last.aim[1]) +
                                     fabsf(probe.aim[2] - s->last.aim[2]) > 0.5f;
                if ((moved && now - s->sent_ns >= BEAM_AIM_GAP_NS) || !s->on || now - s->sent_ns >= BEAM_AIM_KEEP_NS)
                    beam_aim_send(s, area, COOP_BEAM_AIM, org, end, now);
                s->on = true;
                continue;
            }
        }
        if (s->on) {
            float z[3] = {0};
            beam_aim_send(s, area, COOP_BEAM_AIM_END, z, z, now);
            memset(&s->last, 0, sizeof s->last);
            s->on = false;
        }
    }
}

// ======== followers: lasers drawn from our copy
static struct BeamLaserView {
    uint32_t hash;
    uint8_t kind;
    int8_t target;
    float org_off[3], aim[3];
    uint64_t emitter, unit;  // our copy's cannon (or body)
    uint32_t id;
    uint64_t h_laser, h_target, h_snd, h_tmp;  // guest handles (16 bytes each)
    uint64_t last_ns, retry_ns;
    bool on;
    float end[3];
} lview[BEAM_LASERS];
struct BeamFind { float at[3]; float best; uint64_t a, u; };
static void beam_cannon_seen(uint64_t a, uint64_t u, const char *nm, void *ctx) {
    struct BeamFind *f = ctx;
    if (strncmp(nm, "GuardianBeamCannon", 18)) return;
    float p[3];
    beam_mpos(a, p);
    float d = dist3(p, f->at);
    if (d < f->best) f->best = d, f->a = a, f->u = u;
}
static void lview_stop(Cpu *c, struct BeamLaserView *v, bool end_fx) {
    if (!v->on) return;
    if (elink_live_effect(v->h_laser)) beam_call1(c, A_FX_KILL_H, v->h_laser);
    if (elink_live_effect(v->h_target)) beam_call1(c, A_FX_KILL_H, v->h_target);
    if (v->h_snd && LD64(v->h_snd)) beam_call1(c, A_SND_STOP, v->h_snd);
    if (end_fx && beam_live(v->emitter, v->unit, v->id) && local_in_world()) {
        uint64_t fx = beam_emit(c, A_FX_EMIT_H, v->emitter, beam_keys + 0x20, v->h_tmp);
        if (fx) beam_fx_pos(fx, v->end);
        if (beamsound_on()) beam_emit(c, A_SND_EMIT_H, v->emitter, beam_keys + 0x50, v->h_tmp);
    }
    memset((void *)(uintptr_t)v->h_laser, 0, 16), memset((void *)(uintptr_t)v->h_target, 0, 16);
    memset((void *)(uintptr_t)v->h_snd, 0, 16);
    v->on = false;
    static unsigned said;
    if (said++ < 80) LOG("[coop] beams: shown laser of Guardian %u ended\n", v->hash);
}
static struct BeamLaserView *lview_for(uint32_t hash, bool make) {
    struct BeamLaserView *fr = NULL;
    for (int i = 0; i < BEAM_LASERS; i++) {
        if (lview[i].hash == hash && (lview[i].on || lview[i].last_ns)) return &lview[i];
        if (!fr && !lview[i].on && !lview[i].last_ns) fr = &lview[i];
    }
    if (!make || !fr) return NULL;
    if (!fr->h_laser) {
        fr->h_laser = (uint64_t)(uintptr_t)hle_alloc(16), fr->h_target = (uint64_t)(uintptr_t)hle_alloc(16);
        fr->h_snd = (uint64_t)(uintptr_t)hle_alloc(16), fr->h_tmp = (uint64_t)(uintptr_t)hle_alloc(16);
        if (!fr->h_laser || !fr->h_target || !fr->h_snd || !fr->h_tmp) { fr->h_laser = 0; return NULL; }
        memset((void *)(uintptr_t)fr->h_laser, 0, 16), memset((void *)(uintptr_t)fr->h_target, 0, 16);
        memset((void *)(uintptr_t)fr->h_snd, 0, 16), memset((void *)(uintptr_t)fr->h_tmp, 0, 16);
    }
    fr->hash = hash;
    return fr;
}
// Each frame: every shown laser from our copy's emitter to the player it is on.
static void beam_lasers_view(Cpu *c, uint64_t now) {
    for (int i = 0; i < BEAM_LASERS; i++) {
        struct BeamLaserView *v = &lview[i];
        if (!v->last_ns && !v->on) continue;
        struct Enemy *e = beam_enemy_by_hash(v->hash);
        bool keep = now - v->last_ns < BEAM_AIM_TIMEOUT_NS && e && e->actor && e->following && local_in_world() &&
                    !spawn_hold();
        float bp[3], org[3], tgt[3];
        CoopBeam q = {0};
        q.target = v->target;
        memcpy(q.aim, v->aim, 12);
        if (keep) {
            beam_mpos(e->actor, bp);
            for (int k = 0; k < 3; k++) org[k] = bp[k] + v->org_off[k];
            keep = beam_resolve(&q, tgt);
        }
        if (keep && !beam_live(v->emitter, v->unit, v->id)) {
            v->emitter = 0;
            if (v->kind == 1) {
                v->emitter = e->actor, v->unit = LD64(e->actor + PROC_UNIT), v->id = LD32(e->actor + PROC_ID);
            } else {
                struct BeamFind f = {{bp[0], bp[1], bp[2]}, 12.0f, 0, 0};
                beam_ring(beam_cannon_seen, &f);
                if (f.a) v->emitter = f.a, v->unit = f.u, v->id = LD32(f.a + PROC_ID);
            }
            if (v->on) {  // (a new emitter: its effects start again)
                memset((void *)(uintptr_t)v->h_laser, 0, 16), memset((void *)(uintptr_t)v->h_target, 0, 16);
                v->on = false;
            }
            keep = v->emitter != 0;
        }
        if (!keep) {
            lview_stop(c, v, false);
            v->last_ns = 0, v->hash = 0, v->emitter = 0;
            continue;
        }
        float d[3] = {tgt[0] - org[0], tgt[1] - org[1], tgt[2] - org[2]};
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(len > 0.2f && len < 300.0f)) continue;
        for (int k = 0; k < 3; k++) d[k] /= len;
        memcpy(v->end, tgt, 12);
        uint64_t fl = v->on ? elink_live_effect(v->h_laser) : 0, ft = v->on ? elink_live_effect(v->h_target) : 0;
        if (!v->on) {
            ft = beam_emit(c, A_FX_EMIT_H, v->emitter, beam_keys + 0x10, v->h_target);
            fl = beam_emit(c, A_FX_EMIT_H, v->emitter, beam_keys, v->h_laser);
            if (beamsound_on()) beam_emit(c, A_SND_EMIT_H, v->emitter, beam_keys + 0x30, v->h_snd);
            v->on = true;
            beam_aims_shown++;
            static unsigned said;
            if (said++ < 80)
                LOG("[coop] beams: showing the laser of Guardian %u on player %d (%.1f m; effects laser %s, target %s)\n",
                    v->hash, v->target, len, fl ? "ok" : "MISSING", ft ? "ok" : "MISSING");
        }
        else if ((!fl || !ft) && now >= v->retry_ns) {  // (run 1: an emit now and then gave no effect: again)
            v->retry_ns = now + 250000000ull;
            if (!ft) ft = beam_emit(c, A_FX_EMIT_H, v->emitter, beam_keys + 0x10, v->h_target);
            if (!fl) fl = beam_emit(c, A_FX_EMIT_H, v->emitter, beam_keys, v->h_laser);
            static unsigned said;
            if (said++ < 20)
                LOG("[coop] beams: laser effects emitted again: laser %s, target %s\n", fl ? "ok" : "MISSING",
                    ft ? "ok" : "MISSING");
        }
        if (fl) {
            float m[12];
            beam_basis(d, org, m);
            static const float one[3] = {1, 1, 1};
            const float size[2] = {1.0f, len};
            memcpy((void *)(uintptr_t)(fl + 0xb4), one, 12);
            memcpy((void *)(uintptr_t)(fl + 0xc0), m, sizeof m);
            memcpy((void *)(uintptr_t)(fl + 0x10c), size, sizeof size);
            ST32(fl + 0x180, LD32(fl + 0x180) | 0x408);
        }
        if (ft) beam_fx_pos(ft, tgt);
    }
}

// ======== followers: our copies stay quiet
// (test_beams run 1: the host's screenshots showed a red laser from its copy to the HOST's Link while ours went to
// the guest's avatar: the copy's body has its AI off, but its GuardianBeamCannon part is its own actor whose AI still
// aims at our Link. Its laser effects are killed here each time they start (the calc then skips them: the handle is
// stale), and a GuardianBeam we did not create that starts at a copy is deleted (only the owner's beams count).
// SAVAGEWIND_COOP_BEAMQUIET=0: left as they are.)
static int beamquiet_c = -1;
static bool beamquiet_on(void) { return env_on("SAVAGEWIND_COOP_BEAMQUIET", &beamquiet_c); }
static struct { uint64_t actor, unit, action, next_find_ns; uint32_t id; } copysrc[BEAM_LASERS];
static uint64_t copysrc_scan_ns;
static int beam_copy_kills, beam_copy_dels;
static struct Enemy *beam_copy(const float p[3], float r) {
    struct Enemy *best = NULL;
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (!e->actor || !e->following || !beam_guardian_name(e->name)) continue;
        float q[3];
        beam_mpos(e->actor, q);
        float d = dist3(p, q);
        if (d < r) r = d, best = e;
    }
    return best;
}
static void copysrc_seen(uint64_t a, uint64_t u, const char *nm, void *ctx) {
    (void)ctx;
    if (!beam_guardian_name(nm) && strncmp(nm, "GuardianBeamCannon", 18)) return;
    float p[3];
    beam_mpos(a, p);
    if (!beam_copy(p, 12.0f)) return;
    uint32_t id = LD32(a + PROC_ID);
    int free_i = -1;
    for (int i = 0; i < BEAM_LASERS; i++) {
        if (copysrc[i].actor == a && copysrc[i].id == id) return;
        if (free_i < 0 && (!copysrc[i].actor || !beam_live(copysrc[i].actor, copysrc[i].unit, copysrc[i].id))) free_i = i;
    }
    if (free_i < 0) return;
    memset(&copysrc[free_i], 0, sizeof copysrc[free_i]);
    copysrc[free_i].actor = a, copysrc[free_i].unit = u, copysrc[free_i].id = id;
}
struct BeamDel { Cpu *c; };
static void beam_copy_shot_seen(uint64_t a, uint64_t u, const char *nm, void *ctx) {
    (void)u;
    int k = beam_kind(nm);
    if (k < 0 || !beam_kind_on(k) || beam_seen(LD32(a + PROC_ID))) return;
    float p[3];
    beam_mpos(a, p);
    struct Enemy *cp = beam_copy(p, BEAM_FOE_M);
    if (!cp || beam_foe(p, BEAM_FOE_M)) return;  // (near a Guardian we run too: may be its own)
    bool ok = despawn_now(((struct BeamDel *)ctx)->c, a, ACTOR_DELETE_REASON);
    beam_copy_dels++;
    static unsigned said;
    if (said++ < 20)
        LOG("[coop] beams: our copy of %s %u fired a beam of its own at %.1f %.1f %.1f: %s\n", cp->name, cp->hash, p[0],
            p[1], p[2], ok ? "deleted" : "NOT deleted");
}
static bool beam_copies_near(const float me[3]) {
    for (int i = 0; i < enemy_count; i++) {
        const struct Enemy *e = &enemies[i];
        if (!e->actor || !e->following || !beam_guardian_name(e->name)) continue;
        float q[3];
        beam_mpos(e->actor, q);
        if (dist3(q, me) < 150.0f) return true;
    }
    return false;
}
static void beam_copies_quiet(Cpu *c, const float me[3], uint64_t now) {
    if (!beamquiet_on()) return;
    if (!beam_copies_near(me)) { memset(copysrc, 0, sizeof copysrc); return; }
    struct BeamDel del = {c};
    beam_ring(beam_copy_shot_seen, &del);
    if (now >= copysrc_scan_ns) {
        copysrc_scan_ns = now + 1000000000ull;
        beam_ring(copysrc_seen, NULL);
    }
    for (int i = 0; i < BEAM_LASERS; i++) {
        if (!copysrc[i].actor) continue;
        if (!beam_live(copysrc[i].actor, copysrc[i].unit, copysrc[i].id)) { memset(&copysrc[i], 0, sizeof copysrc[i]); continue; }
        uint64_t act = copysrc[i].action;
        if (!act || LD64(act) != VT_GUARDIAN_AIM_BEAM || LD64(act + 8) != copysrc[i].actor) {
            copysrc[i].action = 0;
            if (now < copysrc[i].next_find_ns) continue;
            copysrc[i].next_find_ns = now + 5000000000ull;
            if (!(act = wp_find_ai(copysrc[i].actor, VT_GUARDIAN_AIM_BEAM))) continue;
            copysrc[i].action = act;
        }
        bool killed = false;
        if (elink_live_effect(act + AIMB_LASER_H)) beam_call1(c, A_FX_KILL_H, act + AIMB_LASER_H), killed = true;
        if (elink_live_effect(act + AIMB_TARGET_H)) beam_call1(c, A_FX_KILL_H, act + AIMB_TARGET_H), killed = true;
        if (killed) {
            beam_copy_kills++;
            static unsigned said;
            if (said++ < 20)
                LOG("[coop] beams: our copy's own laser (%llx) put out (%d so far)\n",
                    (unsigned long long)copysrc[i].actor, beam_copy_kills);
        }
    }
}

// ======== followers: a beam shot created here
static void beam_shot_show(Cpu *c, const CoopBeam *e) {
    float tgt[3];
    if (!beam_resolve(e, tgt)) {
        LOG("[coop] beams: a beam at player %d, who is not here: not shown\n", e->target);
        return;
    }
    float p[3] = {e->pos[0], e->pos[1], e->pos[2]};
    float d[3] = {tgt[0] - p[0], tgt[1] - p[1], tgt[2] - p[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 0.3f && len < 400.0f)) return;
    for (int k = 0; k < 3; k++) d[k] /= len;
    // start outside our copy of the Guardian (the beam has no shooter here and would burst on the copy's body)
    struct Enemy *foe = beam_enemy_by_hash(e->hash);
    float t0 = 0;
    if (foe && foe->actor) {
        float bc[3];
        beam_mpos(foe->actor, bc);
        bc[1] += 2.5f;
        float w[3] = {bc[0] - p[0], bc[1] - p[1], bc[2] - p[2]};
        float tc = w[0] * d[0] + w[1] * d[1] + w[2] * d[2], w2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
        float h2 = BEAM_COPY_R * BEAM_COPY_R - (w2 - tc * tc);  // (ray-sphere: exit at tc + sqrt(h2))
        if (h2 > 0) t0 = tc + sqrtf(h2);
        if (t0 < 0) t0 = 0;
        if (t0 > len - 1.5f) t0 = len - 1.5f > 0 ? len - 1.5f : 0;
    }
    float s[3] = {p[0] + d[0] * t0, p[1] + d[1] * t0, p[2] + d[2] * t0}, m[12];
    beam_basis(d, s, m);
    uint64_t a = spawn_mtx_now(c, beam_kind_names[e->kind], m);
    if (a) {
        beam_seen_ids[beam_seen_n++ % 32] = LD32(a + PROC_ID);
        beam_shots_shown++;
    }
    static unsigned said;
    if (said++ < 80)
        LOG("[coop] beams: %s of Guardian %u at player %d, %.1f m (start pushed %.1f m): %s\n", beam_kind_names[e->kind],
            e->hash, e->target, len - t0, t0, a ? "created" : "NOT created");
}

static void beam_ev(Cpu *c, const uint8_t *data, int len) {
    if (len != (int)sizeof(CoopBeam) || !beams_on()) return;
    CoopBeam e;
    memcpy(&e, data, sizeof e);
    if (!coop_beam_ok(&e)) {
        LOG("[coop] beams: bad event dropped\n");
        return;
    }
    char area[96];
    if (!local_in_world() || !player_info_ptr() || local_area(area, sizeof area) != e.area) return;
    struct Enemy *foe = beam_enemy_by_hash(e.hash);
    if (foe && foe->mine && !foe->following) return;  // (we run that Guardian: ours is the real one)
    if (e.what == COOP_BEAM_SHOT) {
        struct BeamLaserView *v = lview_for(e.hash, false);
        if (v) lview_stop(c, v, true), v->last_ns = 0, v->hash = 0;
        if (!beam_mem()) return;
        float me[3];
        drop_me(me);
        if (dist3(me, e.pos) > 250.0f) return;
        // (2026-11-21 security review M2) Each shown shot spawns an actor: rate-limit them so a flood of shot events
        // cannot pile up actors. Real Guardians fire every few seconds: at most one shot per Guardian per 0.4 s and
        // six per second in all.
        static struct { uint32_t hash; uint64_t ns; } last_shot[16];
        static uint64_t window_ns;
        static int window_n;
        uint64_t now = plat_now_ns();
        int slot = -1, oldest = 0;
        for (int i = 0; i < 16; i++) {
            if (last_shot[i].hash == e.hash) { slot = i; break; }
            if (last_shot[i].ns < last_shot[oldest].ns) oldest = i;
        }
        if (slot >= 0 && now - last_shot[slot].ns < 400000000ull) return;
        if (now - window_ns > 1000000000ull) window_ns = now, window_n = 0;
        if (window_n >= 6) return;
        window_n++;
        if (slot < 0) slot = oldest;
        last_shot[slot].hash = e.hash, last_shot[slot].ns = now;
        beam_shot_show(c, &e);
        return;
    }
    if (!beamlaser_on() || !beam_mem()) return;
    if (e.what == COOP_BEAM_AIM_END) {
        struct BeamLaserView *v = lview_for(e.hash, false);
        if (v) lview_stop(c, v, true), v->last_ns = 0, v->hash = 0;
        return;
    }
    if (!foe || !foe->actor || !foe->following) return;  // (no copy here to draw it from)
    struct BeamLaserView *v = lview_for(e.hash, true);
    if (!v) return;
    if (v->on && v->kind != e.kind) lview_stop(c, v, false), v->emitter = 0;
    v->kind = e.kind, v->target = e.target;
    memcpy(v->org_off, e.pos, 12), memcpy(v->aim, e.aim, 12);
    v->last_ns = plat_now_ns();
}

static void beams_frame(Cpu *c) {
    if (!beams_on()) return;
    uint64_t now = plat_now_ns();
    if (!local_in_world() || !player_info_ptr()) {
        // (scene teardown retires the effects: forget the handles)
        for (int i = 0; i < BEAM_LASERS; i++) {
            if (lview[i].h_laser) {
                memset((void *)(uintptr_t)lview[i].h_laser, 0, 16), memset((void *)(uintptr_t)lview[i].h_target, 0, 16);
                memset((void *)(uintptr_t)lview[i].h_snd, 0, 16);
            }
            lview[i].on = false, lview[i].last_ns = 0, lview[i].hash = 0, lview[i].emitter = 0;
        }
        memset(aimsrc, 0, sizeof aimsrc), memset(copysrc, 0, sizeof copysrc), memset(bflight, 0, sizeof bflight);
        return;
    }
    float me[3];
    drop_me(me);
    if (rav_n) beam_copies_quiet(c, me, now);
    if (beamlaser_on()) beam_lasers_view(c, now);
    if (!rav_n) return;  // (nobody else here: nothing to send)
    char text[96];
    struct BeamScan sc = {local_area(text, sizeof text), now};
    if (!sc.area) return;
    if (!beam_foes_near(me)) {
        for (int i = 0; i < BEAM_LASERS; i++) {
            if (aimsrc[i].on) {
                float z[3] = {0};
                beam_aim_send(&aimsrc[i], sc.area, COOP_BEAM_AIM_END, z, z, now);
            }
            memset(&aimsrc[i], 0, sizeof aimsrc[i]);
        }
        return;
    }
    beam_ring(beam_shot_seen, &sc);
    if (beamlaser_on()) beam_lasers_owner(sc.area, now, true);
}
