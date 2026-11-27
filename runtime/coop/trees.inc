// Part of runtime/coop.c (included there, same translation unit): trees cut down in one game fall in the other too.
// (2026-11-15, play report: "se eu corto uma árvore deveria ser cortada no outro jogo e vice versa"; protocol 54)
//
// RE (offline, not yet probed live): a placed tree is an actor with a map
// HashId and the AI class LumberjackTree (also signboards / scaffolds); ai = FUN 0x7000951a50(actor) (vtable
// 0x7001c71338). ai+0x24c byte = 1 once it fell (written only by the fell); ai+0x2b8 -> float[3] fall direction;
// ai+0x2dc byte = felled at once (bomb); ai+0x38 = the cut-state object. Felling = FUN 0x700095157c(ai): it spawns the
// stump and the log itself (it returns at once when actor+0x694 != 0). Trees have no save flag: a cut tree grows back
// when its area reloads, so only the live fall is shared.
// Sender: every 8 frames the trees within TREE_NEAR_M of our Link are looked at; a fell flag going 0 -> 1 is sent
// (unless we felled it for the other player). Receiver: the same tree (HashId) is felled the game's way; a tree not
// loaded here yet waits in a short list. Drops (branches, apples) come from each game's own tree.
// SAVAGEWIND_COOP_TREES=0: off. Dev: `coop tree` (nearest tree's fields) / `coop treecut` (fell the nearest tree).
#define A_TREE_AI 0x7000951a50ull      // (x0 tree actor) -> the LumberjackTree AI, or 0
#define A_TREE_FELL 0x700095157cull    // (x0 ai)
#define A_TREE_HIT 0x7001164cb4ull     // LumberjackTree's cut marks and hit reaction
#define TREE_AI_VT 0x7001c71338ull
// (2026-11-15 play: an axe cut in a dense forest was never seen - with 80 m and 384 slots the list filled up with
// other trees before the one cut 3 m away. A cut is made right next to our Link: 35 m is plenty.)
#define TREE_NEAR_M 80.0f  // (2026-11-15 22:20 probe: 30 tree actors within 70 m, list was never full; 80 m so a partner watching from afar retries a refused fell too)
#define TREE_MAX 768
#define TREE_PENDING 32

static bool trees_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_TREES");
    return on;
}
static bool tree_hits_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_TREEHITS");
    return on;
}
static bool tree_name(const char *n) {
    if (strncmp(n, "Obj_Tree", 8) && strncmp(n, "FldObj_SignboardWood_", 21) && strncmp(n, "FldObj_ScaffoldWood_", 20) &&
        strncmp(n, "FldObj_EnemyLookoutBanana", 25) && strncmp(n, "FldObj_RockLayerMossTree", 24))
        return false;
    return !strstr(n, "_Stump") && !strstr(n, "_Trunk");
}
struct Tree { uint64_t actor, ai; uint32_t hash; uint8_t fell; float x, z; int32_t life; };
static struct Tree trees[TREE_MAX], trees_new[TREE_MAX];
static int tree_n, tree_new_n;
static uint32_t tree_applied[64];  // hashes we felled for the other player (no echo)
static int tree_applied_next;
static struct { uint32_t hash; float dir[3]; uint8_t instant; uint64_t until_ns, fail_ns; } tree_wait[TREE_PENDING];
static struct { CoopTreeHit hit; uint64_t until_ns; } tree_hits[TREE_PENDING];
static int tree_hit_n;
static uint64_t tree_hit_mem;

static uint64_t tree_ai(Cpu *c, uint64_t actor) {
    Cpu saved = *c;
    c->x[0] = actor;
    guest_call(c, A_TREE_AI);
    uint64_t ai = c->x[0];
    *c = saved;
    return in_guest(ai) && LD64(ai) == TREE_AI_VT ? ai : 0;
}
static float tree_me[3];
// (2026-11-15 probe: 15 trees watched in that forest, yet the player's axe cut was never sent. Likely: a real cut swaps
// the tree for stump + log within a few frames, so the 8-frame scan never sees the fell flag on the tree actor.) A new
// stump within 2.5 m of a tree watched in the last scan is a cut of that tree too (once per tree: tree_sent).
static struct { uint64_t actor; uint32_t id; float x, z; } stump_new[16];
static uint32_t stumps_seen[64];
static int stump_new_n, stumps_next;
static uint32_t tree_sent[32];
static int tree_sent_next;
static bool tree_was_sent(uint32_t h) {
    for (int i = 0; i < 32; i++)
        if (tree_sent[i] == h) return true;
    return false;
}
// (2026-11-15 play, 21:40: in a forest only 1-2 trees were watched and an axe cut was never seen. Counters for the
// play log: what the scan found and why trees were left out; a stump that appears near us is logged, it proves a cut.)
static struct { int names, far, noai, nohash; char noai_name[48]; } tree_diag;
static bool tree_or_stump(const char *n) { return tree_name(n) || (!strncmp(n, "Obj_Tree", 8) && strstr(n, "_Stump")); }
static void tree_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)ctx;
    float dx = LDF32(a + ACTOR_MTX + 12) - tree_me[0], dz = LDF32(a + ACTOR_MTX + 44) - tree_me[2];
    if (strstr(name, "_Stump")) {
        if (dx * dx + dz * dz > TREE_NEAR_M * TREE_NEAR_M || stump_new_n == 16) return;
        uint32_t id = LD32(a + PROC_ID);
        for (int i = 0; i < 64; i++)  // (64: an area with many old stumps must not cycle them back as new)
            if (stumps_seen[i] == id) return;
        stump_new[stump_new_n].actor = a;
        stump_new[stump_new_n].id = id;
        stump_new[stump_new_n].x = LDF32(a + ACTOR_MTX + 12);
        stump_new[stump_new_n++].z = LDF32(a + ACTOR_MTX + 44);
        return;
    }
    tree_diag.names++;
    if (dx * dx + dz * dz > TREE_NEAR_M * TREE_NEAR_M) { tree_diag.far++; return; }
    if (tree_new_n >= TREE_MAX) {
        static int said;
        if (!said++) LOG("[coop] trees: more than %d trees near us, some not watched\n", TREE_MAX);
        return;
    }
    struct Tree t = {.actor = a};
    for (int i = 0; i < tree_n; i++)  // (known from the last scan: no game calls)
        if (trees[i].actor == a) { t = trees[i]; break; }
    if (!t.ai) {
        t.ai = tree_ai(c, a);
        if (!t.ai) { tree_diag.noai++, snprintf(tree_diag.noai_name, sizeof tree_diag.noai_name, "%s", name); return; }
        t.hash = actor_hash(c, a);
        if (!t.hash) { tree_diag.nohash++; return; }
        t.fell = LD8(t.ai + 0x24c);  // (already down when we first see it: not ours to send)
    }
    t.x = LDF32(a + ACTOR_MTX + 12), t.z = LDF32(a + ACTOR_MTX + 44);
    trees_new[tree_new_n++] = t;
}
static bool tree_was_applied(uint32_t h) {
    for (int i = 0; i < 64; i++)
        if (tree_applied[i] == h) return true;
    return false;
}
// Fell this tree the game's way. False: not possible here (already down, no cut state, busy).
static bool tree_fell(Cpu *c, const struct Tree *t, const float dir[3], uint8_t instant, bool for_partner) {
    if (!in_guest(t->actor) || LD64(t->ai) != TREE_AI_VT || LD8(t->ai + 0x24c) || LD32(t->actor + 0x694) ||
        !in_guest(LD64(t->ai + 0x38)))
        return false;
    uint64_t d = LD64(t->ai + 0x2b8);
    if (in_guest(d) && (dir[0] != 0 || dir[2] != 0)) memcpy((void *)(uintptr_t)d, dir, 12);
    ST8(t->ai + 0x2dc, instant);
    if (for_partner) tree_applied[tree_applied_next] = t->hash, tree_applied_next = (tree_applied_next + 1) % 64;
    Cpu saved = *c;
    c->x[0] = t->ai;
    guest_call(c, A_TREE_FELL);
    *c = saved;
    return true;
}
static const char *tree_why_not(const struct Tree *t) {
    if (!in_guest(t->actor) || LD64(t->ai) != TREE_AI_VT) return "tree gone";
    if (LD8(t->ai + 0x24c)) return "already down";
    if (LD32(t->actor + 0x694)) return "tree busy";
    if (!in_guest(LD64(t->ai + 0x38))) return "no cut state yet";
    return "?";
}
static struct Tree *tree_by_hash(uint32_t h) {
    for (int i = 0; i < tree_n; i++)
        if (trees[i].hash == h) return &trees[i];
    return NULL;
}

