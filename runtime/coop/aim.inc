// Part of runtime/coop.c (included after coop/horse.inc: uses bone_matrix): the avatar's upper body follows the other
// player's aim (2026-11-14: the partner's aim animation did not follow where they aim). The player's bow AS
// (BowCharge...) have no inputs: the game bends Link's spine toward the aim in code,
// with the actor's look-at object (BoneControl "Spine" nodes of Actor/BoneControl/Player_Link.bbonectrl: Spine_1 in
// battle mode, down 70 / up 50 / sideways 45 degrees; Neck, Head). It is applied after the animation (bone modifiers),
// which is why writing Spine_1 ourselves did nothing (tried first: the animation overrode it).
//
// Found in the code (2026-11-14): the look-at object is obj = *(u64 *)*(u64 *)(actor +
// 0x5a0) (made at actor init when the ActorLink BoneControlUser has spine nodes); obj+0x18 = the world target,
// obj+0x9c u16 bit 0x10 = battle mode, obj+0xe4 u32 bit 0x2 = look request, 0x40 = no yaw, 0x80 = no pitch. The player
// calls FUN_7100aa5840(actor, float target[3], pitch, yaw) every frame while it aims; FUN_7100aa58e0(actor) ends
// battle mode, FUN_7100aa8110(actor) drops the request (the spine eases back).
//
// Sender (local_aim): while the player's look request is on in battle mode, the direction from its chest to the
// target (pitch, yaw from the body) goes in the state packet. Receiver (aim_frame, below): our own bone modifier
// turns the avatar's Spine_1 that way (within the player's battle limits).
// SAVAGEWIND_COOP_AIM=0: off. Dev: `coop aimdev <pitch|999>` forces an avatar aim, `coop bones <player|avatar>`.
static volatile float aim_dev_pitch = 999;
static volatile int bones_dev;  // 1 = player, 2 = avatar

static float AIMF(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }

static bool aim_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_AIM");
        on = !(e && e[0] == '0');
    }
    return on;
}
static uint64_t look_obj(uint64_t actor) {
    uint64_t h = in_guest(actor) ? LD64(actor + 0x5a0) : 0, o = in_guest(h) ? LD64(h) : 0;
    return in_guest(o) ? o : 0;
}
// The chest (Spine_2, else 1.2 m above the actor) and the body's horizontal forward / right (unit) from its matrix.
static bool aim_frame_of(Cpu *c, uint64_t actor, float chest[3], float fwd[3], float right[3]) {
    const float *m = (const float *)(uintptr_t)(actor + ACTOR_MTX);
    float b[12];
    if (bone_matrix(c, actor, "Spine_2", b)) chest[0] = b[3], chest[1] = b[7], chest[2] = b[11];
    else chest[0] = m[3], chest[1] = m[7] + 1.2f, chest[2] = m[11];
    float fl = sqrtf(m[2] * m[2] + m[10] * m[10]);
    if (!(fl > 1e-3f) || !isfinite(chest[0] + chest[1] + chest[2])) return false;
    fwd[0] = m[2] / fl, fwd[1] = 0, fwd[2] = m[10] / fl;
    right[0] = fwd[2], right[1] = 0, right[2] = -fwd[0];  // (fwd turned 90 degrees about +y; the receiver uses the same)
    return true;
}

