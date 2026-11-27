// Part of runtime/coop.c (included there, same translation unit): shared enemies: actor list, one health bar, run-time enemies, thaw, hit reaction.
// ---- shared world, Phase 0 list the live actors with a stable key.
// Every process (actor or not) has a pooled proc unit (actor+0x68) in one ring list: the node is at unit+0x10
// (prev, next), the unit holds the id at +0x40 and the live process at +0x48 (see avatar_alive). Walking the ring
// from the player's own unit visits every live process. Only actors are listed (the game's own type test:
// proc->vtable[0x10](proc, FUN_710056b6d0()), as FUN_7101173368 does). A placed actor's map object is at +0x7c8;
// FUN_710128d5b8(mapobj + 0x28, &u32, &SafeString "HashId") reads its HashId from the map data, which is the same
// on every copy of the game: the shared key.
#define A_ACTOR_TYPE 0x700056b6d0ull   // returns the Actor type info for the isA test
#define A_BYML_GET_U32 0x700128d5b8ull  // bool f(byml iterator *, u32 *out, SafeString *key)
#define A_STR_HASHID 0x7001626a71ull    // "HashId"
#define A_SAFESTRING_VT 0x7001bc32c8ull // the SafeString vtable FUN_7100d4e648 uses for its key
#define UNIT_NODE 0x10
#define ACTOR_MAPOBJ 0x7c8
static struct { volatile int pending; char filter[64]; float radius; } actlist;
// `coop call <fn> [x0 [x1 [x2 [x3]]]]`: dev test, runs one game function on the game thread and logs x0.
static struct { volatile int pending; uint64_t fn, x[4]; } devcall;
static uint64_t actlist_mem;  // guest scratch: out u32 at +0, SafeString key at +0x10 (reset on reboot)

// A placed actor's map HashId (0: none, e.g. spawned at run time).
static uint32_t actor_hash(Cpu *c, uint64_t a) {
    uint64_t mo = LD64(a + ACTOR_MAPOBJ);
    if (!in_guest(mo)) return 0;
    if (!actlist_mem) actlist_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
    uint64_t out = actlist_mem, key = actlist_mem + 0x10;
    ST64(key, A_SAFESTRING_VT); ST64(key + 8, A_STR_HASHID);
    ST32(out, 0);
    Cpu saved = *c;
    c->x[0] = mo + 0x28; c->x[1] = out; c->x[2] = key;
    guest_call(c, A_BYML_GET_U32);
    uint32_t hash = (c->x[0] & 1) ? LD32(out) : 0;
    *c = saved;
    return hash;
}

static float actor_dist(uint64_t a, const float me[3], float p[3]) {
    p[0] = *(float *)(uintptr_t)(a + ACTOR_MTX + 0x0c), p[1] = *(float *)(uintptr_t)(a + ACTOR_MTX + 0x1c);
    p[2] = *(float *)(uintptr_t)(a + ACTOR_MTX + 0x2c);
    float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Call fn for every live actor (game thread). want(name) filters by name before the type test (cheap). Returns the
// number of live processes seen, or -1 without a player.
typedef void (*ActorFn)(Cpu *c, uint64_t actor, const char *name, void *ctx);
static int walk_actors(Cpu *c, bool (*want)(const char *name), ActorFn fn, void *ctx) {
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t start = in_guest(pa) ? LD64(pa + PROC_UNIT) : 0;
    if (!in_guest(start)) return -1;
    Cpu saved = *c;
    guest_call(c, A_ACTOR_TYPE);
    uint64_t actor_type = c->x[0];
    *c = saved;
    int seen = 0;
    uint64_t u = start, unit_vt = LD64(start);
    for (unsigned step = 0; step < 16384; step++) {
        uint64_t next = LD64(u + UNIT_NODE + 8);
        // The ring also passes through the manager's list head, which is no unit: its "actor" field is garbage (a
        // call through it crashed both test games, 2026-11-07). Units all share the first word (vtable).
        uint64_t a = LD64(u) == unit_vt ? LD64(u + UNIT_ACTOR) : 0;
        if (in_guest(a) && LD32(u + UNIT_ID) == LD32(a + PROC_ID) && LD8(a + PROC_STATE) != 3) {
            seen++;
            char name[64];
            snprintf(name, sizeof name, "%.63s", (const char *)(uintptr_t)(a + 0x1c));
            uint64_t vt = LD64(a), isa = vt >= 0x7000000000ull && vt < 0x7100000000ull ? LD64(vt + 0x10) : 0;
            if ((!want || want(name)) && isa >= 0x7000000000ull && isa < 0x7003000000ull) {  // (main module code)
                c->x[0] = a; c->x[1] = actor_type;
                guest_call(c, isa);
                bool is_actor = c->x[0] & 1;
                *c = saved;
                if (is_actor) fn(c, a, name, ctx);
            }
        }
        if (!in_guest(next)) break;
        u = next - UNIT_NODE;
        if (u == start) break;
    }
    return seen;
}

static void list_one(Cpu *c, uint64_t a, const char *name, void *ctx) {
    unsigned *listed = ctx;
    float p[3], d = actor_dist(a, (const float *)(uintptr_t)(player_info_ptr() + 0x88), p);
    if (d > actlist.radius || (actlist.filter[0] && !strstr(name, actlist.filter))) return;
    LOG("[coop] actor %-32s hash %10u  at %8.1f %7.1f %8.1f  %5.1f m  (%llx)\n", name, actor_hash(c, a), p[0], p[1],
        p[2], d, (unsigned long long)a);
    (*listed)++;
}

static void list_actors(Cpu *c) {
    unsigned listed = 0;
    int seen = walk_actors(c, NULL, list_one, &listed);
    if (seen < 0) { LOG("[coop] actors: no player\n"); return; }
    LOG("[coop] actors: %u listed (%d live processes; filter \"%s\", within %.0f m)\n", listed, seen, actlist.filter,
        actlist.radius);
}

// ---- shared world, Phase 1: one enemy, one health bar.
// Enemies are matched by their map HashId. Each side keeps the running total of damage its own player dealt to each
// enemy (life drops we did not cause) and sends it; both set the life to start - own - peer's total. Writing the life
// is enough for the game to run the real death (seen: the plateau Bokoblin died and dropped its horn). Life: s32 at
// actor+0x848 (the enemy vtable's slot 0x2b8 returns that address).
// Position (Phase 2): the host runs the real enemy and sends where it is; the guest switches its copy's AI off with
// the game's own skip-AI bit (byte actor+0x518, bit 0x40: the actor job 0x700151f9d4 then does not call the AI root
// actor+0x558) and moves it there every frame: physics body position and rotation (*(*(actor+0x198)+0x920) + 0x1a0,
// + 0x1d0/0x1e0; the game derives the matrices). No fresh host position (the host does not have it loaded): own AI.
#define ENEMY_LIFE 0x848
#define ENEMY_FLAGS 0x518
#define ENEMY_SKIP_AI 0x40u
#define ENEMY_FOLLOW_MAX_AGE 1.0  // s: older host data and the guest's copy runs its own AI again
#define ENEMY_SCAN_NS 2000000000ull
#define ENEMY_RADIUS 120.0f
// Enemies remembered (totals of damage, positions). It used to be COOP_ENEMY_MAX (16) and never forgot one: after
// 16 different enemies in a session, no new enemy was shared at all. Now: 256, the packet carries the 16 most
// useful (enemies_frame), and a full table forgets the longest-unseen untouched enemy.
#define ENEMY_TABLE 256
#define FOLLOW_MAX 32
static volatile uint64_t follow_actor[FOLLOW_MAX];  // guest: copies following the host (read by job threads)
// (ENEMYLOOK, for enemy_look_job on the job threads; rebuilt with follow_actor each frame) the following copies whose
// look-at we solve: actor, its vtable and look-at object as recorded when it began to follow, whether a request is on.
static volatile uint64_t look_pub_actor[FOLLOW_MAX], look_pub_vt[FOLLOW_MAX], look_pub_obj[FOLLOW_MAX];
static volatile uint8_t look_pub_req[FOLLOW_MAX];
static struct Enemy {
    uint32_t hash;
    uint64_t actor, unit;  // 0: not loaded here now (dead, unloaded, or not seen yet since a load)
    uint32_t unit_id;
    uint32_t elem_native, elem_remote, elem_shown, elem_unit_id;
    uint32_t elem_send, elem_recent;  // (2026-11-20) flags sent = native + any started in the last ELEMFOE_HOLD_NS
    uint64_t elem_recent_ns;
    uint64_t elem_status;
    bool ice_held;
    float ice_frame;
    int32_t base;          // its full life (the highest seen)
    int32_t last;          // the life we last saw or wrote
    uint32_t own;          // damage our player dealt (running total, sent to the peer)
    uint32_t applied_remote;  // the peer's total last applied (for the log)
    uint64_t inject_ns;       // owner: when the peer's last damage was injected as a hit (0: none pending)
    bool mine;                // this side runs its AI now (the other side's copy follows ours)
    bool guest_runs;          // host's decision: the guest's player is nearer, the guest runs it
    uint64_t owner_ns;        // host: when guest_runs last changed (hysteresis)
    int8_t owner_pid;         // (58, host) which guest runs it while guest_runs (the partner with two players)
    int8_t cand_pid;          // (58, host) the guest the distance rule weighs against us (nearest world guest)
    uint64_t cand_ns;         // (58, host) when cand_pid / owner_pid last changed
    uint64_t no_handoff_until;  // host: a hand-over to the guest failed (no position came back): not before this
    bool aggro_handoff;         // host: the guest got it because its player hit it (aggro rule)
    bool following;        // guest: AI off, moved to the host's position
    float shown[3];        // guest: where we put it last frame (smoothed toward the host's position)
    char playing[32];      // guest: the host's AS we last told the copy to play
    char own_as[32];       // (2026-11-14, wake) the AS our own AI had chosen when the copy started following
    uint64_t wake_ns;      // (wake) when this side took it over with a replayed hit (0: no wake check pending)
    int wake_tries;        // (wake) extra hits injected because it still slept after the take-over
    bool wake_battle;      // (2026-11-14, softlock) taken over mid-fight: an idle AS counts as "asleep" in the check
    uint64_t follow_ns;    // (softlock) when this copy started following (own_as is only fresh just after that)
    uint64_t fight_ns;     // (softlock, host) when the side running it last showed a battle AS
    bool tgt_partner;      // (2026-11-14, target) we run it and point it at the other player's avatar
    uint64_t tgt_av;       // (n4) which remote avatar (the partner's or, MULTITARGET, an extra player's)
    int tgt_pid;           // (n4) and that player's id
    uint64_t tgt_ns, tgt_log_ns;
    uint64_t tgt_set_ns;   // (softlock) when we last wrote the avatar as its target
    uint64_t octo_wake_ns; // (Octoroks) when we last gave it our Link as target to keep it awake
    uint64_t octo_atk_ns;  // (Octoroks) when its last attack got the avatar as target
    unsigned tgt_overrides;  // (softlock) times the AI had put its own target back since the last log line
    float tgt_bias, tgt_score;  // (2026-11-15, threat) random lean toward the partner (m) / last score (partner - ours)
    uint64_t tgt_bias_until;    // (threat) when the lean is rolled again
    uint64_t tgt_wait_ns;       // (SWITCHHOLD) when a change of target first waited for the attack to end (0: none)
    uint64_t tgt_battle_ns, tgt_as_ns;  // (threat) when it last showed a battle AS here / when that was last read
    float rot[4];          // guest: the host's body rotation (quaternion x y z w) last written
    uint64_t body;         // guest: the copy's physics body found by enemy_follow this frame (used by enemy_reapply)
    uint64_t seen_ns;      // last frame it was loaded here (eviction: the table forgets the longest-unseen first)
    uint64_t hurt_ns;      // when our player last hurt it (sent first for a while: see enemies_frame)
    uint64_t peer_hit_ns;  // (2026-11-14, aggro) when the other player's damage total last rose
    uint64_t swap_ns;      // (0.1.1, FIGHTSWAP, host) since when the other player is clearly the nearer one in a fight
    uint32_t own_base, remote_base;  // (0.1.1, REVIVE) both damage totals when it last came back to life (Stalkoblin)
    bool remote_known;     // the peer's total was read once since it was attached (a first read is no new hit)
    bool animal;           // (2026-11-16, ANIMALS) a wild animal shared like an enemy (no targets, no wake kicks)
    bool spawned;          // created by the game at run time (no HashId): hash is a co-op id (COOP_ENEMY_SPAWNED)
    bool native;           // spawned: our game created it (we run it); else it is our copy of the peer's
    bool dead;             // spawned copy: it died here (never created again)
    uint64_t retry_ns;     // spawned copy: no new creation attempt before this
    uint64_t born_ns;      // spawned copy: when we created it (its life reads 1 until the game initialises it)
    bool look_req;         // (ENEMYLOOK) follower: the owner's enemy looks at a player (request kept on)
    uint64_t look_who;     // (ENEMYLOOK) follower: whom the copy's look-at points at now (for the log)
    uint64_t look_vt, look_o;  // (ENEMYLOOK) follower: its actor's vtable and look-at object when it began to follow
    bool mtx_follow;       // (MTXPOS) follower: no body found, the copy is moved by its actor matrices
    float mtx_off[3];      // (MTXPOS) follower with a body: its body position minus its matrix position
    char name[48];
} enemies[ENEMY_TABLE];
static int enemy_count;
static volatile int enemy_report;  // `coop enemies`: log the table on the game thread
static uint64_t enemy_scan_ns;
static uint32_t enemy_area;

// (2026-11-16: sync the objects and things in the overworld, little animals and such)
// Placed wild animals (map HashId) are shared like enemies: the nearer player's game runs the animal, the other
// game's copy follows its body and AS, and the kill is shared (life totals). Not shared: animals the game spawns at
// run time (each game makes its own), birds, fish, insects and horses (horse.inc). SAVAGEWIND_COOP_ANIMALS=0: off.
static bool animals_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_ANIMALS");
    return on;
}
static bool animal_name(const char *n) {
    static const char *const kinds[] = {"Boar", "Deer", "Doe", "Elk", "WildGoat", "Fox", "Wolf", "Bear", "Rhino",
                                        "Bull", "Squirrel", "RupeeRabbit", "Kokko", "Sunazarashi", "Goat", "Sheep",
                                        "Cow", "Dog", "Cat", "Bear", "Hedgehog", "Rabbit",
                                        "Cassowary"};  // (2026-11-19 fauna: in AreaData's Animal list)
    if (strncmp(n, "Animal_", 7)) return false;
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (!strncmp(n + 7, kinds[i], strlen(kinds[i])) && n[7 + strlen(kinds[i])] == '_') return true;
    return false;
}
static bool enemy_name(const char *name) {
    return !strncmp(name, "Enemy_", 6) || (animals_on() && animal_name(name));
}

