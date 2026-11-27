// Part of runtime/coop.c (included after coop/aim.inc: it uses aim.inc's bone modifier nodes): the partner's Master
// Cycle puppet animates (2026-11-20: the cycle puppet did not animate).
//
// Facts (Ghidra, 1.9.0): the real cycle (GameROMMotorcycle) has NO driving AS (its AS list: Close / MatAnim / Open /
// PreCooking*). Its wheels are separate Havok bodies (bphysics: MotorcycleChassis + MotorcycleFrontWheel /
// MotorcycleRearWheel, radius 0.45, no bone link) and the cycle's own code drives the model bones: FUN_71005c77d4
// binds bone accessors at actor +0x11b0.. for Wheel_F, Wheel_R, SwingArm_F/R, Suspension_F/R, Handle, Body_1, Head_A,
// Saddle_Root, Seat_*, RearCowl_A, SeatArm_* (model Motorcycle.sbfres). Our puppet (Dm_CoopH_GameROMMotorcycle: model
// only, DemoDummy AI, no physics) has none of that, so its wheels stood still.
// Receiver only (no protocol change): from the puppet's own movement each frame (it follows the sent matrix), the
// wheels roll (angle += forward distance / 0.45 m) and the handlebar steers like a bicycle (steer = atan(wheelbase *
// yaw rate / speed), clamped, smoothed; held while standing). Applied with our own bone modifier nodes (aim.inc:
// bone-local D after the animation) on Wheel_F, Wheel_R and Handle; each axis is the cycle's right (wheels) / up
// (handle) axis written in that bone's frame, taken once before the node is linked. Chassis lean / pitch / wheelies
// come with the sent matrix already. NOT copied: suspension travel, the real wheel spin when skidding or in the air.
// SAVAGEWIND_COOP_CYCLEFX=0: off (a static cycle puppet, as before). Dev: `coop cycle fake <m/s> <deg/s>` moves the
// shown cycle puppet itself in a circle (receiver only, for tests: riding cannot be driven by script), `coop cycle
// fake 0 0` stops, `coop cycle` logs the bones.
#define CYC_WHEEL_R 0.45f      // (bphysics: both wheel shapes are spheres of radius 0.45)
#define CYC_WHEELBASE 1.55f
#define CYC_STEER_MAX 0.6f     // radians
static int cyclefx_c = -1;
static bool cyclefx_on(void) {
    if (cyclefx_c < 0) {
        // default ON since 2026-11-20 (seen in close screenshots of a test run: wheels turn, handlebar steers
        // both ways; motion still faked by `coop cycle fake`, real riding not tried); SAVAGEWIND_COOP_CYCLEFX=0: off
        const char *e = getenv("SAVAGEWIND_COOP_CYCLEFX");
        cyclefx_c = !(e && e[0] == '0');
        LOG("[coop] cycle: the partner's Master Cycle wheels / handlebar animate: %s\n", cyclefx_c ? "on" : "off");
    }
    return cyclefx_c;
}
static volatile float cyc_fake_speed, cyc_fake_turn;  // (dev) m/s, deg/s
static volatile int cyc_report;
static const char *const cyc_bone[CYC_BONES] = {"Wheel_F", "Wheel_R", "Handle"};

static void cycle_fx_drop(Cpu *c) {
    for (int i = 0; i < CYC_BONES; i++) {
        if (hr.cyc.on[i] && hr.cyc.actor && hr.p[0].actor == hr.cyc.actor && pup_alive(&hr.p[0])) {
            Cpu saved = *c;
            c->x[0] = hr.cyc.node[i]; c->x[1] = hr.cyc.actor + 0x4d8;
            guest_call(c, A_BMOD_DEL);
            *c = saved;
        }
        bmod_release(&hr.cyc.node[i]);
        hr.cyc.on[i] = hr.cyc.failed[i] = hr.cyc.axis_ok[i] = false;
    }
    hr.cyc.actor = 0, hr.cyc.have_prev = false, hr.cyc.spin_f = hr.cyc.spin_r = hr.cyc.steer = 0;
}

// The direction w (world) in the bone's own frame (bone matrix rows: world = R * local, so local = R^T w), unit.
static bool cyc_local_axis(const float b[12], const float w[3], float out[3]) {
    float l[3];
    for (int j = 0; j < 3; j++) l[j] = b[j] * w[0] + b[4 + j] * w[1] + b[8 + j] * w[2];
    float n = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (!(n > 1e-4f) || !isfinite(n)) return false;
    for (int j = 0; j < 3; j++) out[j] = l[j] / n;
    return true;
}

