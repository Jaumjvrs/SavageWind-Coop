// Part of runtime/coop.c (included there, same translation unit): shrine rail lifts in step in both games.
//
// (2026-11-21, list item "shrine elevators"; probe in CDungeon/Dungeon034) Rail lifts (RailLift,
// RailLift_B, DgnObj_Hrl_RailLift_6x6_01) loop along their rail on their own from the moment their stage loads, so
// the two games run them out of step (measured: the guest's lift 6 m behind the host's): the partner riding a lift
// was drawn floating beside ours. The host's lifts lead: the host sends each one's rail state in its object list
// (CoopObject with COOP_OBJ_RAIL, objects.inc rails_fill), and the guest steers its own lift to it.
// Game facts (offline RE, 1.9.0): the AI class RailMoveObject (vtable
// RAIL_AI_VT, found through the AI manager at actor+0x558) holds +0x6c the rail coordinate the leg started from (or
// the point it waits at), +0x70 the leg's destination point, +0x74 the direction byte; its active child (+0x30 array,
// index u16 +0x1c, count +0x28) is MoveToTarget (cursor xyz at +0x30, advanced by RailMoveSpeed * delta each tick;
// the keyframed body follows it with a velocity, so riders are carried) or StopForLimitedTime (elapsed wait at +0x58).
// 0x7000a050f0(ai) enters a leg from +0x6c to +0x70; 0x7000a04d7c(ai) enters the wait at +0x6c.
// Wire: pos = the cursor (moving) or the lift's position (waiting); rot = {start coord, destination, direction,
// elapsed wait or -1 while moving}; vel = the cursor's velocity.
// Guest: same leg -> the cursor is nudged toward the host's (RAILSYNC_V m/s on top of its own speed; > 8 m: set);
// same wait -> the elapsed time copied; the host just left the point we wait at -> our leg starts now; any other
// difference -> our lift is put on the host's leg / wait (a jump, at most once per 2 s).
// SAVAGEWIND_COOP_RAILSYNC=0: off (each game runs its own lifts, as before).

#define RAIL_AI_VT 0x7001c839e0ull
#define RAIL_MOVE_VT 0x7001c12e30ull
#define RAIL_STOP_VT 0x7001c2ea98ull
#define A_RAIL_ENTER_MOVE 0x7000a050f0ull
#define A_RAIL_ENTER_STOP 0x7000a04d7cull
#define RAIL_MAX 8
#define RAIL_SYNC_V 2.0f
#define RAIL_SET_M 8.0f

static int railsync_c = -1;
static bool railsync_on(void) { return env_on("SAVAGEWIND_COOP_RAILSYNC", &railsync_c); }

static struct Rail {
    uint64_t actor, unit;
    uint32_t id, hash;
    float last_cur[3];
    uint64_t last_ns, hard_ns;
    bool have_last;
    int state_logged;  // (log: the last case seen, so each change is one line)
} rails[RAIL_MAX];
static int rail_n;
static uint64_t rail_scan_ns;
static uint32_t rail_area;

static bool rail_name(const char *n) {
    return !strcmp(n, "RailLift") || !strcmp(n, "RailLift_B") || !strcmp(n, "DgnObj_Hrl_RailLift_6x6_01");
}
static void rail_seen(uint64_t a, uint64_t u, const char *nm, void *ctx) {  // (ctx: the Cpu, for actor_hash)
    Cpu *c = ctx;
    if (!rail_name(nm) || rail_n >= RAIL_MAX) return;
    uint32_t id = LD32(a + PROC_ID);
    for (int i = 0; i < rail_n; i++)
        if (rails[i].actor == a && rails[i].id == id) return;
    struct Rail *r = &rails[rail_n++];
    memset(r, 0, sizeof *r);
    r->actor = a, r->unit = u, r->id = id, r->hash = actor_hash(c, a);
    LOG("[coop] rails: %s %u found (%llx)\n", nm, r->hash, (unsigned long long)a);
}
static bool rail_live(const struct Rail *r) { return beam_live(r->actor, r->unit, r->id); }

static uint64_t rail_ai(uint64_t actor) {
    uint64_t man = LD64(actor + 0x558);
    if (!in_guest(man) || !in_guest(man + 0x88)) return 0;
    uint32_t n = LD32(man + 0x78);
    uint64_t list = LD64(man + 0x80);
    for (uint32_t i = 0; i < n && i < 64 && in_guest(list + (uint64_t)i * 8); i++) {
        uint64_t ai = LD64(list + (uint64_t)i * 8);
        if (in_guest(ai) && in_guest(ai + 0x78) && LD64(ai) == RAIL_AI_VT && LD64(ai + 8) == actor) return ai;
    }
    return 0;
}
static uint64_t rail_child(uint64_t ai) {
    uint16_t i = LD16(ai + 0x1c);
    if (i >= LD32(ai + 0x28)) return 0;
    uint64_t slot = LD64(ai + 0x30) + (uint64_t)i * 8;
    uint64_t ch = in_guest(slot) ? LD64(slot) : 0;
    return in_guest(ch) && in_guest(ch + 0x60) ? ch : 0;
}

