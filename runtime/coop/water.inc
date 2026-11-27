// Part of runtime/coop.c (included there, same translation unit): shrine water wheels / capstans (SwitchWheel),
// dynamic water / lava levels (WaterSurface), and timed switches / timed gates (SwitchTimer, SwitchTimeLimited,
// SwitchTimeLag). (2026-11-19, worker water; protocol 86, reliable event 28 COOP_EV_WATERPUZ; audit item S11)
//
// Research (RomFS 120-shrine map survey + 1.9.0 binary / Ghidra RE; details omitted):
// - Water wheels & capstans (SwitchWaterWheel_A in Dungeon026, DgnObj_SwitchCapstan_A in Dungeon023,
//   DgnObj_SwitchCapstan_B_01 in DLC) run AI SwitchWheel (vtable 0x7001c95368). SwitchWheel::calc_ reads the rigid
//   body's angular velocity via A_RB_GET_ANGVEL (0x70012d4cc8), transforms it into initial local space via the inverse
//   3x3 matrix at ai+0x68, picks component *(int*)(ai+0x60) (RotAxis), and switches child to "回転" / "逆回転" / "待機"
//   when crossing RotateStartRad / RotateEndRad (emitting BasicSig or AxisY / -AxisY and playing "Roll").
//   Sync: one owner (whoever spins the wheel) sends pose + world angular velocity; the follower updates its rigid body
//   with A_RB_SET_ANGVEL (0x70012d32a8) and drift-corrected rb_set_transform, so the follower's own SwitchWheel AI
//   naturally turns the wheel, plays "Roll", and fires the linked signals (e.g. SwitchTimeLag -> shutter gate).
// - Dynamic water / lava surfaces (DgnObj_DungeonWater50x50_A_01 in Dungeon023 & Dungeon033; static surfaces have no
//   incoming link and are skipped) run AI WaterSurface (vtable 0x7001c9caa0): base_pos at ai+0x68, RiseLength at
//   *(float**)(ai+0x80), RiseSpeed at *(float**)(ai+0x88), moving via WaterSurfaceMove -> A_RB_MOVE_KEYFRAMED
//   (0x70012d4e18). Sync: owner sends (y, target_y, speed, raised, moving); follower aligns height via
//   A_RB_MOVE_KEYFRAMED / rb_set_transform when its incoming signal agrees (no fighting WaterSurface AI).
// - Timed switches: SwitchTimer (periodic oscillator, vtable 0x7001c95128), SwitchTimeLimited (retriggerable hold
//   countdown, vtable 0x7001c95008), SwitchTimeLag (rising-edge delay, vtable 0x7001c94ee8). All three store
//   float *WaitTime at ai+0x38 (in 30fps frames) and float elapsed at ai+0x40, and change state via A_AI_CHANGE_CHILD
//   (0x70015375d4) to "オン" (0x70015f88fa) / "オフ" (0x700164f567).
//   Sync: host owns periodic SwitchTimer phase; whoever triggers SwitchTimeLimited / SwitchTimeLag owns it; followers
//   align child state and elapsed (age-compensated at 30 ticks/s) without fighting gated inputs.
// SAVAGEWIND_COOP_WATERPUZ=0 turns all of this off.
// Dev: `coop water` lists tracked wheels, water surfaces, timers, and nearby shutters;
//      `coop water spin <hash> <rad_s> [secs]` spins a wheel; `coop water trigger <hash> [0|1]` flips a timer;
//      `coop water level <hash> <y>` nudges a water surface.

#define WP_VT_WHEEL       0x7001c95358ull  // SwitchWheel (ctor 0x7000a89484)
#define WP_VT_WATER       0x7001c9ca90ull  // WaterSurface (ctor 0x7000ab81fc)
#define WP_VT_TIMELAG     0x7001c94ed8ull  // SwitchTimeLag (ctor 0x7000a887d0)
#define WP_VT_TIMELIMITED 0x7001c94ff8ull  // SwitchTimeLimited (ctor 0x7000a88a54)
#define WP_VT_TIMER       0x7001c95118ull  // SwitchTimer (ctor 0x7000a88cec)

#define A_RB_GET_ANGVEL     0x70012d4cc8ull  // void f(rb, float out[3])
#define A_RB_SET_ANGVEL     0x70012d32a8ull  // f(rb, const float v[3])
#define A_RB_MOVE_KEYFRAMED 0x70012d4e18ull  // f(rb, const float mtx[12])
#define A_AI_CHANGE_CHILD   0x70015375d4ull  // void f(ai, const char *child_name, void *params)
#define A_AI_IS_CHILD       0x7001537800ull  // bool f(ai, SafeString *name)
#define A_LINK_IS_BASIC_ON  0x700120f8f0ull  // bool f(actor)
#define A_LINK_HAS_BASIC    0x700120f910ull  // bool f(actor)

#define WP_STR_ON  0x70015f88faull  // "オン"
#define WP_STR_OFF 0x700164f567ull  // "オフ"

#define WP_WHEEL_MAX 8
#define WP_WATER_MAX 8
#define WP_TIMER_MAX 32
#define WP_PEND_MAX  32

static bool waterpuz_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_WATERPUZ");
    return on;
}

static bool wp_code_ok(void) {
    static int ok = -1;
    if (ok < 0) {
        ok = LD32(A_RB_GET_ANGVEL) == 0xaa0003e8u &&
             LD32(A_RB_SET_ANGVEL) == 0x2d400821u &&
             LD32(A_RB_MOVE_KEYFRAMED) == 0xd10343ffu &&
             LD32(A_AI_CHANGE_CHILD) == 0xd100c3ffu &&
             LD32(A_AI_IS_CHILD) == 0xa9be7bfdu &&
             LD32(A_LINK_IS_BASIC_ON) == 0x2a1f03e1u &&
             LD32(A_LINK_HAS_BASIC) == 0xa9bf7bfdu;
        if (!ok) LOG("[coop] water: 1.9.0 instruction check failed; water puzzle sync disabled\n");
    }
    return ok != 0;
}

static uint64_t wp_mem;  // guest scratch: 0x00 Matrix34f (48 B), 0x40 Vec3f (12 B); reset in coop_reset

static bool wp_ensure_mem(void) {
    if (!wp_mem) wp_mem = (uint64_t)(uintptr_t)hle_alloc(0x80);
    return wp_mem != 0;
}

static bool wp_get_angvel(Cpu *c, uint64_t rb, float out[3]) {
    if (!in_guest(rb) || !wp_ensure_mem()) { out[0] = out[1] = out[2] = 0; return false; }
    memset((void *)(uintptr_t)(wp_mem + 0x40), 0, 12);
    Cpu saved = *c;
    c->x[0] = rb; c->x[1] = wp_mem + 0x40;
    guest_call(c, A_RB_GET_ANGVEL);
    *c = saved;
    memcpy(out, (const void *)(uintptr_t)(wp_mem + 0x40), 12);
    for (int i = 0; i < 3; i++)
        if (!isfinite(out[i])) { out[0] = out[1] = out[2] = 0; return false; }
    return true;
}

static void wp_unfix_rb(Cpu *c, uint64_t rb) {
    if (!in_guest(rb)) return;
    if (LD32(rb + 0x68) & 0x80000u) {
        Cpu saved = *c;
        c->x[0] = rb; c->x[1] = 0; c->x[2] = 0;
        guest_call(c, A_RB_SET_FIXED);
        *c = saved;
    }
}

