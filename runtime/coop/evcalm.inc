// Part of runtime/coop.c (included there, same translation unit): the partners' avatars during OUR events
// (2026-11-18: when someone interacts with something, the other players' animations did not show and they
// looked like moving statues. Also cutscenes could bug because all Links are in the cutscene, and a Link did
// something in the wrong place because he got pushed).
//
// DEMOCALM (SAVAGEWIND_COOP_DEMOCALM=0: off):
//  - while an event holds our Link (talk, item fanfare, cutscene: the event plays a scene / Demo* / Talk* AS on him),
//    the avatars' collision bodies are parked (objects.inc collide_frame): the event walks our Link along its marks
//    and a partner standing there shoved him off them (or was shoved);
//  - while our Link plays a SHARED cutscene (story.inc story_shared: the partner close by plays it too, their Link on
//    the same marks as ours), the avatars within EVCALM_HIDE_R of our Link are put out of sight (EVCALM_PARK_M under
//    their place: armor and gear follow the avatar), else a second Link stood in the scene doing something else.
#define EVCALM_HIDE_R 80.0f
#ifndef EVCALM_PARK_M
#define EVCALM_PARK_M 60.0f
#endif
static int democalm_c = -1;
static bool democalm_on(void) { return env_on("SAVAGEWIND_COOP_DEMOCALM", &democalm_c); }
static struct {
    bool held, scene;           // (this frame) an event holds our Link / it is a shared cutscene
    char flow[48], entry[48];  // the cutscene's names (log)
} evc;
static uint16_t as_name_index(const char *n);  // (coop/astree.inc)
static bool astree_table_ready(void);
// Our Link is driven by the running event: a scene AS not in the player's AS table ("C01-Link-A-0"), Demo* (DemoWait,
// DemoTurn...) or Talk*. A background event leaves him on his usual ASes (Wait, Run...): not held.
static bool evcalm_link_held(const char *as) {
    if (!as[0]) return false;  // (no AS: a load)
    if (starts(as, "Demo") || starts(as, "Talk")) return true;
    return astree_table_ready() && !as_name_index(as);
}
// Once a frame (rt_coop_game_frame, before the avatars are drawn).
static void evcalm_frame(void) {
    bool held = false, scene = false;
    char f[48] = "", en[48] = "";
    if (democalm_on() && framepace_in_event() && local_in_world()) {
        held = evcalm_link_held(shold_local_as);
        uint64_t mgr = g_site.event_mgr ? LD64(g_site.event_mgr) : 0, ev = in_guest(mgr) ? LD64(mgr + 0x1d2b8) : 0;
        if (held && in_guest(ev) && text_at(LD64(ev + 0x68), f, sizeof f)) {
            if (!text_at(LD64(ev + 0xc0), en, sizeof en)) snprintf(en, sizeof en, "%s", f);
            scene = story_shared(f, en);
            if (!scene && calldemo_shape(ev)) {  // (a cutscene called inside a talk: the old man's Demo027_0)
                uint32_t depth = LD32(ev + 0x1f0);
                uint8_t idx = depth ? LD8(ev + 0x1e8 + depth) : 0xff;
                uint64_t e = idx < LD32(ev + 8) ? LD64(LD64(ev + 0x10) + idx * 8) : 0;
                char cf[48], ce[48];
                if (in_guest(e) && text_at(LD64(e + 0x18), cf, sizeof cf)) {
                    if (!text_at(LD64(e + 0x70), ce, sizeof ce)) snprintf(ce, sizeof ce, "%s", cf);
                    if (story_shared(cf, ce)) scene = true, memcpy(f, cf, sizeof f), memcpy(en, ce, sizeof en);
                }
            }
        }
    }
    if (held != evc.held)
        LOG("[coop] evcalm: %s (AS %s)\n", held ? "an event holds our Link: avatar bodies parked" : "our Link is free again",
            shold_local_as);
    if (scene != evc.scene || (scene && strcmp(f, evc.flow)))
        LOG("[coop] evcalm: %s%s%s%s\n", scene ? "shared cutscene " : "shared cutscene over: avatars shown again",
            scene ? f : "", scene ? " / " : "", scene ? en : "");
    evc.held = held, evc.scene = scene;
    snprintf(evc.flow, sizeof evc.flow, "%s", f);
    snprintf(evc.entry, sizeof evc.entry, "%s", en);
}
static bool evcalm_held(void) { return evc.held; }
// The avatar at pos is put out of sight this frame (avatar.inc: its matrices are written EVCALM_PARK_M lower).
// The partner (the current peer view: multi.inc swaps it with the avatar) plays an event too: its body AS is a scene
// AS (not in the name table: sent as 0), Demo* or Talk*. A free partner stays visible (EVANIM keeps it animating).
static bool evcalm_partner_in_event(void) {
    CoopAsTree t;
    double age = 0;
    if (!coop_net_remote_astree(&t, &age) || age > 1.0 || !(t.flags & COOP_ASTREE_VALID)) return false;
    const char *n = t.as[0] < COOP_AS_NAME_COUNT ? coop_as_names[t.as[0]] : NULL;
    return !t.as[0] || !n || starts(n, "Demo") || starts(n, "Talk");
}
static bool evcalm_hide(const float pos[3]) {
    static bool was;
    uint64_t pi = evc.scene && evcalm_partner_in_event() ? player_info_ptr() : 0;
    bool hide = pi && dist3(pos, (const float *)(uintptr_t)(pi + 0x88)) < EVCALM_HIDE_R;
    if (hide != was)
        LOG("[coop] evcalm: avatar %s (at %.1f %.1f %.1f)\n", hide ? "hidden: the partner plays the cutscene too" : "shown",
            pos[0], pos[1], pos[2]);
    was = hide;
    return hide;
}

