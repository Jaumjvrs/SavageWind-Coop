// Part of runtime/coop.c (included there, same translation unit): (2026-11-19, worker fauna, protocol 80) the small
// animals - birds, fish, insects / frogs / lizards - are the same and in the same place for both players.
//
// Research (2026-11-19): the game's ecosystem makes most of them at run time around the player (1.9.0 AutoPlacement
// flows AutoPlacement_Bird / _Fish / _Insect, Bootup Ecosystem/AreaData.sbyml lists the species); each one it makes
// sits in the placement pool (singleton 0x7001e0c680, 300 records from +0x58, stride 0x38, actor first: see
// enemies_frame.inc wildlife_pool), so pool membership tells an ecosystem animal from a quest / item / dropped one.
// Placed ones (map HashId) exist in both games but each game's AI moves its own.
// Mammals (fox, deer, boar, squirrel, goat, bear, wolf...) are not handled here: they are run-time enemies of the
// enemy table (enemies.inc SPAWNANIMALS / SPAWNDEDUP, wildlife population rule), Keese / bats are enemies.
//
// One authority while the players are close (host <-> partner within FAUNA_ZONE_ON, off again past FAUNA_ZONE_OFF):
//  - the host sends (coop_net_set_fauna, ~5/s) its COOP_FAUNA_MAX nearest small animals within COOP_FAUNA_R of the
//    guest: ecosystem ones (pool) by process id, placed ones by HashId;
//  - the guest deletes its own ecosystem small animals within COOP_FAUNA_R of itself (its game makes new ones, they
//    are deleted again while the zone lasts: counted in the log), creates a copy of each host ecosystem animal by name
//    where the host has it, and pulls copies and its own placed animals of the same HashId toward the host's
//    positions each frame (their own AI keeps animating them; only a drift over FAUNA_DRIFT_* is corrected);
//  - caught (gone next to a player) / killed (life 0): COOP_EV_FAUNA_GONE, the other game deletes its one too.
// When the players part, the guest's copies go and its own ecosystem fills its surroundings again.
// SAVAGEWIND_COOP_FAUNA=0 turns this off (also off with SAVAGEWIND_COOP_WILDLIFE=0); the wildlife roster's insect
// copies (wildlife.inc) only run while this is off. Dev: `coop fauna` (census of small animals to the log).
#define FAUNA_ZONE_ON 60.0f
#define FAUNA_ZONE_OFF 80.0f
#define FAUNA_SEND_NS 200000000ull
#define FAUNA_CATCH_M 4.5f
#define FAUNA_COPIES 48
#define FAUNA_PLACED 96

