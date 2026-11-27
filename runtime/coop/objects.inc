// Part of runtime/coop.c (included there, same translation unit): physics wrapper and shared physics objects.
// ---- the game's physics wrapper (ksys RigidBody, 2026-11-08 RE): a havok body (vtable RIGID_BODY_VT) has its
// wrapper in userData (+0x18), and the wrapper points back at +0x78. Wrapper calls (all through its motion accessor
// at +0xb8): setTransform 0x70012d3d24(rb, Matrix34f *, bool propagate), setPosition 0x70012d3d88(rb, Vec3 *, bool),
// setLinearVelocity 0x70012d3210(s0 epsilon, rb, Vec3 *), getPosition 0x70012d602c(rb, Vec3 *out).
#define A_RB_SET_TRANSFORM 0x70012d3d24ull
#define A_RB_SET_POSITION 0x70012d3d88ull
#define A_RB_SET_LINVEL 0x70012d3210ull
#define A_RB_SET_FIXED 0x70012d316cull  // (rb, bool fixed, bool keep): flags +0x68 0x40000 asked / 0x80000 is fixed
struct RbFind { uint64_t actor, rb; };
static bool objects_fast_only;  // (objects_frame: the slow memory walk is for the dev commands only)
static void rb_walk(uint64_t obj, int depth, struct RbFind *f) {
    const float *m = (const float *)(uintptr_t)(f->actor + ACTOR_MTX);
    for (uint32_t off = 0; off < 0x1000 && !f->rb; off += 8) {
        uint64_t p = LD64(obj + off);
        if (!in_guest(p) || (p & 7)) continue;
        if (LD64(p) == RIGID_BODY_VT) {
            const float *bp = (const float *)(uintptr_t)(p + 0x1a0);
            float dx = bp[0] - m[3], dy = bp[1] - m[7], dz = bp[2] - m[11];
            uint64_t rb = LD64(p + 0x18);
            // (flags +0x68 bit 0: a sensor body - an iron box has one at the same spot; the game's impulse and
            // transform calls ignore it)
            if (dx * dx + dy * dy + dz * dz < 0.25f && in_guest(rb) && LD64(rb + 0x78) == p) {
                if (!(LD32(rb + 0x68) & 1)) f->rb = rb;
            }
            continue;
        }
        if (depth < 2 && in_guest(p + 0x1000)) rb_walk(p, depth + 1, f);
    }
}
// The wrapper of the actor's main body (the havok body at the actor's own position), or 0.
static uint64_t actor_rb(uint64_t actor) {
    if (!in_guest(actor) || !in_guest(actor + 0x1000)) return 0;
    // Fast path (rbscan on every Dungeon038 object, 2026-11-08): the physics instance *(actor+0x190) holds the main
    // havok body at +0x78, whose userData is the wrapper.
    uint64_t pi = LD64(actor + 0x190), body = in_guest(pi) ? LD64(pi + 0x78) : 0;
    if (in_guest(body) && LD64(body) == RIGID_BODY_VT) {
        uint64_t rb = LD64(body + 0x18);
        if (in_guest(rb) && LD64(rb + 0x78) == body && !(LD32(rb + 0x68) & 1)) return rb;
    }
    if (objects_fast_only) return 0;
    struct RbFind f = {actor, 0};
    rb_walk(actor, 0, &f);
    return f.rb;
}
static uint64_t rb_mem;  // guest scratch: a Matrix34f and a Vec3 (reset on reboot)
static void rb_set_transform(Cpu *c, uint64_t rb, const float m[12]) {
    if (!rb_mem) rb_mem = (uint64_t)(uintptr_t)hle_alloc(0x80);
    if (!rb_mem) return;
    memcpy((void *)(uintptr_t)rb_mem, m, 48);
    Cpu saved = *c;
    c->x[0] = rb; c->x[1] = rb_mem; c->x[2] = 1;
    guest_call(c, A_RB_SET_TRANSFORM);
    *c = saved;
}
static void rb_set_linvel(Cpu *c, uint64_t rb, const float v[3]) {
    if (!rb_mem) rb_mem = (uint64_t)(uintptr_t)hle_alloc(0x80);
    if (!rb_mem) return;
    memcpy((void *)(uintptr_t)(rb_mem + 0x40), v, 12);
    Cpu saved = *c;
    c->x[0] = rb; c->x[1] = rb_mem + 0x40;
    memset(&c->v[0], 0, 16);  // (s0: epsilon 0)
    guest_call(c, A_RB_SET_LINVEL);
    *c = saved;
}
// ---- shared physics objects (2026-11-08, protocol 14; puzzles should be shared). Inside a shrine both
// players are in, every placed object with a HashId and a main rigid body (boxes, blocks, plates, balls, doors...) is
// shared: whichever game moves it takes it ("owner") and sends its pose and velocity 20 times a second; the other
// game holds its copy there with the game's own RigidBody setTransform + setLinearVelocity (a fixed resting body
// ignores raw memory writes; those calls move it, verified on a Dungeon038 iron box). If our player moves a copy the
// other side owns (it ends a frame > OBJ_CLAIM_M from where we put it), we take it over with a newer claim. Owners
// keep their objects (no time-out: a door that one side opened must not be closed again by the other side's logic).
// SAVAGEWIND_COOP_OBJECTS=0 turns this off.
#define OBJ_MAX 160
#define OBJ_MOVE_M 0.20f    // our copy moved this far from where it rested: ours now (when nobody owns it)
#define OBJ_CLAIM_M 0.50f   // a followed copy ended a frame this far from where we put it: our player moved it
#define OBJ_CALM_NS 1500000000ull  // after an object changed hands: no claim of ours for this long
#define OBJ_VEL_MAX 25.0f   // m/s: a faster sample is a jump (a teleport, a reset), sent as 0
#define OBJ_SETTLE_NS 3000000000ull  // after an object is found: no claims (spawn settling)
#define OBJ_STALE_S 1.5     // the owner's pose this old: stop holding the copy
static struct Obj {
    uint32_t hash;
    uint64_t actor, unit, rb, body;
    uint32_t unit_id;
    char name[40];
    int owner;              // 0 nobody, 1 us, 2 the other player
    int8_t opid;            // (59) owner 2: which player owns it (more than two players: the copy can change hands
                            // between two others)
    uint16_t gen;           // the claim's generation (ours or theirs)
    float rest[3];          // where it rested when nobody owned it
    float last[3], vel[3];  // last sample, and the velocity from the last two
    uint64_t last_ns, found_ns, moved_ns, changed_ns;
    float put[3];           // where we put it last frame (following)
    bool have_put;
    bool matched;           // following: resting where they have it, and let go of
    // carried objects (protocol 30)
    bool carried;           // our player holds it overhead
    int release_wait;       // frames until we send its release (the throw's velocity needs two samples)
    uint64_t released_ns;   // when it was let go (ours sent / theirs received): a break soon after is shared
    bool parked;            // the other player holds it: our copy is parked out of sight, fixed
    float park_at[3], park_from[3];
    uint64_t park_wait_ns;  // they let go of it, its release event not here yet: stay parked until then
    uint64_t free_until_ns, free_rest_ns;  // released here: our copy flies by itself until it rests (or then)
    // breakables (2026-11-12)
    uint64_t driven_ns;     // when we last put it where the other player has it (following)
    uint64_t kill_ns;       // broken because the other player's broke: removed by force if still here then
    // Stasis (2026-11-14, protocol 43; runes.inc)
    bool stasis;            // our Stasis holds it (we own it and send COOP_OBJ_STASIS)
    bool frozen_there;      // the other player's Stasis holds it: our copy is fixed at their pose
    bool fake_fixed;        // (`coop stasisfake`: we fixed the body ourselves)
    // hinged objects (2026-11-16, HINGE)
    bool hinged;            // a seesaw / hinged door / fence / gear: obj_is_hinged
    uint8_t claim_frames;   // consecutive looks a claim condition held (hinged objects need OBJ_HINGE_FRAMES)
} objs[OBJ_MAX];
static int obj_n;
static uint32_t obj_area;
static bool objects_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_OBJECTS") && !strcmp(getenv("SAVAGEWIND_COOP_OBJECTS"), "0"));
    return on;
}
// (2026-11-16, worker world2; play log play-night1116.log, Cryonis shrine Dungeon065: DgnObj_Seesaw_B and
// DgnObj_FenceDynamic_A changed hands ~10 times, "taken over 0.2 m off".) Objects on a hinge (map links HingeCs /
// LimitHingeCs to a ConstraintDummy or a shaft: seesaws, hinged boards and doors, gears, capstans, water wheels; the
// dynamic fence) barely move their centre when they swing, and the hinge solver kicks a copy we set in place, while
// the avatar's collision body tilts our copy's far end (more than 1.8 m from the centre). So for them: a claim needs
// larger moves (OBJ_HINGE_MOVE_M / OBJ_HINGE_CLAIM_M) that last OBJ_HINGE_FRAMES looks in a row, a longer calm period
// after a change of hands, and the avatar anywhere within OBJ_HINGE_AVATAR_M (with our Link farther) blocks claims.
// SAVAGEWIND_COOP_HINGE=0: off (the normal rules).
#define OBJ_HINGE_MOVE_M 0.35f
#define OBJ_HINGE_CLAIM_M 0.80f
#define OBJ_HINGE_FRAMES 8
#define OBJ_HINGE_CALM_NS 4000000000ull
#define OBJ_HINGE_AVATAR_M 5.0f
static volatile int hinge_dev = 1;  // (dev: `coop hinge 0|1`)
static bool hinge_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_HINGE") && !strcmp(getenv("SAVAGEWIND_COOP_HINGE"), "0"));
    return on;
}
static bool obj_is_hinged(const char *n) {
    static const char *const parts[] = {"Seesaw", "HingeBoard", "FenceDynamic", "DoorStone", "DoorIron", "IronDoor",
                                        "SwitchGear", "SpurGear", "Capstan", "WaterWheel", "BoardStone", "StageWood_A_06"};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++)
        if (strstr(n, parts[i])) return true;
    return false;
}
static bool obj_is_ball(const char *n) {
    return !strncmp(n, "AncientBall", 11) || !strncmp(n, "StoneBall", 9) || !strncmp(n, "IronBall", 8) ||
           !strncmp(n, "ElectricBall", 12) || !strncmp(n, "DgnObj_AncientBall_", 19) || !strncmp(n, "DgnObj_StoneBall", 16) ||
           !strncmp(n, "DgnObj_IronBall", 15) || !strncmp(n, "DgnObj_SpikeBall", 16);
}
static bool waterpuz_on(void);  // (water.inc: SwitchWaterWheel / DgnObj_SwitchCapstan handled with angular velocity)
static bool obj_name(const char *n) {
    if (!strncmp(n, "DgnObj_SwitchCapstan_", 21) && waterpuz_on()) return false;
    // (balls carry their own names: AncientBall* - the guardian-orb balls of ball sockets -, StoneBall, IronBall,
    // ElectricBall*; the sockets AncientBallSwitch* only check what lies in them and stay out)
    if (!strncmp(n, "AncientBallSwitch", 17)) return false;
    // (2026-11-15, play report: a metal chest moved with Magnesis did not move for the other player. Chests are
    // TBox_* actors; shared like any physics object, the owner moves it. SAVAGEWIND_COOP_CHESTOBJ=0: off.)
    if (!strncmp(n, "TBox_", 5)) {
        static int on = -1;
        if (on < 0) on = look_on("SAVAGEWIND_COOP_CHESTOBJ");
        return on;
    }
    if (!strncmp(n, "AncientBall", 11) || !strncmp(n, "StoneBall", 9) || !strncmp(n, "IronBall", 8) ||
        !strncmp(n, "ElectricBall", 12) || !strncmp(n, "Barrel", 6) || !strcmp(n, "Pot") ||
        !strncmp(n, "RockRoll", 8) || !strncmp(n, "FldObj_PushRock", 15) || !strncmp(n, "FldObj_BoardIron", 16) ||
        !strncmp(n, "FldObj_KorokStone", 17) || !strncmp(n, "BoxWater", 8))
        return true;
    // (2026-11-19, play report: wooden crates broken by one player stayed whole for the other. Crates are Kibako*
    // (Kibako_Contain_01 ~270 on the map, KibakoZora/Desert/SnowMountain/Volcano/Seaside/Jungle_Contain_01,
    // KibakoDungeon); they were not shared at all. SAVAGEWIND_COOP_CRATES=0: off.)
    if (!strncmp(n, "Kibako", 6)) {
        static int on = -1;
        if (on < 0) on = look_on("SAVAGEWIND_COOP_CRATES");
        return on;
    }
    if (strncmp(n, "Obj_", 4) && strncmp(n, "DgnObj_", 7)) return false;
    // (architecture, lights, water, the entrance/goal machinery, trees/plants/static rocks: they never move or must not be shared)
    static const char *const skip[] = {"DgnObj_Hrl_", "DgnObj_hrl_", "DgnObj_DungeonWater", "DgnObj_Pillar",
                                       "DgnObj_Entrance", "DgnObj_Goal", "DgnObj_Warp", "DgnObj_DownloadTerminal",
                                       "DgnMrgPrt", "DgnObj_SwitchStep", "Obj_Tree", "Obj_Plant", "Obj_Flower",
                                       "Obj_Mineral", "Obj_Rock",
                                       // (2026-11-16, play log Dungeon041: the shrine's own machinery moves its
                                       // shutters in each game; shared, they changed hands 2700 times - the two
                                       // games fought over them and they jittered. Their switches are shared.)
                                       "DgnObj_BoxShutter"};  // (floor switches: switches_frame)
    for (size_t i = 0; i < sizeof skip / sizeof *skip; i++)
        if (!strncmp(n, skip[i], strlen(skip[i]))) return false;
    return true;
}
static bool obj_alive(const struct Obj *o) {
    return in_guest(o->actor) && in_guest(o->unit) && LD32(o->unit + UNIT_ID) == o->unit_id &&
           LD64(o->unit + UNIT_ACTOR) == o->actor && LD8(o->actor + PROC_STATE) != 3 && LD64(o->rb + 0x78) == o->body;
}
struct ObjFind { uint64_t now; int budget; bool overworld; float me[3]; };
static bool brk_only_name(const char *n);  // (breakables below: things that never move and only break)
static void brk_found(Cpu *c, uint64_t a, const char *name, struct ObjFind *f);
static bool brk_on(void);
static bool gyro_partner_tilting(void);  // (gyro.inc: the other player tilts a motion-control platform here)
static bool gyro_maze_ball(uint32_t hash, bool *driving);
static uint64_t stasis_target(Cpu *c);   // (runes.inc: the actor our Stasis holds, or 0)
static bool stasis_fake_holds(uint32_t hash, uint64_t now);  // (runes.inc: `coop stasisfake`)
static bool stasis_on(void);
static void stasis_fx(Cpu *c, uint64_t actor, bool start);
static bool obj_or_brk_name(const char *n) { return obj_name(n) || (brk_on() && brk_only_name(n)); }
static void obj_found(Cpu *c, uint64_t a, const char *name, void *ctx) {
    struct ObjFind *f = ctx;
    if (brk_only_name(name)) { brk_found(c, a, name, f); return; }
    for (int i = 0; i < obj_n; i++)
        if (objs[i].actor == a) return;
    if (f->budget <= 0 || obj_n == OBJ_MAX) return;
    float p[3];
    if (f->overworld && actor_dist(a, f->me, p) > 80.0f) return;
    f->budget--;
    uint32_t h = actor_hash(c, a);
    objects_fast_only = true;
    uint64_t rb = h ? actor_rb(a) : 0;
    objects_fast_only = false;
    if (!rb) return;  // (nothing to share; asked again on the next look - cheap enough with the budget)
    struct Obj *o = &objs[obj_n++];
    memset(o, 0, sizeof *o);
    o->hash = h, o->actor = a, o->unit = LD64(a + PROC_UNIT), o->unit_id = LD32(a + PROC_ID), o->rb = rb;
    o->body = LD64(rb + 0x78);
    snprintf(o->name, sizeof o->name, "%s", name);
    o->hinged = hinge_on() && obj_is_hinged(name);
    memcpy(o->rest, (void *)(uintptr_t)(o->body + 0x1a0), 12);
    memcpy(o->last, o->rest, 12);
    o->found_ns = o->last_ns = f->now;
}
static void quat_mtx(const float q[4], const float p[3], float m[12]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p[0],
                   2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p[1],
                   2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p[2]};
    memcpy(m, r, sizeof r);
}
static float dist3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
// (59, coop-n3) More than two players: objects are shared with every world player near us (in the world, our area,
// and in the overworld within 120 m), not only with the partner. Their ids and positions; with two players (or
// SAVAGEWIND_COOP_MULTIWORLD=0: coop_net_world_players is the partner alone) just the partner, as before.
struct ObjNear { int n; int pid[COOP_MAX_PLAYERS]; float pos[COOP_MAX_PLAYERS][3]; };
static void obj_near_players(struct ObjNear *nr, uint32_t here, bool shrine, bool overworld, const float me[3]) {
    nr->n = 0;
    uint32_t w = coop_net_world_players(3.0);
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
        if (!(w >> p & 1)) continue;
        CoopState rs;
        double age = 99;
        coop_net_view(p);
        bool ok = coop_net_get_remote(&rs, &age);
        coop_net_view(-1);
        if (!ok || age >= 3.0 || !(rs.flags & COOP_FLAG_IN_WORLD) || rs.area != here) continue;
        if (!shrine && !(overworld && dist3(me, rs.pos) < 120.0f)) continue;
        nr->pid[nr->n] = p;
        memcpy(nr->pos[nr->n], rs.pos, 12);
        nr->n++;
    }
}
// Is p within r of us or of one of those players?
static bool obj_near_any(const struct ObjNear *nr, const float me[3], const float p[3], float r) {
    if (dist3(p, me) < r) return true;
    for (int i = 0; i < nr->n; i++)
        if (dist3(p, nr->pos[i]) < r) return true;
    return false;
}
// Where player pid is (the first near player when not near: the parking spot only needs to be out of sight).
static const float *obj_player_pos(const struct ObjNear *nr, int pid) {
    for (int i = 0; i < nr->n; i++)
        if (nr->pid[i] == pid) return nr->pos[i];
    return nr->pos[0];
}
// ---- carried objects (2026-11-11, protocol 30; a dropped barrel should drop for the other
// player). Probe (Dungeon034 barrel): while Link holds a barrel (*(player actor + 0x88)) its
// body is moved with his hands (body = actor matrix, overhead, flags +0x68 0x10800098); A or B puts it down (the hold
// ends when it touches the floor), R throws it (the hold ends at the release, ~(0, 6, 9) m/s two frames later). Before
// this, the other game followed the carried barrel as a plain owned object: its copy hung at the avatar's hands next to
// the carried prop puppet, rested there "matched", fell, was "taken over" by that game, and the carrier's real barrel
// was then pulled down onto his head (he was hurt, the barrel broke in one game only).
// Now: the carrier claims the object at once (a newer claim, whatever the settle/calm timers) and flags it CARRIED in
// every packet; the other game parks its copy out of sight (30 m under the carrier, fixed) and shows only the prop
// puppet. Three frames after the hold ends the carrier sends EV_OBJ (release: pose + velocity); the other game puts its
// copy there with that velocity and lets its own physics fly it (rolls, lands, breaks) until it rests, then the normal
// owner/follower rules of objects_frame take over again (the owner is the carrier). A copy that breaks within 8 s of
// its release is removed in the other game too (EV_OBJ gone). SAVAGEWIND_COOP_CARRY=0: the old behaviour.
#define EV_OBJ 8
#define EV_OBJ_RELEASE 1
#define EV_OBJ_GONE 2
typedef struct {
    uint32_t hash;
    uint16_t gen;
    uint8_t kind, pad;
    float pos[3], rot[4], vel[3];
} EvObj;
_Static_assert(sizeof(EvObj) == 48, "EvObj layout");
#define OBJ_PARK_DROP 30.0f              // m under the carrier
#define OBJ_FREE_NS 6000000000ull        // a released copy flies by itself at most this long
#define OBJ_GONE_NS 8000000000ull        // a break this soon after the release is shared
static bool carry_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_CARRY") && !strcmp(getenv("SAVAGEWIND_COOP_CARRY"), "0"));
    return on;
}
static uint64_t local_held(void) {  // what our Link holds overhead, or 0
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t h = in_guest(pa) ? LD64(pa + 0x88) : 0;
    return in_guest(h) && LD8(h + PROC_STATE) != 3 ? h : 0;
}
static void rb_fix(Cpu *c, uint64_t rb, bool fixed) {
    Cpu saved = *c;
    c->x[0] = rb; c->x[1] = fixed; c->x[2] = 0;
    guest_call(c, A_RB_SET_FIXED);
    *c = saved;
}
// ---- RAFTGUST (2026-11-21: a raft fanned with the Korok Leaf was not well synced). The Leaf's wind is no
// actor: it only moves things in the swinger's own game. A raft with both players on it stays with its owner
// (RAFTRIDER in objects_frame), so a gust of the other player's (seen as its avatar swinging the Leaf: pvp_wind) is
// replayed here on the rafts we own that lie in the gust: their body gets RAFT_GUST_V m/s more along the gust.
// SAVAGEWIND_COOP_RAFTGUST=0: off; SAVAGEWIND_COOP_RAFTGUST_V=<m/s> (dev).
#define RAFT_GUST_V 3.0f
static int raft_gust(Cpu *c, const float from[3], const float f[2]) {
    static int on = -1;
    if (!env_on("SAVAGEWIND_COOP_RAFTGUST", &on)) return 0;
    static float v_add = -1;
    if (v_add < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_RAFTGUST_V");
        v_add = e && atof(e) > 0 ? (float)atof(e) : RAFT_GUST_V;
    }
    int n = 0;
    for (int i = 0; i < obj_n; i++) {
        struct Obj *o = &objs[i];
        if (o->owner != 1 || !o->rb || !o->body || !strstr(o->name, "Raft") || strstr(o->name, "Sail")) continue;
        const float *p = (const float *)(uintptr_t)(o->body + 0x1a0);
        float dx = p[0] - from[0], dz = p[2] - from[2], dy = p[1] - from[1];
        float along = dx * f[0] + dz * f[1], side = fabsf(-dx * f[1] + dz * f[0]);
        bool in = along > -1.5f && along < 10.0f && side < 3.0f + along * 0.2f && fabsf(dy) < 4.0f;
        LOG("[coop] raft: the other player's Korok Leaf gust; our %s %u is %.1f m in front, %.1f m to the side: %s "
            "(RAFTGUST)\n", o->name, o->hash, along, side, in ? "pushed" : "not reached");
        if (!in) continue;
        if (LD32(o->rb + 0x68) & 0x80000) rb_fix(c, o->rb, false);
        float v[3] = {o->vel[0] + f[0] * v_add, o->vel[1], o->vel[2] + f[1] * v_add};
        rb_set_linvel(c, o->rb, v);
        o->moved_ns = plat_now_ns();
        n++;
    }
    return n;
}
// (2026-11-21, a two-game test: the partner fanning the sail knocked us off our own raft) True when we and the other
// player both stand on the same shared raft: the gust goes into the sail, it does not knock us down
// (pvp.inc pvp_wind). SAVAGEWIND_COOP_RAFTCALM=0: knocked down as anywhere else.
static bool raft_shared_ride(const float me[3], const float other[3]) {
    static int on = -1;
    if (!env_on("SAVAGEWIND_COOP_RAFTCALM", &on)) return false;
    for (int i = 0; i < obj_n; i++) {
        struct Obj *o = &objs[i];
        if (!o->body || !strstr(o->name, "Raft") || strstr(o->name, "Sail")) continue;
        const float *p = (const float *)(uintptr_t)(o->body + 0x1a0);
        bool a = hypotf(me[0] - p[0], me[2] - p[2]) < 2.8f && me[1] - p[1] > -1.0f && me[1] - p[1] < 2.5f;
        bool b = hypotf(other[0] - p[0], other[2] - p[2]) < 2.8f && other[1] - p[1] > -1.0f && other[1] - p[1] < 2.5f;
        if (a && b) return true;
    }
    return false;
}
static void obj_park(Cpu *c, struct Obj *o, const float carrier[3]) {
    static const float zero[3] = {0, 0, 0}, q0[4] = {0, 0, 0, 1};
    if (!o->parked) memcpy(o->park_from, (void *)(uintptr_t)(o->body + 0x1a0), 12);
    float m[12];
    o->park_at[0] = carrier[0], o->park_at[1] = carrier[1] - OBJ_PARK_DROP, o->park_at[2] = carrier[2];
    if (LD32(o->rb + 0x68) & 0x80000) rb_fix(c, o->rb, false);
    quat_mtx(q0, o->park_at, m);
    rb_set_transform(c, o->rb, m);
    rb_set_linvel(c, o->rb, zero);
    rb_fix(c, o->rb, true);
    o->parked = true, o->park_wait_ns = 0, o->matched = false, o->have_put = false, o->free_until_ns = 0;
}
// Out of the parking spot: at pos (rotation rot) moving at vel.
static void obj_unpark(Cpu *c, struct Obj *o, const float pos[3], const float rot[4], const float vel[3]) {
    float m[12];
    rb_fix(c, o->rb, false);  // (also when only asked for, flag 0x40000, and not applied yet)
    quat_mtx(rot, pos, m);
    rb_set_transform(c, o->rb, m);
    rb_set_linvel(c, o->rb, vel);
    o->parked = false, o->park_wait_ns = 0, o->matched = false, o->have_put = false;
}
static void obj_send_ev(const struct Obj *o, int kind, const float vel[3]) {
    EvObj e;
    memset(&e, 0, sizeof e);
    e.hash = o->hash, e.gen = o->gen, e.kind = (uint8_t)kind;
    memcpy(e.pos, (void *)(uintptr_t)(o->body + 0x1a0), 12);
    memcpy(e.rot, (void *)(uintptr_t)(o->body + 0x1d0), 16);
    if (vel) memcpy(e.vel, vel, 12);
    bool ok = coop_net_send_event(EV_OBJ, &e, sizeof e);
    LOG("[coop] objects: %s %u %s sent (claim %u, at %.2f %.2f %.2f, vel %.2f %.2f %.2f)%s\n", o->name, o->hash,
        kind == EV_OBJ_RELEASE ? "release" : "broken", o->gen, e.pos[0], e.pos[1], e.pos[2], e.vel[0], e.vel[1],
        e.vel[2], ok ? "" : " - NOT queued");
}
// ---- breakables shared (2026-11-12: a pot broken by one player must break in the partner's game too).
// Probe (plateau pots + barrels): Pot / Barrel actors use the shared Actor class
// whose vfunc +0x2b8 returns actor+0x848 = the life (Pot 1, Barrel 20), and writing 0 there makes the game run its
// own break on the next frame: the break effect (dust), the actor deleted, and the actor's own drop table rolled
// (arrows / an arrow bundle came out of 2 of 5 pots). So:
//  - Sender: a shared breakable (a tracked physics object with a breakable name, or a "break-only" thing that never
//    moves: ore, breakable rocks, town pots, wooden scaffolds) that vanishes near one of us (40 m) while we are
//    together is sent as EV_OBJ kind 2 (gone; same 48 B event as protocol 30, no new packet). Guards against an
//    unload: the area change / load is caught before (objects_frame forgets everything), each vanish waits
//    BRK_WAIT_NS and is dropped if we left the world / the area meanwhile, and more than BRK_MASS vanishing in that
//    window is a load (nothing sent). A copy our following was driving that vanishes while our Link is away from
//    it (> 4 m) is our own physics' doing: not sent (the owner's game decides).
//  - Receiver: its copy gets life 0 (the game's own break: effect, sound, its OWN drops for this player - loot is
//    each player's own, like enemy loot); still there after 1.5 s (or no life): deleted silently. A copy our Link
//    holds is left alone. Ours broken by the event never sends a gone back.
// SAVAGEWIND_COOP_BREAK=0: off (only protocol 30's "broken within 8 s of a release" stays, with a silent delete).
// SAVAGEWIND_COOP_BREAK_LOOT=0: the partner's copy is deleted silently (no break effect, no second set of drops).
#define A_ACTOR_LIFE_VF 0x2b8      // actor vfunc: the life's address (Pot/Barrel: actor+0x848)
#define BRK_NEAR_M 40.0f           // a vanish this close to one of us is a break (unloads happen far away)
#define BRK_WAIT_NS 250000000ull   // a vanish is sent after this (a load in between drops it)
#define BRK_MASS 6                 // more vanishes than this within the wait: a load, nothing sent
#define BRK_KILL_NS 1500000000ull  // the game's break did not remove our copy by then: delete it
#define BRK_MAX 128
static bool brk_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_BREAK") && !strcmp(getenv("SAVAGEWIND_COOP_BREAK"), "0"));
    return on;
}
static bool brk_loot_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_BREAK_LOOT") && !strcmp(getenv("SAVAGEWIND_COOP_BREAK_LOOT"), "0"));
    return on;
}
// Shared physics objects (objs[]) that break: pots, barrels (also BarrelBomb*, Barrel_CDungeon), old barrels,
// breakable iron crates (BreakBoxIron), wooden crates (Kibako*), hidden barrels, Korok pots, shrine stone barrels.
static bool obj_breakable(const char *n) {
    return !strcmp(n, "Pot") || !strncmp(n, "Barrel", 6) || !strncmp(n, "Obj_BarrelOld", 13) ||
           !strncmp(n, "Obj_BreakBox", 12) || !strncmp(n, "Obj_HideBarrel", 14) || !strncmp(n, "Obj_KorokPot", 12) ||
           !strncmp(n, "DgnObj_StoneBarrel", 18) || !strncmp(n, "Kibako", 6) || !strncmp(n, "DgnObj_KibakoDungeon", 20);
}
// Things that never move and only break (no pose sync; their own small table): ore deposits, breakable rocks, rock
// covers, town pots (TwnObj_*Pot*_Act_*), wooden scaffolds.
static bool stasisfoe_on(void) {
    static int on = -1;
    return env_on("SAVAGEWIND_COOP_STASISFOE", &on);
}
static bool brk_delete_only(const char *n) { return !strcmp(n, "IceBlock") || !strcmp(n, "IceWall"); }
static bool brk_only_name(const char *n) {
    if ((!strcmp(n, "IceBlock") || !strcmp(n, "IceWall")) && stasisfoe_on()) return true;
    if (!strncmp(n, "Obj_Mineral", 11) || !strncmp(n, "BreakableRock", 13) || !strncmp(n, "FldObj_BreakableRock", 20) ||
        !strncmp(n, "Obj_RockCover", 13) || !strncmp(n, "FldObj_ScaffoldWoodBreak", 24) ||
        !strncmp(n, "BlockCrack", 10))  // (2026-11-15 play, bomb shrine Dungeon041: cracked walls not shared)
        return true;
    return !strncmp(n, "TwnObj_", 7) && strstr(n, "Pot_") && strstr(n, "_Act_");
}
static struct Brk {
    uint32_t hash;  // 0: an actor without a HashId (remembered so the walk does not ask again)
    uint64_t actor, unit;
    uint32_t unit_id;
    char name[40];
    float pos[3];
    uint64_t kill_ns;
} brks[BRK_MAX];
static int brk_n;
static bool brk_ow, fb_together;  // (set by objects_frame: we are in the overworld / near the partner)
static void fb_add(uint32_t hash, const char *name, const char *why);
static bool farbrk_on(void);
static void fb_quiet(uint32_t hash);
static bool brk_alive(const struct Brk *b) {
    return in_guest(b->actor) && in_guest(b->unit) && LD32(b->unit + UNIT_ID) == b->unit_id &&
           LD64(b->unit + UNIT_ACTOR) == b->actor && LD8(b->actor + PROC_STATE) != 3;
}
static void brk_found(Cpu *c, uint64_t a, const char *name, struct ObjFind *f) {
    if (!brk_on()) return;
    for (int i = 0; i < brk_n; i++)
        if (brks[i].actor == a) return;
    if (f->budget <= 0 || brk_n == BRK_MAX) return;
    float p[3];
    if (actor_dist(a, f->me, p) > 80.0f) return;
    f->budget--;
    struct Brk *b = &brks[brk_n++];
    memset(b, 0, sizeof *b);
    b->hash = actor_hash(c, a), b->actor = a, b->unit = LD64(a + PROC_UNIT), b->unit_id = LD32(a + PROC_ID);
    snprintf(b->name, sizeof b->name, "%s", name);
    memcpy(b->pos, p, 12);
}
// Vanished here, sent after BRK_WAIT_NS unless a load / area change came in between.
static struct { uint32_t hash; uint16_t gen; float pos[3]; uint64_t at_ns; char name[40]; } brk_pend[BRK_MASS + 1];
static int brk_pend_n, brk_pend_over;
static void brk_vanished(uint32_t hash, uint16_t gen, const char *name, const float pos[3], uint64_t now,
                         const char *why) {
    if (!hash) return;
    if (brk_pend_n > BRK_MASS - 1) { brk_pend_over++; return; }
    brk_pend[brk_pend_n].hash = hash, brk_pend[brk_pend_n].gen = gen, brk_pend[brk_pend_n].at_ns = now;
    memcpy(brk_pend[brk_pend_n].pos, pos, 12);
    snprintf(brk_pend[brk_pend_n].name, sizeof brk_pend[brk_pend_n].name, "%s", name);
    brk_pend_n++;
    LOG("[coop] break: %s %u broke here (%s) at %.1f %.1f %.1f\n", name, hash, why, pos[0], pos[1], pos[2]);
}
static void brk_pend_drop(const char *why) {
    if (brk_pend_n || brk_pend_over)
        LOG("[coop] break: %d vanished object(s) not sent (%s)\n", brk_pend_n + brk_pend_over, why);
    brk_pend_n = brk_pend_over = 0;
}
static void brk_pend_flush(uint64_t now) {
    if (brk_pend_over) { brk_pend_drop("too many at once: a load or an unload"); return; }
    for (int i = 0; i < brk_pend_n; i++) {
        if (now - brk_pend[i].at_ns < BRK_WAIT_NS) continue;
        EvObj e;
        memset(&e, 0, sizeof e);
        e.hash = brk_pend[i].hash, e.gen = brk_pend[i].gen, e.kind = EV_OBJ_GONE, e.rot[3] = 1;
        e.pad = brk_ow;  // (1: an overworld hash: the partner remembers it for when the thing loads there)
        if (brk_ow) fb_add(brk_pend[i].hash, brk_pend[i].name, "broken here");
        memcpy(e.pos, brk_pend[i].pos, 12);
        bool ok = coop_net_send_event(EV_OBJ, &e, sizeof e);
        LOG("[coop] break: %s %u broken here: sent%s\n", brk_pend[i].name, brk_pend[i].hash, ok ? "" : " - NOT queued");
        brk_pend[i--] = brk_pend[--brk_pend_n];
    }
}
// The game's own break on our copy: its life to 0 (true), or false when it has no life to take (then delete it).
static bool brk_kill(Cpu *c, uint64_t a) {
    if (!brk_loot_on()) return false;
    uint64_t vt = LD64(a), fn = in_guest(vt) ? LD64(vt + A_ACTOR_LIFE_VF) : 0;
    if (fn < 0x7000000000ull || fn >= 0x7003000000ull) return false;
    Cpu saved = *c;
    c->x[0] = a;
    guest_call(c, fn);
    uint64_t life = c->x[0];
    *c = saved;
    if (life < a || life >= a + 0x2000 || (int32_t)LD32(life) <= 0) return false;  // (not inside the actor: unknown)
    ST32(life, 0);
    return true;
}
// The other player's break of a shared thing (objects_ev): true when we know it.
static bool brk_apply(Cpu *c, uint32_t hash, const float at[3], bool silent) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < obj_n; i++) {
        struct Obj *o = &objs[i];
        if (o->hash != hash || !obj_alive(o)) continue;
        if (o->carried) {
            LOG("[coop] break: %s %u broke in the other game - ignored: our Link holds it\n", o->name, hash);
            return true;
        }
        if (o->kill_ns) return true;
        if (o->parked) {  // (out of sight under the carrier: just remove it)
            LOG("[coop] break: %s %u broke in the other game: removed here (it was parked)\n", o->name, hash);
            uint64_t a = o->actor;
            *o = objs[--obj_n];
            despawn_now(c, a, ACTOR_DELETE_REASON);
            return true;
        }
        if (!silent && brk_kill(c, o->actor)) {
            o->kill_ns = now + BRK_KILL_NS;
            LOG("[coop] break: %s %u broke in the other game: broken here too (life 0)\n", o->name, hash);
        } else {
            LOG("[coop] break: %s %u broke in the other game: removed here\n", o->name, hash);
            uint64_t a = o->actor;
            *o = objs[--obj_n];
            despawn_now(c, a, ACTOR_DELETE_REASON);
        }
        return true;
    }
    for (int i = 0; i < brk_n; i++) {
        struct Brk *b = &brks[i];
        if (b->hash != hash || !brk_alive(b)) continue;
        if (b->kill_ns) return true;
        // (2026-11-20) ice melts by its volume, not a life: life 0 left it standing. It is removed instead.
        if (!silent && !brk_delete_only(b->name) && brk_kill(c, b->actor)) {
            b->kill_ns = now + BRK_KILL_NS;
            LOG("[coop] break: %s %u broke in the other game: broken here too (life 0)\n", b->name, hash);
        } else {
            LOG("[coop] break: %s %u broke in the other game: removed here\n", b->name, hash);
            uint64_t a = b->actor;
            *b = brks[--brk_n];
            despawn_now(c, a, ACTOR_DELETE_REASON);
        }
        return true;
    }
    (void)at;
    return false;
}
// Per frame (objects_frame, together, in the world): break-only things that vanished, our forced breaks that did
// not happen, then the waiting vanishes out.
static volatile uint32_t brk_del_dev;  // `coop brkdel <hash>` (dev): delete our break-only thing as the game would
static void brk_frame(Cpu *c, uint64_t now, const float me[3], const struct ObjNear *nr, bool overworld) {
    uint32_t del = brk_del_dev;
    if (del) {
        brk_del_dev = 0;
        int i = 0;
        while (i < brk_n && !(brks[i].hash == del && brk_alive(&brks[i]))) i++;
        LOG("[coop] break: dev delete %u: %s\n", del, i < brk_n ? brks[i].name : "not in the list");
        if (i < brk_n) despawn_now(c, brks[i].actor, ACTOR_DELETE_REASON);
    }
    for (int i = 0; i < brk_n; i++) {
        struct Brk *b = &brks[i];
        if (!brk_alive(b)) {
            bool near = obj_near_any(nr, me, b->pos, BRK_NEAR_M);
            if (b->hash && !b->kill_ns && near) brk_vanished(b->hash, 0, b->name, b->pos, now, "gone");
            brks[i--] = brks[--brk_n];
            continue;
        }
        if (b->kill_ns && now > b->kill_ns) {
            LOG("[coop] break: %s %u still here %.1f s after its life 0: removed\n", b->name, b->hash,
                (double)BRK_KILL_NS / 1e9);
            uint64_t a = b->actor;
            brks[i--] = brks[--brk_n];
            despawn_now(c, a, ACTOR_DELETE_REASON);
            continue;
        }
        if (overworld && !obj_near_any(nr, me, b->pos, 120.0f)) brks[i--] = brks[--brk_n];
    }
    brk_pend_flush(now);
}