static void bones_log(Cpu *c, uint64_t actor, const char *who) {
    // (positions in the actor's own frame: x / y / z along its matrix columns, so the player and the avatar compare)
    static const char *const names[] = {"Skl_Root", "Spine_1", "Spine_2", "Neck", "Head", "Clavicle_L", "Arm_1_L",
                                        "Arm_2_L", "Clavicle_R", "Arm_2_R", "Weapon_L", "Weapon_R"};
    char line[1200];
    size_t n = 0;
    const float *am = (const float *)(uintptr_t)(actor + ACTOR_MTX);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        float b[12];
        if (bone_matrix(c, actor, names[i], b)) {
            float d[3] = {b[3] - am[3], b[7] - am[7], b[11] - am[11]};
            n += (size_t)snprintf(line + n, sizeof line - n, " %s %.2f %.2f %.2f", names[i],
                                  am[0] * d[0] + am[4] * d[1] + am[8] * d[2], am[1] * d[0] + am[5] * d[1] + am[9] * d[2],
                                  am[2] * d[0] + am[6] * d[1] + am[10] * d[2]);
        } else
            n += (size_t)snprintf(line + n, sizeof line - n, " %s -", names[i]);
    }
    uint64_t lo = look_obj(actor);  // (the game's own spine nodes: count, then bone / yaw / pitch / D rows)
    if (lo) {
        int cnt = (int)LD32(lo + 0x28);
        uint64_t arr = LD64(lo + 0x30);
        LOG("[coop] bones %s: look-at nodes %d (neck %d) target %.2f %.2f %.2f extra %.2f %.2f %.2f\n", who, cnt,
            (int)LD32(lo + 0x38), AIMF(lo + 0x18), AIMF(lo + 0x1c), AIMF(lo + 0x20), AIMF(lo + 0x90), AIMF(lo + 0x94),
            AIMF(lo + 0x98));
        for (int k = 0; in_guest(arr) && k < cnt && k < 8; k++) {
            uint64_t nd = arr + (uint64_t)k * 0x180;
            const float *D = (const float *)(uintptr_t)(nd + 0x68);
            LOG("[coop] bones %s: node %d bone %d/%d yaw %.3f pitch %.3f D %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f\n",
                who, k, (int)(int16_t)LD16(nd + 0x60), (int)(int16_t)LD16(nd + 0x62), AIMF(nd + 0x134), AIMF(nd + 0x138),
                D[0], D[1], D[2], D[4], D[5], D[6], D[8], D[9], D[10]);
        }
    }
    float hd[12], s1[12], ch[3], fw[3], rt[3];  // (the head from Spine_1 along the body: forward / up / right)
    if (bone_matrix(c, actor, "Head", hd) && bone_matrix(c, actor, "Spine_1", s1) && aim_frame_of(c, actor, ch, fw, rt)) {
        float d[3] = {hd[3] - s1[3], hd[7] - s1[7], hd[11] - s1[11]};
        n += (size_t)snprintf(line + n, sizeof line - n, " head fwd %.3f up %.3f right %.3f",
                              d[0] * fw[0] + d[2] * fw[2], d[1], d[0] * rt[0] + d[2] * rt[2]);
    }
    uint64_t o = look_obj(actor);
    const float *m = (const float *)(uintptr_t)(actor + ACTOR_MTX);
    LOG("[coop] bones %s: actor %.3f %.3f %.3f%s look-at %s flags %x mode %x\n", who, m[3], m[7], m[11], line,
        o ? "yes" : "none", o ? LD32(o + 0xe4) : 0, o ? LD16(o + 0x9c) : 0);
}