static bool fauna_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_FAUNA") && look_on("SAVAGEWIND_COOP_WILDLIFE");
    return on;
}
static bool fauna_want(const char *name) { return coop_fauna_species(name) > 0; }
static const float *fauna_me(void) { return (const float *)(uintptr_t)(player_info_ptr() + 0x88); }
static float fauna_d(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
static struct Critter fauna_handle(uint64_t a) {
    return (struct Critter){.actor = a, .unit = LD64(a + PROC_UNIT), .unit_id = LD32(a + PROC_ID)};
}
static bool fauna_free(uint64_t a) {  // not held, not a dropped / thrown item of ours
    return a != local_held() && !drop_by_actor(a) && !drop_was_held(a);
}
static int16_t fauna_yaw(uint64_t a) {
    float y = atan2f(LD32F(a + ACTOR_MTX + 8), LD32F(a + ACTOR_MTX + 40));
    return (int16_t)lrintf(y * (32767.0f / 3.14159265f));
}
// Move an animal (body if it has a free one, else both actor matrices), facing yaw (radians).
static void fauna_move(Cpu *c, uint64_t a, const float to[3], float yaw) {
    float q[4] = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    uint64_t body = enemy_body(a), rb = body ? LD64(body + 0x18) : 0;
    if (in_guest(rb) && LD64(rb + 0x78) == body && !(LD32(rb + 0x68) & 1)) {
        float cur[3], p[3], m[12];
        drop_read_pos(a, cur);
        for (int j = 0; j < 3; j++) p[j] = to[j] + LD32F(body + 0x1a0 + j * 4) - cur[j];  // (actor-to-body offset)
        quat_mtx(q, p, m);
        rb_set_transform(c, rb, m);
    } else enemy_put_mtx(a, to, q);
}

// ---- host
// (3+ players) Each world guest has its own zone and its own set (the animals near it); a host animal may be in
// several guests' sets. Placed animals are watched near any guest in the zone.
struct FaunaListed { struct Critter k; uint32_t id; uint8_t species, flags; float pos[3]; int32_t life; };
#define FAUNA_FLED_MPS 3.0f  // faster than this between two sets: fleeing (birds take off, insects dart away)
static struct {
    bool zone;  // any guest in the zone
    uint64_t next_ns, placed_ns;
    struct { struct Critter k; uint8_t species; } placed[FAUNA_PLACED];
    int placed_n;
    struct {
        bool zone;
        float center[3];
        struct FaunaListed listed[COOP_FAUNA_MAX];
        int listed_n;
        unsigned sent, fled;
        uint64_t set_ns;
    } g[COOP_MAX_PLAYERS];
    struct { uint64_t actor; uint32_t unit_id; } deleted[16];  // removed for a guest: their going is no catch here
    int deleted_next;
} fh;
static float fauna_near_guest(const float p[3]) {  // distance to the nearest guest in the zone
    float best = 1e30f;
    for (int g = 1; g < COOP_MAX_PLAYERS; g++)
        if (fh.g[g].zone) {
            float d = fauna_d(p, fh.g[g].center);
            if (d < best) best = d;
        }
    return best;
}
static void fauna_placed_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)ctx;
    float p[3];
    if (fh.placed_n >= FAUNA_PLACED || wildlife_ecosystem_actor(a)) return;
    actor_dist(a, fauna_me(), p);
    if (fauna_near_guest(p) > COOP_FAUNA_R + 8) return;
    struct Critter k = fauna_handle(a);
    for (int i = 0; i < fh.placed_n; i++)
        if (fh.placed[i].k.actor == a && fh.placed[i].k.unit_id == k.unit_id) return;
    k.hash = actor_hash(c, a);
    if (!k.hash) return;
    fh.placed[fh.placed_n].k = k, fh.placed[fh.placed_n].species = (uint8_t)coop_fauna_species(name);
    fh.placed_n++;
}
static bool fauna_was_deleted(const struct Critter *k) {
    for (int i = 0; i < 16; i++)
        if (fh.deleted[i].actor == k->actor && fh.deleted[i].unit_id == k->unit_id) return true;
    return false;
}
static void fauna_send_gone(uint32_t id, uint8_t species, uint8_t flags, uint8_t why, const float pos[3], uint32_t area,
                            const char *who) {
    CoopFaunaGone g = {.area = area, .id = id, .species = species, .flags = flags, .why = why};
    memcpy(g.pos, pos, sizeof g.pos);
    bool ok = coop_fauna_gone_ok(&g) && coop_net_send_event(COOP_EV_FAUNA_GONE, &g, sizeof g);
    LOG("[coop] fauna: %s %s %s %08x at %.1f %.1f %.1f: %s\n", who, why == COOP_FAUNA_KILLED ? "killed" : "caught",
        coop_fauna_name(species), id, pos[0], pos[1], pos[2], ok ? "sent" : "NOT sent");
}
// Guest gid's set: the host's COOP_FAUNA_MAX nearest small animals within COOP_FAUNA_R of it.
static void fauna_host_set_for(int gid, uint32_t area, uint64_t now) {
    typeof(fh.g[0]) *G = &fh.g[gid];
    float dt = G->set_ns ? (float)((now - G->set_ns) / 1e9) : 0;
    G->set_ns = now;
    CoopFauna f = {.area = area};
    memcpy(f.center, G->center, sizeof f.center);
    struct { struct Critter k; uint32_t id; uint8_t species, flags; float pos[3]; float d; } cand[COOP_FAUNA_MAX];
    int n = 0;
    uint64_t mgr = wildlife_pool();
    int total = in_guest(mgr) ? WILDLIFE_POOL_SIZE : 0;
    for (int i = 0; i < total + fh.placed_n; i++) {
        struct Critter k;
        uint8_t species, flags = 0;
        uint32_t id;
        if (i < total) {
            uint64_t a = wildlife_slot(mgr, i);
            if (!in_guest(a)) continue;
            species = (uint8_t)coop_fauna_species((const char *)(uintptr_t)(a + 0x1c));
            if (!species) continue;
            k = fauna_handle(a);
            id = k.unit_id;
        } else {
            k = fh.placed[i - total].k, species = fh.placed[i - total].species, id = k.hash, flags = COOP_FAUNA_PLACED;
        }
        if (!id || !critter_alive(&k) || !fauna_free(k.actor)) continue;
        float p[3], d = actor_dist(k.actor, G->center, p);
        if (!(d <= COOP_FAUNA_R - 0.5f)) continue;
        int at = n < COOP_FAUNA_MAX ? n : COOP_FAUNA_MAX;
        while (at > 0 && cand[at - 1].d > d) at--;
        if (at >= COOP_FAUNA_MAX) continue;
        int last = n < COOP_FAUNA_MAX ? n : COOP_FAUNA_MAX - 1;
        memmove(&cand[at + 1], &cand[at], (last - at) * sizeof cand[0]);
        cand[at].k = k, cand[at].id = id, cand[at].species = species, cand[at].flags = flags, cand[at].d = d;
        memcpy(cand[at].pos, p, sizeof p);
        if (n < COOP_FAUNA_MAX) n++;
    }
    uint8_t fled[COOP_FAUNA_MAX] = {0};
    for (int i = 0; i < n && dt > 0.05f && dt < 1.0f; i++)
        for (int j = 0; j < G->listed_n; j++)
            if (G->listed[j].id == cand[i].id && G->listed[j].flags == cand[i].flags) {
                if (fauna_d(G->listed[j].pos, cand[i].pos) > FAUNA_FLED_MPS * dt) fled[i] = COOP_FAUNA_FLED, G->fled++;
                break;
            }
    G->listed_n = 0;
    for (int i = 0; i < n; i++) {
        f.a[i] = (CoopFaunaAnimal){.id = cand[i].id, .species = cand[i].species, .flags = cand[i].flags | fled[i],
                                   .yaw = fauna_yaw(cand[i].k.actor)};
        memcpy(f.a[i].pos, cand[i].pos, sizeof f.a[i].pos);
        struct FaunaListed *l = &G->listed[G->listed_n++];
        l->k = cand[i].k, l->id = cand[i].id, l->species = cand[i].species, l->flags = cand[i].flags;
        memcpy(l->pos, cand[i].pos, sizeof l->pos);
        l->life = (int32_t)LD32(cand[i].k.actor + ENEMY_LIFE);
    }
    f.count = (uint8_t)n;
    if (!coop_fauna_ok(&f)) return;
    coop_net_set_fauna_for(gid, &f);
    if (!(G->sent++ % 50))
        LOG("[coop] fauna: host set %u for guest %d: %d small animals near it (pool + %d placed watched), %u fled\n",
            G->sent, gid, n, fh.placed_n, G->fled);
}
static void fauna_host_frame(Cpu *c, uint64_t now) {
    if (now < fh.next_ns) return;
    fh.next_ns = now + FAUNA_SEND_NS;
    char an[96];
    uint32_t area = local_area(an, sizeof an);
    const float *me = fauna_me();
    uint32_t worlds = coop_net_world_players(1.0);
    bool any = false;
    // Caught / killed here since the last sets (gone next to our Link, or life 0): one event per animal.
    uint32_t told[COOP_FAUNA_MAX * 2];
    int told_n = 0;
    for (int gid = 1; gid < COOP_MAX_PLAYERS; gid++) {
        typeof(fh.g[0]) *G = &fh.g[gid];
        CoopState rs;
        double age = 99;
        bool peer = false;
        if ((worlds & (1u << gid)) && area) {
            coop_net_view(gid);
            peer = coop_net_get_remote(&rs, &age) && age < 1.0 && (rs.flags & COOP_FLAG_IN_WORLD) && rs.area == area;
            coop_net_view(-1);
        }
        float apart = peer ? fauna_d(me, rs.pos) : 1e9f;
        bool zone = peer && apart <= (G->zone ? FAUNA_ZONE_OFF : FAUNA_ZONE_ON);
        if (zone != G->zone) {
            LOG("[coop] fauna: host zone %s for guest %d (%.0f m apart)\n",
                zone ? "on: our small animals are its" : "off", gid, peer ? apart : -1.0f);
            G->zone = zone, G->listed_n = 0;
        }
        if (!zone) continue;
        any = true;
        for (int i = 0; i < G->listed_n; i++) {
            struct FaunaListed *l = &G->listed[i];
            bool dup = false;
            for (int j = 0; j < told_n && !dup; j++) dup = told[j] == l->id;
            if (dup) continue;
            bool alive = critter_alive(&l->k);
            int32_t life = alive ? (int32_t)LD32(l->k.actor + ENEMY_LIFE) : 0;
            uint8_t why = !alive && !fauna_was_deleted(&l->k) && fauna_d(l->pos, me) <= FAUNA_CATCH_M ? COOP_FAUNA_CAUGHT
                          : alive && l->life > 0 && life <= 0                                    ? COOP_FAUNA_KILLED
                                                                                                  : 0;
            if (!why) continue;
            fauna_send_gone(l->id, l->species, l->flags, why, l->pos, area, "host");
            if (told_n < (int)(sizeof told / sizeof *told)) told[told_n++] = l->id;
        }
        memcpy(G->center, rs.pos, sizeof G->center);
    }
    if (any != fh.zone) fh.zone = any, fh.placed_n = 0, fh.placed_ns = 0;
    if (!any) return;
    if (now - fh.placed_ns > 1000000000ull) {  // placed small animals near the guests, once a second
        fh.placed_ns = now;
        int keep = 0;
        for (int i = 0; i < fh.placed_n; i++)
            if (critter_alive(&fh.placed[i].k)) fh.placed[keep++] = fh.placed[i];
        fh.placed_n = keep;
        walk_actors(c, fauna_want, fauna_placed_seen, NULL);
    }
    for (int gid = 1; gid < COOP_MAX_PLAYERS; gid++)
        if (fh.g[gid].zone) fauna_host_set_for(gid, area, now);
}

