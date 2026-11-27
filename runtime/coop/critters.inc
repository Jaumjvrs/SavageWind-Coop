// Part of runtime/coop.c (included there, same translation unit): placed small critters (birds, fish, insects) caught
// or killed in one game are gone in the other too. (2026-11-16, worker world2; protocol 56, event 19 COOP_EV_CRITTER)
//
// Research (probe, plateau forest -895 190 1760, 2026-11-16):
// within 150 m of a player the game had ~40-75 Animal_* actors. Most small critters are made at run time by the
// ecosystem (no map HashId): Animal_Insect_E/G/Q/S (~20-35), Animal_LittleBird_A (~2-17), Pigeons, a Hawk. Placed
// ones (HashId) were few: 4 Animal_Fish_A, 3 Animal_Pigeon_A, up to 5 Animal_LittleBird_A. The map has ~1100 placed
// insects, ~700 fish and ~800 birds in all (MainField), so a handful is near a player at a time.
// What is shared: a PLACED critter (HashId) that disappears right next to our Link (caught: insects and fish go into
// the pouch, the actor is deleted) or whose life drops to 0 (shot) is sent (HashId + area); the other game deletes its
// own copy of it (if loaded; else it waits up to 2 min for it to load). Not shared: run-time critters (each game's own
// ecosystem makes different ones; showing the host's in the guest's game would mean streaming dozens of small, fast
// actors at 20 Hz in the 16-slot enemy packet, crowding out the enemies - not worth it for ambient life), and where
// placed critters fly / swim (each game's own AI; they are not followed). Off by default; SAVAGEWIND_COOP_CRITTERS=1: on.
// Dev: `coop critterkill <actor hex>` deletes a critter here (as a catch would).
#define CRITTER_MAX 128
#define CRITTER_NEAR_M 60.0f  // watched within this of our Link
#define CRITTER_CATCH_M 4.5f  // gone this close to our Link (last scan): caught
#define CRITTER_SCAN_NS 250000000ull
#define CRITTER_WAIT 16

static bool critters_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_CRITTERS");
    return on;
}
static bool critter_name(const char *n) {
    static const char *const kinds[] = {"Insect_", "Fish_", "LittleBird_", "Pigeon_", "Crow_", "Gull_", "Hawk_",
                                        "Heron_", "WildDuck_", "Ptarmigan_"};
    if (strncmp(n, "Animal_", 7) || strstr(n, "Swarm")) return false;
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (!strncmp(n + 7, kinds[i], strlen(kinds[i]))) return true;
    return false;
}
struct Critter { uint64_t actor, unit; uint32_t unit_id, hash; float pos[3]; int32_t life; bool seen; };
static struct Critter critters[CRITTER_MAX];
static int critter_n;
static struct { uint64_t actor; uint32_t unit_id; } critter_nohash[256];  // run-time critters (HashId 0): not asked again
static int critter_nohash_next;
static uint32_t critter_applied[32];  // HashIds we deleted for the other player (their disappearance is no catch)
static int critter_applied_next;
static struct { uint32_t hash; uint64_t until_ns; } critter_wait[CRITTER_WAIT];
static volatile uint64_t critter_kill_dev;  // `coop critterkill <actor>`
static float critter_me[3];

