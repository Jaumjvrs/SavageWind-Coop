// Part of runtime/coop.c (included there, same translation unit): dropped and thrown items shared between the players,
// and thrown weapons hitting the other player.
//
// ---- dropped items (2026-11-11, protocol 32; a dropped item was not seen by the partner).
// SAVAGEWIND_COOP_DROPS=0 turns it off.
// Game facts (live, Zora's Domain 2026-11-11): an item dropped from the pouch menu ("Soltar") is a NEW actor of the
// item's own name (Weapon_Sword_002, Item_Fruit_A), no map HashId, created while the menu is still open about 2 m
// from Link; the pouch loses the item only when the menu closes. A thrown weapon (R) is the SAME actor Link held
// (the equipped sword's actor flies ~6 m in 0.5 s and lies there); the pouch loses it at the throw. Picking up a
// lying weapon with empty hands keeps that actor (it goes back on Link's hand bone); otherwise it is deleted. Actor
// proc ids (+0x60) count up with every creation (107 at boot ... 816, 818, 820, 822 for later spawns). A weapon
// actor's durability is the s32 at +0xd4c (2000 for a sword given "Weapon_Sword_001:2000"; the copy made by
// spawn_now has the item's default, 2700 for Weapon_Sword_003, set by its first update: see life_want). A
// weapon picked up with something already in the hand shows the "new item" box first (the game waits for A) and
// its actor is deleted only after that box.
// How it works:
//  - Our pouch is read 4 times a second; an item whose count went down opens a "pending drop" for 3 s. A pending drop
//    looks for the actor: same name, no HashId, within DROP_NEAR_M of our Link, not on one of Link's bones, newer than
//    the actor ids seen 90 s ago (or the weapon / shield / bow actor Link held: a thrown or dropped equipped item),
//    the newest one wins. Nothing found (eaten, sold, broken, cooked): no drop.
//  - Our held melee weapon flying away from Link (> 1.2 m from his chest, > 4 m/s) is followed from that frame on
//    (thrown); it becomes a drop once the pouch confirms it left (a boomerang caught again is forgotten).
//  - A drop that came to rest (and is not on Link's body) is sent (COOP_EV_DROP) while the other player is in the
//    same area within DROP_SHARE_M; the other game makes a copy with the game's own actor creation (spawn_now).
//  - Each game watches its item (the original or the copy). Picked up = the actor is gone or on our Link's bones AND
//    the pouch has more of that name than before: COOP_EV_DROP_GONE, the other game deletes its one. So whoever
//    picks it up first gets it, and the other world loses it. The original vanishing by itself (burnt, fell, the
//    area unloaded) deletes the copy too; a copy vanishing by itself is only forgotten.
//  - The world share's pouch diff (gate.inc world_frame) must not take a pick-up of a shared drop as an acquisition
//    of ours and send it again (the echo duplication of 7fc80d0): drops_owns_gain() tells it to skip that gain.
// Limits: the copy has no weapon modifier (bonus). Two players picking up within the network delay both got it until
// the DROPRACE fix below (2026-11-12): the guest now gives its one back.
static bool drops_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_DROPS") && !strcmp(getenv("SAVAGEWIND_COOP_DROPS"), "0"));
    return on;
}
// ---- (protocol 34; play-test 3: a drop only appeared for the other player when the dropper closed the
// inventory). Seen in the play-test logs (play-test logs 2026-11-11 21:33): a weapon dropped
// from the menu leaves the pouch only when the menu closes (Weapon_Bow_002: "left our pouch" right after "game running
// again"); its new actor already exists during the menu, ~1.5-2 m in front of Link (protocol 32 notes); a material
// ("Hold") leaves the pouch at once but its actor waits at the world origin until the menu closes, then sits in Link's
// hands (GrabPouchUpper) and is put down / thrown later (GrabPut) - that is the moment it is dropped. And every drop
// was sent only once it had lain still for 0.4 s.
// Now: (a) MENUDROP - while the game is paused (menu open), a new actor of a pouch item's name lying near our Link (or
// the weapon he held, moved off his body) is a drop at once and is sent right away (still in the air in our paused
// game: the copy falls with its own physics in the other game); the pouch confirms it when the menu closes. (b)
// DROPEARLY - every drop is sent as soon as it is off our Link, moving or not (COOP_DROP_F_MOVING), and once it comes
// to rest the same id is sent again (COOP_DROP_F_UPDATE): the other game moves its copy there if it lies elsewhere
// (more than DROP_FIX_M away). An item held in Link's hands is watched every frame, so putting it down is noticed in
// the same frame. SAVAGEWIND_COOP_MENUDROP=0 / SAVAGEWIND_COOP_DROPEARLY=0 turn (a) / (b) off (protocol 33 behaviour).
static bool menudrop_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_MENUDROP");
    return on;
}
static bool dropearly_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_DROPEARLY");
    return on;
}
#define DROP_FIX_M 0.6f        // a copy farther than this from the original's resting spot is moved there
#define DROP_UPDATES_MAX 6     // position updates per item (it was kicked / rolled away and came to rest again)
#define DROP_MAX 32
#define DROP_NEAR_M 4.0f       // a dropped item lies this close to our Link (menu drop: 1.7-1.9 m seen)
#define DROP_SHARE_M 60.0f     // shared with a partner this close (same area)
#define DROP_PENDING_NS 3000000000ull
#define DROP_WM_AGO 90         // s: a dropped item's actor is newer than the ids seen this long ago
#define DROP_REST_NS 400000000ull
#define DROP_LIFE_OFS 0xd4c    // weapon actor: durability (s32)
#define THROW_BODY_R 1.2f      // the other player's body (its 0.4 m collision capsule + the blade ahead of the grip; 34: was 0.8)
struct Drop {
    uint32_t id, pid;
    int32_t life;              // weapons: durability seen last (actor +0xd4c)
    int32_t life_want;         // a copy: the original's durability, written for its first frames (-1: none)
    unsigned life_frames;
    uint64_t actor, unit;
    bool ours, confirmed, sent, flying, hit_done, resting, held_last;
    float fly_dir[3];          // (34) a thrown weapon's last horizontal move direction (bounce check)
    bool menu;                 // (34) ours, found while the pouch menu is open (the pouch confirms it on menu close)
    bool kill_later;           // (34) picked up in the other game while our menu was open: deleted once it closes
    uint64_t kill_ns;          // (34) ... at this time (0.5 s after the menu closed)
    bool sent_moving;          // (34) sent before it came to rest: one update follows when it rests
    int updates;               // (34) position updates sent for it
    float sent_pos[3];         // (34) where it was when sent / last updated
    int32_t base;              // our pouch count of the name before a pick-up could happen
    uint32_t area;
    float pos[3];
    uint64_t made_ns, moved_ns, gone_ns, frame_ns;
    char name[COOP_DROP_NAME];
};
static struct Drop drops[DROP_MAX];
static int drop_n;
static uint32_t drop_counter;
// (2026-11-11 evening, split-screen test: food was never dropped, and nothing dropped while the menu stayed
// open) A pending drop is kept open past DROP_PENDING_NS while its item is still on its way: an actor of that name,
// newer than ref, (a) parked at the world origin (a material dropped from the menu waits at 0 1.2 0 until the menu
// closes: pid 1015 sat there 3 s in play-test/savagewind.log, the pouch had already lost it), or (b) on our Link's body /
// in his hands ("Hold" from the menu: GrabPouchUpper, the item is put down later with GrabPut; equipped gear dropped
// while the menu stays open). At most DROP_HOLD_NS in all.
#define DROP_HOLD_NS 180000000000ull
// (34) relook: opened for an item of ours that vanished from Link's hands (not a pouch decrease). watch: the item seen in
// Link's hands for this pending drop; drops_frame checks it every frame (no actor walk) and searches at once when it
// leaves the hands, so a put-down / thrown held item is shared without waiting for the next 0.25 s poll.
static struct {
    char name[COOP_DROP_NAME]; int n; uint32_t ref; uint64_t until_ns, first_ns; int logged, said_wait; bool relook;
    uint64_t watch, watch_unit; uint32_t watch_id;
} drop_pending[8];
static int drop_pending_n;
// our recent pick-ups of shared drops (id: the drop's id when we told the other game, 0 = not told / race settled)
static struct { char name[COOP_DROP_NAME]; uint64_t at_ns; uint32_t id; } drop_picked[4];
// ---- (2026-11-12, play-test 4: picking up a dropped item at the same moment duplicated it)
// Both games picked up their actor of the same drop before the other game's DROP_GONE arrived, and each had already
// forgotten the drop: both kept the item. Now the HOST keeps it and the guest gives its one back with the game's own
// "remove one item" (FUN_7100d93320(pouch mgr, name), called by an event action: the first NOT equipped entry of the
// name; a stack of 2+ loses 1, else the entry goes; the menu lists are updated). A weapon picked up with empty hands is
// equipped: then the equipped entry goes (FUN_7100be22a0: remove the equipped item by name + the equipment refresh).
// Noticed as: (a) a DROP_GONE (picked up there) for an id we picked up ourselves within DROP_RACE_NS, or (b) a
// DROP_GONE for our actor of it that is on our Link / already gone while our pouch had not risen yet: when the pouch
// rises within DROP_RACE_NS, the guest gives it back, the host sends its own DROP_GONE (the guest then sees (a)).
// No protocol change (same events). SAVAGEWIND_COOP_DROPRACE=0 turns it off.
#define A_POUCH_REMOVE1 0x7000d93320ull       // (pouch mgr, SafeString*): one of the first non-equipped entry
#define A_POUCH_REMOVE_EQUIPPED 0x7000be22a0ull  // (SafeString*): the equipped entry of the name + equipment refresh
#define DROP_RACE_NS 3000000000ull
static bool droprace_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_DROPRACE");
    return on;
}
static struct { uint32_t id; char name[COOP_DROP_NAME]; int32_t base; uint64_t until_ns; bool give; } drop_race[4];
static int drop_race_n;
// dev (a two-game test): SAVAGEWIND_COOP_DROPGONE_DELAY_MS=<ms> holds every received DROP_GONE that long, so two
// pick-ups "at the same moment" can be staged with real input.
static struct { uint8_t data[sizeof(CoopDropGone)]; uint64_t at_ns; } drop_gone_held[4];
static int drop_gone_held_n;
static bool drop_gone_replay;
static char drop_giveback_dev[COOP_DROP_NAME];  // `coop giveback <name>` (dev)
// (2026-11-17) the other player's drops that arrived while the spawn hold was on (coop.c spawn_hold: an event or a
// load runs here): made once it is over (drops_frame), instead of being lost.
static CoopDrop drop_hold_q[8];
static int drop_hold_n;
static struct PouchCount drop_pouch[POUCH_MAX];
static int drop_pouch_n = -1;
static uint32_t drop_wm[128];  // max proc id, one sample a second (ring)
static unsigned drop_wm_i;
static volatile int drops_dev;  // `coop drops`: the table, to the log

