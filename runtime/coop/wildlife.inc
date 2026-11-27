// Host ecosystem critters around the guest. Each poll also acknowledges the last
// complete roster. Only animals offered in that roster are eligible for a catch.
struct WildCopy {
    struct Critter handle;
    uint32_t id, species;
    float distance;
    uint64_t missing;
    bool caught;
};
static struct {
    CoopWildRoster roster;
    struct WildCopy copies[COOP_WILD_MAX];
    uint64_t sent, received, sampled, session;
    uint32_t waiting;
    int peer;
} wild;
static uint32_t wild_serial;  // survives a game reboot so delayed replies cannot match a new poll

static bool wild_here(CoopState *peer, uint32_t *area, uint64_t *session) {
    char name[96];
    double age;
    if (!look_on("SAVAGEWIND_COOP_WILDLIFE") || !local_in_world() || !player_info_ptr() || spawn_hold()) return false;
    bool heard = coop_net_is_host() ? coop_net_get_remote(peer, &age) : coop_net_get_host(peer, &age, session);
    if (!heard || age >= 1 || !(peer->flags & COOP_FLAG_IN_WORLD)) return false;
    *area = local_area(name, sizeof name);
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    return *area && *area == peer->area && dist3(me, peer->pos) <= 85;
}

static bool wildlife_host_ready(void) {
    CoopState peer;
    uint32_t area;
    uint64_t session = 0;
    return wild.received && plat_now_ns() - wild.received < 2000000000ull &&
           wild_here(&peer, &area, &session) && area == wild.roster.area && session == wild.session;
}

static bool wild_eligible(Cpu *c, uint64_t a) {
    return a != local_held() && !drop_by_actor(a) && !drop_was_held(a) && !actor_hash(c, a);
}

static void wild_scan(Cpu *c, CoopWildRoster *out, const CoopWildPoll *poll, float suppress) {
    uint64_t mgr = wildlife_pool();
    if (!in_guest(mgr)) return;
    for (int slot = 0; slot < WILDLIFE_POOL_SIZE; slot++) {
        uint64_t a = wildlife_slot(mgr, slot);
        if (!in_guest(a) || strncmp((const char *)(uintptr_t)(a + 0x1c), "Animal_Insect_", 14)) continue;
        struct Critter k = {.actor = a, .unit = LD64(a + PROC_UNIT), .unit_id = LD32(a + PROC_ID)};
        if (!k.unit_id || !critter_alive(&k)) continue;
        uint32_t species = 1;
        while (coop_wild_name(species) && strncmp(coop_wild_name(species), (const char *)(uintptr_t)(a + 0x1c), 48))
            species++;
        if (!coop_wild_name(species)) continue;
        CoopWildAnimal item = {.id = k.unit_id, .species = species};
        float distance = actor_dist(a, out->center, item.pos);
        if (!(distance <= 24) || !wild_eligible(c, a)) continue;
        if (!poll) {
            if (distance <= suppress) despawn_now(c, a, ACTOR_DELETE_REASON);
            continue;
        }
        bool remove = false;
        if (distance <= 8 && poll->previous && poll->previous == wild.roster.token &&
            poll->area == wild.roster.area && plat_now_ns() - wild.received < 4000000000ull) {
            for (int i = 0; i < COOP_WILD_MAX; i++) {
                const CoopWildAnimal *old = &wild.roster.animals[i];
                if (old->id != item.id || old->species != species) continue;
                for (int j = 0; j < COOP_WILD_MAX; j++)
                    if (poll->caught[j] == item.id) remove = true;
            }
        }
        if (remove && despawn_now(c, a, ACTOR_DELETE_REASON)) {
            LOG("[coop] wildlife: guest caught %s %u, host removed original\n", coop_wild_name(species), item.id);
            continue;
        }
        item.yaw = atan2f(LD32F(a + ACTOR_MTX + 8), LD32F(a + ACTOR_MTX + 40));
        for (int i = 0; i < COOP_WILD_MAX; i++) {
            CoopWildAnimal *at = &out->animals[i];
            if (at->id && dist3(at->pos, out->center) <= distance) continue;
            memmove(at + 1, at, (COOP_WILD_MAX - 1 - i) * sizeof *at);
            *at = item;
            break;
        }
    }
}