static bool critter_was_applied(uint32_t h) {
    for (int i = 0; i < 32; i++)
        if (critter_applied[i] == h) return true;
    return false;
}
static bool critter_alive(const struct Critter *k) {
    if (!in_guest(k->actor) || !in_guest(k->unit)) return false;
    return LD32(k->unit + UNIT_ID) == k->unit_id && LD64(k->unit + UNIT_ACTOR) == k->actor && LD8(k->actor + PROC_STATE) != 3;
}
static void critter_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)ctx;
    float p[3];
    if (actor_dist(a, critter_me, p) > CRITTER_NEAR_M) return;
    for (int i = 0; i < critter_n; i++)
        if (critters[i].actor == a && critters[i].unit_id == LD32(a + PROC_ID)) { critters[i].seen = true; return; }
    uint32_t uid = LD32(a + PROC_ID);
    for (int i = 0; i < 256; i++)
        if (critter_nohash[i].actor == a && critter_nohash[i].unit_id == uid) return;
    uint32_t h = actor_hash(c, a);
    if (!h) {
        critter_nohash[critter_nohash_next].actor = a, critter_nohash[critter_nohash_next].unit_id = uid;
        critter_nohash_next = (critter_nohash_next + 1) % 256;
        return;
    }
    if (critter_n == CRITTER_MAX) return;
    struct Critter *k = &critters[critter_n++];
    memset(k, 0, sizeof *k);
    k->actor = a, k->unit = LD64(a + PROC_UNIT), k->unit_id = LD32(a + PROC_ID), k->hash = h, k->seen = true;
    k->life = (int32_t)LD32(a + ENEMY_LIFE);
    memcpy(k->pos, p, 12);
    for (int w = 0; w < CRITTER_WAIT; w++)  // (the other player caught it before it loaded here)
        if (critter_wait[w].hash == h && plat_now_ns() < critter_wait[w].until_ns) {
            critter_wait[w].hash = 0;
            critter_applied[critter_applied_next] = h, critter_applied_next = (critter_applied_next + 1) % 32;
            bool gone = despawn_now(c, a, ACTOR_DELETE_REASON);
            LOG("[coop] critters: %s %u caught by the other player earlier: removed here now (%s)\n", name, h,
                gone ? "ok" : "FAILED");
            k->actor = 0;
        }
}
static void critter_send(const struct Critter *k, const char *why, uint32_t area) {
    CoopCritter e;
    memset(&e, 0, sizeof e);
    e.hash = k->hash, e.area = area;
    memcpy(e.pos, k->pos, 12);
    bool ok = coop_critter_ok(&e) && coop_net_send_event(COOP_EV_CRITTER, &e, sizeof e);
    LOG("[coop] critters: %u %s here at %.1f %.1f %.1f: %s\n", k->hash, why, k->pos[0], k->pos[1], k->pos[2],
        ok ? "sent" : "NOT sent");
}
static void critters_frame(Cpu *c) {
    static uint64_t next_ns;
    static uint32_t last_area;
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (!critters_on() || !local_in_world() || !player_info_ptr() || here != last_area) {
        critter_n = 0, last_area = here;  // (a load deletes them all: no catches)
        memset(critter_nohash, 0, sizeof critter_nohash);
        return;
    }
    uint64_t dk = critter_kill_dev;
    if (dk) {
        critter_kill_dev = 0;
        bool gone = in_guest(dk) && despawn_now(c, dk, ACTOR_DELETE_REASON);
        LOG("[coop] critters: dev kill %llx: %s\n", (unsigned long long)dk, gone ? "deleted" : "FAILED");
    }
    if (now < next_ns) return;
    next_ns = now + CRITTER_SCAN_NS;
    memcpy(critter_me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    for (int i = 0; i < critter_n; i++) critters[i].seen = false;
    walk_actors(c, critter_name, critter_seen, NULL);
    for (int i = 0; i < critter_n; i++) {
        struct Critter *k = &critters[i];
        bool alive = k->actor && k->seen && critter_alive(k);
        int32_t life = alive ? (int32_t)LD32(k->actor + ENEMY_LIFE) : 0;
        bool applied = critter_was_applied(k->hash);
        if (!alive) {
            float dx = k->pos[0] - critter_me[0], dy = k->pos[1] - critter_me[1], dz = k->pos[2] - critter_me[2];
            float d2 = dx * dx + dy * dy + dz * dz;
            if (k->actor && !applied && d2 < CRITTER_CATCH_M * CRITTER_CATCH_M)
                critter_send(k, "caught", here);
            else if (k->actor && !applied && d2 < 100.0f)  // (diagnostic: gone near us but not at hand)
                LOG("[coop] critters: %u gone %.1f m from us: no catch\n", k->hash, sqrtf(d2));
            critters[i--] = critters[--critter_n];
            continue;
        }
        if (k->life > 0 && life <= 0 && !applied) critter_send(k, "killed", here);
        k->life = life;
        float p[3];
        actor_dist(k->actor, critter_me, p);
        memcpy(k->pos, p, 12);
    }
}

// EV 19 from the other game (gate.inc world_event, game thread).
static void critter_ev(Cpu *c, const uint8_t *data, int len) {
    if (!critters_on() || len != (int)sizeof(CoopCritter)) return;
    CoopCritter e;
    memcpy(&e, data, sizeof e);
    if (!coop_critter_ok(&e)) { LOG("[coop] critters: bad event dropped\n"); return; }
    char area[96];
    if (e.area != local_area(area, sizeof area)) return;
    critter_applied[critter_applied_next] = e.hash, critter_applied_next = (critter_applied_next + 1) % 32;
    struct HashHit hit = {e.hash, 0};
    for (int i = 0; i < critter_n && !hit.a; i++)
        if (critters[i].hash == e.hash && critter_alive(&critters[i])) hit.a = critters[i].actor;
    if (!hit.a && local_in_world() && player_info_ptr()) walk_actors(c, critter_name, find_hashid, &hit);
    if (hit.a) {
        bool gone = despawn_now(c, hit.a, ACTOR_DELETE_REASON);
        LOG("[coop] critters: %u caught / killed by the other player: removed here (%s)\n", e.hash, gone ? "ok" : "FAILED");
        return;
    }
    for (int w = 0; w < CRITTER_WAIT; w++)
        if (!critter_wait[w].hash || plat_now_ns() > critter_wait[w].until_ns) {
            critter_wait[w].hash = e.hash, critter_wait[w].until_ns = plat_now_ns() + 120000000000ull;
            LOG("[coop] critters: %u caught / killed by the other player: not loaded here, waits\n", e.hash);
            return;
        }
}