static bool wp_set_angvel(Cpu *c, uint64_t rb, const float v[3]) {
    if (!in_guest(rb) || !in_guest(LD64(rb + 0xb8)) || !wp_ensure_mem()) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(v[i]) || fabsf(v[i]) > 50.0f) return false;
    wp_unfix_rb(c, rb);
    memcpy((void *)(uintptr_t)(wp_mem + 0x40), v, 12);
    Cpu saved = *c;
    c->x[0] = rb; c->x[1] = wp_mem + 0x40;
    guest_call(c, A_RB_SET_ANGVEL);
    *c = saved;
    return true;
}

static void wp_move_keyframed(Cpu *c, uint64_t rb, const float m[12]) {
    if (!in_guest(rb) || !wp_ensure_mem()) return;
    memcpy((void *)(uintptr_t)wp_mem, m, 48);
    Cpu saved = *c;
    c->x[0] = rb; c->x[1] = wp_mem;
    guest_call(c, A_RB_MOVE_KEYFRAMED);
    *c = saved;
}

static void wp_ai_change_child(Cpu *c, uint64_t ai, uint64_t str_addr) {
    if (!in_guest(ai)) return;
    Cpu saved = *c;
    c->x[0] = ai; c->x[1] = str_addr; c->x[2] = 0;
    guest_call(c, A_AI_CHANGE_CHILD);
    *c = saved;
}

static bool wp_link_has_basic(Cpu *c, uint64_t actor) {
    if (!in_guest(actor)) return false;
    Cpu saved = *c;
    c->x[0] = actor;
    guest_call(c, A_LINK_HAS_BASIC);
    bool r = (c->x[0] & 1) != 0;
    *c = saved;
    return r;
}

static bool wp_link_is_basic_on(Cpu *c, uint64_t actor) {
    if (!in_guest(actor)) return false;
    Cpu saved = *c;
    c->x[0] = actor;
    guest_call(c, A_LINK_IS_BASIC_ON);
    bool r = (c->x[0] & 1) != 0;
    *c = saved;
    return r;
}

// Find an AI object owned by `actor` with vtable `vt` reachable from actor+0x558 (AIController).
static uint64_t wp_find_ai(uint64_t actor, uint64_t vt) {
    uint64_t ctrl = LD64(actor + 0x558);
    if (!in_guest(ctrl) || (ctrl & 7) || !in_guest(ctrl + 0x300)) return 0;
    if (LD64(ctrl) == vt && LD64(ctrl + 8) == actor) return ctrl;
    for (uint32_t o = 0; o < 0x300; o += 8) {
        uint64_t p = LD64(ctrl + o);
        if (!in_guest(p) || (p & 7) || !in_guest(p + 0x48)) continue;
        if (LD64(p) == vt && LD64(p + 8) == actor) return p;
        for (uint32_t o2 = 0; o2 < 0x200 && in_guest(p + o2 + 8); o2 += 8) {
            uint64_t q = LD64(p + o2);
            if (!in_guest(q) || (q & 7) || !in_guest(q + 0x48)) continue;
            if (LD64(q) == vt && LD64(q + 8) == actor) return q;
            for (uint32_t o3 = 0; o3 < 0x100 && in_guest(q + o3 + 8); o3 += 8) {
                uint64_t r = LD64(q + o3);
                if (in_guest(r) && !(r & 7) && in_guest(r + 0x48) && LD64(r) == vt && LD64(r + 8) == actor) return r;
            }
        }
    }
    return 0;
}

static bool wp_quat_norm(float q[4]) {
    float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!isfinite(n2) || n2 < 0.25f) return false;
    float inv = 1.0f / sqrtf(n2);
    for (int i = 0; i < 4; i++) q[i] *= inv;
    return true;
}

static float wp_quat_absdot(const float a[4], const float b[4]) {
    return fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
}

// Extrapolate unit quaternion q by world angular velocity w (rad/s) over dt seconds: out = exp(0.5*w*dt) * q.
static void wp_quat_extrap(const float q[4], const float w[3], float dt, float out[4]) {
    float spd = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (spd < 1e-4f || dt <= 0.0f) { memcpy(out, q, 16); return; }
    float half = 0.5f * spd * dt;
    float s = sinf(half) / spd, dw = cosf(half);
    float dx = w[0] * s, dy = w[1] * s, dz = w[2] * s;
    out[0] = dw * q[0] + dx * q[3] + dy * q[2] - dz * q[1];
    out[1] = dw * q[1] - dx * q[2] + dy * q[3] + dz * q[0];
    out[2] = dw * q[2] + dx * q[1] - dy * q[0] + dz * q[3];
    out[3] = dw * q[3] - dx * q[0] - dy * q[1] - dz * q[2];
    if (!wp_quat_norm(out)) memcpy(out, q, 16);
}

// ---- tracked state (reset on area change or reboot)
struct WpWheel {
    uint32_t hash, unit_id;
    uint64_t actor, unit, rb, body, ai;
    char name[40];
    int rot_axis;
    int owner;              // 0 nobody, 1 us, 2 remote
    uint16_t gen, top;
    uint8_t claim_frames;
    bool spinning_sent, have_remote, remote_active, remote_fresh, matched;
    float rest_q[4];
    float remote_pos[3], remote_rot[4], remote_angvel[3];
    uint64_t found_ns, changed_ns, sent_ns, remote_ns, ai_try_ns;
    uint64_t dev_spin_until_ns;
    float dev_spin_rad;
};

struct WpWater {
    uint32_t hash, unit_id;
    uint64_t actor, unit, rb, body, ai;
    char name[48];
    float base_y, rise_len, rise_spd;
    int owner;              // 0 nobody, 1 us, 2 remote
    uint16_t gen, top;
    bool last_raised, last_moving, have_remote, remote_raised, remote_moving;
    float remote_y, remote_target_y, remote_speed;
    uint64_t found_ns, changed_ns, sent_ns, remote_ns;
};

struct WpTimer {
    uint32_t hash, unit_id;
    uint64_t actor, unit, ai;
    char name[32];
    uint8_t kind;           // COOP_WP_TIMER / COOP_WP_LIMITED / COOP_WP_TIMELAG
    int owner;              // 0 nobody, 1 us, 2 remote
    uint16_t gen, top;
    bool last_on, last_in_on, have_remote, remote_on;
    float wait_time, last_elapsed, remote_elapsed, remote_wait;
    uint64_t found_ns, changed_ns, sent_ns, remote_ns, applied_ns, ai_try_ns;
};

static struct WpWheel wp_wheels[WP_WHEEL_MAX];
static int wp_wheel_n;
static struct WpWater wp_waters[WP_WATER_MAX];
static int wp_water_n;
static struct WpTimer wp_timers[WP_TIMER_MAX];
static int wp_timer_n;
static uint32_t wp_area;
static bool wp_probe;  // (`coop water` was used: scan even when alone)

// Pending events arriving before the actor is discovered in this shrine
static struct { CoopWaterPuz e; uint64_t at_ns; } wp_pend[WP_PEND_MAX];

static struct {
    volatile int list_req;
    volatile int spin_req;
    uint32_t spin_hash;
    float spin_rad, spin_secs;
    volatile int trig_req;
    uint32_t trig_hash;
    int trig_val;  // -1 toggle, 0 off, 1 on
    volatile int level_req;
    uint32_t level_hash;
    float level_y;
} wp_dev;

