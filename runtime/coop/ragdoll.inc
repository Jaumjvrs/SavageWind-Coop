// Part of runtime/coop.c (included after coop/aim.inc: uses bone_matrix, bmod_new_named, the A_BMOD_* calls): the
// avatar copies the other player's ragdoll pose (2026-11-15: when the other Link goes tumbling, like when
// he gets hit hard, he did not go into the ragdoll state on the other screen).
//
// A hard hit (DamageL) throws Link into Havok ragdoll: AS SlippingDown is one static 1-frame pose ("Rolldown", the
// ragdoll's target pose) and the physics bodies (Actor/RagdollConfigList/GameROMPlayer: Ragdoll_Skl_Root, _Spine_1,
// _Head, _Arm_1/_Arm_2/_Wrist L R, _Leg_1/_Leg_2/_Ankle L R; Physics/Ragdoll/Link/Link.hkrg) drive the bones. Our
// avatar has no ragdoll, so it held the stiff pose and slid.
//
// Sender (rag_send): while our player plays a ragdoll AS (SlippingDown*, DeadRagdoll, and DamageL, which leads into
// it), the world matrices of rag_bones[] (bone_matrix: the final pose the model draws) go out relative to the frame F
// built from the position / yaw the state packet sends: rotation quaternions, plus Skl_Root's position. Protocol 48
// ("SWCR", CoopPose). Then a short tail with flags 0 so the receiver blends out.
//
// Receiver (rag_recv): one bone modifier node per bone on the avatar (and on each armor puppet, same D). The game
// applies a node after the animation (FUN_7101096748, node+9 == 0): the bone's LOCAL matrix L (unit vt+0x60, relative
// to its parent) becomes L * D, so children follow. For bone c with listed ancestor p (from the model's skeleton:
// parent index u16 at +0x1a of each 0x50-byte bone record, the same walk the game's model-space branch does):
//   chain = Wp^-1 * Wc * Dc_prev^-1   (what the animation puts between p and c this frame; Wp, Wc as read: final)
//   D     = chain^-1 * S,  S = [Rp_target^T * Rc_target | chain.t]   (a pure turn: D.t = 0)
// and for Skl_Root (ancestor = the actor frame A): chain = A_prev^-1 * Wroot * D_prev^-1, S = [R_target | root_target].
// The result: every bone's rotation relative to its parent is the sender's, the root sits where the sender's sat.
// Blended in over 0.15 s, out over 0.35 s (D slerped toward the identity).
// SAVAGEWIND_COOP_RAGDOLL=0: off (nothing sent, nothing applied). Dev: `coop ragdev <seconds>` sends our pose for that
// long whatever the AS (to compare avatar and player with `coop bones`), `coop ragdev 0` stops.
static const char *const rag_bones[COOP_POSE_BONES] = {
    "Skl_Root", "Waist", "Spine_1", "Spine_2", "Neck", "Head",
    "Clavicle_L", "Arm_1_L", "Arm_2_L", "Wrist_L", "Clavicle_R", "Arm_1_R", "Arm_2_R", "Wrist_R",
    "Leg_1_L", "Leg_2_L", "Ankle_L", "Toe_L", "Leg_1_R", "Leg_2_R", "Ankle_R", "Toe_R"};
// (guessed hierarchy, used only when the model's skeleton cannot be read: list index of the parent, -1 = actor frame)
static const int8_t rag_parent_guess[COOP_POSE_BONES] = {-1, 0, 1, 2, 3, 4, 3, 6, 7, 8, 3, 10, 11, 12, 1, 14, 15, 16, 1, 18, 19, 20};
#define RAG_TAIL_NS 400000000ull
#define RAG_IN_S 0.15f
#define RAG_OUT_S 0.35f
static volatile uint64_t rag_dev_until_ns;

static bool rag_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_RAGDOLL");
        on = !(e && e[0] == '0');
    }
    return on;
}