// The table entry for that id (created if new; -1 when the table is full of enemies still needed).
static int enemy_slot(uint32_t hash, const char *name) {
    int k = 0;
    while (k < enemy_count && enemies[k].hash != hash) k++;
    if (k < enemy_count) return k;
    if (enemy_count == ENEMY_TABLE) {  // full: forget one not loaded now: gone run-time enemies, untouched ones, oldest
        int best = -1;
        uint64_t now = plat_now_ns();
        for (int pass = 0; pass < 3 && best < 0; pass++)
            for (int i = 0; i < enemy_count; i++) {
                const struct Enemy *v = &enemies[i];
                if (v->actor) continue;
                if (pass == 0 && !(v->spawned && now - v->seen_ns > 30000000000ull)) continue;
                if (pass == 1 && (v->own || coop_net_remote_damage(v->hash))) continue;
                if (best < 0 || v->seen_ns < enemies[best].seen_ns) best = i;
            }
        if (best < 0) return -1;
        k = best;
    } else {
        enemy_count++;
    }
    memset(&enemies[k], 0, sizeof enemies[k]);
    enemies[k].hash = hash;
    enemies[k].seen_ns = plat_now_ns();
    snprintf(enemies[k].name, sizeof enemies[k].name, "%s", name);
    enemies[k].animal = !strncmp(name, "Animal_", 7);
    return k;
}

// ---- enemies the game created at run time (2026-11-08, protocol 12): no map HashId, so the side whose game made
// one ("native") gives it a co-op id and always runs it; the other side creates a copy by name where it is, and that
// copy follows it like any shared enemy (AI off, position/AS from the packet, damage totals both ways, shared
// death). The copy goes away when the native side stops reporting it loaded. Enemies both games spawn on their own
// (random spawns) are therefore shown twice: each side's own plus the other's copy.
// SAVAGEWIND_COOP_SPAWNED=0 turns this off.
static bool spawned_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_SPAWNED") && !strcmp(getenv("SAVAGEWIND_COOP_SPAWNED"), "0"));
    return on;
}
// (2026-11-16: sync the enemies and things that appear randomly too) One shared set of random spawns: both
// games spawn their own random enemies/animals near their players. When the host reports a run-time enemy and the
// guest's game made one of the same kind near it (SPAWN_DEDUP_R), untouched by both players, the guest deletes its own
// and shows the copy of the host's instead (spawned_frame); the host waits SPAWN_DEDUP_GRACE before copying a guest
// spawn that has a host twin, so the guest has time to remove it. Run-time enemies the host runs may also be steered
// at the guest's avatar (enemy_target_frame), as placed ones. SAVAGEWIND_COOP_SPAWNDEDUP=0: off (each game's random
// spawns shown twice again, as before). Run-time wild animals (animal_name kinds; not birds, fish, insects) are
// shared like run-time enemies: SAVAGEWIND_COOP_SPAWNANIMALS=0 off (also off with ANIMALS=0).
static bool spawndedup_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_SPAWNDEDUP");
    return on;
}
static bool spawnanimals_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_SPAWNANIMALS");
    return on && animals_on();
}
static uint32_t spawn_counter;
static void spawned_seen(uint64_t a, const char *name, int32_t life) {
    if (!spawned_on()) return;
    if (!strncmp(name, "Animal_", 7) && !(spawnanimals_on() && animal_name(name))) return;  // (each game's own)
    for (int i = 0; i < enemy_count; i++)
        if (enemies[i].actor == a) return;  // (known: one of ours, or a copy we made)
    uint32_t id = 0x80000000u | coop_net_id_tag() | (++spawn_counter & 0x03ffffffu);
    int k = enemy_slot(id, name);
    if (k < 0) return;
    struct Enemy *e = &enemies[k];
    e->spawned = e->native = e->mine = true;
    e->actor = a, e->unit = LD64(a + PROC_UNIT), e->unit_id = LD32(a + PROC_ID);
    e->base = e->last = life;
    LOG("[coop] run-time enemy %s attached as %08x: life %d\n", e->name, id, life);
}

static void enemy_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)ctx;
    float p[3];
    if (actor_dist(a, (const float *)(uintptr_t)(player_info_ptr() + 0x88), p) > castle_enemy_radius(name, ENEMY_RADIUS)) return;
    int32_t life = (int32_t)LD32(a + ENEMY_LIFE);
    if (life <= 0 || life > 100000) return;
    uint32_t hash = actor_hash(c, a);
    if (!hash) { spawned_seen(a, name, life); return; }
    int k = enemy_slot(hash, name);
    if (k < 0) return;
    struct Enemy *e = &enemies[k];
    if (e->actor == a) return;
    // (Re)attached: a new copy of this enemy (first sight, or loaded again). Its life now is not damage.
    e->actor = a, e->unit = LD64(a + PROC_UNIT), e->unit_id = LD32(a + PROC_ID);
    e->following = false;
    e->seen_ns = e->born_ns = plat_now_ns();  // (born_ns: the AI warm-up in enemy_follow)
    if (life > e->base) e->base = life;
    e->last = life;
    // (0.1.1, REVIVE) A Stal body that fell apart is a NEW actor when it gets up: if the damage counted since its last
    // life already used that life up, this copy starts a new one (else the old totals killed it at once, test run 4).
    // Stal only: a normal enemy the partner killed must still load dead.
    static int revive_c = -1;
    uint32_t remote = coop_net_remote_damage(e->hash);
    uint64_t spent = (uint64_t)(e->own - e->own_base) + (remote >= e->remote_base ? remote - e->remote_base : 0);
    if (e->base > 0 && spent >= (uint64_t)e->base && strstr(e->name, "_Bone") && !strstr(e->name, "_Head") &&
        env_on("SAVAGEWIND_COOP_REVIVE", &revive_c)) {
        e->own_base = e->own, e->remote_base = remote;
        LOG("[coop] enemy %s %u got up again as a new copy: a new life\n", e->name, e->hash);
    }
    LOG("[coop] enemy %s %u attached: life %d/%d, own damage %u\n", e->name, e->hash, life, e->base, e->own);
}

static bool enemy_alive(const struct Enemy *e) {
    if (!in_guest(e->actor) || !in_guest(e->unit)) return false;
    return LD32(e->unit + UNIT_ID) == e->unit_id && LD64(e->unit + UNIT_ACTOR) == e->actor &&
           LD8(e->actor + PROC_STATE) != 3;
}

// The enemy's main physics body. Bokoblin: *(*(actor+0x198)+0x920); that chain is wrong for a Moblin (+0x920 there
// holds a float). *(*(*(actor+0x578)+0x80)+0x410) reached the body on both (2026-11-08, verified by memory scan +
// pointer path). Accept a candidate only if it is a rigid body (vtable 0x7001b87b20, same on both) within 5 m of the
// actor's matrix position.
#define RIGID_BODY_VT 0x7001b87b20ull
static bool body_ok(uint64_t a, uint64_t b) {
    if (!in_guest(b) || LD64(b) != RIGID_BODY_VT) return false;
    const float *bp = (const float *)(uintptr_t)(b + 0x1a0);
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    float dx = bp[0] - m[3], dy = bp[1] - m[7], dz = bp[2] - m[11];
    return dx * dx + dy * dy + dz * dz < 25.0f;
}
static uint64_t enemy_body(uint64_t a) {
    uint64_t p = LD64(a + 0x578), q = in_guest(p) ? LD64(p + 0x80) : 0, b = in_guest(q) ? LD64(q + 0x410) : 0;
    if (body_ok(a, b)) return b;
    p = LD64(a + 0x198);
    b = in_guest(p) ? LD64(p + 0x920) : 0;
    if (body_ok(a, b)) return b;
    // (animals, 2026-11-16, not verified: the physics instance's main body, as objects.inc actor_rb's fast path)
    p = LD64(a + 0x190);
    b = in_guest(p) ? LD64(p + 0x78) : 0;
    return body_ok(a, b) ? b : 0;
}

// A following copy keeps the body it had (2026-11-16, soak probe: a follower's actor matrix can stay where it was
// while the body follows the owner's enemy, so body_ok's 5 m test dropped the body once the owner's enemy walked off
// and the copy stopped following): accepted again while one of enemy_body's pointer paths from this actor still leads
// to it and it is still a rigid body.
static bool body_linked(uint64_t a, uint64_t b) {
    if (!in_guest(b) || LD64(b) != RIGID_BODY_VT) return false;
    uint64_t p = LD64(a + 0x578), q = in_guest(p) ? LD64(p + 0x80) : 0;
    if (in_guest(q) && LD64(q + 0x410) == b) return true;
    p = LD64(a + 0x198);
    if (in_guest(p) && LD64(p + 0x920) == b) return true;
    p = LD64(a + 0x190);
    return in_guest(p) && LD64(p + 0x78) == b;
}
static uint64_t enemy_body_kept(const struct Enemy *e) {
    uint64_t b = enemy_body(e->actor);
    if (!b && e->following && body_linked(e->actor, e->body)) b = e->body;
    return b;
}

// (2026-11-16, worker world2; spawns report: squirrels whose body is not found had no position, so they were neither
// copied nor deduped) A wild animal whose physics body enemy_body cannot find is shared by its actor matrix instead
// (position + facing; flag COOP_ENEMY_MTXPOS): the owner sends the matrix, a follower with a body puts its body there
// plus its own body-to-matrix offset, a follower without one writes both actor matrices (that moves a body-less
// actor). SAVAGEWIND_COOP_MTXPOS=0: off (such animals unshared again).
static bool mtxpos_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_MTXPOS");
    return on;
}
static void mtx_quat(const float m[12], float q[4]) {  // rotation part of a row-major 3x4 -> quaternion x y z w
    float r00 = m[0], r01 = m[1], r02 = m[2], r10 = m[4], r11 = m[5], r12 = m[6], r20 = m[8], r21 = m[9], r22 = m[10];
    float tr = r00 + r11 + r22;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q[3] = 0.25f * s, q[0] = (r21 - r12) / s, q[1] = (r02 - r20) / s, q[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = sqrtf(1.0f + r00 - r11 - r22) * 2;
        q[3] = (r21 - r12) / s, q[0] = 0.25f * s, q[1] = (r01 + r10) / s, q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = sqrtf(1.0f + r11 - r00 - r22) * 2;
        q[3] = (r02 - r20) / s, q[0] = (r01 + r10) / s, q[1] = 0.25f * s, q[2] = (r12 + r21) / s;
    } else {
        float s = sqrtf(1.0f + r22 - r00 - r11) * 2;
        q[3] = (r10 - r01) / s, q[0] = (r02 + r20) / s, q[1] = (r12 + r21) / s, q[2] = 0.25f * s;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 0.5f && n < 2.0f)) { q[0] = q[1] = q[2] = 0, q[3] = 1; return; }
    for (int k = 0; k < 4; k++) q[k] /= n;
}
// Both actor matrices from a position and a quaternion (moves an actor without a physics body).
static void enemy_put_mtx(uint64_t a, const float p[3], const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float m[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p[0],
                   2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p[1],
                   2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p[2]};
    memcpy((void *)(uintptr_t)(a + ACTOR_MTX), m, sizeof m);
    memcpy((void *)(uintptr_t)(a + ACTOR_MTX2), m, sizeof m);
}