static void waterpuz_reset(void) {
    wp_mem = 0;
    wp_wheel_n = wp_water_n = wp_timer_n = 0;
    wp_area = 0;
    wp_probe = false;
    memset(wp_pend, 0, sizeof wp_pend);
    memset(&wp_dev, 0, sizeof wp_dev);
}

static bool wp_actor_alive(uint64_t actor, uint64_t unit, uint32_t unit_id) {
    return in_guest(actor) && in_guest(unit) && LD32(unit + UNIT_ID) == unit_id &&
           LD64(unit + UNIT_ACTOR) == actor && LD8(actor + PROC_STATE) != 3;
}

static bool wp_wheel_alive(const struct WpWheel *w) {
    return wp_actor_alive(w->actor, w->unit, w->unit_id) && in_guest(w->rb) && LD64(w->rb + 0x78) == w->body;
}

static bool wp_water_alive(const struct WpWater *w) {
    return wp_actor_alive(w->actor, w->unit, w->unit_id) && in_guest(w->rb) && LD64(w->rb + 0x78) == w->body &&
           in_guest(w->ai) && LD64(w->ai) == WP_VT_WATER && LD64(w->ai + 8) == w->actor;
}

static bool wp_timer_alive(const struct WpTimer *t) {
    return wp_actor_alive(t->actor, t->unit, t->unit_id);
}

static uint64_t wp_timer_vt(uint8_t kind) {
    if (kind == COOP_WP_TIMER) return WP_VT_TIMER;
    if (kind == COOP_WP_LIMITED) return WP_VT_TIMELIMITED;
    return WP_VT_TIMELAG;
}

static bool wp_wheel_name(const char *n) {
    return !strncmp(n, "SwitchWaterWheel", 16) || !strncmp(n, "DgnObj_SwitchCapstan_", 21);
}

static bool wp_water_name(const char *n) {
    return !strncmp(n, "DgnObj_DungeonWater", 19) || !strncmp(n, "DgnObj_DungeonLava", 18);
}

static uint8_t wp_timer_kind(const char *n) {
    if (!strcmp(n, "SwitchTimer")) return COOP_WP_TIMER;
    if (!strcmp(n, "SwitchTimeLimited")) return COOP_WP_LIMITED;
    if (!strcmp(n, "SwitchTimeLag")) return COOP_WP_TIMELAG;
    return 0;
}

static bool wp_scan_name(const char *n) {
    return wp_wheel_name(n) || wp_water_name(n) || wp_timer_kind(n) != 0;
}

static bool wp_shutter_name(const char *n) {
    return !strncmp(n, "SS_object_ShutterFence", 22) || !strncmp(n, "SwitchShutter", 13) ||
           !strncmp(n, "DgnObj_BoxShutter", 17);
}

static bool wp_claim_beats(int cur_owner, uint16_t cur_gen, uint16_t new_gen) {
    int16_t dg = (int16_t)(new_gen - cur_gen);
    if (cur_owner == 1) return dg > 0 || (dg == 0 && !coop_net_is_host());
    return cur_owner == 0 || dg >= 0;
}

static void wp_apply_ev_wheel(struct WpWheel *w, const CoopWaterPuz *e, uint64_t now) {
    if ((int16_t)(e->gen - w->top) > 0) w->top = e->gen;
    if (!wp_claim_beats(w->owner, w->gen, e->gen)) return;
    bool active = (e->flags & COOP_WP_F_ACTIVE) != 0;
    if (w->owner != 2 || w->gen != e->gen || w->remote_active != active) {
        LOG("[coop] water: wheel %s %u is remote's (claim %u, %s, angvel %.2f %.2f %.2f)\n",
            w->name[0] ? w->name : "?", w->hash, e->gen, active ? "spinning" : "stopped",
            e->val[0], e->val[1], e->val[2]);
        if (w->owner != 2 || w->gen != e->gen) w->changed_ns = now;
    }
    bool was_active = w->remote_active;
    bool diff_rot = w->owner != 2 || !w->have_remote || wp_quat_absdot(w->remote_rot, e->rot) < 0.999f;
    w->owner = 2;
    w->gen = e->gen;
    w->have_remote = true;
    w->remote_active = active;
    w->remote_fresh = true;
    if (active || was_active || diff_rot) w->matched = false;
    memcpy(w->remote_pos, e->pos, 12);
    memcpy(w->remote_rot, e->rot, 16);
    wp_quat_norm(w->remote_rot);
    memcpy(w->remote_angvel, e->val, 12);
    w->remote_ns = now;
}

static void wp_apply_ev_water(struct WpWater *w, const CoopWaterPuz *e, uint64_t now) {
    if ((int16_t)(e->gen - w->top) > 0) w->top = e->gen;
    if (!wp_claim_beats(w->owner, w->gen, e->gen)) return;
    bool raised = (e->flags & COOP_WP_F_ON) != 0;
    bool moving = (e->flags & COOP_WP_F_ACTIVE) != 0;
    if (w->owner != 2 || w->remote_raised != raised || w->remote_moving != moving) {
        LOG("[coop] water: surface %s %u remote y=%.2f target=%.2f (%s, %s)\n",
            w->name[0] ? w->name : "?", w->hash, e->pos[1], e->val[0],
            raised ? "raised" : "base", moving ? "moving" : "still");
        if (w->owner != 2) w->changed_ns = now;
    }
    w->owner = 2;
    w->gen = e->gen;
    w->have_remote = true;
    w->remote_raised = raised;
    w->remote_moving = moving;
    w->remote_y = e->pos[1];
    w->remote_target_y = e->val[0];
    w->remote_speed = e->val[1];
    w->remote_ns = now;
}

static void wp_apply_ev_timer(struct WpTimer *t, const CoopWaterPuz *e, uint64_t now) {
    if (t->kind == COOP_WP_TIMER) {
        // Periodic SwitchTimer: host is canonical owner; host ignores guest packets.
        if (coop_net_is_host()) return;
        t->owner = 2;
        t->have_remote = true;
        t->remote_on = (e->flags & COOP_WP_F_ON) != 0;
        t->remote_elapsed = e->val[0];
        t->remote_wait = e->val[1];
        t->remote_ns = now;
        return;
    }
    if ((int16_t)(e->gen - t->top) > 0) t->top = e->gen;
    if (!wp_claim_beats(t->owner, t->gen, e->gen)) return;
    bool on = (e->flags & COOP_WP_F_ON) != 0;
    if (t->owner != 2 || t->gen != e->gen || t->remote_on != on) {
        LOG("[coop] water: timer %s %u remote %s (claim %u, el=%.1f/%.1f)\n",
            t->name[0] ? t->name : "?", t->hash, on ? "ON" : "OFF", e->gen, e->val[0], e->val[1]);
        if (t->owner != 2 || t->gen != e->gen) t->changed_ns = now;
    }
    t->owner = 2;
    t->gen = e->gen;
    t->have_remote = true;
    t->remote_on = on;
    t->remote_elapsed = e->val[0];
    t->remote_wait = e->val[1];
    t->remote_ns = now;
}