static bool drop_alive(const struct Drop *d) {
    return in_guest(d->actor) && in_guest(d->unit) && LD32(d->unit + UNIT_ID) == d->pid &&
           LD64(d->unit + UNIT_ACTOR) == d->actor && LD8(d->actor + PROC_STATE) != 3;
}
static void drop_read_pos(uint64_t a, float p[3]) {
    p[0] = LD32F(a + ACTOR_MTX + 0x0c), p[1] = LD32F(a + ACTOR_MTX + 0x1c), p[2] = LD32F(a + ACTOR_MTX + 0x2c);
}
static int32_t drop_pouch_count(const char *name) {
    for (int i = 0; i < drop_pouch_n; i++)
        if (!strcmp(drop_pouch[i].name, name)) return drop_pouch[i].count;
    return 0;
}
static bool drop_name_ok(const char *n) {
    if (!strncmp(n, "Weapon_", 7)) return true;
    if (!strncmp(n, "Item_", 5)) return strncmp(n, "Item_Cook_", 10) != 0;  // (meals can't be dropped)
    if (!strncmp(n, "Obj_", 4)) return strncmp(n, "Obj_Arrow", 9) && strncmp(n, "Obj_DRStone", 11);  // (shot arrows)
    // (2026-11-11 evening) materials whose actor name is not Item_*: insects / frogs / lizards, Courser Bee Honey
    if (!strncmp(n, "Animal_Insect_", 14) || !strcmp(n, "BeeHome")) return true;
    return false;
}
// The newest proc id now (every live process's unit; no guest calls).
static uint32_t proc_id_max(void) {
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t start = in_guest(pa) ? LD64(pa + PROC_UNIT) : 0;
    if (!in_guest(start)) return 0;
    uint64_t u = start, vt = LD64(start);
    uint32_t mx = 0;
    for (unsigned step = 0; step < 16384; step++) {
        uint64_t next = LD64(u + UNIT_NODE + 8);
        uint64_t a = LD64(u) == vt ? LD64(u + UNIT_ACTOR) : 0;
        if (in_guest(a) && LD32(u + UNIT_ID) == LD32(a + PROC_ID) && LD32(u + UNIT_ID) > mx) mx = LD32(u + UNIT_ID);
        if (!in_guest(next)) break;
        u = next - UNIT_NODE;
        if (u == start) break;
    }
    return mx;
}
// Our Link's chest and his nearest bone to p (bone_now: local_look's bones of this frame).
static void drop_me(float me[3]) {
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
}
static float drop_bone_dist(const float p[3]) {
    float m[12] = {1, 0, 0, p[0], 0, 1, 0, p[1], 0, 0, 1, p[2]}, d;
    nearest_bone(m, &d);
    return d;
}
static struct Drop *drop_by_actor(uint64_t a) {
    for (int i = 0; i < drop_n; i++)
        if (drops[i].actor == a) return &drops[i];
    return NULL;
}
static struct Drop *drop_by_id(uint32_t id) {
    for (int i = 0; i < drop_n; i++)
        if (drops[i].id == id) return &drops[i];
    return NULL;
}
static struct Drop *drop_new(uint64_t a, const char *name, bool ours, uint64_t now) {
    if (drop_n == DROP_MAX) {  // full: forget the oldest
        int o = 0;
        for (int i = 1; i < drop_n; i++) if (drops[i].made_ns < drops[o].made_ns) o = i;
        drops[o] = drops[--drop_n];
    }
    struct Drop *d = &drops[drop_n++];
    memset(d, 0, sizeof *d);
    d->actor = a, d->unit = LD64(a + PROC_UNIT), d->pid = LD32(a + PROC_ID);
    d->ours = ours, d->made_ns = d->moved_ns = d->frame_ns = now;
    snprintf(d->name, sizeof d->name, "%s", name);
    char area[96];
    d->area = local_area(area, sizeof area);
    drop_read_pos(a, d->pos);
    d->base = drop_pouch_count(name);
    d->life_want = -1;
    if (ours) d->id = COOP_DROP_ID | coop_net_id_tag() | (++drop_counter & 0x03ffffffu);
    return d;
}
static void drop_forget(struct Drop *d) { *d = drops[--drop_n]; }

// The other game's player, if in our area: where (false: not connected / elsewhere).
static bool drop_partner(float pos[3]) {
    CoopState rs;
    double age = 99;
    char area[96];
    if (!coop_net_get_remote(&rs, &age) || age > 3.0 || !(rs.flags & COOP_FLAG_IN_WORLD) ||
        rs.area != local_area(area, sizeof area))
        return false;
    memcpy(pos, rs.pos, 12);
    return true;
}

static void drop_gone_send(const struct Drop *d, int reason) {
    CoopDropGone g = {d->id, (uint8_t)reason, {0, 0, 0}};
    bool ok = coop_net_send_event(COOP_EV_DROP_GONE, &g, sizeof g);
    LOG("[coop] drops: %s %s (id %08x) %s here: told the other game%s\n", d->ours ? "our" : "the shared", d->name, d->id,
        reason == COOP_DROP_PICKED ? "picked up" : "vanished", ok ? "" : " (NOT sent: no peer or queue full)");
}