static uint64_t tree_life(Cpu *c, const struct Tree *t) {
    if (LD8(t->ai + 0x24c)) return 0;
    uint64_t vt = LD64(t->actor);
    uint64_t fn = in_guest(vt + A_ACTOR_LIFE_VF) ? LD64(vt + A_ACTOR_LIFE_VF) : 0;
    if (fn < 0x7000000000ull || fn >= 0x7003000000ull) return 0;
    Cpu saved = *c;
    c->x[0] = t->actor;
    guest_call(c, fn);
    uint64_t life = c->x[0];
    *c = saved;
    return life >= t->actor && life < t->actor + 0x2000 && in_guest(life + 3) ? life : 0;
}

static int32_t tree_max_life(Cpu *c, const struct Tree *t) {
    // LumberjackTree divides the damage by the actor's maximum life (vfunc +0xf0).
    uint64_t fn = LD64(LD64(t->actor) + 0xf0);
    if (fn < 0x7000000000ull || fn >= 0x7003000000ull) return 0;
    Cpu saved = *c;
    c->x[0] = t->actor;
    guest_call(c, fn);
    int32_t max = (int32_t)c->x[0];
    *c = saved;
    return max;
}

static bool tree_hit_reaction(Cpu *c, const struct Tree *t, int32_t damage, const float from[2]) {
    uint64_t cut = LD64(t->ai + 0x38);
    if (LD8(t->ai + 0x240) || LD32(t->actor + 0x694) || !in_guest(cut + 0x38) || LD64(cut + 0x38) != t->actor)
        return false;
    uint64_t stump = LD64(t->ai + 0x1f0), trunk = LD64(t->ai + 0x198);
    if (!in_guest(stump) || !in_guest(trunk)) return false;
    int32_t max = tree_max_life(c, t);
    if (max <= 0 || damage > max) return false;
    if (!tree_hit_mem) tree_hit_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
    if (!tree_hit_mem) return false;
    float pos[3] = {from[0], LDF32(t->actor + ACTOR_MTX + 28), from[1]};
    memcpy((void *)(uintptr_t)tree_hit_mem, pos, sizeof pos);
    float ratio = (float)damage / (float)max;
    Cpu saved = *c;
    memset(&c->v[0], 0, 16);
    memcpy(&c->v[0], &ratio, sizeof ratio);
    c->x[0] = cut; c->x[1] = tree_hit_mem; c->x[2] = 0;  // normal cut, no automatic fell
    c->x[3] = stump; c->x[4] = trunk; c->x[5] = 0;       // no local attacker for the effect
    c->x[6] = 1; c->x[7] = 0;
    guest_call(c, A_TREE_HIT);
    *c = saved;
    return true;
}

static void tree_hit_ev(const uint8_t *data, int len) {
    if (!trees_on() || !tree_hits_on() || !local_in_world() || len != (int)sizeof(CoopTreeHit)) return;
    CoopTreeHit hit;
    memcpy(&hit, data, sizeof hit);
    char area[96];
    if (!coop_tree_hit_ok(&hit) || hit.area != local_area(area, sizeof area) || tree_hit_n == TREE_PENDING) return;
    tree_hits[tree_hit_n].hit = hit;
    tree_hits[tree_hit_n++].until_ns = plat_now_ns() + 1000000000ull;
}

// Compare consecutive scans, then apply each waiting hit to the freshly found actor.
static void tree_hits_scan(Cpu *c, struct Tree *t, uint32_t here, uint64_t now) {
    uint64_t ptr = tree_life(c, t);
    int32_t life = ptr ? (int32_t)LD32(ptr) : 0;
    if (life <= 0 || life > COOP_TREE_LIFE_MAX) { t->life = 0; return; }
    if (t->life > life) {
        CoopTreeHit hit = {t->hash, here, life, {tree_me[0], tree_me[2]}};
        if (coop_tree_hit_ok(&hit) && coop_net_send_event(COOP_EV_TREE_HIT, &hit, sizeof hit))
            LOG("[coop] tree hits: %u life %d -> %d: sent\n", t->hash, t->life, life);
    }
    for (int i = 0; i < tree_hit_n; i++) {
        CoopTreeHit *hit = &tree_hits[i].hit;
        if (hit->hash != t->hash || hit->area != here || tree_hits[i].until_ns <= now) continue;
        float dx = hit->from[0] - t->x, dz = hit->from[1] - t->z;
        if (hit->life < life && dx * dx + dz * dz <= TREE_NEAR_M * TREE_NEAR_M) {
            ST32(ptr, hit->life);
            bool reaction = tree_hit_reaction(c, t, life - hit->life, hit->from);
            LOG("[coop] tree hits: %u life %d -> %d from partner (reaction %s)\n", t->hash, life, hit->life,
                reaction ? "called" : "unavailable");
            life = hit->life;
        }
        hit->hash = 0;
    }
    t->life = life;  // the next scan starts from the applied life, so it cannot echo the hit
}

// EV 18 from the other game (gate.inc world_event, game thread).
static void tree_ev(Cpu *c, const uint8_t *data, int len) {
    if (!trees_on() || len != (int)sizeof(CoopTree)) return;
    CoopTree e;
    memcpy(&e, data, sizeof e);
    if (!coop_tree_ok(&e)) { LOG("[coop] trees: bad event dropped\n"); return; }
    char area[96];
    if (e.area != local_area(area, sizeof area)) { LOG("[coop] trees: %u cut in another area: ignored\n", e.hash); return; }
    struct Tree *t = local_in_world() ? tree_by_hash(e.hash) : NULL;
    static struct Tree far;  // (loaded here but outside our watched circle: looked up by HashId)
    if (!t && local_in_world() && player_info_ptr()) {
        struct HashHit hit = {e.hash, 0};
        walk_actors(c, tree_name, find_hashid, &hit);
        if (hit.a && (far.ai = tree_ai(c, hit.a))) far.actor = hit.a, far.hash = e.hash, far.fell = 0, t = &far;
    }
    if (t && tree_fell(c, t, e.dir, e.instant, true)) {
        t->fell = 1;
        LOG("[coop] trees: %u cut by the other player: felled here too\n", e.hash);
        return;
    }
    if (t && LD8(t->ai + 0x24c)) { LOG("[coop] trees: %u cut by the other player: already down here\n", e.hash); return; }
    for (int i = 0; i < TREE_PENDING; i++)
        if (!tree_wait[i].hash || plat_now_ns() > tree_wait[i].until_ns) {
            tree_wait[i].hash = e.hash, tree_wait[i].instant = e.instant;
            memcpy(tree_wait[i].dir, e.dir, 12);
            tree_wait[i].until_ns = plat_now_ns() + 120000000000ull, tree_wait[i].fail_ns = 0;
            LOG("[coop] trees: %u cut by the other player: %s, waits\n", e.hash,
                t ? tree_why_not(t) : "not loaded here / not near us yet");
            return;
        }
}

