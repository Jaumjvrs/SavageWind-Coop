// Part of runtime/coop.c (included there, same translation unit): the Stasis and Cryonis runes in co-op
// (2026-11-14, protocol 43).
//
// ---- Stasis (game name "BitaLock"). The player's Stasis manager is a singleton, *(*(0x7001d6af70)); its calc
// FUN_7100b3d314 / FUN_7100b3cb60 runs from the player calc FUN_7100cd1ee8. The frozen target's time left is the float
// at mgr+0x48 (the game sends the release message 0x3000004 when it reaches 0), and the target is entry 0 of 16 entries
// at mgr+0x60 (stride 0x320) that start with an ActorLink, resolved like the game does with 0x700056d8d8(link, out):
// actor = out+8, then 0x7001126464(out) releases the handle. NOT verified live yet (written without a game run).
// The sync itself is objects_frame's (objects.inc): an object our Stasis holds is claimed by us at once and sent with
// COOP_OBJ_STASIS; the other game freezes its copy at that pose (fixed rigid body) until the flag clears and then
// follows the owner's launch like any owned object. SAVAGEWIND_COOP_STASIS=0 turns it off.
#define A_STASIS_MGR_SLOT 0x7001d6af70ull
#define A_HANDLE_RELEASE 0x7001126464ull
#define STASIS_LEFT 0x48
#define STASIS_ENTRY0 0x60
static bool stasis_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_STASIS") && !strcmp(getenv("SAVAGEWIND_COOP_STASIS"), "0"));
    return on;
}
// (n5, 2026-11-17: the other player's Stasis did not show on the local screen) The frozen copy is given the look the
// game gives a Stasis object, by the stasis manager's own messages to that actor (RE FUN_7100b3d314 / _d68c /
// _cb60: the manager sends through its transceiver mgr+0x3370 to the actor's message id *(actor+0x5f0) with
// 0x700140409c: 0x3000005 = aimed at (highlight on), 0x3000006 = highlight off, 0x3000003 = lock, 0x3000004 =
// release, 0x300000a / 0x300000b = the countdown blink on / off). Probes 2026-11-17: the highlight messages change
// nothing visible; the lock message makes our own manager lock the copy, which draws the real yellow Stasis glow. So
// the copy is locked by our manager while the other player's Stasis holds it (marked stasis_shown: our sync does not
// take it as our player's Stasis) and released when theirs ends. Not shown while our own Stasis holds something; the
// chains start and fade on their own. stasisfx.inc replays the strike arrow and supplies chains if the native
// start effect never ran. SAVAGEWIND_COOP_STASISFX: 1 / unset = show these visuals;
// elink = the old experiment (ELink keys BitaLocked_Start / _End on the copy: "no effect user", nothing shown); 0 =
// off. SAVAGEWIND_COOP_STASISFX_ON / _OFF=<hex>: other message ids (dev).
static int stasis_fx_mode(void) {  // 0 off, 1 messages, 2 elink
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_STASISFX");
        m = !e || !*e || !strcmp(e, "1") ? 1 : !strcmp(e, "elink") ? 2 : 0;
    }
    return m;
}
static bool stasis_fx_on(void) { return stasis_fx_mode() != 0; }
static uint64_t stasis_mgr(void) {
    uint64_t slot = LD64(A_STASIS_MGR_SLOT);
    if (!in_guest(slot)) return 0;
    uint64_t mgr = LD64(slot);
    return in_guest(mgr) && in_guest(mgr + 0x400) ? mgr : 0;
}
static uint64_t stasis_out;  // guest scratch for the link resolve (reset on reboot)
static uint64_t stasis_shown;  // (n5) our copy that our manager locks only to show the other player's Stasis: not ours
static uint64_t stasis_request_ns;
// The actor our Stasis holds now, or 0.
static uint64_t stasis_link_actor(Cpu *c, uint64_t link) {
    if (!stasis_out) stasis_out = (uint64_t)(uintptr_t)hle_alloc(0x20);
    if (!stasis_out) return 0;
    memset((void *)(uintptr_t)stasis_out, 0, 0x20);
    Cpu saved = *c;
    c->x[0] = link; c->x[1] = stasis_out;
    guest_call(c, A_EQUIP_GET);
    *c = saved;
    uint64_t a = LD64(stasis_out + 8);
    c->x[0] = stasis_out;
    guest_call(c, A_HANDLE_RELEASE);
    *c = saved;
    return in_guest(a) && in_guest(a + 0x1000) && LD8(a + PROC_STATE) != 3 ? a : 0;
}
static uint64_t stasis_manager_target(Cpu *c) {
    uint64_t mgr = stasis_on() ? stasis_mgr() : 0;
    if (!mgr) return 0;
    float left = LDF32(mgr + STASIS_LEFT);
    return left > 0 && left <= 100000 ? stasis_link_actor(c, mgr + STASIS_ENTRY0) : 0;
}
static uint64_t stasis_target(Cpu *c) {
    uint64_t a = stasis_manager_target(c);
    return a != stasis_shown ? a : 0;
}
// `coop stasisfake <hash> [s]` (dev): objects_frame treats that shared object as frozen by our Stasis for s seconds
// (default 8; the body is fixed here like the game's Stasis does), to test the sync without using the rune.
static struct { volatile uint32_t hash; volatile uint64_t until_ns; } stasis_fake;
static bool stasis_fake_holds(uint32_t hash, uint64_t now) {
    return stasis_on() && stasis_fake.hash && stasis_fake.hash == hash && now < stasis_fake.until_ns;
}
#define A_MSG_SEND 0x700140409cull
#define STASIS_TX 0x3370
#define STASIS_MSG_HL_ON 0x3000005u   // (probe: no visible change on the copy)
#define STASIS_MSG_HL_OFF 0x3000006u
#define STASIS_MSG_LOCK 0x3000003u    // (probe 2026-11-17: the real yellow Stasis glow on the copy)
#define STASIS_MSG_RELEASE 0x3000004u
#define ACTOR_MSG_ID 0x5f0
static uint64_t stasis_msg_mem;
static bool stasis_msg(Cpu *c, uint64_t actor, uint32_t msg) {
    uint64_t mgr = stasis_mgr(), id = in_guest(actor) ? LD64(actor + ACTOR_MSG_ID) : 0;
    if (!mgr || !in_guest(id)) return false;
    if (!stasis_msg_mem) stasis_msg_mem = (uint64_t)(uintptr_t)hle_alloc(0x10);
    if (!stasis_msg_mem) return false;
    ST32(stasis_msg_mem, msg);
    Cpu saved = *c;
    c->x[0] = mgr + STASIS_TX; c->x[1] = id; c->x[2] = stasis_msg_mem; c->x[3] = 0; c->x[4] = 1;
    guest_call(c, A_MSG_SEND);
    *c = saved;
    return true;
}
static uint32_t stasis_fx_msg(bool on) {
    const char *e = getenv(on ? "SAVAGEWIND_COOP_STASISFX_ON" : "SAVAGEWIND_COOP_STASISFX_OFF");
    return e && *e ? (uint32_t)strtoul(e, NULL, 16) : on ? STASIS_MSG_LOCK : STASIS_MSG_RELEASE;
}
// (n6, STASISCD) After the shown-only lock ends, our manager's own release (FUN_7100b3d194, run by its calc once no
// entry is locked and the "locked" byte mgr+0x42 is set) starts OUR rune's recharge: it writes the recharge time
// mgr+0x44 (the calc counts it down by 1 per frame while mgr+0x44 * mgr+0x58 > 0; the HUD's recharge circle). Our
// player never used the rune, so the recharge time from before the lock is put back once the manager has released it
// (mgr+0x42 back to 0), unless our own Stasis is busy again by then. SAVAGEWIND_COOP_STASISCD=0: off (the circle shows).
#define STASIS_RECHARGE 0x44
#define STASIS_LOCKED 0x42
static int stasiscd_c = -1;
static struct { bool armed, watch; float saved; uint64_t lock_ns, until_ns; } stasis_cd;
static void stasis_cd_frame(Cpu *c, uint64_t now) {
    if (!stasis_cd.watch) return;
    uint64_t mgr = stasis_mgr();
    if (!mgr || now > stasis_cd.until_ns) {
        stasis_cd.watch = false;
        LOG("[coop] stasis: recharge not put back (%s)\n", mgr ? "the manager did not release within 3 s" : "no manager");
        return;
    }
    if (LD8(mgr + STASIS_LOCKED)) return;  // (not released yet)
    stasis_cd.watch = false;
    if (stasis_target(c)) return;  // (our own Stasis holds something now: its recharge is real)
    float cur;
    memcpy(&cur, (const void *)(uintptr_t)(mgr + STASIS_RECHARGE), 4);
    // (the old recharge, less the frames it would have counted down meanwhile at ~30 per second; the calc subtracts 1)
    float left = stasis_cd.saved - (float)((now - stasis_cd.lock_ns) / 1e9 * 30.0);
    if (left < 0) left = 0;
    memcpy((void *)(uintptr_t)(mgr + STASIS_RECHARGE), &left, 4);
    LOG("[coop] stasis: our rune's recharge put back %.1f -> %.1f (the lock only showed the other player's Stasis)\n",
        cur, left);
}
static bool stasis_busy(Cpu *c) {
    // A queued request can precede manager acknowledgement. A streamed-out
    // object can leave stasis_shown set after its native lock has expired.
    return (stasis_shown && plat_now_ns() - stasis_request_ns < 500000000ull) || stasis_manager_target(c);
}
static void stasis_native_copy(Cpu *c, uint64_t actor, bool start, uint32_t msg) {
    // Our own manager locks the copy (that draws the glow); stasis_shown keeps our sync from taking it as our
    // player's Stasis. Not done while our player's own Stasis holds something (one lock per manager).
    if (start && stasis_busy(c)) {
        LOG("[coop] stasis: no glow on our copy %llx: our own Stasis is in use\n", (unsigned long long)actor);
        return;
    }
    if (!start && actor != stasis_shown) return;
    uint64_t mgr = stasis_mgr();
    if (start && mgr && env_on("SAVAGEWIND_COOP_STASISCD", &stasiscd_c)) {  // (STASISCD: the recharge before)
        memcpy(&stasis_cd.saved, (const void *)(uintptr_t)(mgr + STASIS_RECHARGE), 4);
        if (!(stasis_cd.saved >= 0 && stasis_cd.saved < 100000)) stasis_cd.saved = 0;
        stasis_cd.armed = true, stasis_cd.watch = false, stasis_cd.lock_ns = plat_now_ns();
    }
    stasis_shown = actor;
    stasis_request_ns = plat_now_ns();
    bool ok = stasis_msg(c, actor, msg);
    if (start && !ok) stasis_cd.armed = false;
    if (!start && stasis_cd.armed) {
        stasis_cd.armed = false, stasis_cd.watch = true, stasis_cd.until_ns = plat_now_ns() + 3000000000ull;
    }
    if (!start || !ok) stasis_shown = 0;
    LOG("[coop] stasis: message %x (%s) to our copy %llx: %s\n", msg, start ? "glow on" : "glow off",
        (unsigned long long)actor, ok ? "sent" : "no manager / id");
}
static void stasis_fx(Cpu *c, uint64_t actor, bool start) {
    if (!stasis_fx_on()) return;
    if (stasis_fx_mode() == 1) {
        stasis_native_copy(c, actor, start, stasis_fx_msg(start));
        return;
    }
    bool ok = fx_emit(c, actor, start ? "BitaLocked_Start" : "BitaLocked_End");
    LOG("[coop] stasis: effect %s on our copy %llx: %s\n", start ? "BitaLocked_Start" : "BitaLocked_End",
        (unsigned long long)actor, ok ? "fired" : "no effect user");
}