// (sender, from avatar.inc local state) the player's aim, or aim_flags 0
static void local_aim(Cpu *c, uint64_t pa, CoopState *s) {
    s->aim_pitch = 0, s->aim_yaw = 0, s->aim_flags = 0;
    uint64_t o = aim_on() ? look_obj(pa) : 0;
    if (!o) return;
    uint32_t f = LD32(o + 0xe4);
    if (!(f & 2) || !(LD16(o + 0x9c) & 0x10)) return;
    float t[3], ch[3], fw[3], rt[3];
    memcpy(t, (void *)(uintptr_t)(o + 0x18), 12);
    if (!isfinite(t[0] + t[1] + t[2]) || !aim_frame_of(c, pa, ch, fw, rt)) return;
    float d[3] = {t[0] - ch[0], t[1] - ch[1], t[2] - ch[2]};
    float h = sqrtf(d[0] * d[0] + d[2] * d[2]);
    if (!(h + fabsf(d[1]) > 0.1f)) return;
    float pitch = atan2f(d[1], h) * 57.29578f, yaw = atan2f(d[0] * rt[0] + d[2] * rt[2], d[0] * fw[0] + d[2] * fw[2]) * 57.29578f;
    // (2026-11-14, measured with `coop bones`: the game turns only its battle spine node (Spine_1) and hardly sideways
    // (<= 6 deg; the body faces the aim). The direction to the target gave wild yaws when aiming steeply (91 seen: the
    // the aim looked way off when aiming at the sky). Send the angles the game itself applied to that node instead: our
    // receiver's turn with pitch = node pitch and yaw = node yaw (degrees) put every bone within ~2 cm of the real one.)
    float az = yaw;  // (the aim's direction from the body's heading, degrees -180..180)
    int cnt = (int)LD32(o + 0x28);
    uint64_t arr = LD64(o + 0x30);
    float best = -1;
    for (int k = 0; in_guest(arr) && k < cnt && k < 8; k++) {
        float ny = AIMF(arr + (uint64_t)k * 0x180 + 0x134), np = AIMF(arr + (uint64_t)k * 0x180 + 0x138);
        if (!isfinite(ny) || !isfinite(np) || fabsf(ny) > 3.2f || fabsf(np) > 3.2f) continue;
        if (fabsf(ny) + fabsf(np) > best) best = fabsf(ny) + fabsf(np), pitch = np * 57.29578f, yaw = ny * 57.29578f;
    }
    pitch = pitch > 90 ? 90 : pitch < -90 ? -90 : pitch;
    yaw = yaw > 127 ? 127 : yaw < -127 ? -127 : yaw;
    // (2026-11-14, horseback: "if he is aiming behind the axis gets reversed") With the game's no-yaw flag (0x40, on
    // horseback) the spine only tips, about an axis turned toward the aim (the game: obj+0x94 = target azimuth - body
    // azimuth); the body twist is the AimHorseLower clip (astree.inc). Then aim_yaw carries that azimuth / 2.
    if (f & 0x40) yaw = az * 0.5f;
    s->aim_pitch = (int8_t)lrintf(pitch), s->aim_yaw = (int8_t)lrintf(yaw);
    s->aim_flags = COOP_AIM_ON | (f & 0x80 ? 0 : COOP_AIM_PITCH) | (f & 0x40 ? 0 : COOP_AIM_YAW);
    static bool was;
    if (!was && coop_look_log()) LOG("[coop] aim: sending pitch %d yaw %d flags %d\n", s->aim_pitch, s->aim_yaw, s->aim_flags);
    was = true;
}

// Rotation by ang radians about the unit axis u through point p, as a 3x4 matrix.
static void rot_about(const float u[3], float ang, const float p[3], float o[12]) {
    float cs = cosf(ang), sn = sinf(ang), t = 1 - cs, x = u[0], y = u[1], z = u[2];
    float r[9] = {t * x * x + cs, t * x * y - sn * z, t * x * z + sn * y,
                  t * x * y + sn * z, t * y * y + cs, t * y * z - sn * x,
                  t * x * z - sn * y, t * y * z + sn * x, t * z * z + cs};
    for (int i = 0; i < 3; i++) {
        o[i * 4] = r[i * 3], o[i * 4 + 1] = r[i * 3 + 1], o[i * 4 + 2] = r[i * 3 + 2];
        o[i * 4 + 3] = p[i] - (r[i * 3] * p[0] + r[i * 3 + 1] * p[1] + r[i * 3 + 2] * p[2]);
    }
}