// ---- felled trunks (2026-11-18: sync tree trunks as well; protocol 67). The fell above
// makes each game spawn its own trunk (<tree>_Trunk, made standing where the tree stood) with its own physics, so it
// came to rest in a different place in each game - or (2026-11-18 probe, screenshots) kept standing in the game that
// felled the tree for the other player (its body never put into the physics world) while the other one lay down. Now one game owns each trunk: first the one whose player cut
// the tree, later whichever game's player moves it (claim generations, as the shared objects). The owner sends the
// trunk's body pose (COOP_EV_TRUNK) every TRUNK_SEND_NS while it moves and once more when it rests; the other game
// holds its own trunk of the same tree there with the game's RigidBody setTransform + setLinearVelocity, and puts it
// at the rest pose at the end. When the trunk is cut into pieces in one game (its actor goes away while the tree's
// stump stays), the other game breaks its copy too (GONE). SAVAGEWIND_COOP_TRUNKWOOD=0 skips the wood drop.
// SAVAGEWIND_COOP_TRUNKS=0: off.
#define TRUNK_MAX 16
#define TRUNK_SEND_NS 33000000ull  // owner: a pose ~30 Hz while it moves (so falling rotation does not hitch)
#define TRUNK_REST_NS 1000000000ull  // owner: still (< TRUNK_STILL_MS) this long = resting
#define TRUNK_STILL_MS 0.08f
#define TRUNK_CLAIM_M 0.6f  // follower: our trunk ended this far from where we put it: our player moved it
#define TRUNK_CALM_NS 2000000000ull  // after a change of hands: no claim of ours
static struct Trunk {
    uint32_t hash;
    uint64_t actor, rb, body, unit;
    uint32_t unit_id;
    int owner;  // 1 us, 2 the other player
    uint16_t gen;
    bool have_rx, rx_applied, rest_sent, have_put, gone_sent, enable_asked;
    int tip;  // (dev `coop trunk tip`: frames left to tip it over)
    int hold;  // frames left to keep putting ours at their rest pose (the standing actor puts it back)
    bool hold_said, native_drop;
    CoopTrunk rx, prev_rx;  // the owner's last two poses (for smooth angular extrapolation)
    uint64_t rx_ns, prev_rx_ns, sent_ns, still_ns, changed_ns, found_ns, kill_ns;
    float put[3], sent_pos[3];
} trunks[TRUNK_MAX];
static CoopTrunk trunk_early[TRUNK_MAX];  // poses for a trunk not here yet (newest per tree)
static bool trunks_on(void) {
    static int on = -1;
    if (on < 0) on = trees_on() && look_on("SAVAGEWIND_COOP_TRUNKS"), LOG("[coop] trunks: shared felled trunks %s\n", on ? "on" : "off");
    return on;
}
// (2026-11-18 probe: the body's own pose (+0x1a0) is 0 while the trunk still stands where the tree was, its body
// not in the physics world yet (wrapper flags +0x68 without 0x8); the actor matrix is the body's pose once it is.)
static void trunk_pose(const struct Trunk *t, float pos[3], float rot[4]) {
    const float *m = (const float *)(uintptr_t)(t->actor + ACTOR_MTX);
    pos[0] = m[3], pos[1] = m[7], pos[2] = m[11];
    mtx_quat(m, rot);
}
// The trunk's body is in the physics world (it fell, or was hit): wrapper flags 0x8 (0x190 standing, 0x198 after).
static bool trunk_active(const struct Trunk *t) { return LD32(t->rb + 0x68) & 0x8; }
// The game's RigidBody enable (its trunk placement calls it after setTransform: FUN_700116535c): into the world.
#define A_RB_ENABLE 0x70012d1fa4ull
static void rb_enable(Cpu *c, uint64_t rb) {
    Cpu saved = *c;
    c->x[0] = rb;
    guest_call(c, A_RB_ENABLE);
    *c = saved;
}
// Trees that fell near us lately (where they stood): a new <tree>_Trunk there is that tree's trunk. (The tree's cut
// state only holds the trunk for the one frame it places it: L+0x48 is 0 again after.)
static struct { uint32_t hash; float x, z; uint64_t ns; } trunk_felled[32];
static void trunk_tree_fell(uint32_t hash, float x, float z) {
    int slot = 0;
    for (int i = 0; i < 32; i++) {
        if (trunk_felled[i].hash == hash) { slot = i; break; }
        if (trunk_felled[i].ns < trunk_felled[slot].ns) slot = i;
    }
    trunk_felled[slot].hash = hash, trunk_felled[slot].x = x, trunk_felled[slot].z = z, trunk_felled[slot].ns = plat_now_ns();
}
static bool trunk_alive(const struct Trunk *t) {
    if (!in_guest(t->actor) || !in_guest(t->unit) || LD32(t->unit + UNIT_ID) != t->unit_id ||
        LD64(t->unit + UNIT_ACTOR) != t->actor || LD8(t->actor + PROC_STATE) == 3)
        return false;
    return t->rb && LD64(t->rb + 0x78) == t->body && in_guest(t->body) && LD64(t->body) == RIGID_BODY_VT;
}

// BOTW 1.9.0: LumberjackFallenTree only checks life in its log child. A co-op-felled trunk can also have an
// inactive AI, so keep the native drop sequence as a fallback when there is no calc to finish the break.
#define TRUNK_FALLEN_AI_VT 0x7001c711a8ull
#define TRUNK_LOG_CHILD_VT 0x7001c8b7c8ull
#define A_TRUNK_CHANGE_TO_LOG 0x700094e4dcull
#define A_TRUNK_WEAPON_LIFE 0x70007b5b1cull
#define A_TRUNK_DROP_COMPONENT 0x70007ac74cull
#define A_TRUNK_BREAK_STATE 0x70007b5b44ull
#define A_TRUNK_REQUEST_DROP 0x7000bdc1f0ull
#define A_TRUNK_BREAK_ACTOR 0x7000aa5330ull

static bool trunkwood_on(void) { return brk_loot_on() && look_on("SAVAGEWIND_COOP_TRUNKWOOD"); }

static uint64_t trunk_fallen_ai(uint64_t actor) {
    uint64_t manager = LD64(actor + 0x558);
    if (!in_guest(manager + 0x80)) return 0;
    uint32_t count = LD32(manager + 0x78);
    uint64_t list = LD64(manager + 0x80);
    for (uint32_t i = 0; i < count && in_guest(list + (uint64_t)i * 8); i++) {
        uint64_t ai = LD64(list + (uint64_t)i * 8);
        if (in_guest(ai) && LD64(ai) == TRUNK_FALLEN_AI_VT && LD64(ai + 8) == actor) return ai;
    }
    return 0;
}