static void wp_pend_store(const CoopWaterPuz *e, uint64_t now) {
    int k = -1;
    for (int i = 0; i < WP_PEND_MAX; i++)
        if (wp_pend[i].at_ns && wp_pend[i].e.hash == e->hash && wp_pend[i].e.kind == e->kind) { k = i; break; }
    if (k < 0)
        for (int i = 0; i < WP_PEND_MAX; i++)
            if (!wp_pend[i].at_ns || now - wp_pend[i].at_ns > 10000000000ull) { k = i; break; }
    if (k < 0) return;
    wp_pend[k].e = *e;
    wp_pend[k].at_ns = now;
}

static void wp_pend_drain_for(uint8_t kind, uint32_t hash, void *slot, uint64_t now) {
    for (int i = 0; i < WP_PEND_MAX; i++) {
        if (!wp_pend[i].at_ns || wp_pend[i].e.kind != kind || wp_pend[i].e.hash != hash) continue;
        if (now - wp_pend[i].at_ns <= 10000000000ull) {
            if (kind == COOP_WP_WHEEL) wp_apply_ev_wheel(slot, &wp_pend[i].e, wp_pend[i].at_ns);
            else if (kind == COOP_WP_WATER) wp_apply_ev_water(slot, &wp_pend[i].e, wp_pend[i].at_ns);
            else wp_apply_ev_timer(slot, &wp_pend[i].e, wp_pend[i].at_ns);
        }
        wp_pend[i].at_ns = 0;
    }
}

static void waterpuz_ev(const uint8_t *data, int len) {
    if (len != (int)sizeof(CoopWaterPuz) || !waterpuz_on()) return;
    CoopWaterPuz e;
    memcpy(&e, data, sizeof e);
    if (!coop_waterpuz_ok(&e) || !wp_area || e.area != wp_area) return;
    uint64_t now = plat_now_ns();
    if (e.kind == COOP_WP_WHEEL) {
        for (int i = 0; i < wp_wheel_n; i++)
            if (wp_wheels[i].hash == e.hash) { wp_apply_ev_wheel(&wp_wheels[i], &e, now); return; }
        wp_pend_store(&e, now);
    } else if (e.kind == COOP_WP_WATER) {
        for (int i = 0; i < wp_water_n; i++)
            if (wp_waters[i].hash == e.hash) { wp_apply_ev_water(&wp_waters[i], &e, now); return; }
        wp_pend_store(&e, now);
    } else {
        for (int i = 0; i < wp_timer_n; i++)
            if (wp_timers[i].hash == e.hash && wp_timers[i].kind == e.kind) { wp_apply_ev_timer(&wp_timers[i], &e, now); return; }
        wp_pend_store(&e, now);
    }
}

static void wp_found(Cpu *c, uint64_t a, const char *name, void *ctx) {
    uint64_t now = *(const uint64_t *)ctx;
    uint32_t uid = LD32(a + PROC_ID);
    if (wp_wheel_name(name)) {
        for (int i = 0; i < wp_wheel_n; i++)
            if (wp_wheels[i].actor == a && wp_wheels[i].unit_id == uid) return;
        if (wp_wheel_n >= WP_WHEEL_MAX) return;
        uint32_t h = actor_hash(c, a);
        objects_fast_only = true;
        uint64_t rb = h ? actor_rb(a) : 0;
        objects_fast_only = false;
        if (!rb) return;
        struct WpWheel *w = &wp_wheels[wp_wheel_n++];
        memset(w, 0, sizeof *w);
        w->hash = h; w->actor = a; w->unit = LD64(a + PROC_UNIT); w->unit_id = uid;
        w->rb = rb; w->body = LD64(rb + 0x78);
        snprintf(w->name, sizeof w->name, "%s", name);
        w->ai = wp_find_ai(a, WP_VT_WHEEL);
        w->rot_axis = !strncmp(name, "DgnObj_SwitchCapstan", 20) ? 1 : 0;
        if (w->ai) {
            uint64_t axp = LD64(w->ai + 0x60);
            if (in_guest(axp)) {
                int ax = (int)LD32(axp);
                if (ax >= 0 && ax <= 2) w->rot_axis = ax;
            }
        }
        memcpy(w->rest_q, (const void *)(uintptr_t)(w->body + 0x1d0), 16);
        wp_quat_norm(w->rest_q);
        w->found_ns = w->ai_try_ns = now;
        LOG("[coop] water: found wheel %s %u actor %llx rb %llx ai %llx axis %d\n",
            w->name, w->hash, (unsigned long long)w->actor, (unsigned long long)w->rb,
            (unsigned long long)w->ai, w->rot_axis);
        wp_pend_drain_for(COOP_WP_WHEEL, h, w, now);
        return;
    }
    if (wp_water_name(name)) {
        for (int i = 0; i < wp_water_n; i++)
            if (wp_waters[i].actor == a && wp_waters[i].unit_id == uid) return;
        if (wp_water_n >= WP_WATER_MAX) return;
        if (!wp_link_has_basic(c, a)) return;  // static water plane: nothing to sync
        uint32_t h = actor_hash(c, a);
        uint64_t ai = h ? wp_find_ai(a, WP_VT_WATER) : 0;
        objects_fast_only = true;
        uint64_t rb = ai ? actor_rb(a) : 0;
        objects_fast_only = false;
        if (!rb || !ai) return;
        struct WpWater *w = &wp_waters[wp_water_n++];
        memset(w, 0, sizeof *w);
        w->hash = h; w->actor = a; w->unit = LD64(a + PROC_UNIT); w->unit_id = uid;
        w->rb = rb; w->body = LD64(rb + 0x78); w->ai = ai;
        snprintf(w->name, sizeof w->name, "%s", name);
        w->base_y = LDF32(ai + 0x6c);
        uint64_t lp = LD64(ai + 0x80), sp = LD64(ai + 0x88);
        w->rise_len = in_guest(lp) ? LDF32(lp) : 0.0f;
        w->rise_spd = in_guest(sp) ? LDF32(sp) : 1.0f;
        if (!isfinite(w->rise_len)) w->rise_len = 0.0f;
        if (!isfinite(w->rise_spd) || w->rise_spd < 0.0f || w->rise_spd > 100.0f) w->rise_spd = 1.0f;
        w->last_raised = wp_link_is_basic_on(c, a);
        w->found_ns = now;
        LOG("[coop] water: found dynamic surface %s %u actor %llx ai %llx base_y=%.2f rise=%.2f spd=%.2f sig=%d\n",
            w->name, w->hash, (unsigned long long)w->actor, (unsigned long long)w->ai,
            w->base_y, w->rise_len, w->rise_spd, (int)w->last_raised);
        wp_pend_drain_for(COOP_WP_WATER, h, w, now);
        return;
    }
    uint8_t kind = wp_timer_kind(name);
    if (kind) {
        for (int i = 0; i < wp_timer_n; i++)
            if (wp_timers[i].actor == a && wp_timers[i].unit_id == uid) return;
        if (wp_timer_n >= WP_TIMER_MAX) return;
        uint32_t h = actor_hash(c, a);
        if (!h) return;
        struct WpTimer *t = &wp_timers[wp_timer_n++];
        memset(t, 0, sizeof *t);
        t->hash = h; t->actor = a; t->unit = LD64(a + PROC_UNIT); t->unit_id = uid;
        t->kind = kind;
        snprintf(t->name, sizeof t->name, "%s", name);
        t->ai = wp_find_ai(a, wp_timer_vt(kind));
        if (t->ai) {
            uint64_t wp = LD64(t->ai + 0x38);
            if (in_guest(wp)) t->wait_time = LDF32(wp);
            t->last_elapsed = LDF32(t->ai + 0x40);
        }
        if (!isfinite(t->wait_time) || t->wait_time <= 0.0f) t->wait_time = 30.0f;
        t->last_on = (LD32(a + ACTOR_OUT_SIGNALS) & 1) != 0;
        t->last_in_on = wp_link_is_basic_on(c, a);
        t->found_ns = t->ai_try_ns = now;
        LOG("[coop] water: found timer %s %u actor %llx ai %llx wait=%.1f on=%d in=%d\n",
            t->name, t->hash, (unsigned long long)t->actor, (unsigned long long)t->ai,
            t->wait_time, (int)t->last_on, (int)t->last_in_on);
        wp_pend_drain_for(kind, h, t, now);
    }
}