static bool wild_retire(Cpu *c, struct WildCopy *copy) {
    uint64_t a = copy->handle.actor;
    if (critter_alive(&copy->handle) && a != local_held() && !drop_was_held(a) && !drop_by_actor(a) &&
        !despawn_now(c, a, ACTOR_DELETE_REASON)) return false;
    memset(copy, 0, sizeof *copy);
    return true;
}

static void wild_apply(Cpu *c, CoopWildRoster *r, bool share) {
    bool ready = true;
    for (int i = 0; i < COOP_WILD_MAX; i++) {
        struct WildCopy *copy = &wild.copies[i];
        if (!copy->id) continue;
        int j = 0;
        while (j < COOP_WILD_MAX && r->animals[j].id != copy->id) j++;
        if ((j == COOP_WILD_MAX || r->animals[j].species != copy->species) && !wild_retire(c, copy)) ready = false;
    }
    float radius = 24;
    for (int i = 0; i < COOP_WILD_MAX && r->animals[i].id; i++) {
        const CoopWildAnimal *item = &r->animals[i];
        struct WildCopy *copy = NULL;
        for (int j = 0; j < COOP_WILD_MAX; j++) {
            if (wild.copies[j].id == item->id) { copy = &wild.copies[j]; break; }
            if (!wild.copies[j].id) copy = &wild.copies[j];
        }
        if (!copy) { ready = false; continue; }
        if (i == COOP_WILD_MAX - 1) radius = dist3(item->pos, r->center);
        if (copy->caught) continue;  // a poll already in flight can still list this catch
        const char *name = coop_wild_name(item->species);
        if (!critter_alive(&copy->handle)) {
            if (copy->id && (!copy->missing || plat_now_ns() - copy->missing < 1200000000ull)) {
                ready = false;
                continue;
            }
            float pos[3];
            memcpy(pos, item->pos, sizeof pos);
            uint64_t a = spawn_now(c, name, pos);
            if (!a) { ready = false; continue; }
            *copy = (struct WildCopy){.id = item->id, .species = item->species,
                .handle = {.actor = a, .unit = LD64(a + PROC_UNIT), .unit_id = LD32(a + PROC_ID)}};
            copy->distance = dist3(pos, (const float *)(uintptr_t)(player_info_ptr() + 0x88));
            LOG("[coop] wildlife: guest copied %s %u (actor %llx)\n", name, item->id, (unsigned long long)a);
        } else {
            uint64_t a = copy->handle.actor;
            if (a == local_held() || drop_was_held(a) || drop_by_actor(a)) { ready = false; continue; }
            float q[4] = {0, sinf(item->yaw * 0.5f), 0, cosf(item->yaw * 0.5f)};
            uint64_t body = enemy_body(a), rb = body ? LD64(body + 0x18) : 0;
            if (in_guest(rb) && LD64(rb + 0x78) == body && !(LD32(rb + 0x68) & 1)) {
                float pos[3], m[12];
                drop_read_pos(a, pos);
                // Use the actor-to-body offset; the body's origin need not be at the feet.
                for (int j = 0; j < 3; j++)
                    pos[j] = item->pos[j] + LD32F(body + 0x1a0 + j*4) - pos[j];
                quat_mtx(q, pos, m);
                rb_set_transform(c, rb, m);
            } else enemy_put_mtx(a, item->pos, q);
        }
    }
    if (ready && share) wild_scan(c, r, NULL, radius);
}