// ---- small quaternion helpers (x, y, z, w; rotation matrices as 9 floats, rows)
static void q_from_r9(const float r[9], float q[4]) {
    float t = r[0] + r[4] + r[8];
    if (t > 0) {
        float s = sqrtf(t + 1) * 2;
        q[3] = s / 4, q[0] = (r[7] - r[5]) / s, q[1] = (r[2] - r[6]) / s, q[2] = (r[3] - r[1]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        float s = sqrtf(1 + r[0] - r[4] - r[8]) * 2;
        q[3] = (r[7] - r[5]) / s, q[0] = s / 4, q[1] = (r[1] + r[3]) / s, q[2] = (r[2] + r[6]) / s;
    } else if (r[4] > r[8]) {
        float s = sqrtf(1 + r[4] - r[0] - r[8]) * 2;
        q[3] = (r[2] - r[6]) / s, q[0] = (r[1] + r[3]) / s, q[1] = s / 4, q[2] = (r[5] + r[7]) / s;
    } else {
        float s = sqrtf(1 + r[8] - r[0] - r[4]) * 2;
        q[3] = (r[3] - r[1]) / s, q[0] = (r[2] + r[6]) / s, q[1] = (r[5] + r[7]) / s, q[2] = s / 4;
    }
}
static void q_norm(float q[4]) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(l > 1e-6f)) { q[0] = q[1] = q[2] = 0, q[3] = 1; return; }
    for (int k = 0; k < 4; k++) q[k] /= l;
}
static void r9_from_q(const float q0[4], float r[9]) {
    float q[4] = {q0[0], q0[1], q0[2], q0[3]};
    q_norm(q);
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}
static void q_slerp(const float a[4], const float b0[4], float t, float o[4]) {
    float b[4] = {b0[0], b0[1], b0[2], b0[3]}, d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) d = -d, b[0] = -b[0], b[1] = -b[1], b[2] = -b[2], b[3] = -b[3];
    float wa = 1 - t, wb = t;
    if (d < 0.9995f) {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
    }
    for (int k = 0; k < 4; k++) o[k] = wa * a[k] + wb * b[k];
    q_norm(o);
}
static void r9_of(const float m[12], float r[9]) {
    r[0] = m[0], r[1] = m[1], r[2] = m[2], r[3] = m[4], r[4] = m[5], r[5] = m[6], r[6] = m[8], r[7] = m[9], r[8] = m[10];
}
static void r9_mul(const float a[9], const float b[9], float o[9]) {  // o = a * b
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}
static void r9_tmul(const float a[9], const float b[9], float o[9]) {  // o = a^T * b
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o[i * 3 + j] = a[i] * b[j] + a[3 + i] * b[3 + j] + a[6 + i] * b[6 + j];
}