// -- pending drops: find the actor of an item that left our pouch
struct DropFind { int k; float me[3]; uint64_t best; uint32_t best_id; bool waiting; };
static bool drop_find_want(const char *name) {
    for (int k = 0; k < drop_pending_n; k++)
        if (!strcmp(drop_pending[k].name, name)) return true;
    return false;
}
// (2026-11-12, a two-game test) avatar.inc rebuilds held[] every 0.5 s from what is near Link's bones: an equipped
// sword dropped from the menu (lying 1.5 m away) dropped out of it before the pouch confirmed the drop at menu close,
// and was then refused as an "older" actor (nothing shared). So the gear Link held stays known for DROP_HELD_NS.
#define DROP_HELD_NS 30000000000ull
static struct { uint64_t actor; uint64_t at_ns; } drop_held_was[8];
static void drop_held_note(uint64_t now) {
    for (int k = 0; k < COOP_GEAR_COUNT; k++) {
        uint64_t a = held[k].actor;
        if (!a || !held[k].name[0]) continue;
        int j = 0, old = 0;
        while (j < 8 && drop_held_was[j].actor != a) {
            if (drop_held_was[j].at_ns < drop_held_was[old].at_ns) old = j;
            j++;
        }
        if (j == 8) j = old;
        drop_held_was[j].actor = a, drop_held_was[j].at_ns = now;
    }
}
static bool drop_was_held(uint64_t a) {
    for (int k = 0; k < COOP_GEAR_COUNT; k++)
        if (held[k].actor == a) return true;
    uint64_t now = plat_now_ns();
    for (int j = 0; j < 8; j++)
        if (a && drop_held_was[j].actor == a && now - drop_held_was[j].at_ns < DROP_HELD_NS) return true;
    return false;
}
static void drop_find_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    struct DropFind *f = ctx;
    if (strcmp(name, drop_pending[f->k].name) || drop_by_actor(a)) return;
    float p[3];
    float dx, dz;
    drop_read_pos(a, p);
    dx = p[0] - f->me[0], dz = p[2] - f->me[2];
    uint32_t id = LD32(a + PROC_ID);
    float bd = drop_bone_dist(p);
    uint64_t pa = LD64(player_info_ptr() + 0x60);
    const char *why = NULL;
    // (the weapon Link held may have been thrown: up to ~15 m away)
    float near_m = drop_was_held(a) ? 20.0f : DROP_NEAR_M;
    bool fresh = id > drop_pending[f->k].ref || drop_was_held(a);
    bool parked = fabsf(p[0]) < 3.0f && fabsf(p[1]) < 5.0f && fabsf(p[2]) < 3.0f &&
                  f->me[0] * f->me[0] + f->me[2] * f->me[2] > 100.0f * 100.0f;
    if (parked && fresh) why = "waiting at the world origin (menu still open?)", f->waiting = true;
    else if (dx * dx + dz * dz > near_m * near_m || fabsf(p[1] - f->me[1]) > (drop_was_held(a) ? 10.0f : 3.0f))
        why = "too far from us";
    else if (!fresh) why = "older";  // (not what we just dropped)
    // (still on Link: equipped / held in the hands. Gear sits within ~0.3 m of its bone; a sword thrown into a wall
    // right in front of Link lay 1.1 m from him, maybe < 0.6 m from his hand bone: 0.45 m)
    else if (bd < 0.45f) {
        why = "on our Link";
        // (equipped gear: a same-named copy sold / broken while this one stays equipped must not hold the search
        // open for minutes; 20 s covers a drop with the menu kept open)
        if (!drop_was_held(a) || plat_now_ns() - drop_pending[f->k].first_ns < 20000000000ull) f->waiting = true;
    } else if (in_guest(pa) && LD64(pa + 0x88) == a) why = "carried", f->waiting = true;
    else if (actor_hash(c, a)) why = "placed on the map";
    if (drop_pending[f->k].logged < 12 && (!why || strcmp(why, "too far from us") || id > drop_pending[f->k].ref)) {
        drop_pending[f->k].logged++;
        LOG("[coop] drops: candidate %s %llx pid %u at %.1f %.1f %.1f (%.1f m from us, %.2f m from a bone): %s\n", name,
            (unsigned long long)a, id, p[0], p[1], p[2], sqrtf(dx * dx + dz * dz), bd, why ? why : "ok");
    }
    // (34) a new item in Link's hands: watched every frame from now on (drops_watch_frame)
    if (why && fresh && (!strcmp(why, "carried") || (!strcmp(why, "on our Link") && !drop_was_held(a))) &&
        drop_pending[f->k].watch != a) {
        drop_pending[f->k].watch = a, drop_pending[f->k].watch_unit = LD64(a + PROC_UNIT), drop_pending[f->k].watch_id = id;
        LOG("[coop] drops: %s (pid %u) is in our Link's hands: watching it to share it the moment it leaves them\n", name, id);
    }
    if (why) return;
    if (!f->best || id > f->best_id) f->best = a, f->best_id = id;
}
static void drops_pending_frame(Cpu *c, uint64_t now) {
    for (int k = 0; k < drop_pending_n; k++) {
        // A thrown weapon followed already: the pouch confirms it.
        for (int i = 0; i < drop_n && drop_pending[k].n > 0; i++) {
            struct Drop *d = &drops[i];
            if (d->ours && !d->confirmed && !strcmp(d->name, drop_pending[k].name)) {
                d->confirmed = true, drop_pending[k].n--;
                LOG("[coop] drops: our thrown %s (id %08x) left the pouch\n", d->name, d->id);
            }
        }
    }
    for (int k = 0; k < drop_pending_n; k++) {
        struct DropFind f = {k, {0, 0, 0}, 0, 0, false};
        drop_me(f.me);
        while (drop_pending[k].n > 0) {
            f.best = 0, f.best_id = 0, f.waiting = false;
            walk_actors(c, drop_find_want, drop_find_seen, &f);
            if (!f.best) {
                // (still in the menu or in Link's hands: wait for it, up to DROP_HOLD_NS)
                if (f.waiting && now - drop_pending[k].first_ns < DROP_HOLD_NS &&
                    drop_pending[k].until_ns < now + DROP_PENDING_NS) {
                    if (!drop_pending[k].said_wait++)
                        LOG("[coop] drops: %s is not on the ground yet (menu open / in Link's hands): still waiting\n",
                            drop_pending[k].name);
                    drop_pending[k].until_ns = now + DROP_PENDING_NS;
                }
                break;
            }
            struct Drop *d = drop_new(f.best, drop_pending[k].name, true, now);
            d->confirmed = true;
            drop_pending[k].n--;
            LOG("[coop] drops: we dropped %s (actor %llx, proc id %u > %u, id %08x) at %.1f %.1f %.1f\n", d->name,
                (unsigned long long)d->actor, d->pid, drop_pending[k].ref, d->id, d->pos[0], d->pos[1], d->pos[2]);
        }
        if (drop_pending[k].n <= 0 || now > drop_pending[k].until_ns) {
            if (drop_pending[k].n > 0 && drop_pending[k].relook)  // (34: not a pouch decrease - nothing lost)
                LOG("[coop] drops: our %s that left Link's hands is not on the ground near us (put away / broke?): "
                    "nothing to share\n", drop_pending[k].name);
            else if (drop_pending[k].n > 0)
                LOG("[coop] drops: %s x%d left the pouch with no item on the ground near us (eaten / sold / broken?)\n",
                    drop_pending[k].name, drop_pending[k].n);
            drop_pending[k--] = drop_pending[--drop_pending_n];
        }
    }
}