// ---- guest
struct FaunaCopy {
    uint32_t id;
    uint8_t species, flags, missing;
    bool spawned, gone;
    struct Critter k;
    float target[3], vel[3], last[3], yaw, dist_me;
    int32_t life;
    uint64_t target_ns, born_ns;
    bool in_pool;  // (diagnostic) the game put our copy in its placement pool
    bool fled;     // the host's one is fleeing: keep up with it (no slack, a bigger step)
};
static struct {
    bool active;
    uint32_t seq;
    uint64_t suppress_ns, log_ns, placed_ns, check_ns;
    struct FaunaCopy copies[FAUNA_COPIES];
    struct { struct Critter k; } placed[FAUNA_PLACED];
    int placed_n;
    struct { uint32_t id; uint8_t flags; uint64_t until; } tomb[64];
    int tomb_next;
    unsigned suppressed, created, retired, refused, vanished;
    struct { uint32_t id; uint8_t fails; uint64_t until; } backoff[32];  // copies that keep going away
    int backoff_next;
    struct { uint8_t fails; uint64_t last, until; } kind[64];  // species whose fresh copies the game removes
} fg;
static bool fauna_tombed(uint32_t id, uint8_t flags, uint64_t now) {
    for (int i = 0; i < 64; i++)
        if (fg.tomb[i].id == id && fg.tomb[i].flags == flags && now < fg.tomb[i].until) return true;
    return false;
}
static void fauna_tomb(uint32_t id, uint8_t flags, uint64_t now) {
    fg.tomb[fg.tomb_next].id = id, fg.tomb[fg.tomb_next].flags = flags, fg.tomb[fg.tomb_next].until = now + 60000000000ull;
    fg.tomb_next = (fg.tomb_next + 1) % 64;
}
static void fauna_retire(Cpu *c, struct FaunaCopy *f) {
    if (f->spawned && critter_alive(&f->k) && fauna_free(f->k.actor)) despawn_now(c, f->k.actor, ACTOR_DELETE_REASON);
    if (f->id) fg.retired++;
    memset(f, 0, sizeof *f);
}
static void fauna_guest_placed_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)name, (void)ctx;
    float p[3];
    if (fg.placed_n >= FAUNA_PLACED || actor_dist(a, fauna_me(), p) > COOP_FAUNA_R + 12 || wildlife_ecosystem_actor(a)) return;
    struct Critter k = fauna_handle(a);
    for (int i = 0; i < fg.placed_n; i++)
        if (fg.placed[i].k.actor == a && fg.placed[i].k.unit_id == k.unit_id) return;
    for (int i = 0; i < FAUNA_COPIES; i++)
        if (fg.copies[i].id && fg.copies[i].spawned && fg.copies[i].k.actor == a) return;  // (our copy)
    k.hash = actor_hash(c, a);
    if (k.hash) fg.placed[fg.placed_n++].k = k;
}
static uint64_t fauna_guest_placed(uint32_t hash) {
    for (int i = 0; i < fg.placed_n; i++)
        if (fg.placed[i].k.hash == hash && critter_alive(&fg.placed[i].k)) return fg.placed[i].k.actor;
    return 0;
}
// A copy (or our placed one) gone next to our Link or with life 0: caught / killed here, sent once (true).
static bool fauna_check_gone(struct FaunaCopy *cp, uint64_t now, uint32_t area) {
    if (cp->gone) return true;
    bool alive = critter_alive(&cp->k);
    int32_t life = alive ? (int32_t)LD32(cp->k.actor + ENEMY_LIFE) : 0;
    uint8_t why = 0;
    if (!alive && cp->dist_me <= FAUNA_CATCH_M) why = COOP_FAUNA_CAUGHT;
    else if (alive && cp->life > 0 && life <= 0) why = COOP_FAUNA_KILLED;
    if (alive) cp->life = life;
    if (!why) return false;
    fauna_send_gone(cp->id, cp->species, cp->flags, why, cp->last, area, "guest");
    fauna_tomb(cp->id, cp->flags, now);
    cp->gone = true;
    return true;
}
// A copy that went away by itself (not caught): logged; one that keeps going is not made again for a while.
// (2026-11-19) The game removes a fresh copy of a species that is out of its time of day / weather (ActorInfo
// invalidTimes / invalidWeathers: e.g. fireflies Animal_Insect_E outside the night, birds at night); the host can still
// have such animals (made before the hour changed). Two fresh copies of a species gone within 8 s each: that species
// is not copied for 60 s, then tried again.
#define FAUNA_FRESH_NS 8000000000ull
static bool fauna_backed_off(uint32_t id, uint8_t species, uint64_t now) {
    if (species < 64 && now < fg.kind[species].until) return true;
    for (int i = 0; i < 32; i++)
        if (fg.backoff[i].id == id) return now < fg.backoff[i].until;
    return false;
}
static void fauna_vanished(struct FaunaCopy *cp, uint64_t now) {
    if (!cp->spawned) return;
    fg.vanished++;
    int b = -1;
    for (int i = 0; i < 32 && b < 0; i++)
        if (fg.backoff[i].id == cp->id) b = i;
    if (b < 0) {
        b = fg.backoff_next, fg.backoff_next = (fg.backoff_next + 1) % 32;
        fg.backoff[b].id = cp->id, fg.backoff[b].fails = 0;
    }
    uint8_t fails = ++fg.backoff[b].fails;
    bool kind_off = false;
    if (cp->species < 64 && now - cp->born_ns < FAUNA_FRESH_NS) {
        typeof(fg.kind[0]) *k = &fg.kind[cp->species];
        if (now - k->last > 60000000000ull) k->fails = 0;
        k->last = now;
        if (++k->fails >= 2) k->until = now + 60000000000ull, k->fails = 0, kind_off = true;
    }
    fg.backoff[b].until = fails >= 2 ? now + (fails >= 4 ? 60000000000ull : 15000000000ull) : 0;
    if (fg.vanished <= 30)
        LOG("[coop] fauna: copy %s %08x went away by itself after %.1f s, %.1f m from us (in pool %d, %u times)%s\n",
            coop_fauna_name(cp->species), cp->id, (double)(now - cp->born_ns) / 1e9, cp->dist_me, cp->in_pool, fails,
            kind_off ? ": this kind is not copied for 60 s (out of its time/weather here?)"
            : fails >= 2 ? ": not made again for a while" : "");
}
static void fauna_guest_off(Cpu *c, const char *why) {
    if (!fg.active) return;
    for (int i = 0; i < FAUNA_COPIES; i++)
        if (fg.copies[i].id) fauna_retire(c, &fg.copies[i]);
    LOG("[coop] fauna: guest zone off (%s): %u copies made, %u retired, %u own animals removed\n", why, fg.created,
        fg.retired, fg.suppressed);
    fg.active = false, fg.seq = 0, fg.placed_n = 0;
}
// (2026-11-19) The guest's own spawner stands still for birds, fish and insects while the zone is on, instead of
// making animals we delete again. 1.9.0 AutoPlacementMgr (FUN_7100b19424, the "NonAutoPlacement*" event action):
// per flow type a stop counter at +0x171e4c + 4*type and a stop byte at +0x171e68 + type that the manager's update
// (FUN_7100b1589c) counts down each frame while the counter is 0. Types (from the action's parameter order, Animal,
// Bird, Enemy, Fish, Insect, Material -> 1, 4, 0, 3, 2, 5): Insect 2, Fish 3, Bird 4. We only hold the byte (no
// counter), so the game lets go by itself 5 frames after we stop. Tested 2026-11-19: no own animals made, none
// deleted - but the stop is global, so the guest also gets none 45-100 m away (copies can't fill that: the game
// deletes copies made 50+ m from Link). So OFF by default: SAVAGEWIND_COOP_FAUNA_GATE=1 turns it on.
#define FAUNA_APM_STOP 0x171e68
static void fauna_gate(void) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("SAVAGEWIND_COOP_FAUNA_GATE"); on = v && !strcmp(v, "1"); }
    uint64_t mgr = wildlife_pool();
    if (!on || !in_guest(mgr)) return;
    static const uint8_t types[] = {2, 3, 4};
    for (int i = 0; i < 3; i++)
        if (LD8(mgr + FAUNA_APM_STOP + types[i]) < 5) ST8(mgr + FAUNA_APM_STOP + types[i], 5);
}
static void fauna_guest_frame(Cpu *c, uint64_t now) {
    CoopFauna f;
    double age = 99;
    uint32_t seq = 0;
    char an[96];
    uint32_t area = local_area(an, sizeof an);
    const float *me = fauna_me();
    bool got = coop_net_remote_fauna(&f, &age, &seq);
    bool active = got && age < 1.5 && area && f.area == area && fauna_d(f.center, me) < 30;
    if (!active) {
        if (fg.active) fauna_guest_off(c, !got || age >= 1.5 ? "no host set" : f.area != area ? "other area" : "moved");
        return;
    }
    if (!fg.active) {
        LOG("[coop] fauna: guest zone on: the host's small animals are shown here, ours near us removed\n");
        fg.active = true, fg.created = fg.retired = fg.suppressed = fg.refused = 0;
        fg.log_ns = now;
    }
    fauna_gate();
    if (seq != fg.seq) {  // a new host set
        fg.seq = seq;
        if (now - fg.placed_ns > 1000000000ull) {
            fg.placed_ns = now;
            int keep = 0;
            for (int i = 0; i < fg.placed_n; i++)
                if (critter_alive(&fg.placed[i].k)) fg.placed[keep++] = fg.placed[i];
            fg.placed_n = keep;
            walk_actors(c, fauna_want, fauna_guest_placed_seen, NULL);
        }
        bool seen[FAUNA_COPIES] = {0};
        int made = 0;
        for (int i = 0; i < f.count; i++) {
            const CoopFaunaAnimal *a = &f.a[i];
            uint8_t af = a->flags & COOP_FAUNA_PLACED;  // (FLED is not part of who it is)
            if (fauna_tombed(a->id, af, now)) continue;
            struct FaunaCopy *cp = NULL, *free_slot = NULL;
            for (int j = 0; j < FAUNA_COPIES; j++) {
                if (fg.copies[j].id == a->id && fg.copies[j].flags == af) { cp = &fg.copies[j]; seen[j] = true; break; }
                if (!fg.copies[j].id && !free_slot) free_slot = &fg.copies[j];
            }
            float yaw = a->yaw * (3.14159265f / 32767.0f);
            if (cp && fauna_check_gone(cp, now, area)) continue;
            if (cp && !critter_alive(&cp->k)) {  // (ours went away without a catch: made / found again below)
                seen[cp - fg.copies] = false;
                fauna_vanished(cp, now);
                memset(cp, 0, sizeof *cp);
                free_slot = cp, cp = NULL;
            }
            if (!cp) {
                if (!free_slot) {
                    for (int j = 0; j < FAUNA_COPIES && !free_slot; j++)
                        if (!fg.copies[j].id) free_slot = &fg.copies[j];
                    if (!free_slot) continue;
                }
                uint64_t actor = 0;
                bool spawned = false;
                if (a->flags & COOP_FAUNA_PLACED) actor = fauna_guest_placed(a->id);
                else if (made < 3 && !spawn_hold() && local_in_world() && !fauna_backed_off(a->id, a->species, now)) {
                    actor = spawn_now(c, coop_fauna_name(a->species), a->pos);
                    made++, spawned = true;
                    if (actor) fg.created++; else fg.refused++;
                    if (actor && fg.created <= 12)
                        LOG("[coop] fauna: guest copied %s %08x at %.1f %.1f %.1f (actor %llx)\n",
                            coop_fauna_name(a->species), a->id, a->pos[0], a->pos[1], a->pos[2], (unsigned long long)actor);
                }
                if (!actor) continue;
                cp = free_slot;
                memset(cp, 0, sizeof *cp);
                cp->id = a->id, cp->species = a->species, cp->flags = af, cp->spawned = spawned;
                cp->k = fauna_handle(actor);
                if (a->flags & COOP_FAUNA_PLACED) cp->k.hash = a->id;
                cp->life = (int32_t)LD32(actor + ENEMY_LIFE);
                cp->dist_me = 999;  // (until the first follow step)
                memcpy(cp->target, a->pos, sizeof cp->target);
                cp->target_ns = cp->born_ns = now;
                cp->in_pool = spawned && wildlife_ecosystem_actor(actor);
                seen[cp - fg.copies] = true;
            } else {
                float dt = (float)((now - cp->target_ns) / 1e9);
                for (int k = 0; k < 3; k++) {
                    float v = dt > 0.05f && dt < 1.0f ? (a->pos[k] - cp->target[k]) / dt : 0;
                    cp->vel[k] = v > 20 ? 20 : v < -20 ? -20 : v;
                }
                memcpy(cp->target, a->pos, sizeof cp->target);
                cp->target_ns = now;
            }
            cp->yaw = yaw, cp->missing = 0, cp->fled = (a->flags & COOP_FAUNA_FLED) != 0;
        }
        for (int j = 0; j < FAUNA_COPIES; j++) {  // gone from the host's set (gone there, or out of range)
            struct FaunaCopy *cp = &fg.copies[j];
            if (!cp->id || seen[j]) continue;
            if (cp->gone || ++cp->missing >= 3) fauna_retire(c, cp);
        }
    }
    // Our own ecosystem small animals near us: the host's are shown instead (twice a second).
    if (now - fg.suppress_ns > 500000000ull && local_in_world()) {
        fg.suppress_ns = now;
        uint64_t mgr = wildlife_pool();
        for (int i = 0; in_guest(mgr) && i < WILDLIFE_POOL_SIZE; i++) {
            uint64_t a = wildlife_slot(mgr, i);
            if (!in_guest(a) || !coop_fauna_species((const char *)(uintptr_t)(a + 0x1c))) continue;
            struct Critter k = fauna_handle(a);
            float p[3];
            if (!k.unit_id || !critter_alive(&k) || !fauna_free(a) || actor_dist(a, me, p) > COOP_FAUNA_R) continue;
            bool copy = false;
            for (int j = 0; j < FAUNA_COPIES && !copy; j++) copy = fg.copies[j].id && fg.copies[j].k.actor == a;
            if (!copy && despawn_now(c, a, ACTOR_DELETE_REASON)) fg.suppressed++;
        }
    }
    // Caught / killed here (every 100 ms): a copy gone next to our Link, or its life 0.
    bool check = now - fg.check_ns > 100000000ull;
    if (check) fg.check_ns = now;
    for (int j = 0; j < FAUNA_COPIES; j++) {
        struct FaunaCopy *cp = &fg.copies[j];
        if (!cp->id || cp->gone || (check && fauna_check_gone(cp, now, area))) continue;
        if (!critter_alive(&cp->k)) continue;
        float cur[3];
        cp->dist_me = actor_dist(cp->k.actor, me, cur);
        memcpy(cp->last, cur, sizeof cur);
        if (!fauna_free(cp->k.actor)) continue;
        // Follow: where the host's animal should be now; a drift beyond the kind's slack is pulled in (snapped when far).
        float lead = (float)((now - cp->target_ns) / 1e9);
        if (lead > 0.4f) lead = 0.4f;
        float to[3];
        for (int k = 0; k < 3; k++) to[k] = cp->target[k] + cp->vel[k] * lead;
        const char *nm = coop_fauna_name(cp->species);
        float slack = !strncmp(nm, "Animal_Insect_", 14) || !strncmp(nm, "Animal_Fish_", 12) ? 1.0f : 2.0f;
        if (cp->fled) slack = 0.5f;
        float d = fauna_d(cur, to);
        if (d <= slack) continue;
        float step = d > 12 ? 1.0f : cp->fled ? 0.5f : 0.2f;
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = cur[k] + (to[k] - cur[k]) * step;
        fauna_move(c, cp->k.actor, p, cp->yaw);
    }
    if (now - fg.log_ns > 10000000000ull) {
        fg.log_ns = now;
        int live = 0;
        for (int j = 0; j < FAUNA_COPIES; j++) live += fg.copies[j].id && critter_alive(&fg.copies[j].k);
        LOG("[coop] fauna: guest: host lists %d, %d followed here (%u made, %u refused, %u retired), %u own removed\n",
            f.count, live, fg.created, fg.refused, fg.retired, fg.suppressed);
    }
}

