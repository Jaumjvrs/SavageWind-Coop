// Part of runtime/coop.c (included there, same translation unit): starting events by name and shared cutscenes.
// ---- starting an event (cutscene) by name (2026-11-08 RE, the AI that launches a map unit's event,
// 0x71011662e0 in Ghidra): Metadata m; ctor(&m); setNames(&m, flow, entry, ""); a request {the actor's matrix (0x30
// bytes from actor+0x398), u32 0x101 at +0x30, &m at +0x38, 0 at +0x40, the actor at +0x48}; callEvent(evt::Manager,
// &request) -> bool; dtor(&m).
#define A_EVT_META_CTOR 0x7001112508ull
#define A_EVT_META_NAMES 0x7001112794ull
#define A_EVT_META_DTOR 0x70011128b4ull
#define A_EVT_CALL 0x700110d590ull
#define A_EMPTY_STR 0x70015e615bull
// (2026-11-15, RE, the game's own pattern at 0x7100d1528c) flow arguments: the Metadata's
// parameter list at meta+0x148, add a string / a bool by name (each returns a bool).
#define A_EVT_PARAM_STR 0x70011134d0ull   // (x0 list, x1 SafeString *value, x2 SafeString *name)
#define A_EVT_PARAM_BOOL 0x70011138f8ull  // (x0 list, x1 const u8 *value, x2 SafeString *name)
static uint64_t evt_mem;  // guest scratch: metadata 0x200, request 0x60, two names 0x80 each; arguments from +0x400
static bool event_start_args(Cpu *c, uint64_t actor, const char *flow, const char *entry, const char *sname,
                             const char *sval, const char *bname, bool bval) {
    if (!evt_mem) evt_mem = (uint64_t)(uintptr_t)hle_alloc(0x600);
    uint64_t mgr = g_site.event_mgr ? LD64(g_site.event_mgr) : 0;
    if (!evt_mem || !in_guest(mgr) || !in_guest(actor) || strlen(flow) >= 0x80 || strlen(entry) >= 0x80) return false;
    if ((sname && (strlen(sname) >= 0x60 || !sval || strlen(sval) >= 0x60)) || (bname && strlen(bname) >= 0x60)) return false;
    uint64_t meta = evt_mem, req = evt_mem + 0x200, fn = evt_mem + 0x280, en = evt_mem + 0x300;
    memset((void *)(uintptr_t)evt_mem, 0, 0x600);
    memcpy((void *)(uintptr_t)fn, flow, strlen(flow) + 1);
    memcpy((void *)(uintptr_t)en, entry, strlen(entry) + 1);
    Cpu saved = *c;
    c->x[0] = meta;
    guest_call(c, A_EVT_META_CTOR);
    *c = saved;
    c->x[0] = meta; c->x[1] = fn; c->x[2] = en; c->x[3] = A_EMPTY_STR;
    guest_call(c, A_EVT_META_NAMES);
    *c = saved;
    uint64_t op = LD64(meta + 0x148);
    if ((sname || bname) && !in_guest(op)) {
        c->x[0] = meta;
        guest_call(c, A_EVT_META_DTOR);
        *c = saved;
        return false;
    }
    // SafeStrings {vtable, text}: name at +0x400 (text +0x410), value at +0x480 (text +0x490), bool name at +0x500
    // (text +0x510), the bool byte at +0x580.
    uint64_t ssvt = LD64(A_SAFESTRING_VT_SLOT) + 0x10;
    if (sname) {
        ST64(evt_mem + 0x400, ssvt); ST64(evt_mem + 0x408, evt_mem + 0x410);
        memcpy((void *)(uintptr_t)(evt_mem + 0x410), sname, strlen(sname) + 1);
        ST64(evt_mem + 0x480, ssvt); ST64(evt_mem + 0x488, evt_mem + 0x490);
        memcpy((void *)(uintptr_t)(evt_mem + 0x490), sval, strlen(sval) + 1);
        c->x[0] = op; c->x[1] = evt_mem + 0x480; c->x[2] = evt_mem + 0x400;
        guest_call(c, A_EVT_PARAM_STR);
        *c = saved;
    }
    if (bname) {
        ST64(evt_mem + 0x500, ssvt); ST64(evt_mem + 0x508, evt_mem + 0x510);
        memcpy((void *)(uintptr_t)(evt_mem + 0x510), bname, strlen(bname) + 1);
        ST8(evt_mem + 0x580, bval ? 1 : 0);
        c->x[0] = op; c->x[1] = evt_mem + 0x580; c->x[2] = evt_mem + 0x500;
        guest_call(c, A_EVT_PARAM_BOOL);
        *c = saved;
    }
    memcpy((void *)(uintptr_t)req, (void *)(uintptr_t)(actor + ACTOR_MTX), 0x30);
    ST32(req + 0x30, 0x101);
    ST64(req + 0x38, meta);
    ST64(req + 0x40, 0);
    ST64(req + 0x48, actor);
    c->x[0] = mgr; c->x[1] = req;
    guest_call(c, A_EVT_CALL);
    bool ok = c->x[0] & 1;
    *c = saved;
    c->x[0] = meta;
    guest_call(c, A_EVT_META_DTOR);
    *c = saved;
    return ok;
}
static bool event_start(Cpu *c, uint64_t actor, const char *flow, const char *entry) {
    return event_start_args(c, actor, flow, entry, NULL, NULL, NULL, false);
}
// `coop event <flow> <entry>` (dev): start that event with our player as its actor; `coop evdump`: the running event's
// object, every pointer in its first 0x200 bytes that leads to text (to find the entry name), to the log.
static struct { volatile int pending; char flow[64], entry[64]; uint64_t actor; } event_dev;
static uint64_t event_dev_last_actor;  // (evdump: where the event keeps the actor it was started with)
static volatile int evdump_dev;
static bool text_at(uint64_t p, char *out, size_t cap);
static struct { volatile int pending; char pat[48]; } evscan_dev;  // `coop evscan <text>` (dev, RE)
static void event_dev_frame(Cpu *c) {
    if (event_dev.pending) {
        event_dev.pending = 0;
        uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
        uint64_t a = event_dev.actor ? event_dev.actor : pa;
        event_dev_last_actor = a;
        bool ok = event_start(c, a, event_dev.flow, event_dev.entry);
        LOG("[coop] event %s / %s (actor %llx%s): %s\n", event_dev.flow, event_dev.entry, (unsigned long long)a,
            a == pa ? ", the player" : "", ok ? "started" : "refused");
    }
    if (evscan_dev.pending) {  // (2026-11-13 RE) where does the event keep a cutscene it called (Demo_CallDemo)?
        evscan_dev.pending = 0;
        uint64_t mgr = g_site.event_mgr ? LD64(g_site.event_mgr) : 0, ev = in_guest(mgr) ? LD64(mgr + 0x1d2b8) : 0;
        int hits = 0;
        uint64_t roots[2] = {ev, mgr + 0x1d000};
        uint32_t span[2] = {0x800, 0x1000};
        for (int r = 0; r < 2 && hits < 40; r++) {
            if (!in_guest(roots[r])) continue;
            for (uint32_t o = 0; o < span[r] && hits < 40; o += 8) {
                uint64_t p = LD64(roots[r] + o);
                char t[64];
                if (text_at(p, t, sizeof t) && strstr(t, evscan_dev.pat)) {
                    LOG("[coop] evscan %s+0x%x -> \"%s\"\n", r ? "mgr+0x1d000" : "ev", o, t), hits++;
                    continue;
                }
                if (!in_guest(p) || !in_guest(p + 0x200)) continue;
                for (uint32_t o2 = 0; o2 < 0x200 && hits < 40; o2 += 8) {
                    uint64_t q = LD64(p + o2);
                    if (text_at(q, t, sizeof t) && strstr(t, evscan_dev.pat)) {
                        LOG("[coop] evscan %s+0x%x -> %llx +0x%x -> \"%s\"\n", r ? "mgr+0x1d000" : "ev", o,
                            (unsigned long long)p, o2, t), hits++;
                        // the whole object: every pointer to text and every inline text in its first 0x400 bytes
                        for (uint32_t o3 = 0; r && o3 < 0x400 && in_guest(p + o3 + 8); o3 += 8) {
                            char u[64];
                            if (text_at(LD64(p + o3), u, sizeof u)) LOG("[coop] evscan   obj+0x%x -> \"%s\"\n", o3, u);
                            const char *s = (const char *)(uintptr_t)(p + o3);
                            int n = 0;
                            while (n < 60 && s[n] >= 0x20 && s[n] < 0x7f) n++;
                            if (n >= 4 && !s[n]) LOG("[coop] evscan   obj+0x%x inline \"%.60s\"\n", o3, s);
                        }
                    }
                }
            }
        }
        LOG("[coop] evscan \"%s\" done: ev %llx, %d hits\n", evscan_dev.pat, (unsigned long long)ev, hits);
    }
    if (evdump_dev) {
        evdump_dev = 0;
        uint64_t mgr = g_site.event_mgr ? LD64(g_site.event_mgr) : 0, ev = in_guest(mgr) ? LD64(mgr + 0x1d2b8) : 0;
        if (!in_guest(ev)) { LOG("[coop] evdump: no event\n"); return; }
        uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
        for (uint32_t o = 0; o < 0x400; o += 8) {
            uint64_t p = LD64(ev + o);
            if (p && (p == pa || p == event_dev_last_actor))
                LOG("[coop] evdump %llx +0x%x -> %s actor %llx\n", (unsigned long long)ev, o,
                    p == pa ? "the player" : "the started-with", (unsigned long long)p);
            if (o >= 0x200 || !in_guest(p) || !in_guest(p + 64)) continue;
            const char *s = (const char *)(uintptr_t)p;
            int n = 0;
            while (n < 48 && s[n] >= 0x20 && s[n] < 0x7f) n++;
            if (n >= 3 && !s[n]) LOG("[coop] evdump %llx +0x%x -> \"%s\"\n", (unsigned long long)ev, o, s);
        }
        LOG("[coop] evdump done\n");
    }
}