// (34) The items seen in our Link's hands for a pending drop (drop_find_seen): true when one of them left the hands
// (or vanished: the game may swap it for a new actor) this frame. No actor walk: only the watched actors are read.
static bool drops_watch_frame(void) {
    static uint64_t last_ns;
    uint64_t now = plat_now_ns(), pa = player_info_ptr() ? LD64(player_info_ptr() + 0x60) : 0;
    bool left = false;
    for (int k = 0; k < drop_pending_n; k++) {
        uint64_t a = drop_pending[k].watch;
        if (!a || drop_pending[k].n <= 0) continue;
        bool alive = in_guest(a) && in_guest(drop_pending[k].watch_unit) &&
                     LD32(drop_pending[k].watch_unit + UNIT_ID) == drop_pending[k].watch_id &&
                     LD64(drop_pending[k].watch_unit + UNIT_ACTOR) == a && LD8(a + PROC_STATE) != 3;
        float p[3];
        if (alive) drop_read_pos(a, p);
        if (!alive || (drop_bone_dist(p) >= 0.45f && !(in_guest(pa) && LD64(pa + 0x88) == a)))
            drop_pending[k].watch = 0, left = true;
    }
    if (!left || now - last_ns < 100000000ull) return false;  // (at most 10 searches a second)
    last_ns = now;
    return true;
}

// (34) Items dropped from the pouch menu while it is open (MENUDROP): a new actor of a pouch item's name near our Link,
// created after the menu opened (proc id above the newest one at that moment), or the weapon / shield / bow he held,
// moved off his body.
static struct { bool was; uint32_t ref; int found, opened; } menu_w;
struct MenuFind { float me[3]; uint32_t ref; uint64_t now; int added; };
static bool menu_want(const char *name) {
    if (!drop_name_ok(name)) return false;
    for (int i = 0; i < drop_pouch_n; i++)
        if (drop_pouch[i].count > 0 && !strcmp(drop_pouch[i].name, name)) return true;
    return drop_find_want(name);
}
static void menu_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    struct MenuFind *f = ctx;
    if (f->added >= 4 || drop_by_actor(a)) return;
    float p[3];
    drop_read_pos(a, p);
    float dx = p[0] - f->me[0], dz = p[2] - f->me[2];
    // (a "Hold" item waits at the world origin meanwhile: far from us, left out here)
    if (dx * dx + dz * dz > DROP_NEAR_M * DROP_NEAR_M || fabsf(p[1] - f->me[1]) > 3.0f) return;
    // (a menu drop lies 1-2 m in front of Link; a weapon equipped from the menu may be made at his feet first - its
    // nearest tracked bone can be ~1 m up - so nothing closer than 0.7 m sideways counts)
    if (dx * dx + dz * dz < 0.7f * 0.7f) return;
    uint32_t id = LD32(a + PROC_ID);
    bool was_held = drop_was_held(a);
    if (id <= f->ref && !was_held) return;  // (it lay here before the menu opened)
    float bd = drop_bone_dist(p);
    if (bd < (was_held ? GEAR_NEAR_M : 0.45f)) return;  // (still on Link: equipped / in his hands)
    uint64_t pa = LD64(player_info_ptr() + 0x60);
    if (in_guest(pa) && LD64(pa + 0x88) == a) return;
    if (actor_hash(c, a)) return;  // (placed on the map)
    struct Drop *d = drop_new(a, name, true, f->now);
    d->menu = true;
    f->added++;
    LOG("[coop] drops: %s dropped from the menu while it is open (actor %llx, %s, %.1f m from us, %.2f m from a bone) at "
        "%.1f %.1f %.1f: shown to the other player now (id %08x)\n", name, (unsigned long long)a,
        was_held ? "the one Link held" : "new", sqrtf(dx * dx + dz * dz), bd, p[0], p[1], p[2], d->id);
}
static void drops_menu_watch(Cpu *c, uint64_t now, bool paused, bool polled) {
    if (!menudrop_on()) return;
    if (paused && !menu_w.was) {
        menu_w.ref = proc_id_max(), menu_w.found = 0;
        if (menu_w.opened++ < 40)
            LOG("[coop] drops: game paused (menu): watching for items dropped from it (newest proc id %u)\n", menu_w.ref);
    }
    if (!paused && menu_w.was && menu_w.found)
        LOG("[coop] drops: menu closed: %d item(s) dropped from it were shown to the other player while it was open\n",
            menu_w.found);
    menu_w.was = paused;
    if (!paused || !polled || drop_pouch_n < 0 || !menu_w.ref) return;
    struct MenuFind f = {{0, 0, 0}, menu_w.ref, now, 0};
    drop_me(f.me);
    walk_actors(c, menu_want, menu_seen, &f);
    menu_w.found += f.added;
}

// Our pouch, 4 times a second: what went down opens a pending drop.
static void drops_pouch_poll(uint64_t now) {
    static struct PouchCount cur[POUCH_MAX];
    static uint64_t wm_ns;
    if (now >= wm_ns) {
        wm_ns = now + 1000000000ull;
        drop_wm[drop_wm_i++ % 128] = proc_id_max();
    }
    int n = pouch_read(cur);
    if (n < 0) return;
    if (drop_pouch_n >= 0) {
        unsigned back = drop_wm_i < DROP_WM_AGO ? drop_wm_i : DROP_WM_AGO;
        uint32_t ref = drop_wm_i ? drop_wm[(drop_wm_i - back) % 128] : 0;
        for (int j = 0; j < drop_pouch_n; j++) {
            if (!drop_name_ok(drop_pouch[j].name)) continue;
            int32_t now_has = 0;
            for (int i = 0; i < n; i++)
                if (!strcmp(cur[i].name, drop_pouch[j].name)) now_has = cur[i].count;
            int lost = drop_pouch[j].count - now_has;
            if (lost < 0) {  // (back in the pouch: held items put away again, a "Hold" cancelled)
                for (int k = 0; k < drop_pending_n; k++)
                    if (!strcmp(drop_pending[k].name, drop_pouch[j].name) && drop_pending[k].n > 0) {
                        drop_pending[k].n += lost;
                        if (drop_pending[k].n < 0) drop_pending[k].n = 0;
                        LOG("[coop] drops: %s back in our pouch (%d -> %d): %d still looked for\n", drop_pouch[j].name,
                            drop_pouch[j].count, now_has, drop_pending[k].n);
                    }
                continue;
            }
            if (lost == 0) continue;
            if (lost > 8) lost = 8;  // (a whole stack sold / cooked: at most a few drops looked for)
            int k = 0;
            while (k < drop_pending_n && strcmp(drop_pending[k].name, drop_pouch[j].name)) k++;
            if (k == drop_pending_n) {
                if (drop_pending_n == 8) continue;
                drop_pending_n++;
                snprintf(drop_pending[k].name, sizeof drop_pending[k].name, "%s", drop_pouch[j].name);
                drop_pending[k].n = 0, drop_pending[k].ref = ref;
                drop_pending[k].first_ns = now, drop_pending[k].logged = 0, drop_pending[k].said_wait = 0;
                drop_pending[k].watch = 0;
            }
            drop_pending[k].relook = false;
            drop_pending[k].n += lost;
            drop_pending[k].until_ns = now + DROP_PENDING_NS;
            LOG("[coop] drops: %s x%d left our pouch (%d -> %d): looking for it on the ground\n", drop_pouch[j].name,
                lost, drop_pouch[j].count, now_has);
        }
    }
    memcpy(drop_pouch, cur, sizeof *cur * (size_t)n), drop_pouch_n = n;
}

// Our held melee weapon flying away from us: thrown (followed from this frame on).
static void drops_throw_watch(uint64_t now) {
    static uint64_t last_actor;
    static float last_p[3];
    static uint64_t last_ns;
    static int fast;
    const struct Held *h = &held[COOP_GEAR_WEAPON];
    if (!h->name[0] || !held_alive(h)) { last_actor = 0; return; }
    float p[3], me[3];
    drop_read_pos(h->actor, p);
    drop_me(me);
    me[1] += 1.0f;
    bool have = last_actor == h->actor && now > last_ns;
    float dt = have ? (float)((now - last_ns) / 1e9) : 0, v = have && dt > 0 ? dist3(p, last_p) / dt : 0;
    last_actor = h->actor, last_ns = now, memcpy(last_p, p, 12);
    // (two frames in a row at a throw's speed: dropping the equipped sword from the menu moves it from the back to
    // the ground in ONE frame, 42.8 m/s seen; a throw flies ~14 m/s)
    fast = have && v > 4.0f ? fast + 1 : 0;
    if (fast < 2 || dist3(p, me) < 1.2f) return;
    struct Drop *d = drop_by_actor(h->actor);
    if (!d) {
        d = drop_new(h->actor, h->name, true, now);
        LOG("[coop] drops: our %s flies away (%.1f m/s, %.1f m from us): thrown (id %08x)\n", d->name, v, dist3(p, me),
            d->id);
    }
    d->flying = true;
}