static bool stasisfoe_controls_actor(uint64_t actor);
static uint64_t stasisfoe_aim_id;
static GuestFn stasisfoe_ai_orig;
static bool stasisfoe_aim_actor(uint64_t actor) {
    uint64_t id = __atomic_load_n(&stasisfoe_aim_id, __ATOMIC_RELAXED);
    return id && in_guest(actor) && id == 1ull + LD32(actor + PROC_ID);
}
static void enemy_ai(struct Enemy *e, bool on) {
    if (stasisfoe_controls_actor(e->actor)) return;
    uint8_t f = LD8(e->actor + ENEMY_FLAGS);
    ST8(e->actor + ENEMY_FLAGS, on ? (uint8_t)(f & ~ENEMY_SKIP_AI) : (uint8_t)(f | ENEMY_SKIP_AI));
}

// ---- thaw: the skip-AI bit is the game's general freeze flag (30+ checks): with it set, the skeleton stops too and
// the guest's copy is a statue. The actor's per-frame jobs are called through pointers (registered by the actor
// constructor 0x700151ec34), so we wrap the six that are not the AI job (0x700151f9d4): for a thawed actor the bit is
// cleared while the job runs and set again afterwards. Which jobs: thaw_mask bits, in thaw_jobs order.
static const uint64_t thaw_jobs[] = {0x700151f090ull, 0x700151f8ecull, 0x700151fdb4ull,
                                     0x700151fe18ull, 0x700152038cull, 0x70015203b0ull};
#define THAW_JOBS (int)(sizeof thaw_jobs / sizeof thaw_jobs[0])
static GuestFn thaw_orig[THAW_JOBS];
static volatile int thaw_mask = (1 << THAW_JOBS) - 1;  // all six; SAVAGEWIND_COOP_THAW_MASK overrides (tests)
static volatile uint64_t thaw_dev_actor;  // `coop thaw <actor> <mask>` (dev test)
static bool enemy_thawed(uint64_t a);
// While the local reticle is on a follower, confine our freeze bit to its AI job.
static void stasisfoe_ai_job(Cpu *c) {
    uint64_t a = c->x[0];
    if (stasisfoe_aim_actor(a) && enemy_thawed(a) && !(LD8(a + ENEMY_FLAGS) & ENEMY_SKIP_AI)) {
        ST8(a + ENEMY_FLAGS, LD8(a + ENEMY_FLAGS) | ENEMY_SKIP_AI);
        stasisfoe_ai_orig(c);
        ST8(a + ENEMY_FLAGS, LD8(a + ENEMY_FLAGS) & ~ENEMY_SKIP_AI);
    } else {
        stasisfoe_ai_orig(c);
    }
}
static void enemy_reapply(uint64_t a);
static void enemy_look_job(Cpu *c, uint64_t a, bool before);

// (2026-11-18, events worker, dev `coop evanim`) calls of each wrapped job for one watched actor (the avatar)
static volatile uint64_t evjob_actor;
static volatile uint32_t evjob_calls[6];
static bool evanim_job(Cpu *c, int i, uint64_t a);  // (coop/evcalm.inc: an avatar in our event, unpaused)
static void thaw_call(Cpu *c, int i) {
    uint64_t a = c->x[0];
    if (a && a == evjob_actor) evjob_calls[i]++;
    if (evanim_job(c, i, a)) return;
    if (((thaw_mask >> i) & 1) && in_guest(a) && (a == thaw_dev_actor || enemy_thawed(a)) &&
        !stasisfoe_controls_actor(a) &&
        (LD8(a + ENEMY_FLAGS) & ENEMY_SKIP_AI)) {
        ST8(a + ENEMY_FLAGS, (uint8_t)(LD8(a + ENEMY_FLAGS) & ~ENEMY_SKIP_AI));
        if (i == 0) enemy_look_job(c, a, true);  // (ENEMYLOOK: the spine solver the frozen copy's model update skips)
        thaw_orig[i](c);
        if (i == 0) enemy_look_job(c, a, false);
        ST8(a + ENEMY_FLAGS, (uint8_t)(LD8(a + ENEMY_FLAGS) | ENEMY_SKIP_AI));
        // Thawed jobs also move the copy on their own (job 0x700151f090 dragged it 30-50 m off the host's position;
        // with the put-back after that job only, half the samples were still 11-28 m off): put it back where the host
        // has it after every one.
        enemy_reapply(a);
    } else {
        thaw_orig[i](c);
    }
}
static void thaw_job0(Cpu *c) { thaw_call(c, 0); }
static void thaw_job1(Cpu *c) { thaw_call(c, 1); }
static void thaw_job2(Cpu *c) { thaw_call(c, 2); }
static void thaw_job3(Cpu *c) { thaw_call(c, 3); }
static void thaw_job4(Cpu *c) { thaw_call(c, 4); }
static void thaw_job5(Cpu *c) { thaw_call(c, 5); }
static void lightning_hooks_init(void);  // weather.inc
static void elemarrow_hooks_init(void);  // elemarrow.inc

// Before the game runs (runtime/main.c): overrides are only seen by calls not yet cached. Co-op only.
void coop_hooks_init(void) {
    static const HostFn wraps[THAW_JOBS] = {thaw_job0, thaw_job1, thaw_job2, thaw_job3, thaw_job4, thaw_job5};
    const char *port = getenv("SAVAGEWIND_COOP_PORT"), *peer = getenv("SAVAGEWIND_COOP_PEER");
    // (coop-menu) the menu may start co-op later, and hooks cannot be added then: installed too when it is on (they
    // only pass the call on while no partner's avatar / enemy copy exists)
    if ((!port || !*port) && (!peer || !*peer) && !coop_menu_on()) return;
    if (getenv("SAVAGEWIND_COOP_THAW_MASK")) thaw_mask = (int)strtol(getenv("SAVAGEWIND_COOP_THAW_MASK"), NULL, 0);
    if (!code_matches()) { LOG("[coop] thaw: game code differs, off\n"); return; }
    astree_hooks_init();  // (protocol 29, coop/astree.inc) the avatar's AS inputs after the NPC's own update
    lightning_hooks_init();
    elemarrow_hooks_init();
    for (int i = 0; i < THAW_JOBS; i++)
        if (!(thaw_orig[i] = dispatch_lookup(thaw_jobs[i]))) { LOG("[coop] thaw: job %llx not found, off\n",
                                                                    (unsigned long long)thaw_jobs[i]); return; }
    for (int i = 0; i < THAW_JOBS; i++) hle_override(thaw_jobs[i], wraps[i], "coop thaw job");
    if (look_on("SAVAGEWIND_COOP_STASISFOLLOW") && look_on("SAVAGEWIND_COOP_STASISFOE") &&
        look_on("SAVAGEWIND_COOP_STASIS") && (stasisfoe_ai_orig = dispatch_lookup(0x700151f9d4ull)))
        hle_override(0x700151f9d4ull, stasisfoe_ai_job, "coop Stasis follower AI");
    LOG("[coop] thaw: %d actor jobs wrapped\n", THAW_JOBS);
}

// Guest: follow the host's copy (AI off) while its position is fresh and the enemy is alive; else run our own AI.
// The guest's copy plays the host's AS (slot 0): with its AI off nothing else picks one. SAVAGEWIND_COOP_ENEMY_ANIM=0
// turns this off (tests: compare with and without).
static bool enemy_anim_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_ENEMY_ANIM") && !strcmp(getenv("SAVAGEWIND_COOP_ENEMY_ANIM"), "0"));
    return on;
}