static uint64_t trunk_ai_child(uint64_t ai) {
    uint16_t index = LD16(ai + 0x1c);
    if (index >= LD32(ai + 0x28)) return 0;
    uint64_t slot = LD64(ai + 0x30) + (uint64_t)index * 8;
    return in_guest(slot) ? LD64(slot) : 0;
}

static bool trunk_break_ai(Cpu *c, uint64_t actor, uint64_t ai) {
    uint64_t child = trunk_ai_child(ai);
    if (!in_guest(child)) return false;
    if (LD64(child) != TRUNK_LOG_CHILD_VT) {
        Cpu saved = *c;
        c->x[0] = ai;
        guest_call(c, A_TRUNK_CHANGE_TO_LOG);
        *c = saved;
        child = trunk_ai_child(ai);
        if (!in_guest(child) || LD64(child) != TRUNK_LOG_CHILD_VT) return false;
    }
    return brk_kill(c, actor) ||
        (LD64(LD64(actor) + A_ACTOR_LIFE_VF) == A_TRUNK_WEAPON_LIFE && (int32_t)LD32(actor + 0xd4c) <= 0);
}

static bool trunk_break_drop(Cpu *c, uint64_t actor, uint64_t ai) {
    uint64_t vt = LD64(actor), drop_pos = LD64(ai + 0xe8);
    if (!in_guest(drop_pos) || !in_guest(drop_pos + 11) || !in_guest(vt + 0x438) ||
        LD64(vt + 0x430) != A_TRUNK_DROP_COMPONENT || LD64(vt + 0x438) != A_TRUNK_BREAK_STATE ||
        (LD32(actor + 0x78) & 1))
        return false;

    Cpu saved = *c;
    c->x[0] = actor;
    guest_call(c, A_TRUNK_DROP_COMPONENT);
    uint64_t drop = c->x[0];
    *c = saved;
    if (!in_guest(drop + 0xc) || (LD16(drop + 0xc) & 0x80)) return false;

    // The drop helper copies a world position into the AI's drop-position parameter. Use the current pose
    // in case the AI never entered and initialized its local drop offset.
    const float *m = (const float *)(uintptr_t)(actor + ACTOR_MTX);
    float pos[3] = {m[3], m[7], m[11]};
    memcpy((void *)(uintptr_t)(ai + 0x274), pos, sizeof pos);
    c->x[0] = actor, c->x[1] = drop_pos, c->x[2] = ai + 0x274;
    guest_call(c, A_TRUNK_REQUEST_DROP);
    bool requested = c->x[0] != 0;
    *c = saved;
    if (!requested) return false;

    c->x[0] = actor;
    guest_call(c, A_TRUNK_BREAK_STATE);
    uint64_t state = c->x[0];
    *c = saved;
    ST32(state + 4, 1);
    c->x[0] = actor, c->x[1] = 0;
    guest_call(c, A_TRUNK_BREAK_ACTOR);
    *c = saved;
    return true;
}

// Called only from trunks_frame, which runs on the game thread with the world loaded.
static void trunk_break(Cpu *c, struct Trunk *t, uint64_t now) {
    bool wood = trunkwood_on();
    uint64_t ai = wood ? trunk_fallen_ai(t->actor) : 0;
    if (ai && !t->kill_ns && trunk_break_ai(c, t->actor, ai)) {
        t->kill_ns = now + BRK_KILL_NS;
        LOG("[coop] trunks: %u requested break via fallen-tree AI\n", t->hash);
        return;
    }
    if (ai && !t->native_drop && trunk_break_drop(c, t->actor, ai)) {
        t->native_drop = true;
        t->kill_ns = now + BRK_KILL_NS;
        LOG("[coop] trunks: %u requested break via native drop\n", t->hash);
        return;
    }
    bool gone = despawn_now(c, t->actor, ACTOR_DELETE_REASON);
    LOG("[coop] trunks: %u cut into pieces by the other player (%s): our trunk removed %s\n",
        t->hash, wood ? "wood path unavailable" : "wood disabled", gone ? "ok" : "FAILED");
    memset(t, 0, sizeof *t);
}