// ---- shared cutscenes (2026-11-08: cutscenes should be seen together). When an event starts in our game
// (the event manager's active event: its flow name at +0x68, entry name at +0xc0, both char *), and it is a cutscene
// - flow "Demo*", "SDemo_*" or the old man's "Npc_King_*" - that is not one of each player's own (story_own below),
// we send EV_STORY (flow, entry, where, area). The other game, if its player is in the same area within STORY_RADIUS,
// in the world and not in an event, starts the same event with its own player as the actor (event_start; the game
// refuses while another event runs, so it retries for STORY_WAIT_NS). An event we started for the other player is
// never sent back, and one that ran here in the last STORY_DEDUP_NS is not started again (both games may trigger a
// map's event tag on their own). Verified first on the plateau: Npc_King_Parasail004/Appear started by name plays the
// old man's appearance (then his conversation, answered by each player). SAVAGEWIND_COOP_STORY=0 turns this off.
#define EV_STORY 7
#define STORY_RADIUS 50.0f
#define STORY_WAIT_NS 150000000000ull  // (2026-11-13: longer than the partner waits for us, STORY_HOLD_NS)
#define STORY_DEDUP_NS 60000000000ull
typedef struct EvStory { char flow[48], entry[48]; float pos[3]; uint32_t area; } EvStory;
static bool story_on(void) {
    static int on = -1;
    // (2026-11-08: an event that seemed endless was the old man's conversation waiting for an answer; with the player
    // as its actor it runs to its end like the original - about 30 A presses - and Link moves again after it.)
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_STORY") && !strcmp(getenv("SAVAGEWIND_COOP_STORY"), "0"));
    return on;
}
// Each player's own events (seen in the test logs, 2026-11-08): chest (Demo000), item fanfare (Demo002), rune
// terminal (Demo003), game over (Demo006), shrine entrances and elevators (Demo008), shrine signal camera cuts
// (Demo024: the shared switches trigger them in both games), arriving after a load (Demo025, Demo318), the shrine monk
// (Demo033: the clear is shared), Demo710 (seen near enemies, unknown).
static bool story_own(const char *flow) {
    // (2026-11-13, now that cutscenes called inside a talk are seen too: an inn's night (Demo007_2, Demo011_0) and a
    // great fairy's armour upgrade (Demo029_0, called by Demo045_0) are each player's own)
    // (2026-11-15, first two-PC session: the host's game softlocked (only the camera moved) while equipping armour,
    // right after the partner's Demo042_0 arrived. The game's own "system demos" (bdemo Folder システムデモ) are all
    // personal: Demo001 cooking get, Demo005 warp, Demo010 first telescope use, Demo017 hidden Korok found, Demo042
    // first-time button/menu demo, Demo048 fall-into-the-void handling; never shared)
    static const char *const own[] = {"Demo000_", "Demo001_", "Demo002_", "Demo003_", "Demo004_", "Demo005_", "Demo006_",
                                       "Demo007_", "Demo008_", "Demo010_", "Demo011_", "Demo017_", "Demo024_", "Demo025_",
                                       "Demo029_", "Demo033_", "Demo042_", "Demo043_", "Demo045_", "Demo048_", "Demo164_",
                                       "Demo318_", "Demo707_", "Demo710_"};
    // (2026-11-20 evening, user: Hestu's dance (Demo707_0, entries Weapon_/Shield_/Bow_<place>) is each player's own:
    // each chooses which pouch to grow)
    for (size_t i = 0; i < sizeof own / sizeof *own; i++)
        if (!strncmp(flow, own[i], strlen(own[i]))) return true;
    return false;
}
// (a conversation - entry "Talk*", which each player starts by pressing A at an NPC - stays each player's own: seen
// 2026-11-08, the old man's Talk events went back and forth after his shared appearance)
// (0.1.1, playtest 2026-11-26: whoever speaks to the statue, the other player did not see the talk, it should
// start for both; the waiting partner was then poked back onto the entrance elevator by leave_follow) The shrine
// monk's scene Demo033_0 (entry Demo033_0) is shared with a partner anywhere in the same shrine. The partner's game
// starts it the native way: the monk's barrier (DgnObj_GoalShield_A, map data: its BasicSig link goes through a
// LinkTagAnd to the EventTag Demo033_0, whose map unit carries the warp-to-the-monk destination) sends its signal, as
// when Link walks into it. Each game's own scene gives its own spirit orb (orb wait skips the shared one) and its own
// exit (Demo033_1), so both leave together. SAVAGEWIND_COOP_MONK=0: each player's own, as before.
static bool monk_share_on(void) {
    static int on = -1;
    return env_on("SAVAGEWIND_COOP_MONK", &on);
}
static bool story_monk(const char *flow, const char *entry) {
    return !strcmp(flow, "Demo033_0") && !strcmp(entry, "Demo033_0") && monk_share_on();
}
static bool monk_shield_name(const char *n) { return !strncmp(n, "DgnObj_GoalShield", 17); }
static void monk_shield_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)name;
    if (!*(uint64_t *)ctx) *(uint64_t *)ctx = a;
}
static bool monk_signal(Cpu *c) {
    uint64_t shield = 0;
    walk_actors(c, monk_shield_name, monk_shield_seen, &shield);
    if (!shield) return false;
    Cpu saved = *c;
    c->x[0] = shield; c->x[1] = 0; c->x[2] = 1;
    guest_call(c, 0x70015335d8ull);  // A_SET_OUT_SIGNAL (GoalShield BasicSig -> LinkTagAnd -> EventTag Demo033_0)
    *c = saved;
    return true;
}
static bool story_shared(const char *flow, const char *entry) {
    if (story_monk(flow, entry)) return true;
    // (2026-11-12) A Divine Beast terminal (Demo044_0 Access00..04) lowers the terminal count: played for the partner
    // too it would count twice. Its flag is shared instead (beast.inc).
    if (!strncmp(flow, "Demo044_", 8) && !strncmp(entry, "Access", 6)) return false;
    int castle = castle_story(flow, entry);  // (2026-11-14) the castle flow's entries and cutscenes (castle.inc)
    if (castle) return castle != 3;
    int mem = memories_story(flow, entry);  // (2026-11-14) Captured Memories (memories.inc)
    if (mem) return mem != 3;
    return (!strncmp(flow, "Demo", 4) || !strncmp(flow, "SDemo_", 6) || !strncmp(flow, "Npc_King_", 9)) &&
           !story_own(flow) && strncmp(entry, "Talk", 4);
}
static bool text_at(uint64_t p, char *out, size_t cap) {
    if (!in_guest(p) || !in_guest(p + 64)) return false;
    const char *s = (const char *)(uintptr_t)p;
    size_t n = 0;
    while (n < cap - 1 && s[n] >= 0x20 && s[n] < 0x7f) n++;
    if (!n || n >= cap - 1 || s[n]) return false;
    memcpy(out, s, n + 1);
    return true;
}
// (2026-11-12) A Divine Beast's enter scene (it ends by moving Link inside: Water Demo344_1, Fire Demo346_6,
// Electric Demo347_3, Wind Demo348_0) takes a partner up to 400 m away: the approach is a fight around the beast
// (Sidon carries one player only), the other player may be on the shore.
static float story_range(const char *flow, const char *entry) {
    bool beast_enter = beast_on() && (!strncmp(flow, "Demo344_1", 9) || !strncmp(flow, "Demo346_6", 9) ||
                                      !strncmp(flow, "Demo347_3", 9) || !strncmp(flow, "Demo348_0", 9));
    if (story_monk(flow, entry)) return 100000.0f;  // (anywhere in the same shrine: the area check does the rest)
    return castle_story_range(flow, entry, beast_enter ? 400.0f : STORY_RADIUS);
}
static uint64_t story_busy_until;  // (dev `coop storybusy <s>`, FAKE: we act as if an event (a talk) ran until then)
static struct { EvStory e; bool pending; uint64_t at; } story_in;  // the other player's event, to start here
static struct { char flow[48], entry[48]; uint64_t at; } story_done[8];  // events that ran here (dedup)
static struct { char flow[48], entry[48]; uint64_t at; } story_mine;  // the event we started for them (not sent back)
static void story_ack(const char *flow, const char *entry, bool started);
static void story_ev(const uint8_t *data, int len) {
    if (len != (int)sizeof(EvStory)) return;
    EvStory e;
    memcpy(&e, data, sizeof e);
    e.flow[sizeof e.flow - 1] = e.entry[sizeof e.entry - 1] = 0;
    if (!story_on() || !story_shared(e.flow, e.entry) || !isfinite(e.pos[0]) || !isfinite(e.pos[1]) || !isfinite(e.pos[2])) {
        // (2026-11-13: answer "not started", or the other game holds its cutscene up to STORY_HOLD_NS for us - seen
        // with SAVAGEWIND_COOP_STORY=0 here)
        story_ack(e.flow, e.entry, false);
        return;
    }
    story_in.e = e, story_in.pending = true, story_in.at = plat_now_ns();
    LOG("[coop] story: the other player's cutscene %s / %s\n", e.flow, e.entry);
}
// ---- waiting for the partner (2026-11-13: if one finishes first, the game should wait for the other one to
// finish talking to play the cutscene, so everyone always sees the same thing). When a shared cutscene starts here
// and the partner is near enough to get it (same area, in range), our game stops at the cutscene's first frame (the
// game thread waits in story_hold) until the partner's game starts the same cutscene (or both started it on their
// own): then both play it from the start together. The partner answers with EV_STORY_ACK: started, or not started
// (too far, already seen, no free moment). We wait at most STORY_HOLD_NS, or until the partner is gone.
// SAVAGEWIND_COOP_STORYWAIT=0 turns the wait off (the partner then starts the cutscene late, as before).
#define EV_STORY_ACK 13
#define STORY_HOLD_NS 120000000000ull
typedef struct EvStoryAck { char flow[48], entry[48]; uint32_t started; } EvStoryAck;
_Static_assert(sizeof(EvStoryAck) <= COOP_EVENT_DATA && sizeof(EvStory) <= COOP_EVENT_DATA, "story events fit an event");
static bool story_wait_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_STORYWAIT") && !strcmp(getenv("SAVAGEWIND_COOP_STORYWAIT"), "0"));
    return on;
}
static void story_ack(const char *flow, const char *entry, bool started) {
    EvStoryAck a = {{0}, {0}, started};
    snprintf(a.flow, sizeof a.flow, "%s", flow);
    snprintf(a.entry, sizeof a.entry, "%s", entry);
    coop_net_send_event(EV_STORY_ACK, &a, sizeof a);
}
static void story_ack_ev(const uint8_t *data, int len) {  // (an answer that came after we stopped waiting: nothing to do)
    if (len != (int)sizeof(EvStoryAck)) return;
    EvStoryAck a;
    memcpy(&a, data, sizeof a);
    a.flow[sizeof a.flow - 1] = a.entry[sizeof a.entry - 1] = 0;
    LOG("[coop] story: late answer for %s / %s (%s there)\n", a.flow, a.entry, a.started ? "started" : "not started");
}
static void story_hold(Cpu *c, const char *flow, const char *entry) {
    uint64_t t0 = plat_now_ns();
    const char *why = "no answer in time";
    LOG("[coop] story: waiting for the other player before %s / %s\n", flow, entry);
    for (bool done = false; !done;) {
        plat_sleep_until_ns(plat_now_ns() + 5000000ull);
        if (plat_now_ns() - t0 > STORY_HOLD_NS) break;
        CoopState rs;
        double age = 99;
        if (!coop_net_get_remote(&rs, &age) || age > 5.0) { why = "the other player is gone"; break; }
        // (2026-11-13) a dead partner (game over, then a reload) reads no events until it is back: don't wait for it
        // (seen: 2 x 120 s held at the Naboris boss death while the host lay dead)
        if (rs.anim == COOP_ANIM_DIE || rs.anim == COOP_ANIM_SWIM_DIE || rs.anim == COOP_ANIM_HORSE_DEAD ||
            rs.anim == COOP_ANIM_MASTER_DEAD) { why = "the other player's Link is dead"; break; }
        uint16_t type;
        int len;
        uint8_t data[COOP_EVENT_DATA];
        int origin = 0;
        while (!done && coop_net_next_event_from(&type, data, &len, &origin)) {
            if (origin != coop_net_partner_id()) {
                world_event_from(c, type, data, len, origin);  // (58: another world player's: as usual)
            } else if (type == EV_STORY_ACK && len == (int)sizeof(EvStoryAck)) {
                EvStoryAck a;
                memcpy(&a, data, sizeof a);
                a.flow[sizeof a.flow - 1] = a.entry[sizeof a.entry - 1] = 0;
                if (!strcmp(a.flow, flow) && !strcmp(a.entry, entry)) {
                    why = a.started ? "it started there" : "it will not start there";
                    done = true;
                }
            } else if (type == EV_STORY && len == (int)sizeof(EvStory)) {
                EvStory e;
                memcpy(&e, data, sizeof e);
                e.flow[sizeof e.flow - 1] = e.entry[sizeof e.entry - 1] = 0;
                if (!strcmp(e.flow, flow) && !strcmp(e.entry, entry)) {  // (it started there on its own: both waiting)
                    story_ack(flow, entry, true);
                    why = "it started there too";
                    done = true;
                } else {
                    // (2026-11-14) the other game started ANOTHER cutscene and waits for us, while we wait for it: both
                    // held 2 x 120 s at Ganon's death (castle test). Say no to theirs and stop waiting.
                    story_ack(e.flow, e.entry, false);
                    why = "the other player is in another cutscene";
                    done = true;
                }
            } else {
                world_event_from(c, type, data, len, origin);  // (everything else as usual)
            }
        }
    }
    LOG("[coop] story: stopped waiting before %s / %s after %.1f s: %s\n", flow, entry, (plat_now_ns() - t0) / 1e9, why);
}
// ---- (2026-11-13 RE, Ghidra 0x7101117b08 = the Demo_CallDemo action, 0x7101117860 = its lookup) A cutscene called
// from inside a running event (a talk that ends in a cutscene) does not become a new event: the event's list object L
// keeps the preloaded flows (u32 count at +8, array of entries at +0x10; an entry's flow name char * at +0x18, entry
// name char * at +0x70) and a stack of entry indices (u8 at +0x1e8 + depth, depth u32 at +0x1f0; at most 7). The
// call pushes the called cutscene: depth > 0 while one plays. Probe: find L from the event by that shape.
static bool calldemo_shape(uint64_t l) {
    if (!in_guest(l) || !in_guest(l + 0x200)) return false;
    uint32_t n = LD32(l + 8), depth = LD32(l + 0x1f0);
    uint64_t arr = LD64(l + 0x10);
    if (n < 1 || n > 32 || depth > 7 || !in_guest(arr)) return false;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t e = LD64(arr + i * 8);
        char t[48];
        if (!in_guest(e) || !text_at(LD64(e + 0x18), t, sizeof t)) return false;
    }
    return true;
}
// (Probe 2026-11-13, a probe: L is the event object itself - Demo344_1 calling Demo120_0 read "depth 1
// index 1: Demo120_0 / Demo120_0" while it played.) Returns true (with its names) when a called cutscene starts.
static bool calldemo_watch(uint64_t ev, char flow[48], char entry[48]) {
    static uint64_t w_ev;
    static uint32_t w_depth;
    if (ev != w_ev) w_ev = ev, w_depth = 0;
    if (!calldemo_shape(ev)) return false;
    uint32_t depth = LD32(ev + 0x1f0), was = w_depth;
    w_depth = depth;
    if (depth <= was) return false;  // (no new call: the same, or one ended)
    uint8_t idx = LD8(ev + 0x1e8 + depth);
    if (idx >= LD32(ev + 8)) return false;
    uint64_t e = LD64(LD64(ev + 0x10) + idx * 8);
    if (!text_at(LD64(e + 0x18), flow, 48)) return false;
    if (!text_at(LD64(e + 0x70), entry, 48)) snprintf(entry, 48, "%s", flow);
    LOG("[coop] story: cutscene %s / %s called inside the running event (depth %u)\n", flow, entry, depth);
    return true;
}
// A cutscene started here (a new event, or one called from inside the running event): remember it (dedup), answer
// the other game if it was theirs, else tell them and wait for them when they will get it.
static void story_started(Cpu *c, const char *flow, const char *entry, uint64_t now, uint32_t here) {
    bool replayed = story_mine.at && now - story_mine.at < 30000000000ull && !strcmp(flow, story_mine.flow) &&
                    !strcmp(entry, story_mine.entry);
    if (replayed) story_mine.at = 0, story_ack(flow, entry, true);  // (the other game waits for this)
    // (2026-11-14) the other player's same cutscene, still waiting to start here: it just did (our own flow called it;
    // the castle's cutscenes run in both games' flows). Our EV_STORY below tells the waiting game.
    if (story_in.pending && !strcmp(story_in.e.flow, flow) && !strcmp(story_in.e.entry, entry)) story_in.pending = false;
    int k = 0;
    for (int i = 1; i < 8; i++)
        if (story_done[i].at < story_done[k].at) k = i;
    snprintf(story_done[k].flow, sizeof story_done[k].flow, "%s", flow);
    snprintf(story_done[k].entry, sizeof story_done[k].entry, "%s", entry);
    story_done[k].at = now;
    CoopState rs;
    double rage = 99;
    bool peer = coop_net_get_remote(&rs, &rage) && rage < 5.0;
    if (!replayed && story_shared(flow, entry) && peer) {
        EvStory e = {{0}, {0}, {0}, here};
        snprintf(e.flow, sizeof e.flow, "%s", flow);
        snprintf(e.entry, sizeof e.entry, "%s", entry);
        memcpy(e.pos, (void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
        bool sent = coop_net_send_event(EV_STORY, &e, sizeof e);
        LOG("[coop] story: cutscene %s / %s here: %s\n", flow, entry, sent ? "told the other player" : "queue full");
        // wait for the partner when it will get the cutscene (same area, in range, in the world)
        float rd = dist3(e.pos, rs.pos);
        if (sent && story_wait_on() && rs.area == here && rd <= story_range(flow, entry) && local_in_world())
            story_hold(c, flow, entry);
        else if (sent)
            LOG("[coop] story: not waiting for the other player (%s, %.0f m)\n",
                !story_wait_on() ? "turned off" : rs.area != here ? "another area" :
                !local_in_world() ? "loading" : "too far", rd);
    }
}
static void story_frame(Cpu *c) {
    if (!story_on()) return;
    uint64_t now = plat_now_ns();
    uint64_t mgr = g_site.event_mgr ? LD64(g_site.event_mgr) : 0, ev = in_guest(mgr) ? LD64(mgr + 0x1d2b8) : 0;
    static uint64_t last_ev;
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    {  // (2026-11-13 RE) log every change of the running event or of its names (a talk's Demo_CallDemo: same event?)
        static uint64_t w_ev;
        static char w_f[48], w_e[48];
        char f[48] = "", en[48] = "";
        if (in_guest(ev)) text_at(LD64(ev + 0x68), f, sizeof f), text_at(LD64(ev + 0xc0), en, sizeof en);
        if (ev != w_ev || strcmp(f, w_f) || strcmp(en, w_e)) {
            LOG("[coop] event now: %llx %s / %s\n", (unsigned long long)ev, f, en);
            w_ev = ev, memcpy(w_f, f, sizeof w_f), memcpy(w_e, en, sizeof w_e);
        }
    }
    if (ev != last_ev) {  // an event started here
        last_ev = ev;
        char flow[48], entry[48];
        if (in_guest(ev) && text_at(LD64(ev + 0x68), flow, sizeof flow) && text_at(LD64(ev + 0xc0), entry, sizeof entry))
        {
            story_started(c, flow, entry, now, here);
            shrine_door_scene_started(flow);  // (coop/gate.inc: the partner's pedestal scene with ours)
        }
    }
    {  // (2026-11-13) a cutscene called from inside the running event (a talk that ends in a cutscene)
        char flow[48], entry[48];
        char of[48] = "", oe[48] = "";
        // (inside a shared cutscene or a talk/quest flow (Npc_*, Kakariko_Cha_001, ...). Not inside flows each player runs
        // on their own: a beast's ClearRemains after each one's answer at the control unit, shops, armour upgrades,
        // the stables: their called cutscenes stay in order there)
        static const char *const solo[] = {"ClearRemains", "DisplayShop", "ArmorProcessing", "HorseAssociation"};
        bool ok = calldemo_watch(ev, flow, entry) && text_at(LD64(ev + 0x68), of, sizeof of) &&
                  text_at(LD64(ev + 0xc0), oe, sizeof oe);
        if (ok && (!strncmp(of, "Demo", 4) || !strncmp(of, "SDemo_", 6))) ok = story_shared(of, oe);
        for (size_t i = 0; ok && i < sizeof solo / sizeof *solo; i++)
            if (!strncmp(of, solo[i], strlen(solo[i]))) ok = false;
        if (ok) story_started(c, flow, entry, now, here);
    }
    static uint64_t monk_sent_ns;  // (the monk's barrier was signalled: its scene should start within a few frames)
    if (monk_sent_ns && now - monk_sent_ns > 5000000000ull && !in_guest(ev) && local_in_world()) {
        monk_sent_ns = 0;
        if (story_mine.at && !strcmp(story_mine.flow, "Demo033_0")) {  // (not started: story_mine still waits)
            bool ok = event_start(c, LD64(player_info_ptr() + 0x60), "Demo033_0", "Demo033_0");
            if (ok) story_mine.at = now;
            else story_mine.at = 0, story_ack("Demo033_0", "Demo033_0", false);
            LOG("[coop] story: the monk's barrier started nothing here; scene started directly: %s\n", ok ? "yes" : "no");
        }
    }
    if (!story_in.pending) return;
    EvStory *e = &story_in.e;
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    float d = dist3(me, e->pos);
    const char *why = NULL;
    for (int i = 0; i < 8 && !why; i++)
        if (story_done[i].at && now - story_done[i].at < STORY_DEDUP_NS && !strcmp(story_done[i].flow, e->flow) &&
            !strcmp(story_done[i].entry, e->entry))
            why = "it ran here already";
    if (!why && e->area != here) why = "another area";
    if (!why && !story_shared(e->flow, e->entry)) why = "not shared here now";  // (2026-11-14: e.g. a memory found since)
    if (!why && d > story_range(e->flow, e->entry)) why = "too far";
    {  // (2026-11-13) our Link is dead (the game over scene Demo006_0): say no at once, or the other game would hold
       // its cutscene up to STORY_HOLD_NS for us (seen: 120 s at the Naboris boss death while the host was dead)
        char bf[48];
        if (!why && in_guest(ev) && text_at(LD64(ev + 0x68), bf, sizeof bf) && !strncmp(bf, "Demo006_", 8))
            why = "our Link is dead";
    }
    if (why) {
        castle_missed(e->flow, e->entry, why);  // (2026-11-14) a dead player catches up on the boss's death later
        story_in.pending = false;
        story_ack(e->flow, e->entry, false);
        LOG("[coop] story: not starting %s / %s here (%s; %.0f m)\n", e->flow, e->entry, why, d);
        return;
    }
    if (now - story_in.at > STORY_WAIT_NS) {
        story_in.pending = false;
        story_ack(e->flow, e->entry, false);
        char bf[48] = "?", be[48] = "?";
        if (in_guest(ev)) text_at(LD64(ev + 0x68), bf, sizeof bf), text_at(LD64(ev + 0xc0), be, sizeof be);
        LOG("[coop] story: %s / %s not started here: no moment free of other events within %d s (running: %s / %s)\n",
            e->flow, e->entry, (int)(STORY_WAIT_NS / 1000000000ull), bf, be);
        return;
    }
    if (castle_story(e->flow, e->entry) == 2) {  // (2026-11-14) own-only: our own flow calls it, never alone
        if (now - story_in.at > CASTLE_OWN_WAIT) {
            story_in.pending = false;
            story_ack(e->flow, e->entry, false);
            LOG("[coop] story: %s / %s not reached by our own flow in %d s (not started alone)\n", e->flow, e->entry,
                (int)(CASTLE_OWN_WAIT / 1000000000ull));
        }
        return;
    }
    if (in_guest(ev) || !local_in_world() || now < story_busy_until) return;  // (wait: an event is running, or a load)
    uint64_t link = LD64(player_info_ptr() + 0x60), pa = memories_actor(c, e->flow, e->entry, link);  // (memories.inc)
    bool monk = story_monk(e->flow, e->entry) && monk_signal(c);
    if (monk) monk_sent_ns = now;
    if (monk) LOG("[coop] story: the monk's barrier signalled here (the other player talks to the monk)\n");
    if (monk || event_start(c, pa, e->flow, e->entry) || (pa != link && event_start(c, link, e->flow, e->entry))) {
        story_in.pending = false;
        // (the event becomes the manager's active one a frame or more later: recognise it by its names)
        snprintf(story_mine.flow, sizeof story_mine.flow, "%s", e->flow);
        snprintf(story_mine.entry, sizeof story_mine.entry, "%s", e->entry);
        story_mine.at = now;
        LOG("[coop] story: started the other player's cutscene %s / %s here (%.0f m from where it began there)\n",
            e->flow, e->entry, d);
    }
}

// `coop rbscan <name filter> [radius]` (dev, RE): for each live actor matching, its main (non-sensor) wrapper and
// where that wrapper pointer sits in the first 0x800 bytes of *(actor+0x190) (the actor's physics instance), to the log.
static struct { volatile int pending; char filter[64]; float radius; } rbscan_dev;
static bool rbscan_want(const char *n) { return strstr(n, rbscan_dev.filter) != NULL; }
