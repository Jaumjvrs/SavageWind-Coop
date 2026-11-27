// Part of runtime/coop.c (included after coop/ragdoll.inc: uses the bone modifier nodes of aim.inc and the rotation
// helpers of ragdoll.inc): the avatar's right hand closes around a held two-handed weapon (2026-11-21: the
// other Link holding a hammer still floated a bit, not connected perfectly with his hand).
//
// Facts (PODDIAG 2026-11-18, see held_arm_bone in avatar.inc): with a two-handed sword / hammer drawn, the real Link's
// right wrist and Weapon_R are turned ~50 deg away from where the animation alone puts them (the game turns them by
// code), while the forearm Arm_2_R matches. HOLDARM sends the weapon relative to the forearm (right angle), HOLDGRIP puts
// its grip point on the avatar's own Weapon_R (right place); but the avatar's hand keeps the animation's wrist angle,
// so the handle crossed the fist at that angle instead of lying in it.
//
// Here (receiver only, no protocol change): while the partner's weapon is sent on Arm_2_R, one bone modifier node on
// the avatar's Wrist_R (and the same D on each armor puppet's Wrist_R: gloves / sleeves) turns the wrist so that the
// avatar's Weapon_R takes the weapon's own angle (the real Link holds a weapon with its origin and axes on Weapon_R).
// The node is applied by the game after the animation as L * D (bone-local, children follow: hand, fingers, Weapon_R).
//   M     = Wrist_R as read * D_prev^-1            (the wrist the animation alone gives)
//   Lw    = Wrist_R^-1 * Weapon_R                  (the skeleton's wrist -> weapon bone offset)
//   R_t   = R(Arm_2_R * rel) * R(Lw)^T             (the wrist angle that puts Weapon_R on the weapon's angle)
//   D     = [R(M)^T * R_t | 0]                     (a pure turn about the wrist: the hand stays on the forearm)
// Blended in over 0.15 s, out over 0.25 s; turns over GRIP_MAX_DEG are left alone (a bad frame).
// SAVAGEWIND_COOP_HOLDWRIST=0: off (the hand keeps the animation's angle, as before).
#define GRIP_IN_S 0.15f
#define GRIP_OUT_S 0.25f
#define GRIP_MAX_DEG 110.0f
static struct {
    uint64_t actor, last_ns, said_ns;
    float w;       // blend weight 0..1
    float q[4];    // the full turn (before blending), kept while blending out
    bool have_q;
    float d[12];   // the D written last on the avatar (the bone as read carries it)
    bool have_d;
} grip;
static struct { uint64_t node, actor; bool on, failed; } grip_node[1 + COOP_ARMOR_SLOTS];  // [0] avatar, [1..] armor

