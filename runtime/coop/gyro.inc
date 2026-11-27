// Part of runtime/coop.c (included there, same translation unit): motion-control shrines (2026-11-14, protocol 41).
// A motion-control platform is a DungeonRotateTagGyro* actor (also ...GyroPutter, ...GyroHammer: same AI program).
// Its AI action DungeonRotateGyro (vtable GYRO_ACTION_VT, AI +0x190) runs only while a player stands at its pedestal
// (SwitchGyroTilt): calc (0x7000634490) reads the controller (0x7000cf6e5c, the site pc_gyro_scene.c hooks), turns
// its rotation (action +0x74, 3x3 rows; rebuilt each frame from the cached columns +0x5c / +0x68) toward the
// controller's pose, and gives the shrine's merged parts the matrix [R | T - R T] (T = the tag's position, actor
// +0x3dc/+0x3ec/+0x3fc) through the scenery manager (A_SCENERY_SET with the actor's scenery key +0x5a8 and the
// action's index **(action +0x30)). Physics on the platform (the ball) follows from that.
// Sharing: one owner per platform - the player at its pedestal. The owner's game sends the applied rotation; the
// other game applies the same matrix through the same call (smoothed between packets); if that player stands at
// their own pedestal meanwhile, their mechanism's input is withheld (the hook nulls it) and it holds the owner's pose.
// When the owner lets go, the other player may take it (a newer claim). While the other player tilts a platform, our
// shared objects make no claims (objects.inc: the ball rolls in both games; the tilter's game owns it).
// SAVAGEWIND_COOP_GYRO=0 turns this off.
#include <pthread.h>
#include "pc_gyro_scene.h"

#define GYRO_ACTION_VT 0x7001be50a8ull  // DungeonRotateGyro (Ghidra FUN_7100634144 ctor; calc FUN_7100634490)
#define A_SCENERY_GOT 0x7001d66ab0ull   // **this + 0x158: the scenery (merged parts) manager
#define A_SCENERY_FIND 0x700130bbf0ull  // entry *f(mgr, u64 *key): count +0x28, matrices +0x30 (0x30 bytes each)
#define A_SCENERY_SET 0x700130bd60ull   // f(mgr, u64 *key, Matrix34f *, u32 index): what the action's calc calls
#define ACTOR_SCENERY_KEY 0x5a8
#define GYRO_MAX 6
static bool gyro_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_GYRO") && !strcmp(getenv("SAVAGEWIND_COOP_GYRO"), "0"));
    return on;
}

// ---- the hook (inside the game's actor update; guarded: it may run on another thread than the frame)
static pthread_mutex_t gyro_mu = PTHREAD_MUTEX_INITIALIZER;
static struct GyroHook { uint64_t actor, action, beat_ns; bool force; float r[9]; } gyro_hk[GYRO_MAX];
static struct GyroHook *gyro_hk_slot(uint64_t actor, uint64_t now, bool create) {  // (under gyro_mu)
    struct GyroHook *free_slot = NULL;
    for (int i = 0; i < GYRO_MAX; i++) {
        if (gyro_hk[i].actor == actor) return &gyro_hk[i];
        if (!free_slot && (!gyro_hk[i].actor || (!gyro_hk[i].force && now - gyro_hk[i].beat_ns > 5000000000ull)))
            free_slot = &gyro_hk[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->actor = actor;
    return free_slot;
}
static void gyro_hook(Cpu *c, uint64_t actor, uint64_t action, uint64_t control) {
    if (!gyro_on() || !in_guest(action) || !in_guest(action + 0x200) || LD64(action) != GYRO_ACTION_VT ||
        LD64(action + 8) != actor)
        return;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&gyro_mu);
    struct GyroHook *h = gyro_hk_slot(actor, now, true);
    if (h) {
        h->action = action;
        if (control) h->beat_ns = now;
        if (h->force && control) {
            // The other player's pose: into the rotation and the cached columns it is rebuilt from (calc's
            // FUN_7101546f60(+0x74, +0x68, +0x5c)), its turn speeds (the "Rotate" sound) to 0, and no input this frame:
            // calc then applies exactly this pose.
            for (int i = 0; i < 9; i++) STF(action + 0x74 + 4u * i, h->r[i]);
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++) STF(action + 0x50 + 12u * col + 4u * row, h->r[3 * row + col]);
            for (int i = 0; i < 6; i++) STF(action + 0x110 + 4u * i, 0.0f);
            c->x[0] = 0;
        }
    }
    pthread_mutex_unlock(&gyro_mu);
}
static uint64_t gyro_hk_beat(uint64_t actor) {
    pthread_mutex_lock(&gyro_mu);
    struct GyroHook *h = gyro_hk_slot(actor, plat_now_ns(), false);
    uint64_t b = h ? h->beat_ns : 0;
    pthread_mutex_unlock(&gyro_mu);
    return b;
}
static void gyro_hk_force(uint64_t actor, bool on, const float r[9]) {
    pthread_mutex_lock(&gyro_mu);
    struct GyroHook *h = gyro_hk_slot(actor, plat_now_ns(), on);
    if (h) {
        h->force = on;
        if (on) memcpy(h->r, r, sizeof h->r);
    }
    pthread_mutex_unlock(&gyro_mu);
}
static void gyro_hk_clear(void) {
    pthread_mutex_lock(&gyro_mu);
    memset(gyro_hk, 0, sizeof gyro_hk);
    pthread_mutex_unlock(&gyro_mu);
}