// ---- far breaks (2026-11-19, audit S1): a break stays broken for the partner also when we were apart.
// Before: breakables were tracked and sent only while the players were near each other (120 m in the overworld), and a
// break received for a thing we did not track was thrown away. Now (overworld only: hashes are not unique across maps)
// - every overworld break we see (together or not) goes out as the same EV_OBJ gone event (pad = 1, no new packet:
//   old receivers ignore pad), and both sides REMEMBER the hash for the session (512 hashes, 60 min each: ore and pots
//   come back after a blood moon);
// - a watch list of the breakables loaded around us (hash, position); one whose hash is remembered is removed silently
//   (no break effect, no drops: the loot stayed with whoever broke it) as soon as it is loaded; one that vanishes
//   within 40 m of us while we are apart is a break (same debounce / load guard as the together path: brk_vanished).
// SAVAGEWIND_COOP_FARBREAK=0: off (back to: shared only while together).
#define FB_SET 512
#define FB_WATCH 192
#define FB_TTL_NS (3600ull * 1000000000ull)
static bool farbrk_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_FARBREAK") && !strcmp(getenv("SAVAGEWIND_COOP_FARBREAK"), "0"));
    return on;
}
static struct { uint32_t hash; uint64_t at_ns; } fb_set[FB_SET];
static int fb_set_n, fb_head;
static bool fb_has(uint32_t h) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < fb_set_n; i++)
        if (fb_set[i].hash == h) return now - fb_set[i].at_ns < FB_TTL_NS;
    return false;
}
static void fb_add(uint32_t hash, const char *name, const char *why) {
    if (!hash || !farbrk_on()) return;
    uint64_t now = plat_now_ns();
    for (int i = 0; i < fb_set_n; i++)
        if (fb_set[i].hash == hash) { fb_set[i].at_ns = now; return; }
    int k = fb_set_n < FB_SET ? fb_set_n++ : (fb_head++ % FB_SET);
    fb_set[k].hash = hash, fb_set[k].at_ns = now;
    LOG("[coop] farbreak: %s %u remembered (%s; %d remembered)\n", name, hash, why, fb_set_n);
}
static struct FbW {
    uint64_t actor, unit, first_ns, seen_ns;
    uint32_t unit_id, hash;
    char name[40];
    float pos[3];
    bool quiet;  // (its vanishing is our own removal or the partner's break: not ours to send)
} fbw[FB_WATCH];
static int fbw_n;
static bool fbw_alive(const struct FbW *w) {
    return in_guest(w->actor) && in_guest(w->unit) && LD32(w->unit + UNIT_ID) == w->unit_id &&
           LD64(w->unit + UNIT_ACTOR) == w->actor && LD8(w->actor + PROC_STATE) != 3;
}
static void fb_quiet(uint32_t hash) {
    for (int i = 0; i < fbw_n; i++)
        if (fbw[i].hash == hash) fbw[i].quiet = true;
}
static bool farbrk_name(const char *n) { return obj_breakable(n) || brk_only_name(n); }
struct FbFind { uint64_t now; int budget; float me[3]; };
static void farbrk_found(Cpu *c, uint64_t a, const char *name, void *ctx) {
    struct FbFind *f = ctx;
    for (int i = 0; i < fbw_n; i++)
        if (fbw[i].actor == a) return;
    float p[3];
    if (f->budget <= 0 || fbw_n == FB_WATCH || actor_dist(a, f->me, p) > 150.0f) return;
    f->budget--;
    struct FbW *w = &fbw[fbw_n++];
    memset(w, 0, sizeof *w);
    w->actor = a, w->unit = LD64(a + PROC_UNIT), w->unit_id = LD32(a + PROC_ID), w->hash = actor_hash(c, a);
    w->first_ns = w->seen_ns = f->now;
    snprintf(w->name, sizeof w->name, "%s", name);
    memcpy(w->pos, p, 12);
}
static void farbrk_frame(Cpu *c, uint64_t now, const float me[3], bool together) {
    static uint64_t scan_ns;
    for (int i = 0; i < fbw_n; i++) {
        struct FbW *w = &fbw[i];
        if (!fbw_alive(w)) {
            bool fresh = now - w->seen_ns < 500000000ull && now - w->first_ns > 1000000000ull;
            if (!together && !w->quiet && w->hash && fresh && dist3(w->pos, me) < BRK_NEAR_M)
                brk_vanished(w->hash, 0, w->name, w->pos, now, "gone, partner not near");
            fbw[i--] = fbw[--fbw_n];
            continue;
        }
        w->seen_ns = now;
        actor_dist(w->actor, me, w->pos);
        if (dist3(w->pos, me) > 220.0f) { fbw[i--] = fbw[--fbw_n]; continue; }
        if (w->hash && now - w->first_ns > 500000000ull && fb_has(w->hash)) {
            LOG("[coop] farbreak: %s %u was broken in the other game: removed here, silently (no drops)\n", w->name,
                w->hash);
            uint64_t a = w->actor;
            uint32_t h = w->hash;
            fbw[i--] = fbw[--fbw_n];
            if (!brk_apply(c, h, NULL, true)) despawn_now(c, a, ACTOR_DELETE_REASON);
        }
    }
    if (now >= scan_ns) {
        struct FbFind f = {now, 12, {me[0], me[1], me[2]}};
        walk_actors(c, farbrk_name, farbrk_found, &f);
        scan_ns = now + (f.budget > 0 ? 2000000000ull : 200000000ull);
    }
    if (!together) brk_pend_flush(now);
}