static void wp_shutter_list(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)ctx;
    uint32_t h = actor_hash(c, a);
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    bool in_on = wp_link_is_basic_on(c, a);
    LOG("[coop] water shutter: %s %u actor %llx pos %.2f %.2f %.2f in_sig=%d\n",
        name, h, (unsigned long long)a, m[3], m[7], m[11], (int)in_on);
}

static bool wp_send_wheel(struct WpWheel *w, const float pos[3], const float rot[4], const float angvel[3],
                          bool spinning, uint64_t now) {
    CoopWaterPuz e;
    memset(&e, 0, sizeof e);
    e.hash = w->hash; e.area = wp_area; e.gen = w->gen;
    e.kind = COOP_WP_WHEEL;
    e.flags = spinning ? (COOP_WP_F_ON | COOP_WP_F_ACTIVE) : 0;
    memcpy(e.pos, pos, 12);
    memcpy(e.rot, rot, 16);
    if (!wp_quat_norm(e.rot)) { e.rot[0] = e.rot[1] = e.rot[2] = 0; e.rot[3] = 1; }
    if (spinning) memcpy(e.val, angvel, 12);
    if (!coop_waterpuz_ok(&e)) return false;
    bool ok = coop_net_send_event(COOP_EV_WATERPUZ, &e, sizeof e);
    if (ok) {
        if (spinning != w->spinning_sent)
            LOG("[coop] water: wheel %s %u sent %s (claim %u, angvel %.2f %.2f %.2f)\n",
                w->name, w->hash, spinning ? "spinning" : "stopped", w->gen, e.val[0], e.val[1], e.val[2]);
        w->spinning_sent = spinning;
        w->sent_ns = now;
    }
    return ok;
}

static bool wp_send_water(struct WpWater *w, float cur_y, float target_y, bool raised, bool moving, uint64_t now) {
    CoopWaterPuz e;
    memset(&e, 0, sizeof e);
    e.hash = w->hash; e.area = wp_area; e.gen = w->gen;
    e.kind = COOP_WP_WATER;
    e.flags = (uint8_t)((raised ? COOP_WP_F_ON : 0) | (moving ? COOP_WP_F_ACTIVE : 0));
    e.pos[1] = cur_y;
    e.rot[3] = 1.0f;
    e.val[0] = target_y;
    e.val[1] = w->rise_spd;
    if (!coop_waterpuz_ok(&e)) return false;
    bool ok = coop_net_send_event(COOP_EV_WATERPUZ, &e, sizeof e);
    if (ok) w->sent_ns = now;
    return ok;
}

static bool wp_send_timer(struct WpTimer *t, bool on, float elapsed, uint64_t now) {
    CoopWaterPuz e;
    memset(&e, 0, sizeof e);
    e.hash = t->hash; e.area = wp_area; e.gen = t->gen;
    e.kind = t->kind;
    e.flags = on ? COOP_WP_F_ON : 0;
    e.rot[3] = 1.0f;
    e.val[0] = elapsed < 0.0f ? 0.0f : (elapsed > 10000.0f ? 10000.0f : elapsed);
    e.val[1] = t->wait_time > 0.0f ? t->wait_time : 30.0f;
    if (!coop_waterpuz_ok(&e)) return false;
    bool ok = coop_net_send_event(COOP_EV_WATERPUZ, &e, sizeof e);
    if (ok) t->sent_ns = now;
    return ok;
}

// Check if any remote avatar is closer to `pos` than our own Link (prevents accidental remote-avatar claims).
static bool wp_remote_closer(const float pos[3], float max_m) {
    float me[3];
    if (!player_pos(me)) return false;
    float d_me = dist3(pos, me);
    for (int i = 0; i < rav_n; i++) {
        if (!rav[i].fresh) continue;
        float d_r = dist3(pos, rav[i].rs.pos);
        if (d_r < max_m && d_r + 0.5f < d_me) return true;
    }
    return false;
}

