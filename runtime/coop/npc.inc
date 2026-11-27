// Part of runtime/coop.c (included there, same translation unit): townspeople (placed Npc_* actors) in the same
// place for both players. (2026-11-20, audit S12, worker npc)
//
// Before this every game ran its own NPCs on its own schedule. Time is shared, so schedules mostly lined up, but a
// walking NPC, one reacting to a talk, or one walking off after a chat stood in different places for the two players.
// Now (visible state only) the NPCs near the players are run like the shared enemies:
//  - one owner per NPC: the player nearer to it (NPC_MARGIN m nearer to take it over, NPC_HOLD_NS between changes);
//    the host decides (COOP_ENEMY_GUEST_RUNS in its entry), as for enemies (enemies.inc enemy_owner);
//  - the owner's game runs the NPC (its own AI and schedule) and sends where it stands (its actor matrix: feet +
//    facing, COOP_ENEMY_MTXPOS) and the AS it plays in slot 0 (Walk / Wait / Talk / Sit ...);
//  - the other game's copy has its AI off (the skip-AI bit, enemies.inc enemy_ai), is moved there gently (30% a frame,
//    a snap only over 8 m) and plays the same AS; its animation keeps running through the thawed jobs (enemies.inc
//    thaw_call: npc_thawed / npc_reapply);
//  - a follower whose own player is close to the NPC (NPC_TALK_R: about to talk, trading) or whose game runs an event
//    (talk, cutscene: evcalm_held) never follows: its NPC runs its own AI, so talking, shops, quest choices and
//    cutscenes stay each player's own and are never frozen. Nothing of a conversation is sent.
// Wire: no change. Our NPCs ride in the enemy packet's entries (coop_net_set_enemies: hash = the NPC's map HashId,
// damage 0), after the real enemies and before wild animals, at most NPC_SEND_MAX of them; they are looked up by
// HashId only, so the enemy code never sees them (it keeps its own table of Enemy_* / Animal_* actors).
// Not synced: run-time NPCs (no HashId), story NPCs (the old man, Great Fairies, Koroks, Hestu, Sidon - a mount in
// horse.inc -, Kass, Yiga in disguise, NPCs under attack, the escort Goron, minigame hosts), dialogue, shops, quest
// flags, rupees, cooking results.
// Cap: the NPC_SYNC_MAX nearest NPCs within NPC_RADIUS m of our player.
// SAVAGEWIND_COOP_NPC=0: off (each game runs its own NPCs, as before). Dev: `coop npc` logs the table;
// SAVAGEWIND_COOP_NPCLOG=1 logs every tracked NPC's position once a second (tests).
#define NPC_SYNC_MAX 16
#define NPC_SEND_MAX 10
#define NPC_RADIUS 60.0f
#define NPC_TALK_R 4.0f          // our player this close: our NPC runs its own AI (talk, shop)
#define NPC_TALK_LEAVE_R 5.0f    // ... until our player is this far again
#define NPC_MARGIN 20.0f         // the other player must be this much nearer to take an NPC over (2026-11-21: was 3 m;
                                 // players walking together flipped the owner every few s and each new owner's AI
                                 // walked the NPC off on its own schedule: "NPCs in different places")
#define NPC_HOLD_NS 10000000000ull  // (2026-11-21: was 2 s, see NPC_MARGIN)
#define NPC_SCAN_NS 2000000000ull
#define NPC_MAX_AGE 1.0          // s: an older position and the copy runs its own AI again
static bool npc_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_NPC");
    return on;
}
static bool npclog_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_COOP_NPCLOG") && !strcmp(getenv("SAVAGEWIND_COOP_NPCLOG"), "1");
    return on;
}
static bool npc_name(const char *n) {
    if (strncmp(n, "Npc_", 4)) return false;
    static const char *const skip[] = {
        "Npc_King",         // the old man (story, paraglider)
        "Npc_DressFairy",   // Great Fairies
        "Npc_HiddenKorok",  // Koroks (puzzles)
        "Npc_OldKorok",     // Hestu
        "Npc_ZoraB",        // Prince Sidon (a mount: horse.inc)
        "Npc_Assassin",     // Yiga in disguise (turn into enemies)
        "Npc_Attacked",     // travellers under attack (an enemy event)
        "Npc_Musician",     // Kass (shrine quests, songs)
        "Npc_FollowGoron",  // the escort Goron: follows the player who has the quest
        // (2026-11-20 evening play) the shrine monk plays its own end cutscene (Demo033, each player's own): a
        // follower copy (AI off) stayed sunk in the floor; the invisible combat-tutorial guide (Npc_TutorialDungeon)
        // restarted its steps each time its owner flipped. Each game runs its own.
        "Npc_DungeonPriest", "Npc_TutorialDungeon",
        "Npc_OasisGoddess", "Npc_Strange_Beacon", "Npc_Gerudo_FruitMelon", "Npc_Bottle_Mes",
        "Npc_MiniGame", "Npc_Minigame", "Npc_Bowling", "Npc_Challenge",  // minigame hosts (personal games)
    };
    for (size_t i = 0; i < sizeof skip / sizeof *skip; i++)
        if (!strncmp(n, skip[i], strlen(skip[i]))) return false;
    return true;
}