// The other player's EV_OBJ (game thread, from world_event).
static void objects_ev(Cpu *c, const uint8_t *data, int len) {
    if (len != (int)sizeof(EvObj)) return;
    EvObj e;
    memcpy(&e, data, sizeof e);
    bool ok = e.hash && (e.kind == EV_OBJ_RELEASE || e.kind == EV_OBJ_GONE);
    for (int k = 0; k < 3 && ok; k++)
        ok = isfinite(e.pos[k]) && fabsf(e.pos[k]) < 100000.0f && isfinite(e.vel[k]) && fabsf(e.vel[k]) <= OBJ_VEL_MAX;
    float qn = 0;
    for (int k = 0; k < 4 && ok; k++) ok = isfinite(e.rot[k]), qn += e.rot[k] * e.rot[k];
    if (!ok || qn < 0.9f || qn > 1.1f) { LOG("[coop] objects: bad EV_OBJ dropped\n"); return; }
    if (e.kind == EV_OBJ_GONE && brk_on()) {  // (breakables: any shared object, whoever owns it)
        if (e.pad == 1 && farbrk_on()) {  // (an overworld break: remembered, also when it is far from us)
            fb_add(e.hash, "?", "broke in the other game");
            fb_quiet(e.hash);
        }
        if (!local_in_world()) return;
        if (!brk_apply(c, e.hash, e.pos, !fb_together))
            LOG("[coop] break: %u broke in the other game: not a shared object here\n", e.hash);
        return;
    }
    if (!local_in_world() || !carry_on()) return;
    struct Obj *o = NULL;
    for (int i = 0; i < obj_n && !o; i++)
        if (objs[i].hash == e.hash && obj_alive(&objs[i])) o = &objs[i];
    if (!o) { LOG("[coop] objects: EV_OBJ %d for %u: not a shared object here\n", e.kind, e.hash); return; }
    if (o->carried || (o->owner == 1 && (int16_t)(e.gen - o->gen) <= 0)) {  // (ours now: a newer claim of ours)
        LOG("[coop] objects: EV_OBJ %d for %s %u (claim %u) ignored: ours (claim %u)\n", e.kind, o->name, o->hash,
            e.gen, o->gen);
        return;
    }
    uint64_t now = plat_now_ns();
    if (e.kind == EV_OBJ_GONE) {
        LOG("[coop] objects: %s %u broke in the other game: removed here\n", o->name, o->hash);
        uint64_t a = o->actor;
        *o = objs[--obj_n];
        despawn_now(c, a, ACTOR_DELETE_REASON);
        return;
    }
    // (their newest pose, when it is from after the release and fresher than the event, is the better start: the
    // object has flown on while the event travelled)
    CoopObject ro;
    double age = 99;
    bool newer = coop_net_remote_object(e.hash, &ro, &age) && age < 0.1 && ro.gen == e.gen &&
                 (ro.flags & COOP_OBJ_OWNED) && !(ro.flags & COOP_OBJ_CARRIED) && dist3(ro.pos, e.pos) < 5.0f;
    if (newer) {
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = ro.pos[k] + ro.vel[k] * (float)age;
        obj_unpark(c, o, p, ro.rot, ro.vel);
    } else {
        obj_unpark(c, o, e.pos, e.rot, e.vel);
    }
    o->owner = 2, o->gen = e.gen, o->changed_ns = now, o->released_ns = now;
    if (world_ev_origin >= 0) o->opid = (int8_t)world_ev_origin;  // (59: who let go of it)
    o->free_until_ns = now + OBJ_FREE_NS, o->free_rest_ns = 0;
    LOG("[coop] objects: %s %u released by the other player at %.2f %.2f %.2f, vel %.2f %.2f %.2f: flying here%s\n",
        o->name, o->hash, e.pos[0], e.pos[1], e.pos[2], e.vel[0], e.vel[1], e.vel[2],
        newer ? " (from their newer pose)" : "");
}