// The flying weapon against the other player's avatar (in our game): a hit is told to the other game.
static void drops_throw_hit(struct Drop *d, const float prev[3], const float p[3], float v) {
    if (d->hit_done || !d->flying || v < 1.5f || !pvp_on() || !avatar.actor || !avatar_alive() || avatar.frames < 6)
        return;
    float pp[3];
    if (!drop_partner(pp)) return;
    const float *am = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
    float lo[3] = {am[3], am[7] + PVP_BODY_LO, am[11]}, hi[3] = {am[3], am[7] + PVP_BODY_HI, am[11]};
    float dist = seg_seg(prev, p, lo, hi);
    float dir[3] = {p[0] - prev[0], 0, p[2] - prev[2]}, l = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    if (l > 1e-4f) dir[0] /= l, dir[2] /= l;
    else dir[0] = 0, dir[2] = 1;
    // (2026-11-12, test_drops on protocol 34; throwtrace) the weapon bounces off the avatar's collision capsule
    // (Dm_CoopBody, r 0.4) before its origin (the grip; the blade reaches ~0.8 m ahead) gets near the axis: a thrown
    // sword stopped 1.07 m from the axis and fell back (no hit with 0.8 m). So THROW_BODY_R is 1.2 m, and the push
    // goes the way the weapon last flew AT the avatar (the frame of the hit may already be the bounce).
    float to[3] = {am[3] - prev[0], 0, am[11] - prev[2]}, tl = sqrtf(to[0] * to[0] + to[2] * to[2]);
    if (tl > 1e-3f && (dir[0] * to[0] + dir[2] * to[2]) / tl > 0.3f) memcpy(d->fly_dir, dir, 12);
    if (getenv("SAVAGEWIND_COOP_THROWTRACE"))
        LOG("[coop] throwtrace %s: at %.2f %.2f %.2f, %.2f m from the body axis, %.1f m/s, dir %.2f %.2f\n",
            d->name, p[0], p[1], p[2], dist, v, dir[0], dir[2]);
    if (dist > THROW_BODY_R) return;
    if (d->fly_dir[0] || d->fly_dir[2]) dir[0] = d->fly_dir[0], dir[2] = d->fly_dir[2];
    d->hit_done = true;
    int power = weapon_power(d->name);
    CoopAreaFx e = {COOP_AREAFX_THROW, 0, (int16_t)(power > 400 ? 400 : power), {p[0], p[1], p[2]}, {dir[0], 0, dir[2]}, 0};
    bool ok = coop_net_send_event(COOP_EV_AREAFX, &e, sizeof e);
    LOG("[coop] drops: our thrown %s hit the other player (%.2f m from the body axis, %.1f m/s, power %d): %s\n", d->name,
        dist, v, power, ok ? "told them" : "NOT sent");
}

// The other game's thrown weapon hit our Link there (game thread, from pvp_areafx_ev).
static void drops_throw_ev(Cpu *c, const CoopAreaFx *e) {
    uint64_t pa = player_info_ptr() ? LD64(player_info_ptr() + 0x60) : 0;
    char as[64] = "", area[96];
    float me[3];
    drop_me(me);
    float body[3] = {me[0], me[1] + 0.9f, me[2]}, dist = dist3(body, e->pos);
    CoopState rs;
    double age = 99;
    bool same_area = coop_net_get_remote(&rs, &age) && rs.area == local_area(area, sizeof area);
    // (where the weapon met the avatar there vs. where we are here: a few tenths of a second of lag apart)
    bool ok = pvp_on() && same_area && dist < 3.0f && pvp_victim_ok(c, pa, as, sizeof as);
    LOG("[coop] pvp: the other player's thrown weapon hit us there (%.1f m from us here): %s\n", dist,
        ok ? "hit" : !pvp_on() ? "friendly damage off" : "out of reach / can't be hit now");
    if (!ok) return;
    float from[3] = {e->pos[0] - e->dir[0] * 2, e->pos[1], e->pos[2] - e->dir[2] * 2};
    if (pvp_guarded(c, pa, from)) {
        LOG("[coop] pvp: blocked the thrown weapon with the shield (%s)\n", as);
        guard_fx(c, pa, from);
        return;
    }
    int dmg = e->damage > PVP_DMG_MAX ? PVP_DMG_MAX : e->damage, life = (int)LD32(pa + PVP_LIFE);
    float dir[3] = {e->dir[0], 0, e->dir[2]};
    int got = player_hit(pa, dmg, 1, 0x11, 0x14, dir);
    LOG("[coop] pvp: thrown weapon: -%d life (%d -> %d), medium reaction\n", got, life, life - got);
}

// (34) Our copy of the other game's item to where theirs came to rest: its physics body moved by the game's own
// RigidBody setPosition (raw matrix writes do nothing on a physics item), by the difference between the copy's actor
// and the target (the body's centre may sit off the actor's origin); no body found: the copy is made again there.
static bool drop_copy_move(Cpu *c, struct Drop *d, const float to[3], const char *how) {
    float p[3];
    drop_read_pos(d->actor, p);
    float delta[3] = {to[0] - p[0], to[1] + 0.1f - p[1], to[2] - p[2]};
    bool fast = objects_fast_only;
    objects_fast_only = false;
    uint64_t rb = actor_rb(d->actor);
    objects_fast_only = fast;
    uint64_t body = in_guest(rb) ? LD64(rb + 0x78) : 0;
    if (!rb_mem) rb_mem = (uint64_t)(uintptr_t)hle_alloc(0x80);
    if (in_guest(body) && rb_mem) {
        static const float zero[3] = {0, 0, 0};
        if (LD32(rb + 0x68) & 0x80000) rb_fix(c, rb, false);  // (a resting item is "fixed": it ignores moves)
        float bp[3];
        memcpy(bp, (const void *)(uintptr_t)(body + 0x1a0), 12);
        for (int k = 0; k < 3; k++) bp[k] += delta[k];
        memcpy((void *)(uintptr_t)(rb_mem + 0x50), bp, 12);
        Cpu saved = *c;
        c->x[0] = rb; c->x[1] = rb_mem + 0x50; c->x[2] = 1;
        guest_call(c, A_RB_SET_POSITION);
        *c = saved;
        rb_set_linvel(c, rb, zero);
        LOG("[coop] drops: the shared %s (id %08x) moved %.1f m to where it lies in the other game (%s, physics body)\n",
            d->name, d->id, sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]), how);
    } else {
        float at[3] = {to[0], to[1] + 0.3f, to[2]};
        uint64_t a = spawn_now(c, d->name, at);
        if (!a) { LOG("[coop] drops: the shared %s (id %08x) could not be moved (no body, no new copy)\n", d->name, d->id); return false; }
        despawn_now(c, d->actor, ACTOR_DELETE_REASON);
        d->actor = a, d->unit = LD64(a + PROC_UNIT), d->pid = LD32(a + PROC_ID), d->life_frames = 0;
        LOG("[coop] drops: the shared %s (id %08x) made again where it lies in the other game (%s, no physics body found)\n",
            d->name, d->id, how);
    }
    memcpy(d->pos, to, 12);
    d->moved_ns = plat_now_ns(), d->resting = false;
    return true;
}