static void trunk_send(struct Trunk *t, uint8_t flags, uint64_t now) {
    CoopTrunk e;
    memset(&e, 0, sizeof e);
    char area[96];
    e.hash = t->hash, e.area = local_area(area, sizeof area), e.gen = t->gen, e.flags = flags;
    if (!(flags & COOP_TRUNK_GONE)) {
        trunk_pose(t, e.pos, e.rot);
        float n = 0;
        for (int k = 0; k < 4; k++) n += e.rot[k] * e.rot[k];
        if (n > 0.5f) for (int k = 0; k < 4; k++) e.rot[k] /= sqrtf(n);
        float dt = t->sent_ns && now > t->sent_ns ? (float)((now - t->sent_ns) / 1e9) : 0;
        for (int k = 0; k < 3 && dt > 0.01f && !(flags & COOP_TRUNK_REST); k++) e.vel[k] = (e.pos[k] - t->sent_pos[k]) / dt;
        for (int k = 0; k < 3; k++) if (!(fabsf(e.vel[k]) <= 50)) e.vel[k] = 0;
        memcpy(t->sent_pos, e.pos, 12);
    }
    t->sent_ns = now;
    bool ok = coop_trunk_ok(&e) && coop_net_send_event(COOP_EV_TRUNK, &e, sizeof e);
    static unsigned said;
    if (flags || said++ < 6 || coop_look_log())
        LOG("[coop] trunks: %u %s (gen %u) at %.2f %.2f %.2f: %s\n", t->hash,
            flags & COOP_TRUNK_GONE ? "cut into pieces here" : flags & COOP_TRUNK_REST ? "rests here" : "moving here",
            t->gen, e.pos[0], e.pos[1], e.pos[2], ok ? "sent" : "NOT sent");
}
// Extrapolate the follower's trunk orientation forward from its last two received poses so falling over is smooth
// between network updates instead of snapping in 10 Hz / packet steps.
static void trunk_extrap_rot(const struct Trunk *t, uint64_t now, float out[4]) {
    memcpy(out, t->rx.rot, 16);
    if (!t->prev_rx_ns || t->rx_ns <= t->prev_rx_ns || (t->rx.flags & COOP_TRUNK_REST)) return;
    double dt_rx = (double)(t->rx_ns - t->prev_rx_ns) / 1e9;
    double age = now > t->rx_ns ? (double)(now - t->rx_ns) / 1e9 : 0.0;
    if (dt_rx < 0.005 || dt_rx > 0.5 || age <= 0.0) return;
    float alpha = (float)(age / dt_rx);
    if (alpha > 1.5f) alpha = 1.5f;
    float q0[4], q1[4];
    memcpy(q0, t->prev_rx.rot, 16);
    memcpy(q1, t->rx.rot, 16);
    float dot = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
    if (dot < 0) for (int k = 0; k < 4; k++) q0[k] = -q0[k];
    // dq = q1 * conj(q0) in (x, y, z, w) layout
    float dw = q1[3] * q0[3] + q1[0] * q0[0] + q1[1] * q0[1] + q1[2] * q0[2];
    float dx = -q1[3] * q0[0] + q1[0] * q0[3] - q1[1] * q0[2] + q1[2] * q0[1];
    float dy = -q1[3] * q0[1] + q1[0] * q0[2] + q1[1] * q0[3] - q1[2] * q0[0];
    float dz = -q1[3] * q0[2] - q1[0] * q0[1] + q1[1] * q0[0] + q1[2] * q0[3];
    float vlen = sqrtf(dx * dx + dy * dy + dz * dz);
    if (vlen < 1e-5f) return;
    float half = atan2f(vlen, dw) * alpha;
    float s = sinf(half) / vlen, c = cosf(half);
    float sx = dx * s, sy = dy * s, sz = dz * s, sw = c;
    // out = step * q1
    out[0] = sw * q1[0] + sx * q1[3] + sy * q1[2] - sz * q1[1];
    out[1] = sw * q1[1] - sx * q1[2] + sy * q1[3] + sz * q1[0];
    out[2] = sw * q1[2] + sx * q1[1] - sy * q1[0] + sz * q1[3];
    out[3] = sw * q1[3] - sx * q1[0] - sy * q1[1] - sz * q1[2];
    float n = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (n > 0.5f) for (int k = 0; k < 4; k++) out[k] /= n;
}
static void trunk_take_rx(struct Trunk *t, const CoopTrunk *e) {
    if (e->gen < t->gen || (e->gen == t->gen && t->owner == 1)) return;  // (older, or the same claim as ours: ours wins)
    uint64_t now = plat_now_ns();
    if (t->owner != 2) LOG("[coop] trunks: %u is the other player's (gen %u)\n", t->hash, e->gen), t->changed_ns = now;
    if (t->have_rx && t->owner == 2 && e->gen == t->gen && !(e->flags & COOP_TRUNK_REST) && now > t->rx_ns) {
        t->prev_rx = t->rx;
        t->prev_rx_ns = t->rx_ns;
    } else {
        t->prev_rx_ns = 0;
    }
    t->owner = 2, t->gen = e->gen, t->rx = *e, t->have_rx = true, t->rx_ns = now, t->rx_applied = false;
}
// EV 20 from the other game (gate.inc world_event, game thread).
static void trunk_ev(Cpu *c, const uint8_t *data, int len) {
    (void)c;
    if (!trunks_on() || len != (int)sizeof(CoopTrunk)) return;
    CoopTrunk e;
    memcpy(&e, data, sizeof e);
    if (!coop_trunk_ok(&e)) { LOG("[coop] trunks: bad event dropped\n"); return; }
    char area[96];
    if (e.area != local_area(area, sizeof area)) return;
    for (int i = 0; i < TRUNK_MAX; i++)
        if (trunks[i].hash == e.hash) { trunk_take_rx(&trunks[i], &e); return; }
    int slot = 0;  // (not here yet: kept for when it is; the same tree's slot, else a free / the oldest one)
    for (int i = 0; i < TRUNK_MAX; i++) {
        if (trunk_early[i].hash == e.hash) { slot = i; break; }
        if (!trunk_early[i].hash) slot = i;
    }
    if (trunk_early[slot].hash != e.hash)
        LOG("[coop] trunks: %u pose from the other player before our trunk is here: kept\n", e.hash);
    trunk_early[slot] = e;
}
static bool trunk_name(const char *n) {
    size_t l = strlen(n);
    return l > 6 && !strcmp(n + l - 6, "_Trunk");
}
static void trunk_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)ctx;
    int free_i = -1;
    for (int k = 0; k < TRUNK_MAX; k++) {
        if (trunks[k].hash && trunks[k].actor == a) return;
        if (!trunks[k].hash && free_i < 0) free_i = k;
    }
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    int best = -1;
    float bd = 1.5f * 1.5f;  // (a new trunk is made standing where its tree stood)
    for (int i = 0; i < 32; i++) {
        if (!trunk_felled[i].hash) continue;
        float dx = m[3] - trunk_felled[i].x, dz = m[11] - trunk_felled[i].z;
        if (dx * dx + dz * dz < bd) bd = dx * dx + dz * dz, best = i;
    }
    if (best < 0 || free_i < 0) return;
    uint32_t hash = trunk_felled[best].hash;
    for (int k = 0; k < TRUNK_MAX; k++)
        if (trunks[k].hash == hash) return;  // (that tree's trunk is known: a second one there is not its)
    uint64_t rb = actor_rb(a);
    if (!rb) return;
    uint64_t now = *(const uint64_t *)ctx;
    struct Trunk *t = &trunks[free_i];
    memset(t, 0, sizeof *t);
    t->hash = hash, t->actor = a, t->unit = LD64(a + PROC_UNIT), t->unit_id = LD32(a + PROC_ID), t->rb = rb,
    t->body = LD64(rb + 0x78), t->found_ns = t->changed_ns = now;
    // (the tree we felled for the other player: theirs; the one our player cut: ours)
    t->owner = tree_was_applied(hash) ? 2 : 1, t->gen = 1;
    LOG("[coop] trunks: %u trunk %s here at %.2f %.2f %.2f, body %s (%s)\n", hash, name, m[3], m[7], m[11],
        trunk_active(t) ? "in the world" : "not in the world yet (standing)",
        t->owner == 1 ? "we cut it: ours" : "the other player cut it: theirs");
    for (int e = 0; e < TRUNK_MAX; e++)
        if (trunk_early[e].hash == hash) trunk_take_rx(t, &trunk_early[e]), trunk_early[e].hash = 0;
}
// `coop trunk tip` (dev, a two-game test): our standing trunks are put into the physics world tipped over (lying along
// +z, 0.8 m up) and fall from there - a stand-in for the push a real axe cut gives (sword hits knock it over only
// sometimes).
static volatile int trunk_tip_dev;
static void trunk_tip(Cpu *c) {
    for (int i = 0; i < TRUNK_MAX; i++) {
        struct Trunk *t = &trunks[i];
        if (!t->hash || !t->tip || t->owner != 1 || !trunk_alive(t)) continue;
        t->tip--;
        if (!trunk_active(t)) {  // (into the world first; a setTransform before that is lost)
            rb_enable(c, t->rb);
            continue;
        }
        float pos[3], rot[4], m[12];
        trunk_pose(t, pos, rot);
        static const float q[4] = {0.7071068f, 0, 0, 0.7071068f};  // (90 degrees about x: up becomes +z)
        pos[1] += 0.8f;
        quat_mtx(q, pos, m);
        uint32_t fl = LD32(t->rb + 0x68);
        if (fl & 0x80000) rb_fix(c, t->rb, false);  // (a resting/fixed body ignores moves)
        rb_set_transform(c, t->rb, m);
        t->tip = 0;
        LOG("[coop] trunks: %u tipped over (dev) at %.2f %.2f %.2f (body flags %08x)\n", t->hash, pos[0], pos[1],
            pos[2], fl);
    }
}
// Each frame (trees_frame): find the trunks of the trees felled near us, send ours, follow theirs.
static void trunks_frame(Cpu *c) {
    if (!trunks_on()) return;
    if (trunk_tip_dev) {
        trunk_tip_dev = 0;
        for (int i = 0; i < TRUNK_MAX; i++) trunks[i].tip = 30;
    }
    trunk_tip(c);
    uint64_t now = plat_now_ns();
    static unsigned frame;
    if (++frame % 4 == 0) {  // new trunks (of the trees that fell near us)
        bool any = false;
        for (int i = 0; i < tree_n; i++)
            if (trees[i].fell) trunk_tree_fell(trees[i].hash, trees[i].x, trees[i].z);
        for (int i = 0; i < 32; i++) {
            if (trunk_felled[i].hash && trunk_felled[i].ns + 30000000000ull < now) trunk_felled[i].hash = 0;
            if (trunk_felled[i].hash) any = true;
        }
        if (any) walk_actors(c, trunk_name, trunk_seen, &now);
    }
    for (int i = 0; i < TRUNK_MAX; i++) {
        struct Trunk *t = &trunks[i];
        if (!t->hash) continue;
        if (!trunk_alive(t)) {
            // Gone: cut into pieces (or picked up / unloaded). Ours: tell the other game. Theirs gone there: done.
            if (t->kill_ns) {
                LOG("[coop] trunks: %u cut into pieces by the other player: broken here too (%s)\n",
                    t->hash, t->native_drop ? "native drop" : "fallen-tree AI");
            } else if (t->owner == 1 && !t->gone_sent && local_in_world()) {
                t->actor = 0;
                trunk_send(t, COOP_TRUNK_GONE, now);
            }
            LOG("[coop] trunks: %u trunk gone here\n", t->hash);
            memset(t, 0, sizeof *t);
            continue;
        }
        if (t->kill_ns) {
            if (now > t->kill_ns) trunk_break(c, t, now);
            continue;
        }
        if (t->have_rx && (t->rx.flags & COOP_TRUNK_GONE)) {
            trunk_break(c, t, now);
            continue;
        }
        float pos[3], rot[4];
        trunk_pose(t, pos, rot);
        if (t->owner == 1) {
            if (!trunk_active(t)) continue;  // (still standing where the tree was, as in the other game)
            float d = dist3(pos, t->sent_pos);
            float sp = t->sent_ns && now > t->sent_ns ? d / (float)((now - t->sent_ns) / 1e9) : 99;
            bool still = sp < TRUNK_STILL_MS * 2 && d < 0.05f;
            if (!still) t->still_ns = 0, t->rest_sent = t->rest_sent && d < 0.15f;
            else if (!t->still_ns) t->still_ns = now;
            if (!t->rest_sent && t->still_ns && now - t->still_ns > TRUNK_REST_NS) {
                trunk_send(t, COOP_TRUNK_REST, now);
                t->rest_sent = true;
            } else if (!t->rest_sent && now - t->sent_ns >= TRUNK_SEND_NS) {
                trunk_send(t, 0, now);
            }
            continue;
        }
        if (!t->have_rx) {
            // (theirs, still standing there; our player knocked ours over first: ours now)
            if (trunk_active(t) && now - t->changed_ns > TRUNK_CALM_NS) {
                LOG("[coop] trunks: %u knocked over by our player first: ours now\n", t->hash);
                t->owner = 1, t->gen++, t->rest_sent = false, t->still_ns = 0, t->changed_ns = now;
                memcpy(t->sent_pos, pos, 12);
                trunk_send(t, 0, now);
            }
            continue;
        }
        // Follower. Our player moved our copy (it ended a frame far from where we put it, after a calm time): ours now.
        if (t->have_put && !t->hold && now - t->changed_ns > TRUNK_CALM_NS && dist3(pos, t->put) > TRUNK_CLAIM_M) {
            LOG("[coop] trunks: %u moved by our player (%.2f m from the other player's pose): ours now\n", t->hash,
                dist3(pos, t->put));
            t->owner = 1, t->gen++, t->rest_sent = false, t->still_ns = 0, t->changed_ns = now, t->have_put = false;
            memcpy(t->sent_pos, pos, 12);
            trunk_send(t, 0, now);
            continue;
        }
        bool rest = t->rx.flags & COOP_TRUNK_REST;
        if (rest && t->rx_applied) {  // (at their rest pose and let go: only watched for our player's moves)
            if (!t->hold) continue;
            // (2026-11-18 test 6: a trunk standing where the tree was is held upright by its actor - our setTransform
            // was undone the next frame; a sword hit lets it go) so keep putting it there, the actor matrix too,
            // until it stays
            float d = rot[0] * t->rx.rot[0] + rot[1] * t->rx.rot[1] + rot[2] * t->rx.rot[2] + rot[3] * t->rx.rot[3];
            if (dist3(pos, t->put) < 0.15f && fabsf(d) > 0.99f) {
                if (t->hold_said) LOG("[coop] trunks: %u stays at the other player's rest pose now\n", t->hash);
                t->hold = 0;
                continue;
            }
            if (!t->hold_said) LOG("[coop] trunks: %u our trunk went back (%.2f m off): held there\n", t->hash, dist3(pos, t->put));
            t->hold_said = true;
            if (--t->hold == 0) {
                LOG("[coop] trunks: %u could not hold it at the other player's rest pose\n", t->hash);
                continue;
            }
            float m[12];
            quat_mtx(t->rx.rot, t->put, m);
            if (LD32(t->rb + 0x68) & 0x80000) rb_fix(c, t->rb, false);
            memcpy((float *)(uintptr_t)(t->actor + ACTOR_MTX), m, sizeof m);
            rb_set_transform(c, t->rb, m);
            continue;
        }
        float m[12], to[3], vel[3] = {0, 0, 0}, erot[4];
        double age = (now - t->rx_ns) / 1e9;
        if (age > 0.15) age = 0.15;
        for (int k = 0; k < 3; k++) to[k] = t->rx.pos[k] + (rest ? 0 : t->rx.vel[k] * (float)age), vel[k] = rest ? 0 : t->rx.vel[k];
        if (!trunk_active(t)) {  // (ours still stands, never placed by its tree: into the physics world first)
            rb_enable(c, t->rb);
            if (!t->enable_asked)
                LOG("[coop] trunks: %u our trunk was not in the physics world: enabled (%s)\n", t->hash,
                    trunk_active(t) ? "ok" : "from the next frame");
            t->enable_asked = true;
            if (!trunk_active(t)) continue;  // (2026-11-18 test: a setTransform before it is in the world is lost)
        }
        if (LD32(t->rb + 0x68) & 0x80000) rb_fix(c, t->rb, false);
        if (rest) memcpy(erot, t->rx.rot, 16);
        else trunk_extrap_rot(t, now, erot);
        quat_mtx(erot, to, m);
        rb_set_transform(c, t->rb, m);
        rb_set_linvel(c, t->rb, vel);
        memcpy(t->put, to, 12), t->have_put = true;
        if (rest) {
            t->rx_applied = true, t->hold = 120;
            LOG("[coop] trunks: %u put where the other player's rests: %.2f %.2f %.2f (ours was %.2f m away)\n", t->hash,
                to[0], to[1], to[2], dist3(pos, to));
        }
    }
}