// ---- sender
static bool rag_as(const char *as) {
    return !strncmp(as, "SlippingDown", 12) || !strcmp(as, "DeadRagdoll") || !strcmp(as, "DamageL");
}
static void rag_send(Cpu *c) {
    static bool sending, active_was;
    static uint64_t tail_until_ns, log_ns;
    static unsigned said;
    uint64_t now = plat_now_ns(), pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    char as[40] = "";
    bool ok = rag_on() && pinfo && in_guest(pa) && local_in_world();
    if (ok) player_as_name(c, pa, as, sizeof as);
    bool active = ok && (rag_as(as) || now < rag_dev_until_ns);
    if (active) tail_until_ns = now + RAG_TAIL_NS;
    if (!ok || (!active && now >= tail_until_ns)) {
        if (sending) {
            coop_net_set_pose(NULL);
            if (said++ < 60) LOG("[coop] ragdoll: our Link is up again (AS %s): pose no longer sent\n", as[0] ? as : "-");
        }
        sending = active_was = false;
        return;
    }
    // F: the sent position (PlayerInfo +0x88) and yaw (the actor matrix), as publish_local / write_mtx make them
    float pos[3], am[12];
    memcpy(pos, (void *)(uintptr_t)(pinfo + 0x88), 12);
    memcpy(am, (void *)(uintptr_t)(pa + ACTOR_MTX), sizeof am);
    float yaw = atan2f(am[2], am[0]) * 180 / 3.14159265f, a = yaw * 3.14159265f / 180, cs = cosf(a), sn = sinf(a);
    float f[12] = {cs, 0, sn, pos[0], 0, 1, 0, pos[1], -sn, 0, cs, pos[2]}, fi[12], fr[9];
    if (!mtx_inv(f, fi)) return;
    r9_of(f, fr);
    CoopPose p;
    memset(&p, 0, sizeof p);
    p.flags = active ? COOP_POSE_ACTIVE : 0;
    p.count = COOP_POSE_BONES;
    float head[3] = {0, 0, 0}, rootw[3] = {0, 0, 0};
    for (int i = 0; i < COOP_POSE_BONES; i++) {
        float w[12], r9[9], rr[9], q[4];
        if (!bone_matrix(c, pa, rag_bones[i], w)) continue;
        r9_of(w, rr);
        r9_tmul(fr, rr, r9);  // (F's rotation^T * the bone's)
        horse_orthonormal(r9);
        q_from_r9(r9, q);
        q_norm(q);
        bool fine = true;
        for (int k = 0; k < 4; k++) fine = fine && isfinite(q[k]);
        if (!fine) continue;
        for (int k = 0; k < 4; k++) p.q[i][k] = (int16_t)lrintf(q[k] * 32767.0f);
        p.mask |= 1u << i;
        float rel[12];
        mtx_mul(fi, w, rel);
        if (i == 0) {
            p.root[0] = rel[3], p.root[1] = rel[7], p.root[2] = rel[11];
            rootw[0] = w[3], rootw[1] = w[7], rootw[2] = w[11];
        }
        if (i == 5) head[0] = rel[3], head[1] = rel[7], head[2] = rel[11];
    }
    for (int k = 0; k < 3; k++)
        if (!isfinite(p.root[k]) || fabsf(p.root[k]) > 20) p.root[k] = 0, p.mask &= ~1u;
    if (!(p.mask & 1u)) memset(p.q[0], 0, sizeof p.q[0]);
    coop_net_set_pose(&p);
    if (active && !active_was && said++ < 60) {
        int n = 0;
        for (int i = 0; i < COOP_POSE_BONES; i++) n += p.mask >> i & 1;
        LOG("[coop] ragdoll: our Link tumbles (AS %s%s): sending our pose, %d of %d bones\n", as,
            now < rag_dev_until_ns ? ", dev" : "", n, COOP_POSE_BONES);
    }
    if (now >= log_ns && said < 60) {  // (research: does the actor / PlayerInfo follow the body? does the pose move?)
        log_ns = now + 250000000ull;
        said++;
        LOG("[coop] ragdoll: send AS %s flags %d | root in F %.2f %.2f %.2f, head in F %.2f %.2f %.2f | PlayerInfo %.2f %.2f %.2f "
            "actor %.2f %.2f %.2f Skl_Root %.2f %.2f %.2f\n",
            as, p.flags, p.root[0], p.root[1], p.root[2], head[0], head[1], head[2], pos[0], pos[1], pos[2], am[3], am[7],
            am[11], rootw[0], rootw[1], rootw[2]);
    }
    sending = true, active_was = active;
}

// ---- receiver
struct RagNode { uint64_t node; bool on, failed; float d[12]; bool have_d; };
static struct {
    uint64_t actor;                         // the avatar these nodes are for
    struct RagNode n[COOP_POSE_BONES];
    int parent[COOP_POSE_BONES];            // list index of the nearest listed ancestor, -1 = the actor frame
    bool parents_done;
    float w;                                // blend weight 0..1
    float q[COOP_POSE_BONES][4], root[3];   // smoothed target
    uint32_t mask;
    bool have_target;
    uint64_t last_ns;
} rag;
static struct { uint64_t actor; uint64_t node[COOP_POSE_BONES]; bool on[COOP_POSE_BONES], failed[COOP_POSE_BONES]; } rag_pup[COOP_ARMOR_SLOTS];

static void rag_node_del(Cpu *c, uint64_t node, uint64_t actor) {
    Cpu saved = *c;
    c->x[0] = node; c->x[1] = actor + 0x4d8;
    guest_call(c, A_BMOD_DEL);
    *c = saved;
}
static bool rag_node_add(Cpu *c, uint64_t actor, uint64_t node) {
    Cpu saved = *c;
    c->x[0] = actor; c->x[1] = node; c->x[2] = 0;
    guest_call(c, A_BMOD_ADD);
    *c = saved;
    return LD8(node + 8) != 0;
}
static void rag_unlink_all(Cpu *c) {
    bool av = rag.actor && rag.actor == avatar.actor && avatar_alive();
    int n = 0;
    for (int i = 0; i < COOP_POSE_BONES; i++) {
        if (rag.n[i].on && av) rag_node_del(c, rag.n[i].node, rag.actor), n++;
        rag.n[i].on = false, rag.n[i].have_d = false;
    }
    for (int s = 0; s < COOP_ARMOR_SLOTS; s++)
        for (int i = 0; i < COOP_POSE_BONES; i++) {
            if (rag_pup[s].on[i] && rag_pup[s].actor == armor_pup[s].actor && pup_alive(&armor_pup[s]))
                rag_node_del(c, rag_pup[s].node[i], rag_pup[s].actor);
            rag_pup[s].on[i] = false;
        }
    static unsigned said;
    if (n && said++ < 40) LOG("[coop] ragdoll: the avatar's pose is its animation again (%d bone modifiers off)\n", n);
}