// (race) The guest's side: give back one `name` (game thread, not paused). True if the pouch lost one.
static bool drop_give_back(Cpu *c, const char *name) {
    uint64_t mgr = LD64(A_POUCH_MGR), s = world_safestring(name);
    if (!in_guest(mgr) || !s) return false;
    int32_t had = pouch_count_of(name), now_has;
    Cpu saved = *c;
    c->x[0] = mgr, c->x[1] = s;
    guest_call(c, A_POUCH_REMOVE1);
    *c = saved;
    now_has = pouch_count_of(name);
    const char *how = "an unequipped one";
    if (now_has >= had && !strncmp(name, "Weapon_", 7)) {  // (only the equipped one: picked up with empty hands)
        s = world_safestring(name);
        c->x[0] = s;
        guest_call(c, A_POUCH_REMOVE_EQUIPPED);
        *c = saved;
        now_has = pouch_count_of(name);
        how = "the equipped one";
    }
    for (int j = 0; j < drop_pouch_n; j++)  // (not a drop of ours: the pouch poll must not look for its actor)
        if (!strcmp(drop_pouch[j].name, name)) drop_pouch[j].count = now_has;
    LOG("[coop] drops: picked up in both games at once: gave our %s back (%s): pouch %d -> %d%s\n", name, how, had,
        now_has, now_has < had ? "" : " - NOTHING REMOVED");
    return now_has < had;
}
static void drop_race_add(const struct Drop *d, int32_t base, bool give, uint64_t now) {
    if (drop_race_n == 4) drop_race[0] = drop_race[--drop_race_n];
    drop_race[drop_race_n].id = d->id, drop_race[drop_race_n].base = base, drop_race[drop_race_n].give = give;
    drop_race[drop_race_n].until_ns = now + DROP_RACE_NS;
    snprintf(drop_race[drop_race_n].name, sizeof drop_race[0].name, "%s", d->name);
    drop_race_n++;
}
// (race) every frame: open races settle once our pouch rose (or expire); held DROP_GONE events (dev) are replayed.
static void drops_ev(Cpu *c, uint16_t type, const uint8_t *data, int len);
static void drops_race_frame(Cpu *c, uint64_t now, bool paused) {
    if (drop_giveback_dev[0] && !paused) {
        drop_give_back(c, drop_giveback_dev);
        drop_giveback_dev[0] = 0;
    }
    for (int i = 0; i < drop_gone_held_n; i++)
        if (now >= drop_gone_held[i].at_ns) {
            uint8_t data[sizeof(CoopDropGone)];
            memcpy(data, drop_gone_held[i].data, sizeof data);
            drop_gone_held[i--] = drop_gone_held[--drop_gone_held_n];
            drop_gone_replay = true;
            drops_ev(c, COOP_EV_DROP_GONE, data, (int)sizeof data);
            drop_gone_replay = false;
        }
    for (int i = 0; i < drop_race_n; i++) {
        if (now >= drop_race[i].until_ns) {
            LOG("[coop] drops: %s (id %08x): our pouch did not rise - we did not pick it up, no race\n",
                drop_race[i].name, drop_race[i].id);
            drop_race[i--] = drop_race[--drop_race_n];
            continue;
        }
        if (paused || pouch_count_of(drop_race[i].name) <= drop_race[i].base) continue;
        if (drop_race[i].give) {
            drop_give_back(c, drop_race[i].name);
        } else {  // (host: keep it, and tell the guest we have it too - it gives its one back)
            struct Drop tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.id = drop_race[i].id, tmp.ours = false;
            snprintf(tmp.name, sizeof tmp.name, "%s", drop_race[i].name);
            LOG("[coop] drops: %s (id %08x) picked up in both games at once: the host keeps it\n", tmp.name, tmp.id);
            drop_gone_send(&tmp, COOP_DROP_PICKED);
        }
        drop_race[i--] = drop_race[--drop_race_n];
    }
}

// The other game's drop / pick-up (game thread, from world_event; we are in the world).
static void drops_ev(Cpu *c, uint16_t type, const uint8_t *data, int len) {
    uint64_t now = plat_now_ns();
    if (type == COOP_EV_DROP_GONE) {
        if (len != (int)sizeof(CoopDropGone)) return;
        static int delay_ms = -1;
        if (delay_ms < 0) delay_ms = getenv("SAVAGEWIND_COOP_DROPGONE_DELAY_MS") ? atoi(getenv("SAVAGEWIND_COOP_DROPGONE_DELAY_MS")) : 0;
        if (delay_ms > 0 && !drop_gone_replay && drop_gone_held_n < 4) {
            memcpy(drop_gone_held[drop_gone_held_n].data, data, sizeof(CoopDropGone));
            drop_gone_held[drop_gone_held_n++].at_ns = now + (uint64_t)delay_ms * 1000000ull;
            LOG("[coop] drops: (dev) a pick-up event held for %d ms\n", delay_ms);
            return;
        }
        CoopDropGone g;
        memcpy(&g, data, sizeof g);
        if (!coop_drop_gone_ok(&g)) { LOG("[coop] drops: an invalid pick-up event dropped\n"); return; }
        // (race a) we picked this one up ourselves a moment ago, and told them: both have it
        if (g.reason == COOP_DROP_PICKED && droprace_on()) {
            for (int k = 0; k < 4; k++) {
                if (!drop_picked[k].id || drop_picked[k].id != g.id || now - drop_picked[k].at_ns > DROP_RACE_NS) continue;
                drop_picked[k].id = 0;
                if (coop_net_is_host())
                    LOG("[coop] drops: %s (id %08x) picked up in both games at once: the host keeps it (the other game "
                        "gives its one back)\n", drop_picked[k].name, g.id);
                else if (!local_paused()) drop_give_back(c, drop_picked[k].name);
                else {  // (menu open: given back once it closes)
                    struct Drop tmp;
                    memset(&tmp, 0, sizeof tmp);
                    tmp.id = g.id;
                    snprintf(tmp.name, sizeof tmp.name, "%s", drop_picked[k].name);
                    drop_race_add(&tmp, pouch_count_of(tmp.name) - 1, true, now);
                }
                return;
            }
        }
        struct Drop *d = drop_by_id(g.id);
        if (!d) { LOG("[coop] drops: the other game's item %08x is gone there (not here)\n", g.id); return; }
        bool alive = drop_alive(d), del = false;
        // (34) our item dropped from the menu that is still open: the game still places it when the menu closes, so
        // it is deleted only after that (drops_frame, kill_later)
        if (alive && d->ours && d->menu && local_paused()) {
            d->kill_later = true;
            LOG("[coop] drops: our %s (id %08x) picked up in the other game while our menu is open: deleted when it closes\n",
                d->name, d->id);
            return;
        }
        float pd = alive ? drop_bone_dist(d->pos) : 99;
        // (already on our own Link's body: we picked it up at the same moment - keep ours, the network was too slow)
        if (alive && pd >= GEAR_NEAR_M) del = despawn_now(c, d->actor, ACTOR_DELETE_REASON);
        // (race b) on our Link, or gone here right by us: if our pouch rises now, we picked it up too
        float me[3];
        drop_me(me);
        if (g.reason == COOP_DROP_PICKED && droprace_on() && (!alive || pd < GEAR_NEAR_M) && dist3(d->pos, me) < 4.0f) {
            drop_race_add(d, d->base, !coop_net_is_host(), now);
            LOG("[coop] drops: %s (id %08x) picked up there while ours is %s: watching our pouch (%d) for a double pick-up\n",
                d->name, d->id, alive ? "on our Link" : "gone here", d->base);
        }
        LOG("[coop] drops: %s %s (id %08x) %s in the other game: %s\n", d->ours ? "our" : "the shared", d->name, d->id,
            g.reason == COOP_DROP_PICKED ? "picked up" : "vanished",
            del ? "deleted here" : !alive ? "already gone here" : pd < GEAR_NEAR_M ? "we hold it too (both picked it up)" : "delete refused");
        drop_forget(d);
        return;
    }
    if (type != COOP_EV_DROP || len != (int)sizeof(CoopDrop)) return;
    CoopDrop e;
    memcpy(&e, data, sizeof e);
    if (!coop_drop_ok(&e)) { LOG("[coop] drops: an invalid drop event dropped\n"); return; }
    if (!drops_on()) return;
    struct Drop *have = drop_by_id(e.id);
    if (have) {  // (34) the item came to rest there (COOP_DROP_F_UPDATE): our copy goes there if it lies elsewhere
        if (!(e.flags & COOP_DROP_F_UPDATE) || have->ours) return;
        if (!drop_alive(have)) { LOG("[coop] drops: update for the shared %s (id %08x): our copy is gone\n", have->name, have->id); return; }
        float p[3];
        drop_read_pos(have->actor, p);
        float off = dist3(p, e.pos);
        if (drop_bone_dist(p) < GEAR_NEAR_M)
            LOG("[coop] drops: update for the shared %s (id %08x): our Link holds it, left alone\n", have->name, have->id);
        else if (off <= DROP_FIX_M)
            LOG("[coop] drops: the shared %s (id %08x) came to rest there %.2f m from our copy: already in place\n",
                have->name, have->id, off);
        else
            drop_copy_move(c, have, e.pos, "came to rest there");
        return;
    }
    if (e.flags & COOP_DROP_F_UPDATE) {  // (no copy: never made here, or already picked up - never made again: no duplicate)
        LOG("[coop] drops: update for the other player's %s (id %08x): no copy of it here, ignored\n", e.name, e.id);
        return;
    }
    char area[96];
    float me[3];
    drop_me(me);
    float d = dist3(me, e.pos);
    if (e.area != local_area(area, sizeof area) || d > DROP_SHARE_M + 10.0f) {
        LOG("[coop] drops: the other player dropped %s (id %08x) - not here (%.0f m away%s)\n", e.name, e.id, d,
            e.area == local_area(area, sizeof area) ? "" : ", another area");
        return;
    }
    if (spawn_hold()) {  // (no creation while an event / load runs: kept for later, made by drops_frame)
        if (drop_hold_n < (int)(sizeof drop_hold_q / sizeof *drop_hold_q)) drop_hold_q[drop_hold_n++] = e;
        return;
    }
    float at[3] = {e.pos[0], e.pos[1] + 0.3f, e.pos[2]};
    uint64_t a = spawn_now(c, e.name, at);
    if (!a) { LOG("[coop] drops: the other player dropped %s: the game refused to make it here\n", e.name); return; }
    struct Drop *x = drop_new(a, e.name, false, now);
    x->id = e.id, x->confirmed = x->sent = true;
    if (!strncmp(e.name, "Weapon_", 7) && e.life >= 0) x->life_want = e.life;
    LOG("[coop] drops: the other player dropped %s (id %08x, durability %d%s): made here at %.1f %.1f %.1f (actor %llx, "
        "%.0f m from us)\n", e.name, e.id, e.life,
        e.flags & COOP_DROP_F_UPDATE ? ", resting there" : e.flags & COOP_DROP_F_MOVING ? ", still falling there" : "", at[0],
        at[1], at[2], (unsigned long long)a, d);
}