static void wild_sample(const float me[3], uint64_t now);
// (bugs B5, 2026-11-19) Two players catching the same runtime creature at almost the same time: the guest caught its
// copy (it tells the host in the poll: caught[]), the host caught the original (it is gone, and the host's pouch
// gained one of that species a moment ago). One creature, two inventory gains. The host arbitrates: the guest's catch
// stands (it was already accepted in the guest's game), the host gives its own gain back (drop_give_back: one of that
// species, never more than one per creature id, and only for a gain seen in the last WILD_RACE_NS). A second creature
// of the same species caught independently by the host is not touched: its original was still in the world when the
// guest's catch of the other id arrived, or it has no gain to give back. SAVAGEWIND_COOP_CATCHONE=0: as before.
#define WILD_RACE_NS 3000000000ull
static int catchone_c = -1;
static bool catchone_on(void) { return env_on("SAVAGEWIND_COOP_CATCHONE", &catchone_c); }
static struct { int count; uint64_t at; } wild_host_gain[32];  // (host) our own pouch gains per species, not yet attributed
static int wild_host_counts[32];
static bool wild_host_baseline;
static uint32_t wild_race_done[8];  // creature ids already arbitrated (ring)
static int wild_race_n;
static void wild_host_sample(uint64_t now) {
    if (!catchone_on()) return;
    static struct PouchCount pouch[POUCH_MAX];
    int n = pouch_read(pouch);
    int counts[32] = {0};
    for (int i = 0; i < n; i++) {
        if (strncmp(pouch[i].name, "Animal_Insect_", 14)) continue;
        for (int s = 1; coop_wild_name(s) && s < 32; s++)
            if (!strcmp(pouch[i].name, coop_wild_name(s))) counts[s] = pouch[i].count;
    }
    for (int s = 1; wild_host_baseline && n >= 0 && coop_wild_name(s) && s < 32; s++) {
        if (wild_host_gain[s].count && now - wild_host_gain[s].at > WILD_RACE_NS) wild_host_gain[s].count = 0;
        if (counts[s] > wild_host_counts[s]) {
            wild_host_gain[s].count += counts[s] - wild_host_counts[s];
            wild_host_gain[s].at = now;
        }
    }
    wild_host_baseline = n >= 0;
    memcpy(wild_host_counts, counts, sizeof counts);
}
static bool wild_original_alive(uint32_t id) {
    uint64_t mgr = wildlife_pool();
    for (int slot = 0; in_guest(mgr) && slot < WILDLIFE_POOL_SIZE; slot++) {
        uint64_t a = wildlife_slot(mgr, slot);
        if (!in_guest(a) || strncmp((const char *)(uintptr_t)(a + 0x1c), "Animal_Insect_", 14)) continue;
        struct Critter k = {.actor = a, .unit = LD64(a + PROC_UNIT), .unit_id = LD32(a + PROC_ID)};
        if (k.unit_id == id && critter_alive(&k)) return true;
    }
    return false;
}
static void wild_host_race(Cpu *c, const CoopWildPoll *poll, uint64_t now) {
    if (!catchone_on() || !poll->previous || poll->previous != wild.roster.token) return;
    wild_host_sample(now);
    for (int j = 0; j < COOP_WILD_MAX; j++) {
        uint32_t id = poll->caught[j];
        if (!id) continue;
        const CoopWildAnimal *old = NULL;
        for (int i = 0; i < COOP_WILD_MAX; i++)
            if (wild.roster.animals[i].id == id) old = &wild.roster.animals[i];
        if (!old) continue;  // (not a creature we offered)
        bool done = false;
        for (int i = 0; i < 8; i++) done |= wild_race_done[i] == id;
        if (done || wild_original_alive(id)) continue;  // (still here: the normal path removes it, no race)
        uint32_t s = old->species;
        if (s >= 32 || !wild_host_gain[s].count || now - wild_host_gain[s].at > WILD_RACE_NS) continue;
        wild_race_done[wild_race_n++ & 7] = id;
        wild_host_gain[s].count--;
        bool ok = drop_give_back(c, coop_wild_name(s));
        LOG("[coop] wildlife: %s %u caught by both players at once: the guest keeps it, host gave its own back%s\n",
            coop_wild_name(s), id, ok ? "" : " - NOTHING REMOVED");
        if (ok) {  // (the give-back is not a pouch change to count again)
            wild_host_counts[s] = wild_host_counts[s] > 0 ? wild_host_counts[s] - 1 : 0;
        }
    }
}
static void wildlife_event(Cpu *c, uint16_t type, const uint8_t *data, int len, int origin) {
    uint32_t area;
    CoopState peer;
    uint64_t session = 0;
    if (origin != coop_net_partner_id() || !wild_here(&peer, &area, &session) || session != wild.session) return;
    uint64_t now = plat_now_ns();
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    bool critters_near = dist3(me, peer.pos) <= 40 && !fauna_on();  // (fauna.inc shares them while it is on)
    if (type == COOP_EV_WILD_POLL && coop_net_is_host() && origin > 0 && len == sizeof(CoopWildPoll)) {
        CoopWildPoll poll;
        memcpy(&poll, data, sizeof poll);
        if (!coop_wild_poll_ok(&poll) || poll.area != area || !in_guest(wildlife_pool())) return;
        if (wild.peer != origin) wild.roster.token = 0;
        wild.peer = origin;
        wild_host_race(c, &poll, now);
        CoopWildRoster roster = {.area = area, .token = poll.token};
        if (!critters_near) {
            CoopWildReady ready = {area, poll.token};
            if (coop_net_send_event_to(origin, COOP_EV_WILD_ROSTER, &ready, sizeof ready))
                wild.roster = roster, wild.received = now;
            return;
        }
        memcpy(roster.center, peer.pos, sizeof roster.center);
        wild_scan(c, &roster, &poll, 0);
        if (coop_wild_roster_ok(&roster) && coop_net_send_event_to(origin, COOP_EV_WILD_ROSTER, &roster, sizeof roster))
            wild.roster = roster, wild.received = now;
    } else if (type == COOP_EV_WILD_ROSTER && !coop_net_is_host() && origin == 0 &&
               (len == sizeof(CoopWildRoster) || len == sizeof(CoopWildReady))) {
        CoopWildRoster roster = {0};
        memcpy(&roster, data, len);
        bool share = critters_near && len == sizeof roster;
        if (!coop_wild_roster_ok(&roster) || roster.area != area || roster.token != wild.waiting ||
            !wild.waiting || now - wild.sent > 2000000000ull || (share && dist3(roster.center, me) > 8)) return;
        if (!share) memset(roster.animals, 0, sizeof roster.animals);
        wild.waiting = 0;
        wild.received = now;
        wild.roster = roster;
        wild_sample(me, now);
        wild_apply(c, &roster, share);
    }
}