// `coop objtrace <hash> <seconds>` (dev): every 0.2 s, the shared object's actor matrix, body, flags, owner and
// whether our player holds it (*(player actor + 0x88)), to the log.
static struct { volatile uint32_t hash; volatile uint64_t until_ns; uint64_t next_ns; } objtrace_dev;
static void objtrace_line(const struct Obj *o, const CoopObject *ro, bool have_ro, double age, uint64_t now) {
    if (!objtrace_dev.hash || o->hash != objtrace_dev.hash || now > objtrace_dev.until_ns) return;
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t held = in_guest(pa) ? LD64(pa + 0x88) : 0;
    static int fast;  // (every frame for 20 frames after the hold changes)
    static uint64_t held_was;
    if (held != held_was) fast = 20, held_was = held;
    if (fast > 0) fast--;
    else if (now < objtrace_dev.next_ns) return;
    objtrace_dev.next_ns = now + 200000000ull;
    const float *am = (const float *)(uintptr_t)(o->actor + ACTOR_MTX), *bp = (const float *)(uintptr_t)(o->body + 0x1a0);
    LOG("[coop] objtrace %u: actor %.2f %.2f %.2f body %.2f %.2f %.2f flags %08x/%08x owner %d gen %u matched %d "
        "held %d(%llx) vel %.2f %.2f %.2f remote %s %.2f %.2f %.2f fl %u gen %u age %.2f\n",
        o->hash, am[3], am[7], am[11], bp[0], bp[1], bp[2], LD32(o->rb + 0x68), LD32(o->rb + 0x6c), o->owner, o->gen,
        o->matched, held == o->actor, (unsigned long long)held, o->vel[0], o->vel[1], o->vel[2], have_ro ? "yes" : "no", have_ro ? ro->pos[0] : 0.f,
        have_ro ? ro->pos[1] : 0.f, have_ro ? ro->pos[2] : 0.f, have_ro ? ro->flags : 0u, have_ro ? ro->gen : 0u,
        have_ro ? age : -1.0);
}
static bool col_near_avatar(const float p[3], float r);
static int rails_fill(Cpu *c, CoopObject *out, int max);  // (coop/rails.inc)
static void objects_frame(Cpu *c) {
    if (!objects_on()) return;
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    bool overworld = !strcmp(area, "MainField");
    brk_ow = overworld;
    float me[3] = {0, 0, 0};
    if (local_in_world()) memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    struct ObjNear nr;  // (59: every world player near us; two players: the partner)
    obj_near_players(&nr, here, !strncmp(area, "CDungeon", 8), overworld, me);
    bool together = nr.n > 0 && local_in_world();
    if (here != obj_area || !local_in_world()) {  // (a load: the actors are gone)
        if (obj_n) LOG("[coop] objects: %d forgotten (area change or load)\n", obj_n);
        obj_n = 0, obj_area = here, brk_n = 0, fbw_n = 0, fb_together = false;
        brk_pend_drop("area change or load");
        coop_net_set_objects(NULL, 0);
        return;
    }
    fb_together = together;
    if (farbrk_on() && brk_on() && overworld) farbrk_frame(c, now, me, together);
    uint64_t held = carry_on() ? local_held() : 0;
    if (held) {  // (local_look: a shared object's prop is not kept on the avatar after the throw)
        carry_last_shared = false;
        for (int i = 0; i < obj_n; i++)
            if (objs[i].actor == held) carry_last_shared = true;
    }
    if (!together) {
        // A copy parked for the other player's carry: back into the world where it was last seen (they left, the
        // connection dropped, or the area differs now).
        for (int i = 0; i < obj_n; i++) {
            struct Obj *o = &objs[i];
            if (!o->parked || !obj_alive(o)) continue;
            CoopObject ro;
            double age;
            static const float zero[3] = {0, 0, 0};
            if (coop_net_remote_object(o->hash, &ro, &age)) obj_unpark(c, o, ro.pos, ro.rot, zero);
            else { float q0[4] = {0, 0, 0, 1}; obj_unpark(c, o, o->park_from, q0, zero); }
            o->owner = 0, o->changed_ns = now;
            LOG("[coop] objects: %s %u unparked (not together any more)\n", o->name, o->hash);
        }
        brk_n = 0;  // (break-only things: found again when together; what breaks meanwhile is not shared)
        if (!(farbrk_on() && brk_on() && overworld)) brk_pend_drop("not together any more");  // (far breaks: sent by farbrk_frame)
        coop_net_set_objects(NULL, 0);
        return;
    }
    static uint64_t look_ns;
    if (now >= look_ns) {  // new objects (a few per frame: finding a body walks memory)
        struct ObjFind f = {now, 6, overworld, {me[0], me[1], me[2]}};
        int before = obj_n;
        walk_actors(c, obj_or_brk_name, obj_found, &f);
        look_ns = now + (f.budget > 0 ? 1000000000ull : 50000000ull);
        // (2026-11-21: one object found and dropped again every look logged this 15000 times in a session: only a
        // new high since the area's list was empty is logged)
        static int obj_logged_max;
        if (before == 0) obj_logged_max = 0;
        if (obj_n > before && obj_n > obj_logged_max) {
            LOG("[coop] objects: %d shared objects known (+%d)\n", obj_n, obj_n - before);
            obj_logged_max = obj_n;
        }
    }
    bool host = coop_net_is_host();
    uint64_t frozen_actor = stasis_target(c);  // (what our Stasis holds now, 0: nothing)
    CoopObject out[COOP_OBJ_MAX];
    int out_n = rails_fill(c, out, 4);  // (rails.inc) the host's rail lifts first
    static int rr;  // round-robin start among owned objects (more than fit in a packet)
    for (int i = 0; i < obj_n; i++) {
        struct Obj *o = &objs[i];
        if (!obj_alive(o)) {
            // A breakable that vanished near one of us, seen alive just before (not while we were apart): broken
            // here. Not when the other player's break did it (kill_ns), not a copy parked for their carry, and not a
            // copy our following drove while our Link was away from it (our physics, not a break by our player).
            // (and known for 1 s: some objects are found and dropped again every look - seen before this change)
            bool fresh = now - o->last_ns < 500000000ull && now - o->found_ns > 1000000000ull;
            bool near = obj_near_any(&nr, me, o->last, BRK_NEAR_M);
            bool released = carry_on() && o->released_ns && now - o->released_ns < OBJ_GONE_NS;
            bool driven = o->driven_ns && now - o->driven_ns < 500000000ull && dist3(o->last, me) > 4.0f;
            if (o->kill_ns || o->parked || !brk_on()) {
                // (nothing new to share; the protocol 30 rule below, unchanged, when breakables are off)
            } else if (fresh && near && (obj_breakable(o->name) || released)) {
                if (driven && !released)
                    LOG("[coop] break: %s %u vanished while our following drove it (our Link %.1f m away): not sent\n",
                        o->name, o->hash, dist3(o->last, me));
                else
                    brk_vanished(o->hash, o->gen, o->name, o->last, now, released ? "after its release" : "gone");
            }
            // (broken soon after a release - ours or theirs: break it in the other game too)
            if (!brk_on() && carry_on() && o->released_ns && now - o->released_ns < OBJ_GONE_NS && !o->parked) {
                EvObj e;
                memset(&e, 0, sizeof e);
                e.hash = o->hash, e.gen = o->gen, e.kind = EV_OBJ_GONE, e.rot[3] = 1;
                memcpy(e.pos, o->last, 12);
                bool ok = coop_net_send_event(EV_OBJ, &e, sizeof e);
                LOG("[coop] objects: %s %u broke here %.1f s after its release: sent%s\n", o->name, o->hash,
                    (double)(now - o->released_ns) / 1e9, ok ? "" : " - NOT queued");
            }
            if (objtrace_dev.hash == o->hash && now < objtrace_dev.until_ns)
                LOG("[coop] objtrace %u: dropped (actor %d unit %d id %d proc %d body %d)\n", o->hash,
                    in_guest(o->actor), in_guest(o->unit), in_guest(o->unit) && LD32(o->unit + UNIT_ID) == o->unit_id,
                    in_guest(o->actor) ? LD8(o->actor + PROC_STATE) : -1, in_guest(o->rb) && LD64(o->rb + 0x78) == o->body);
            objs[i--] = objs[--obj_n];
            continue;
        }
        if (o->kill_ns) {  // (broken by the other player's break: the game's own break runs; nothing else to do)
            if (now > o->kill_ns) {
                LOG("[coop] break: %s %u still here %.1f s after its life 0: removed\n", o->name, o->hash,
                    (double)BRK_KILL_NS / 1e9);
                uint64_t a = o->actor;
                objs[i--] = objs[--obj_n];
                despawn_now(c, a, ACTOR_DELETE_REASON);
            }
            continue;
        }
        float pos[3], rot[4];
        memcpy(pos, (void *)(uintptr_t)(o->body + 0x1a0), 12);
        memcpy(rot, (void *)(uintptr_t)(o->body + 0x1d0), 16);
        if (overworld && o->owner == 0 && !obj_near_any(&nr, me, pos, 120.0f)) {
            objs[i--] = objs[--obj_n];
            continue;
        }
        CoopObject ro;
        double age = 99;
        bool have_ro = coop_net_remote_object(o->hash, &ro, &age);
        bool theirs = have_ro && age < OBJ_STALE_S && (ro.flags & COOP_OBJ_OWNED);
        objtrace_line(o, &ro, have_ro, age, now);
        bool settled = now - o->found_ns > OBJ_SETTLE_NS;
        // Our player holds it: ours at once (a newer claim; no settle/calm wait: lifting is unambiguous).
        bool holding = held && o->actor == held;
        if (holding) {
            if (o->owner != 1) {
                uint16_t g = o->gen;
                if (have_ro && (int16_t)(ro.gen - g) > 0) g = ro.gen;
                o->owner = 1, o->gen = (uint16_t)(g + 1), o->changed_ns = now;
                LOG("[coop] objects: %s %u lifted here: ours (claim %u)\n", o->name, o->hash, o->gen);
            }
            if (!o->carried) LOG("[coop] objects: %s %u carried here\n", o->name, o->hash);
            o->carried = true, o->moved_ns = now, o->have_put = false, o->matched = false, o->free_until_ns = 0;
            o->release_wait = 0;
        } else if (o->carried) {
            o->carried = false, o->release_wait = 3, o->released_ns = now;
            LOG("[coop] objects: %s %u let go here (at %.2f %.2f %.2f)\n", o->name, o->hash, pos[0], pos[1], pos[2]);
        }
        // (2026-11-14, protocol 43) Our Stasis holds it: ours at once, like a lift (the frozen object is launched by
        // our player's hits, which only our game sees); sent with COOP_OBJ_STASIS while it lasts.
        bool fake = stasis_fake_holds(o->hash, now);
        bool frozen = (frozen_actor && o->actor == frozen_actor) || fake;
        if (frozen) {
            if (o->owner != 1) {
                uint16_t g = o->gen;
                if (have_ro && (int16_t)(ro.gen - g) > 0) g = ro.gen;
                o->owner = 1, o->gen = (uint16_t)(g + 1), o->changed_ns = now;
                LOG("[coop] objects: %s %u frozen by our Stasis: ours (claim %u)\n", o->name, o->hash, o->gen);
            }
            if (!o->stasis) LOG("[coop] stasis: %s %u frozen here%s\n", o->name, o->hash, fake ? " (stasisfake)" : "");
            if (fake && !o->fake_fixed) {
                if (LD32(o->rb + 0x68) & 0x80000) rb_fix(c, o->rb, false);
                static const float zero[3] = {0, 0, 0};
                rb_set_linvel(c, o->rb, zero);
                rb_fix(c, o->rb, true);
                o->fake_fixed = true;
            }
            o->stasis = true, o->moved_ns = now, o->have_put = false, o->matched = false, o->free_until_ns = 0;
            if (o->frozen_there) o->frozen_there = false;  // (ours now)
        } else if (o->stasis) {
            o->stasis = false, o->moved_ns = now;
            if (o->fake_fixed) rb_fix(c, o->rb, false), o->fake_fixed = false;
            LOG("[coop] stasis: %s %u released here (at %.2f %.2f %.2f)\n", o->name, o->hash, pos[0], pos[1], pos[2]);
        }
        bool gyro_driving = false;
        bool maze_ball = gyro_maze_ball(o->hash, &gyro_driving);
        // The active maze driver needs their own ball physics immediately on takeover. A previous
        // owner's recurring pose would otherwise pin this copy before it could roll and claim itself.
        if (gyro_driving && !holding && !frozen &&
            (o->owner != 1 || (theirs && (int16_t)(ro.gen - o->gen) >= 0))) {
            uint16_t g = o->gen;
            if (theirs && (int16_t)(ro.gen - g) > 0) g = ro.gen;
            o->owner = 1, o->gen = (uint16_t)(g + 1), o->changed_ns = o->moved_ns = now;
            o->have_put = o->matched = false;
            LOG("[coop] objects: %s %u follows our maze tilt (claim %u)\n", o->name, o->hash, o->gen);
        }
        // A newer claim of theirs wins (a tie: the host's) - not while our player holds it or our Stasis does.
        if (theirs && !holding && !frozen && o->owner != 2 &&
            (o->owner == 0 || ro.gen > o->gen || (ro.gen == o->gen && !host))) {
            LOG("[coop] objects: %s %u is the other player's now (claim %u, player %u)\n", o->name, o->hash, ro.gen,
                ro.pid);
            o->owner = 2, o->gen = ro.gen, o->opid = (int8_t)ro.pid, o->have_put = false, o->matched = false;
            o->changed_ns = now;
        } else if (theirs && o->owner == 2 && ro.pid != (uint8_t)o->opid && (int16_t)(ro.gen - o->gen) >= 0) {
            // (59) More than two players: the copy we follow changed hands between two other players (a newer claim
            // of a third one): the same fresh start and calm period as any change of hands.
            LOG("[coop] objects: %s %u: player %d's -> player %u's now (claim %u)\n", o->name, o->hash, o->opid,
                ro.pid, ro.gen);
            o->gen = ro.gen, o->opid = (int8_t)ro.pid, o->have_put = false, o->matched = false, o->changed_ns = now;
        }
        // (no claim of ours right after a change of hands: the copy is still catching up; seen 2026-11-08 without
        // this: two games took a box from each other 100 times a minute and flung it away)
        bool hinged = o->hinged && hinge_dev;  // (HINGE; `coop hinge 0|1` switches it live for A/B tests)
        bool calm = now - o->changed_ns > (hinged ? OBJ_HINGE_CALM_NS : OBJ_CALM_NS);
        // (the avatar's body - collide_frame - pushes our copy where the other player's own body pushes theirs:
        // not a move of our player's, no claim while it is right there and our Link is not)
        if (calm && col_near_avatar(pos, 1.8f) && dist3(pos, me) > 1.6f) calm = false;
        // (2026-11-21, 11-20 play: a raft fanned with the Korok Leaf changed hands back and forth) A raft the other
        // player stands on and ours does not is theirs: the rider's leaf and weight move it, never our copy's drift.
        static int raft_c = -1;
        // (2026-11-21 RAFTGUST) Both on it: it stays with its owner (no claim of ours while it is theirs and their
        // player is on it); our Leaf gusts reach their copy through our avatar's swing (raft_gust in their game).
        if (calm && strstr(o->name, "Raft") && env_on("SAVAGEWIND_COOP_RAFTRIDER", &raft_c) && col_near_avatar(pos, 3.5f) &&
            (dist3(pos, me) > 3.5f || o->owner == 2))
            calm = false;
        if (calm && hinged && col_near_avatar(pos, OBJ_HINGE_AVATAR_M) && dist3(pos, me) > OBJ_HINGE_AVATAR_M) calm = false;
        float move_m = hinged ? OBJ_HINGE_MOVE_M : OBJ_MOVE_M, claim_m = hinged ? OBJ_HINGE_CLAIM_M : OBJ_CLAIM_M;
        // (hinged: a claim condition must hold OBJ_HINGE_FRAMES looks in a row - claim_frames counts them - and a
        // resting copy that left its spot ("hold") is left alone meanwhile instead of being put back)
        bool claim_seen = false, claim_hold = false;
#define CLAIM_OK(cond, hold) ((cond) && (claim_seen = true, claim_hold |= hinged && (hold), !hinged || o->claim_frames + 1 >= OBJ_HINGE_FRAMES))
        // (2026-11-14) While the other player tilts a motion-control platform, what rolls on ours is their tilt, not
        // our player: their game owns it (the ball of a tilt maze).
        if (calm && gyro_partner_tilting()) calm = false;
        if (objtrace_dev.hash == o->hash && now < objtrace_dev.until_ns)
            LOG("[coop] objtrace %u: settled %d (found %.1f s) calm %d (changed %.1f s, avatar near %d, gyro %d) rest %.2f m "
                "parked %d free %d\n", o->hash, settled, (double)(now - o->found_ns) / 1e9, calm,
                (double)(now - o->changed_ns) / 1e9, col_near_avatar(pos, 1.8f), gyro_partner_tilting(),
                dist3(pos, o->rest), o->parked, o->free_until_ns != 0);
        bool carried_there = carry_on() && theirs && (ro.flags & COOP_OBJ_CARRIED) && o->owner == 2 &&
                             (!o->released_ns || now - o->released_ns > 300000000ull);
        if (carried_there) {
            // The other player holds it: our copy out of sight (they see their own; we see the prop puppet).
            if (!o->parked) {
                obj_park(c, o, obj_player_pos(&nr, o->opid));
                LOG("[coop] objects: %s %u carried by the other player: parked here (from %.2f %.2f %.2f)\n", o->name,
                    o->hash, o->park_from[0], o->park_from[1], o->park_from[2]);
            } else if (dist3(pos, o->park_at) > 1.0f || !(LD32(o->rb + 0x68) & 0xc0000)) {
                obj_park(c, o, obj_player_pos(&nr, o->opid));  // (something moved or woke it)
            }
            o->park_wait_ns = 0;
        } else if (o->parked && o->owner == 2 && theirs) {
            // They let go of it: the release event (reliable) normally comes now; if it does not, after 0.5 s the
            // normal following below brings our copy to their pose.
            if (!o->park_wait_ns) o->park_wait_ns = now + 500000000ull;
            if (now >= o->park_wait_ns) {
                static const float zero[3] = {0, 0, 0};
                obj_unpark(c, o, ro.pos, ro.rot, zero);
                o->changed_ns = now;
                LOG("[coop] objects: %s %u: no release event, unparked to their pose\n", o->name, o->hash);
            }
        } else if (o->parked && !theirs) {
            static const float zero[3] = {0, 0, 0};
            if (have_ro) obj_unpark(c, o, ro.pos, ro.rot, zero);
            else { float q0[4] = {0, 0, 0, 1}; obj_unpark(c, o, o->park_from, q0, zero); }
            LOG("[coop] objects: %s %u unparked (the other player stopped sending it)\n", o->name, o->hash);
        }
        if (o->free_until_ns && o->owner == 2 && theirs && !o->parked) {
            // Released here: our own physics flies it (no following, no claims) until it has rested 0.5 s.
            static const float zero[3] = {0, 0, 0};
            if (dist3(o->vel, zero) < 0.1f && now - o->changed_ns > 500000000ull) {
                if (!o->free_rest_ns) o->free_rest_ns = now;
            } else {
                o->free_rest_ns = 0;
            }
            // (a roll or bounce that went another way than theirs: following again from here, while it is still
            // close, is a small step; at the end of a long roll it was an 18 m jump - test_carry_real 2026-11-11)
            float there[3];
            float lead = (float)(age < 0.2 ? age : 0.2);
            for (int k = 0; k < 3; k++) there[k] = ro.pos[k] + ro.vel[k] * lead;
            bool drift = now - o->changed_ns > 300000000ull && dist3(pos, there) > 2.0f;
            if (drift || now >= o->free_until_ns || (o->free_rest_ns && now - o->free_rest_ns > 500000000ull)) {
                LOG("[coop] objects: %s %u %s here at %.2f %.2f %.2f (theirs %.2f %.2f %.2f, %.2f m apart)\n", o->name,
                    o->hash, drift ? "went its own way: following again" : "landed", pos[0], pos[1], pos[2],
                    ro.pos[0], ro.pos[1], ro.pos[2], dist3(pos, ro.pos));
                o->free_until_ns = 0, o->changed_ns = now, o->matched = false, o->have_put = false;
            }
        } else if (o->free_until_ns && (o->owner != 2 || !theirs)) {
            o->free_until_ns = 0;
        }
        // (2026-11-14, protocol 43) The other player's Stasis holds it: our copy fixed at their pose until it ends;
        // then the normal following below flies it along their launch (calm: none of that is our player's move).
        bool stasis_there = stasis_on() && theirs && (ro.flags & COOP_OBJ_STASIS) && o->owner == 2 && !o->parked;
        if (stasis_there) {
            static const float zero[3] = {0, 0, 0};
            if (!o->frozen_there || dist3(pos, ro.pos) > 0.03f || !(LD32(o->rb + 0x68) & 0xc0000)) {
                float m[12];
                if (LD32(o->rb + 0x68) & 0x80000) rb_fix(c, o->rb, false);
                quat_mtx(ro.rot, ro.pos, m);
                rb_set_transform(c, o->rb, m);
                rb_set_linvel(c, o->rb, zero);
                rb_fix(c, o->rb, true);
            }
            if (!o->frozen_there) {
                LOG("[coop] stasis: %s %u frozen by the other player's Stasis: our copy held at %.2f %.2f %.2f\n",
                    o->name, o->hash, ro.pos[0], ro.pos[1], ro.pos[2]);
                stasis_fx(c, o->actor, true);
            }
            o->frozen_there = true, o->matched = false, o->have_put = false, o->free_until_ns = 0, o->driven_ns = now;
        } else if (o->frozen_there) {
            o->frozen_there = false, o->matched = false, o->have_put = false, o->changed_ns = now;
            rb_fix(c, o->rb, false);
            stasis_fx(c, o->actor, false);
            LOG("[coop] stasis: %s %u: the other player's Stasis ended: following (their vel %.2f %.2f %.2f)\n",
                o->name, o->hash, ro.vel[0], ro.vel[1], ro.vel[2]);
        }
        if (o->parked || o->free_until_ns || o->frozen_there) {
            // (handled above)
        } else if (o->owner == 2) {
            static const float zero[3] = {0, 0, 0};
            if (!theirs) {  // (they stopped sending it: it stays where it is, nobody's)
                o->owner = 0, o->have_put = false, o->changed_ns = now;
                memcpy(o->rest, pos, 12);
                LOG("[coop] objects: %s %u let go by the other player\n", o->name, o->hash);
            } else if (CLAIM_OK(o->have_put && settled && calm && dist3(pos, o->put) > claim_m, false)) {  // our player moved it
                o->owner = 1, o->gen = (uint16_t)(ro.gen + 1), o->have_put = false, o->moved_ns = o->changed_ns = now;
                LOG("[coop] objects: %s %u taken over here (moved %.2f m off; claim %u)\n", o->name, o->hash,
                    dist3(pos, o->put), o->gen);
            } else if (CLAIM_OK(o->matched && settled && calm && dist3(pos, o->rest) > move_m, true)) {
                // We had let go of it (it rested at o->rest, matching theirs) and OUR copy left that spot: our player
                // moved it. This must not depend on their velocity (2026-11-10: a barrel that still crept on the
                // owner's side, vel >= 0.05, sent us to the follow branch below, which silently put our player's
                // move back: the "taken over" check of test_fast_shrine failed about every second run).
                o->owner = 1, o->gen = (uint16_t)(ro.gen + 1), o->have_put = false, o->matched = false;
                o->moved_ns = o->changed_ns = now;
                LOG("[coop] objects: %s %u moved here: taken over (%.2f m off its resting spot; claim %u)\n", o->name,
                    o->hash, dist3(pos, o->rest), o->gen);
            } else if (o->matched && dist3(ro.vel, zero) < 0.05f) {
                // Resting on both sides and we let go of it: if it drifts off their spot now, our player is moving
                // it (magnesis pulls slowly: a few cm a frame, never a jump).
                if (CLAIM_OK(settled && calm && dist3(pos, ro.pos) > move_m, true)) {
                    o->owner = 1, o->gen = (uint16_t)(ro.gen + 1), o->have_put = false, o->matched = false;
                    o->moved_ns = o->changed_ns = now;
                    LOG("[coop] objects: %s %u moved here: taken over (%.2f m off their spot; claim %u)\n", o->name,
                        o->hash, dist3(pos, ro.pos), o->gen);
                }
            } else if (claim_hold) {
                // (HINGE: our resting copy left its spot; not put back while the claim waits for OBJ_HINGE_FRAMES)
            } else if (dist3(pos, ro.pos) < 0.03f && dist3(ro.vel, zero) < 0.05f) {
                // Already where they have it, and resting there: let go of it (holding a resting object wakes it
                // up, and in a stack the others then push it around).
                static const float maze_socket[3] = {-32.5f, -22.0f, 46.0f};
                if (!o->matched && obj_is_ball(o->name) && (!maze_ball || dist3(pos, maze_socket) < 2.0f)) {
                    // A ball put into a socket by our following does not count there: the socket's ActorObserverTag
                    // notices a ball that moves into its Area by physics, not one set in place (2026-11-08, Dungeon033:
                    // the copy at the owner's exact spot left the socket dark until lifted 0.6 m and dropped). So a
                    // ball we let go of drops the last 0.4 m by itself (calm period: the drop is not our player's).
                    float m[12], up[3] = {pos[0], pos[1] + 0.4f, pos[2]}, q[4];
                    memcpy(q, (void *)(uintptr_t)(o->body + 0x1d0), 16);
                    quat_mtx(q, up, m);
                    rb_set_transform(c, o->rb, m);
                    rb_set_linvel(c, o->rb, zero);
                    o->changed_ns = now;
                }
                if (!o->matched) memcpy(o->rest, pos, 12);  // (where our copy rests while we leave it alone)
                o->matched = true, o->have_put = false;
            } else {
                o->matched = false;
                if (LD32(o->rb + 0x68) & 0x80000) {  // fixed (resting objects are): it would ignore the move
                    Cpu saved = *c;
                    c->x[0] = o->rb; c->x[1] = 0; c->x[2] = 0;
                    guest_call(c, A_RB_SET_FIXED);
                    *c = saved;
                }
                float to[3], m[12];
                float lead = (float)(age < 0.1 ? age : 0.1);  // (a little ahead along its velocity)
                for (int k = 0; k < 3; k++) to[k] = ro.pos[k] + ro.vel[k] * lead;
                quat_mtx(ro.rot, to, m);
                rb_set_transform(c, o->rb, m);
                rb_set_linvel(c, o->rb, ro.vel);
                o->driven_ns = now;
                // (where it should be at our next look: it moves along that velocity until then)
                for (int k = 0; k < 3; k++) o->put[k] = to[k] + ro.vel[k] * (1.0f / 30.0f);
                o->have_put = true;
            }
        } else if (o->owner == 0) {
            if (CLAIM_OK(settled && calm && dist3(pos, o->rest) > move_m, false)) {  // moved here: ours
                o->owner = 1, o->gen = (uint16_t)(o->gen + 1), o->moved_ns = o->changed_ns = now;
                LOG("[coop] objects: %s %u moved here: ours (claim %u)\n", o->name, o->hash, o->gen);
            } else if (!settled) {
                memcpy(o->rest, pos, 12);
            }
        }
#undef CLAIM_OK
        o->claim_frames = claim_seen && o->owner != 1 ? (uint8_t)(o->claim_frames < 250 ? o->claim_frames + 1 : 250) : 0;
        double dt = (double)(now - o->last_ns) / 1e9;
        if (dt > 0.001 && dt < 0.5)
            for (int k = 0; k < 3; k++) o->vel[k] = (float)((pos[k] - o->last[k]) / dt);
        else
            memset(o->vel, 0, sizeof o->vel);
        static const float zero[3] = {0, 0, 0};
        if (dist3(o->vel, zero) > OBJ_VEL_MAX) memset(o->vel, 0, sizeof o->vel);
        memcpy(o->last, pos, 12), o->last_ns = now;
        if (o->owner == 1 && dist3(o->vel, zero) > 0.05f) o->moved_ns = now;
        // Three frames after our player let go of it: its release pose and velocity (a throw's velocity shows in
        // the samples only two frames after the hold ended - probe 2026-11-11).
        if (o->release_wait > 0 && --o->release_wait == 0 && o->owner == 1) {
            obj_send_ev(o, EV_OBJ_RELEASE, o->vel);
            o->moved_ns = now;
        }
    }
    if (brk_on()) brk_frame(c, now, me, &nr, overworld);
    // What we own: the most recently moved first, the rest in turns.
    int owned[OBJ_MAX], on_ = 0;
    for (int i = 0; i < obj_n; i++)
        if (objs[i].owner == 1) owned[on_++] = i;
    for (int a = 0; a < on_; a++)  // (few: insertion sort by moved_ns, newest first)
        for (int b = a; b > 0 && objs[owned[b]].moved_ns > objs[owned[b - 1]].moved_ns; b--) {
            int t = owned[b]; owned[b] = owned[b - 1]; owned[b - 1] = t;
        }
    int fresh = 0;
    while (fresh < on_ && now - objs[owned[fresh]].moved_ns < 2000000000ull && out_n < COOP_OBJ_MAX) {
        struct Obj *o = &objs[owned[fresh++]];
        CoopObject e = {.hash = o->hash, .gen = o->gen, .flags = (uint8_t)(COOP_OBJ_OWNED | (o->carried ? COOP_OBJ_CARRIED : 0) | (o->stasis ? COOP_OBJ_STASIS : 0))};
        memcpy(e.pos, (void *)(uintptr_t)(o->body + 0x1a0), 12);
        memcpy(e.rot, (void *)(uintptr_t)(o->body + 0x1d0), 16);
        memcpy(e.vel, o->vel, 12);
        out[out_n++] = e;
    }
    for (int k = 0; k < on_ - fresh && out_n < COOP_OBJ_MAX; k++) {
        struct Obj *o = &objs[owned[fresh + (rr + k) % (on_ - fresh)]];
        CoopObject e = {.hash = o->hash, .gen = o->gen, .flags = (uint8_t)(COOP_OBJ_OWNED | (o->carried ? COOP_OBJ_CARRIED : 0) | (o->stasis ? COOP_OBJ_STASIS : 0))};
        memcpy(e.pos, (void *)(uintptr_t)(o->body + 0x1a0), 12);
        memcpy(e.rot, (void *)(uintptr_t)(o->body + 0x1d0), 16);
        out[out_n++] = e;
    }
    if (on_ > fresh) rr = (rr + 1) % (on_ - fresh);
    coop_net_set_objects(out, out_n);
}