// Receiver: our own bone modifier on the avatar's Spine_1 (the game's generic node class: what its look-at spine nodes
// are). The game applies an actor's modifiers right after its animation (FUN_7101096ce8 over the list at actor+0x4d8,
// each node: bone world matrix M -> M * D, D = the node's bone-local 3x4 at +0x68, scale at +0x98). Every frame we
// write D = the aim rotation about the bone; registered only while the other player aims. (The player's own bone
// control on the avatar, so the game's look-at call could drive it, crashed both games at spawn: not used.)
// Node: ctor FUN_71010966cc (size >= 0xa8), bone name FUN_7101096be4(node, SafeString *) (keeps the pointer: the name
// lives in our buffer), register FUN_7101533770(actor, node, 0) (+8 = 1 when bound), unregister
// FUN_7101096e48(node, actor + 0x4d8). Linking / unlinking frees nothing (the node is ours).
#define A_BMOD_CTOR 0x70010966ccull
#define A_BMOD_NAME 0x7001096be4ull
#define A_BMOD_ADD 0x7001533770ull
#define A_BMOD_DEL 0x7001096e48ull
#define AIM_UP_MAX 50.0f    // (Player_Link.bbonectrl battle limits: up 50, down 70, sideways 45)
#define AIM_DOWN_MAX 70.0f
#define AIM_SIDE_MAX 45.0f
static struct {
    uint64_t node, actor;  // our modifier (guest memory, fresh per avatar actor) and the actor it is on (0 = none)
    bool on;               // linked into actor's list
    float pitch, yaw;      // smoothed degrees
    float az;              // smoothed degrees: on horseback (no yaw) the pitch axis turned toward the aim
    float d[12];           // the D written last (have_d: applied since: the bone as read carries it)
    bool have_d;
    uint64_t last_ns;
} aim;