// ---- Cryonis (game name "IceMaker"). The manager singleton is *(*(0x7001d67ae8)) (constructed by FUN_7100b2d10c from
// FUN_7100c3da18). Making a block (FUN_7100b2e364): matrix from the hit point and normal (FUN_7100b2e17c), creation
// param "@M" (Matrix34f, 0x30 bytes, type 7), actor Obj_IceMakerBlock (preloaded into the link at mgr+0x170 by
// FUN_7100b2d8e0; the manager's own blocks are listed at mgr+0x40, count mgr+0x38, at most 3). Breaking one
// (FUN_7100b2e03c(mgr, id, 1)): removes it from that list if it is there and sends it message 0x8000004 through the
// manager's transceiver (mgr+0x90); the block's own AI then breaks it. id = the actor's message id, *(actor+0x5f0)
// (FUN_7101070e58). That call works for blocks that are not in the list too: our copies break the game's way.
// Here: every block our player makes (every Obj_IceMakerBlock that is not a copy of ours) is sent with its matrix once
// it stands still (EV 17 CREATE); the other game makes a copy with that matrix. Whichever block or copy vanishes, its
// game sends GONE and the other game breaks its own. Only while the players are together (same shrine; overworld
// within 120 m) and in the world. SAVAGEWIND_COOP_CRYONIS=0 turns it off.
#define A_ICE_MGR_SLOT 0x7001d67ae8ull
#define A_ICE_BREAK 0x7000b2e03cull
#define ICE_MAX 8
#define ICE_NAME "Obj_IceMakerBlock"
#define ICE_KILL_NS 3000000000ull  // a block we asked to break still here after this: deleted by force
static bool cryonis_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_CRYONIS") && !strcmp(getenv("SAVAGEWIND_COOP_CRYONIS"), "0"));
    return on;
}
struct Ice {
    uint64_t actor, unit;
    uint32_t unit_id, id;
    float m[12];          // the matrix sent / received
    uint64_t found_ns, stable_ns, kill_ns, placed_ns;
    bool sent;            // (ours) its CREATE went out while we were together
    bool killing;         // we asked the game to break it (the other game's GONE): no GONE back
    bool idle;            // (ours) a pooled block actor without a position yet (logged once)
    uint64_t ai_off_ns;   // (n5, copy) when its AI is switched off (0: done / never)
    int remakes;          // (n5, copy) made again after it broke by itself
};
static struct Ice ice_mine[ICE_MAX], ice_copy[ICE_MAX];
static int ice_mine_n, ice_copy_n;
static uint32_t ice_area, ice_next;
static uint64_t ice_look_ns;
static volatile int ice_report;          // `coop runes`
static volatile int ice_dev_make, ice_dev_break;  // `coop icemake`, `coop icebreak`