static struct Npc {
    uint32_t hash;
    uint64_t actor, unit;
    uint32_t unit_id;
    float dist;           // from our player at the last scan / frame
    bool guest_runs;      // host's decision: the guest's player is nearer, the guest runs it
    uint64_t owner_ns;    // host: when guest_runs last changed
    bool mine;            // our game runs its AI now
    bool lock;            // our player is close / in an event: never followed (own AI)
    bool following;       // AI off, moved to the owner's pose
    uint64_t body;        // follower: its physics body (0: moved by its matrices)
    float off[3];         // follower with a body: body position minus matrix position (when it began to follow)
    float shown[3], rot[4];  // follower: the pose we put it in (feet, quaternion)
    char playing[32];     // follower: the owner's AS we last told it to play
    char name[48];
} npcs[NPC_SYNC_MAX];
static int npc_count;
static uint64_t npc_scan_ns;
static uint32_t npc_area;
static volatile int npc_report;  // `coop npc`
// (job threads: enemies.inc thaw_call) the following copies and their poses, rebuilt each frame
static volatile uint64_t npc_pub_actor[NPC_SYNC_MAX], npc_pub_body[NPC_SYNC_MAX];
static float npc_pub_pos[NPC_SYNC_MAX][3], npc_pub_off[NPC_SYNC_MAX][3], npc_pub_rot[NPC_SYNC_MAX][4];

static bool npc_alive(const struct Npc *n) {
    if (!in_guest(n->actor) || !in_guest(n->unit)) return false;
    return LD32(n->unit + UNIT_ID) == n->unit_id && LD64(n->unit + UNIT_ACTOR) == n->actor &&
           LD8(n->actor + PROC_STATE) != 3;
}
static void npc_ai(struct Npc *n, bool on) {
    uint8_t f = LD8(n->actor + ENEMY_FLAGS);
    ST8(n->actor + ENEMY_FLAGS, on ? (uint8_t)(f & ~ENEMY_SKIP_AI) : (uint8_t)(f | ENEMY_SKIP_AI));
}
static void npc_release(struct Npc *n, const char *why) {
    if (n->following && npc_alive(n)) npc_ai(n, true);
    if (n->following) LOG("[coop] npc %s %u own AI again (%s)\n", n->name, n->hash, why);
    n->following = false;
    n->playing[0] = 0;
}
static void npc_release_all(const char *why) {
    for (int i = 0; i < npc_count; i++) npc_release(&npcs[i], why);
    for (int k = 0; k < NPC_SYNC_MAX; k++) npc_pub_actor[k] = 0;
}

// The pose of a following copy: body (when it has one) and both actor matrices. Also from the job threads, after
// every thawed job (they move the copy on their own: enemies.inc thaw_call).
static void npc_put(uint64_t a, uint64_t body, const float p[3], const float off[3], const float q[4]) {
    if (in_guest(body) && LD64(body) == RIGID_BODY_VT) {
        float bp[3] = {p[0] + off[0], p[1] + off[1], p[2] + off[2]};
        static const uint32_t pos_at[] = {0x1a0, 0x1b0, 0x1c0, 0x250};  // (enemies.inc enemy_reapply)
        for (int k = 0; k < 4; k++) memcpy((void *)(uintptr_t)(body + pos_at[k]), bp, 12);
        memcpy((void *)(uintptr_t)(body + 0x1d0), q, 16);
        memcpy((void *)(uintptr_t)(body + 0x1e0), q, 16);
    }
    enemy_put_mtx(a, p, q);
}
static bool npc_thawed(uint64_t a) {
    for (int i = 0; i < NPC_SYNC_MAX; i++)
        if (npc_pub_actor[i] == a) return true;
    return false;
}
static void npc_reapply(uint64_t a) {
    for (int i = 0; i < NPC_SYNC_MAX; i++)
        if (npc_pub_actor[i] == a) {
            npc_put(a, npc_pub_body[i], npc_pub_pos[i], npc_pub_off[i], npc_pub_rot[i]);
            return;
        }
}