static void aim_pups_unlink(Cpu *c);
static void aim_unlink(Cpu *c) {
    if (aim.on && aim.actor == avatar.actor && avatar_alive()) {
        Cpu saved = *c;
        c->x[0] = aim.node; c->x[1] = aim.actor + 0x4d8;
        guest_call(c, A_BMOD_DEL);
        *c = saved;
        LOG("[coop] aim: the avatar stops aiming\n");
    }
    aim.on = false, aim.have_d = false;
    aim_pups_unlink(c);
}
// (bugs B7, 2026-11-19) hle_alloc never frees (until a runtime reset), and every new avatar actor / armor puppet got
// fresh 0x140-byte nodes: aim (1 + one per armor slot) and ragdoll (COOP_POSE_BONES x (1 + armor slots)). A node that
// is no longer used by its actor is now put in a pool and made again (zeroed, constructor, name) for a later node
// instead of allocating. Not linked into any list of a live actor: the pool only takes a node after its actor was
// replaced / gone (its modifier list went with it), and hands it out again only after BMOD_QUARANTINE_NS, so an actor
// still being deleted cannot see the node re-made under it. No native unlink was needed. The log line counts
// allocations and reuses. SAVAGEWIND_COOP_BMODREUSE=0: no pool (every node allocated, as before).
#define BMOD_POOL 512
#define BMOD_QUARANTINE_NS 30000000000ull  // (SAVAGEWIND_COOP_BMODREUSE_MS=<ms>: dev override for probes)
static uint64_t bmod_quarantine_ns(void) {
    static int ms = -1;
    if (ms < 0) ms = getenv("SAVAGEWIND_COOP_BMODREUSE_MS") ? atoi(getenv("SAVAGEWIND_COOP_BMODREUSE_MS")) : (int)(BMOD_QUARANTINE_NS / 1000000);
    return ms < 0 ? 0 : (uint64_t)ms * 1000000ull;
}
static struct { uint64_t node, at; } bmod_pool[BMOD_POOL];
static int bmod_pool_n;
static unsigned bmod_allocs, bmod_reused, bmod_released;
static int bmodreuse_c = -1;
// (2026-11-20 review) OFF by default: a puppet that briefly counts as not ready (frames < 6 / pup_alive false) with
// the same actor would pool a node still linked in that live actor's modifier list, and reuse after 30 s would link it
// into a second list. The leak is small (0x140 bytes per node); SAVAGEWIND_COOP_BMODREUSE=1 turns the pool on.
static bool bmodreuse_on(void) {
    if (bmodreuse_c < 0) bmodreuse_c = getenv("SAVAGEWIND_COOP_BMODREUSE") && !strcmp(getenv("SAVAGEWIND_COOP_BMODREUSE"), "1");
    return bmodreuse_c;
}
static void bmod_release(uint64_t *node) {  // (the caller's pointer is cleared)
    if (!*node) return;
    if (bmodreuse_on() && bmod_pool_n < BMOD_POOL) {
        bmod_pool[bmod_pool_n].node = *node, bmod_pool[bmod_pool_n].at = plat_now_ns(), bmod_pool_n++;
        bmod_released++;
    }
    *node = 0;
}
RT_ON_RESET(coop_bmod_reset) {  // (hle_heap_reset: every pooled pointer is dead)
    memset(bmod_pool, 0, sizeof bmod_pool);
    bmod_pool_n = 0;
}
// A fresh modifier node on a bone (guest memory), or 0. (ragdoll.inc uses it for every ragdoll bone)
static uint64_t bmod_new_named(Cpu *c, const char *bone) {
    size_t len = strlen(bone);
    if (len >= 0x30) return 0;
    uint64_t m = 0, now = plat_now_ns();
    for (int i = 0; i < bmod_pool_n; i++)
        if (now - bmod_pool[i].at >= bmod_quarantine_ns()) {
            m = bmod_pool[i].node;
            bmod_pool[i] = bmod_pool[--bmod_pool_n];
            bmod_reused++;
            break;
        }
    if (!m) m = (uint64_t)(uintptr_t)hle_alloc(0x140), bmod_allocs += m != 0;
    if (!m) return 0;
    if ((bmod_allocs + bmod_reused) % 16 == 1 || bmod_reused % 16 == 1)
        LOG("[coop] bmod: %u allocated (0x140 bytes each), %u reused, %u released, %d pooled\n", bmod_allocs, bmod_reused,
            bmod_released, bmod_pool_n);
    memset((void *)(uintptr_t)m, 0, 0x140);
    Cpu saved = *c;
    c->x[0] = m;
    guest_call(c, A_BMOD_CTOR);
    *c = saved;
    memcpy((void *)(uintptr_t)(m + 0x110), bone, len + 1);  // the name and its SafeString (kept by pointer)
    ST64(m + 0x100, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
    ST64(m + 0x108, m + 0x110);
    c->x[0] = m; c->x[1] = m + 0x100;
    guest_call(c, A_BMOD_NAME);
    *c = saved;
    return m;
}
static uint64_t bmod_new(Cpu *c) { return bmod_new_named(c, "Spine_1"); }

// (2026-11-14: the head piece stayed detached) The head / hair, armor and gear puppets are separate actors
// that copy the avatar's animation (astree phase lock) but not our spine turn: the body bent, the head piece stayed
// upright. Each armor puppet gets its own node with the same bone-local D (same skeleton, same pose).
static struct { uint64_t node, actor; bool on, failed; } aim_pup[COOP_ARMOR_SLOTS];
static void aim_pups_unlink(Cpu *c) {
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
        if (aim_pup[i].on && aim_pup[i].actor == armor_pup[i].actor && pup_alive(&armor_pup[i])) {
            Cpu saved = *c;
            c->x[0] = aim_pup[i].node; c->x[1] = aim_pup[i].actor + 0x4d8;
            guest_call(c, A_BMOD_DEL);
            *c = saved;
        }
        aim_pup[i].on = false;
    }
}
static void aim_pups_apply(Cpu *c, const float dn[12]) {
    static unsigned said;
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
        uint64_t a = armor_pup[i].actor && armor_pup[i].frames >= 6 && pup_alive(&armor_pup[i]) ? armor_pup[i].actor : 0;
        if (aim_pup[i].actor != a) bmod_release(&aim_pup[i].node), aim_pup[i].actor = a, aim_pup[i].on = false, aim_pup[i].failed = false;
        if (!a || aim_pup[i].failed) continue;
        if (!aim_pup[i].on) {
            if (!aim_pup[i].node && !(aim_pup[i].node = bmod_new(c))) continue;
            Cpu saved = *c;
            c->x[0] = a; c->x[1] = aim_pup[i].node; c->x[2] = 0;
            guest_call(c, A_BMOD_ADD);
            *c = saved;
            aim_pup[i].on = LD8(aim_pup[i].node + 8) != 0;
            if (!aim_pup[i].on) aim_pup[i].failed = true, bmod_release(&aim_pup[i].node);  // (no Spine_1 in its model: leave it)
            if (said++ < 40 || coop_look_log())
                LOG("[coop] aim: puppet %d (%llx) spine modifier %s\n", i, (unsigned long long)a, aim_pup[i].on ? "linked" : "NOT linked");
            if (!aim_pup[i].on) continue;
        }
        memcpy((void *)(uintptr_t)(aim_pup[i].node + 0x68), dn, 48);
    }
}