// At most one disappeared copy per unit gained in the pouch. If several of the
// same species vanish together, choose the nearest to Link at its last sample.
static int wild_counts[32];
static struct { int count; uint64_t at; } wild_gain[32];
static bool wild_baseline;
static void wild_sample(const float me[3], uint64_t now) {
    bool nearby = false;
    for (int i = 0; i < COOP_WILD_MAX; i++) {
        const CoopWildAnimal *offered = &wild.roster.animals[i];
        if (offered->id && dist3(offered->pos, me) < 8) nearby = true;
        struct WildCopy *copy = &wild.copies[i];
        if (!copy->id || copy->caught) continue;
        if (critter_alive(&copy->handle)) {
            float p[3];
            copy->distance = actor_dist(copy->handle.actor, me, p);
        } else if (!copy->missing) copy->missing = now;
        if (copy->distance < 8) nearby = true;
    }
    if (!nearby) {
        wild_baseline = false;
        memset(wild_gain, 0, sizeof wild_gain);
        return;
    }
    static struct PouchCount pouch[POUCH_MAX];
    int n = pouch_read(pouch);
    int counts[32] = {0};
    for (int i = 0; i < n; i++) {
        if (strncmp(pouch[i].name, "Animal_Insect_", 14)) continue;
        for (int s = 1; coop_wild_name(s); s++)
            if (!strcmp(pouch[i].name, coop_wild_name(s))) counts[s] = pouch[i].count;
    }
    for (int s = 1; wild_baseline && n >= 0 && coop_wild_name(s); s++) {
        if (now - wild_gain[s].at > 400000000ull) wild_gain[s].count = 0;
        if (counts[s] > wild_counts[s]) {
            wild_gain[s].count += counts[s] - wild_counts[s];
            wild_gain[s].at = now;
        }
        while (wild_gain[s].count > 0) {
            struct WildCopy *best = NULL;
            for (int i = 0; i < COOP_WILD_MAX; i++) {
                struct WildCopy *copy = &wild.copies[i];
                if (copy->species == (uint32_t)s && !copy->caught && copy->missing &&
                    now - copy->missing < 1200000000ull && copy->distance <= 4.5f &&
                    (!best || copy->distance < best->distance)) best = copy;
            }
            if (!best) break;
            best->caught = true;
            wild_gain[s].count--;
            LOG("[coop] wildlife: guest caught %s %u (pouch gain)\n", coop_wild_name(s), best->id);
        }
    }
    wild_baseline = n >= 0;
    memcpy(wild_counts, counts, sizeof counts);
}