static bool npc_known_actor(uint64_t a) {
    for (int j = 0; j < npc_count; j++)
        if (npcs[j].actor == a) return true;
    return false;
}
// ---- scan (every NPC_SCAN_NS): the nearest placed NPCs within NPC_RADIUS
typedef struct { uint64_t actor; float dist; char name[48]; } NpcSeen;
static struct { NpcSeen s[64]; int n; } npc_seen;
// (2026-11-21, 11-20 play: "NPCs in different places", log: the same NPCs tracked again and again at 55-60 m) The
// distance is from the NEARER player (an NPC by the partner but over 60 m from us ran its own schedule here), and a
// known NPC stays tracked up to NPC_KEEP_R (no drop / re-track at the edge). SAVAGEWIND_COOP_NPCBOTH=0: the old rule.
#define NPC_KEEP_R (NPC_RADIUS + 15.0f)
static float npc_partner_pos[3];
static bool npc_partner_have;
static bool npc_known_actor(uint64_t a);
static void npc_seen_one(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)ctx;
    float p[3], d = actor_dist(a, (const float *)(uintptr_t)(player_info_ptr() + 0x88), p);
    if (npc_partner_have) {
        float dp = actor_dist(a, npc_partner_pos, p);
        if (dp < d) d = dp;
    }
    if (d > (npc_known_actor(a) ? NPC_KEEP_R : NPC_RADIUS) || npc_seen.n >= 64) return;
    NpcSeen *s = &npc_seen.s[npc_seen.n++];
    s->actor = a, s->dist = d;
    snprintf(s->name, sizeof s->name, "%s", name);
}
static void npc_scan(Cpu *c) {
    npc_seen.n = 0;
    static int both_c = -1;
    static char npc_area_buf[96];
    CoopState rs;
    double age = 99;
    npc_partner_have = env_on("SAVAGEWIND_COOP_NPCBOTH", &both_c) && coop_net_get_remote(&rs, &age) && age < 2 &&
                       (rs.flags & COOP_FLAG_IN_WORLD) && rs.area == local_area(npc_area_buf, sizeof npc_area_buf);
    if (npc_partner_have) memcpy(npc_partner_pos, rs.pos, sizeof npc_partner_pos);
    walk_actors(c, npc_name, npc_seen_one, NULL);
    // nearest first (insertion sort, a few dozen at most)
    for (int i = 1; i < npc_seen.n; i++) {
        NpcSeen t = npc_seen.s[i];
        int k = i;
        while (k > 0 && npc_seen.s[k - 1].dist > t.dist) npc_seen.s[k] = npc_seen.s[k - 1], k--;
        npc_seen.s[k] = t;
    }
    static struct Npc next[NPC_SYNC_MAX];
    int nn = 0;
    bool kept[NPC_SYNC_MAX] = {0};
    for (int i = 0; i < npc_seen.n && nn < NPC_SYNC_MAX; i++) {
        uint64_t a = npc_seen.s[i].actor;
        int j = 0;
        while (j < npc_count && npcs[j].actor != a) j++;
        if (j < npc_count && npc_alive(&npcs[j])) {  // (known: keep its state)
            next[nn] = npcs[j];
            next[nn++].dist = npc_seen.s[i].dist;
            kept[j] = true;
            continue;
        }
        uint32_t hash = actor_hash(c, a);
        if (!hash) continue;  // (run-time NPC: each game its own)
        struct Npc *n = &next[nn++];
        memset(n, 0, sizeof *n);
        n->hash = hash, n->actor = a, n->unit = LD64(a + PROC_UNIT), n->unit_id = LD32(a + PROC_ID);
        n->dist = npc_seen.s[i].dist, n->mine = true;
        snprintf(n->name, sizeof n->name, "%s", npc_seen.s[i].name);
        LOG("[coop] npc %s %u tracked (%.1f m)\n", n->name, n->hash, n->dist);
    }
    for (int j = 0; j < npc_count; j++)
        if (!kept[j]) npc_release(&npcs[j], "out of range");
    for (int k = 0; k < NPC_SYNC_MAX; k++) npc_pub_actor[k] = 0;  // (rebuilt by npc_frame below)
    memcpy(npcs, next, sizeof next[0] * (size_t)nn);
    npc_count = nn;
}