static bool grip_on(void) {
    static int c_ = -1;
    return env_on("SAVAGEWIND_COOP_HOLDWRIST", &c_);
}
static void grip_release_all(void) {
    for (int i = 0; i <= COOP_ARMOR_SLOTS; i++) bmod_release(&grip_node[i].node);
    memset(grip_node, 0, sizeof grip_node);
}
// Our node on actor a's Wrist_R (slot i), linked once; false when it cannot be.
static bool grip_link(Cpu *c, int i, uint64_t a) {
    if (grip_node[i].actor != a)
        bmod_release(&grip_node[i].node), grip_node[i].actor = a, grip_node[i].on = false, grip_node[i].failed = false;
    if (!a || grip_node[i].failed) return false;
    if (grip_node[i].on) return true;
    if (!grip_node[i].node && !(grip_node[i].node = bmod_new_named(c, "Wrist_R"))) return false;
    Cpu saved = *c;
    c->x[0] = a; c->x[1] = grip_node[i].node; c->x[2] = 0;
    guest_call(c, A_BMOD_ADD);
    *c = saved;
    grip_node[i].on = LD8(grip_node[i].node + 8) != 0;
    if (coop_look_log() || i == 0)
        LOG("[coop] look: hold wrist modifier on %s Wrist_R %s (HOLDWRIST)\n", i ? "armor puppet" : "avatar",
            grip_node[i].on ? "linked" : "NOT linked");
    if (!grip_node[i].on) grip_node[i].failed = true, bmod_release(&grip_node[i].node);  // (no Wrist_R: a head piece)
    return grip_node[i].on;
}
static void grip_frame(Cpu *c, const CoopState *rs, bool have) {
    bool live = grip_on() && avatar.actor && avatar_alive() && avatar.frames >= 6 && local_in_world();
    if (!live || grip.actor != avatar.actor) {  // (gone / reloading / a new avatar: its modifier list went with it)
        if (grip.actor) grip_release_all(), memset(&grip, 0, sizeof grip);
        if (!live) return;
        grip.actor = avatar.actor;
    }
    uint64_t now = plat_now_ns();
    float dt = grip.last_ns && now > grip.last_ns ? (float)((now - grip.last_ns) / 1e9) : 1 / 60.0f;
    if (dt > 0.1f) dt = 0.1f;
    grip.last_ns = now;
    const CoopGear *g = have ? &rs->gear[COOP_GEAR_WEAPON] : NULL;
    bool want = g && g->name[0] && g->bone == COOP_BONE_ARM_R && hold_grip_on() && rag.w <= 0;
    float wr[12], wp[12], ar[12], dinv[12], mn[12], wri[12], lw[12], tw[12];
    if (want && bone_matrix(c, avatar.actor, "Wrist_R", wr) && bone_matrix(c, avatar.actor, "Weapon_R", wp) &&
        bone_matrix(c, avatar.actor, "Arm_2_R", ar) && mtx_inv(wr, wri)) {
        if (grip.have_d && mtx_inv(grip.d, dinv)) mtx_mul(wr, dinv, mn);
        else memcpy(mn, wr, sizeof mn);
        mtx_mul(wri, wp, lw);
        mtx_mul(ar, g->rel, tw);
        float rm[9], rl[9], rt9[9], rt[9], rd[9], q[4];
        r9_of(tw, rt9), r9_of(lw, rl), r9_of(mn, rm);
        for (int i = 0; i < 3; i++)  // rt = R(tw) * R(lw)^T
            for (int j = 0; j < 3; j++) rt[i * 3 + j] = rt9[i * 3] * rl[j * 3] + rt9[i * 3 + 1] * rl[j * 3 + 1] + rt9[i * 3 + 2] * rl[j * 3 + 2];
        r9_tmul(rm, rt, rd);  // rd = R(M)^T * rt
        q_from_r9(rd, q);
        q_norm(q);
        float deg = 2 * acosf(fminf(1.0f, fabsf(q[3]))) * 57.29578f;
        if (isfinite(deg) && deg <= GRIP_MAX_DEG) {
            if (grip.have_q) q_slerp(grip.q, q, dt * 25 > 1 ? 1 : dt * 25, grip.q);  // (a little smoothing)
            else memcpy(grip.q, q, sizeof q);
            grip.have_q = true;
            if (coop_look_log() && now > grip.said_ns)
                grip.said_ns = now + 3000000000ull, LOG("[coop] look: %s held: avatar wrist turned %.0f deg (HOLDWRIST)\n", g->name, deg);
        }
    }
    grip.w += want && grip.have_q ? dt / GRIP_IN_S : -dt / GRIP_OUT_S;
    grip.w = grip.w > 1 ? 1 : grip.w < 0 ? 0 : grip.w;
    if (grip.w <= 0 && !grip.have_d) return;  // (nothing written yet / back to the identity already)
    float id[4] = {0, 0, 0, 1}, qb[4], r9[9], dn[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (grip.w > 0 && grip.have_q) {
        q_slerp(id, grip.q, grip.w, qb);
        r9_from_q(qb, r9);
        for (int i = 0; i < 3; i++) dn[i * 4] = r9[i * 3], dn[i * 4 + 1] = r9[i * 3 + 1], dn[i * 4 + 2] = r9[i * 3 + 2];
    }
    for (int i = 0; i <= COOP_ARMOR_SLOTS; i++) {
        uint64_t a = i == 0 ? avatar.actor
                     : armor_pup[i - 1].actor && armor_pup[i - 1].frames >= 6 && pup_alive(&armor_pup[i - 1]) ? armor_pup[i - 1].actor : 0;
        if (grip_link(c, i, a)) memcpy((void *)(uintptr_t)(grip_node[i].node + 0x68), dn, 48);
    }
    memcpy(grip.d, dn, sizeof dn);
    grip.have_d = grip.w > 0;
    if (grip.w <= 0) grip.have_q = false;
}