// Keep the list of loaded rail lifts (rescanned every second; emptied on a stage change).
static void rails_scan(Cpu *c, uint64_t now) {
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (here != rail_area || !local_in_world()) rail_n = 0, rail_area = here;
    for (int i = 0; i < rail_n; i++)
        if (!rail_live(&rails[i])) rails[i--] = rails[--rail_n];
    if (!local_in_world() || now - rail_scan_ns < 1000000000ull) return;
    rail_scan_ns = now;
    beam_ring(rail_seen, c);
}

// Host: our lifts' rail state into the object list (objects.inc objects_frame, before the physics objects).
static int rails_fill(Cpu *c, CoopObject *out, int max) {
    if (!railsync_on() || !coop_net_is_host()) return 0;
    uint64_t now = plat_now_ns();
    rails_scan(c, now);
    int n = 0;
    for (int i = 0; i < rail_n && n < max; i++) {
        struct Rail *r = &rails[i];
        uint64_t ai = rail_ai(r->actor), ch = ai ? rail_child(ai) : 0;
        if (!ch) continue;
        bool moving = LD64(ch) == RAIL_MOVE_VT;
        if (!moving && LD64(ch) != RAIL_STOP_VT) continue;
        CoopObject e = {.hash = r->hash, .flags = COOP_OBJ_RAIL};
        if (moving) {
            memcpy(e.pos, (const void *)(uintptr_t)(ch + 0x30), 12);
            double dt = r->have_last && now > r->last_ns ? (now - r->last_ns) / 1e9 : 0;
            if (dt > 0.005 && dt < 0.5)
                for (int j = 0; j < 3; j++) e.vel[j] = (float)((e.pos[j] - r->last_cur[j]) / dt);
        } else {
            const float *m = (const float *)(uintptr_t)(r->actor + ACTOR_MTX);
            e.pos[0] = m[3], e.pos[1] = m[7], e.pos[2] = m[11];
        }
        memcpy(r->last_cur, e.pos, 12), r->last_ns = now, r->have_last = moving;
        e.rot[0] = LDF(ai + 0x6c), e.rot[1] = LDF(ai + 0x70), e.rot[2] = (float)LD8(ai + 0x74);
        e.rot[3] = moving ? -1.0f : LDF(ch + 0x58);
        bool ok = true;
        for (int j = 0; j < 3; j++) ok = ok && isfinite(e.pos[j]) && isfinite(e.vel[j]) && fabsf(e.vel[j]) < 50;
        for (int j = 0; j < 4; j++) ok = ok && isfinite(e.rot[j]);
        if (ok) out[n++] = e;
    }
    return n;
}

static void rail_log(struct Rail *r, int st, const char *what, float a, float b) {
    if (r->state_logged == st) return;
    r->state_logged = st;
    LOG("[coop] rails: lift %u %s (%.2f, %.2f) (RAILSYNC)\n", r->hash, what, a, b);
}