// (horse_frame, after the puppet was placed on m this frame)
static void cycle_fx(Cpu *c, uint64_t body, float m[12], double dt) {
    if (!cyclefx_on() || !body) return;
    if (hr.cyc.actor != body) cycle_fx_drop(c), hr.cyc.actor = body;
    // (dev) the puppet itself drives in a circle: receiver-only test of the wheels / handlebar
    if (cyc_fake_speed != 0 || cyc_fake_turn != 0) {
        if (!hr.cyc.fake_have) memcpy(hr.cyc.fake, m, sizeof hr.cyc.fake), hr.cyc.fake_have = true;
        float yaw = cyc_fake_turn * 3.14159265f / 180 * (float)dt, cs = cosf(yaw), sn = sinf(yaw);
        float *f = hr.cyc.fake;  // (rotate the axes about world up, then step along the new forward)
        float R[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs}, o[9];
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) o[r * 3 + k] = R[r * 3] * f[k] + R[r * 3 + 1] * f[4 + k] + R[r * 3 + 2] * f[8 + k];
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) f[r * 4 + k] = o[r * 3 + k];
        for (int r = 0; r < 3; r++) f[r * 4 + 3] += f[r * 4 + 2] * cyc_fake_speed * (float)dt;
        memcpy(m, f, sizeof hr.cyc.fake);
        for (int p = 0; p < HORSE_PARTS; p++)
            if (hr.p[p].actor) {
                memcpy((void *)(uintptr_t)(hr.p[p].actor + ACTOR_MTX), m, 48);
                memcpy((void *)(uintptr_t)(hr.p[p].actor + ACTOR_MTX2), m, 48);
            }
    } else
        hr.cyc.fake_have = false;
    if (hr.p[0].frames < 6) return;
    // roll and steer from this frame's move
    if (hr.cyc.have_prev) {
        float d[3] = {m[3] - hr.cyc.prev[3], m[7] - hr.cyc.prev[7], m[11] - hr.cyc.prev[11]};
        float fl = sqrtf(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
        float v = fl > 1e-4f ? (d[0] * m[2] + d[1] * m[6] + d[2] * m[10]) / fl : 0;  // (forward distance this frame)
        if (fabsf(v) > 5.0f) v = 0;  // (a jump of the puppet: no spin)
        float yaw = atan2f(m[2], m[10]), yawp = atan2f(hr.cyc.prev[2], hr.cyc.prev[10]), dy = yaw - yawp;
        while (dy > 3.14159265f) dy -= 6.2831853f;
        while (dy < -3.14159265f) dy += 6.2831853f;
        hr.cyc.spin_f = fmodf(hr.cyc.spin_f + v / CYC_WHEEL_R, 6.2831853f);
        hr.cyc.spin_r = fmodf(hr.cyc.spin_r + v / CYC_WHEEL_R, 6.2831853f);
        if (fabsf(v) > 0.003f) {
            float want = atanf(CYC_WHEELBASE * dy / v);
            if (want > CYC_STEER_MAX) want = CYC_STEER_MAX;
            if (want < -CYC_STEER_MAX) want = -CYC_STEER_MAX;
            float k = (float)(dt * 8 > 1 ? 1 : dt * 8);
            hr.cyc.steer += (want - hr.cyc.steer) * k;
        }
        if (!isfinite(hr.cyc.steer)) hr.cyc.steer = 0;
        if (!isfinite(hr.cyc.spin_f) || !isfinite(hr.cyc.spin_r)) hr.cyc.spin_f = hr.cyc.spin_r = 0;
    }
    float lastm[12];  // (last frame's matrix: the bones read now were posed with it)
    bool last_ok = hr.cyc.have_prev;
    memcpy(lastm, hr.cyc.prev, sizeof lastm);
    memcpy(hr.cyc.prev, m, sizeof hr.cyc.prev);
    hr.cyc.have_prev = true;
    // link a node per bone (its axis read first, while no D of ours is on it), then write D
    static unsigned said;
    for (int i = 0; i < CYC_BONES; i++) {
        if (hr.cyc.failed[i]) continue;
        if (!hr.cyc.on[i]) {
            float b[12], w[3];
            if (!bone_matrix(c, body, cyc_bone[i], b)) continue;  // (not posed yet)
            if (i == 2) w[0] = m[1], w[1] = m[5], w[2] = m[9];   // the cycle's up (the handlebar turns about it)
            else w[0] = m[0], w[1] = m[4], w[2] = m[8];          // its x axis (the wheels' axle)
            if (!cyc_local_axis(b, w, hr.cyc.axis[i])) continue;
            if (i == 2) {  // (the handle's rest direction: its up axis, across the cycle's x / z)
                float u[3] = {b[1], b[5], b[9]};
                hr.cyc.h0[0] = u[0] * m[0] + u[1] * m[4] + u[2] * m[8], hr.cyc.h0[1] = u[0] * m[2] + u[1] * m[6] + u[2] * m[10];
                if (hr.cyc.h0[0] * hr.cyc.h0[0] + hr.cyc.h0[1] * hr.cyc.h0[1] < 0.01f) hr.cyc.h0[0] = hr.cyc.h0[1] = 0;
            }
            hr.cyc.done[i] = 0;
            if (!hr.cyc.node[i] && !(hr.cyc.node[i] = bmod_new_named(c, cyc_bone[i]))) continue;
            Cpu saved = *c;
            c->x[0] = body; c->x[1] = hr.cyc.node[i]; c->x[2] = 0;
            guest_call(c, A_BMOD_ADD);
            *c = saved;
            hr.cyc.on[i] = LD8(hr.cyc.node[i] + 8) != 0;
            if (!hr.cyc.on[i]) hr.cyc.failed[i] = true, bmod_release(&hr.cyc.node[i]);
            if (said++ < 12)
                LOG("[coop] cycle: %s modifier %s (local axis %.2f %.2f %.2f)\n", cyc_bone[i],
                    hr.cyc.on[i] ? "linked" : "NOT linked", hr.cyc.axis[i][0], hr.cyc.axis[i][1], hr.cyc.axis[i][2]);
            if (!hr.cyc.on[i]) continue;
        }
        // (seen 2026-11-20: the puppet plays no AS, so nothing resets these bones each frame and the game's M * D
        // ACCUMULATES: an absolute D spun the handlebar round and round.) So D is the step still missing: for the
        // wheels the spin since last frame; for the handlebar the target minus the angle it shows now (read from its
        // bone, posed last frame), which also cannot drift.
        float want = i == 0 ? hr.cyc.spin_f : i == 1 ? hr.cyc.spin_r : hr.cyc.steer, step = want - hr.cyc.done[i];
        if (i == 2) {
            float b[12];
            step = 0;
            if (last_ok && (hr.cyc.h0[0] != 0 || hr.cyc.h0[1] != 0) && bone_matrix(c, body, cyc_bone[i], b)) {
                float u[3] = {b[1], b[5], b[9]};
                float hx = u[0] * lastm[0] + u[1] * lastm[4] + u[2] * lastm[8], hz = u[0] * lastm[2] + u[1] * lastm[6] + u[2] * lastm[10];
                float seen = atan2f(hr.cyc.h0[1] * hx - hr.cyc.h0[0] * hz, hr.cyc.h0[0] * hx + hr.cyc.h0[1] * hz);
                if (isfinite(seen)) hr.cyc.steer_seen = seen, step = want - seen;
            }
        }
        while (step > 3.14159265f) step -= 6.2831853f;
        while (step < -3.14159265f) step += 6.2831853f;
        hr.cyc.done[i] = want;
        float zero[3] = {0, 0, 0}, D[12];
        rot_about(hr.cyc.axis[i], step, zero, D);
        memcpy((void *)(uintptr_t)(hr.cyc.node[i] + 0x68), D, 48);
    }
    if (cyc_report) {
        cyc_report = 0;
        char line[600];
        size_t n = 0;
        for (int i = 0; i < CYC_BONES; i++) {  // (each bone's up axis in the cycle's frame: turns when D is applied)
            float b[12];
            if (!bone_matrix(c, body, cyc_bone[i], b)) { n += (size_t)snprintf(line + n, sizeof line - n, " %s -", cyc_bone[i]); continue; }
            float u[3] = {b[1], b[5], b[9]};
            n += (size_t)snprintf(line + n, sizeof line - n, " %s up(x %.2f y %.2f z %.2f)%s", cyc_bone[i],
                                  u[0] * m[0] + u[1] * m[4] + u[2] * m[8], u[0] * m[1] + u[1] * m[5] + u[2] * m[9],
                                  u[0] * m[2] + u[1] * m[6] + u[2] * m[10], hr.cyc.on[i] ? "" : " (no node)");
        }
        LOG("[coop] cycle report: spin %.2f rad steer %.2f rad (handle seen at %.2f) at %.1f %.1f %.1f fake %.1f m/s %.0f deg/s |%s\n",
            hr.cyc.spin_f, hr.cyc.steer, hr.cyc.steer_seen, m[3], m[7], m[11], cyc_fake_speed, cyc_fake_turn, line);
    }
}