// (n5, 2026-11-17, play report 19: "climbing the partner's block breaks it") A copy is made outside the rune manager;
// its own AI broke it (on a floor within a second; on water when our Link climbed it), and that break was sent as
// GONE and broke the partner's real block. Now a copy's AI is switched off once it has appeared (the game's skip-AI
// bit, as following enemies: it stays a solid, climbable block that never breaks by itself), and a copy that vanishes
// anyway is sent as GONE only when our player was using a rune just before (our Cryonis broke it); otherwise it is
// made again (at most ICE_REMAKES times). A break the other game asks for switches the AI back on first, so the
// block breaks the game's way. SAVAGEWIND_COOP_ICECOPY=0: as before n5. SAVAGEWIND_COOP_ICECOPY_AI_MS=<ms>: how long
// after its creation the copy's AI is switched off (default ICECOPY_AI_MS: its rise out of the water plays first).
#define ICE_REMAKES 5
#define ICECOPY_AI_MS 3000  // (probe 2026-11-17: AI off at once = never rises, Link falls through; 1.2 s = 2.5 m;
                            //  2.5 s and 4 s = full height ~4 m, Link stands on it)
#define ICE_RUNE_NS 3000000000ull  // our rune aimed this recently: a copy that vanished was broken by our player
static int icecopy_c = -1, icegrace_c = -1;
static bool icecopy_on(void) { return env_on("SAVAGEWIND_COOP_ICECOPY", &icecopy_c); }
static bool icegrace_on(void) { return env_on("SAVAGEWIND_COOP_ICEGRACE", &icegrace_c); }
static uint64_t icecopy_ai_ns(void) {
    static int ms = -1;
    if (ms < 0) ms = getenv("SAVAGEWIND_COOP_ICECOPY_AI_MS") ? atoi(getenv("SAVAGEWIND_COOP_ICECOPY_AI_MS")) : ICECOPY_AI_MS;
    if (ms < 0) ms = 0;
    return (uint64_t)ms * 1000000ull;
}
static volatile int ice_dev_copy;  // `coop icecopy` (dev)
// (bugs B4, 2026-11-19) A copy that vanished is sent as GONE only when OUR Cryonis broke it: the selected rune
// (GameData PlayerEquipItemType, 4 = Cryonis) was Cryonis when the rune was aimed, and the copy is within reach of our
// Link. Before, any rune aimed in the last 3 s counted (a bomb or Magnesis used while a copy broke by itself would
// break the partner's real block). SAVAGEWIND_COOP_ICEOWN=0: as before.
#define ICE_RUNE_CRYONIS 4
#define ICE_OWN_REACH 60.0f  // metres from our Link: Cryonis cannot reach a block further away
static int iceown_c = -1;
static bool iceown_on(void) { return env_on("SAVAGEWIND_COOP_ICEOWN", &iceown_c); }
static uint64_t cryonis_aim_ns, aim_seen_ns;