// EV 26 from the other game (gate.inc world_event_from, game thread).
static void fauna_event(Cpu *c, const uint8_t *data, int len, int origin) {
    if (!fauna_on() || len != (int)sizeof(CoopFaunaGone) || !local_in_world() || !player_info_ptr()) return;
    CoopFaunaGone g;
    memcpy(&g, data, sizeof g);
    char an[96];
    if (!coop_fauna_gone_ok(&g) || g.area != local_area(an, sizeof an)) return;
    const char *name = coop_fauna_name(g.species);
    uint64_t now = plat_now_ns(), a = 0;
    if (coop_net_is_host()) {
        if (origin <= 0 || !fh.g[origin < COOP_MAX_PLAYERS ? origin : 0].zone) return;  // (a guest in the zone)
        for (int gid = 1; gid < COOP_MAX_PLAYERS && !a; gid++)
            for (int i = 0; i < fh.g[gid].listed_n && !a; i++) {
                struct FaunaListed *l = &fh.g[gid].listed[i];
                if (l->id == g.id && l->flags == g.flags && critter_alive(&l->k)) a = l->k.actor;
            }
        if (!a && !(g.flags & COOP_FAUNA_PLACED)) {
            uint64_t mgr = wildlife_pool();
            for (int i = 0; in_guest(mgr) && i < WILDLIFE_POOL_SIZE && !a; i++) {
                uint64_t s = wildlife_slot(mgr, i);
                if (in_guest(s) && LD32(s + PROC_ID) == g.id && critter_alive(&(struct Critter){s, LD64(s + PROC_UNIT), g.id}))
                    a = s;
            }
        }
        if (!a && (g.flags & COOP_FAUNA_PLACED)) {
            struct HashHit hit = {g.id, 0};
            walk_actors(c, fauna_want, find_hashid, &hit);
            a = hit.a;
        }
        if (a) {
            fh.deleted[fh.deleted_next].actor = a, fh.deleted[fh.deleted_next].unit_id = LD32(a + PROC_ID);
            fh.deleted_next = (fh.deleted_next + 1) % 16;
        }
    } else {  // (from the host, or another guest's catch the host passes on: ids are the host's either way)
        fauna_tomb(g.id, g.flags, now);
        for (int j = 0; j < FAUNA_COPIES; j++) {
            struct FaunaCopy *cp = &fg.copies[j];
            if (cp->id != g.id || cp->flags != g.flags) continue;
            if (critter_alive(&cp->k)) a = cp->k.actor;
            memset(cp, 0, sizeof *cp);
        }
        if (!a && (g.flags & COOP_FAUNA_PLACED)) {
            a = fauna_guest_placed(g.id);
            if (!a) {
                struct HashHit hit = {g.id, 0};
                walk_actors(c, fauna_want, find_hashid, &hit);
                a = hit.a;
            }
        }
    }
    bool gone = a && fauna_free(a) && despawn_now(c, a, ACTOR_DELETE_REASON);
    LOG("[coop] fauna: %s %08x %s by the other player: %s here\n", name, g.id,
        g.why == COOP_FAUNA_KILLED ? "killed" : "caught", gone ? "removed" : a ? "removal FAILED" : "not found");
}