// The skeleton record of a bone a node is bound to: model actor+0x4e0, units *(model+0x40), unit **(units + u*8),
// skeleton *(*(unit+0x118)), bones *(skeleton+0x18), 0x50 each: +0x18 index, +0x1a parent (0xffff: none).
static uint64_t rag_skel_bones(uint64_t actor, int unit) {
    uint64_t model = LD64(actor + 0x4e0), units = in_guest(model) ? LD64(model + 0x40) : 0;
    uint64_t up = in_guest(units) ? LD64(units + (uint64_t)unit * 8) : 0, u = in_guest(up) ? LD64(up) : 0;
    uint64_t sp = in_guest(u) ? LD64(u + 0x118) : 0, sk = in_guest(sp) ? LD64(sp) : 0;
    uint64_t bones = in_guest(sk) ? LD64(sk + 0x18) : 0;
    return in_guest(bones) ? bones : 0;
}
static void rag_parents(void) {
    int idx[COOP_POSE_BONES], unit = -1;
    for (int i = 0; i < COOP_POSE_BONES; i++) {
        idx[i] = rag.n[i].on ? (int)(int16_t)LD16(rag.n[i].node + 0x62) : -1;
        if (rag.n[i].on && unit < 0) unit = (int)(int16_t)LD16(rag.n[i].node + 0x60);
    }
    uint64_t bones = unit >= 0 ? rag_skel_bones(rag.actor, unit) : 0;
    bool good = bones != 0;
    for (int i = 0; i < COOP_POSE_BONES && good; i++) {
        rag.parent[i] = -1;
        if (idx[i] < 0) continue;
        if ((int)LD16(bones + (uint64_t)idx[i] * 0x50 + 0x18) != idx[i]) { good = false; break; }
        int b = (int)LD16(bones + (uint64_t)idx[i] * 0x50 + 0x1a);
        for (int steps = 0; b != 0xffff && steps < 64 && good; steps++) {
            if ((int)LD16(bones + (uint64_t)b * 0x50 + 0x18) != b) { good = false; break; }
            int hit = -1;
            for (int j = 0; j < COOP_POSE_BONES; j++)
                if (idx[j] == b) hit = j;
            if (hit >= 0) { rag.parent[i] = hit; break; }
            b = (int)LD16(bones + (uint64_t)b * 0x50 + 0x1a);
        }
    }
    if (!good)
        for (int i = 0; i < COOP_POSE_BONES; i++) {  // (fallback: the guessed hierarchy, skipping bones not there)
            int p = rag_parent_guess[i];
            while (p >= 0 && !rag.n[p].on) p = rag_parent_guess[p];
            rag.parent[i] = p;
        }
    char line[900];
    size_t n = 0;
    for (int i = 0; i < COOP_POSE_BONES; i++)
        if (rag.n[i].on)
            n += (size_t)snprintf(line + n, sizeof line - n, " %s(%d)<-%s", rag_bones[i], idx[i],
                                  rag.parent[i] >= 0 ? rag_bones[rag.parent[i]] : "actor");
    LOG("[coop] ragdoll: avatar hierarchy (%s):%s\n", good ? "from its skeleton" : "GUESSED: skeleton not readable", line);
    rag.parents_done = true;
}

static void rag_pups_apply(Cpu *c, int i, const float d[12]) {
    static unsigned said;
    for (int s = 0; s < COOP_ARMOR_SLOTS; s++) {
        uint64_t a = armor_pup[s].actor && armor_pup[s].frames >= 6 && pup_alive(&armor_pup[s]) ? armor_pup[s].actor : 0;
        if (rag_pup[s].actor != a) {
            rag_pup[s].actor = a;
            for (int k = 0; k < COOP_POSE_BONES; k++) bmod_release(&rag_pup[s].node[k]);
            memset(rag_pup[s].node, 0, sizeof rag_pup[s].node);
            memset(rag_pup[s].on, 0, sizeof rag_pup[s].on);
            memset(rag_pup[s].failed, 0, sizeof rag_pup[s].failed);
        }
        if (!a || rag_pup[s].failed[i]) continue;
        if (!rag_pup[s].on[i]) {
            if (!rag_pup[s].node[i] && !(rag_pup[s].node[i] = bmod_new_named(c, rag_bones[i]))) continue;
            rag_pup[s].on[i] = rag_node_add(c, a, rag_pup[s].node[i]);
            if (!rag_pup[s].on[i]) { rag_pup[s].failed[i] = true, bmod_release(&rag_pup[s].node[i]); continue; }  // (bone not in it)
            if (i == 0 && said++ < 20) LOG("[coop] ragdoll: puppet %d (%llx) follows the pose\n", s, (unsigned long long)a);
        }
        memcpy((void *)(uintptr_t)(rag_pup[s].node[i] + 0x68), d, 48);
    }
}