// ---- who runs it
static void npc_owner(struct Npc *n, const CoopState *rs, bool partner_here, bool in_event, uint64_t now) {
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    float p[3], dh = actor_dist(n->actor, me, p);
    n->dist = dh;
    // our player about to talk / trade with it, or in an event: our own AI, never followed
    bool lock = in_event || dh < (n->lock ? NPC_TALK_LEAVE_R : NPC_TALK_R);
    n->lock = lock;
    uint32_t f = 0;
    double age = 99;
    bool known = coop_net_remote_enemy_flags(n->hash, &f, &age);
    if (!coop_net_is_host()) {  // guest: ours unless the host says it runs it
        bool host_runs = known && age < NPC_MAX_AGE && (f & COOP_ENEMY_LOADED) && !(f & COOP_ENEMY_GUEST_RUNS);
        n->mine = !host_runs || lock;
        return;
    }
    bool was = n->guest_runs;
    if (!partner_here || !(known && age < 3.0 && (f & COOP_ENEMY_LOADED))) {
        n->guest_runs = false;  // (the guest is not there, or does not have it loaded)
    } else {
        float dg = actor_dist(n->actor, rs->pos, p);
        bool want = n->guest_runs, now_too = false;
        if (dh < NPC_TALK_R && dh <= dg) want = false, now_too = true;       // (the host talks to it)
        else if (dg < NPC_TALK_R && dg < dh) want = true, now_too = true;    // (the guest talks to it)
        else if (dg + NPC_MARGIN < dh) want = true;
        else if (dh + NPC_MARGIN < dg) want = false;
        if (want != n->guest_runs && (now_too || now - n->owner_ns > NPC_HOLD_NS)) n->guest_runs = want;
    }
    if (n->guest_runs != was) {
        n->owner_ns = now;
        LOG("[coop] npc %s %u: the %s runs it now\n", n->name, n->hash, n->guest_runs ? "guest" : "host");
    }
    n->mine = !n->guest_runs || lock;
}

// ---- follower: the owner's pose and AS
static void npc_follow(Cpu *c, struct Npc *n) {
    float p[3], rot[4];
    double age = 99;
    char as[32] = "";
    uint32_t f = 0;
    double fage = 99;
    bool fresh = coop_net_remote_enemy_pos(n->hash, p, rot, &age, as, sizeof as) && age < NPC_MAX_AGE &&
                 coop_net_remote_enemy_flags(n->hash, &f, &fage) && (f & COOP_ENEMY_MTXPOS);
    if (!fresh) { npc_release(n, "no position from the other side"); return; }
    float ql = rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3];
    if (!(ql > 0.8f && ql < 1.2f)) { npc_release(n, "bad rotation"); return; }
    const float *am = (const float *)(uintptr_t)(n->actor + ACTOR_MTX);
    if (!n->following) {
        uint64_t body = enemy_body(n->actor);
        n->body = body;
        for (int k = 0; k < 3; k++) {
            n->shown[k] = am[3 + 4 * k];
            n->off[k] = body ? ((const float *)(uintptr_t)(body + 0x1a0))[k] - am[3 + 4 * k] : 0.0f;
        }
        npc_ai(n, false);
        n->following = true;
        n->playing[0] = 0;
        float dx = p[0] - n->shown[0], dz = p[2] - n->shown[2];
        LOG("[coop] npc %s %u follows the other side (AI off; %.1f m apart; %s)\n", n->name, n->hash,
            sqrtf(dx * dx + dz * dz), body ? "body + matrix" : "matrix only");
    }
    if (!(LD8(n->actor + ENEMY_FLAGS) & ENEMY_SKIP_AI)) npc_ai(n, false);  // (something cleared it)
    if (n->body && !(in_guest(n->body) && LD64(n->body) == RIGID_BODY_VT)) n->body = 0;
    float d2 = 0;
    for (int k = 0; k < 3; k++) d2 += (p[k] - n->shown[k]) * (p[k] - n->shown[k]);
    // (leave it alone when it already matches: no write below 2 cm and a near-equal facing)
    float dq = fabsf(rot[0] * n->rot[0] + rot[1] * n->rot[1] + rot[2] * n->rot[2] + rot[3] * n->rot[3]);
    bool same = d2 < 0.0004f && dq > 0.9999f;
    for (int k = 0; k < 3; k++) n->shown[k] = d2 > 64.0f ? p[k] : n->shown[k] + (p[k] - n->shown[k]) * 0.3f;
    memcpy(n->rot, rot, sizeof n->rot);
    if (!same) npc_put(n->actor, n->body, n->shown, n->off, n->rot);
    if (as[0]) {
        char cur[64];
        player_as_name(c, n->actor, cur, sizeof cur);
        if (strcmp(cur, as)) {
            play_anim(c, n->actor, as, 0);
            if (strcmp(n->playing, as)) {
                snprintf(n->playing, sizeof n->playing, "%s", as);
                LOG("[coop] npc %s %u plays the other side's %s (was %s)\n", n->name, n->hash, as, cur);
            }
        }
    }
}