// ---- collision with the other player (2026-11-11: collision with the other player was wanted). The avatar
// has a body: the model-less actor Dm_CoopBody (build_overlay.py: Mannequin.bphysics as CoopLinkE.bphysics, one
// Keyframed capsule r 0.4 from the feet to 1.9 m, layer EntityObject). Each frame it is put on the avatar with the game's RigidBody setTransform and
// given the avatar's velocity (a keyframed body moving at a velocity pushes the player's controller smoothly), or
// parked 60 m under the avatar when it must not collide: SAVAGEWIND_COOP_COLLIDE=0, data older than 0.5 s, the avatar
// jumped more than 2 m in a frame (1 s off: a teleport, a respawn), the partner climbs / swims / glides / rides /
// lies dead (states with a body shape far from a standing capsule), and while it overlaps our Link (never switched on
// inside him: the controller would be shoved out, maybe through a wall).
#define COL_PARK_DROP 60.0f
#define COL_LIFT_M 0.25f
static struct {
    uint64_t actor, bactor, rb, body, look_ns;  // (actor: the avatar; bactor: its body actor Dm_CoopBody)
    bool on, parked, said;
    float last[3], rs_last[3];  // (the avatar's drawn position, and the last received one)
    bool rs_have;
    uint64_t last_ns, off_until_ns;
} col;
static struct { volatile uint64_t until_ns; volatile int nodrive; uint64_t next_ns; } coltrace_dev;
static bool ice_mine_near(const float p[3]);  // (runes.inc, n6)
static bool evcalm_held(void);  // (coop/evcalm.inc)
static bool collide_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_COLLIDE") && !strcmp(getenv("SAVAGEWIND_COOP_COLLIDE"), "0"));
    return on;
}
// States where the partner's body is not a standing capsule (or moves on things the local game does not have).
static bool collide_state_off(int anim) {
    if (anim < 0 || anim >= COOP_ANIM_COUNT) return false;
    static const char *const off[] = {"CoopClimb", "CoopSwim", "CoopGlide", "CoopHorse", "CoopLadder", "CoopWakeboard",
                                      "CoopMount", "CoopDismount", "CoopDive", "CoopWaterfall", "CoopDie",
                                      "CoopMasterDead", "CoopWallOff", "CoopGroundDownWall", "CoopCutHorse"};
    const char *n = coop_anim_as[anim];
    for (size_t i = 0; i < sizeof off / sizeof *off; i++)
        if (!strncmp(n, off[i], strlen(off[i]))) return true;
    return false;
}
static bool col_body_near(bool on, uint64_t actor, const float p[3], float r) {
    if (!on || !in_guest(actor)) return false;
    const float *am = (const float *)(uintptr_t)(actor + ACTOR_MTX);
    float a[3] = {am[3], am[7] + 0.9f, am[11]};
    return dist3(p, a) < r;
}
static bool multi_col_near(const float p[3], float r);  // (n4, multi.inc: the extra players' bodies)
static bool col_near_avatar(const float p[3], float r) {  // (objects_frame: is an avatar's body right there?)
    return col_body_near(col.on, col.actor, p, r) || multi_col_near(p, r);
}
// The body is its own model-less actor Dm_CoopBody (build_overlay.py), created with the avatar: an actor's model follows
// its physics body, so on the avatar itself (protocol 30 as first built) the drawn body faced +z (no rotation given)
// and went 60 m down with the parked body, apart from its armor puppets and with the gear on its bones (2026-11-11).
#define BODY_ACTOR "Dm_CoopBody"
static struct Pup body_pup;
static void col_pup_drop(Cpu *c, struct Pup *p) {
    // Deleted like the avatar (avatar_frame): not during a stage load, which deletes it itself; else it would
    // stay behind as an invisible wall (dead, menus: local_in_world() is false but no load is coming).
    char area[96];
    bool loading = avatar.area != local_area(area, sizeof area);
    if (!loading && pup_alive(p)) despawn_now(c, p->actor, ACTOR_DELETE_REASON);
    p->actor = 0, p->unit = 0, p->id = 0, p->frames = 0, p->name[0] = 0;
}
static void collide_frame(Cpu *c) {
    uint64_t now = plat_now_ns();
    if (!avatar.actor || !avatar_alive() || avatar.frames < 3 || !local_in_world() || !collide_on()) {
        col.actor = 0, col.on = false;
        if (body_pup.actor) col_pup_drop(c, &body_pup);
        return;
    }
    if (col.actor != avatar.actor) {
        memset(&col, 0, sizeof col);
        col.actor = avatar.actor;
    }
    static int body_pack = -1;  // (the overlay has the body actor: looked up once)
    if (body_pack < 0) body_pack = pack_exists(BODY_ACTOR);
    pup_spawn_drop = COL_PARK_DROP;
    bool have_body = pup_need(c, &body_pup, body_pack ? BODY_ACTOR : "");
    pup_spawn_drop = 0;
    if (have_body && col.bactor != body_pup.actor) col.parked = false, col.on = false;  // (new: placed below)
    if (!have_body) {
        if (!col.said && !body_pack)
            col.said = true, LOG("[coop] collide: no %s in the overlay (re-run the setup): no collision\n", BODY_ACTOR);
        col.rb = 0, col.on = false;
        return;
    }
    if (col.rb && col.bactor != body_pup.actor) col.rb = 0;  // (a new body actor)
    if (!col.rb && now >= col.look_ns) {
        col.look_ns = now + 1000000000ull;
        col.bactor = body_pup.actor;
        objects_fast_only = true;
        uint64_t pi = LD64(col.bactor + 0x190), body = in_guest(pi) ? LD64(pi + 0x78) : 0;
        if (in_guest(body) && LD64(body) == RIGID_BODY_VT) {
            uint64_t rb = LD64(body + 0x18);
            if (in_guest(rb) && LD64(rb + 0x78) == body) col.rb = rb, col.body = body;
        }
        objects_fast_only = false;
        if (col.rb)
            LOG("[coop] collide: avatar body %llx (actor %llx rb %llx, flags %08x)\n", (unsigned long long)col.body,
                (unsigned long long)col.bactor, (unsigned long long)col.rb, LD32(col.rb + 0x68));
        else if (!col.said)
            col.said = true, LOG("[coop] collide: %s has no body yet\n", BODY_ACTOR);
    }
    if (!col.rb || LD64(col.rb + 0x78) != col.body) { col.rb = 0; return; }
    const float *am = (const float *)(uintptr_t)(col.actor + ACTOR_MTX);
    float pos[3] = {am[3], am[7], am[11]}, me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    const float *bp = (const float *)(uintptr_t)(col.body + 0x1a0);
    if (coltrace_dev.until_ns && now < coltrace_dev.until_ns && (now >= coltrace_dev.next_ns || dist3(pos, me) < 1.5f)) {
        coltrace_dev.next_ns = now + 200000000ull;  // (every frame while we are close)
        LOG("[coop] coltrace: avatar %.2f %.2f %.2f body %.2f %.2f %.2f flags %08x on %d me %.2f %.2f %.2f (%.2f m)\n",
            pos[0], pos[1], pos[2], bp[0], bp[1], bp[2], LD32(col.rb + 0x68), col.on, me[0], me[1], me[2],
            dist3(pos, me));
    }
    if (coltrace_dev.nodrive) return;
    CoopState rs;
    double age = 99;
    bool have = coop_net_get_remote(&rs, &age);
    // A teleport, a respawn: off for a second. Judged on the RECEIVED positions (20 a second; running moves 0.3 m
    // between two): the drawn avatar is smoothed toward them and glided 3 m into our Link in a few frames, shoving
    // him 1.4 m (test_collide 2026-11-11).
    float step = col.last_ns ? dist3(pos, col.last) : 0;
    if (have && col.rs_have && dist3(rs.pos, col.rs_last) > 2.0f) step = 99;
    if (have) memcpy(col.rs_last, rs.pos, 12), col.rs_have = true;
    if (step > 2.0f) col.off_until_ns = now + 1000000000ull;
    const char *why = !collide_on() ? "switched off" : !have || age > 0.5 ? "stale" : collide_state_off(rs.anim) ? "state"
                    : now < col.off_until_ns ? "jump" : NULL;
    float dx = me[0] - pos[0], dz = me[2] - pos[2], dyy = me[1] - pos[1];
    bool overlap = sqrtf(dx * dx + dz * dz) < 0.85f && dyy > -1.9f && dyy < 1.9f;
    if (!why && !col.on && overlap) why = "overlaps us";
    // (n6, ICESAFE) Off on / beside one of our Cryonis blocks: the body touching it broke our real block when the
    // other player stood on its copy (probe 2026-11-17: broke at once; with SAVAGEWIND_COOP_COLLIDE=0 it held).
    if (!why && ice_mine_near(pos)) why = "on our ice block";
    // (2026-11-18, DEMOCALM: coop/evcalm.inc) an event walks our Link along its marks: nobody pushes or is pushed
    if (!why && evcalm_held()) why = "an event holds our Link";
    if (!why && (snd_mute_dev & 1)) why = "dev mute";
    double dt = col.last_ns && now > col.last_ns ? (double)(now - col.last_ns) / 1e9 : 0;
    if (!why) {
        float vel[3] = {0, 0, 0}, m[12];
        if (dt > 0.001 && dt < 0.2)
            for (int k = 0; k < 3; k++) vel[k] = (float)((pos[k] - col.last[k]) / dt);
        float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
        if (sp > 12.0f) for (int k = 0; k < 3; k++) vel[k] *= 12.0f / sp;
        // The body gets the avatar's own matrix, rotation included (no model follows it any more, see above; when it
        // was the avatar's own body, an identity rotation here turned the drawn avatar to face +z).
        memcpy(m, am, sizeof m);
        // (2026-11-18, user: the partner's walk "sounds like a rock or something moving". Source log, test_partnersound:
        // one sample started again and again only while this body was on and the partner walked - the capsule (from the
        // feet down to y 0, a metal-like physics material) scraping the ground.) The body rides COL_LIFT_M higher: its
        // round bottom clears the ground, Link still bumps into it. SAVAGEWIND_COOP_BODYLIFT=0: off (on the ground).
        static int lift = -1;
        if (lift < 0) lift = look_on("SAVAGEWIND_COOP_BODYLIFT"), LOG("[coop] collide: body lifted off the ground: %s\n", lift ? "on" : "off");
        if (lift) m[7] += COL_LIFT_M;
        if (LD32(col.rb + 0x68) & 0x80000) rb_fix(c, col.rb, false);
        rb_set_transform(c, col.rb, m);
        rb_set_linvel(c, col.rb, vel);
        if (!col.on) LOG("[coop] collide: on at %.1f %.1f %.1f\n", pos[0], pos[1], pos[2]);
        col.on = true, col.parked = false;
    } else if (col.on || !col.parked || dist3(bp, pos) < COL_PARK_DROP - 5) {
        static const float zero[3] = {0, 0, 0};
        float m[12];
        memcpy(m, am, sizeof m);  // (same rotation as the avatar: see above)
        m[7] -= COL_PARK_DROP;
        rb_set_transform(c, col.rb, m);
        rb_set_linvel(c, col.rb, zero);
        if (col.on) LOG("[coop] collide: off (%s)\n", why);
        col.on = false, col.parked = true;
    }
    memcpy(col.last, pos, 12), col.last_ns = now;
}