// ---- the platforms (game thread)
static struct Gyro {
    uint32_t hash, id;
    uint64_t actor, unit, action;
    int owner;                        // 0 nobody, 1 us (our player at its pedestal, or the last one there), 2 the other player
    uint16_t gen, top;                // the owner's claim; the newest generation seen
    bool driving;                     // (owner 1) our player is at its pedestal
    bool have_remote, remote_driving, have_cur, have_applied;
    uint64_t remote_ns, sent_ns;
    float remote[9], cur[9], applied[9], sent[9];  // their pose; ours on the way to it; what we applied; what we sent
} gyro[GYRO_MAX];
static int gyro_n;
static uint32_t gyro_area;
static uint64_t gyro_mtx;                    // guest scratch: a Matrix34f (reset on reboot)
static volatile int gyro_report;             // `coop gyro`
static struct { volatile int pending; float r[9]; uint64_t until_ns; } gyro_dev;  // `coop gyroset <x deg> <z deg>`

static bool gyro_name(const char *n) { return !strncmp(n, "DungeonRotateTagGyro", 20); }
static float gyro_diff(const float a[9], const float b[9]) {
    float d = 0;
    for (int i = 0; i < 9; i++) d = fmaxf(d, fabsf(a[i] - b[i]));
    return d;
}
static void gyro_ortho(float r[9]) {  // rows orthonormal, right-handed
    float *x = r, *y = r + 3, *z = r + 6;
    float l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (!(l > 1e-6f)) { static const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(r, id, sizeof id); return; }
    for (int k = 0; k < 3; k++) x[k] /= l;
    float d = y[0] * x[0] + y[1] * x[1] + y[2] * x[2];
    for (int k = 0; k < 3; k++) y[k] -= d * x[k];
    l = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!(l > 1e-6f)) { static const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(r, id, sizeof id); return; }
    for (int k = 0; k < 3; k++) y[k] /= l;
    z[0] = x[1] * y[2] - x[2] * y[1], z[1] = x[2] * y[0] - x[0] * y[2], z[2] = x[0] * y[1] - x[1] * y[0];
}
static uint64_t gyro_mgr(void) {
    uint64_t got = LD64(A_SCENERY_GOT), inst = in_guest(got) ? LD64(got) : 0, mgr = in_guest(inst) ? LD64(inst + 0x158) : 0;
    return in_guest(mgr) ? mgr : 0;
}
static uint32_t gyro_index(uint64_t action) {
    uint64_t p = LD64(action + 0x30);
    uint32_t i = in_guest(p) ? LD32(p) : 0;
    return i < 64 ? i : 0;
}
static bool gyro_alive(const struct Gyro *g) {
    return g->actor && in_guest(g->unit) && LD32(g->unit + UNIT_ID) == g->id && LD64(g->unit + UNIT_ACTOR) == g->actor &&
           LD8(g->actor + PROC_STATE) != 3 && in_guest(g->action) && LD64(g->action) == GYRO_ACTION_VT &&
           LD64(g->action + 8) == g->actor && LD64(g->actor + ACTOR_SCENERY_KEY);
}
// The rotation the scenery manager gives the platform now (rows of the 3x3).
static bool gyro_read_scenery(Cpu *c, const struct Gyro *g, float r[9]) {
    uint64_t mgr = gyro_mgr(), e = 0;
    if (mgr) {
        Cpu saved = *c;
        c->x[0] = mgr; c->x[1] = g->actor + ACTOR_SCENERY_KEY;
        guest_call(c, A_SCENERY_FIND);
        e = c->x[0];
        *c = saved;
    }
    float m[12];
    if (in_guest(e) && in_guest(e + 0x40)) {
        uint32_t n = LD32(e + 0x28), i = gyro_index(g->action);
        uint64_t mats = LD64(e + 0x30);
        if (i >= n) i = 0;
        uint64_t mat = mats + 0x30ull * i;
        if (n && n < 1024 && in_guest(mats) && in_guest(mat + sizeof m - 1)) {
            memcpy(m, (const void *)(uintptr_t)mat, sizeof m);
            float t[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
            bool ok = true;
            for (int k = 0; k < 9; k++) ok = ok && isfinite(t[k]) && fabsf(t[k]) <= 1.01f;
            if (ok) { memcpy(r, t, sizeof t); return true; }
        }
    }
    return false;
}
static bool gyro_read(Cpu *c, const struct Gyro *g, float r[9]) {
    if (gyro_read_scenery(c, g, r)) return true;
    for (int k = 0; k < 9; k++) {
        r[k] = LDF(g->action + 0x74 + 4u * k);
        if (!isfinite(r[k]) || fabsf(r[k]) > 1.01f) return false;
    }
    return true;
}
// Give the platform rotation r the way the action's calc does: [r | T - r T] through the scenery manager.
static bool gyro_apply(Cpu *c, const struct Gyro *g, const float r[9]) {
    uint64_t mgr = gyro_mgr(), a = g->actor;
    if (!mgr) return false;
    if (!gyro_mtx) gyro_mtx = (uint64_t)(uintptr_t)hle_alloc(0x40);
    if (!gyro_mtx) return false;
    float t[3] = {LDF(a + 0x3dc), LDF(a + 0x3ec), LDF(a + 0x3fc)}, m[12];
    for (int row = 0; row < 3; row++) {
        const float *q = r + 3 * row;
        m[4 * row + 0] = q[0], m[4 * row + 1] = q[1], m[4 * row + 2] = q[2];
        m[4 * row + 3] = t[row] - (q[0] * t[0] + q[1] * t[1] + q[2] * t[2]);
    }
    memcpy((void *)(uintptr_t)gyro_mtx, m, sizeof m);
    Cpu saved = *c;
    c->x[0] = mgr; c->x[1] = a + ACTOR_SCENERY_KEY; c->x[2] = gyro_mtx; c->x[3] = gyro_index(g->action);
    guest_call(c, A_SCENERY_SET);
    *c = saved;
    return true;
}
static struct Gyro *gyro_find(uint32_t hash, bool create) {
    for (int i = 0; i < gyro_n; i++)
        if (gyro[i].hash == hash) return &gyro[i];
    if (!create || gyro_n == GYRO_MAX) return NULL;
    struct Gyro *g = &gyro[gyro_n++];
    memset(g, 0, sizeof *g);
    g->hash = hash;
    return g;
}
// Dungeon039 has one maze and a dedicated ball. These HashIds belong to the 1.9.0 shrine.
static bool gyro_maze_ball(uint32_t hash, bool *driving) {
    if (hash != 1068887642u || !gyro_on()) return false;
    const struct Gyro *g = gyro_find(3281110075u, false);
    if (!g || !gyro_alive(g)) return false;
    *driving = g->owner == 1 && g->driving;
    return true;
}
static void gyro_add(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)ctx;
    uint64_t ai = LD64(a + 0x558), act = in_guest(ai) && in_guest(ai + 0x400) ? LD64(ai + 0x190) : 0;
    if (!in_guest(act) || LD64(act) != GYRO_ACTION_VT || LD64(act + 8) != a) {  // (not at +0x190: look around the AI)
        act = 0;
        for (uint32_t off = 0; in_guest(ai) && in_guest(ai + 0x400) && off < 0x400 && !act; off += 8) {
            uint64_t p = LD64(ai + off);
            if (in_guest(p) && !(p & 7) && in_guest(p + 0x200) && LD64(p) == GYRO_ACTION_VT && LD64(p + 8) == a) act = p;
        }
    }
    uint32_t h = act ? actor_hash(c, a) : 0;
    if (!h) return;
    struct Gyro *g = gyro_find(h, true);
    if (!g) return;
    bool fresh = g->actor != a;
    g->actor = a, g->action = act, g->unit = LD64(a + PROC_UNIT), g->id = LD32(g->unit + UNIT_ID);
    if (fresh)
        LOG("[coop] gyro: platform %s (hash %u) action %llx scenery key %llx index %u\n", name, h,
            (unsigned long long)act, (unsigned long long)LD64(a + ACTOR_SCENERY_KEY), gyro_index(act));
}
static void gyro_send(struct Gyro *g, const float r[9], uint8_t flags, uint64_t now) {
    CoopGyro e;
    memset(&e, 0, sizeof e);
    e.hash = g->hash, e.area = gyro_area, e.gen = g->gen, e.flags = flags;
    memcpy(e.rot, r, sizeof e.rot);
    if (!coop_gyro_ok(&e)) return;
    if (coop_net_send_event(COOP_EV_GYRO, &e, sizeof e)) memcpy(g->sent, r, sizeof g->sent), g->sent_ns = now;
}
// While the other player tilts a platform here (or ours still swings to their pose): objects.inc makes no claims.
static bool gyro_partner_tilting(void) {
    if (!gyro_on()) return false;
    uint64_t now = plat_now_ns();
    for (int i = 0; i < gyro_n; i++) {
        const struct Gyro *g = &gyro[i];
        if (g->owner == 2 && g->have_remote && now - g->remote_ns < 2000000000ull &&
            (g->remote_driving || !g->have_cur || gyro_diff(g->cur, g->remote) > 1e-4f))
            return true;
    }
    return false;
}