static void wp_wheels_frame(Cpu *c, bool together, uint64_t now) {
    for (int i = 0; i < wp_wheel_n; i++) {
        struct WpWheel *w = &wp_wheels[i];
        if (!wp_wheel_alive(w)) { wp_wheels[i--] = wp_wheels[--wp_wheel_n]; continue; }
        if (!w->ai && now - w->ai_try_ns > 500000000ull) {
            w->ai_try_ns = now;
            w->ai = wp_find_ai(w->actor, WP_VT_WHEEL);
            if (w->ai) {
                uint64_t axp = LD64(w->ai + 0x60);
                if (in_guest(axp)) {
                    int ax = (int)LD32(axp);
                    if (ax >= 0 && ax <= 2) w->rot_axis = ax;
                }
            }
        }
        // Dev command: spin wheel along its SwitchWheel RotAxis (row rot_axis of inv_init_rot at ai+0x68).
        bool dev_spinning = w->dev_spin_until_ns && now < w->dev_spin_until_ns;
        if (dev_spinning) {
            float axis[3] = {0, 0, 0};
            if (w->ai && in_guest(w->ai + 0x90)) {
                const float *inv = (const float *)(uintptr_t)(w->ai + 0x68);
                axis[0] = inv[w->rot_axis * 3 + 0];
                axis[1] = inv[w->rot_axis * 3 + 1];
                axis[2] = inv[w->rot_axis * 3 + 2];
            } else {
                const float *m = (const float *)(uintptr_t)(w->actor + ACTOR_MTX);
                axis[0] = m[w->rot_axis];
                axis[1] = m[4 + w->rot_axis];
                axis[2] = m[8 + w->rot_axis];
            }
            float al = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            if (al > 1e-4f) {
                float wv[3] = {axis[0] / al * w->dev_spin_rad, axis[1] / al * w->dev_spin_rad, axis[2] / al * w->dev_spin_rad};
                wp_set_angvel(c, w->rb, wv);
            }
        } else if (w->dev_spin_until_ns) {
            w->dev_spin_until_ns = 0;
            float zero[3] = {0, 0, 0};
            wp_set_angvel(c, w->rb, zero);
            LOG("[coop] water: dev spin %s %u ended\n", w->name, w->hash);
        }
        if (!together) {
            w->owner = 0; w->have_remote = false; w->remote_active = false; w->spinning_sent = false;
            continue;
        }
        float pos[3], cur_q[4], angvel[3];
        memcpy(pos, (const void *)(uintptr_t)(w->body + 0x1a0), 12);
        memcpy(cur_q, (const void *)(uintptr_t)(w->body + 0x1d0), 16);
        wp_quat_norm(cur_q);
        wp_get_angvel(c, w->rb, angvel);
        float ang_spd = sqrtf(angvel[0] * angvel[0] + angvel[1] * angvel[1] + angvel[2] * angvel[2]);
        uint32_t sig = LD32(w->actor + ACTOR_OUT_SIGNALS) & 7u;
        bool spinning = ang_spd > 0.08f || sig != 0 || dev_spinning;
        bool remote_live = w->have_remote && w->remote_active && now - w->remote_ns < 1500000000ull;

        if (w->owner == 0) {
            bool settled = dev_spinning || (now - w->found_ns > 1500000000ull);
            if (!settled) memcpy(w->rest_q, cur_q, 16);
            bool moved = ang_spd > 0.12f || sig != 0 || wp_quat_absdot(cur_q, w->rest_q) < 0.998f;
            if (settled && moved && (dev_spinning || !wp_remote_closer(pos, 5.0f))) {
                w->gen = w->top = (uint16_t)((w->top > w->gen ? w->top : w->gen) + 1);
                w->owner = 1; w->changed_ns = now; w->sent_ns = 0; w->claim_frames = 0;
                LOG("[coop] water: wheel %s %u moved here: ours (claim %u, spd=%.2f sig=%u)\n",
                    w->name, w->hash, w->gen, ang_spd, sig);
            }
        } else if (w->owner == 2 && !remote_live && (dev_spinning || now - w->changed_ns > 2500000000ull)) {
            bool want = dev_spinning || (ang_spd > 0.25f && !wp_remote_closer(pos, 5.0f));
            if (want) {
                if (dev_spinning || ++w->claim_frames >= 4) {
                    w->gen = w->top = (uint16_t)((w->top > w->gen ? w->top : w->gen) + 1);
                    w->owner = 1; w->changed_ns = now; w->sent_ns = 0; w->claim_frames = 0;
                    w->have_remote = false; w->remote_active = false;
                    LOG("[coop] water: wheel %s %u taken over here (claim %u, spd=%.2f)\n",
                        w->name, w->hash, w->gen, ang_spd);
                }
            } else {
                w->claim_frames = 0;
            }
        }

        if (w->owner == 1) {
            uint64_t since = now - w->sent_ns;
            if (!w->sent_ns || spinning != w->spinning_sent ||
                (spinning && since >= 80000000ull) || (!spinning && since >= 3000000000ull)) {
                wp_send_wheel(w, pos, cur_q, angvel, spinning, now);
            }
            if (!spinning) memcpy(w->rest_q, cur_q, 16);
        } else if (w->owner == 2 && w->have_remote) {
            if (remote_live) {
                float dt = (float)(now - w->remote_ns) / 1e9f;
                if (dt > 0.25f) dt = 0.25f;
                float target_q[4];
                wp_quat_extrap(w->remote_rot, w->remote_angvel, dt, target_q);
                // Only nudge transform if angle drifted > ~8 deg (or first frame after packet with drift > ~4 deg)
                float dot = wp_quat_absdot(cur_q, target_q);
                if ((w->remote_fresh && dot < 0.9993f) || dot < 0.9975f) {
                    float m[12];
                    quat_mtx(target_q, w->remote_pos, m);
                    wp_unfix_rb(c, w->rb);
                    rb_set_transform(c, w->rb, m);
                }
                w->remote_fresh = false;
                wp_set_angvel(c, w->rb, w->remote_angvel);
            } else if (!w->matched) {
                float zero[3] = {0, 0, 0};
                wp_set_angvel(c, w->rb, zero);
                if (wp_quat_absdot(cur_q, w->remote_rot) < 0.9995f) {
                    float m[12];
                    quat_mtx(w->remote_rot, w->remote_pos, m);
                    rb_set_transform(c, w->rb, m);
                }
                memcpy(w->rest_q, w->remote_rot, 16);
                w->matched = true;
                w->remote_fresh = false;
                LOG("[coop] water: wheel %s %u matched at rest\n", w->name, w->hash);
            }
        }
    }
}

static void wp_waters_frame(Cpu *c, bool together, uint64_t now) {
    for (int i = 0; i < wp_water_n; i++) {
        struct WpWater *w = &wp_waters[i];
        if (!wp_water_alive(w)) { wp_waters[i--] = wp_waters[--wp_water_n]; continue; }
        if (!together) {
            w->owner = 0; w->have_remote = false;
            continue;
        }
        float cur_y = LDF32(w->actor + ACTOR_MTX + 0x1c);
        bool raised = wp_link_is_basic_on(c, w->actor);
        float target_y = raised ? (w->base_y + w->rise_len) : w->base_y;
        bool moving = fabsf(cur_y - target_y) > 0.05f;

        bool changed = raised != w->last_raised;
        w->last_raised = raised;

        if (w->owner == 0) {
            if ((changed && now - w->found_ns > 1500000000ull) ||
                (coop_net_is_host() && now - w->found_ns > 2000000000ull)) {
                w->gen = w->top = (uint16_t)((w->top > w->gen ? w->top : w->gen) + 1);
                w->owner = 1; w->changed_ns = now; w->sent_ns = 0;
            }
        } else if (w->owner == 2 && changed && now - w->changed_ns > 1500000000ull &&
                   (!w->have_remote || raised != w->remote_raised)) {
            w->gen = w->top = (uint16_t)((w->top > w->gen ? w->top : w->gen) + 1);
            w->owner = 1; w->changed_ns = now; w->sent_ns = 0;
        }

        if (w->owner == 1) {
            uint64_t since = now - w->sent_ns;
            if (!w->sent_ns || changed || moving != w->last_moving ||
                (moving && since >= 250000000ull) || (!moving && since >= 3000000000ull)) {
                wp_send_water(w, cur_y, target_y, raised, moving, now);
            }
        } else if (w->owner == 2 && w->have_remote && now - w->remote_ns < 5000000000ull) {
            // Only align height when local incoming signal agrees with owner's target so WaterSurface AI never fights us.
            if (raised == w->remote_raised) {
                float want_y = w->remote_y;
                if (w->remote_moving) {
                    float age = (float)(now - w->remote_ns) / 1e9f;
                    if (age > 0.5f) age = 0.5f;
                    float dir = (w->remote_target_y > w->remote_y) ? 1.0f : -1.0f;
                    want_y += dir * w->remote_speed * age;
                    if ((dir > 0.0f && want_y > w->remote_target_y) || (dir < 0.0f && want_y < w->remote_target_y))
                        want_y = w->remote_target_y;
                }
                if (fabsf(cur_y - want_y) > 0.35f) {
                    float m[12];
                    memcpy(m, (const void *)(uintptr_t)(w->actor + ACTOR_MTX), 48);
                    m[7] = want_y;
                    if (w->remote_moving) {
                        wp_move_keyframed(c, w->rb, m);
                    } else {
                        rb_set_transform(c, w->rb, m);
                        static const float zero[3] = {0, 0, 0};
                        rb_set_linvel(c, w->rb, zero);
                    }
                }
            }
        }
        w->last_moving = moving;
    }
}