// ---- (2026-11-16, ENEMYLOOK, protocol 55) whom the enemy's head looks at. Reported: on a target switch, enemies aim
// at one player while their head looks at the other. An actor's look-at object (aim.inc: obj = **(actor+0x5a0);
// +0x18 world target, +0x9c u16 bit 0x10 battle, +0xe4 u32 bit 0x2 look request) is fed every frame by its own AI;
// the follower's copy has its AI off, so its target stayed wherever its own AI last put it while the body (copied)
// turned to the owner's target. The owner sends whether its enemy looks at a player and which one (the one nearer
// the look target); the follower points its copy's look-at at the same player in its game (the owner's own Link =
// our avatar, the owner's partner = our Link). SAVAGEWIND_COOP_ENEMYLOOK=0: off (dev `coop enemylook 0|1`).
static volatile int enemylook_mode = -1;
static bool enemylook_on(void) {
    if (enemylook_mode < 0) enemylook_mode = look_on("SAVAGEWIND_COOP_ENEMYLOOK");
    return enemylook_mode;
}
#define ENEMY_LOOK_HEIGHT 1.5f  // follower: look at this height above the player's feet (m)
#define ENEMY_LOOK_NEAR 4.0f // the look target counts as "at a player" this close to that player (m)
static uint64_t enemy_look_obj(uint64_t actor) {
    uint64_t h = in_guest(actor) ? LD64(actor + 0x5a0) : 0, o = in_guest(h) ? LD64(h) : 0;
    return in_guest(o) ? o : 0;
}
static float look_dist(const float *t, const float *pos) {
    float dx = t[0] - pos[0], dy = t[1] - pos[1], dz = t[2] - pos[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
// (n6, protocol 61, MULTILOOK) More than two players: the owner also names the player by id (COOP_ENEMY_LOOK_PID:
// its own Link = our id, else the nearest remote avatar's player, extras included), and a follower turns its copy's
// head to that player (our Link when it is our id, else that player's avatar drawn here). With two players the id
// names the same player LOOK_PARTNER does. SAVAGEWIND_COOP_MULTILOOK=0: no ids (the protocol-55 rule only).

static bool multilook_on(void) { static int on = -1; if (on < 0) on = look_on("SAVAGEWIND_COOP_MULTILOOK"); return on; }
static uint64_t rav_actor_of(int pid) {  // the avatar of player pid drawn here (0: none)
    for (int t = 0; t < rav_n; t++)
        if (rav[t].pid == pid && in_guest(rav[t].actor)) return rav[t].actor;
    return 0;
}
// Owner: the COOP_ENEMY_LOOK* flags for an enemy we run.
static uint32_t enemy_look_flags(uint64_t actor) {
    uint64_t o = enemy_look_obj(actor);
    if (!enemylook_on() || !o || !(LD32(o + 0xe4) & 2)) return 0;
    const float *t = (const float *)(uintptr_t)(o + 0x18);
    if (!isfinite(t[0] + t[1] + t[2])) return 0;
    float dm = look_dist(t, (const float *)(uintptr_t)(player_info_ptr() + 0x88)), dp = 1e9f;
    if (avatar.actor && avatar_alive()) {
        const float *am = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
        float ap[3] = {am[3], am[7], am[11]};
        dp = look_dist(t, ap);
    }
    // (n6) the nearest extra player's avatar
    int xpid = -1;
    float dx = 1e9f;
    for (int k = 0; multilook_on() && k < rav_n; k++) {
        if (rav[k].partner || !in_guest(rav[k].actor)) continue;
        const float *am = (const float *)(uintptr_t)(rav[k].actor + ACTOR_MTX);
        float ap[3] = {am[3], am[7], am[11]}, d = look_dist(t, ap);
        if (d < dx) dx = d, xpid = rav[k].pid;
    }
    if (dm > ENEMY_LOOK_NEAR && dp > ENEMY_LOOK_NEAR && dx > ENEMY_LOOK_NEAR) return 0;  // (something else: no player)
    uint32_t battle = (LD16(o + 0x9c) & 0x10) ? COOP_ENEMY_LOOK_BATTLE : 0;
    if (dx < dm && dx < dp) {  // (an extra player: only by id)
        if (xpid < 0 || xpid >= COOP_MAX_PLAYERS) return 0;
        return COOP_ENEMY_LOOK | battle | COOP_ENEMY_LOOK_PID | ((uint32_t)xpid << COOP_ENEMY_LOOK_PID_SHIFT);
    }
    uint32_t f = COOP_ENEMY_LOOK | (dp < dm ? COOP_ENEMY_LOOK_PARTNER : 0) | battle;
    int pid = dp < dm ? coop_net_partner_id() : coop_net_my_id();
    if (multilook_on() && pid >= 0 && pid < COOP_MAX_PLAYERS)
        f |= COOP_ENEMY_LOOK_PID | ((uint32_t)pid << COOP_ENEMY_LOOK_PID_SHIFT);
    return f;
}
// Follower: point the copy's look-at where the owner's enemy looks (called every frame while it follows); enemy_look_job
// runs the game's spine solver for it (0x70010e9494(obj+0x10, obj+8): yaw/pitch per node). Probe 2026-11-16: the game runs
// that solver only from the model update FUN_7101525f3c, gated by the skip-AI bit (actor+0x518 & 0x40) that a
// following copy has set: its head never turned at all (yaw 0, request bit dropped every frame) while the owner's
// looked at its player. The thawed job 0x700151f090 then turns the nodes' yaw/pitch into the bones (aim.inc notes).
#define A_LOOK_SOLVE 0x70010e9494ull
static void enemy_look_follow(Cpu *c, struct Enemy *e) {
    (void)c;
    uint64_t o = enemy_look_obj(e->actor);
    if (!enemylook_on() || !o) return;
    uint32_t fl = 0;
    double fage = 99;
    if (!coop_net_remote_enemy_flags(e->hash, &fl, &fage) || fage > ENEMY_FOLLOW_MAX_AGE) return;
    uint64_t who = 0;
    int look_pid = -1;  // (n6: the player named by id; -1: the protocol-55 rule)
    if ((fl & COOP_ENEMY_LOOK) && (fl & COOP_ENEMY_LOOK_PID) && multilook_on())
        look_pid = (int)((fl & COOP_ENEMY_LOOK_PID_MASK) >> COOP_ENEMY_LOOK_PID_SHIFT);
    if (look_pid >= 0) who = look_pid == coop_net_my_id() ? LD64(player_info_ptr() + 0x60) : rav_actor_of(look_pid);
    else if (fl & COOP_ENEMY_LOOK) who = (fl & COOP_ENEMY_LOOK_PARTNER) ? LD64(player_info_ptr() + 0x60)
                                         : (avatar.actor && avatar_alive() ? avatar.actor : 0);
    uint32_t req = LD32(o + 0xe4);
    if (!in_guest(who)) {
        if (req & 2) ST32(o + 0xe4, req & ~0xc02u);  // (as the game's "drop the request", 0x71010e7644)
        e->look_who = 0;
        e->look_req = false;
    } else {
        const float *m = (const float *)(uintptr_t)(who + ACTOR_MTX);
        float t[3] = {m[3], m[7] + ENEMY_LOOK_HEIGHT, m[11]};
        memcpy((void *)(uintptr_t)(o + 0x18), t, 12);
        ST32(o + 0xe4, req | 2);
        e->look_req = true;
        uint16_t b = LD16(o + 0x9c);
        ST16(o + 0x9c, (fl & COOP_ENEMY_LOOK_BATTLE) ? (uint16_t)(b | 0x10) : (uint16_t)(b & ~0x10));
        if (e->look_who != who) {
            e->look_who = who;
            bool ours = look_pid >= 0 ? look_pid == coop_net_my_id() : (fl & COOP_ENEMY_LOOK_PARTNER) != 0;
            if (ours || look_pid < 0 || look_pid == coop_net_partner_id())
                LOG("[coop] enemy %s %u looks at %s (as the other side's)\n", e->name, e->hash,
                    ours ? "our player" : "the avatar");
            else
                LOG("[coop] enemy %s %u looks at player %d's avatar (as the other side's)\n", e->name, e->hash, look_pid);
        }
    }
}
// (job thread, from thaw_call around the thawed job 0x700151f090 of a following copy) The game's own frame runs the
// spine solver in the model update, which the frozen copy skips; we run it right before job 0, which turns the nodes'
// yaw/pitch into bone deltas (FUN_7101524db0 -> FUN_71010e9fd0). Probe 2026-11-16 run 4: solving at our frame start
// gave yaw -0.08 that was ~0 again by the next frame start. SAVAGEWIND_COOP_LOOKTRACE=1 (dev): yaw before / after.
// Hardened 2026-11-16 (one unexplained crash in run 5: a followed Bokoblin with a garbage vtable called from a job a
// few seconds after this first ran): the job thread reads only the look_pub_* lists (never enemies[], which the game
// thread changes) and solves only when the actor's vtable and look-at object are still the ones recorded when the
// copy began to follow, the vtable lies in the main module, and the node list looks sane. Skipped otherwise.
#define LOOK_NODES_MAX 16
static bool look_vt_ok(uint64_t vt) { return vt >= 0x7000000000ull && vt < 0x7003000000ull; }
static void enemy_look_job(Cpu *c, uint64_t a, bool before) {
    if (!enemylook_on()) return;
    int k = 0;
    while (k < FOLLOW_MAX && look_pub_actor[k] && look_pub_actor[k] != a) k++;
    if (k >= FOLLOW_MAX || look_pub_actor[k] != a) return;
    uint64_t vt = LD64(a), o = enemy_look_obj(a);
    if (!o || vt != look_pub_vt[k] || !look_vt_ok(vt) || o != look_pub_obj[k]) return;
    uint32_t nc = LD32(o + 0x28);
    uint64_t nodes = LD64(o + 0x30);
    if (nc > LOOK_NODES_MAX || (nc && !in_guest(nodes + (uint64_t)nc * 0x180))) return;
    bool req = look_pub_req[k];
    static int trace = -1;
    if (trace < 0) trace = getenv("SAVAGEWIND_COOP_LOOKTRACE") && !strcmp(getenv("SAVAGEWIND_COOP_LOOKTRACE"), "1");
    bool nn = nc && in_guest(nodes);
    static _Thread_local float y0, y1;
    static _Thread_local uint32_t r0;
    if (before) {
        if (req) ST32(o + 0xe4, LD32(o + 0xe4) | 2);
        if (nn) memcpy(&y0, (void *)(uintptr_t)(nodes + 0x134), 4);
        r0 = LD32(o + 0xe4);
        Cpu saved = *c;  // (also without a request: the angles ease back to straight ahead)
        c->x[0] = o + 0x10; c->x[1] = o + 8;
        guest_call(c, A_LOOK_SOLVE);
        *c = saved;
        if (nn) memcpy(&y1, (void *)(uintptr_t)(nodes + 0x134), 4);
        return;
    }
    static uint64_t tr_ns;
    if (trace && nn && req && plat_now_ns() - tr_ns > 400000000ull) {
        tr_ns = plat_now_ns();
        float y2;
        memcpy(&y2, (void *)(uintptr_t)(nodes + 0x134), 4);
        LOG("[coop] looktrace %llx: req %x yaw %.3f -> solver %.3f -> after job 0 %.3f (req now %x)\n",
            (unsigned long long)a, r0, y0, y1, y2, LD32(o + 0xe4));
    }
}

static uint64_t enemy_target_actor(uint64_t enemy);
// (2026-11-18, LOOKDIAG, dev: SAVAGEWIND_COOP_LOOKDIAG=1) ~4 lines/s per living enemy within 25 m of our Link: who runs
// it, its AS, whom it attacks, where its body faces and where its look-at points (degrees from the body's forward to
// our Link / the avatar / the look target; + = to its left), the look-at nodes' summed yaw (the head turn, degrees).
static void enemy_look_diag(Cpu *c, struct Enemy *e, uint64_t now) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_COOP_LOOKDIAG") && !strcmp(getenv("SAVAGEWIND_COOP_LOOKDIAG"), "1");
    static uint64_t last[ENEMY_TABLE];
    int idx = (int)(e - enemies);
    if (!on || idx < 0 || idx >= ENEMY_TABLE || now - last[idx] < 250000000ull) return;
    const float *em = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
    uint64_t pa = LD64(player_info_ptr() + 0x60);
    if (!in_guest(pa)) return;
    const float *pm = (const float *)(uintptr_t)(pa + ACTOR_MTX);
    float dx = pm[3] - em[3], dz = pm[11] - em[11];
    if (dx * dx + dz * dz > 25.0f * 25.0f) return;
    last[idx] = now;
    float fy = atan2f(em[2], em[10]);  // (body forward, yaw: 0 = +z, 90 = +x)
#define LD_ANG(px, pz) ({ float a_ = (atan2f((px) - em[3], (pz) - em[11]) - fy) * 57.29578f; \
        while (a_ > 180) a_ -= 360; while (a_ < -180) a_ += 360; a_; })
    float ap = LD_ANG(pm[3], pm[11]), aa = 999, al = 999, hy = 0;
    uint64_t av = avatar.actor && avatar_alive() ? avatar.actor : 0;
    if (av) { const float *m = (const float *)(uintptr_t)(av + ACTOR_MTX); aa = LD_ANG(m[3], m[11]); }
    uint64_t o = enemy_look_obj(e->actor);
    uint32_t req = 0, nc = 0;
    const char *lw = "-";
    if (o) {
        const float *t = (const float *)(uintptr_t)(o + 0x18);
        req = LD32(o + 0xe4);
        if (isfinite(t[0] + t[2])) al = LD_ANG(t[0], t[2]);
        float pp[3] = {pm[3], pm[7], pm[11]}, dpl = look_dist(t, pp), dav = 1e9f;
        if (av) { const float *m = (const float *)(uintptr_t)(av + ACTOR_MTX); float q[3] = {m[3], m[7], m[11]}; dav = look_dist(t, q); }
        lw = dpl < dav ? (dpl < ENEMY_LOOK_NEAR + 1 ? "PLAYER" : "near-none") : (dav < ENEMY_LOOK_NEAR + 1 ? "AVATAR" : "near-none");
        nc = LD32(o + 0x28);
        uint64_t nodes = LD64(o + 0x30);
        for (uint32_t k = 0; k < nc && k < LOOK_NODES_MAX && in_guest(nodes + (uint64_t)(k + 1) * 0x180); k++) {
            float y;
            memcpy(&y, (void *)(uintptr_t)(nodes + (uint64_t)k * 0x180 + 0x134), 4);
            if (isfinite(y)) hy += y;
        }
    }
#undef LD_ANG
    uint64_t ta = enemy_target_actor(e->actor);
    char as[64];
    player_as_name(c, e->actor, as, sizeof as);
    uint32_t fl = 0;
    double fage = 99;
    coop_net_remote_enemy_flags(e->hash, &fl, &fage);
    LOG("[coop] lookdiag %u %s AS %s target %s | body->player %.0f body->avatar %.0f body->look %.0f look@%s req %x "
        "nodes %u headyaw %.0f | sends %x their %x%s\n", e->hash, e->mine ? "OWNER" : e->following ? "FOLLOW" : "other",
        as, ta == pa ? "PLAYER" : (av && ta == av) ? "AVATAR" : ta ? "other" : "none", ap, aa, al, lw, req, nc,
        hy * 57.29578f, e->mine ? enemy_look_flags(e->actor) : 0,
        fl & (COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PARTNER | COOP_ENEMY_LOOK_BATTLE | COOP_ENEMY_LOOK_PID),
        e->tgt_partner ? " steered" : "");
}

#define WARM_NS 1500000000ull
static bool warm_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_ENEMY_WARM") && !strcmp(getenv("SAVAGEWIND_COOP_ENEMY_WARM"), "0"));
    return on;
}
static void enemy_follow(Cpu *c, struct Enemy *e, int32_t life) {
    float p[3], rot[4];
    double age;
    char as[32];
    bool fresh = coop_net_remote_enemy_pos(e->hash, p, rot, &age, as, sizeof as) && age < ENEMY_FOLLOW_MAX_AGE;
    if (!fresh && life > 0 && e->spawned) return;  // (a copy has no AI of its own: it waits, or spawned_frame removes it)
    if (!fresh || life <= 0) {  // (dead: the game's death may need the AI; let it run)
        if (e->following) {
            enemy_ai(e, true);
            e->following = false;
            LOG("[coop] enemy %s %u own AI again (%s)\n", e->name, e->hash, life <= 0 ? "dead" : "no position from the other side");
        }
        return;
    }
    // AI warm-up (2026-11-14): a copy frozen from its first frame has an AI that never ran; when it later became ours
    // (a hand-over) that AI stayed asleep in Hide and ignored real sword hits and injected ones (probe_aggro: guest
    // took over a sleeping Bokoblin, 8 s in Hide, 3 hits). The host's copy, whose AI had run before it was frozen,
    // woke at once on a hit after taking it back. So a fresh copy runs its own AI for WARM_NS before it follows.
    // SAVAGEWIND_COOP_ENEMY_WARM=0 turns this off.
    if (!e->following && !e->spawned && warm_on() && plat_now_ns() - e->born_ns < WARM_NS) return;
    uint64_t body = enemy_body_kept(e);
    uint32_t rfl = 0;
    double rfage = 99;
    coop_net_remote_enemy_flags(e->hash, &rfl, &rfage);
    bool their_mtx = (rfl & COOP_ENEMY_MTXPOS) != 0;
    if (!body && !(e->animal && mtxpos_on())) return;
    const float *am = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
    if (e->following && e->mtx_follow != !body) {  // (body found / lost while following: shown is in the other frame)
        e->following = false;
        LOG("[coop] enemy %s %u: %s, following anew\n", e->name, e->hash, body ? "body found" : "body lost");
    }
    e->mtx_follow = !body;
    e->body = body;
    if (!e->following) {
        e->playing[0] = 0;  // Only an animation applied during this follow period can be released on takeover.
        // (wake, 2026-11-14) The AS our own AI chose last: its current action waits on it (see enemy_wake).
        player_as_name(c, e->actor, e->own_as, sizeof e->own_as);
        enemy_ai(e, false);
        e->following = true;
        e->look_vt = LD64(e->actor), e->look_o = enemy_look_obj(e->actor);  // (ENEMYLOOK: checked on the job thread)
        e->wake_ns = 0;
        e->follow_ns = plat_now_ns();
        if (body) {
            memcpy(e->shown, (void *)(uintptr_t)(body + 0x1a0), 12);
            for (int k = 0; k < 3; k++) e->mtx_off[k] = e->shown[k] - am[3 + 4 * k];
        } else {
            for (int k = 0; k < 3; k++) e->shown[k] = am[3 + 4 * k];
        }
        LOG("[coop] enemy %s %u follows the other side (AI off; its own AS was %s)%s\n", e->name, e->hash,
            e->own_as[0] ? e->own_as : "-", body ? (their_mtx ? " - their matrix, our body" : "") : " - by its matrix (no body)");
    }
    if (body && their_mtx)  // (their feet; our body sits mtx_off above ours)
        for (int k = 0; k < 3; k++) p[k] += e->mtx_off[k];
    if (!(LD8(e->actor + ENEMY_FLAGS) & ENEMY_SKIP_AI)) enemy_ai(e, false);  // (something cleared it)
    float d2 = 0;
    for (int k = 0; k < 3; k++) d2 += (p[k] - e->shown[k]) * (p[k] - e->shown[k]);
    for (int k = 0; k < 3; k++) e->shown[k] = d2 > 64.0f ? p[k] : e->shown[k] + (p[k] - e->shown[k]) * 0.3f;
    memcpy(e->rot, rot, sizeof e->rot);
    if (!body) {
        enemy_put_mtx(e->actor, e->shown, rot);  // (MTXPOS: no body here)
    } else {
        // Only the physics body: the game derives the actor matrices from it every frame. (Writing the host's matrix
        // position here, which is at the feet, put the body ~1 m into the ground and the copy lay flat.)
        memcpy((void *)(uintptr_t)(body + 0x1a0), e->shown, 12);
        memcpy((void *)(uintptr_t)(body + 0x1d0), rot, 16);
        memcpy((void *)(uintptr_t)(body + 0x1e0), rot, 16);
    }
    if (enemy_anim_on() && as[0] && !e->ice_held) {
        char cur[64];
        player_as_name(c, e->actor, cur, sizeof cur);
        if (strcmp(cur, as)) {
            play_anim(c, e->actor, as, 0);
            if (strcmp(e->playing, as)) {
                snprintf(e->playing, sizeof e->playing, "%s", as);
                LOG("[coop] enemy %s %u plays the host's %s (was %s)\n", e->name, e->hash, as, cur);
            }
        }
    }
}