// ---- the partner's horse is solid (2026-11-14: add collision between the horses). Like the avatar's
// body above: the model-less actor Dm_CoopHorseBody (build_overlay.py: one Keyframed capsule r 0.55 lying along the
// horse at 1.25 m, layer EntityObject) is put on the horse puppet every frame with its velocity, so our horse and our
// Link bump into it; parked 60 m down while it must not collide (data older than 0.5 s, the puppet jumped > 3 m in a
// frame, or it would switch on overlapping our Link or our horse) and deleted when there is no partner horse.
// SAVAGEWIND_COOP_HORSECOLLIDE=0: off.
#define HORSE_BODY_ACTOR "Dm_CoopHorseBody"
static struct Pup hbody_pup;
static struct { uint64_t bactor, rb, body, look_ns, last_ns, off_until_ns; bool on, parked, said; float last[3]; } hcol;
// Horizontal distance from p to the partner horse's capsule axis (chest z 1.1 to rump z -0.8 in the horse's frame).
static float hcol_axis_dist(const float m[12], const float p[3]) {
    float a[3], b[3], ab[3], ap[3];
    for (int i = 0; i < 3; i++) a[i] = m[i * 4 + 3] + m[i * 4 + 2] * 1.1f, b[i] = m[i * 4 + 3] - m[i * 4 + 2] * 0.8f;
    for (int i = 0; i < 3; i++) ab[i] = b[i] - a[i], ap[i] = p[i] - a[i];
    float l2 = ab[0] * ab[0] + ab[2] * ab[2], t = l2 > 1e-4f ? (ap[0] * ab[0] + ap[2] * ab[2]) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = ap[0] - ab[0] * t, dz = ap[2] - ab[2] * t;
    return sqrtf(dx * dx + dz * dz);
}
static void horse_body_frame(Cpu *c) {
    uint64_t now = plat_now_ns();
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_HORSECOLLIDE");
    // (coop-horse2: only a horse; a seal, Sidon, the Master Cycle or a wild deer / bear puppet stays without a body)
    uint64_t horse = hr.have && hr.kind == MOUNT_HORSE && horse_target_alive(&hr.p[0], hr.targets[0]) ? hr.targets[0] : 0;
    if (!on || !horse || !avatar.actor || !avatar_alive() || !local_in_world()) {
        if (hbody_pup.actor) {
            col_pup_drop(c, &hbody_pup);
            LOG("[coop] horse collide: body deleted\n");
        }
        memset(&hcol, 0, sizeof hcol);
        return;
    }
    static int pack = -1;
    if (pack < 0) pack = pack_exists(HORSE_BODY_ACTOR);
    pup_spawn_drop = COL_PARK_DROP;
    bool have = pup_need(c, &hbody_pup, pack ? HORSE_BODY_ACTOR : "");
    pup_spawn_drop = 0;
    if (!have) {
        if (!hcol.said && !pack) hcol.said = true, LOG("[coop] horse collide: no %s in the overlay\n", HORSE_BODY_ACTOR);
        hcol.rb = 0;
        return;
    }
    if (hcol.rb && hcol.bactor != hbody_pup.actor) hcol.rb = 0, hcol.on = hcol.parked = false;
    if (!hcol.rb && now >= hcol.look_ns) {
        hcol.look_ns = now + 1000000000ull;
        hcol.bactor = hbody_pup.actor;
        objects_fast_only = true;
        uint64_t pi = LD64(hcol.bactor + 0x190), body = in_guest(pi) ? LD64(pi + 0x78) : 0;
        if (in_guest(body) && LD64(body) == RIGID_BODY_VT) {
            uint64_t rb = LD64(body + 0x18);
            if (in_guest(rb) && LD64(rb + 0x78) == body) hcol.rb = rb, hcol.body = body;
        }
        objects_fast_only = false;
        if (hcol.rb) LOG("[coop] horse collide: body %llx (rb %llx)\n", (unsigned long long)hcol.body, (unsigned long long)hcol.rb);
    }
    if (!hcol.rb || LD64(hcol.rb + 0x78) != hcol.body) { hcol.rb = 0; return; }
    float m[12];
    memcpy(m, (const void *)(uintptr_t)(horse + ACTOR_MTX), sizeof m);
    float pos[3] = {m[3], m[7], m[11]}, me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    double age = 99;
    CoopState rs;
    bool fresh = coop_net_get_remote(&rs, &age) && age <= 0.5;
    float step = hcol.last_ns ? dist3(pos, hcol.last) : 0;
    if (step > 3.0f) hcol.off_until_ns = now + 1000000000ull;
    const char *why = !fresh ? "stale" : now < hcol.off_until_ns ? "jump" : NULL;
    if (!why && !hcol.on) {
        float dy = me[1] - pos[1];
        if (hcol_axis_dist(m, me) < 1.1f && dy > -1.5f && dy < 2.5f) why = "overlaps us";
        uint64_t ours = hs.part[0].actor;
        if (!why && ours && in_guest(ours)) {
            const float *om = (const float *)(uintptr_t)(ours + ACTOR_MTX);
            float op[3] = {om[3], om[7], om[11]};
            if (hcol_axis_dist(m, op) < 1.6f && fabsf(op[1] - pos[1]) < 2.0f) why = "overlaps our horse";
        }
    }
    double dt = hcol.last_ns && now > hcol.last_ns ? (double)(now - hcol.last_ns) / 1e9 : 0;
    if (!why) {
        float vel[3] = {0, 0, 0};
        if (dt > 0.001 && dt < 0.2)
            for (int k = 0; k < 3; k++) vel[k] = (float)((pos[k] - hcol.last[k]) / dt);
        float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
        if (sp > 25.0f) for (int k = 0; k < 3; k++) vel[k] *= 25.0f / sp;
        if (LD32(hcol.rb + 0x68) & 0x80000) rb_fix(c, hcol.rb, false);
        rb_set_transform(c, hcol.rb, m);
        rb_set_linvel(c, hcol.rb, vel);
        if (!hcol.on) LOG("[coop] horse collide: on at %.1f %.1f %.1f\n", pos[0], pos[1], pos[2]);
        hcol.on = true, hcol.parked = false;
    } else if (hcol.on || !hcol.parked) {
        static const float zero[3] = {0, 0, 0};
        m[7] -= COL_PARK_DROP;
        rb_set_transform(c, hcol.rb, m);
        rb_set_linvel(c, hcol.rb, zero);
        if (hcol.on) LOG("[coop] horse collide: off (%s)\n", why);
        hcol.on = false, hcol.parked = true;
    }
    memcpy(hcol.last, pos, 12), hcol.last_ns = now;
}