static void wp_timers_frame(Cpu *c, bool together, uint64_t now) {
    bool host = coop_net_is_host();
    for (int i = 0; i < wp_timer_n; i++) {
        struct WpTimer *t = &wp_timers[i];
        if (!wp_timer_alive(t)) { wp_timers[i--] = wp_timers[--wp_timer_n]; continue; }
        if (!t->ai && now - t->ai_try_ns > 500000000ull) {
            t->ai_try_ns = now;
            t->ai = wp_find_ai(t->actor, wp_timer_vt(t->kind));
            if (t->ai) {
                uint64_t wp = LD64(t->ai + 0x38);
                if (in_guest(wp) && isfinite(LDF32(wp)) && LDF32(wp) > 0.0f) t->wait_time = LDF32(wp);
            }
        }
        if (!t->ai || !in_guest(t->ai + 0x48) || LD64(t->ai) != wp_timer_vt(t->kind) || LD64(t->ai + 8) != t->actor)
            continue;

        bool on = (LD32(t->actor + ACTOR_OUT_SIGNALS) & 1) != 0;
        float elapsed = LDF32(t->ai + 0x40);
        if (!isfinite(elapsed) || elapsed < 0.0f) elapsed = 0.0f;
        bool has_in = wp_link_has_basic(c, t->actor);
        bool in_on = has_in ? wp_link_is_basic_on(c, t->actor) : false;

        if (!together) {
            t->owner = 0; t->have_remote = false;
            t->last_on = on; t->last_elapsed = elapsed; t->last_in_on = in_on;
            continue;
        }

        if (t->kind == COOP_WP_TIMER) {
            // Periodic oscillator: host is canonical owner.
            if (host) {
                bool active = !has_in || in_on || on;
                if (active && (on != t->last_on || !t->sent_ns || now - t->sent_ns >= 2000000000ull)) {
                    wp_send_timer(t, on, elapsed, now);
                }
            } else if (t->have_remote && now - t->remote_ns < 5000000000ull) {
                // Only adjust if timer is free-running or its gating input is ON locally.
                if (!has_in || in_on) {
                    float wt = t->wait_time > 1.0f ? t->wait_time : t->remote_wait;
                    float want_el = t->remote_elapsed + (float)(now - t->remote_ns) / 1e9f * 30.0f;
                    bool want_on = t->remote_on;
                    for (int w = 0; w < 4 && wt > 1.0f && want_el >= wt; w++) {
                        want_el -= wt;
                        want_on = !want_on;
                    }
                    if (want_el >= wt) want_el = wt - 0.5f;
                    if (on != want_on && now - t->applied_ns > 250000000ull) {
                        wp_ai_change_child(c, t->ai, want_on ? WP_STR_ON : WP_STR_OFF);
                        STF(t->ai + 0x40, want_el);
                        t->applied_ns = now;
                        LOG("[coop] water: SwitchTimer %u aligned to host (%s, el=%.1f/%.1f)\n",
                            t->hash, want_on ? "ON" : "OFF", want_el, wt);
                    } else if (on == want_on && fabsf(elapsed - want_el) > 6.0f &&
                               want_el > 3.0f && want_el < wt - 3.0f && now - t->applied_ns > 500000000ull) {
                        STF(t->ai + 0x40, want_el);
                        t->applied_ns = now;
                    }
                }
            }
            t->last_on = on; t->last_elapsed = elapsed; t->last_in_on = in_on;
            continue;
        }

        // SwitchTimeLimited & SwitchTimeLag: owner is whoever triggers it.
        bool ours_edge = (on != t->last_on) && (now - t->applied_ns > 1000000000ull) &&
                         (now - t->found_ns > 1000000000ull);
        bool counting_start = (t->kind == COOP_WP_TIMELAG) && in_on && !on && elapsed > 1.0f &&
                              t->last_elapsed <= 1.0f && (now - t->applied_ns > 1000000000ull);
        bool retriggered = (t->kind == COOP_WP_LIMITED) && on && in_on && !t->last_in_on &&
                           (now - t->applied_ns > 1000000000ull);

        if (t->owner == 0 && (ours_edge || counting_start)) {
            t->gen = t->top = (uint16_t)((t->top > t->gen ? t->top : t->gen) + 1);
            t->owner = 1; t->changed_ns = now; t->sent_ns = 0;
        } else if (t->owner == 2 && (retriggered || (ours_edge && on && !t->remote_on && now - t->changed_ns > 1000000000ull))) {
            t->gen = t->top = (uint16_t)((t->top > t->gen ? t->top : t->gen) + 1);
            t->owner = 1; t->changed_ns = now; t->sent_ns = 0; t->have_remote = false;
        }

        if (t->owner == 1) {
            bool active = (t->kind == COOP_WP_LIMITED) ? on : (on || (in_on && elapsed > 0.5f));
            uint64_t since = now - t->sent_ns;
            if (!t->sent_ns || on != t->last_on || retriggered ||
                (active && since >= 400000000ull)) {
                wp_send_timer(t, on, elapsed, now);
                if (on != t->last_on)
                    LOG("[coop] water: %s %u %s here (sent, claim %u, el=%.1f/%.1f)\n",
                        t->name, t->hash, on ? "ON" : "OFF", t->gen, elapsed, t->wait_time);
            }
            if (!active && !on && now - t->changed_ns > 2000000000ull) t->owner = 0;
        } else if (t->owner == 2 && t->have_remote) {
            float wt = t->wait_time > 1.0f ? t->wait_time : t->remote_wait;
            float want_el = t->remote_elapsed + (float)(now - t->remote_ns) / 1e9f * 30.0f;
            if (t->kind == COOP_WP_LIMITED) {
                if (t->remote_on && want_el < wt && now - t->remote_ns < 5000000000ull) {
                    if (!on && now - t->applied_ns > 250000000ull) {
                        wp_ai_change_child(c, t->ai, WP_STR_ON);
                        STF(t->ai + 0x40, want_el);
                        t->applied_ns = now;
                        LOG("[coop] water: SwitchTimeLimited %u ON here too (el=%.1f/%.1f)\n", t->hash, want_el, wt);
                    } else if (on && !in_on && fabsf(elapsed - want_el) > 4.5f && want_el < wt - 2.0f &&
                               now - t->applied_ns > 400000000ull) {
                        STF(t->ai + 0x40, want_el);
                        t->applied_ns = now;
                    }
                } else if (!t->remote_on || want_el >= wt) {
                    if (on && !in_on && now - t->applied_ns > 250000000ull) {
                        wp_ai_change_child(c, t->ai, WP_STR_OFF);
                        STF(t->ai + 0x40, 0.0f);
                        t->applied_ns = now;
                        LOG("[coop] water: SwitchTimeLimited %u OFF here too\n", t->hash);
                    }
                    if (!on) { t->owner = 0; t->have_remote = false; }
                }
            } else {
                // SwitchTimeLag: only act while our incoming BasicSig is ON so SwitchTimeLag::calc_ never fights us.
                if (in_on && now - t->remote_ns < 5000000000ull) {
                    if (t->remote_on && !on && now - t->applied_ns > 250000000ull) {
                        wp_ai_change_child(c, t->ai, WP_STR_ON);
                        STF(t->ai + 0x40, 0.0f);
                        t->applied_ns = now;
                        LOG("[coop] water: SwitchTimeLag %u ON here too\n", t->hash);
                    } else if (!t->remote_on && !on && want_el < wt - 2.0f && fabsf(elapsed - want_el) > 4.5f &&
                               now - t->applied_ns > 400000000ull) {
                        STF(t->ai + 0x40, want_el);
                        t->applied_ns = now;
                    }
                } else if (!t->remote_on && !in_on && !on) {
                    t->owner = 0; t->have_remote = false;
                }
            }
        }
        t->last_on = on; t->last_elapsed = elapsed; t->last_in_on = in_on;
    }
}