static volatile int tree_dev;  // `coop tree` (1), `coop treecut` (2), `coop treehit` (3)
// `coop trunk` (dev, 2026-11-18): every felled trunk / log piece near us: actor matrix, its main body's fields.
static volatile int trunk_dump_dev;
static bool trunk_dump_name(const char *n) { return strstr(n, "_Trunk") || strstr(n, "Wood") || strstr(n, "_Log"); }
static void trunk_dump_one(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)ctx;
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    float dx = m[3] - tree_me[0], dz = m[11] - tree_me[2];
    if (dx * dx + dz * dz > 60 * 60) return;
    uint64_t rb = actor_rb(a), body = rb ? LD64(rb + 0x78) : 0;
    LOG("[coop] trunk (dev): %s (%llx) mtx t %.2f %.2f %.2f x-axis %.2f %.2f %.2f rb %llx flags %08x y-axis %.2f %.2f %.2f\n", name,
        (unsigned long long)a, m[3], m[7], m[11], m[0], m[4], m[8], (unsigned long long)rb, rb ? LD32(rb + 0x68) : 0,
        m[1], m[5], m[9]);
    if (strstr(name, "_Trunk") && in_guest(a + 0xa00)) {
        uint64_t vt = LD64(a), life_fn = in_guest(vt) ? LD64(vt + A_ACTOR_LIFE_VF) : 0;
        uint64_t drop_fn = in_guest(vt) ? LD64(vt + 0x430) : 0, life = 0, drop = 0;
        if (life_fn >= 0x7000000000ull && life_fn < 0x7003000000ull) {
            Cpu sv = *c; c->x[0] = a; guest_call(c, life_fn); life = c->x[0]; *c = sv;
        }
        if (drop_fn >= 0x7000000000ull && drop_fn < 0x7003000000ull) {
            Cpu sv = *c; c->x[0] = a; guest_call(c, drop_fn); drop = c->x[0]; *c = sv;
        }
        LOG("[coop] trunk (dev):   vt +%llx life_fn +%llx life %llx (+%llx=%d) drop %llx (f=%02x) f920=%u f7c8=%u\n",
            (unsigned long long)(vt >= 0x7000000000ull ? vt - 0x7000000000ull : vt),
            (unsigned long long)(life_fn >= 0x7000000000ull ? life_fn - 0x7000000000ull : life_fn),
            (unsigned long long)life, (unsigned long long)(life >= a && life < a + 0x4000 ? life - a : 0),
            in_guest(life) ? (int32_t)LD32(life) : -999,
            (unsigned long long)drop, in_guest(drop + 0xc) ? LD8(drop + 0xc) : 0,
            LD8(a + 0x920), LD8(a + 0x7c8));
        uint64_t aim = LD64(a + 0x558);
        if (in_guest(aim)) {
            uint32_t n = LD32(aim + 0x78);
            uint64_t arr = LD64(aim + 0x80);
            for (uint32_t i = 0; i < n && i < 16 && in_guest(arr + (uint64_t)i * 8); i++) {
                uint64_t ai = LD64(arr + (uint64_t)i * 8);
                if (!in_guest(ai)) continue;
                uint64_t avt = LD64(ai), child = 0;
                const char *aname = "-", *cname = "-";
                if (in_guest(avt + 0xd8) && LD64(avt + 0xd0) == 0x70015377d0ull) {
                    Cpu sv = *c; c->x[0] = ai; guest_call(c, 0x700107600cull);
                    aname = in_guest(c->x[0]) ? (const char *)(uintptr_t)c->x[0] : "-"; *c = sv;
                    uint16_t cur = LD16(ai + 0x1c); uint32_t cnt = LD32(ai + 0x28); uint64_t ch = LD64(ai + 0x30);
                    if (cur < cnt && in_guest(ch + (uint64_t)cur * 8)) child = LD64(ch + (uint64_t)cur * 8);
                    if (in_guest(child)) {
                        sv = *c; c->x[0] = child; guest_call(c, 0x700107600cull);
                        cname = in_guest(c->x[0]) ? (const char *)(uintptr_t)c->x[0] : "-"; *c = sv;
                    }
                }
                LOG("[coop] trunk (dev):   ai[%u] %llx vt +%llx (%s) child %llx (%s)\n", i,
                    (unsigned long long)ai, (unsigned long long)(avt >= 0x7000000000ull ? avt - 0x7000000000ull : avt),
                    aname, (unsigned long long)child, cname);
            }
        }
    }
    if (in_guest(body)) {
        const float *b = (const float *)(uintptr_t)(body + 0x180);
        LOG("[coop] trunk (dev):   body +180 %.2f %.2f %.2f %.2f | +190 %.2f %.2f %.2f %.2f | +1a0 %.2f %.2f %.2f %.2f | "
            "+1b0 %.2f %.2f %.2f %.2f | +1c0 %.3f %.3f %.3f %.3f | +1d0 %.3f %.3f %.3f %.3f | +1e0 %.2f %.2f %.2f\n",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], b[16],
            b[17], b[18], b[19], b[20], b[21], b[22], b[23], b[24], b[25], b[26]);
    }
}
static void trunks_drop_actors(void) {
    memset(trunks, 0, sizeof trunks);
    memset(trunk_felled, 0, sizeof trunk_felled);
}
static void trees_frame(Cpu *c) {
    static unsigned frame;
    if (!trees_on() || !local_in_world() || !player_info_ptr()) {
        tree_n = tree_hit_n = 0;
        trunks_drop_actors();
        return;
    }
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    static uint32_t last_area;
    if (here != last_area) {
        last_area = here;
        tree_n = 0;
        trunks_drop_actors();
        for (int i = 0; i < TRUNK_MAX; i++)
            if (trunk_early[i].hash && trunk_early[i].area != here) trunk_early[i].hash = 0;
    }
    trunks_frame(c);
    if (++frame % 8 && !tree_dev && !trunk_dump_dev) return;
    memcpy(tree_me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    if (trunk_dump_dev) {
        trunk_dump_dev = 0;
        walk_actors(c, trunk_dump_name, trunk_dump_one, NULL);
        for (int i = 0; i < tree_n; i++) {
            if (!trees[i].fell || !in_guest(trees[i].ai)) continue;
            uint64_t L = LD64(trees[i].ai + 0x38);
            if (!in_guest(L)) continue;
            LOG("[coop] trunk (dev): tree %u felled: L flags %04x stump %llx trunk %llx\n", trees[i].hash, LD16(L + 0x10),
                (unsigned long long)LD64(L + 0x40), (unsigned long long)LD64(L + 0x48));
        }
    }
    tree_new_n = 0;
    stump_new_n = 0;
    memset(&tree_diag, 0, sizeof tree_diag);
    walk_actors(c, tree_or_stump, tree_seen, NULL);
    bool hits = tree_hits_on();
    uint64_t scan_ns = plat_now_ns();
    for (int i = 0; i < tree_new_n; i++) {
        struct Tree *t = &trees_new[i];
        uint8_t now = LD8(t->ai + 0x24c);
        if (now && !t->fell && !tree_was_applied(t->hash) && !tree_was_sent(t->hash)) {
            tree_sent[tree_sent_next] = t->hash, tree_sent_next = (tree_sent_next + 1) % 32;
            CoopTree e;
            memset(&e, 0, sizeof e);
            e.hash = t->hash, e.area = here, e.instant = LD8(t->ai + 0x2dc) ? 1 : 0;
            uint64_t d = LD64(t->ai + 0x2b8);
            if (in_guest(d)) memcpy(e.dir, (const void *)(uintptr_t)d, 12);
            if (!coop_tree_ok(&e)) memset(e.dir, 0, sizeof e.dir), e.instant = 0;
            bool ok = coop_tree_ok(&e) && coop_net_send_event(COOP_EV_TREE, &e, sizeof e);
            LOG("[coop] trees: we cut %u (%s): %s\n", t->hash, (const char *)(uintptr_t)(t->actor + 0x1c),
                ok ? "sent" : "NOT sent");
        }
        t->fell = now;
        for (int k = 0; k < TREE_PENDING; k++)  // (a tree the other player cut, here now)
            if (tree_wait[k].hash == t->hash && plat_now_ns() < tree_wait[k].until_ns) {
                // (2026-11-15 play: "often one cuts the tree, it falls for him, for the other Link it stays standing")
                // A refused fell is retried every scan (it was dropped after one try); after 5 s of refusals the tree
                // is removed instead (no stump/log there, but it does not stand). SAVAGEWIND_COOP_TREEREMOVE=0: off.
                if (now) { tree_wait[k].hash = 0; continue; }
                if (tree_fell(c, t, tree_wait[k].dir, tree_wait[k].instant, true)) {
                    t->fell = 1;
                    LOG("[coop] trees: %u cut by the other player earlier: felled here now\n", t->hash);
                    tree_wait[k].hash = 0;
                    continue;
                }
                if (!tree_wait[k].fail_ns) {
                    tree_wait[k].fail_ns = plat_now_ns();
                    LOG("[coop] trees: %u cut by the other player: here but refused (%s), retrying\n", t->hash,
                        tree_why_not(t));
                } else if (plat_now_ns() - tree_wait[k].fail_ns > 5000000000ull && look_on("SAVAGEWIND_COOP_TREEREMOVE")) {
                    bool gone = despawn_now(c, t->actor, ACTOR_DELETE_REASON);
                    LOG("[coop] trees: %u still refused after 5 s (%s): tree removed %s\n", t->hash, tree_why_not(t),
                        gone ? "ok" : "FAILED");
                    tree_wait[k].hash = 0;
                }
            }
        if (hits && !t->fell) tree_hits_scan(c, t, here, scan_ns);
    }
    int pending = 0;
    for (int i = 0; i < tree_hit_n; i++)
        if (tree_hits[i].hit.hash && tree_hits[i].hit.area == here && tree_hits[i].until_ns > scan_ns)
            tree_hits[pending++] = tree_hits[i];
    tree_hit_n = pending;
    static bool first_scan = true;  // (stumps seen at the first scan were there before: map stumps, old cuts)
    for (int s = 0; s < stump_new_n; s++) {
        int best = -1;
        float bd = 2.5f * 2.5f;
        for (int i = 0; i < tree_n; i++) {  // (the last scan's trees: the cut one may be gone now)
            float dx = trees[i].x - stump_new[s].x, dz = trees[i].z - stump_new[s].z;
            if (dx * dx + dz * dz < bd) bd = dx * dx + dz * dz, best = i;
        }
        struct Tree *t = best < 0 ? NULL : &trees[best];
        bool standing = false;
        // The first hit preloads a stump at the tree's position. Wait for the tree to fall or disappear before
        // counting that stump. Leave it eligible for the next scan, in case the tree disappears before its fell flag is seen.
        for (int i = 0; t && i < tree_new_n; i++)
            if (trees_new[i].hash == t->hash) { standing = !trees_new[i].fell; break; }
        if (standing) continue;
        stumps_seen[stumps_next] = stump_new[s].id, stumps_next = (stumps_next + 1) % 64;
        LOG("[coop] trees: stump %s at %.1f %.1f (a tree near us was cut; %d trees watched)\n",
            (const char *)(uintptr_t)(stump_new[s].actor + 0x1c), stump_new[s].x, stump_new[s].z, tree_n);
        if (!t || first_scan || !look_on("SAVAGEWIND_COOP_TREESTUMP") ||
            t->fell || tree_was_applied(t->hash) || tree_was_sent(t->hash)) continue;
        tree_sent[tree_sent_next] = t->hash, tree_sent_next = (tree_sent_next + 1) % 32;
        trunk_tree_fell(t->hash, t->x, t->z);
        CoopTree e;
        memset(&e, 0, sizeof e);
        e.hash = t->hash, e.area = here;
        bool ok = coop_tree_ok(&e) && coop_net_send_event(COOP_EV_TREE, &e, sizeof e);
        LOG("[coop] trees: we cut %u (stump appeared where it stood): %s\n", t->hash, ok ? "sent" : "NOT sent");
    }
    first_scan = false;
    memcpy(trees, trees_new, sizeof(struct Tree) * (size_t)tree_new_n);
    tree_n = tree_new_n;
    static uint64_t said_ns;  // (play logs: how many trees are watched, at most once a minute)
    if (plat_now_ns() - said_ns > 60000000000ull)
        said_ns = plat_now_ns(), LOG("[coop] trees: watching %d near us (tree actors loaded %d: %d farther than %.0f m, %d "
                                     "without the tree AI (e.g. %s), %d without a map id)\n",
                                     tree_n, tree_diag.names, tree_diag.far, TREE_NEAR_M, tree_diag.noai,
                                     tree_diag.noai_name[0] ? tree_diag.noai_name : "-", tree_diag.nohash);
    if (tree_dev) {
        int command = tree_dev;
        tree_dev = 0;
        int best = -1;
        float bd = 1e18f;
        for (int i = 0; i < tree_n; i++) {
            float dx = LDF32(trees[i].actor + ACTOR_MTX + 12) - tree_me[0], dz = LDF32(trees[i].actor + ACTOR_MTX + 44) - tree_me[2];
            if (dx * dx + dz * dz < bd) bd = dx * dx + dz * dz, best = i;
        }
        if (best < 0) { LOG("[coop] tree (dev): no tree near (%d seen)\n", tree_n); return; }
        struct Tree *t = &trees[best];
        LOG("[coop] tree (dev): %s hash %u at %.1f m, ai %llx, fell %u, state %d, L %llx, busy %u\n",
            (const char *)(uintptr_t)(t->actor + 0x1c), t->hash, sqrtf(bd), (unsigned long long)t->ai,
            LD8(t->ai + 0x24c), (int)(int8_t)LD8(t->ai + 0x240), (unsigned long long)LD64(t->ai + 0x38),
            LD32(t->actor + 0x694));
        uint64_t ptr = tree_life(c, t);
        int32_t life = ptr ? (int32_t)LD32(ptr) : 0;
        int32_t max = ptr ? tree_max_life(c, t) : 0;
        LOG("[coop] tree (dev): hash %u life %d at %.2f %.2f %.2f, max %d\n",
            t->hash, life, t->x, LDF32(t->actor + ACTOR_MTX + 28), t->z, max);
        if (command == 2) {
            static const float dir[3] = {1, 0, 0};
            bool ok = tree_fell(c, t, dir, 0, false);  // (as if we cut it: sent to the other game)
            LOG("[coop] tree (dev): fell %s\n", ok ? "called" : "not possible");
        } else if (command == 3 && life > 0 && max > 0 && max <= COOP_TREE_LIFE_MAX) {
            int32_t damage = (max + 3) / 4;
            int32_t next = life > damage ? life - damage : 0;
            float from[2] = {tree_me[0], tree_me[2]};
            if (!tree_hit_reaction(c, t, life - next, from)) return;
            ST32(ptr, next);
            if (!next) {
                float dir[3] = {t->x - from[0], 0, t->z - from[1]};
                float len = hypotf(dir[0], dir[2]);
                if (len > 0) dir[0] /= len, dir[2] /= len;
                tree_fell(c, t, dir, 0, false);
            }
            LOG("[coop] tree (dev): hit %u life %d -> %d\n", t->hash, life, next);
        }
    }
}