static bool aim_link(Cpu *c) {
    if (aim.actor != avatar.actor) bmod_release(&aim.node), aim.actor = avatar.actor, aim.on = false;  // (a new avatar: new node)
    if (aim.on) return true;
    if (!aim.node && !(aim.node = bmod_new(c))) return false;
    Cpu saved = *c;
    c->x[0] = avatar.actor; c->x[1] = aim.node; c->x[2] = 0;
    guest_call(c, A_BMOD_ADD);
    *c = saved;
    aim.on = LD8(aim.node + 8) != 0;
    aim.have_d = false;  // (the node's D starts as the identity)
    const float *sc = (const float *)(uintptr_t)(aim.node + 0x98);
    LOG("[coop] aim: the avatar aims: modifier on Spine_1 %s (bone %d/%d, scale %.2f %.2f %.2f)\n",
        aim.on ? "linked" : "NOT linked", (int)(int16_t)LD16(aim.node + 0x60), (int)(int16_t)LD16(aim.node + 0x62),
        sc[0], sc[1], sc[2]);
    if (!aim.on) bmod_release(&aim.node);  // (try a fresh one next time)
    return aim.on;
}

static void aim_frame(Cpu *c) {
    bool live = avatar.actor && avatar_alive() && avatar.frames >= 6;
    if (bones_dev) {
        int w = bones_dev;
        bones_dev = 0;
        uint64_t pi = player_info_ptr(), pa = pi ? LD64(pi + 0x60) : 0;
        if (w == 1 && in_guest(pa)) bones_log(c, pa, "player");
        if (w == 2 && live) bones_log(c, avatar.actor, "avatar");
    }
    int pitch = 0, yaw = 0, fl = 0;
    CoopState rs;
    double age;
    if (live && aim_on()) {
        if (aim_dev_pitch <= 360) pitch = (int)aim_dev_pitch, fl = COOP_AIM_ON | COOP_AIM_PITCH | COOP_AIM_YAW;
        else if (coop_net_get_remote(&rs, &age) && age < 0.5) pitch = rs.aim_pitch, yaw = rs.aim_yaw, fl = rs.aim_flags;
    }
    if (!live) {
        // A new actor can reuse the address; its modifier list belongs to a different lifetime.
        if (local_in_world()) aim_pups_unlink(c);
        bmod_release(&aim.node);
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++) bmod_release(&aim_pup[i].node);
        memset(&aim, 0, sizeof aim);
        memset(aim_pup, 0, sizeof aim_pup);
        return;
    }
    uint64_t now = plat_now_ns();
    double dt = aim.last_ns && now > aim.last_ns ? (now - aim.last_ns) / 1e9 : 1 / 60.0;
    aim.last_ns = now;
    float k = (float)(dt * 12 > 1 ? 1 : dt * 12);
    float tp = (fl & COOP_AIM_ON) && (fl & COOP_AIM_PITCH) ? (float)pitch : 0, ty = (fl & COOP_AIM_ON) && (fl & COOP_AIM_YAW) ? (float)yaw : 0;
    tp = tp > AIM_UP_MAX ? AIM_UP_MAX : tp < -AIM_DOWN_MAX ? -AIM_DOWN_MAX : tp;
    ty = ty > AIM_SIDE_MAX ? AIM_SIDE_MAX : ty < -AIM_SIDE_MAX ? -AIM_SIDE_MAX : ty;
    if (!aim.on) aim.pitch = 0, aim.yaw = 0;
    aim.pitch += (tp - aim.pitch) * k, aim.yaw += (ty - aim.yaw) * k;
    float taz = (fl & COOP_AIM_ON) && !(fl & COOP_AIM_YAW) ? 2.0f * (float)yaw : 0, daz = taz - aim.az;
    daz -= 360.0f * floorf((daz + 180.0f) / 360.0f);  // (the short way round)
    if (!aim.on) aim.az = taz;
    else aim.az += daz * k;
    aim.az -= 360.0f * floorf((aim.az + 180.0f) / 360.0f);
    if (!(fl & COOP_AIM_ON)) {  // (eased back to straight: off)
        if (aim.on && fabsf(aim.pitch) < 0.5f && fabsf(aim.yaw) < 0.5f) aim_unlink(c);
        if (!aim.on) return;
    } else if (!aim_link(c)) return;
    // D = M^-1 * R * M, R = turn about the bone's origin: pitch about the body's right, then yaw about up
    // (the bone as read already carries last frame's D: M = read * D_prev^-1, else the turn feeds back on itself;
    // seen 2026-11-14: at 45 degrees the matrix ran off to garbage and the body mesh stretched to the sky)
    float sp[12], inv[12], ch[3], fw[3], rt[3], r1[12], r2[12], r[12], t1[12], d[12], dinv[12], rd[12];
    if (!bone_matrix(c, avatar.actor, "Spine_1", rd) || !aim_frame_of(c, avatar.actor, ch, fw, rt)) {
        aim_unlink(c);  // (no good bone: off rather than a stale turn)
        return;
    }
    if (aim.have_d && mtx_inv(aim.d, dinv)) mtx_mul(rd, dinv, sp);
    else memcpy(sp, rd, sizeof sp);
    if (!mtx_inv(sp, inv)) return;
    float o[3] = {sp[3], sp[7], sp[11]}, up[3] = {0, 1, 0};
    if (aim.az != 0) {  // (horseback: tip about the aim's own side axis; aiming behind, that is the body's left)
        float ca = cosf(aim.az / 57.29578f), sa = sinf(aim.az / 57.29578f);
        float af[3] = {ca * fw[0] + sa * rt[0], 0, ca * fw[2] + sa * rt[2]};
        rt[0] = af[2], rt[1] = 0, rt[2] = -af[0];
    }
    rot_about(rt, -aim.pitch / 57.29578f, o, r1);  // (about up x fwd a positive angle tips forward down)
    rot_about(up, aim.yaw / 57.29578f, o, r2);
    mtx_mul(r2, r1, r);
    mtx_mul(inv, r, t1);
    mtx_mul(t1, sp, d);
    float r9[9] = {d[0], d[1], d[2], d[4], d[5], d[6], d[8], d[9], d[10]};
    horse_orthonormal(r9);  // (a pure turn: no scale creeping in)
    float dn[12] = {r9[0], r9[1], r9[2], 0, r9[3], r9[4], r9[5], 0, r9[6], r9[7], r9[8], 0};  // (about the bone's origin)
    for (int i = 0; i < 12; i++)
        if (!isfinite(dn[i])) { aim_unlink(c); return; }
    memcpy((void *)(uintptr_t)(aim.node + 0x68), dn, sizeof dn);
    memcpy(aim.d, dn, sizeof dn);
    aim_pups_apply(c, dn);
    aim.have_d = true;
    static unsigned said;
    if ((fl & COOP_AIM_ON) && said++ % 120 == 0)
        LOG("[coop] aim: avatar spine pitch %.0f yaw %.0f (sent %d %d flags %d)\n", aim.pitch, aim.yaw, pitch, yaw, fl);
}