// (Read from the game's job threads: a stale answer only thaws or freezes one job run.)
static bool npc_thawed(uint64_t a);   // (coop/npc.inc: following townspeople)
static void npc_reapply(uint64_t a);
static bool enemy_thawed(uint64_t a) {
    if (npc_thawed(a)) return true;
    for (int i = 0; i < FOLLOW_MAX && follow_actor[i]; i++)  // (enemies_frame keeps this list short)
        if (follow_actor[i] == a) return true;
    return false;
}

// After a thawed job moved a following copy: its body and both matrices back to the host's pose (e->shown, e->rot).
static void enemy_reapply(uint64_t a) {
    if (npc_thawed(a)) { npc_reapply(a); return; }
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (!e->following || e->actor != a) continue;
        uint64_t body = e->body;  // (not enemy_body: mid-snap the body and the matrix can be > 5 m apart)
        if (e->mtx_follow && !body) { enemy_put_mtx(a, e->shown, e->rot); return; }  // (MTXPOS)
        if (!in_guest(body)) return;
        // (+0x1b0, +0x1c0 and +0x250 hold more copies of the position; without them the copy still jumped back
        // toward its old spot now and then: 2 of 8 samples 13-27 m off.)
        static const uint32_t pos_at[] = {0x1a0, 0x1b0, 0x1c0, 0x250};
        for (int k = 0; k < 4; k++) memcpy((void *)(uintptr_t)(body + pos_at[k]), e->shown, 12);
        memcpy((void *)(uintptr_t)(body + 0x1d0), e->rot, 16);
        memcpy((void *)(uintptr_t)(body + 0x1e0), e->rot, 16);
        float x = e->rot[0], y = e->rot[1], z = e->rot[2], w = e->rot[3];  // quaternion -> 3x4 row-major
        float m[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), e->shown[0],
                       2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), e->shown[1],
                       2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), e->shown[2]};
        memcpy((void *)(uintptr_t)(a + ACTOR_MTX), m, sizeof m);
        memcpy((void *)(uintptr_t)(a + ACTOR_MTX2), m, sizeof m);
        return;
    }
}

// ---- hit reaction (2026-11-08). Dev only now (`coop flinch`): 0x7000bdd410(actor, AttackInfo*) turned out to be
// the XLink side of a hit (sound/effect "Damage" and XLink property 0x16), not the flinch. A real sword hit passed
// {+0 0, +4 type 2, +8 damage 2, +0x10 the attacker's link, bytes +0x1b..+0x1d = 1}; our link is empty ({0, id -1}:
// the resolver 0x700056d8d8 treats id -1 as "no actor"). The real reaction comes from the received-hit fields
// (enemy_inject below), which the host uses for the other player's hits. SAVAGEWIND_COOP_FLINCH=0 turns that off
// (the life is then written directly, no reaction).
#define A_ENEMY_REACT 0x7000bdd410ull
static uint64_t flinch_mem;
static bool flinch_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_FLINCH") && !strcmp(getenv("SAVAGEWIND_COOP_FLINCH"), "0"));
    return on;
}
static void enemy_flinch(Cpu *c, uint64_t actor, int damage) {
    if (!flinch_mem) flinch_mem = (uint64_t)(uintptr_t)hle_alloc(0x100);
    if (!flinch_mem || !in_guest(actor)) return;
    uint8_t *m = (uint8_t *)(uintptr_t)flinch_mem;
    memset(m, 0, 0x100);
    int32_t type = 2;
    memcpy(m + 4, &type, 4);
    memcpy(m + 8, &damage, 4);
    ST64(flinch_mem + 0x10, flinch_mem + 0x80);  // the attacker link -> an empty handle
    ST32(flinch_mem + 0x88, 0xffffffffu);
    m[0x1b] = m[0x1c] = m[0x1d] = 1;
    Cpu saved = *c;
    c->x[0] = actor; c->x[1] = flinch_mem;
    for (int i = 2; i < 8; i++) c->x[i] = 0;
    guest_call(c, A_ENEMY_REACT);
    *c = saved;
}
static volatile uint64_t flinch_dev_actor;  // `coop flinch <actor>` (dev test)
static struct { uint64_t addr; volatile uint32_t len; volatile int fresh; int lines; uint8_t prev[0x400]; } watch;

// A received hit, written the way a real sword hit leaves it for one frame (2026-11-08, `coop watch` on the
// plateau Bokoblin's receiver *(actor+0x840) during a hit): +0x44/+0x48 damage, +0x50 0, +0x54 kind (15 there;
// the AttackInfo of other hits had 2), +0x5c 2, +0x6c 0, +0x7c 2, +0x8c 0x10. The next frame the game resets them.
#define ENEMY_RECEIVER 0x840
static void enemy_inject(uint64_t actor, int damage, int kind) {
    uint64_t r = LD64(actor + ENEMY_RECEIVER);
    if (!in_guest(r)) return;
    ST32(r + 0x44, (uint32_t)damage);
    ST32(r + 0x48, (uint32_t)damage);
    ST32(r + 0x50, 0);
    ST32(r + 0x54, (uint32_t)kind);
    ST32(r + 0x5c, 2);
    ST32(r + 0x6c, 0);
    ST32(r + 0x7c, 2);
    ST32(r + 0x8c, 0x10);
}
// Take-over after our own hit (aggro, 2026-11-14): our player hit our following copy while its AI was off, so the
// game never reacted to that hit here. When the enemy becomes ours because of it, replay it as a received hit (kind
// 15, flinch; it also wakes a sleeping enemy into battle against our Link). The damage is already in its life: life
// +1 and a 1-damage hit (net 0; a hit the game ignores is undone by the 0.3 s life write in enemies_frame).
// SAVAGEWIND_COOP_AGGRO_KICK=0 turns it off.
#define KICK_NS 2000000000ull
static bool kick_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_AGGRO_KICK") && !strcmp(getenv("SAVAGEWIND_COOP_AGGRO_KICK"), "0"));
    return on;
}
static void enemy_kick(struct Enemy *e, uint64_t now) {
    int32_t life = (int32_t)LD32(e->actor + ENEMY_LIFE);
    if (life <= 0 || !in_guest(LD64(e->actor + ENEMY_RECEIVER))) return;
    if (life < e->base) {
        ST32(e->actor + ENEMY_LIFE, (uint32_t)(life + 1));
        enemy_inject(e->actor, 1, 15);
    } else {
        enemy_inject(e->actor, 0, 15);
    }
    e->last = life;
    e->inject_ns = now;
}
// AS names of a sleeping / hidden enemy (Bokoblin camp: Hide).
static bool asleep_as(const char *as) { return !strcmp(as, "Hide") || strstr(as, "Sleep"); }
// ---- softlock fixes (2026-11-14, play-test: enemy AI did not switch targets smoothly and often stayed
// softlocked until hit). Seen in the play-test logs
// (play-test/, play-test-p2/): two archer Bokoblins fighting the guest were taken back by the host with the 6 m
// distance rule; enemy_wake gave them the AS their AI had 36 s before (Wait, StandTalk) and both stood idle ~25 s on
// both screens until the host hit them. The frozen AI of a following copy keeps its pre-fight state; a hand-over
// resumes it as it was. Four rules, each with an off switch:
//  - BATTLE_HOLD: no distance hand-over while the enemy fights (only a hit, or its player far away, moves it);
//  - WAKE_FRESH: give the copy's own AS back only if it started following < 3 s ago (else that AS is stale);
//  - WAKE_KICK: a copy taken over mid-fight (it showed a battle AS, or a player hit it within 10 s) gets a 0-damage
//    hit and the local player as its target, and the wake check treats an idle AS as asleep;
//  - TARGET_CALM: steer only fighting enemies at the avatar, and re-write that target at most every 0.5 s.
// A battle AS (Bokoblin/Moblin/Lizalfos names seen in the logs and the AS lists).
static bool battle_as(const char *as) {
    static const char *const sub[] = {"Battle", "Arrow", "Bow", "Attack", "Guard", "Grab", "Damage", "Throw",
                                      "Search", "Notice", "Cry", "Rush", "Jump", "Dash", "Find", "Warn", "Noise"};
    if (!as[0] || strstr(as, "CareFree")) return false;  // (SearchCareFree: a calm look-around)
    if (!strcmp(as, "Run") || !strcmp(as, "Turn")) return true;
    for (size_t i = 0; i < sizeof sub / sizeof sub[0]; i++)
        if (strstr(as, sub[i])) return true;
    return false;
}
static bool env_on(const char *name, int *cache) {
    if (*cache < 0) *cache = !(getenv(name) && !strcmp(getenv(name), "0"));
    return *cache;
}
static int battle_hold_c = -1, wake_fresh_c = -1, wake_kick_c = -1, target_calm_c = -1;
static bool battle_hold_on(void) { return env_on("SAVAGEWIND_COOP_BATTLE_HOLD", &battle_hold_c); }
static bool wake_fresh_on(void) { return env_on("SAVAGEWIND_COOP_WAKE_FRESH", &wake_fresh_c); }
static bool wake_kick_on(void) { return env_on("SAVAGEWIND_COOP_WAKE_KICK", &wake_kick_c); }
static bool target_calm_on(void) { return env_on("SAVAGEWIND_COOP_TARGET_CALM", &target_calm_c); }
#define FIGHT_NS 4000000000ull     // a battle AS counts this long (Wait/Walk frames between swings)
#define FIGHT_HIT_NS 10000000000ull  // a hit this recent: taken over mid-fight
#define WAKE_FRESH_NS 3000000000ull
#define OWNER_FAR 40.0f            // in a fight, a distance hand-over only when its player is this far from it
#define TGT_RESET_NS 150000000ull  // (2026-11-15: 0.5 s let the AI put its own Link back 4 times in 2 s; test_combat)