// Each frame (from enemies_frame, before its packet is built): scan, owners, followers; writes up to max entries
// for the packet into out (nearest first) and returns how many.
static int npc_frame(Cpu *c, uint64_t now, CoopEnemyDamage *out, int max) {
    static bool was_on;
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    bool placed = last_local.placed_since_ns && now - last_local.placed_since_ns > 3000000000ull;
    bool on = npc_on() && local_in_world() && placed;
    if (!on || here != npc_area) {
        if (was_on || npc_count) {
            npc_release_all(!on ? "off / loading" : "area changed");
            npc_count = 0;
        }
        npc_area = here;
        was_on = false;
        if (!on) return 0;
    }
    was_on = true;
    if (now >= npc_scan_ns) {
        npc_scan_ns = now + NPC_SCAN_NS;
        npc_scan(c);
    }
    CoopState rs;
    double rage = 99;
    bool partner_here = coop_net_get_remote(&rs, &rage) && rage < 1.0 && (rs.flags & COOP_FLAG_IN_WORLD);
    bool in_event = evcalm_held();
    int pub = 0;
    for (int i = 0; i < npc_count; i++) {
        struct Npc *n = &npcs[i];
        if (!npc_alive(n)) {
            if (n->actor) LOG("[coop] npc %s %u gone\n", n->name, n->hash);
            n->actor = 0, n->following = false;
            continue;
        }
        npc_owner(n, &rs, partner_here, in_event, now);
        if (n->mine) {
            if (n->following) npc_release(n, n->lock ? "our player is close / in an event" : "this side runs it now");
        } else {
            npc_follow(c, n);
        }
        if (n->following && pub < NPC_SYNC_MAX) {
            npc_pub_actor[pub] = 0;  // (invalid while rewritten)
            npc_pub_body[pub] = n->body;
            memcpy(npc_pub_pos[pub], n->shown, 12), memcpy(npc_pub_off[pub], n->off, 12);
            memcpy(npc_pub_rot[pub], n->rot, 16);
            npc_pub_actor[pub++] = n->actor;
        }
    }
    for (int k = pub; k < NPC_SYNC_MAX; k++) npc_pub_actor[k] = 0;
    // packet entries: the nearest loaded NPCs (npcs[] is nearest first from the last scan)
    int m = 0;
    for (int i = 0; i < npc_count && m < max; i++) {
        struct Npc *n = &npcs[i];
        if (!n->actor) continue;
        CoopEnemyDamage *o = &out[m++];
        *o = (CoopEnemyDamage){.hash = n->hash, .flags = COOP_ENEMY_LOADED};
        if (coop_net_is_host() && n->guest_runs) o->flags |= COOP_ENEMY_GUEST_RUNS;
        if (n->mine && !n->following) {
            const float *mt = (const float *)(uintptr_t)(n->actor + ACTOR_MTX);
            o->flags |= COOP_ENEMY_POS | COOP_ENEMY_MTXPOS;
            o->pos[0] = mt[3], o->pos[1] = mt[7], o->pos[2] = mt[11];
            mtx_quat(mt, o->rot);
            player_as_name(c, n->actor, o->as, sizeof o->as);
        }
    }
    if (npclog_on() || npc_report) {
        static uint64_t log_ns;
        if (npc_report || now - log_ns > 1000000000ull) {
            log_ns = now;
            for (int i = 0; i < npc_count; i++) {
                struct Npc *n = &npcs[i];
                if (!n->actor) continue;
                const float *mt = (const float *)(uintptr_t)(n->actor + ACTOR_MTX);
                char cur[64];
                player_as_name(c, n->actor, cur, sizeof cur);
                LOG("[coop] npcpos %u %s at %.2f %.2f %.2f as %s %s%s dist %.1f\n", n->hash, n->name, mt[3], mt[7],
                    mt[11], cur[0] ? cur : "-", n->following ? "follows" : n->mine ? "runs" : "waits",
                    n->lock ? " (close)" : "", n->dist);
            }
            if (npc_report) LOG("[coop] npc: %d tracked, %s\n", npc_count, coop_net_is_host() ? "host" : "guest");
            npc_report = 0;
        }
    }
    return m;
}