static void rag_release_all(void) {  // (B7: nodes of a gone avatar / puppet go to the pool)
    for (int i = 0; i < COOP_POSE_BONES; i++) bmod_release(&rag.n[i].node);
    for (int s = 0; s < COOP_ARMOR_SLOTS; s++)
        for (int i = 0; i < COOP_POSE_BONES; i++) bmod_release(&rag_pup[s].node[i]);
}
static void rag_recv(Cpu *c) {
    bool live = rag_on() && avatar.actor && avatar_alive() && avatar.frames >= 6 && local_in_world();
    if (!live) {  // (gone or reloading: its modifier list went with it)
        if (rag.actor) rag_release_all(), memset(&rag, 0, sizeof rag), memset(rag_pup, 0, sizeof rag_pup);
        return;
    }
    if (rag.actor != avatar.actor) {  // (a new avatar: fresh nodes)
        rag_release_all();
        memset(&rag, 0, sizeof rag);
        memset(rag_pup, 0, sizeof rag_pup);
        rag.actor = avatar.actor;
    }
    uint64_t now = plat_now_ns();
    float dt = rag.last_ns && now > rag.last_ns ? (float)((now - rag.last_ns) / 1e9) : 1 / 60.0f;
    if (dt > 0.1f) dt = 0.1f;
    rag.last_ns = now;
    CoopPose p;
    double age = 99;
    bool fresh = coop_net_remote_pose(&p, &age) && age < 0.5;
    bool want = fresh && (p.flags & COOP_POSE_ACTIVE) && p.mask;
    static unsigned said;
    if (fresh && p.mask) {
        float k = dt * 20 > 1 || rag.w <= 0 || !rag.have_target ? 1 : dt * 20;
        for (int i = 0; i < COOP_POSE_BONES; i++) {
            if (!(p.mask >> i & 1)) continue;
            float q[4] = {p.q[i][0] / 32767.0f, p.q[i][1] / 32767.0f, p.q[i][2] / 32767.0f, p.q[i][3] / 32767.0f};
            q_norm(q);
            if (!(rag.mask >> i & 1) || k >= 1) memcpy(rag.q[i], q, sizeof q);
            else q_slerp(rag.q[i], q, k, rag.q[i]);
        }
        if (p.mask & 1u)
            for (int k3 = 0; k3 < 3; k3++)
                rag.root[k3] = (rag.mask & 1u) && k < 1 ? rag.root[k3] + (p.root[k3] - rag.root[k3]) * k : p.root[k3];
        rag.mask = p.mask, rag.have_target = true;
    }
    float w0 = rag.w;
    rag.w += want ? dt / RAG_IN_S : -dt / RAG_OUT_S;
    rag.w = rag.w > 1 ? 1 : rag.w < 0 ? 0 : rag.w;
    if (!rag.have_target) rag.w = 0;
    if (w0 <= 0 && rag.w > 0 && said++ < 60) LOG("[coop] ragdoll: the other player tumbles: the avatar takes their pose\n");
    if (rag.w <= 0) {
        if (w0 > 0 || rag.n[0].on) rag_unlink_all(c);
        rag.have_target = rag.have_target && fresh;
        return;
    }
    // nodes (bound once per avatar; bones the model lacks stay off)
    bool added = false;
    for (int i = 0; i < COOP_POSE_BONES; i++) {
        struct RagNode *n = &rag.n[i];
        if (n->on || n->failed) continue;
        if (!n->node && !(n->node = bmod_new_named(c, rag_bones[i]))) continue;
        n->on = rag_node_add(c, rag.actor, n->node), n->have_d = false, added = true;
        if (!n->on) n->failed = true, bmod_release(&n->node);
    }
    if (added || !rag.parents_done) rag_parents();
    // read the final pose of last frame (with our D in it)
    float W[COOP_POSE_BONES][12];
    bool have[COOP_POSE_BONES];
    for (int i = 0; i < COOP_POSE_BONES; i++) have[i] = rag.n[i].on && bone_matrix(c, rag.actor, rag_bones[i], W[i]);
    // (the root's reference: the avatar matrix its bones were computed with; the next pose uses the one avatar_frame
    // just wrote, and S is relative to that frame, so the body moves with the avatar)
    float a_now[12], a_prev[12], ap_inv[12];
    memcpy(a_now, (void *)(uintptr_t)(rag.actor + ACTOR_MTX), sizeof a_now);
    memcpy(a_prev, avatar.have_prev ? avatar.mtx_prev : a_now, sizeof a_prev);
    if (!mtx_inv(a_prev, ap_inv)) return;
    int applied = 0;
    for (int i = 0; i < COOP_POSE_BONES; i++) {
        struct RagNode *n = &rag.n[i];
        if (!n->on || !have[i] || !(rag.mask >> i & 1)) continue;
        int pi = rag.parent[i];
        if (pi >= 0 && (!have[pi] || !(rag.mask >> pi & 1))) continue;  // (no reference: leave this bone as it is)
        float pinv[12], t1[12], chain[12], dinv[12], cinv[12];
        if (pi >= 0) { if (!mtx_inv(W[pi], pinv)) continue; }
        else memcpy(pinv, ap_inv, sizeof pinv);
        mtx_mul(pinv, W[i], t1);
        if (n->have_d && mtx_inv(n->d, dinv)) mtx_mul(t1, dinv, chain);
        else memcpy(chain, t1, sizeof chain);
        if (!mtx_inv(chain, cinv)) continue;
        // target relative rotation S
        float rc[9], rs[9];
        r9_from_q(rag.q[i], rc);
        if (pi >= 0) {
            float rp[9];
            r9_from_q(rag.q[pi], rp);
            r9_tmul(rp, rc, rs);
        } else memcpy(rs, rc, sizeof rs);  // (relative to F = the avatar's frame)
        float s[12] = {rs[0], rs[1], rs[2], chain[3], rs[3], rs[4], rs[5], chain[7], rs[6], rs[7], rs[8], chain[11]};
        if (i == 0 && pi < 0) s[3] = rag.root[0], s[7] = rag.root[1], s[11] = rag.root[2];
        float d[12], dr[9], dq[4], id[4] = {0, 0, 0, 1}, bq[4];
        mtx_mul(cinv, s, d);
        r9_of(d, dr);
        horse_orthonormal(dr);
        q_from_r9(dr, dq);
        q_norm(dq);
        q_slerp(id, dq, rag.w, bq);
        r9_from_q(bq, dr);
        float dn[12] = {dr[0], dr[1], dr[2], i == 0 ? d[3] * rag.w : 0, dr[3], dr[4], dr[5], i == 0 ? d[7] * rag.w : 0,
                        dr[6], dr[7], dr[8], i == 0 ? d[11] * rag.w : 0};
        bool fine = true;
        for (int k = 0; k < 12; k++) fine = fine && isfinite(dn[k]) && fabsf(dn[k]) < 50;
        if (!fine) continue;
        memcpy((void *)(uintptr_t)(n->node + 0x68), dn, sizeof dn);
        memcpy(n->d, dn, sizeof dn);
        n->have_d = true;
        rag_pups_apply(c, i, dn);
        applied++;
    }
    static uint64_t log_ns;
    if (now >= log_ns && said < 60) {
        log_ns = now + 500000000ull;
        said++;
        float hd[3] = {0, 0, 0};
        if (have[5]) {
            float rel[12];
            mtx_mul(ap_inv, W[5], rel);
            hd[0] = rel[3], hd[1] = rel[7], hd[2] = rel[11];
        }
        LOG("[coop] ragdoll: avatar pose weight %.2f, %d bones driven, root target %.2f %.2f %.2f, head now %.2f %.2f %.2f "
            "(packet age %.2f s, %s)\n",
            rag.w, applied, rag.root[0], rag.root[1], rag.root[2], hd[0], hd[1], hd[2], age, want ? "tumbling" : "ended");
    }
}

static void ragdoll_frame(Cpu *c) {
    rag_send(c);
    rag_recv(c);
}