// ---- EVANIM (SAVAGEWIND_COOP_EVANIM=0: off): the partners' avatars keep animating while an event runs here.
// Observed (probe 2026-11-18): while an event pauses the other actors, the game calls each actor's
// "paused" job variants instead of the normal ones: jobs 1/2/4 of enemies.inc thaw_jobs run, jobs 0/3/5 do not
// (0x700151f8ec only freezes the AS: its time stays 0 while 0x700151f090 is the full calc with the AS update), and
// actor+0x518 gets bit 0x400, +0x524 bit 31. So for our avatars (and their armor puppets) the wrapper calls the
// normal partner job instead of the paused one: 1->0, 2->3, 4->5. EVANIM_BITS = which paused jobs are swapped
// (bit i = job i); SAVAGEWIND_COOP_EVANIM_BITS=<mask> / `coop evanim bits` override (tests).
#define EVANIM_MAX (COOP_MAX_PLAYERS * 20)
static int evanim_c = -1;
static bool evanim_on(void) { return env_on("SAVAGEWIND_COOP_EVANIM", &evanim_c); }
static uint32_t evanim_bits = 0x16;
static volatile uint64_t evanim_set[EVANIM_MAX];
static volatile int evanim_n;
static volatile uint32_t evanim_unpaused;  // (paused job calls swapped for the normal ones: log)
static bool evanim_job(Cpu *c, int i, uint64_t a) {
    static const int normal[6] = {-1, 0, 3, -1, 5, -1};
    int n = __atomic_load_n(&evanim_n, __ATOMIC_ACQUIRE);
    if (!n || !a || normal[i] < 0 || !((evanim_bits >> i) & 1)) return false;
    for (int k = 0; k < n; k++)
        if (evanim_set[k] == a) {
            thaw_orig[normal[i]](c);
            __atomic_fetch_add(&evanim_unpaused, 1, __ATOMIC_RELAXED);
            return true;
        }
    return false;
}
static void evanim_add(uint64_t *list, int *n, uint64_t a) {
    if (!a || !in_guest(a) || *n >= EVANIM_MAX) return;
    for (int k = 0; k < *n; k++)
        if (list[k] == a) return;
    list[(*n)++] = a;
}
static void evanim_add_pup(uint64_t *list, int *n, const struct Pup *p) {
    if (p->actor && pup_alive(p)) evanim_add(list, n, p->actor);
}
static void evanim_add_puppets(uint64_t *list, int *n, const struct Pup *armor, const struct Pup *horse,
                               const struct Pup *gear, const struct Pup *held, const struct Pup *glidefx) {
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++) evanim_add_pup(list, n, &armor[i]);
    for (int i = 0; i < HORSE_PARTS; i++) evanim_add_pup(list, n, &horse[i]);
    for (int i = 0; i < COOP_GEAR_COUNT; i++) evanim_add_pup(list, n, &gear[i]);
    for (int i = 0; i < COOP_HELD_ITEMS; i++) evanim_add_pup(list, n, &held[i]);
    evanim_add_pup(list, n, glidefx);
}
// Game thread, once a frame: the set of actors to keep animating (empty when no event runs here).
static void evanim_frame(void) {
    static bool init;
    if (!init) {
        init = true;
        const char *b = getenv("SAVAGEWIND_COOP_EVANIM_BITS");
        if (b && *b) evanim_bits = (uint32_t)strtoul(b, NULL, 0);
    }
    uint64_t list[EVANIM_MAX];
    int n = 0;
    if (evanim_on() && framepace_in_event() && local_in_world()) {
        if (avatar.actor && avatar_alive()) {
            evanim_add(list, &n, avatar.actor);
            evanim_add_puppets(list, &n, armor_pup, hr.p, gear_pup, held_pup, &glidefx_pup);
        }
        for (int k = 0; k < COOP_MAX_PLAYERS; k++) {
            MultiCtx *m = multi_ctx[k];
            if (!m || !m->avatar.actor || !av_ptr_alive(m->avatar.actor, m->avatar.unit, m->avatar.unit_id)) continue;
            evanim_add(list, &n, m->avatar.actor);
            evanim_add_puppets(list, &n, m->armor_pup, m->hr.p, m->gear_pup, m->held_pup, &m->glidefx_pup);
        }
    }
    static int was;
    if ((n > 0) != (was > 0))
        LOG("[coop] evanim: %s (%d actors, bits %x; %u job calls ran unpaused)\n",
            n ? "an event runs: avatars kept animating" : "event over", n, evanim_bits, (unsigned)evanim_unpaused);
    was = n;
    __atomic_store_n(&evanim_n, 0, __ATOMIC_SEQ_CST);  // (job threads: never see a half-written list)
    for (int k = 0; k < n; k++) evanim_set[k] = list[k];
    __atomic_store_n(&evanim_n, n, __ATOMIC_SEQ_CST);
}
// dev `coop evanim`: the avatar's flags, its 0.0 AS time and the job calls since the last one, to the log
static volatile int evanim_dev;
static void evanim_dev_frame(Cpu *c) {
    if (!evanim_dev) return;
    evanim_dev = 0;
    uint64_t a = avatar.actor;
    static uint32_t last[6];
    if (!a || !avatar_alive()) { LOG("[coop] evanim dev: no avatar\n"); return; }
    if (evjob_actor != a) { evjob_actor = a; memset((void *)evjob_calls, 0, sizeof evjob_calls); memset(last, 0, sizeof last); }
    uint32_t d[6];
    for (int i = 0; i < 6; i++) d[i] = evjob_calls[i] - last[i], last[i] = evjob_calls[i];
    uint64_t tm = as_layer_time(c, as_layer_rec(a, 0, 0));
    float fr[2] = {-1, -1};
    if (tm) memcpy(fr, (void *)(uintptr_t)(tm + 4), 8);
    LOG("[coop] evanim dev: event %d held %d | avatar %llx 518 %08x 51c %08x 520 %08x 524 %08x 685 %02x | jobs %u %u %u "
        "%u %u %u | AS 0.0 %s time %.2f rate %.2f | unpaused %u\n", framepace_in_event(), evc.held,
        (unsigned long long)a, LD32(a + 0x518), LD32(a + 0x51c), LD32(a + 0x520), LD32(a + 0x524), LD8(a + 0x685),
        d[0], d[1], d[2], d[3], d[4], d[5], as_layer_name(as_layer_rec(a, 0, 0)), fr[0], fr[1], (unsigned)evanim_unpaused);
}