// Guest: steer our lifts to the host's.
static void rails_frame(Cpu *c) {
    if (!railsync_on() || coop_net_is_host() || !coop_net_on()) return;
    uint64_t now = plat_now_ns();
    rails_scan(c, now);
    static uint64_t last_ns;
    float dt = last_ns && now > last_ns ? (float)((now - last_ns) / 1e9) : 0;
    last_ns = now;
    if (dt <= 0 || dt > 0.25f) return;
    static float corr_v = -1;
    if (corr_v < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_RAILSYNC_V");
        corr_v = e && atof(e) > 0 ? (float)atof(e) : RAIL_SYNC_V;
    }
    for (int i = 0; i < rail_n; i++) {
        struct Rail *r = &rails[i];
        CoopObject h = {0};
        double age;
        bool have = coop_net_remote_object(r->hash, &h, &age);
        uint64_t ai = rail_ai(r->actor), ch = ai ? rail_child(ai) : 0;
        if (!have || !(h.flags & COOP_OBJ_RAIL) || age > 0.5) continue;
        if (!ch) continue;
        // (2026-11-21 security review M1) The host's rail coordinates are written into our lift's AI: only accept
        // ones inside our own rail (point count at rail +0x18, rail at AI +0x60; the game's lookup 0x70010b5778 clamps
        // the index too) and a sane wait time, so a bad packet can only be ignored.
        uint64_t rail = LD64(ai + 0x60);
        float npts = rail ? (float)LD32(rail + 0x18) : 0;
        if (!(npts >= 1 && h.rot[0] >= 0 && h.rot[0] <= npts && h.rot[1] >= 0 && h.rot[1] <= npts &&
              h.rot[2] >= 0 && h.rot[2] <= 255 && h.rot[3] >= -1 && h.rot[3] <= 3600)) continue;
        bool hmove = h.rot[3] < 0, gmove = LD64(ch) == RAIL_MOVE_VT;
        if (!gmove && LD64(ch) != RAIL_STOP_VT) continue;
        const float *m = (const float *)(uintptr_t)(r->actor + ACTOR_MTX);
        float mine[3] = {m[3], m[7], m[11]};
        if (dist3(mine, h.pos) > 80) continue;  // (not the same rail: another stage's lift with this hash)
        float gc = LDF(ai + 0x6c), gd = LDF(ai + 0x70);
        uint8_t gdir = LD8(ai + 0x74), hdir = (uint8_t)h.rot[2];
        bool same_leg = fabsf(gd - h.rot[1]) < 0.01f && gdir == hdir;
        float target[3] = {h.pos[0] + h.vel[0] * (float)age, h.pos[1] + h.vel[1] * (float)age,
                           h.pos[2] + h.vel[2] * (float)age};
        if (gmove && !hmove && fabsf(gc - h.rot[0]) < 0.01f && dist3(mine, h.pos) < 1.5f) {
            // our lift left the point the host still waits at (a few frames early): held there until the host's goes
            memcpy((void *)(uintptr_t)(ch + 0x30), h.pos, 12);
            rail_log(r, 5, "held at the stop until the host's leaves", gc, h.rot[3]);
            continue;
        }
        if (gmove && (hmove ? same_leg && fabsf(gc - h.rot[0]) < 0.01f
                            : fabsf(gd - h.rot[0]) < 0.01f)) {
            // our leg is the host's (or ends where the host waits): move our cursor toward the host's lift
            float *cur = (float *)(uintptr_t)(ch + 0x30);
            float e[3] = {target[0] - cur[0], target[1] - cur[1], target[2] - cur[2]};
            float d = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            if (d > RAIL_SET_M) {
                memcpy(cur, target, 12);
                LOG("[coop] rails: lift %u was %.1f m off the host's: set (RAILSYNC)\n", r->hash, d);
            } else if (d > 0.02f) {
                float step = corr_v * dt;
                if (step > d) step = d;
                for (int j = 0; j < 3; j++) cur[j] += e[j] / d * step;
            }
            rail_log(r, d > 0.3f ? 1 : 2, d > 0.3f ? "catching up with the host's" : "in step with the host's", d, 0);
            continue;
        }
        if (!gmove && !hmove && fabsf(gc - h.rot[0]) < 0.01f && gdir == hdir) {  // the same wait: the host's clock
            float *el = (float *)(uintptr_t)(ch + 0x58);
            if (*el > h.rot[3] + 0.5f || *el < h.rot[3] - 3) *el = h.rot[3];  // (never ahead: ours must not leave first)
            rail_log(r, 3, "waiting with the host's", gc, h.rot[3]);
            continue;
        }
        if (!gmove && hmove && fabsf(gc - h.rot[0]) < 0.01f) {  // the host just left the point we wait at: go too
            STF(ai + 0x70, h.rot[1]), ST8(ai + 0x74, hdir);
            Cpu saved = *c;
            c->x[0] = ai;
            guest_call(c, A_RAIL_ENTER_MOVE);
            *c = saved;
            rail_log(r, 4, "leaves its stop with the host's", gc, h.rot[1]);
            continue;
        }
        if (!gmove && hmove && fabsf(gc - h.rot[1]) < 0.01f) {  // ours arrived first where the host's is going: wait
            float *el = (float *)(uintptr_t)(ch + 0x58);
            *el = 0;
            rail_log(r, 6, "arrived first: waits for the host's", gc, h.rot[1]);
            continue;
        }
        if (now - r->hard_ns < 2000000000ull) continue;
        r->hard_ns = now;
        // another leg / wait altogether: our lift onto the host's (one jump)
        STF(ai + 0x6c, h.rot[0]), ST8(ai + 0x74, hdir);
        Cpu saved = *c;
        c->x[0] = ai;
        if (hmove) {
            STF(ai + 0x70, h.rot[1]);
            guest_call(c, A_RAIL_ENTER_MOVE);
            uint64_t mv = rail_child(ai);  // (the leg starts at its start point: our cursor where the host's is now)
            if (mv && LD64(mv) == RAIL_MOVE_VT) memcpy((void *)(uintptr_t)(mv + 0x30), target, 12);
        } else {
            guest_call(c, A_RAIL_ENTER_STOP);
            uint64_t st = rail_child(ai);  // (entering the wait cleared its clock: the host's)
            if (st && LD64(st) == RAIL_STOP_VT) STF(st + 0x58, h.rot[3]);
        }
        *c = saved;
        r->state_logged = 0;
        LOG("[coop] rails: lift %u on another %s than the host's (ours %s %.2f -> %.2f dir %u, host %s %.2f -> %.2f "
            "dir %u): put on the host's (RAILSYNC)\n", r->hash, hmove ? "leg" : "stop", gmove ? "moving" : "waiting",
            gc, gd, gdir, hmove ? "moving" : "waiting", h.rot[0], h.rot[1], hdir);
    }
}