// Did a pick-up of a shared drop just bring this into our pouch? (gate.inc's pouch diff then leaves it alone.)
static bool drops_owns_gain(const char *name, uint64_t now) {
    for (int i = 0; i < 4; i++)
        if (drop_picked[i].at_ns && now - drop_picked[i].at_ns < 3000000000ull && !strcmp(drop_picked[i].name, name))
            return true;
    float me[3];
    drop_me(me);
    for (int i = 0; i < drop_n; i++)
        if (!strcmp(drops[i].name, name) && dist3(drops[i].pos, me) < DROP_NEAR_M) return true;
    return false;
}

static void drops_frame(Cpu *c) {
    static uint32_t area_was;
    static uint64_t poll_ns;
    uint64_t now = plat_now_ns();
    if (drops_dev) {
        drops_dev = 0;
        for (int i = 0; i < drop_n; i++) {
            struct Drop *d = &drops[i];
            LOG("[coop] drops: %08x %s %s actor %llx pid %u %s at %.1f %.1f %.1f%s%s%s base %d\n", d->id,
                d->ours ? "ours" : "copy", d->name, (unsigned long long)d->actor, d->pid, drop_alive(d) ? "alive" : "GONE",
                d->pos[0], d->pos[1], d->pos[2], d->confirmed ? " confirmed" : "", d->sent ? " sent" : "",
                d->resting ? " resting" : "", d->base);
        }
        LOG("[coop] drops: %d tracked, %d pending\n", drop_n, drop_pending_n);
    }
    char area[96];
    uint32_t here = local_in_world() ? local_area(area, sizeof area) : 0;
    if (!drops_on() || !here || here != area_was) {  // (a load / area change: the actors are gone)
        if (drop_n || drop_pending_n) LOG("[coop] drops: %d tracked items forgotten (area change / load)\n", drop_n);
        drop_n = 0, drop_pending_n = 0, drop_pouch_n = -1, area_was = here, drop_hold_n = 0;
        memset(drop_held_was, 0, sizeof drop_held_was);  // (actor addresses are reused after a load)
        return;
    }
    if (drop_hold_n && !spawn_hold()) {  // (drops that came in during the spawn hold: made now)
        CoopDrop q[8];
        int n = drop_hold_n;
        memcpy(q, drop_hold_q, sizeof q);
        drop_hold_n = 0;
        for (int i = 0; i < n; i++) drops_ev(c, COOP_EV_DROP, (const uint8_t *)&q[i], (int)sizeof q[i]);
    }
    drops_throw_watch(now);
    drop_held_note(now);
    bool paused = local_paused(), polled = false;
    drops_race_frame(c, now, paused);
    if (now >= poll_ns) {
        poll_ns = now + 250000000ull;
        drops_pouch_poll(now);
        if (drop_pending_n) drops_pending_frame(c, now);
        polled = true;
    }
    drops_menu_watch(c, now, paused, polled);
    if (!polled && drops_watch_frame()) {  // (34) an item left Link's hands this frame: look for it now
        LOG("[coop] drops: an item we held left Link's hands: looking for it now\n");
        drops_pending_frame(c, now);
    }
    float me[3], partner[3];
    int holding = -1;  // (our Link holds something in his hands: asked once per frame, only when needed)
    drop_me(me);
    bool have_partner = drop_partner(partner);
    for (int i = 0; i < drop_n; i++) {
        struct Drop *d = &drops[i];
        bool alive = drop_alive(d);
        float p[3], prev[3];
        memcpy(prev, d->pos, 12);
        if (alive) drop_read_pos(d->actor, p);
        else memcpy(p, d->pos, 12);
        float dt = (float)((now - d->frame_ns) / 1e9), v = alive && dt > 0 ? dist3(p, prev) / dt : 0;
        d->frame_ns = now;
        if (d->kill_later && !paused && !d->kill_ns) d->kill_ns = now + 500000000ull;  // (placed by the game first)
        if (d->kill_later && d->kill_ns && now >= d->kill_ns) {  // (34) taken by the other player while our menu was open
            bool del = alive && despawn_now(c, d->actor, ACTOR_DELETE_REASON);
            LOG("[coop] drops: our %s (id %08x), picked up in the other game, %s after our menu closed\n", d->name,
                d->id, del ? "deleted here" : alive ? "could not be deleted" : "already gone");
            drop_forget(d), i--;
            continue;
        }
        if (d->kill_later) continue;
        if (alive) {
            if (d->ours) drops_throw_hit(d, prev, p, v);
            memcpy(d->pos, p, 12);
            if (v > 0.3f) d->moved_ns = now, d->resting = false;
            else if (now - d->moved_ns > DROP_REST_NS) d->resting = true, d->flying = false;
        }
        float bone_d = alive ? drop_bone_dist(p) : 99, my_d = dist3(p, me);
        int32_t has = drop_pouch_count(d->name);
        if (my_d > 3.0f) d->base = has;
        else if (has < d->base) d->base = has;
        // Picked up: gone, or on our Link's bones (a weapon picked up with empty hands stays the same actor), and the
        // pouch has more of it. (On our bones without a pouch gain: Link only stands over it, or caught a boomerang.)
        bool on_us = alive && bone_d < GEAR_NEAR_M;
        // (items held in the hands after "Hold" in the menu sit over Link's head, maybe farther than GEAR_NEAR_M from
        // the bones we track: while Link holds something, an item of ours within 1.6 m of his chest counts as held)
        float chest[3] = {me[0], me[1] + 1.0f, me[2]};
        // ((34) only above the hips: one just put down on the ground in front of Link while he still holds others -
        // AS GrabPut / GrabStand - is not in his hands)
        if (alive && d->ours && !d->sent && !on_us && dist3(p, chest) < 1.6f && (!dropearly_on() || p[1] > me[1] + 0.5f)) {
            if (holding < 0) {
                char all[256] = "";
                uint64_t pa = LD64(player_info_ptr() + 0x60);
                if (in_guest(pa)) as_list(c, pa, all, sizeof all);
                holding = strstr(all, "Grab") != NULL;
            }
            if (holding) on_us = true;
        }
        if (alive) d->held_last = on_us || (in_guest(LD64(player_info_ptr() + 0x60)) &&
                                            LD64(LD64(player_info_ptr() + 0x60) + 0x88) == d->actor);
        // (a copy's durability: written right after the creation it did not stick - the actor's first update sets the
        // item's default, 2300 seen for Weapon_Sword_002 - so it is written for its first 60 frames; a write
        // seconds later stuck until the pick-up, 2026-11-11. Whether the pouch then holds that value: NOT checked)
        if (alive && d->life_want >= 0 && d->life_frames < 60) {
            ST32(d->actor + DROP_LIFE_OFS, (uint32_t)d->life_want);
            if (++d->life_frames == 60)
                LOG("[coop] drops: copy of %s: durability %d after 60 frames (wanted %d)\n", d->name,
                    (int)LD32(d->actor + DROP_LIFE_OFS), d->life_want);
        }
        if (alive && !strncmp(d->name, "Weapon_", 7)) d->life = (int32_t)LD32(d->actor + DROP_LIFE_OFS);
        if (!alive || on_us) {
            int32_t now_has = pouch_count_of(d->name);
            if (now_has > d->base && my_d < 4.0f) {
                if (d->sent) drop_gone_send(d, COOP_DROP_PICKED);
                else LOG("[coop] drops: we picked up our %s again (id %08x, never shared)\n", d->name, d->id);
                LOG("[coop] drops: picked up %s: pouch %d -> %d (its durability was %d)\n", d->name, d->base, now_has,
                    d->life);
                int k = 0;
                for (int j = 1; j < 4; j++) if (drop_picked[j].at_ns < drop_picked[k].at_ns) k = j;
                snprintf(drop_picked[k].name, sizeof drop_picked[k].name, "%s", d->name), drop_picked[k].at_ns = now;
                drop_picked[k].id = d->sent ? d->id : 0;
                drop_forget(d), i--;
                continue;
            }
        }
        if (!alive) {
            if (!d->gone_ns) d->gone_ns = now;
            if (now - d->gone_ns < 1000000000ull) continue;  // (the pouch may change a moment later)
            // (34) a thrown weapon of ours that vanished in flight (play-test 3: a thrown Korok Leaf) broke / was put
            // away: no other actor to look for (it used to open a search that ended in a misleading "left the pouch")
            if (d->ours && !d->sent && d->flying) {
                LOG("[coop] drops: our thrown %s (id %08x) vanished in flight (broke / put away?): nothing to share\n",
                    d->name, d->id);
                drop_forget(d), i--;
                continue;
            }
            // An item of ours that vanished from Link's hands, not into the pouch: the game may have swapped it for a
            // new actor when it was put down / thrown ("Hold" from the menu): look for that one.
            if (d->ours && !d->sent && d->held_last && drop_pending_n < 8 && !drop_find_want(d->name)) {
                int k = drop_pending_n++;
                memset(&drop_pending[k], 0, sizeof drop_pending[k]);
                snprintf(drop_pending[k].name, sizeof drop_pending[k].name, "%s", d->name);
                drop_pending[k].n = 1, drop_pending[k].ref = d->pid, drop_pending[k].relook = true;
                drop_pending[k].first_ns = now, drop_pending[k].until_ns = now + DROP_PENDING_NS;
                LOG("[coop] drops: our %s (id %08x) left Link's hands as another actor? looking for it\n", d->name, d->id);
                drop_forget(d), i--;
                continue;
            }
            if (d->ours && d->sent) drop_gone_send(d, COOP_DROP_VANISHED);
            else LOG("[coop] drops: %s %s (id %08x) gone here by itself\n", d->ours ? "our" : "the shared", d->name, d->id);
            drop_forget(d), i--;
            continue;
        }
        d->gone_ns = 0;
        // (34) dropped from the menu: the pouch confirms it when the menu closes; the 8 s below count from then
        if (d->ours && !d->confirmed && d->menu && paused) d->made_ns = now;
        // A throw the pouch never confirmed (a boomerang caught again?): forgotten after 8 s.
        if (d->ours && !d->confirmed && now - d->made_ns > 8000000000ull) {
            LOG("[coop] drops: our %s (id %08x) never left the pouch: not a drop\n", d->name, d->id);
            if (d->sent) drop_gone_send(d, COOP_DROP_VANISHED);  // (34: shown early - the other game deletes its copy)
            drop_forget(d), i--;
            continue;
        }
        if (on_us) continue;
        // Sent: at rest (protocol 32) or, with DROPEARLY (34), as soon as it is off our Link - a menu drop even before
        // the pouch confirms it; when it then comes to rest (or rests again somewhere else) its spot follows (UPDATE).
        bool early = dropearly_on();
        float off_m = early && p[1] < me[1] + 0.6f ? GEAR_NEAR_M : 0.8f;  // (on the ground at Link's feet: not on him)
        bool may = d->ours && (d->confirmed || (early && d->menu)) && (d->resting || early) && bone_d > off_m;
        bool send_new = may && !d->sent, send_upd = false;
        if (may && d->sent && early && d->resting && !paused && d->updates < DROP_UPDATES_MAX)
            send_upd = d->sent_moving || dist3(d->sent_pos, p) > 1.0f;
        if ((send_new || send_upd) && have_partner && dist3(partner, p) < DROP_SHARE_M) {
            CoopDrop e;
            memset(&e, 0, sizeof e);
            e.id = d->id, e.area = d->area;
            memcpy(e.pos, p, 12);
            const float *m = (const float *)(uintptr_t)(d->actor + ACTOR_MTX);
            e.yaw = atan2f(m[2], m[10]) * 180.0f / 3.14159265f;
            if (!isfinite(e.yaw)) e.yaw = 0;
            e.life = !strncmp(d->name, "Weapon_", 7) ? (int32_t)LD32(d->actor + DROP_LIFE_OFS) : -1;
            if (e.life < -1 || e.life > 10000000) e.life = -1;
            e.flags = send_upd ? COOP_DROP_F_UPDATE : d->resting && !paused ? 0 : COOP_DROP_F_MOVING;
            snprintf(e.name, sizeof e.name, "%s", d->name);
            bool ok = coop_net_send_event(COOP_EV_DROP, &e, sizeof e);
            if (ok) {
                if (send_upd) d->updates++;
                d->sent = true, d->sent_moving = (e.flags & COOP_DROP_F_MOVING) != 0;
                memcpy(d->sent_pos, p, 12);
            }
            if (send_upd)
                LOG("[coop] drops: our %s (id %08x) came to rest at %.1f %.1f %.1f: %s\n", d->name, d->id, p[0], p[1], p[2],
                    ok ? "told them (update)" : "update NOT sent (queue full)");
            else
                LOG("[coop] drops: our %s (id %08x, durability %d) %s at %.1f %.1f %.1f, %.0f m from the other player: %s\n",
                    d->name, d->id, e.life,
                    d->menu && paused ? "dropped from the menu (still open), hangs" : e.flags ? "left our Link, now" : "lies",
                    p[0], p[1], p[2], dist3(partner, p), ok ? "told them" : "NOT sent (queue full)");
        }
    }
}