static bool ice_name(const char *n) { return !strcmp(n, ICE_NAME); }
static bool ice_alive(const struct Ice *b) {
    return in_guest(b->actor) && in_guest(b->unit) && LD32(b->unit + UNIT_ID) == b->unit_id &&
           LD64(b->unit + UNIT_ACTOR) == b->actor && LD8(b->actor + PROC_STATE) != 3;
}
static void ice_set_actor(struct Ice *b, uint64_t a) {
    b->actor = a, b->unit = LD64(a + PROC_UNIT), b->unit_id = LD32(a + PROC_ID);
}
static struct Ice *ice_find(struct Ice *t, int n, uint32_t id) {
    for (int i = 0; i < n; i++)
        if (t[i].id == id) return &t[i];
    return NULL;
}
// Break a block the game's way (its AI plays the break); deleted by force later if it is still there (ice_frame).
static bool ice_break(Cpu *c, struct Ice *b) {
    uint64_t slot = LD64(A_ICE_MGR_SLOT), mgr = in_guest(slot) ? LD64(slot) : 0;
    uint64_t id = in_guest(b->actor) ? LD64(b->actor + ACTOR_MSG_ID) : 0;
    b->killing = true, b->kill_ns = plat_now_ns() + ICE_KILL_NS;
    if (in_guest(b->actor) && (LD8(b->actor + ENEMY_FLAGS) & ENEMY_SKIP_AI))  // (n5: a copy's AI on again, to break)
        ST8(b->actor + ENEMY_FLAGS, (uint8_t)(LD8(b->actor + ENEMY_FLAGS) & ~ENEMY_SKIP_AI));
    b->ai_off_ns = 0;
    if (!in_guest(mgr) || !in_guest(id)) return false;
    Cpu saved = *c;
    c->x[0] = mgr; c->x[1] = id; c->x[2] = 1;
    guest_call(c, A_ICE_BREAK);
    *c = saved;
    return true;
}
static void ice_send(const struct Ice *b, int kind, uint32_t area) {
    CoopIce e;
    memset(&e, 0, sizeof e);
    e.id = b->id, e.area = area, e.kind = (uint8_t)kind;
    memcpy(e.m, b->m, sizeof e.m);
    bool ok = coop_net_send_event(COOP_EV_ICE, &e, sizeof e);
    LOG("[coop] cryonis: %s %08x at %.2f %.2f %.2f sent%s\n", kind == COOP_ICE_CREATE ? "block" : "gone", b->id,
        b->m[3], b->m[7], b->m[11], ok ? "" : " - NOT queued");
}
// (n6, ICEKEEP) The copies forgotten at a load blip. A goto inside a shrine (and maybe a short load) turned
// local_in_world() off for a moment: the copy actor stayed, and the next scan took it as OUR block and sent it back to
// the other player (seen 2026-11-17, real Cryonis probe). Such an actor (same process id) becomes a copy again.
static struct Ice ice_gone_copy[ICE_MAX];
static int ice_gone_n;
static uint64_t ice_gone_ns;
static int icekeep_c = -1;
// (n6, ICESAFE) Is p on or beside one of our real blocks (2.5 m across, up to 4 m tall)? The avatar's collision body
// is kept off there (objects.inc): touching it broke the block (probe 2026-11-17).
static int icesafe_c = -1;
static bool ice_mine_near(const float p[3]) {
    if (!ice_mine_n || !env_on("SAVAGEWIND_COOP_ICESAFE", &icesafe_c)) return false;
    for (int i = 0; i < ice_mine_n; i++) {
        const struct Ice *b = &ice_mine[i];
        if (!in_guest(b->actor)) continue;  // (not b->idle: it stays set after a late placement)
        const float *m = (const float *)(uintptr_t)(b->actor + ACTOR_MTX);
        if (m[3] == 0 && m[7] == 0 && m[11] == 0) continue;  // (a pooled block without a position)
        float dx = p[0] - m[3], dz = p[2] - m[11], dy = p[1] - m[7];
        if (dx * dx + dz * dz < 2.5f * 2.5f && dy > -2.0f && dy < 6.0f) return true;
    }
    return false;
}
static void ice_found(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)name;
    const float *me = ctx;
    for (int i = 0; i < ice_copy_n; i++)
        if (ice_copy[i].actor == a) return;
    for (int i = 0; i < ice_mine_n; i++)
        if (ice_mine[i].actor == a) return;
    for (int i = 0; i < ice_gone_n; i++)
        if (ice_gone_copy[i].actor == a && ice_gone_copy[i].unit_id == LD32(a + PROC_ID) &&
            plat_now_ns() - ice_gone_ns < 60000000000ull && ice_copy_n < ICE_MAX) {
            ice_copy[ice_copy_n++] = ice_gone_copy[i];
            ice_gone_copy[i].actor = 0;
            LOG("[coop] cryonis: copy %08x (actor %llx) still here after the load: kept as a copy\n", ice_gone_copy[i].id,
                (unsigned long long)a);
            return;
        }
    if (ice_mine_n == ICE_MAX) return;
    float p[3];
    if (actor_dist(a, me, p) > 120.0f) return;
    struct Ice *b = &ice_mine[ice_mine_n++];
    memset(b, 0, sizeof *b);
    ice_set_actor(b, a);
    b->id = COOP_ICE_ID | coop_net_id_tag() | (++ice_next & 0x03ffffffu);
    memcpy(b->m, (const void *)(uintptr_t)(a + ACTOR_MTX), 48);
    b->found_ns = b->stable_ns = plat_now_ns();
    LOG("[coop] cryonis: our block %08x (actor %llx) at %.2f %.2f %.2f\n", b->id, (unsigned long long)a, b->m[3],
        b->m[7], b->m[11]);
}
static bool ice_matrix_ok(const float m[12]) {
    CoopIce e;
    memset(&e, 0, sizeof e);
    e.id = COOP_ICE_ID, e.area = 1, e.kind = COOP_ICE_CREATE;
    memcpy(e.m, m, sizeof e.m);
    return coop_ice_ok(&e);
}
// A copy of the other player's block (from its CREATE, or made again: n5).
static struct Ice *ice_copy_make(Cpu *c, uint32_t id, const float m[12], int remakes) {
    if (ice_copy_n == ICE_MAX) { LOG("[coop] cryonis: block %08x: too many copies, ignored\n", id); return NULL; }
    uint64_t a = spawn_mtx_now(c, ICE_NAME, m);
    if (!a) { LOG("[coop] cryonis: block %08x: the game refused the copy\n", id); return NULL; }
    struct Ice *b = &ice_copy[ice_copy_n++];
    memset(b, 0, sizeof *b);
    ice_set_actor(b, a);
    b->id = id, b->found_ns = plat_now_ns(), b->remakes = remakes;
    memcpy(b->m, m, sizeof b->m);
    if (icecopy_on()) b->ai_off_ns = b->found_ns + icecopy_ai_ns() + 1;  // (n5)
    const float *am = (const float *)(uintptr_t)(a + ACTOR_MTX);
    LOG("[coop] cryonis: block %08x from the other player: copy %llx at %.2f %.2f %.2f (asked %.2f %.2f %.2f)%s\n", id,
        (unsigned long long)a, am[3], am[7], am[11], m[3], m[7], m[11], remakes ? " - made again" : "");
    return b;
}
static void ice_frame(Cpu *c) {
    if (!cryonis_on()) return;
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    bool overworld = !strcmp(area, "MainField");
    if (here != ice_area || !local_in_world()) {  // (a load: the blocks are gone)
        if (ice_mine_n || ice_copy_n)
            LOG("[coop] cryonis: %d blocks, %d copies forgotten (area change or load)\n", ice_mine_n, ice_copy_n);
        if (ice_copy_n && here == ice_area && env_on("SAVAGEWIND_COOP_ICEKEEP", &icekeep_c))
            memcpy(ice_gone_copy, ice_copy, sizeof *ice_copy * (size_t)ice_copy_n), ice_gone_n = ice_copy_n,
                ice_gone_ns = now;
        else if (here != ice_area)
            ice_gone_n = 0;
        ice_mine_n = ice_copy_n = 0, ice_area = here;
        return;
    }
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    CoopState rs;
    double rage = 99;
    bool together = coop_net_get_remote(&rs, &rage) && rage < 3.0 && (rs.flags & COOP_FLAG_IN_WORLD) &&
                    rs.area == here && (!strncmp(area, "CDungeon", 8) || (overworld && dist3(me, rs.pos) < 120.0f));
    if (rune_aim_ns != aim_seen_ns) {  // (bugs B4) which rune was it? (the avatar code only sees "a rune is aimed")
        int32_t sel = -1;
        aim_seen_ns = rune_aim_ns;
        if (gdt_get_s32("PlayerEquipItemType", &sel) && sel == ICE_RUNE_CRYONIS) cryonis_aim_ns = rune_aim_ns;
    }
    if (ice_dev_make) {  // `coop icemake` (dev): a block 3 m north of our Link, as if our player made it
        ice_dev_make = 0;
        float m[12] = {1, 0, 0, me[0], 0, 1, 0, me[1], 0, 0, 1, me[2] + 3.0f};
        uint64_t a = spawn_mtx_now(c, ICE_NAME, m);
        float *am = (float *)(uintptr_t)(a + ACTOR_MTX);
        LOG("[coop] cryonis: icemake -> actor %llx%s at %.2f %.2f %.2f (asked %.2f %.2f %.2f)\n", (unsigned long long)a,
            a ? "" : " (refused)", a ? am[3] : 0.f, a ? am[7] : 0.f, a ? am[11] : 0.f, m[3], m[7], m[11]);
    }
    if (ice_dev_break) {  // `coop icebreak` (dev): break our blocks, as if our player used Cryonis on them
        ice_dev_break = 0;
        for (int i = 0; i < ice_mine_n; i++) {
            bool ok = ice_break(c, &ice_mine[i]);
            ice_mine[i].killing = false;  // (our own break: still sent when it vanishes)
            LOG("[coop] cryonis: icebreak %08x: %s\n", ice_mine[i].id, ok ? "asked" : "no manager / id");
        }
    }
    // Copies: one that vanished without our asking was broken by our player (Cryonis on it): the other game breaks
    // the original. One we asked to break and that is still here after ICE_KILL_NS: deleted.
    if (ice_dev_copy) {  // `coop icecopy` (dev): a copy 3 m north of our Link, as if the other player made a block there
        ice_dev_copy = 0;
        float m[12] = {1, 0, 0, me[0], 0, 1, 0, me[1], 0, 0, 1, me[2] + 3.0f};
        ice_copy_make(c, COOP_ICE_ID | 0x03fffff0u | (ice_next++ & 0xf), m, 0);
    }
    for (int i = 0; i < ice_copy_n; i++) {
        struct Ice *b = &ice_copy[i];
        if (!ice_alive(b)) {
            // (2026-11-16: a block made by our code, not by the rune, can break by itself within a second - probe:
            // `coop icemake` on the shrine floor. Such a copy's end is not our player's Cryonis: the original stays.)
            // While the copy's AI-off timer is still pending (ICECOPY_AI_MS = 3000 ms by default vs 2500 ms above), its
            // AI is still on, so a self-break before ai_off_ns fires must not break the partner's original block.
            // SAVAGEWIND_COOP_ICEGRACE=0: keep the 2.5 s cutoff only.
            bool rising = icecopy_on() && icegrace_on() && b->ai_off_ns && now < b->ai_off_ns;
            bool young = now - b->found_ns < 2500000000ull || rising;
            // (n5) only our player's Cryonis (a rune aimed just before) breaks the original; else the copy comes back
            bool ours = !icecopy_on() || now - rune_aim_ns < ICE_RUNE_NS;
            if (ours && icecopy_on() && iceown_on()) {  // (bugs B4) our Cryonis, and this block within its reach
                float bp[3] = {b->m[3], b->m[7], b->m[11]};
                bool cry = now - cryonis_aim_ns < ICE_RUNE_NS, near = dist3(bp, me) < ICE_OWN_REACH;
                if (!cry || !near)
                    LOG("[coop] cryonis: copy %08x gone: a rune was aimed but %s: not ours\n", b->id,
                        !cry ? "it was not Cryonis" : "the block is out of Cryonis reach");
                ours = cry && near;
            }
            if (!b->killing && together && !young && ours) ice_send(b, COOP_ICE_GONE, here);
            LOG("[coop] cryonis: copy %08x gone (%s)\n", b->id,
                b->killing ? "their break" : young ? "broke by itself right after it was made: not sent"
                : ours ? "broken here" : "broke by itself (no rune of ours): not sent");
            struct Ice old = *b;
            ice_copy[i--] = ice_copy[--ice_copy_n];
            if (icecopy_on() && !old.killing && !ours && together && old.remakes < ICE_REMAKES)
                ice_copy_make(c, old.id, old.m, old.remakes + 1);
            continue;
        } else if (b->ai_off_ns && now >= b->ai_off_ns && !b->killing) {  // (n5) its rise played: AI off
            b->ai_off_ns = 0;
            ST8(b->actor + ENEMY_FLAGS, (uint8_t)(LD8(b->actor + ENEMY_FLAGS) | ENEMY_SKIP_AI));
            LOG("[coop] cryonis: copy %08x: its AI is off (a solid block that does not break by itself)\n", b->id);
        } else if (b->killing && now > b->kill_ns) {
            LOG("[coop] cryonis: copy %08x still here after its break: deleted\n", b->id);
            uint64_t a = b->actor;
            ice_copy[i--] = ice_copy[--ice_copy_n];
            despawn_now(c, a, ACTOR_DELETE_REASON);
        } else if (!together && !b->killing) {
            // (the other player left / the connection dropped: their blocks are not here any more)
            LOG("[coop] cryonis: copy %08x: not together any more, broken\n", b->id);
            ice_break(c, b);
        }
    }
    for (int i = 0; i < ice_mine_n; i++) {
        struct Ice *b = &ice_mine[i];
        if (!ice_alive(b)) {
            if (b->sent && !b->killing && together) ice_send(b, COOP_ICE_GONE, here);
            LOG("[coop] cryonis: our block %08x gone (%s)\n", b->id, b->killing ? "their break" : "broken here");
            ice_mine[i--] = ice_mine[--ice_mine_n];
            continue;
        }
        if (b->killing && now > b->kill_ns) {
            LOG("[coop] cryonis: our block %08x still here after its break: deleted\n", b->id);
            uint64_t a = b->actor;
            ice_mine[i--] = ice_mine[--ice_mine_n];
            despawn_now(c, a, ACTOR_DELETE_REASON);
            continue;
        }
        if (!together) { b->sent = false; continue; }  // (sent again when together again)
        if (b->killing) continue;
        // (probe w2c, 2026-11-16: the block actor is pooled - it exists from the load with an all-zero matrix, gets a
        // position when our player makes a block and goes back to zeros when that block breaks. It used to be given
        // up after 20 s without a position ("sent"), so the real block made later on it never went out.)
        const float *zm = (const float *)(uintptr_t)(b->actor + ACTOR_MTX);
        bool zero = fabsf(zm[3]) + fabsf(zm[7]) + fabsf(zm[11]) <= 0.01f;
        if (zero && b->placed_ns) {  // (its block broke: the pooled actor is free again)
            if (b->sent) ice_send(b, COOP_ICE_GONE, here);
            LOG("[coop] cryonis: our block %08x back to no position (broken): waits for the next block\n", b->id);
            b->sent = false, b->placed_ns = 0, b->idle = false, b->found_ns = b->stable_ns = now;
            b->id = COOP_ICE_ID | coop_net_id_tag() | (++ice_next & 0x03ffffffu);  // (new block)
            continue;
        }
        if (b->sent) continue;
        // Sent once it has stood still for 0.5 s (it rises out of the water while it appears), at most 3 s after its
        // position is known. (2026-11-16, play log Dungeon065: a new block's matrix reads all zeros for its first
        // seconds; it was sent after 3 s as 0,0,0, the copy appeared at the shrine's centre, broke there, its GONE broke
        // the real block - a loop: "it goes up and down really fast". Now a block is sent only with a real matrix.)
        const float *am = (const float *)(uintptr_t)(b->actor + ACTOR_MTX);
        bool placed = (fabsf(am[3]) + fabsf(am[7]) + fabsf(am[11]) > 0.01f) && ice_matrix_ok(am);
        if (!placed) {
            b->stable_ns = now;
            if (now - b->found_ns > 20000000000ull && !b->idle) {
                LOG("[coop] cryonis: our block %08x has no position after 20 s: waits (pooled, no block yet)\n", b->id);
                b->idle = true;
            }
            continue;
        }
        if (!b->placed_ns) {
            b->placed_ns = now;
            LOG("[coop] cryonis: our block %08x placed at %.2f %.2f %.2f after %llu ms\n", b->id, am[3], am[7], am[11],
                (unsigned long long)((now - b->found_ns) / 1000000ull));
        }
        float moved = fabsf(am[3] - b->m[3]) + fabsf(am[7] - b->m[7]) + fabsf(am[11] - b->m[11]);
        if (moved > 0.02f) memcpy(b->m, am, 48), b->stable_ns = now;
        if (now - b->stable_ns < 500000000ull && now - b->placed_ns < 3000000000ull) continue;
        memcpy(b->m, am, 48);
        if (!ice_matrix_ok(b->m)) {
            LOG("[coop] cryonis: our block %08x has an odd matrix: not sent\n", b->id);
            b->sent = true;
            continue;
        }
        ice_send(b, COOP_ICE_CREATE, here);
        b->sent = true;
    }
    if (together && now >= ice_look_ns) {  // new blocks of ours (cheap: a name filter, 5 times a second)
        ice_look_ns = now + 200000000ull;
        walk_actors(c, ice_name, ice_found, me);
    }
    if (ice_report) {
        ice_report = 0;
        uint64_t slot = LD64(A_ICE_MGR_SLOT), mgr = in_guest(slot) ? LD64(slot) : 0;
        LOG("[coop] cryonis: manager %llx (blocks %d), ours %d, copies %d, together %d\n", (unsigned long long)mgr,
            in_guest(mgr) ? (int)LD32(mgr + 0x38) : -1, ice_mine_n, ice_copy_n, together);
        for (int i = 0; i < ice_mine_n; i++) {
            LOG("[coop] cryonis:  ours %08x actor %llx at %.2f %.2f %.2f sent %d\n", ice_mine[i].id,
                (unsigned long long)ice_mine[i].actor, ice_mine[i].m[3], ice_mine[i].m[7], ice_mine[i].m[11],
                ice_mine[i].sent);
            if (!ice_alive(&ice_mine[i])) continue;
            uint64_t a = ice_mine[i].actor, rb = actor_rb(a), body = rb ? LD64(rb + 0x78) : 0;
            const float *m1 = (const float *)(uintptr_t)(a + ACTOR_MTX), *m2 = (const float *)(uintptr_t)(a + ACTOR_MTX2);
            const float *bp = body ? (const float *)(uintptr_t)(body + 0x1a0) : NULL;
            LOG("[coop] cryonis:   mtx %.2f %.2f %.2f mtx2 %.2f %.2f %.2f body %.2f %.2f %.2f (rb %llx)\n", m1[3], m1[7],
                m1[11], m2[3], m2[7], m2[11], bp ? bp[0] : 0.f, bp ? bp[1] : 0.f, bp ? bp[2] : 0.f, (unsigned long long)rb);
        }
        for (int i = 0; i < ice_copy_n; i++)
            LOG("[coop] cryonis:  copy %08x actor %llx at %.2f %.2f %.2f\n", ice_copy[i].id,
                (unsigned long long)ice_copy[i].actor, ice_copy[i].m[3], ice_copy[i].m[7], ice_copy[i].m[11]);
        uint64_t sm = stasis_mgr();
        if (sm) {
            float left;
            memcpy(&left, (const void *)(uintptr_t)(sm + STASIS_LEFT), 4);
            uint64_t t = stasis_target(c);
            LOG("[coop] stasis: manager %llx flags %02x %02x %02x left %.2f (+0x44 %08x +0x58 %08x) target %llx %s\n",
                (unsigned long long)sm, LD8(sm + 0x40), LD8(sm + 0x41), LD8(sm + 0x42), left, LD32(sm + 0x44),
                LD32(sm + 0x58), (unsigned long long)t, t ? (const char *)(uintptr_t)(t + 0x1c) : "-");
        } else {
            LOG("[coop] stasis: no manager\n");
        }
    }
}
// EV 17 from the other game (gate.inc world_event, game thread).
static void ice_ev(Cpu *c, const uint8_t *data, int len) {
    if (!cryonis_on() || len != (int)sizeof(CoopIce)) return;
    CoopIce e;
    memcpy(&e, data, sizeof e);
    if (!coop_ice_ok(&e)) { LOG("[coop] cryonis: bad event dropped\n"); return; }
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (!local_in_world() || e.area != here || here != ice_area) {
        LOG("[coop] cryonis: %08x %s for another area / while loading: ignored\n", e.id,
            e.kind == COOP_ICE_CREATE ? "block" : "gone");
        return;
    }
    if (e.kind == COOP_ICE_GONE) {
        struct Ice *b = ice_find(ice_copy, ice_copy_n, e.id);
        const char *what = "copy";
        if (!b) b = ice_find(ice_mine, ice_mine_n, e.id), what = "our block";
        if (!b) { LOG("[coop] cryonis: gone %08x: not here\n", e.id); return; }
        if (b->killing) return;
        bool ok = ice_break(c, b);
        LOG("[coop] cryonis: gone %08x from the other player: %s %llx %s\n", e.id, what, (unsigned long long)b->actor,
            ok ? "breaking" : "no manager / id (deleted in 3 s)");
        return;
    }
    if (ice_find(ice_copy, ice_copy_n, e.id)) return;  // (sent again after a pause: we still have it)
    if (ice_copy_n == ICE_MAX) { LOG("[coop] cryonis: block %08x: too many copies, ignored\n", e.id); return; }
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    float at[3] = {e.m[3], e.m[7], e.m[11]};
    if (fabsf(at[0]) + fabsf(at[1]) + fabsf(at[2]) < 0.01f) {  // (an older build sent unplaced blocks as 0,0,0)
        LOG("[coop] cryonis: block %08x at 0,0,0 (not placed yet there): ignored\n", e.id);
        return;
    }
    if (dist3(me, at) > 150.0f) { LOG("[coop] cryonis: block %08x: %.0f m away, ignored\n", e.id, dist3(me, at)); return; }
    ice_copy_make(c, e.id, e.m, 0);
}