static void wildlife_frame(Cpu *c) {
    uint64_t now = plat_now_ns();
    if (!look_on("SAVAGEWIND_COOP_WILDLIFE") || now - wild.sampled < 100000000ull) return;
    wild.sampled = now;
    CoopState peer;
    uint32_t area;
    uint64_t session = 0;
    if (!wild_here(&peer, &area, &session) || (wild.session && session != wild.session) ||
        (wild.roster.area && wild.roster.area != area) ||
        (wild.received && now - wild.received > 4000000000ull)) {
        if (local_in_world() && !spawn_hold()) {
            for (int i = 0; i < COOP_WILD_MAX; i++)
                if (!wild_retire(c, &wild.copies[i])) return;
        } else if (local_in_world()) return;  // defer cleanup across a cutscene
        memset(&wild, 0, sizeof wild);
        wild.sampled = now;
        wild_baseline = false;
        memset(wild_gain, 0, sizeof wild_gain);
        return;
    }
    if (coop_net_is_host()) {  // (bugs B5: the host watches its own pouch for a race with the guest's catch)
        wild_host_sample(now);
        return;
    }
    if (coop_net_partner_id() != 0) return;
    wild.session = session;
    if (wild.roster.animals[0].id) wild_sample((const float *)(uintptr_t)(player_info_ptr() + 0x88), now);
    else {
        wild_baseline = false;
        memset(wild_gain, 0, sizeof wild_gain);
    }
    if (now - wild.sent < (wild.waiting ? 2000000000ull : 500000000ull)) return;
    if (!++wild_serial) ++wild_serial;
    CoopWildPoll poll = {.area = area, .token = wild_serial, .previous = wild.roster.token};
    for (int i = 0; i < COOP_WILD_MAX; i++)
        if (wild.copies[i].caught) poll.caught[i] = wild.copies[i].id;
    if (coop_net_send_event_to(0, COOP_EV_WILD_POLL, &poll, sizeof poll)) {
        wild.sent = now;
        wild.waiting = poll.token;
    }
}

RT_ON_RESET(coop_wildlife_reset) {
    memset(&wild, 0, sizeof wild);
    memset(wild_host_gain, 0, sizeof wild_host_gain), memset(wild_race_done, 0, sizeof wild_race_done);
    wild_host_baseline = false;
    wild_baseline = false;
    memset(wild_gain, 0, sizeof wild_gain);
}