// ---- wake on take-over (2026-11-14, combat2). Cause of the "deaf" copy (all 5 failing take-overs in the
// test_enemy_aggro / probe_aggro guest logs, dev-1114-10*-37778): the copy's own AI had just noticed the guest's Link
// (AS Notice, a one-shot) when it started following, and the following code then played the host's AS on it (Hide,
// a loop). The frozen AI's current action still waited for ITS AS (Notice) to end; when the AI was switched back on
// that AS never came, so the action never finished and the AI sat in Hide, ignoring hits. In the one take-over that
// woke (dev-1114-104248), the host's AS at the hand-over was the same kind (Notice -> BattleWait). Fix: on a
// take-over, give the AI back the AS it had chosen itself (own_as, read when following started), then replay our hit
// (enemy_kick). If it still shows a sleeping AS 0.6 s later, replay a hit again (twice at most); the host's fallback
// (still asleep 1.5 s after an aggro hand-over -> back to the host) stays. SAVAGEWIND_COOP_ENEMY_WAKE=0 turns it off.
#define WAKE_RETRY_NS 600000000ull
#define WAKE_TRIES 2
static bool wake_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_ENEMY_WAKE") && !strcmp(getenv("SAVAGEWIND_COOP_ENEMY_WAKE"), "0"));
    return on;
}
// Just after enemy_ai(e, true) on a take-over (game thread).
static void enemy_wake(Cpu *c, struct Enemy *e) {
    if (!wake_on() || !e->own_as[0]) return;
    if (wake_fresh_on()) {  // (softlock, 2026-11-14: "Hide given back (was showing Run)"; "Run given back (was Die)")
        int32_t life = (int32_t)LD32(e->actor + ENEMY_LIFE);
        if (life <= 0 || plat_now_ns() - e->follow_ns > WAKE_FRESH_NS) return;
    }
    char cur[64];
    player_as_name(c, e->actor, cur, sizeof cur);
    if (strcmp(cur, e->own_as)) {
        play_anim(c, e->actor, e->own_as, 0);
        LOG("[coop] enemy %s %u wake: its own AI's AS %s given back (was showing %s)\n", e->name, e->hash, e->own_as,
            cur[0] ? cur : "-");
    }
}
// Every frame for an enemy this side runs: still asleep after a take-over with a replayed hit -> hit it again.
static void enemy_wake_check(Cpu *c, struct Enemy *e, uint64_t now) {
    if (!e->wake_ns || now - e->wake_ns < WAKE_RETRY_NS) return;
    char cur[64];
    player_as_name(c, e->actor, cur, sizeof cur);
    if (!asleep_as(cur) && !(e->wake_battle && !battle_as(cur))) {
        LOG("[coop] enemy %s %u wake: awake (AS %s) after %d extra hit(s)\n", e->name, e->hash, cur, e->wake_tries);
        e->wake_ns = 0, e->wake_battle = false;
    } else if (e->wake_tries >= WAKE_TRIES) {
        LOG("[coop] enemy %s %u wake: still asleep (AS %s) after %d extra hits\n", e->name, e->hash, cur, e->wake_tries);
        e->wake_ns = 0, e->wake_battle = false;
    } else {
        e->wake_tries++;
        enemy_kick(e, now);
        e->wake_ns = now;
        LOG("[coop] enemy %s %u wake: still %s, hit replayed again (%d)\n", e->name, e->hash, cur, e->wake_tries);
    }
}
// ---- the partner as a target (2026-11-14, combat2). In the game that runs an enemy, the other player is only the
// avatar (a puppet), so the enemy fought only the local Link. Found with Ghidra (not yet seen live):
//  - an enemy keeps its target in an object at enemy+0xc48 whose link (BaseProcLink: +0 proc unit, +8 id, +0xc
//    acquired) is at enemy+0xc50; 0x7000aa6b60(enemy) returns that link (101 callers: the AI's "get target"),
//    0x7000aa6ac0(enemy) = "has a target" (44 callers).
//  - 0x7000aa6a00(enemy, link*, mtx34* or 0, pos* or 0) sets it (-> 0x70007a3608(enemy+0xc48, ...): copies the link,
//    with 0/0 reads the target actor's matrix and position itself); the AI calls it when it finds the player and from
//    the hit message (msg 0/1/4: target = the attacker, 0x70008648fc).
//  - 0x7000bcc230(enemy) = set the target to the player (link = player actor + 0x68: every actor carries its own
//    link there, 0x7000bd4dd0 returns the player's).
// So the side that runs an enemy points it at the avatar (link avatar+0x68) while the partner is the better target:
// the partner hit it last, or the partner is clearly nearer and our player did not hit it just now. The avatar has no
// hit body here, but the partner's copy of the enemy plays the same AS at the same spot (enemy_follow), and its attack
// hits the partner's real Link in the partner's game (seen 2026-11-08: guest lost 2 hearts next to a following
// Moblin copy): no damage packet is needed. Enemy arrows / thrown rocks aimed at the avatar exist only in the
// owner's game: arrows.inc (FOE_ARROWS) and shots.inc (FOE_SHOTS) hit the partner with them. Asleep enemies are not steered (a target does not wake
// them). SAVAGEWIND_COOP_ENEMY_TARGET=0 turns it off.
#define A_ENEMY_SET_TARGET 0x7000aa6a00ull
#define A_ENEMY_TARGET_PLAYER 0x7000bcc230ull
#define ENEMY_TARGET_LINK 0xc50
#define TGT_RANGE 30.0f              // the partner must be this close to the enemy
#define TGT_MARGIN 2.5f              // ... and this much nearer than our player (no recent hits)
#define TGT_HOLD_NS 1500000000ull    // at most one change of target per 1.5 s
#define OCTO_MIN 20.0f               // (Octoroks: their AI shoots at 20..35 m)
#define OCTO_MAX 35.0f
#define OCTO_WAKE_NS 3000000000ull
#define OCTO_REST_NS 4000000000ull   // after an attack at the avatar, no wake for this long: its AI attacks at once
                                     // when it notices someone (IsFirstAttackIntervalZero; run 15: 39 shots in 42 s)