static void wp_dev_run(Cpu *c, bool together, uint64_t now) {
    if (wp_dev.list_req) {
        wp_dev.list_req = 0;
        LOG("[coop] water list: wheels=%d waters=%d timers=%d together=%d\n",
            wp_wheel_n, wp_water_n, wp_timer_n, (int)together);
        for (int i = 0; i < wp_wheel_n; i++) {
            struct WpWheel *w = &wp_wheels[i];
            if (!wp_wheel_alive(w)) continue;
            float av[3], q[4];
            wp_get_angvel(c, w->rb, av);
            memcpy(q, (const void *)(uintptr_t)(w->body + 0x1d0), 16);
            LOG("[coop] water wheel: %s %u actor %llx ai %llx owner %d gen %u sig %u av %.2f %.2f %.2f q %.3f %.3f %.3f %.3f\n",
                w->name, w->hash, (unsigned long long)w->actor, (unsigned long long)w->ai,
                w->owner, w->gen, LD32(w->actor + ACTOR_OUT_SIGNALS) & 7u,
                av[0], av[1], av[2], q[0], q[1], q[2], q[3]);
        }
        for (int i = 0; i < wp_water_n; i++) {
            struct WpWater *w = &wp_waters[i];
            if (!wp_water_alive(w)) continue;
            LOG("[coop] water surface: %s %u actor %llx ai %llx owner %d y %.2f base %.2f rise %.2f sig %d\n",
                w->name, w->hash, (unsigned long long)w->actor, (unsigned long long)w->ai,
                w->owner, LDF32(w->actor + ACTOR_MTX + 0x1c), w->base_y, w->rise_len,
                (int)wp_link_is_basic_on(c, w->actor));
        }
        for (int i = 0; i < wp_timer_n; i++) {
            struct WpTimer *t = &wp_timers[i];
            if (!wp_timer_alive(t)) continue;
            float el = t->ai ? LDF32(t->ai + 0x40) : -1.0f;
            LOG("[coop] water timer: %s %u actor %llx ai %llx owner %d on %d in %d el %.1f/%.1f\n",
                t->name, t->hash, (unsigned long long)t->actor, (unsigned long long)t->ai,
                t->owner, (int)(LD32(t->actor + ACTOR_OUT_SIGNALS) & 1),
                (int)wp_link_is_basic_on(c, t->actor), el, t->wait_time);
        }
        walk_actors(c, wp_shutter_name, wp_shutter_list, NULL);
    }
    if (wp_dev.spin_req) {
        wp_dev.spin_req = 0;
        bool found = false;
        for (int i = 0; i < wp_wheel_n; i++) {
            struct WpWheel *w = &wp_wheels[i];
            if (!wp_wheel_alive(w)) continue;
            if (wp_dev.spin_hash && w->hash != wp_dev.spin_hash) continue;
            w->dev_spin_rad = wp_dev.spin_rad;
            w->dev_spin_until_ns = now + (uint64_t)(wp_dev.spin_secs * 1e9f);
            found = true;
            LOG("[coop] water: dev spin %s %u at %.2f rad/s for %.1f s\n",
                w->name, w->hash, wp_dev.spin_rad, wp_dev.spin_secs);
        }
        if (!found) LOG("[coop] water: dev spin: wheel %u not found\n", wp_dev.spin_hash);
    }
    if (wp_dev.trig_req) {
        wp_dev.trig_req = 0;
        bool found = false;
        for (int i = 0; i < wp_timer_n; i++) {
            struct WpTimer *t = &wp_timers[i];
            if (!wp_timer_alive(t) || !t->ai) continue;
            if (wp_dev.trig_hash && t->hash != wp_dev.trig_hash) continue;
            bool cur = (LD32(t->actor + ACTOR_OUT_SIGNALS) & 1) != 0;
            bool want = wp_dev.trig_val < 0 ? !cur : (wp_dev.trig_val != 0);
            wp_ai_change_child(c, t->ai, want ? WP_STR_ON : WP_STR_OFF);
            STF(t->ai + 0x40, 0.0f);
            found = true;
            LOG("[coop] water: dev trigger %s %u -> %s\n", t->name, t->hash, want ? "ON" : "OFF");
        }
        if (!found) LOG("[coop] water: dev trigger: timer %u not found\n", wp_dev.trig_hash);
    }
    if (wp_dev.level_req) {
        wp_dev.level_req = 0;
        bool found = false;
        for (int i = 0; i < wp_water_n; i++) {
            struct WpWater *w = &wp_waters[i];
            if (!wp_water_alive(w)) continue;
            if (wp_dev.level_hash && w->hash != wp_dev.level_hash) continue;
            float m[12];
            memcpy(m, (const void *)(uintptr_t)(w->actor + ACTOR_MTX), 48);
            m[7] = wp_dev.level_y;
            rb_set_transform(c, w->rb, m);
            found = true;
            LOG("[coop] water: dev level %s %u -> y=%.2f\n", w->name, w->hash, wp_dev.level_y);
        }
        if (!found) LOG("[coop] water: dev level: surface %u not found\n", wp_dev.level_hash);
    }
}

static void waterpuz_frame(Cpu *c) {
    if (!waterpuz_on() || !wp_code_ok()) return;
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    bool in_shrine = !strncmp(area, "CDungeon", 8) && local_in_world();
    if (here != wp_area || !in_shrine) {
        if (wp_wheel_n || wp_water_n || wp_timer_n)
            LOG("[coop] water: forgotten (wheels=%d waters=%d timers=%d)\n", wp_wheel_n, wp_water_n, wp_timer_n);
        wp_wheel_n = wp_water_n = wp_timer_n = 0;
        wp_area = here;
        memset(wp_pend, 0, sizeof wp_pend);
        return;
    }
    CoopState rs;
    double rage = 99;
    bool together = coop_net_get_remote(&rs, &rage) && rage < 3.0 && (rs.flags & COOP_FLAG_IN_WORLD) && rs.area == here;
    static int multi_c = -1;
    if (!together && world_others() && player_info_ptr() && env_on("SAVAGEWIND_COOP_MULTIWORLD", &multi_c)) {
        struct ObjNear nr;
        const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
        obj_near_players(&nr, here, true, false, me);
        together = nr.n > 0;
    }
    if (!together && !wp_probe && !wp_dev.list_req && !wp_dev.spin_req && !wp_dev.trig_req && !wp_dev.level_req)
        return;
    static uint64_t scan_ns;
    if (now >= scan_ns || wp_dev.list_req || wp_dev.spin_req || wp_dev.trig_req || wp_dev.level_req) {
        scan_ns = now + 2000000000ull;
        walk_actors(c, wp_scan_name, wp_found, &now);
    }
    wp_dev_run(c, together, now);
    wp_wheels_frame(c, together, now);
    wp_waters_frame(c, together, now);
    wp_timers_frame(c, together, now);
}