// `coop fauna` (dev): the small animals around our Link to the log.
static volatile int fauna_census_dev;
static void fauna_census_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    int *n = ctx;
    float p[3], d = actor_dist(a, fauna_me(), p);
    if (d > 100) return;
    bool pool = wildlife_ecosystem_actor(a), copy = false;
    for (int j = 0; j < FAUNA_COPIES && !copy; j++) copy = fg.copies[j].id && fg.copies[j].k.actor == a;
    uint32_t h = pool || copy ? 0 : actor_hash(c, a);
    if (n[0]++ < 60)
        LOG("[coop] fauna census: %s %s d %.1f at %.1f %.1f %.1f hash %u (%llx)\n", name,
            copy ? "copy" : pool ? "ecosystem" : h ? "placed" : "other", d, p[0], p[1], p[2], h, (unsigned long long)a);
    n[copy ? 1 : pool ? 2 : h ? 3 : 4]++;
}
static void fauna_frame(Cpu *c) {
    if (fauna_census_dev && local_in_world() && player_info_ptr()) {
        fauna_census_dev = 0;
        int n[5] = {0};
        walk_actors(c, fauna_want, fauna_census_seen, n);
        LOG("[coop] fauna census done: %d small animals within 100 m: %d copies, %d ecosystem, %d placed, %d other "
            "(zone %s)\n", n[0], n[1], n[2], n[3], n[4], coop_net_is_host() ? (fh.zone ? "on" : "off") : (fg.active ? "on" : "off"));
    }
    if (!fauna_on() || !player_info_ptr()) return;
    uint64_t now = plat_now_ns();
    if (!local_in_world()) {  // (a load deletes everything: forget, nothing is created meanwhile)
        if (fh.zone) {
            fh.zone = false, fh.placed_n = 0;
            for (int g = 0; g < COOP_MAX_PLAYERS; g++) fh.g[g].zone = false, fh.g[g].listed_n = 0;
        }
        if (fg.active) {
            memset(fg.copies, 0, sizeof fg.copies);
            fg.active = false, fg.seq = 0, fg.placed_n = 0;
        }
        return;
    }
    if (coop_net_is_host()) fauna_host_frame(c, now);
    else if (coop_net_partner_id() == 0) fauna_guest_frame(c, now);
    else fauna_guest_off(c, "not the world partner");
}

RT_ON_RESET(coop_fauna_reset) {
    memset(&fh, 0, sizeof fh);
    memset(&fg, 0, sizeof fg);
}