#define TGT_HIT_NS 4000000000ull     // a hit counts this long for the choice
static bool target_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_ENEMY_TARGET") && !strcmp(getenv("SAVAGEWIND_COOP_ENEMY_TARGET"), "0"));
    return on;
}
// ---- threat score (2026-11-15: combat was too easy when an enemy could only pay attention to 1 player). The
// side running an enemy picks its target between both players like a small aggro table: each player
// scores minus their distance (m), plus THREAT_HIT if they hit it in the last TGT_HIT_NS (+THREAT_LAST for the latest
// hitter), plus THREAT_STICK for the current target (no ping-pong), minus THREAT_SPREAD per other fighting enemy near it
// (that we run) already on that player, so a group splits between both players instead of all chasing one, plus a
// random lean of up to +-THREAT_JITTER m toward the partner, rolled again every 4-7 s. Highest score wins; one change
// per TGT_HOLD_NS at most. SAVAGEWIND_COOP_THREAT=0: the 2026-11-14 rule (last hitter, else clearly nearer).
#define THREAT_HIT 8.0f
#define THREAT_LAST 2.0f
#define THREAT_STICK 3.0f
#define THREAT_SPREAD 3.0f
#define THREAT_JITTER 2.0f
#define THREAT_NEAR 20.0f
#define THREAT_CLOSE 6.0f              // nearer than this, distance does not matter
#define THREAT_BORED_NS 6000000000ull  // on one target this long, it starts to lose interest
#define THREAT_AS_NS 250000000ull  // how often the enemy's AS is read for "is it fighting"
static int threat_c = -1;
static bool threat_on(void) { return env_on("SAVAGEWIND_COOP_THREAT", &threat_c); }
static float threat_rand(void) {  // 0..1
    static uint32_t s = 0;
    if (!s) s = (uint32_t)plat_now_ns() | 1;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return (s >> 8) / 16777216.0f;
}
// (n4) Is this one of the remote avatars drawn here (rav: multi.inc)?
#define TGT_AV_STICK 3.0f  // (n4) the avatar it is steered at counts this much nearer than another remote avatar
static int multitarget_c = -1;
static bool multitarget_on(void) { return env_on("SAVAGEWIND_COOP_MULTITARGET", &multitarget_c); }
static bool rav_has(uint64_t a) {
    for (int t = 0; a && t < rav_n; t++)
        if (rav[t].actor == a) return true;
    return false;
}
static bool threat_pick(struct Enemy *e, float dm, float dp, bool our_hit, bool their_hit, uint64_t now) {
    if (now >= e->tgt_bias_until) {
        e->tgt_bias = (threat_rand() * 2 - 1) * THREAT_JITTER;
        e->tgt_bias_until = now + 4000000000ull + (uint64_t)(threat_rand() * 3e9);
    }
    // (test_combat 2026-11-15, first build: both players at 3-4 m, the Bokoblin only ever fought the one it started
    // on - it walks up to its target, so that one is always nearer, and the stick bonus held it. Now distance counts
    // only beyond THREAT_CLOSE, and after THREAT_BORED_NS on one target the stick bonus turns into a malus.)
    float sm = -(dm > THREAT_CLOSE ? dm : THREAT_CLOSE), sp = -(dp > THREAT_CLOSE ? dp : THREAT_CLOSE) + e->tgt_bias;
    if (our_hit) sm += THREAT_HIT;
    if (their_hit) sp += THREAT_HIT;
    if (our_hit && their_hit) {
        if (e->peer_hit_ns > e->hurt_ns) sp += THREAT_LAST;
        else sm += THREAT_LAST;
    }
    // (stick: +THREAT_STICK for THREAT_BORED_NS on one target, then down to -THREAT_STICK over the same time again)
    if (!e->tgt_ns) e->tgt_ns = now;
    float held = (float)((double)(now - e->tgt_ns) / THREAT_BORED_NS), stick = THREAT_STICK;
    if (held > 1) stick = held >= 2 ? -THREAT_STICK : THREAT_STICK * (1 - 2 * (held - 1));
    if (e->tgt_partner) sp += stick;
    else sm += stick;
    const float *em = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
    float epos[3] = {em[3], em[7], em[11]}, q[3];
    int np = 0, nm = 0;
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *o = &enemies[i];
        if (o == e || !o->actor || !o->mine || o->following || o->last <= 0 || !o->tgt_battle_ns ||
            now - o->tgt_battle_ns > FIGHT_NS || actor_dist(o->actor, epos, q) > THREAT_NEAR)
            continue;
        if (o->tgt_partner) np++;
        else nm++;
    }
    sp -= THREAT_SPREAD * np;
    sm -= THREAT_SPREAD * nm;
    e->tgt_score = sp - sm;
    return sp > sm;
}
// The actor an enemy targets now (0: none / not resolvable).
static uint64_t enemy_target_actor(uint64_t enemy) {
    uint64_t unit = LD64(enemy + ENEMY_TARGET_LINK);
    uint32_t id = LD32(enemy + ENEMY_TARGET_LINK + 8);
    if (!in_guest(unit) || id == 0xffffffffu || LD32(unit + UNIT_ID) != id) return 0;
    uint64_t a = LD64(unit + UNIT_ACTOR);
    return in_guest(a) ? a : 0;
}
static uint64_t tgt_link_mem;  // a link {unit, id, acquired 0} built from the actor's handle (as PROC_UNIT / PROC_ID)
static void enemy_set_target(Cpu *c, uint64_t enemy, uint64_t target_actor) {
    if (target_actor) {
        if (!tgt_link_mem) tgt_link_mem = (uint64_t)(uintptr_t)hle_alloc(0x20);
        uint64_t unit = LD64(target_actor + PROC_UNIT);
        if (!tgt_link_mem || !in_guest(unit)) return;
        // (the game passes actor+0x68 itself, 0x7000bd4dd0; our own copy only relies on the handle fields the
        // avatar code already validates: unit at actor+0x68, id at actor+0x60 = unit+0x40)
        memset((void *)(uintptr_t)tgt_link_mem, 0, 0x20);
        ST64(tgt_link_mem, unit);
        ST32(tgt_link_mem + 8, LD32(target_actor + PROC_ID));
    }
    Cpu saved = *c;
    c->x[0] = enemy;
    if (target_actor) {
        c->x[1] = tgt_link_mem; c->x[2] = 0; c->x[3] = 0;
        guest_call(c, A_ENEMY_SET_TARGET);
    } else {
        guest_call(c, A_ENEMY_TARGET_PLAYER);
    }
    *c = saved;
}
// ---- (2026-11-18, SWITCHHOLD) Reported: enemies aim at one player while looking at the other when they switch target.
// a two-game test run 1 (LOOKDIAG): the look-at of the side running the enemy always follows its target at once
// (owner samples: look = target), but an attack's direction is fixed when it starts: a change of target in the middle
// of JumpAttack / Cry / GrabAndShoot left the body (and weapon) on the old player for the rest of the action while the
// head turned to the new one, on the owner and so on the follower (which copies both). In the plain game the target
// never changes mid-action. So a change of target waits while the enemy plays a committed action (an attack, a shot, a
// throw, a roar, a hit reaction, a jump or rush), at most SWITCH_HOLD_MAX_NS, and happens at the next free moment.
// SAVAGEWIND_COOP_SWITCHHOLD=0: off (switch at once, as before).
#define SWITCH_HOLD_MAX_NS 4000000000ull
static int switchhold_c = -1;
static bool switchhold_on(void) { return env_on("SAVAGEWIND_COOP_SWITCHHOLD", &switchhold_c); }
static bool committed_as(const char *as) {
    static const char *const sub[] = {"Attack", "Shoot", "Throw", "Arrow", "Bow", "Cry", "Rush", "Jump", "Dash",
                                      "Grab", "Damage", "Swing", "Spin", "Tackle", "Fire", "Breath"};
    for (size_t i = 0; i < sizeof sub / sizeof sub[0]; i++)
        if (strstr(as, sub[i])) return true;
    return false;
}
// true: the change of target waits (called when the target rule wants a change now)
static bool switch_hold(Cpu *c, struct Enemy *e, uint64_t now) {
    if (!switchhold_on()) return false;
    char cur[64];
    player_as_name(c, e->actor, cur, sizeof cur);
    if (!committed_as(cur)) {
        if (e->tgt_wait_ns) LOG("[coop] enemy %s %u: target change after its action (waited %.1f s, AS now %s)\n",
                                e->name, e->hash, (now - e->tgt_wait_ns) / 1e9, cur[0] ? cur : "-");
        e->tgt_wait_ns = 0;
        return false;
    }
    if (!e->tgt_wait_ns) {
        e->tgt_wait_ns = now;
        LOG("[coop] enemy %s %u: target change waits for its action to end (AS %s)\n", e->name, e->hash, cur);
    }
    if (now - e->tgt_wait_ns < SWITCH_HOLD_MAX_NS) return true;
    LOG("[coop] enemy %s %u: target change after waiting %.1f s (AS still %s)\n", e->name, e->hash,
        (now - e->tgt_wait_ns) / 1e9, cur);
    e->tgt_wait_ns = 0;
    return false;
}
// Every frame for an awake enemy this side runs (not following).
static void enemy_target_frame(Cpu *c, struct Enemy *e, uint64_t now) {
    // (run-time enemies: the native side's own AI only, as before - unless SPAWNDEDUP: the host's random spawns are
    // the guest's too then, so they may go for the guest's avatar like placed enemies)
    if (!target_on() || (e->spawned && !(e->native && spawndedup_on()))) return;
    if (now - e->tgt_as_ns > THREAT_AS_NS) {  // (threat: which enemies we run are fighting, for the group spread)
        char as[64];
        player_as_name(c, e->actor, as, sizeof as);
        if (battle_as(as)) e->tgt_battle_ns = now;
        e->tgt_as_ns = now;
    }
    // (n4, 2026-11-17) The other player is the remote avatar nearest to it: the partner's, or with MULTITARGET an extra
    // player's (rav: multi.inc; the one it targets now counts TGT_AV_STICK m nearer, no ping-pong between two).
    // SAVAGEWIND_COOP_MULTITARGET=0: only the partner's avatar (as before n4).
    const struct RemoteAv *cand = NULL;
    float dp = 1e9f, best_rank = 1e9f;
    for (int t = 0; t < rav_n; t++) {
        const struct RemoteAv *ra = &rav[t];
        if ((!ra->partner && !multitarget_on()) || !ra->fresh || !(ra->rs.flags & COOP_FLAG_IN_WORLD) ||
            ra->rs.area != enemy_area || ra->rs.anim == COOP_ANIM_DIE)
            continue;
        float q[3];
        const float *ap = (const float *)(uintptr_t)(ra->actor + ACTOR_MTX);
        float avpos[3] = {ap[3], ap[7], ap[11]};
        float d = actor_dist(e->actor, avpos, q), rank = d - (ra->actor == e->tgt_av ? TGT_AV_STICK : 0);
        if (!cand || rank < best_rank) cand = ra, dp = d, best_rank = rank;
    }
    bool av = cand != NULL;
    uint64_t cav = av ? cand->actor : 0;
    bool want = false;
    if (av) {
        float p[3];
        float dm = actor_dist(e->actor, (const float *)(uintptr_t)(player_info_ptr() + 0x88), p);
        bool their_hit = e->peer_hit_ns && now - e->peer_hit_ns < TGT_HIT_NS;
        bool our_hit = e->hurt_ns && now - e->hurt_ns < TGT_HIT_NS;
        if (dp < TGT_RANGE) {
            if (!threat_on()) {  // (the 2026-11-14 rule: last hitter, else clearly nearer)
                if (their_hit && (!our_hit || e->peer_hit_ns > e->hurt_ns)) want = true;
                else if (!our_hit && !their_hit) want = e->tgt_partner ? dp < dm + TGT_MARGIN : dp + TGT_MARGIN < dm;
            } else {
                want = threat_pick(e, dm, dp, our_hit, their_hit, now);
            }
        }
    }
    bool old_ok = false;  // (n4) the avatar it is steered at is still here
    for (int t = 0; t < rav_n; t++) old_ok |= e->tgt_av && rav[t].actor == e->tgt_av;
    bool switch_av = want && e->tgt_partner && cav != e->tgt_av;  // (n4: from one remote avatar to another)
    if (want != e->tgt_partner || (switch_av && old_ok)) {
        if (now - e->tgt_ns < TGT_HOLD_NS) return;
        if (strncmp(e->name, "Enemy_Octarock", 14) && switch_hold(c, e, now)) return;  // (Octoroks: their own aim rule)
        if (want && rav_n > 1)
            LOG("[coop] enemy %s %u target: player %d's avatar (%s)\n", e->name, e->hash, cand->pid,
                cand->partner ? "the partner" : "an extra player");
        e->tgt_partner = want;
        e->tgt_ns = now;
        LOG("[coop] enemy %s %u target: %s (score %.1f)\n", e->name, e->hash,
            want ? "the other player (avatar)" : "our player", e->tgt_score);
    } else if (e->tgt_wait_ns) {
        e->tgt_wait_ns = 0;  // (SWITCHHOLD: the rule changed its mind while it waited)
    }
    if (e->tgt_partner && want) e->tgt_av = cav, e->tgt_pid = cand->pid;
    uint64_t tav = e->tgt_partner && e->tgt_av ? e->tgt_av : cav;  // (n4) the avatar it goes for
    static int tgttrace = -1;  // SAVAGEWIND_COOP_TGTTRACE=1 (dev): ~10 lines/s per steered-capable enemy
    if (tgttrace < 0) tgttrace = getenv("SAVAGEWIND_COOP_TGTTRACE") && !strcmp(getenv("SAVAGEWIND_COOP_TGTTRACE"), "1");
    static uint64_t tt_ns;
    if (tgttrace && av && now - tt_ns > 100000000ull) {
        tt_ns = now;
        uint64_t ta = enemy_target_actor(e->actor), pa = LD64(player_info_ptr() + 0x60);
        const float *em = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
        const float *am = (const float *)(uintptr_t)(tav + ACTOR_MTX), *pm = (const float *)(uintptr_t)(pa + ACTOR_MTX);
        float ax = am[3] - em[3], az = am[11] - em[11], px = pm[3] - em[3], pz = pm[11] - em[11];
        float al = sqrtf(ax * ax + az * az) + 1e-4f, pl = sqrtf(px * px + pz * pz) + 1e-4f;
        char as[64];
        player_as_name(c, e->actor, as, sizeof as);
        LOG("[coop] tgttrace %u want %s target %s AS %s face->avatar %.2f (%.1f m) face->player %.2f (%.1f m)\n", e->hash,
            e->tgt_partner ? "avatar" : "player", ta == tav ? "AVATAR" : ta == pa ? "PLAYER" : ta ? "other" : "none",
            as, (em[2] * ax + em[10] * az) / al, al, (em[2] * px + em[10] * pz) / pl, pl);
    }
    // (2026-11-15, test_shots runs 9-10) A hiding Octorok whose target is the avatar never attacks: it looks around
    // (Search) and hides again, also when the avatar stands in its 20-35 m range - and then it ignores our Link too.
    // With our Link as its target it notices it and shoots. So an Octorok keeps our Link as target and gets the avatar
    // only while it plays an attack AS (HideAttackStart / HideAttack): its shot aims at the target of that moment
    // (run 12: the rock flew at the avatar). It also never notices the avatar, and after an attack at it hides for
    // good, so while the avatar is its target and stands where it would shoot (OCTO_MIN..OCTO_MAX m, its AI's
    // AttackDistMin / FarDist) it is given our Link as target every OCTO_WAKE_NS ("it noticed someone").
    // SAVAGEWIND_COOP_OCTO_AIM=0: Octoroks are steered like every enemy (and stay hidden); =2 (dev): the avatar is
    // always the wanted target.
    static int octo_aim = -1;
    if (octo_aim < 0) octo_aim = getenv("SAVAGEWIND_COOP_OCTO_AIM") ? atoi(getenv("SAVAGEWIND_COOP_OCTO_AIM")) : 1;
    if (octo_aim && !strncmp(e->name, "Enemy_Octarock", 14)) {
        uint64_t t = enemy_target_actor(e->actor);
        char cur[64];
        player_as_name(c, e->actor, cur, sizeof cur);
        bool wantp = av && (e->tgt_partner || octo_aim == 2);
        if (wantp) {
            const float *em = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
            const float *am = (const float *)(uintptr_t)(tav + ACTOR_MTX);
            float d = sqrtf((am[3] - em[3]) * (am[3] - em[3]) + (am[11] - em[11]) * (am[11] - em[11]));
            wantp = d >= OCTO_MIN && d <= OCTO_MAX;
        }
        bool atk = wantp && strstr(cur, "Attack") && !strstr(cur, "AttackEnd");
        if (atk && t != tav) {
            enemy_set_target(c, e->actor, tav);
            e->octo_atk_ns = now;
            LOG("[coop] enemy %s %u: attack (AS %s) aimed at the avatar\n", e->name, e->hash, cur);
        } else if (!atk && av && rav_has(t) && (!wantp || now - e->octo_atk_ns > OCTO_REST_NS)) {
            enemy_set_target(c, e->actor, 0);
        } else if (wantp && !atk && t != tav && now - e->octo_wake_ns > OCTO_WAKE_NS &&
                   now - e->octo_atk_ns > OCTO_REST_NS) {
            e->octo_wake_ns = now;
            enemy_set_target(c, e->actor, 0);
        }
        return;
    }
    if (!e->tgt_partner) {
        // (also after a take-over: the AI may still hold the avatar we gave it before the enemy changed hands)
        if (av && rav_has(enemy_target_actor(e->actor))) {
            char ca[64];  // (SWITCHHOLD: not in the middle of an action, as for a change of target above)
            if (switchhold_on() && (player_as_name(c, e->actor, ca, sizeof ca), committed_as(ca)) &&
                now - e->tgt_ns < SWITCH_HOLD_MAX_NS)
                return;
            enemy_set_target(c, e->actor, 0);
        }
        return;
    }
    if (enemy_target_actor(e->actor) == tav) return;
    char cur[64];
    player_as_name(c, e->actor, cur, sizeof cur);
    if (asleep_as(cur)) return;  // (asleep: leave it; it targets whoever wakes it)
    // (softlock, 2026-11-14: TARGET_CALM. The play-test logs showed aims in AS Wait/EatMeat/"" and fast Turn <->
    // BattleWait while steered: written every frame, our target fought the AI's own choice. Now only a fighting enemy
    // is steered, and the target is written again at most every 0.5 s; how often the AI put its own back is logged.)
    if (target_calm_on()) {
        if (!battle_as(cur)) return;
        if (now - e->tgt_set_ns < TGT_RESET_NS) return;
        if (e->tgt_set_ns && now - e->tgt_set_ns < 3000000000ull) e->tgt_overrides++;  // (the AI replaced ours)
    }
    // (SWITCHHOLD) the AI put its own target back and started an action at it: not turned in the middle of it
    if (switchhold_on() && committed_as(cur) && e->tgt_set_ns && now - e->tgt_set_ns < SWITCH_HOLD_MAX_NS) return;
    enemy_set_target(c, e->actor, tav);
    e->tgt_set_ns = now;
    if (now - e->tgt_log_ns > 2000000000ull) {
        e->tgt_log_ns = now;
        LOG("[coop] enemy %s %u now aims at the avatar (AS %s; the AI replaced it %u time(s) since the last line)\n",
            e->name, e->hash, cur, e->tgt_overrides);
        e->tgt_overrides = 0;
    }
}
static struct { volatile uint64_t actor; int mode; } target_dev;  // `coop target` (dev): 0 show, 1 avatar, 2 player
static struct { volatile uint64_t actor; int damage, kind; } inject_dev;
static volatile int equip_dev;  // `coop equip` (dev)
static volatile uint64_t as_dev_actor;  // `coop as <actor>` (dev): the AS it plays and its output signals, to the log