static void gyro_frame(Cpu *c) {
    static bool hooked;
    if (!gyro_on()) return;
    if (!hooked) hooked = true, pc_gyro_scene_coop = gyro_hook;
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    bool in_shrine = !strncmp(area, "CDungeon", 8) && local_in_world();
    if (here != gyro_area || !in_shrine) {
        if (gyro_n) LOG("[coop] gyro: %d platforms forgotten (area change or load)\n", gyro_n);
        gyro_n = 0, gyro_area = here;
        gyro_hk_clear();
        return;
    }
    CoopState rs;
    double rage = 99;
    bool together = coop_net_get_remote(&rs, &rage) && rage < 3.0 && (rs.flags & COOP_FLAG_IN_WORLD) && rs.area == here;
    // (n6, MULTIGYRO) More than two players: any world player in this shrine counts (their tilts arrive as relayed
    // events from that player; the claim generations pick one owner). Two players: the partner rule above, as before.
    static int multigyro_c = -1;
    if (!together && world_others() && player_info_ptr() && env_on("SAVAGEWIND_COOP_MULTIGYRO", &multigyro_c)) {
        struct ObjNear nr;
        const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
        obj_near_players(&nr, here, true, false, me);
        together = nr.n > 0;
    }
    if (gyro_dev.until_ns && now < gyro_dev.until_ns) together = true;  // (`coop gyroset`: as if the other player tilted)
    static uint64_t scan_ns;
    if (now >= scan_ns) {
        scan_ns = now + 2000000000ull;
        walk_actors(c, gyro_name, gyro_add, NULL);
    }
    if (gyro_dev.pending) {
        gyro_dev.pending = 0;
        for (int i = 0; i < gyro_n; i++) {
            struct Gyro *g = &gyro[i];
            g->owner = 2, g->have_remote = true, g->remote_driving = true, g->remote_ns = now;
            memcpy(g->remote, gyro_dev.r, sizeof g->remote);
        }
        LOG("[coop] gyro: dev pose set on %d platforms\n", gyro_n);
    }
    if (gyro_report) {
        gyro_report = 0;
        for (int i = 0; i < gyro_n; i++) {
            struct Gyro *g = &gyro[i];
            float r[9] = {0};
            bool alive = gyro_alive(g);
            bool ok = alive && gyro_read_scenery(c, g, r);
            uint64_t b = gyro_hk_beat(g->actor);
            LOG("[coop] gyro %u: actor %llx alive %d owner %d gen %u driving %d remote %d(%s, %.1f s) pedestal %s "
                "rot %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f (%s)\n", g->hash, (unsigned long long)g->actor,
                alive, g->owner, g->gen, g->driving, g->have_remote, g->remote_driving ? "tilting" : "still",
                g->have_remote ? (double)(now - g->remote_ns) / 1e9 : -1.0,
                b && now - b < 250000000ull ? "ours" : "-", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8],
                ok ? "scenery" : "unreadable");
        }
        LOG("[coop] gyro: %d platforms, together %d\n", gyro_n, together);
    }
    for (int i = 0; i < gyro_n; i++) {
        struct Gyro *g = &gyro[i];
        if (!gyro_alive(g)) {
            if (g->actor) gyro_hk_force(g->actor, false, NULL);
            continue;
        }
        if (!together) {  // (nothing is shared while apart; their pose comes again with their next packet)
            if (g->owner) LOG("[coop] gyro: platform %u nobody's (not together)\n", g->hash);
            g->owner = 0, g->driving = false, g->have_remote = false, g->have_cur = false, g->have_applied = false;
            gyro_hk_force(g->actor, false, NULL);
            continue;
        }
        uint64_t beat = gyro_hk_beat(g->actor);
        bool at_pedestal = beat && now - beat < 250000000ull;
        bool remote_drive = g->have_remote && g->remote_driving && now - g->remote_ns < 1500000000ull;
        if (at_pedestal && g->owner != 1 && !(g->owner == 2 && remote_drive)) {
            g->gen = g->top = (uint16_t)((g->top > g->gen ? g->top : g->gen) + 1);
            LOG("[coop] gyro: platform %u tilted here: ours (claim %u%s)\n", g->hash, g->gen,
                g->owner == 2 ? ", the other player let go of it" : "");
            g->owner = 1, g->driving = true, g->sent_ns = 0, g->have_remote = false, g->have_cur = false;
            gyro_hk_force(g->actor, false, NULL);
        }
        if (g->owner == 1) {
            float r[9];
            if (!gyro_read(c, g, r)) continue;
            uint64_t since = now - g->sent_ns;
            float d = gyro_diff(r, g->sent);
            if (g->driving && !at_pedestal) {
                g->driving = false;
                gyro_send(g, r, 0, now);
                LOG("[coop] gyro: platform %u let go here (still ours until the other player tilts it)\n", g->hash);
            } else if (at_pedestal) {
                g->driving = true;
                if (!g->sent_ns || (d > 1e-4f && since >= 50000000ull) || since >= 500000000ull)
                    gyro_send(g, r, COOP_GYRO_DRIVING, now);
            } else if (!g->sent_ns || (d > 1e-4f && since >= 200000000ull) || since >= 3000000000ull) {
                gyro_send(g, r, 0, now);  // (after the pedestal: a pose the game still changes, and a reminder)
            }
        } else if (g->owner == 2 && g->have_remote) {
            if (!g->have_cur) {  // (start from where ours is: a matching platform is left alone)
                if (!gyro_read(c, g, g->cur)) memcpy(g->cur, g->remote, sizeof g->cur);
                memcpy(g->applied, g->cur, sizeof g->applied);
                g->have_cur = g->have_applied = true;
            }
            if (gyro_diff(g->cur, g->remote) <= 2e-4f) {
                memcpy(g->cur, g->remote, sizeof g->cur);
            } else {  // (packets come 20 times a second at most: half the way each frame)
                for (int k = 0; k < 9; k++) g->cur[k] += 0.5f * (g->remote[k] - g->cur[k]);
                gyro_ortho(g->cur);
            }
            if (at_pedestal) {  // (our mechanism runs: it applies their pose itself, its input withheld)
                gyro_hk_force(g->actor, true, g->cur);
                memcpy(g->applied, g->cur, sizeof g->applied);
            } else {
                gyro_hk_force(g->actor, false, NULL);
                if (!g->have_applied || gyro_diff(g->cur, g->applied) > 1e-6f) {
                    if (gyro_apply(c, g, g->cur)) memcpy(g->applied, g->cur, sizeof g->applied), g->have_applied = true;
                }
            }
        }
    }
}
// COOP_EV_GYRO: the other player's platform pose.
static void gyro_ev(const uint8_t *data, int len) {
    if (len != (int)sizeof(CoopGyro) || !gyro_on()) return;
    CoopGyro e;
    memcpy(&e, data, sizeof e);
    if (!coop_gyro_ok(&e) || !gyro_area || e.area != gyro_area) return;
    struct Gyro *g = gyro_find(e.hash, true);
    if (!g) return;
    int16_t dg = (int16_t)(e.gen - g->gen);
    if ((int16_t)(e.gen - g->top) > 0) g->top = e.gen;
    bool host = coop_net_is_host();
    bool take = g->owner == 1 ? (dg > 0 || (dg == 0 && !host)) : (g->owner == 0 || dg >= 0);
    if (!take) return;
    if (g->owner != 2 || g->gen != e.gen)
        LOG("[coop] gyro: platform %u is the other player's (claim %u, %s)%s\n", e.hash, e.gen,
            (e.flags & COOP_GYRO_DRIVING) ? "tilting" : "let go", g->driving ? " - our player's tilt stops" : "");
    else if (g->remote_driving && !(e.flags & COOP_GYRO_DRIVING))
        LOG("[coop] gyro: platform %u let go by the other player\n", e.hash);
    g->owner = 2, g->gen = e.gen, g->driving = false;
    memcpy(g->remote, e.rot, sizeof g->remote);
    gyro_ortho(g->remote);
    g->have_remote = true, g->remote_driving = (e.flags & COOP_GYRO_DRIVING) != 0, g->remote_ns = plat_now_ns();
}