// Who runs an enemy's AI (2026-11-08): the side whose player is nearer, so enemies fight whoever is next to them.
// The host decides (guest_runs, sent as COOP_ENEMY_GUEST_RUNS) with hysteresis: the guest must be OWNER_MARGIN m
// nearer than the host to take it, and it goes back when the host is that much nearer, at most every OWNER_HOLD_NS.
// The guest takes it only while the guest reports it loaded; the host takes it back when the guest stops sending
// where it is. SAVAGEWIND_COOP_HANDOFF=0: the host always runs every enemy (the old behaviour).
#define OWNER_MARGIN 6.0f
#define OWNER_HOLD_NS 3000000000ull
// Combat ownership (2026-11-14, SAVAGEWIND_COOP_AGGRO=0: the rule above only): the last hitter, else the rule above.
// (A 3 m / 2 s nearer rule was tried: a guest dodging around its enemy made it change hands every 2 s.)
#define AGGRO_NS 10000000000ull           // a hit counts this long ("it is angry at whoever hit it")
#define AGGRO_RANGE 30.0f                 // ... while the hitter stays this close to it
#define AGGRO_HOLD_NS 1200000000ull       // at most one switch per 1.2 s (both players hitting it in turn)
#define FIGHTSWAP_NEAR 5.0f               // (FIGHTSWAP, below) the other player this close to the enemy ...
#define FIGHTSWAP_MARGIN 4.0f             // ... and this much nearer than the player whose game runs it ...
#define FIGHTSWAP_NS 1500000000ull        // ... for this long
#define FIGHTSWAP_HOLD_NS 4000000000ull   // at most one such hand-over per 4 s
// (2026-11-15: switching the AI between games felt weird in a fight, and an enemy could only pay attention
// to 1 player) The aggro hand-over is now OFF by default (SAVAGEWIND_COOP_AGGRO=1 turns it back
// on): an enemy stays with the game that runs it for the whole fight (BATTLE_HOLD; it moves only when its player is
// OWNER_FAR away), the other player's hits reach it as received hits, and whom it attacks is chosen by
// enemy_target_frame between both players (threat score), not by changing hands.
static bool aggro_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_COOP_AGGRO") && !strcmp(getenv("SAVAGEWIND_COOP_AGGRO"), "1");
    return on;
}
static bool handoff_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_HANDOFF") && !strcmp(getenv("SAVAGEWIND_COOP_HANDOFF"), "0"));
    return on;
}
// (58, host, SAVAGEWIND_COOP_MULTIWORLD) With more than one guest sharing the world, the rule below weighs us against
// one guest: the one running it (while it fights, or until another is OWNER_MARGIN m nearer, at most every
// OWNER_HOLD_NS), else the nearest world guest that has it loaded. A switch between two guests happens here directly.
// Returns that guest's id (-1: none has it loaded). With two players it is always the partner.
static int enemy_pick_guest(struct Enemy *e, uint32_t worlds, uint64_t now) {
    int best = -1, cur = e->guest_runs ? e->owner_pid : e->cand_pid;
    float bd = 1e30f, cd = 1e30f;
    for (int g = 1; g < 8; g++) {
        if (!(worlds & (1u << g))) continue;
        uint32_t fl = 0;
        double fage = 99, rage = 99;
        if (!coop_net_enemy_flags_of(g, e->hash, &fl, &fage) || fage >= 3.0 || !(fl & COOP_ENEMY_LOADED)) continue;
        CoopState rs;
        coop_net_view(g);
        bool ok = coop_net_get_remote(&rs, &rage) && rage <= 1.0 && (rs.flags & COOP_FLAG_IN_WORLD);
        coop_net_view(-1);
        if (!ok) continue;
        float p[3], d = actor_dist(e->actor, rs.pos, p);
        if (d < bd) bd = d, best = g;
        if (g == cur) cd = d;
    }
    if (best < 0) return -1;
    int want = cur;
    bool cur_ok = cur > 0 && cd < 1e29f;
    bool fighting = e->guest_runs && e->fight_ns && now - e->fight_ns < FIGHT_NS && cd < OWNER_FAR;
    if (!cur_ok) want = best;
    else if (best != cur && !fighting && now - e->cand_ns > OWNER_HOLD_NS && bd + OWNER_MARGIN < cd) want = best;
    if (want != cur) {
        e->cand_ns = now;
        if (e->guest_runs) {
            LOG("[coop] enemy %s %u: player %d runs it now (was player %d)\n", e->name, e->hash, want, cur);
            e->owner_pid = (int8_t)want, e->owner_ns = now;
        }
    }
    e->cand_pid = (int8_t)want;
    return want;
}
static void enemy_owner(Cpu *c, struct Enemy *e, uint64_t now) {
    uint32_t flags = 0;
    double age = 99;
    // (58) the guest weighed against us: the partner, or with more world guests the one enemy_pick_guest names
    int gid = coop_net_is_host() ? coop_net_partner_id() : 0;
    uint32_t worlds = coop_net_is_host() ? coop_net_world_players(1.0) & ~1u : 0;
    bool multi = gid > 0 ? (worlds & ~(1u << gid)) != 0 : worlds != 0;
    if (multi) gid = enemy_pick_guest(e, worlds, now);
    bool known = multi ? gid > 0 && coop_net_enemy_flags_of(gid, e->hash, &flags, &age)
                       : coop_net_remote_enemy_flags(e->hash, &flags, &age);
    bool heard = known && age < ENEMY_FOLLOW_MAX_AGE;
    // (host: the guest's packet holds 16 enemies; one missing for a moment must not move the enemy back: in a soak
    // run with ~15 enemies loaded, Chuchus at the edge of that list changed hands 9 times in 3 minutes)
    bool heard_long = known && age < 3.0;
    if (!coop_net_is_host()) {  // guest: ours when the host says so, or when the host does not have it
        bool host_runs = heard && (flags & COOP_ENEMY_LOADED) && !(flags & COOP_ENEMY_GUEST_RUNS);
        // (n6, HOLDFOLLOW) 3+ players: a copy we follow stays followed while its entry is missing from the host's
        // packet for up to 3 s. The 3-player test showed player 1 taking an Octorok player 2 ran for one packet
        // gap at a time ("this side runs it now" then "follows" again, 4 times in 40 s): two runners for a moment.
        static int holdfollow_c = -1;
        uint32_t others = coop_net_players(5.0);
        if (!host_runs && !e->mine && heard_long && (flags & COOP_ENEMY_LOADED) && !(flags & COOP_ENEMY_GUEST_RUNS) &&
            (others & (others - 1)) && env_on("SAVAGEWIND_COOP_HOLDFOLLOW", &holdfollow_c)) {
            host_runs = true;
            static uint64_t said_ns;
            if (now - said_ns > 2000000000ull)
                said_ns = now, LOG("[coop] enemy %s %u: not in the host's packet for %.1f s: still followed\n", e->name,
                                   e->hash, age);
        }
        e->mine = !host_runs;
        return;
    }
    bool was = e->guest_runs;
    CoopState rs;
    double rage = 99;
    float me[3], p[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    bool got = false;
    if (gid > 0) {
        if (multi) coop_net_view(gid);
        got = coop_net_get_remote(&rs, &rage);
        coop_net_view(-1);
    }
    if (!handoff_on() || !got || rage > 1.0 || !(rs.flags & COOP_FLAG_IN_WORLD) ||
        !(heard_long && (flags & COOP_ENEMY_LOADED))) {
        e->guest_runs = false;  // (the guest is not there, or does not have it loaded)
    } else if (now >= e->no_handoff_until) {
        // (only enemies with a physics body: a dormant one - a Chuchu still underground: life 1, no AS, no body -
        // has no position to send, and changed hands every 3 s in a soak run)
        float dh = actor_dist(e->actor, me, p), dg = actor_dist(e->actor, rs.pos, p);
        bool body = enemy_body(e->actor) != 0;
        int want = -1;  // aggro: -1 no recent hit, 0 host, 1 guest
        if (aggro_on()) {
            // Combat (2026-11-14): the enemy fights whoever hit it last (within AGGRO_NS, hitter within
            // AGGRO_RANGE); otherwise the distance rule. The game's enemy AI only knows its own game's Link, so
            // "who runs it" IS "whom it attacks": before this, a camp fought by both players was run by the host
            // (the guest had to be 6 m nearer) and every enemy chased the host.
            uint64_t gh = e->peer_hit_ns, hh = e->hurt_ns;  // (host side: peer = the guest)
            bool g_recent = gh && now - gh < AGGRO_NS && dg < AGGRO_RANGE;
            bool h_recent = hh && now - hh < AGGRO_NS && dh < AGGRO_RANGE;
            if (g_recent && (!h_recent || gh > hh)) want = 1;
            else if (h_recent) want = 0;
            if (want >= 0 && now - e->owner_ns > AGGRO_HOLD_NS && (bool)want != e->guest_runs && (!want || body))
                e->guest_runs = want, e->aggro_handoff = want;
        }
        // (softlock, 2026-11-14: BATTLE_HOLD) While it fights (the side running it showed a battle AS in the last
        // FIGHT_NS), the distance rule does not move it unless its player is OWNER_FAR away: the new side's frozen AI
        // would wake in its pre-fight state (seen: archers shooting at the guest went idle for ~25 s when the host
        // walked up). Whom it attacks is then the target rule's job (enemy_target_frame), not a change of hands.
        bool fighting = false;
        if (battle_hold_on()) {
            char as[64] = "";
            float q[3], r[4];
            double qage = 99;
            if (!e->guest_runs) player_as_name(c, e->actor, as, sizeof as);
            else if (!(coop_net_remote_enemy_pos(e->hash, q, r, &qage, as, sizeof as) && qage < ENEMY_FOLLOW_MAX_AGE))
                as[0] = 0;
            if (battle_as(as)) e->fight_ns = now;
            fighting = e->fight_ns && now - e->fight_ns < FIGHT_NS && (e->guest_runs ? dg : dh) < OWNER_FAR;
        }
        // (0.1.1, FIGHTSWAP. Playtest 2026-11-26: the enemy only seemed to switch when attacked by the
        // other player, maybe by proximity. The logs showed why: the side running a fighting enemy points it at
        // the avatar, but its AI puts its own Link back 8-13 times in 2 s, and in a fight it changed hands only at
        // OWNER_FAR.) In a fight, the enemy moves to the other player's game when that player is the clearly nearer
        // one - within FIGHTSWAP_NEAR m of it and FIGHTSWAP_MARGIN m nearer than the player whose game runs it, for
        // FIGHTSWAP_NS - at most once per FIGHTSWAP_HOLD_NS. Its own AI there then fights the nearest Link (the
        // take-over wakes it as for any hand-over). SAVAGEWIND_COOP_FIGHTSWAP=0: off (BATTLE_HOLD as before).
        static int fightswap_c = -1;
        if (want < 0 && fighting && env_on("SAVAGEWIND_COOP_FIGHTSWAP", &fightswap_c)) {
            float own = e->guest_runs ? dg : dh, other = e->guest_runs ? dh : dg;
            if (!(other < FIGHTSWAP_NEAR && other + FIGHTSWAP_MARGIN < own) || (!e->guest_runs && !body)) {
                e->swap_ns = 0;
            } else if (!e->swap_ns) {
                e->swap_ns = now;
            } else if (now - e->swap_ns > FIGHTSWAP_NS && now - e->owner_ns > FIGHTSWAP_HOLD_NS) {
                e->swap_ns = 0;
                e->guest_runs = !e->guest_runs, e->aggro_handoff = e->guest_runs;
                LOG("[coop] enemy %s %u: in a fight, the %s is the nearer player (%.1f m vs %.1f m): hand-over\n",
                    e->name, e->hash, e->guest_runs ? "guest" : "host", other, own);
            }
        }
        if (want < 0 && !fighting && now - e->owner_ns > OWNER_HOLD_NS) {
            if (!e->guest_runs && dg + OWNER_MARGIN < dh && body) e->guest_runs = true, e->aggro_handoff = false;
            else if (e->guest_runs && dh + OWNER_MARGIN < dg) e->guest_runs = false;
        }
    }
    // The guest took it but sends no position: it does not run it (yet, or any more). After a grace period, back,
    // and no new try for 30 s.
    float gp[3], gr[4];
    double gage = 99;
    if (was && e->guest_runs && now - e->owner_ns > 2000000000ull &&  // (was: owner_ns is this hand-over's time)
        !(coop_net_remote_enemy_pos(e->hash, gp, gr, &gage, NULL, 0) && gage < ENEMY_FOLLOW_MAX_AGE)) {
        e->guest_runs = false;
        e->no_handoff_until = now + 30000000000ull;
    }
    // Asleep after a hit (2026-11-14): the guest's copy of an enemy was frozen since it loaded; when a hit hands it
    // over, its own AI sometimes stays asleep (AS Hide) and ignores even further sword hits (2 of 4 runs of
    // test_enemy_aggro/probe_aggro; cause not found). A hit always wakes an enemy, so: still asleep 1.5 s after an
    // aggro hand-over -> back to the host (its AI was awake), no new hand-over for 30 s (old behaviour: the host's
    // enemy takes the guest's hits as received hits).
    char gas[32] = "";
    if (was && e->guest_runs && e->aggro_handoff && now - e->owner_ns > 1500000000ull &&
        now - e->owner_ns < 3000000000ull && coop_net_remote_enemy_pos(e->hash, gp, gr, &gage, gas, sizeof gas) &&
        asleep_as(gas)) {
        e->guest_runs = false;
        e->no_handoff_until = now + 30000000000ull;
        LOG("[coop] enemy %s %u still asleep (%s) in the guest's game after its hit: back to the host\n", e->name,
            e->hash, gas);
    }
    if (e->guest_runs && !was) e->owner_pid = (int8_t)gid;
    if (e->guest_runs != was) {
        e->owner_ns = now;
        if (multi && e->guest_runs) LOG("[coop] enemy %s %u: player %d runs it now\n", e->name, e->hash, gid);
        else if (multi) LOG("[coop] enemy %s %u: the host runs it now\n", e->name, e->hash);
        else LOG("[coop] enemy %s %u: %s runs it now\n", e->name, e->hash, e->guest_runs ? "the guest" : "the host");
    }
    e->mine = !e->guest_runs;
}
