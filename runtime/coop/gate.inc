// Part of runtime/coop.c: tower proximity gate, shrine entrance and shared departure.
// SAVAGEWIND_COOP_GATE=0 turns off the gate features.
#define GATE_RADIUS 20.0f
#define GATE_MAX 8
#define EV_GATE_RIDE 3
#define EV_GATE_LEAVE 4  // (leave_watch: we left a shrine the other player is in)
typedef struct EvRide { float pos[3]; char event[32]; } EvRide;
static struct Gate {
    uint64_t actor, unit;
    uint32_t unit_id, hash;
    float pos[3];
    bool frozen, tower, activated;
} gates[GATE_MAX];
static int gate_n;
static CoopShrineRide shrine_boarded;
static struct { bool on; uint64_t until_ns; CoopShrineRide r; } shrine_ride_wait;  // (a partner's ride that came during our scene)
static uint64_t shrine_follow_ns;
static bool shrine_riding;
static bool gate_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_GATE") && !strcmp(getenv("SAVAGEWIND_COOP_GATE"), "0"));
    return on;
}
static bool shrine_door_on(void) {
    static int on = -1;
    return gate_on() && env_on("SAVAGEWIND_COOP_SHRINEDOOR", &on);
}
static bool gate_actor_name(const char *n) { return !strncmp(n, "DgnObj_EntranceElevator", 23); }
// (2026-11-09) Sheikah Tower pedestals too: FldObj_DownloadTerminal_A_01 (the plateau tower's: one in the pit at
// -560 177.6 1695 that starts the tower, one on top). Locked while the other player is out of range.
static bool gate_tower_name(const char *n) { return !strcmp(n, "FldObj_DownloadTerminal_A_01"); }
// (2026-11-12) A Divine Beast's terminals (their bool is the terminal's progress; named_seen keeps them only in a beast)
static bool beast_terminal_name(const char *n) { return !strcmp(n, "FldObj_Terminal_A_01"); }
// (2026-11-20) Korok puzzles and seeds. Every hidden Korok (Npc_HiddenKorokGround / Npc_HiddenKorokFly, 900 in the
// map) has a saved bool "MainField_<actor>_<HashId>" (map data); its puzzle's
// CopyWaitRevival tag reads it, so with the flag on the puzzle stays solved and the Korok does not come back. The
// Korok actor exists only once its puzzle is solved (a Create link), so we learn its flag name when it appears near
// us; when the flag turns on (its seed given) it goes out like a picked item: the normal share event + the seed
// (Obj_KorokNuts) that reached the pouch. A partner within the share radius gets the flag (their puzzle counts as
// solved; seen as solved from their next load of that place) and the same seed, once (not if their flag is already
// on). A far partner keeps their puzzle. No protocol change (EV_WORLD/EV_ITEM as for items).
// Default ON since 2026-11-20 (two-instance test with real input: the guest lifted the plateau rock -822.6 171.1
// 1546.9, talked to the Korok; the host 6 m away got the flag and the seed). SAVAGEWIND_COOP_KOROK=0 turns it off.
static bool korok_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_KOROK") && getenv("SAVAGEWIND_COOP_KOROK")[0] == '0');
    return on;
}
static bool korok_actor_name(const char *n) { return !strncmp(n, "Npc_HiddenKorok", 15) && korok_on(); }
static bool world_actor_name(const char *n) {
    return share_actor_name(n) || gate_actor_name(n) || gate_tower_name(n) || beast_terminal_name(n) ||
           korok_actor_name(n);
}
struct NamedCtx { const char *prefix; uint64_t now; float me[3]; };
static void named_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    struct NamedCtx *x = ctx;
    float p[3];
    if (actor_dist(a, x->me, p) > 60.0f) return;
    if (gate_actor_name(name) || gate_tower_name(name)) {
        int k = 0;
        while (k < gate_n && gates[k].actor != a) k++;
        if (k == gate_n && gate_n < GATE_MAX) {
            bool tower = gate_tower_name(name), act = tower && (LD32(a + 0x71c) & 1) != 0;
            gates[gate_n++] = (struct Gate){.actor = a, .unit = LD64(a + PROC_UNIT), .unit_id = LD32(a + PROC_ID),
                .hash = tower ? 0 : actor_hash(c, a), .pos = {p[0], p[1], p[2]}, .tower = tower, .activated = act};
            LOG("[coop] gate: %s %s at %.1f %.1f %.1f\n", tower ? "tower pedestal" : "shrine elevator", name, p[0], p[1],
                p[2]);
        }
        return;
    }
    if (beast_terminal_name(name) && (strcmp(x->prefix, "MainFieldDungeon") || !beast_on())) return;
    if (castle_flag_private(name)) return;  // (2026-11-14: each player's own Light Bow)
    uint32_t hid = actor_hash(c, a);
    if (!hid) return;
    char flag[96];
    snprintf(flag, sizeof flag, "%s_%s_%u", x->prefix, name, hid);
    named_add(flag, hid, x->now);
}

static int pouch_read(struct PouchCount *out) {
    uint64_t mgr = LD64(A_POUCH_MGR);
    if (!in_guest(mgr)) return -1;
    uint64_t head = mgr + 0x68, node = LD64(head + 8);
    int n = 0;
    for (int step = 0; step < 1000 && in_guest(node) && node != head; step++, node = LD64(node + 8)) {
        uint64_t item = node - 8, np = LD64(item + 0x30);
        if (!in_guest(np)) continue;
        int32_t type = (int32_t)LD32(item + 0x18), value = (int32_t)LD32(item + 0x20);
        // Weapons, bows, shields, armor: one entry each (value is durability). Arrows, materials, food, key items:
        // the value is the count.
        int32_t count = (type == 2 || type >= 7) ? value : 1;
        char name[64];
        snprintf(name, sizeof name, "%.63s", (const char *)(uintptr_t)np);
        int k = 0;
        while (k < n && strcmp(out[k].name, name)) k++;
        if (k == n) {
            if (n == POUCH_MAX) continue;
            snprintf(out[n].name, sizeof out[n].name, "%s", name), out[n].count = 0, n++;
        }
        out[k].count += count;
    }
    return n;
}

static int32_t pouch_count_of(const char *name) {  // (as pouch_read counts it; -1: no pouch)
    static struct PouchCount all[POUCH_MAX];
    int n = pouch_read(all);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i].name, name)) return all[i].count;
    return 0;
}

static uint64_t world_safestring(const char *text) {
    if (!world_mem) world_mem = (uint64_t)(uintptr_t)hle_alloc(0x100);
    if (!world_mem || strlen(text) >= 0xe0) return 0;
    ST64(world_mem, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
    ST64(world_mem + 8, world_mem + 0x10);
    memcpy((void *)(uintptr_t)(world_mem + 0x10), text, strlen(text) + 1);
    return world_mem;
}

// GameDataMgr::setBool internal helper (0x7000bc162c) with bypass_perm = 1 on mgr + 0xbb0, so flags with
// IsProgramWritable = False (MapTower_01..15 and all quest step flags) are written too without setting IsChangedByDebug.
#define A_GDT_SET_BOOL_EX 0x7000bc162cull
static bool gdt_get_s32(const char *name, int32_t *out);  // (coop/beast.inc)
static bool gdt_set_s32(Cpu *c, const char *name, int32_t val);
static bool gdt_set_bool(Cpu *c, const char *name, bool val) {
    uint64_t mgr = LD64(A_GDT_MGR), s = world_safestring(name);
    if (!in_guest(mgr) || !s) return false;
    Cpu saved = *c;
    c->x[0] = mgr + 0xbb0;
    c->x[1] = val ? 1 : 0;
    c->x[2] = s;
    c->x[3] = 1;
    c->x[4] = 1;
    guest_call(c, A_GDT_SET_BOOL_EX);
    bool ok = (c->x[0] & 1) != 0;
    *c = saved;
    return ok;
}

static void acq_send(void) {
    if (!acq.open) return;
    acq.open = false;
    bool ok = coop_net_send_event(EV_WORLD, &acq.w, sizeof acq.w);
    for (int i = 0; i < acq.n && ok; i++) ok = coop_net_send_event(EV_ITEM, &acq.items[i], sizeof acq.items[i]);
    char list[256] = "";
    for (int i = 0; i < acq.n; i++)
        snprintf(list + strlen(list), sizeof list - strlen(list), " %s x%d", acq.items[i].name, acq.items[i].count);
    LOG("[coop] share: %s at %.1f %.1f %.1f, items:%s%s\n", acq.w.flag, acq.w.pos[0], acq.w.pos[1], acq.w.pos[2],
        acq.n ? list : " none", ok ? "" : " (NOT sent: no peer or queue full)");
}

static void world_reset(void) {
    flag_n = 0, pouch_prev_n = -1, acq.open = false;
}

static void gate_follow_ev(Cpu *c, const uint8_t *data, int len);
static void shrine_ride_ev(Cpu *c, const uint8_t *data, int len);
static void shrine_door_ev(Cpu *c, const uint8_t *data, int len);
static void leave_ev(const uint8_t *data, int len);
static void switch_ev(const uint8_t *data, int len);
static void switch_hit_ev(const uint8_t *data, int len);
#define EV_SWITCH 5      // (switches_frame: a floor switch is pressed / released in the sender's game)
#define EV_SWITCH_HIT 6  // (sw_hit_frame: a hit switch lit up by a hit in the sender's game)
static void story_ev(const uint8_t *data, int len);  // (EV_STORY 7: a cutscene started in the sender's game)
static void story_ack_ev(const uint8_t *data, int len);  // (EV_STORY_ACK 13: the answer to it)
static void objects_ev(Cpu *c, const uint8_t *data, int len);  // (EV_OBJ 8: objects.inc)
static void pvp_areafx_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_AREAFX 9: pvp.inc)
static void drops_ev(Cpu *c, uint16_t type, const uint8_t *data, int len);  // (COOP_EV_DROP 10 / _GONE 11: drops.inc)
static bool drops_owns_gain(const char *name, uint64_t now);  // (drops.inc: a shared drop we picked up)
static void beast_rot_ev(const uint8_t *data, int len);  // (COOP_EV_BEAST_ROT 12: beast.inc)
static void gyro_ev(const uint8_t *data, int len);  // (COOP_EV_GYRO 15: gyro.inc)
static void ice_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_ICE 17: runes.inc)
static void tree_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_TREE 18: trees.inc)
static void tree_hit_ev(const uint8_t *data, int len);
static void trunk_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_TRUNK 20: trees.inc)
static void critter_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_CRITTER 19: critters.inc)
static void fire_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_FIRE 27: fire.inc)
static void grassfire_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_GRASSFIRE 28: grassfire.inc)
static void lightning_ev(Cpu *c, const uint8_t *data, int len);  // weather.inc
static void waterpuz_ev(const uint8_t *data, int len);  // (COOP_EV_WATERPUZ 29: water.inc)
static void beam_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_BEAM 33: beams.inc)
static void mapmark_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_MAPMARK 34: mapmark.inc)
static void arrow_fly_ev(const uint8_t *data, int len);  // (COOP_EV_ARROW_FLY 22: arrows.inc)
static void chest_shut_ev(Cpu *c, const uint8_t *data, int len);  // (COOP_EV_CHEST_SHUT 23: below)
static void gate_frame(Cpu *c, uint64_t now);
static void gate_release_all(void);
static void gate_pending_frame(Cpu *c, uint64_t now);
struct HashHit { uint32_t hid; uint64_t a; };
static void give_shared_item(Cpu *c, const char *name, int count);
static bool event_running(void);
static bool event_start_args(Cpu *c, uint64_t actor, const char *flow, const char *entry, const char *sname,
                             const char *sval, const char *bname, bool bval);
// (2026-11-15: show this screen to the other player too, so they see what was in the chest) Items from the
// other player's chest are shown with the game's own get-item window: resident flow GetDemo, entry
// ShowGetDemoDialogByName, argument CheckTargetActorName = the item (RE: only the window,
// the pouch is not touched). Shown one at a time while our Link is calm and no event runs; dropped after 60 s.
// The flow route (SAVAGEWIND_COOP_GETDEMO=flow) started and ended within a second with no window seen (2026-11-15
// probe). Direct route (=dialog, 2026-11-16): the window's own open call, 0x70013e942c(getDemoData = **0x7001d67960,
// SafeString *name) (RE: OpenGetDemoDialog::calc 0x7100709ba0 calls exactly this; window open? = 0x70013e8bcc, reads
// the screen manager *0x7001e374b8). Both kept as dev modes only; the DEFAULT is the pick-up panel (see below).
// SAVAGEWIND_COOP_GETDEMO=0: off. Dev: `coop getdemo <item> [panel|flow|dialog]`.
#define A_GETDEMO_DATA_SLOT 0x7001d67960ull
#define A_GETDEMO_OPEN 0x70013e942cull
#define A_GETDEMO_IS_OPEN 0x70013e8bccull
#define A_SCREEN_MGR 0x7001e374b8ull
static bool peer_world_chest;  // (the last EV_WORLD taken was a chest: its EV_ITEMs get the window)
static struct { char name[64]; uint64_t queued_ns; int mode; } getdemo_q[8];  // (mode 0 pick-up panel, 1 flow, 2 dialog)
static int getdemo_n;
static bool getdemo_mine;
static uint64_t getdemo_watch_until_ns;  // (diagnostic: window / pause / event changes logged for a few seconds)
// (2026-11-16, a probe) Both routes to the big window fail the same way: the window's screen does not exist
// until the open call creates it, goes to its loading state (0xfe = 3) and is shut and freed within 2 frames (also when
// the open call is repeated every frame for 1.5 s). Cause not found (polish2, 2026-11-16, ~1.5 h: the window's show
// step 0x7000e2a30c shuts it (vt+0x30, -4) when the item name in getDemoData (**0x7001d67960 + 0x40, text at +0x48)
// is empty - its only early exit. Live (a probe GETDEMO=1): name set, window OPEN; one frame (17 ms) later
// name '' and window closed together. Putting the name back from the frame hook comes too late (the window is gone);
// the code that empties it was not found among the 48 readers of 0x7001d67960 - next step would be a write watch on
// gd+0x48's text). What works: the small pick-up panel the game shows
// when Link picks something up ("Pa_PickUpWin", icon + item name, left side, no pause): 0x7000e7aee0(SafeString *name)
// (the HUD's resident screen #40, type-checked by the function itself; also used by an event action 0x710070c63c).
#define A_PICKUP_WIN 0x7000e7aee0ull
static int getdemo_mode(void) {  // -1 off, 0 pick-up panel (default), 1 GetDemo flow, 2 the big window directly
    const char *e = getenv("SAVAGEWIND_COOP_GETDEMO");
    if (e && !strcmp(e, "0")) return -1;
    if (e && !strcmp(e, "flow")) return 1;
    return e && !strcmp(e, "dialog") ? 2 : 0;
}
static uint64_t pickup_win_mem, getdemo_dlg_mem;
static bool pickup_win_show(Cpu *c, const char *name) {
    if (!pickup_win_mem) pickup_win_mem = (uint64_t)(uintptr_t)hle_alloc(0x100);
    uint64_t mem = pickup_win_mem;
    if (!mem || !in_guest(LD64(A_SCREEN_MGR)) || strlen(name) >= 0x60) return false;
    ST64(mem, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
    ST64(mem + 8, mem + 0x20);
    memcpy((void *)(uintptr_t)(mem + 0x20), name, strlen(name) + 1);
    Cpu saved = *c;
    c->x[0] = mem;
    guest_call(c, A_PICKUP_WIN);
    bool ok = c->x[0] & 1;
    *c = saved;
    return ok;
}
static void getdemo_push_mode(const char *name, int mode) {
    if (getdemo_n >= 8) return;
    snprintf(getdemo_q[getdemo_n].name, sizeof getdemo_q[0].name, "%s", name);
    getdemo_q[getdemo_n].mode = mode;
    getdemo_q[getdemo_n++].queued_ns = plat_now_ns();
}
static void getdemo_push(const char *name) { getdemo_push_mode(name, getdemo_mode() < 0 ? 0 : getdemo_mode()); }
static bool getdemo_is_open(Cpu *c) {
    if (!in_guest(LD64(A_SCREEN_MGR))) return false;
    Cpu saved = *c;
    guest_call(c, A_GETDEMO_IS_OPEN);
    bool open = c->x[0] & 1;
    *c = saved;
    return open;
}
static bool getdemo_open_direct(Cpu *c, const char *name) {
    if (!getdemo_dlg_mem) getdemo_dlg_mem = (uint64_t)(uintptr_t)hle_alloc(0x100);
    uint64_t mem = getdemo_dlg_mem;
    uint64_t slot = LD64(A_GETDEMO_DATA_SLOT), gd = in_guest(slot) ? LD64(slot) : 0;
    if (!mem || !in_guest(gd) || !in_guest(LD64(A_SCREEN_MGR)) || strlen(name) >= 0x60) return false;
    ST64(mem, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
    ST64(mem + 8, mem + 0x20);
    memcpy((void *)(uintptr_t)(mem + 0x20), name, strlen(name) + 1);
    Cpu saved = *c;
    c->x[0] = gd; c->x[1] = mem;
    guest_call(c, A_GETDEMO_OPEN);
    *c = saved;
    return true;
}
static void getdemo_pop(void) {
    memmove(&getdemo_q[0], &getdemo_q[1], sizeof getdemo_q[0] * (size_t)(getdemo_n - 1));
    getdemo_n--;
}
static void getdemo_frame(Cpu *c) {
    static bool was_running;
    bool running = event_running();
    if (plat_now_ns() < getdemo_watch_until_ns) {  // (diagnostic after a start: what opens / closes when)
        static int w_open = -1, w_pause = -1, w_ev = -1;
        int o = getdemo_is_open(c), p = local_paused(), e = running;
        uint64_t sm = LD64(A_SCREEN_MGR), scr = 0;
        if (in_guest(sm)) scr = LD64(LD64(sm + 0x30) + (LD32(sm + 0x28) < 12 ? 0 : 0x58));
        static int w_fd = -1000, w_fe = -1000;
        int fd = in_guest(scr) ? (int)(int8_t)LD8(scr + 0xfd) : -99, fe = in_guest(scr) ? LD8(scr + 0xfe) : -99;
        // (polish2 RE: the window's show step 0x7000e2a30c closes it (vt+0x30, -4) when the item name in getDemoData
        // (**0x7001d67960 + 0x40, a SafeString: text at +0x48) is empty; log that name too)
        uint64_t gslot = LD64(A_GETDEMO_DATA_SLOT), gd = in_guest(gslot) ? LD64(gslot) : 0;
        uint64_t gname = in_guest(gd) ? LD64(gd + 0x48) : 0;
        char nm[40] = "?";
        if (in_guest(gname)) snprintf(nm, sizeof nm, "%.38s", (const char *)(uintptr_t)gname);
        static char w_nm[40];
        if (o != w_open || p != w_pause || e != w_ev || fd != w_fd || fe != w_fe || strcmp(nm, w_nm))
            LOG("[coop] get-item window watch: window %s, game %s, event %s, screen %llx fd/fe %d/%d name '%s' t=%llu ms\n",
                o ? "OPEN" : "closed", p ? "paused" : "running", e ? "running" : "none", (unsigned long long)scr,
                fd, fe, nm, (unsigned long long)((plat_now_ns() / 1000000ull) % 100000ull));
        snprintf(w_nm, sizeof w_nm, "%s", nm);
        w_fd = fd, w_fe = fe;
        w_open = o, w_pause = p, w_ev = e;
    }
    if (getdemo_mine && !running && was_running) {  // (our window closed: the flow's prohibitions back off, in case
        getdemo_mine = false;                       // it was cut short)
        gdt_set_bool(c, "CarryProhibition", false);
        gdt_set_bool(c, "TrashProhibition", false);
        gdt_set_bool(c, "PlayFlashbackDemoProhibition", false);
    }
    was_running = running;
    if (!getdemo_n || running || !local_in_world() || !player_info_ptr()) return;
    if (plat_now_ns() - getdemo_q[0].queued_ns > 60000000000ull) {
        LOG("[coop] get-item window for %s: dropped (no calm moment in 60 s)\n", getdemo_q[0].name);
        getdemo_pop();
        return;
    }
    static uint64_t next_ns;
    if (plat_now_ns() < next_ns) return;
    next_ns = plat_now_ns() + 500000000ull;
    int mode = getdemo_q[0].mode;
    if (mode == 0) {  // (the pick-up panel: no pause, no event: only not in a menu)
        if (local_paused()) return;
        bool shown = pickup_win_show(c, getdemo_q[0].name);
        LOG("[coop] item from the other player's chest: %s in the pick-up panel: %s\n", getdemo_q[0].name,
            shown ? "shown" : "panel not ready, retried");
        if (shown) getdemo_pop();
        return;
    }
    if (local_chest_open_ns && plat_now_ns() - local_chest_open_ns < 8000000000ull) return;  // (our own chest scene)
    if (local_anim_now != COOP_ANIM_WAIT && local_anim_now != COOP_ANIM_WALK && local_anim_now != COOP_ANIM_RUN) return;
    uint64_t pa = LD64(player_info_ptr() + 0x60);
    if (!in_guest(pa)) return;
    if (local_paused() || getdemo_is_open(c)) return;  // (a menu / a window of the game's own is up)
    bool flow = mode == 1;
    bool ok = flow ? event_start_args(c, pa, "GetDemo", "ShowGetDemoDialogByName", "CheckTargetActorName",
                                      getdemo_q[0].name, "IsInvalidOpenPouch", false)
                   : getdemo_open_direct(c, getdemo_q[0].name);
    LOG("[coop] get-item window for %s (from the other player's chest, %s): %s\n", getdemo_q[0].name,
        flow ? "flow" : "direct", ok ? "shown" : "refused by the game, retried");
    getdemo_watch_until_ns = plat_now_ns() + 8000000000ull;
    if (ok) getdemo_mine = flow, getdemo_pop();
}
static struct { int count; int32_t had; uint64_t until_ns; } orb_wait;  // (shared spirit orb held in a shrine)
static bool chest_early_was(uint64_t a);  // (chest_early_frame) a chest we locked when another player began opening it
// (2026-11-16) The freeze bit (ENEMY_SKIP_AI) also stops the chest's model animation: an AS played on a frozen chest
// never moves the lid, and freezing holds the lid wherever it is (probes, one game:
// "Open" on a free chest lifts the lid in a few seconds; frozen first: stays shut). So the copy plays "Open", then
// "OpenWait" (the open pose) CHEST_LID_NS later, and is frozen a moment after that. While it is free our Link could
// open it too (the double-open softlock CHESTEARLY prevents): when our Link is within CHEST_LID_NEAR of it, it is
// frozen at once, lid shut (as before).
#define CHEST_LID_NS 6000000000ull
#define CHEST_LID_NEAR 4.0f
#define CHEST_LID_NEAR_NS 400000000ull
static struct { uint64_t a, at_ns; int stage; } chest_lid[4];  // (stage 2: closing again, chest_shut_ev)
static bool chest_lid_pending(uint64_t a) {
    for (int i = 0; i < 4; i++)
        if (chest_lid[i].a == a && chest_lid[i].stage != 2) return true;
    return false;
}
static void chest_freeze(uint64_t a) { ST8(a + ENEMY_FLAGS, (uint8_t)(LD8(a + ENEMY_FLAGS) | ENEMY_SKIP_AI)); }
static void chest_lid_open(Cpu *c, uint64_t a) {  // play the lid, freeze later (or now: CHESTOPEN=0, we are close, list full)
    if (chest_lid_pending(a)) return;
    for (int i = 0; i < 4; i++)
        if (chest_lid[i].a == a) chest_lid[i].a = 0;  // (it was closing again: opened anew)
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    float dx = LDF32(a + ACTOR_MTX + 12) - me[0], dz = LDF32(a + ACTOR_MTX + 44) - me[2];
    bool near = dx * dx + dz * dz < CHEST_LID_NEAR * CHEST_LID_NEAR;
    if (look_on("SAVAGEWIND_COOP_CHESTOPEN") && !near) {
        for (int i = 0; i < 4; i++)
            if (!chest_lid[i].a) {
                play_anim(c, a, "Open", 0);
                chest_lid[i].a = a, chest_lid[i].at_ns = plat_now_ns() + CHEST_LID_NS, chest_lid[i].stage = 0;
                return;
            }
    }
    // (polish2, 2026-11-16: with our Link close the lid used to stay (nearly) shut - the usual case when playing
    // together. Now the open pose "OpenWait" is applied straight away and the chest frozen CHEST_LID_NEAR_NS later:
    // too short for our Link to open it meanwhile. SAVAGEWIND_COOP_CHESTLIDNEAR=0: locked at once, shut, as before.)
    static int near_on = -1;
    if (near_on < 0) near_on = look_on("SAVAGEWIND_COOP_CHESTLIDNEAR");
    if (near && near_on && look_on("SAVAGEWIND_COOP_CHESTOPEN")) {
        for (int i = 0; i < 4; i++)
            if (!chest_lid[i].a) {
                play_anim(c, a, "OpenWait", 0);
                chest_lid[i].a = a, chest_lid[i].at_ns = plat_now_ns() + CHEST_LID_NEAR_NS, chest_lid[i].stage = 1;
                LOG("[coop] chest %llx: our Link is close: open pose now, locked in a moment\n", (unsigned long long)a);
                return;
            }
    }
    if (near) LOG("[coop] chest %llx: our Link is close: locked at once, lid not played\n", (unsigned long long)a);
    chest_freeze(a);
}
static uint64_t chest_lid_find;
static void chest_lid_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c; (void)name; (void)ctx;
    if (a == chest_lid_find) chest_lid_find |= 1;  // (marks "still loaded")
}
static bool local_paused(void);
static void chest_lid_frame(Cpu *c) {
    // (2026-11-18, a two-game test: the watchers' games stood in a tutorial pop-up - paused - while the other player
    // opened the chest: the lid's AS does not move while paused, and the freeze then held it shut. Paused time does
    // not count: each step waits for that much running time. SAVAGEWIND_COOP_CHESTPAUSE=0: wall-clock as before.)
    static uint64_t last_ns;
    static int pause_on = -1;
    if (pause_on < 0) pause_on = look_on("SAVAGEWIND_COOP_CHESTPAUSE");
    uint64_t now_ns = plat_now_ns();
    if (pause_on && last_ns && local_paused())
        for (int i = 0; i < 4; i++)
            if (chest_lid[i].a) chest_lid[i].at_ns += now_ns - last_ns;
    last_ns = now_ns;
    for (int i = 0; i < 4; i++) {
        if (!chest_lid[i].a || plat_now_ns() < chest_lid[i].at_ns) continue;
        uint64_t a = chest_lid[i].a;
        if (!local_in_world()) { chest_lid[i].a = 0; continue; }
        chest_lid_find = a;
        walk_actors(c, share_actor_name, chest_lid_seen, NULL);
        if (chest_lid_find != (a | 1)) { chest_lid[i].a = 0; continue; }  // (unloaded meanwhile: nothing to do)
        if (chest_lid[i].stage == 2) {  // (closed again: hold the closed pose, not frozen)
            chest_lid[i].a = 0;
            play_anim(c, a, "CloseWait", 0);
            continue;
        }
        if (chest_lid[i].stage == 0) {  // (the lid is up: hold the open pose, freeze once it is applied)
            play_anim(c, a, "OpenWait", 0);
            chest_lid[i].stage = 1, chest_lid[i].at_ns = plat_now_ns() + 2000000000ull;
            continue;
        }
        chest_lid[i].a = 0;
        chest_freeze(a);
        char all[300] = "";
        as_list(c, a, all, sizeof all);
        LOG("[coop] chest %llx: lid played, frozen now (AS: %s)\n", (unsigned long long)a, all);
    }
}
static void find_hashid(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)name;
    struct HashHit *hit = ctx;
    if (!hit->a && actor_hash(c, a) == hit->hid) hit->a = a;
}

// A peer's event (game thread).
static void world_event(Cpu *c, uint16_t type, const uint8_t *data, int len) {
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    if (type == EV_WORLD && len == (int)sizeof(EvWorld)) {
        EvWorld w;
        memcpy(&w, data, sizeof w);
        w.flag[sizeof w.flag - 1] = 0;
        peer_world_chest = false;
        if (!strncmp(w.flag, "Open_Dungeon", 12) || !strncmp(w.flag, "Enter_Dungeon", 13)) {
            peer_world_taken = false;
            return;
        }
        // (the area the flag turned on in, sent with it: the other player may have left it since, e.g. a shrine's
        // clear goes out once they ride back up, seen 2026-11-08)
        char area[96];
        bool same_area = w.area == local_area(area, sizeof area);
        float dx = w.pos[0] - me[0], dy = w.pos[1] - me[1], dz = w.pos[2] - me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        float d_xz = sqrtf(dx * dx + dz * dz);
        // A shrine's clear (spirit orb) and its treasure: for anyone inside the same shrine, at any distance (both
        // arrived at the same trial; shrines are often wider than the radius).
        // For story/tower/quest flags (!w.hashid), horizontal distance counts so a partner at the base of a 70 m
        // Sheikah Tower still gets the tower/quest flags that fire after the cutscene moves Link to the top.
        bool whole_shrine = same_area && !strncmp(area, "CDungeon", 8) &&
                            (!strncmp(w.flag, "Clear_Dungeon", 13) || !strncmp(w.flag, "CompleteTreasure_Dungeon", 24) ||
                             (w.hashid && !strncmp(w.flag, "CDungeon_TBox_", 14)));  // (a shrine's chests: as its treasure)
        // (2026-11-12) Inside a Divine Beast: everything from a partner in the same beast (they are 200 m long; the
        // terminals and the main control unit are progress for both).
        bool whole_beast = same_area && beast_on() && !strncmp(area, "MainFieldDungeon/Remains", 24);
        // (2026-11-14) In the Sanctum / Ganon's arena or the Dark Beast's field: story flags from a partner there too.
        bool whole_castle = same_area && !w.hashid && castle_whole(w.pos, me);
        float dist_check = (!w.hashid && fabsf(dy) < 120.0f) ? d_xz : d;
        peer_world_taken = local_in_world() && same_area &&
                           (dist_check < share_radius() || whole_shrine || whole_beast || whole_castle);
        if (!peer_world_taken) {
            // (2026-11-17, play report: "the other players must not be able to open the chest once one player opened
            // it".) A chest opened by a player too far away for its items: still opened here (flag set, our copy
            // locked), its items not given. SAVAGEWIND_COOP_CHESTFAR=0: left closed here, as before.
            if (w.hashid && strstr(w.flag, "_TBox_") && same_area && local_in_world() &&
                look_on("SAVAGEWIND_COOP_CHESTFAR") && gdt_get_bool(w.flag) == 0) {
                gdt_set_bool(c, w.flag, true);
                applied[applied_next].crc = crc_of(w.flag), applied[applied_next].at_ns = plat_now_ns();
                applied_next = (applied_next + 1) % 64;
                struct HashHit hit = {w.hashid, 0};
                walk_actors(c, share_actor_name, find_hashid, &hit);
                if (hit.a && !chest_early_was(hit.a)) chest_lid_open(c, hit.a);
                LOG("[coop] share from the other player: %s - %.0f m away: chest opened here too%s, items not given\n",
                    w.flag, d, hit.a ? " (ours locked)" : "");
                return;
            }
            LOG("[coop] share from the other player: %s - not here (%.0f m away%s)\n", w.flag, d,
                same_area ? "" : ", another area");
            return;
        }
        // (2026-11-15, first two-PC session: both players opened the same chest within a second; each game gave its
        // own item, then took the other's too: the pants twice.) A chest or placed item whose flag is already on here
        // was already taken here (by us, or shared earlier): its items are not given again.
        // (2026-11-15: the same for a shrine's clear - our own closing scene already gave our spirit orb.)
        if ((w.hashid || !strncmp(w.flag, "Clear_Dungeon", 13)) && gdt_get_bool(w.flag) == 1) {
            peer_world_taken = false;
            LOG("[coop] share from the other player: %s - already taken here too (both took it): items not given twice\n",
                w.flag);
            return;
        }
        // (2026-11-15, play report: while the other player's chest scene played, ours could still be opened, and
        // when their share arrived during OUR opening scene, freezing the chest mid-scene softlocked us - camera
        // only.) If our Link is opening a chest right now (its opening AS within the last 10 s) and this chest is
        // within 4 m of it, it is ours being opened: leave the chest alone and take none of their items (our own
        // scene gives ours; the other game skips ours since its flag is on). SAVAGEWIND_COOP_CHESTBOTH=0: off.
        struct HashHit chest = {w.hashid, 0};
        if (w.hashid && strstr(w.flag, "_TBox_") && look_on("SAVAGEWIND_COOP_CHESTBOTH") && local_chest_open_ns &&
            plat_now_ns() - local_chest_open_ns < 10000000000ull &&
            (walk_actors(c, share_actor_name, find_hashid, &chest), chest.a)) {
            float cx = LDF32(chest.a + ACTOR_MTX + 12) - me[0], cz = LDF32(chest.a + ACTOR_MTX + 44) - me[2];
            if (cx * cx + cz * cz < 16.0f) {
                peer_world_taken = false;
                LOG("[coop] share from the other player: %s - we are opening it ourselves right now: chest left "
                "alone, their items not given\n", w.flag);
                return;
            }
        }
        peer_world_chest = w.hashid && strstr(w.flag, "_TBox_");
        gdt_set_bool(c, w.flag, true);
        applied[applied_next].crc = crc_of(w.flag), applied[applied_next].at_ns = plat_now_ns();
        applied_next = (applied_next + 1) % 64;
        // A placed item/weapon the other player picked up: gone here too (its actor; the flag keeps it gone).
        uint64_t gone = 0;
        bool locked = false;
        if (w.hashid && !strstr(w.flag, "_TBox_")) {
            struct HashHit hit = {w.hashid, 0};
            walk_actors(c, share_actor_name, find_hashid, &hit);
            if (hit.a && despawn_now(c, hit.a, ACTOR_DELETE_REASON)) gone = hit.a;
        } else if (w.hashid) {
            // Our copy of the chest: the game read its flag when it loaded it, so it still shows closed, and opening
            // it gave the item a second time (seen 2026-11-08). Freeze it (the actor freeze bit): it cannot be opened
            // now and shows opened from the next load on.
            struct HashHit hit = {w.hashid, 0};
            walk_actors(c, share_actor_name, find_hashid, &hit);
            if (hit.a) {
                // (2026-11-15, play report: "no animation of the chest being opened by the other player") the lid
                // opens here too: the chest's own AS "Open" (TBox_Field ASList: Close CloseWait FrontOpen Open
                // OpenWait Wait). NOT yet seen in game: whether the frozen chest still plays it.
                // SAVAGEWIND_COOP_CHESTOPEN=0: off.
                if (!chest_early_was(hit.a)) chest_lid_open(c, hit.a);
                else if (!chest_lid_pending(hit.a)) chest_freeze(hit.a);
                locked = true;
                char all[300] = "";
                as_list(c, hit.a, all, sizeof all);
                LOG("[coop] share: chest %llx AS: %s\n", (unsigned long long)hit.a, all);
            }
        }
        // (2026-11-21) A Korok found together: our "Koroks found" total (HiddenKorok_Number, the album / map count)
        // goes up too. SAVAGEWIND_COOP_KOROKCOUNT=0: off.
        static int kc_c = -1;
        int32_t kn = 0;
        if (strstr(w.flag, "_Npc_HiddenKorok") && env_on("SAVAGEWIND_COOP_KOROKCOUNT", &kc_c) &&
            gdt_get_s32("HiddenKorok_Number", &kn) && kn >= 0 && kn < 900) {
            bool ok = gdt_set_s32(c, "HiddenKorok_Number", kn + 1);  // (reads back the new value from the next frame)
            LOG("[coop] korok: Koroks found %d -> %d%s\n", kn, kn + 1, ok ? "" : " (set refused)");
        }
        LOG("[coop] share from the other player: %s (%.0f m away): flag set%s\n", w.flag, d,
            gone ? ", its actor removed here" : locked ? ", our chest locked" : "");
    } else if (type == COOP_EV_CHEST_SHUT) {  // (a chest closed again there, its item refused)
        chest_shut_ev(c, data, len);
    } else if (type == EV_GATE_RIDE) {
        gate_follow_ev(c, data, len);
    } else if (type == COOP_EV_SHRINE_RIDE) {
        shrine_ride_ev(c, data, len);
    } else if (type == COOP_EV_SHRINE_DOOR) {
        shrine_door_ev(c, data, len);
    } else if (type == EV_GATE_LEAVE) {
        leave_ev(data, len);
    } else if (type == EV_SWITCH) {
        switch_ev(data, len);
    } else if (type == EV_SWITCH_HIT) {
        switch_hit_ev(data, len);
    } else if (type == 7) {  // EV_STORY
        story_ev(data, len);
    } else if (type == 13) {  // EV_STORY_ACK (story.inc: the answer to our cutscene, after we stopped waiting)
        story_ack_ev(data, len);
    } else if (type == 8) {  // EV_OBJ (objects.inc: a carried object let go / broken in the sender's game)
        objects_ev(c, data, len);
    } else if (type == COOP_EV_AREAFX) {  // (pvp.inc: the other player's arrow hit us / their bomb blew up)
        pvp_areafx_ev(c, data, len);
    } else if (type == COOP_EV_DROP || type == COOP_EV_DROP_GONE) {  // (drops.inc: an item dropped / picked up there)
        drops_ev(c, type, data, len);
    } else if (type == COOP_EV_BEAST_ROT) {  // (beast.inc: a Divine Beast's moving part turned there)
        beast_rot_ev(data, len);
    } else if (type == COOP_EV_GYRO) {  // (gyro.inc: a motion-control platform tilted there)
        gyro_ev(data, len);
    } else if (type == COOP_EV_ICE) {  // (runes.inc: a Cryonis block made / broken there)
        ice_ev(c, data, len);
    } else if (type == COOP_EV_TREE) {  // (trees.inc: a tree cut down there)
        tree_ev(c, data, len);
    } else if (type == COOP_EV_TREE_HIT) {
        tree_hit_ev(data, len);
    } else if (type == COOP_EV_TRUNK) {  // (trees.inc: a felled trunk's pose there)
        trunk_ev(c, data, len);
    } else if (type == COOP_EV_CRITTER) {  // (critters.inc: a placed bird / fish / insect caught or killed there)
        critter_ev(c, data, len);
    } else if (type == COOP_EV_FIRE) {  // (fire.inc: a campfire / cooking pot lit or put out there)
        fire_ev(c, data, len);
    } else if (type == COOP_EV_GRASSFIRE) {  // (grassfire.inc: grass set on fire there)
        grassfire_ev(c, data, len);
    } else if (type == COOP_EV_LIGHTNING) {
        lightning_ev(c, data, len);
    } else if (type == COOP_EV_WATERPUZ) {  // (water.inc: shrine water wheels, water levels, timed switches)
        waterpuz_ev(data, len);
    } else if (type == COOP_EV_BEAM) {  // (beams.inc: a Guardian's laser / beam there)
        beam_ev(c, data, len);
    } else if (type == COOP_EV_MAPMARK) {  // (mapmark.inc: a map marker placed / removed there)
        mapmark_ev(c, data, len);
    } else if (type == COOP_EV_ARROW_FLY) {  // (arrows.inc, protocol 74: the other player's arrow in flight)
        arrow_fly_ev(data, len);
    } else if (type == EV_ITEM && len == (int)sizeof(EvItem)) {
        EvItem it;
        memcpy(&it, data, sizeof it);
        it.name[sizeof it.name - 1] = 0;
        if (!peer_world_taken || it.count <= 0 || it.count > 999) return;
        // (2026-11-15, play: "I have 2 spirit orbs, she has 1, she did the shrine's closing scene".) The orb came from
        // her clear, then the same closing scene, shared, ran here too and its own orb came on top. A shared orb that
        // arrives while we are in a shrine waits until we leave it (or 3 min): if our own scene gave us one meanwhile,
        // the shared one is not given. SAVAGEWIND_COOP_ORBWAIT=0: off.
        char area[96];
        local_area(area, sizeof area);
        if (!strcmp(it.name, "Obj_DungeonClearSeal") && !strncmp(area, "CDungeon", 8) && look_on("SAVAGEWIND_COOP_ORBWAIT")) {
            orb_wait.count += it.count, orb_wait.had = pouch_count_of(it.name), orb_wait.until_ns = plat_now_ns() + 180000000000ull;
            LOG("[coop] share from the other player: spirit orb x%d waits until we leave the shrine (we have %d)\n",
                it.count, orb_wait.had);
            return;
        }
        int32_t had = pouch_count_of(it.name);
        give_shared_item(c, it.name, it.count);
        if (peer_world_chest && pouch_count_of(it.name) != had && getdemo_mode() >= 0) getdemo_push(it.name);
    }
}

// (58) An event from player `origin` (with more than two players, events of every world player arrive; the host
// relays them with the sender's id). The handlers read "the other player" (coop_net_get_remote...) and the last
// share they took (peer_world_taken / _chest) as that player's, so the distance rules apply to the player that sent
// it. With two players origin is always the partner: as before.
static int world_ev_origin = -1;  // (59) while an event is handled: the player it comes from (objects.inc)
static void wildlife_event(Cpu *c, uint16_t type, const uint8_t *data, int len, int origin);
static void fauna_event(Cpu *c, const uint8_t *data, int len, int origin);  // (coop/fauna.inc)
static void dish_event(uint16_t type, const uint8_t *data, int len, int origin);
static void world_event_from(Cpu *c, uint16_t type, const uint8_t *data, int len, int origin) {
    if (type == COOP_EV_DISH || type == COOP_EV_DISH_RESULT) {
        dish_event(type, data, len, origin);
        return;
    }
    if (type == COOP_EV_FAUNA_GONE) {
        fauna_event(c, data, len, origin);
        return;
    }
    if (type == COOP_EV_WILD_POLL || type == COOP_EV_WILD_ROSTER) {
        wildlife_event(c, type, data, len, origin);
        return;
    }
    static bool taken[8], chest[8];
    int o = origin & 7;
    bool other = origin >= 0 && origin != coop_net_partner_id();
    peer_world_taken = taken[o], peer_world_chest = chest[o];
    if (other) coop_net_view(origin);
    world_ev_origin = origin;
    world_event(c, type, data, len);
    world_ev_origin = -1;
    if (other) coop_net_view(-1);
    taken[o] = peer_world_taken, chest[o] = peer_world_chest;
}

// An item shared by the other player into our pouch (the game's pouch add), kept out of our own pouch diff.
static void give_shared_item(Cpu *c, const char *name, int count) {
    {
        struct { char name[64]; int32_t count; } it;
        snprintf(it.name, sizeof it.name, "%s", name);
        it.count = count;
        uint64_t s = world_safestring(it.name);
        if (!s) return;
        int32_t had = pouch_count_of(it.name);
        Cpu saved = *c;
        c->x[0] = s; c->x[1] = (uint64_t)it.count;
        guest_call(c, A_POUCH_ADD);
        *c = saved;
        int32_t now_has = pouch_count_of(it.name);
        // Our own pouch diff must not count it (2026-11-11, a two-game test): when an acquisition of ours is open at the
        // same time (both games turned FirstTower_Activated on in the chest's scene), it took the received armor as
        // ours and sent it back, and the other game's chest acquisition sent that one back again: Armor_001_Lower x2
        // in both games, and saved so. The item goes into the "before" snapshot (also the one held over a scene).
        int32_t gained = now_has > had ? now_has - had : it.count;
        if (pouch_prev_n >= 0) {
            int k = 0;
            while (k < pouch_prev_n && strcmp(pouch_prev[k].name, it.name)) k++;
            if (k == pouch_prev_n && pouch_prev_n < POUCH_MAX) {
                snprintf(pouch_prev[k].name, sizeof pouch_prev[k].name, "%s", it.name);
                pouch_prev[k].count = 0;
                pouch_prev_n++;
            }
            if (k < pouch_prev_n) pouch_prev[k].count += gained;
        }
        LOG("[coop] share from the other player: got %s x%d (pouch %d -> %d)\n", it.name, it.count, had, now_has);
        // (2026-11-21, 11-20 evening play: the host opened a chest, the partner's weapon pouch was full, she got
        // nothing.) A weapon / shield / bow that did not fit is put on the ground at our Link instead, as the game
        // does with a chest item that does not fit. SAVAGEWIND_COOP_FULLDROP=0: off.
        static int fulldrop_c = -1;
        if (now_has <= had && !strncmp(it.name, "Weapon_", 7) && env_on("SAVAGEWIND_COOP_FULLDROP", &fulldrop_c)) {
            float p[3];
            if (player_pos(p)) {
                p[1] += 1.0f;
                uint64_t a = spawn_now(c, it.name, p);
                LOG("[coop] share from the other player: %s did not fit in the pouch: put on the ground here (%s)\n",
                    it.name, a ? "made" : "refused");
            }
        }
    }
}

// (2026-11-15, play report: while the other player's chest-opening scene played, the same chest could be opened here
// too, and that softlocked.) Their share (EV_WORLD) only comes when their scene ends and the chest's flag turns on. So
// as soon as the other player plays the chest-opening AS, the chest nearest their avatar (within 2.5 m) is locked
// here (the actor freeze bit, as for a shared chest) and its lid opens. Not when our Link is opening that one itself.
// No protocol change. SAVAGEWIND_COOP_CHESTEARLY=0: off.
static bool chest_name(const char *n) { return !strncmp(n, "TBox_", 5); }
struct ChestNear { float p[3], best; uint64_t a; };
static void chest_near_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c; (void)name;
    struct ChestNear *k = ctx;
    float dx = LDF32(a + ACTOR_MTX + 12) - k->p[0], dy = LDF32(a + ACTOR_MTX + 28) - k->p[1],
          dz = LDF32(a + ACTOR_MTX + 44) - k->p[2];
    float d = dx * dx + dy * dy + dz * dz;
    if (d < k->best) k->best = d, k->a = a;
}
// (2026-11-17, 3-player play: not one "[coop] chest" line in two players' logs although partners opened chests.)
// Causes found in the logs: (1) only the world partner was watched (coop_net_get_remote): with 3+ players a chest
// opened by any other player was never locked; (2) only the Unlock AS counted, but big iron chests are only kicked
// (GetTreasurebox_Kick, no Unlock before it). Now every player whose world is shared with ours is watched, each with
// its own action counter, and Unlock or Kick starts the lock. SAVAGEWIND_COOP_CHESTALL=0: the partner only, Unlock
// only (as before).
static uint64_t chest_early_done[8];  // chests locked by chest_early_frame (any player), newest last
static int chest_early_next;
static bool chest_early_was(uint64_t a) {
    for (int i = 0; i < 8; i++)
        if (a && chest_early_done[i] == a) return true;
    return false;
}
static void chest_early_one(Cpu *c, int pid, uint8_t *seq_done, bool all) {
    CoopState rs;
    double age = 99;
    if (pid >= 0) coop_net_view(pid);
    bool ok = coop_net_get_remote(&rs, &age);
    if (pid >= 0) coop_net_view(-1);
    bool opening = rs.anim == COOP_ANIM_CHEST_OPEN || (all && rs.anim == COOP_ANIM_CHEST_KICK);
    if (!ok || age > 1.0 || !opening || rs.anim_seq == *seq_done) return;
    char area[96];
    if (rs.area != local_area(area, sizeof area) || !isfinite(rs.pos[0]) || !isfinite(rs.pos[1]) || !isfinite(rs.pos[2]))
        return;
    *seq_done = rs.anim_seq;
    struct ChestNear k = {{rs.pos[0], rs.pos[1], rs.pos[2]}, 2.5f * 2.5f, 0};
    walk_actors(c, chest_name, chest_near_seen, &k);
    if (!k.a) {
        LOG("[coop] chest: player %d plays a chest-opening AS but no chest is within 2.5 m of them here\n", pid);
        return;
    }
    if (chest_early_was(k.a) || chest_lid_pending(k.a)) return;  // (already locked: e.g. Unlock, then Kick)
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    float mx = LDF32(k.a + ACTOR_MTX + 12) - me[0], mz = LDF32(k.a + ACTOR_MTX + 44) - me[2];
    if (local_chest_open_ns && plat_now_ns() - local_chest_open_ns < 10000000000ull && mx * mx + mz * mz < 16.0f) {
        LOG("[coop] chest: player %d opens the chest we are opening too: left alone\n", pid);
        return;
    }
    chest_lid_open(c, k.a);
    chest_early_done[chest_early_next] = k.a, chest_early_next = (chest_early_next + 1) % 8;
    LOG("[coop] chest: player %d opens a chest (%llx, %.1f m from them, %s): locked here, lid opening\n", pid,
        (unsigned long long)k.a, sqrtf(k.best), rs.anim == COOP_ANIM_CHEST_KICK ? "kick" : "unlock");
}
static void chest_early_frame(Cpu *c) {
    static int on = -1, all = -1;
    static uint8_t seq_done[9];
    if (on < 0) on = look_on("SAVAGEWIND_COOP_CHESTEARLY"), all = look_on("SAVAGEWIND_COOP_CHESTALL");
    if (!local_in_world()) memset(chest_early_done, 0, sizeof chest_early_done);  // (addresses get reused after a load)
    if (!on || !local_in_world()) return;
    if (!all) {
        chest_early_one(c, -1, &seq_done[8], false);
        return;
    }
    int part = coop_net_partner_id();
    uint32_t w = coop_net_world_players(1.0) | (part >= 0 ? 1u << part : 0u);
    if (part < 0 && !w) {  // (no ids yet: plain two-player view)
        chest_early_one(c, -1, &seq_done[8], true);
        return;
    }
    for (int pid = 0; pid < 8; pid++)
        if (w >> pid & 1) chest_early_one(c, pid, &seq_done[pid], true);
}

// ---- our own chest (2026-11-19, play report, shrine chest Dungeon007, host log play/savagewind.log ~13800-14047)
// (a) The partner's "got item" panel came only when the opener closed the item pop-up: the share went out when the
// chest's flag turned on, at the END of the opening scene. The item reaches our pouch when the pop-up appears
// (Demo_SetItemDataToPouch, then the window). Now, while our Link opens a chest, the first item
// that reaches the pouch sends the chest's flag and its items at once (acq.early); the flag turning on later sends
// nothing more. SAVAGEWIND_COOP_CHESTITEMEARLY=0: at the flag, as before.
// (b) A full pouch: the game shows the item, puts it back and closes the chest again; no item, no flag (log: the
// first try sent nothing). But the partners' copies had been locked open by CHESTEARLY when our opening AS began, and
// stayed open. Now when our opening scene ends without the chest's flag (and no item of it reached the pouch) we tell
// the others (COOP_EV_CHEST_SHUT): their copy closes and can be opened again. Items are only ever shared from a real
// pick-up (the pouch), so nothing is given for a refused chest. SAVAGEWIND_COOP_CHESTSHUT=0: off.
static struct {
    uint64_t a, start_ns, calm_ns;
    uint32_t hid;
    float pos[3];
    char flag[96];
    bool event_seen, early_sent;
} chest_mine;
static bool chest_item_early_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_CHESTITEMEARLY");
    return on;
}
static bool chest_shut_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_CHESTSHUT");
    return on;
}
static const char *chest_flag_of(uint32_t hid) {  // (the chest's flag name, from the placed actors we name)
    for (int k = 0; k < named_n; k++)
        if (named[k].hashid == hid && strstr(named[k].name, "_TBox_")) return named[k].name;
    return NULL;
}
static void chest_mine_end(const char *why) {
    if (chest_mine.a)
        LOG("[coop] chest: our chest %s: %s\n", chest_mine.flag[0] ? chest_mine.flag : "(no flag name)", why);
    memset(&chest_mine, 0, sizeof chest_mine);
}
// Every 0.25 s (world_frame).
static void chest_mine_frame(Cpu *c, uint64_t now, const float me[3]) {
    static bool was_opening;
    bool opening = local_chest_open_ns && now - local_chest_open_ns < 400000000ull;
    bool started = opening && !was_opening;
    was_opening = opening;
    if (!local_in_world()) { memset(&chest_mine, 0, sizeof chest_mine); return; }
    if (!chest_item_early_on() && !chest_shut_on()) return;
    if (started && !chest_mine.a) {
        struct ChestNear k = {{me[0], me[1], me[2]}, 2.5f * 2.5f, 0};
        walk_actors(c, chest_name, chest_near_seen, &k);
        uint32_t hid = k.a ? actor_hash(c, k.a) : 0;
        const char *flag = hid ? chest_flag_of(hid) : NULL;
        if (!k.a || !flag) {
            LOG("[coop] chest: our Link opens a chest, but %s\n", !k.a ? "none is within 2.5 m" : "its flag name is not known");
            return;
        }
        if (gdt_get_bool(flag) == 1) return;  // (opened before: nothing to share)
        chest_mine.a = k.a, chest_mine.hid = hid, chest_mine.start_ns = now;
        chest_mine.pos[0] = LDF32(k.a + ACTOR_MTX + 12), chest_mine.pos[1] = LDF32(k.a + ACTOR_MTX + 28),
        chest_mine.pos[2] = LDF32(k.a + ACTOR_MTX + 44);
        snprintf(chest_mine.flag, sizeof chest_mine.flag, "%s", flag);
        LOG("[coop] chest: our Link opens %s: its item is shared when it reaches the pouch\n", flag);
        return;
    }
    if (!chest_mine.a) return;
    bool in_ev = framepace_in_event();
    if (in_ev) chest_mine.event_seen = true, chest_mine.calm_ns = 0;
    if (gdt_get_bool(chest_mine.flag) == 1) { chest_mine_end("opened (its flag is on)"); return; }
    if (chest_mine.event_seen && !in_ev) {
        if (!chest_mine.calm_ns) chest_mine.calm_ns = now;
        else if (now - chest_mine.calm_ns > 2000000000ull) {
            if (chest_mine.early_sent) { chest_mine_end("scene over, item shared, flag still off"); return; }
            char area[96];
            CoopChestShut e = {chest_mine.hid, local_area(area, sizeof area), {chest_mine.pos[0], chest_mine.pos[1], chest_mine.pos[2]}, 0};
            bool sent = chest_shut_on() && coop_chest_shut_ok(&e) && coop_net_send_event(COOP_EV_CHEST_SHUT, &e, sizeof e);
            chest_mine_end(sent ? "closed again without its item (pouch full?): the other players' copies close too"
                                : "closed again without its item (pouch full?): nobody told");
            return;
        }
    }
    if (!chest_mine.event_seen && now - chest_mine.start_ns > 10000000000ull) { chest_mine_end("no opening scene: forgotten"); return; }
    if (now - chest_mine.start_ns > 120000000000ull) chest_mine_end("still not opened after 2 min: forgotten");
}
// While our chest opens: an item that reached the pouch opens its acquisition at once (world_frame, before the diff).
static void chest_mine_pouch(uint64_t now, const struct PouchCount *cur, int cn) {
    if (!chest_mine.a || chest_mine.early_sent || acq.open || cn < 0 || pouch_prev_n < 0 || !chest_item_early_on()) return;
    bool gain = false;
    for (int i = 0; i < cn && !gain; i++) {
        int32_t before = 0;
        for (int j = 0; j < pouch_prev_n; j++)
            if (!strcmp(pouch_prev[j].name, cur[i].name)) before = pouch_prev[j].count;
        gain = cur[i].count > before && strncmp(cur[i].name, "Item_Cook_", 10) && !drops_owns_gain(cur[i].name, now) &&
               !castle_item_own(cur[i].name);
    }
    if (!gain) return;
    acq_send();
    memset(&acq, 0, sizeof acq);
    acq.open = true, acq.early = true, acq.opened_ns = now;
    float *me = (float *)(uintptr_t)(player_info_ptr() + 0x88);
    memcpy(acq.w.pos, me, 12);
    acq.w.hashid = chest_mine.hid;
    char area[96];
    acq.w.area = local_area(area, sizeof area);
    snprintf(acq.w.flag, sizeof acq.w.flag, "%s", chest_mine.flag);
    chest_mine.early_sent = true;
    chest_early_sent_crc = crc_of(chest_mine.flag), chest_early_sent_ns = now;
    LOG("[coop] share: %s - its item reached our pouch (the pop-up): shared now\n", chest_mine.flag);
}
// The other player's chest closed again with its item refused (COOP_EV_CHEST_SHUT): our copy, locked open by
// CHESTEARLY, closes and can be opened again.
static void chest_shut_ev(Cpu *c, const uint8_t *data, int len) {
    if (len != (int)sizeof(CoopChestShut) || !local_in_world()) return;
    CoopChestShut e;
    memcpy(&e, data, sizeof e);
    if (!coop_chest_shut_ok(&e)) { LOG("[coop] chest: bad chest-closed event dropped\n"); return; }
    if (!chest_shut_on()) return;
    char area[96];
    if (e.area != local_area(area, sizeof area)) return;
    struct HashHit hit = {e.hash, 0};
    walk_actors(c, share_actor_name, find_hashid, &hit);
    const char *flag = chest_flag_of(e.hash);
    if (!hit.a) { LOG("[coop] chest: the other player's chest %u closed again: not loaded here\n", e.hash); return; }
    if (flag && gdt_get_bool(flag) == 1) {
        LOG("[coop] chest: the other player's chest %s closed again, but it is opened here: left open\n", flag);
        return;
    }
    float me[3];
    memcpy(me, (const void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    float mx = LDF32(hit.a + ACTOR_MTX + 12) - me[0], mz = LDF32(hit.a + ACTOR_MTX + 44) - me[2];
    if (local_chest_open_ns && plat_now_ns() - local_chest_open_ns < 10000000000ull && mx * mx + mz * mz < 16.0f) {
        LOG("[coop] chest: the other player's chest %u closed again: we are opening it ourselves, left alone\n", e.hash);
        return;
    }
    for (int i = 0; i < 8; i++)
        if (chest_early_done[i] == hit.a) chest_early_done[i] = 0;
    for (int i = 0; i < 4; i++)
        if (chest_lid[i].a == hit.a) chest_lid[i].a = 0;
    ST8(hit.a + ENEMY_FLAGS, (uint8_t)(LD8(hit.a + ENEMY_FLAGS) & ~ENEMY_SKIP_AI));
    play_anim(c, hit.a, "Close", 0);
    for (int i = 0; i < 4; i++)  // (then the closed pose, held: stage 2 of the lid steps)
        if (!chest_lid[i].a) { chest_lid[i].a = hit.a, chest_lid[i].at_ns = plat_now_ns() + 2000000000ull, chest_lid[i].stage = 2; break; }
    LOG("[coop] chest: the other player's chest %s closed again (item refused there): ours closes, openable again\n",
        flag ? flag : "?");
}

static volatile int pouch_dump;  // `coop pouch [filter]` (dev)
static char pouch_dump_filter[64];
static void world_flags(uint64_t now, const float *me);
static void world_frame(Cpu *c) {
    static uint64_t next_ns, named_ns;
    chest_early_frame(c);
    chest_lid_frame(c);
    getdemo_frame(c);
    if (orb_wait.count && local_in_world()) {  // (a shared spirit orb held while we were in the shrine)
        char area[96];
        local_area(area, sizeof area);
        if (strncmp(area, "CDungeon", 8) || plat_now_ns() > orb_wait.until_ns) {
            int32_t now_has = pouch_count_of("Obj_DungeonClearSeal");
            int got_own = now_has > orb_wait.had ? now_has - orb_wait.had : 0;
            int give = orb_wait.count - got_own;
            LOG("[coop] shared spirit orb x%d: our own scene gave %d meanwhile: %d given now\n", orb_wait.count, got_own,
                give > 0 ? give : 0);
            if (give > 0) give_shared_item(c, "Obj_DungeonClearSeal", give);
            orb_wait.count = 0;
        }
    }
    if (pouch_dump) {
        pouch_dump = 0;
        static struct PouchCount all[POUCH_MAX];
        int n = pouch_read(all), shown = 0;
        for (int i = 0; i < n; i++)
            if (strstr(all[i].name, pouch_dump_filter)) LOG("[coop] pouch %s %d\n", all[i].name, all[i].count), shown++;
        LOG("[coop] pouch: %d kinds, %d shown\n", n, shown);
    }
    static uint32_t area_was;
    if (!share_on()) {
        uint16_t type;
        int len, origin;
        uint8_t data[COOP_EVENT_DATA];
        for (int i = 0; i < 8 && local_in_world() && coop_net_next_event_from(&type, data, &len, &origin); i++)
            if (type == COOP_EV_DISH || type == COOP_EV_DISH_RESULT) dish_event(type, data, len, origin);
        return;
    }
    // Peer events first (they wait in the queue while we are loading).
    uint64_t now = plat_now_ns();
    bool placed = last_local.placed_since_ns && now - last_local.placed_since_ns > 3000000000ull;
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (!placed || here != area_was) {
        // One still open goes out first (it carries its own area): a shrine's clear scene ends with the ride back
        // up, and that area change used to drop the clear (seen 2026-11-08).
        acq_send();
        world_reset();
        gate_release_all();
        named_n = 0;
        named_story();
        area_was = here;
        if (!placed) return;
    }
    {
        uint16_t type;
        int len;
        uint8_t data[COOP_EVENT_DATA];
        int origin = 0;
        for (int i = 0; i < 8 && local_in_world() && coop_net_next_event_from(&type, data, &len, &origin); i++)
            world_event_from(c, type, data, len, origin);  // (they wait in the queue while we load)
    }
    gate_frame(c, now);
    if (now < next_ns) return;
    next_ns = now + 250000000ull;
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    if (now >= named_ns) {  // the placed things around us, by flag name
        named_ns = now + 1000000000ull;
        // (the flag prefix: "MainField", a shrine's "DungeonNNN", and inside a Divine Beast "MainFieldDungeon" - not
        // "RemainsWater", which matched no flag: chests and terminals in a beast were never shared, found 2026-11-12)
        // (2026-11-17: a shrine's chests and items are "CDungeon_<name>_<hash>" in the game data - 157 + 39 + 8
        // CDungeon_TBox_Dungeon_* flags, no "DungeonNNN_" actor flag at all - so shrine chests were never shared nor
        // locked: a partner could open the same shrine chest again. SAVAGEWIND_COOP_SHRINEFLAGS=0: "DungeonNNN" as before.)
        static int cdg = -1;
        if (cdg < 0) cdg = look_on("SAVAGEWIND_COOP_SHRINEFLAGS");
        const char *prefix = !strcmp(area, "MainField")                ? "MainField"
                             : !strncmp(area, "MainFieldDungeon/", 17) ? "MainFieldDungeon"
                             : cdg && !strncmp(area, "CDungeon/", 9)   ? "CDungeon"
                                                                       : strchr(area, '/') + 1;
        struct NamedCtx x = {prefix, now, {me[0], me[1], me[2]}};
        walk_actors(c, world_actor_name, named_seen, &x);
    }
    chest_mine_frame(c, now, me);
    world_flags(now, me);  // (first: an item picked up turns its flag on in the same frame it reaches the pouch)
    // Pouch: what came in since the last look (only counted while an acquisition is open).
    static struct PouchCount cur[POUCH_MAX];
    int cn = pouch_read(cur);
    chest_mine_pouch(now, cur, cn);
    if (cn >= 0 && pouch_prev_n >= 0 && acq.open) {
        for (int i = 0; i < cn; i++) {
            int32_t before = 0;
            for (int j = 0; j < pouch_prev_n; j++)
                if (!strcmp(pouch_prev[j].name, cur[i].name)) before = pouch_prev[j].count;
            // (protocol 32: a shared dropped item we picked up is not an acquisition of ours to share again)
            if (cur[i].count <= before || !strncmp(cur[i].name, "Item_Cook_", 10) || drops_owns_gain(cur[i].name, now) ||
                castle_item_own(cur[i].name))
                continue;
            int k = 0;
            while (k < acq.n && strcmp(acq.items[k].name, cur[i].name)) k++;
            if (k == acq.n) {
                if (acq.n == ACQ_ITEMS) continue;
                snprintf(acq.items[acq.n].name, sizeof acq.items[acq.n].name, "%s", cur[i].name);
                acq.items[acq.n++].count = 0;
            }
            acq.items[k].count += cur[i].count - before;
            acq.last_item_ns = now;
        }
    }
    // A chest's item reaches the pouch during its opening scene, before the chest's flag turns on (seen twice: the
    // flag came with no item after it). So during a scene (and 2 s after it) the "before" snapshot stays the one from
    // before the scene: a flag turning on at the scene's end still gets the items the scene gave. The window also
    // stays open while a scene runs (at most 40 s) and closes 1 s after the scene or the last item.
    static uint64_t event_end_ns;
    if (framepace_in_event()) event_end_ns = now;
    bool scene_recent = now - event_end_ns < 2000000000ull;
    if (cn >= 0 && (acq.open || !scene_recent || pouch_prev_n < 0))
        memcpy(pouch_prev, cur, sizeof *cur * (size_t)cn), pouch_prev_n = cn;
    bool quiet = now - event_end_ns > ACQ_SETTLE_NS;
    if (acq.open && acq.early && acq.last_item_ns && now - acq.last_item_ns >= 200000000ull)
        acq_send();  // (our chest's item: at the pop-up, not after the scene)
    else if (acq.open && quiet &&
        (now - acq.opened_ns > ACQ_WAIT_NS || (acq.last_item_ns && now - acq.last_item_ns > ACQ_SETTLE_NS)))
        acq_send();
    else if (acq.open && now - acq.opened_ns > 40000000000ull)
        acq_send();
}

static bool gate_alive(const struct Gate *g) {
    return in_guest(g->actor) && in_guest(g->unit) && LD32(g->unit + UNIT_ID) == g->unit_id &&
           LD64(g->unit + UNIT_ACTOR) == g->actor && LD8(g->actor + PROC_STATE) != 3;
}
static void gate_freeze(struct Gate *g, bool on) {
    uint8_t f = LD8(g->actor + ENEMY_FLAGS);
    ST8(g->actor + ENEMY_FLAGS, on ? (uint8_t)(f | ENEMY_SKIP_AI) : (uint8_t)(f & ~ENEMY_SKIP_AI));
    g->frozen = on;
}
static void gate_release_all(void) {
    for (int i = 0; i < gate_n; i++)
        if (gates[i].frozen && gate_alive(&gates[i])) gate_freeze(&gates[i], false);
    gate_n = 0;  // (a load or area change: the actors are gone or found again)
    shrine_boarded.hash = 0;
    shrine_follow_ns = 0;
    shrine_riding = false;
    shrine_ride_wait.on = false;
}

// (58) More than two players sharing the world (SAVAGEWIND_COOP_MULTIWORLD): a gate waits for every world player in
// the same region (same area, within GATE_REGION m horizontally of it); players elsewhere (another region, a shrine,
// loading) do not hold it. Returns -1 with two players (the caller keeps the partner rule), else 1 open / 0 closed.
#define GATE_REGION 500.0f
static uint32_t world_others(void) {  // world players other than the partner (0: two-player play)
    int part = coop_net_partner_id();
    return coop_net_world_players(5.0) & ~(part >= 0 ? 1u << part : 0u);
}
static int gate_multi_open(const float pos[3], uint32_t area) {
    if (!world_others()) return -1;
    uint32_t w = coop_net_world_players(5.0);
    for (int pid = 0; pid < 8; pid++) {
        if (!(w & (1u << pid))) continue;
        CoopState rs;
        double age = 99;
        coop_net_view(pid);
        bool ok = coop_net_get_remote(&rs, &age) && age < 5.0 && (rs.flags & COOP_FLAG_IN_WORLD) && rs.area == area;
        coop_net_view(-1);
        if (!ok) continue;
        float dx = rs.pos[0] - pos[0], dy = rs.pos[1] - pos[1], dz = rs.pos[2] - pos[2];
        if (dx * dx + dz * dz > GATE_REGION * GATE_REGION) continue;  // (another region)
        if (dx * dx + dy * dy + dz * dz >= GATE_RADIUS * GATE_RADIUS) return 0;
    }
    return 1;
}

const char *framepace_event_name(void);
static bool event_start(Cpu *c, uint64_t actor, const char *flow, const char *entry);
static bool shrine_share_close(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], radius = share_radius();
    return dx * dx + dy * dy + dz * dz < radius * radius;
}

// Demo008_0 sets Open_DungeonNNN. The live map links react to this same flag, opening the
// shutter and activating the travel point without starting a second pedestal cutscene.
static uint64_t door_sent_ns[140];  // (shrine_door_watch: when each door was last sent)
static struct { int d; uint64_t until_ns; } door_retry;  // (a door opened while we were not in the world yet)
static int shrine_door_tell(const CoopShrineDoor *door, uint32_t here, const float me[3]);
static void shrine_door_send(const char *flag, const float me[3]) {
    if (!shrine_door_on()) return;
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (!local_in_world() || strcmp(area, "MainField")) {
        // (split-screen playtest 2026-11-26: the flag turns on as the door scene ends, while our Link is not "in the
        // world" for 3 s (the scene let it go): it was dropped here. Kept and sent again from shrine_door_watch.)
        int d = atoi(flag + 12);
        if (d >= 0 && d < 140 && door_retry.until_ns < plat_now_ns()) {
            door_retry.d = d, door_retry.until_ns = plat_now_ns() + 20000000000ull;
            LOG("[coop] shrine: %s opened here; sent once we are back in the world (in world %d, area %s)\n", flag,
                local_in_world(), area);
        }
        return;
    }
    door_retry.until_ns = 0;
    CoopShrineDoor door = {(uint32_t)atoi(flag + 12), {me[0], me[1], me[2]}};
    int dn = atoi(flag + 12);
    if (dn >= 0 && dn < 140) door_sent_ns[dn] = plat_now_ns();
    LOG("[coop] shrine: %s opened here, told %d nearby partners\n", flag, shrine_door_tell(&door, here, me));
}

// (0.1.1, split-screen playtest 2026-11-26: the partner's cutscene only played after the first one ended, instead
// of at the same time.) The flag turns on only as the pedestal scene ends, so the partner's scene came
// after ours. Now the start of our own pedestal scene (Demo008_*, in MainField, not one a partner's event started
// here) is sent at once as a door with no number (COOP_SHRINE_DOOR_SCENE): a nearby free partner presses its pedestal
// near our spot (shrine_door_scene) and both scenes run together. The send with the number at the scene's end stays
// (it does nothing where the partner's door is already open). SAVAGEWIND_COOP_DOORSTART=0: only the send at the end.
// NOT HANDLED: a partner whose own door at this shrine was open already gets the scene again (the number is not known
// at the start: the map data does not name the shrine at its pedestal).
static uint64_t door_scene_from_partner_ns;  // (when a partner's door started our pedestal scene)
static void shrine_door_scene_started(const char *flow) {
    static int on = -1;
    if (!shrine_door_on() || !env_on("SAVAGEWIND_COOP_DOORSTART", &on) || strncmp(flow, "Demo008_", 8)) return;
    uint64_t now = plat_now_ns();
    if (door_scene_from_partner_ns && now - door_scene_from_partner_ns < 15000000000ull) return;  // (theirs, not ours)
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    float me[3];
    if (strcmp(area, "MainField") || !player_pos(me)) return;
    CoopShrineDoor door = {COOP_SHRINE_DOOR_SCENE, {me[0], me[1], me[2]}};
    LOG("[coop] shrine: our door scene %s started: told %d nearby partners\n", flow, shrine_door_tell(&door, here, me));
}

static int shrine_door_tell(const CoopShrineDoor *door_in, uint32_t here, const float me[3]) {
    CoopShrineDoor door = *door_in;
    uint32_t peers = coop_net_world_players(1.0);
    int sent = 0;
    for (int pid = 0; pid < COOP_MAX_PLAYERS; pid++) {
        if (!(peers & (1u << pid))) continue;
        CoopState rs;
        double age;
        coop_net_view(pid);
        bool close = coop_net_get_remote(&rs, &age) && age < 1.0 && (rs.flags & COOP_FLAG_IN_WORLD) &&
                     rs.area == here && shrine_share_close(me, rs.pos);
        coop_net_view(-1);
        if (close && coop_net_send_event_to(pid, COOP_EV_SHRINE_DOOR, &door, sizeof door)) sent++;
    }
    return sent;
}

// (0.1.1, split-screen playtest 2026-11-26, twice: whoever pressed the pedestal FIRST never told the partner - no
// "opened here" line, the flag watch in world_flags never saw its flag turn on; the second presser's did.) A watch of
// the shrine-door flags alone: the 140 Open_DungeonNNN flag objects, found once by name crc, each checked every 250 ms.
// A flag that turns on here (not one a partner's event set: applied[]) is sent by shrine_door_send unless world_flags
// sent it already. Every change is logged with where we are, to see why the other watch missed it.
static struct { uint64_t obj[140]; uint32_t crc[140]; uint8_t val[140]; bool mapped, seeded; uint64_t next_ns; } doorw;
static void shrine_door_watch(uint64_t now) {
    if (!shrine_door_on() || now < doorw.next_ns) return;
    doorw.next_ns = now + 250000000ull;
    if (door_retry.until_ns) {  // (a door opened during its scene: send it once we are back in the world, 20 s max)
        if (now > door_retry.until_ns) {
            LOG("[coop] shrine: Open_Dungeon%03d never sent: not back in the world within 20 s\n", door_retry.d);
            door_retry.until_ns = 0;
        } else if (local_in_world() && player_info_ptr()) {
            char flag[24];
            snprintf(flag, sizeof flag, "Open_Dungeon%03d", door_retry.d);
            door_retry.until_ns = 0;
            shrine_door_send(flag, (const float *)(uintptr_t)(player_info_ptr() + 0x88));
        }
    }
    uint64_t mgr = LD64(A_GDT_MGR), tp = in_guest(mgr) ? LD64(mgr + 0xc00) : 0;
    if (!in_guest(tp)) return;
    uint32_t count = LD32(tp + 0x08);
    uint64_t ptrs = LD64(tp + 0x10);
    if (!count || count > FLAG_MAX || !in_guest(ptrs)) return;
    if (!doorw.mapped) {
        uint32_t *crcs = doorw.crc;
        char n[24];
        memset(doorw.obj, 0, sizeof doorw.obj);
        doorw.seeded = false;
        for (int d = 0; d < 140; d++) snprintf(n, sizeof n, "Open_Dungeon%03d", d), crcs[d] = crc_of(n);
        for (uint32_t i = 0; i < count; i++) {
            uint64_t f = LD64(ptrs + 8ull * i);
            if (!in_guest(f)) continue;
            uint32_t crc = LD32(f + 0xc);
            for (int d = 0; d < 140; d++)
                if (crcs[d] == crc) doorw.obj[d] = f;
        }
        doorw.mapped = true;
    }
    bool seed = !doorw.seeded;
    doorw.seeded = true;
    for (int d = 0; d < 140; d++) {
        uint64_t f = doorw.obj[d];
        if (!in_guest(f)) continue;
        if (LD32(f + 0xc) != doorw.crc[d]) {  // (the flag store was made again: a save load) find them again
            doorw.mapped = false;
            return;
        }
        uint8_t v = LD8(f + 0xb) & 1;
        if (!seed && v && !doorw.val[d]) {
            char flag[24], area[96];
            snprintf(flag, sizeof flag, "Open_Dungeon%03d", d);
            uint32_t crc = crc_of(flag);
            bool ours = true;
            for (int k = 0; k < 64; k++)
                if (applied[k].crc == crc && now - applied[k].at_ns < 30000000000ull) ours = false;
            local_area(area, sizeof area);
            const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
            LOG("[coop] shrine: watch: %s turned on here (ours %d, in world %d, area %s, sent by the flag watch %d)\n",
                flag, ours, local_in_world(), area, now - door_sent_ns[d] < 30000000000ull);
            if (ours && player_info_ptr() && now - door_sent_ns[d] >= 30000000000ull) shrine_door_send(flag, me);
        }
        doorw.val[d] = v;
    }
}

// (0.1.1, playtest 2026-11-26: the door opened and the cutscene played, but not for the one who did not press
// the button) A nearby partner who is free (no message, menu or scene) gets the door scene too: we send our own
// shrine pedestal's "pressed" signal (the same call the tower follow uses, A_SET_OUT_SIGNAL), so the game starts the
// pedestal's EventTag (Demo008_0, or Demo008_9 at the SP pedestals: map data, 93 + 25 EventTags with 4 links to
// shutter, warp point, entrance and pedestal) with its own map arguments, and the scene sets the flag. Busy partner,
// no pedestal found within 12 m of where the opener stood, or SAVAGEWIND_COOP_DOORSCENE=0: only the flag, as before.
static bool shrine_door_scene_on(void) {
    static int on = -1;
    return env_on("SAVAGEWIND_COOP_DOORSCENE", &on);
}
static bool shrine_pedestal_name(const char *n) { return !strncmp(n, "DgnObj_EntranceTerminal", 23); }
static bool goal_exit_name(const char *n) { return !strcmp(n, "DgnObj_GoalExit"); }
static struct { uint32_t dungeon; uint64_t at_ns; } door_check;  // (the scene did not start: set the flag)
static void shrine_door_check_frame(Cpu *c, uint64_t now) {
    if (!door_check.at_ns || now < door_check.at_ns || framepace_in_event() || local_paused()) return;
    char flag[24];
    snprintf(flag, sizeof flag, "Open_Dungeon%03u", door_check.dungeon);
    door_check.at_ns = 0;
    if (gdt_get_bool(flag) == 0 && gdt_set_bool(c, flag, true))
        LOG("[coop] shrine: the door scene did not set %s; set it\n", flag);
}
struct PedestalCtx { float at[3]; float best; uint64_t actor; };
static void shrine_pedestal_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)name;
    struct PedestalCtx *x = ctx;
    float p[3], d = actor_dist(a, x->at, p);
    if (d < x->best) x->best = d, x->actor = a;
}
static bool shrine_door_scene(Cpu *c, const CoopShrineDoor *door, const float me[3]) {
    if (!shrine_door_scene_on() || local_paused() || framepace_in_event()) return false;
    struct PedestalCtx x = {{door->pos[0], door->pos[1], door->pos[2]}, 12.0f, 0};
    walk_actors(c, shrine_pedestal_name, shrine_pedestal_seen, &x);
    if (!x.actor) {
        LOG("[coop] shrine: no pedestal near the partner's spot; only the door flag\n");
        return false;
    }
    Cpu saved = *c;
    c->x[0] = x.actor; c->x[1] = 0; c->x[2] = 1;
    guest_call(c, 0x70015335d8ull);  // A_SET_OUT_SIGNAL (pedestal BasicSig -> EventTag Demo008_0)
    *c = saved;
    door_scene_from_partner_ns = plat_now_ns();
    if (door->dungeon <= COOP_SHRINE_MAX)  // (no number: the scene sets the flag itself, and the partner's end send)
        door_check.dungeon = door->dungeon, door_check.at_ns = plat_now_ns() + 6000000000ull;
    LOG("[coop] shrine: pedestal %.1f m from the partner's spot signalled (we are %.1f m away)\n", x.best,
        sqrtf((me[0] - door->pos[0]) * (me[0] - door->pos[0]) + (me[2] - door->pos[2]) * (me[2] - door->pos[2])));
    return true;
}

static void shrine_door_ev(Cpu *c, const uint8_t *data, int len) {
    if (len != (int)sizeof(CoopShrineDoor) || !shrine_door_on() || !local_in_world()) return;
    CoopShrineDoor door;
    memcpy(&door, data, sizeof door);
    if (!coop_shrine_door_ok(&door)) return;
    char area[96];
    float me[3];
    local_area(area, sizeof area);
    if (strcmp(area, "MainField") || !player_pos(me) || !shrine_share_close(me, door.pos)) return;
    if (door.dungeon == COOP_SHRINE_DOOR_SCENE) {  // (the partner's pedestal scene just started: ours with it)
        if (shrine_door_scene(c, &door, me)) LOG("[coop] shrine: the partner's door scene started: ours too\n");
        return;
    }
    char flag[24];
    snprintf(flag, sizeof flag, "Open_Dungeon%03u", door.dungeon);
    if (gdt_get_bool(flag) != 0) return;
    applied[applied_next].crc = crc_of(flag), applied[applied_next].at_ns = plat_now_ns();
    applied_next = (applied_next + 1) % 64;
    if (shrine_door_scene(c, &door, me)) {
        LOG("[coop] shrine: %s opening here with the door scene, for a nearby partner\n", flag);
        return;
    }
    if (!gdt_set_bool(c, flag, true)) return;
    LOG("[coop] shrine: %s opened here for a nearby partner\n", flag);
}

static bool shrine_ride_on(void) {
    static int on = -1;
    return gate_on() && env_on("SAVAGEWIND_COOP_RIDETOGETHER", &on);
}

// On the platform: within SHRINE_DECK_R of its centre, feet at deck height. (2026-11-21 review: the attention cylinder
// is 1.1 m, but the host rode from 1.5 m off centre, and two players side by side cannot both stand within 1.1 m;
// 1.8 m covers the blue disc's edge plus a little lag in the partner's position.)
#define SHRINE_DECK_R 1.8f
static bool shrine_on_deck(const struct Gate *g, const float pos[3]) {
    float dx = pos[0] - g->pos[0], dz = pos[2] - g->pos[2];
    return dx * dx + dz * dz <= SHRINE_DECK_R * SHRINE_DECK_R && fabsf(pos[1] - g->pos[1]) <= 1.0f;
}

static bool shrine_peer_on_deck(const struct Gate *g, uint32_t area) {
    CoopState rs;
    double age;
    return coop_net_get_remote(&rs, &age) && age < 0.5 && (rs.flags & COOP_FLAG_IN_WORLD) &&
           rs.area == area && shrine_on_deck(g, rs.pos);
}

static void shrine_ride_frame(uint32_t area, const float me[3], uint64_t now) {
    bool riding = !strcmp(framepace_event_name(), "Demo008_1");
    if (riding && !shrine_riding) {
        for (int i = 0; i < gate_n; i++) {
            struct Gate *g = &gates[i];
            if (!shrine_boarded.hash || g->hash != shrine_boarded.hash || now < shrine_follow_ns) continue;
            int sent = 0;
            uint32_t peers = coop_net_world_players(0.5);
            for (int pid = 0; pid < COOP_MAX_PLAYERS; pid++) {
                if (!(peers & (1u << pid))) continue;
                coop_net_view(pid);
                bool aboard = shrine_peer_on_deck(g, area);
                coop_net_view(-1);
                if (aboard && coop_net_send_event_to(pid, COOP_EV_SHRINE_RIDE, &shrine_boarded, sizeof shrine_boarded)) sent++;
            }
            LOG("[coop] shrine: native ride started at elevator %u, %d partners aboard\n", g->hash, sent);
            break;
        }
    } else if (!riding) {
        shrine_boarded.hash = 0;
        for (int i = 0; i < gate_n; i++) {
            struct Gate *g = &gates[i];
            if (!g->tower && g->hash && shrine_on_deck(g, me)) {
                shrine_boarded.hash = g->hash;
                memcpy(shrine_boarded.pos, me, sizeof shrine_boarded.pos);
            }
        }
    }
    shrine_riding = riding;
}

// (2026-11-21 review) A ride request that arrives while a message/scene holds our Link (seen: the guest closing a
// message box on the deck when the host pressed A) waits up to SHRINE_RIDE_WAIT_NS and starts once our Link is free,
// if we are still on that elevator. By then the partner is already riding down (not on the deck any more), so the
// late start checks only the position they sent.
#define SHRINE_RIDE_WAIT_NS 10000000000ull
static void shrine_ride_try(Cpu *c, const CoopShrineRide *r, bool late);
static void shrine_ride_ev(Cpu *c, const uint8_t *data, int len) {
    uint64_t now = plat_now_ns();
    if (len != (int)sizeof(CoopShrineRide) || now < shrine_follow_ns || !shrine_ride_on() || !local_in_world()) return;
    CoopShrineRide ride;
    memcpy(&ride, data, sizeof ride);
    if (!coop_shrine_ride_ok(&ride)) return;
    if (local_paused() || framepace_in_event()) {
        shrine_ride_wait.on = true, shrine_ride_wait.until_ns = now + SHRINE_RIDE_WAIT_NS, shrine_ride_wait.r = ride;
        LOG("[coop] shrine: partner rides elevator %u; waiting for our message/scene to end\n", ride.hash);
        return;
    }
    shrine_ride_try(c, &ride, false);
}
static void shrine_ride_wait_frame(Cpu *c, uint64_t now) {
    if (!shrine_ride_wait.on) return;
    if (now > shrine_ride_wait.until_ns) {
        shrine_ride_wait.on = false;
        LOG("[coop] shrine: not riding with the partner (our message/scene lasted too long)\n");
        return;
    }
    if (local_paused() || framepace_in_event() || !local_in_world()) return;
    shrine_ride_wait.on = false;
    shrine_ride_try(c, &shrine_ride_wait.r, true);
}
static void shrine_ride_try(Cpu *c, const CoopShrineRide *r, bool late) {
    uint64_t now = plat_now_ns();
    CoopShrineRide ride = *r;
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    float me[3];
    if (strcmp(area, "MainField") || !player_pos(me)) return;
    for (int i = 0; i < gate_n; i++) {
        struct Gate *g = &gates[i];
        if (g->tower || g->hash != ride.hash || !gate_alive(g) || !shrine_on_deck(g, me) ||
            !shrine_on_deck(g, ride.pos) || (!late && !shrine_peer_on_deck(g, here))) continue;
        // Match RequestEventFromMapUnit: use the local elevator as the event actor so its map references resolve.
        if (event_start(c, g->actor, "Demo008_1", "Demo008_1")) {
            shrine_follow_ns = now + 2000000000ull;
            LOG("[coop] shrine: started native ride with partner on elevator %u\n", g->hash);
        }
        return;
    }
}

static void gate_frame(Cpu *c, uint64_t now) {
    static uint64_t said_ns;
    if (!gate_on()) return;
    CoopState rs = {0};
    double rage = 99;
    char area[96];
    bool peer = coop_net_get_remote(&rs, &rage) && rage < 5.0;  // (connected: else no gate, playing alone)
    uint32_t here_id = local_area(area, sizeof area);
    bool peer_here = peer && (rs.flags & COOP_FLAG_IN_WORLD) && rs.area == here_id;
    // Entrance rides and tower pedestals are in MainField. Shrine departures use leave_watch.
    bool overworld = !strcmp(area, "MainField");
    gate_pending_frame(c, now);
    shrine_ride_wait_frame(c, now);
    shrine_door_check_frame(c, now);
    shrine_door_watch(now);
    for (int i = 0; i < gate_n && overworld; i++) {
        struct Gate *g = &gates[i];
        if (!gate_alive(g)) { gates[i--] = gates[--gate_n]; continue; }
        if (!g->tower) continue;
        float dx = rs.pos[0] - g->pos[0], dy = rs.pos[1] - g->pos[1], dz = rs.pos[2] - g->pos[2];
        int multi = gate_multi_open(g->pos, here_id);  // (58: more than two players)
        bool open = multi >= 0 ? multi : !peer ||
                                     (peer_here && dx * dx + dy * dy + dz * dz < GATE_RADIUS * GATE_RADIUS);
        // (never freeze it mid-ride: our own ride's event is running)
        if (!open && !g->frozen && !framepace_in_event()) {
            gate_freeze(g, true);
            LOG("[coop] gate: %s at %.0f %.0f %.0f closed (the other player is not near it)\n",
                "tower pedestal", g->pos[0], g->pos[1], g->pos[2]);
        } else if (open && g->frozen) {
            gate_freeze(g, false);
            LOG("[coop] gate: tower pedestal at %.0f %.0f %.0f open\n", g->pos[0], g->pos[1], g->pos[2]);
        }
        if (g->tower && !g->activated && (LD32(g->actor + 0x71c) & 1)) {
            g->activated = true;
            EvRide r = {{g->pos[0], g->pos[1], g->pos[2]}, "Demo004_0"};
            bool sent = coop_net_send_event(EV_GATE_RIDE, &r, sizeof r);
            LOG("[coop] gate: our tower pedestal at %.1f %.1f %.1f activated%s\n", g->pos[0], g->pos[1], g->pos[2],
                sent ? ", the other player is told" : "");
        }
        const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
        float mx = me[0] - g->pos[0], mz = me[2] - g->pos[2];
        if (g->frozen && mx * mx + mz * mz < 9.0f && now - said_ns > 10000000000ull) {
            said_ns = now;
            LOG("[coop] gate: waiting for the other player to come to the tower\n");
        }
    }
    float me[3];
    if (overworld && shrine_ride_on() && local_in_world() && player_pos(me)) shrine_ride_frame(here_id, me, now);
}

// Follow a nearby tower activation once our own scene is over.
static struct { bool on; uint64_t until_ns; EvRide r; } gate_pending;
static void gate_follow(Cpu *c, const EvRide *r);
static void gate_pending_frame(Cpu *c, uint64_t now) {
    if (!gate_pending.on) return;
    if (now > gate_pending.until_ns) {
        gate_pending.on = false, gate_pending.until_ns = 0;
        LOG("[coop] gate: not following the tower activation (a scene kept running or pedestal not loaded)\n");
        return;
    }
    if (framepace_in_event()) return;
    gate_pending.on = false;
    gate_follow(c, &gate_pending.r);
}

static void gate_follow(Cpu *c, const EvRide *r) {
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    float dx = r->pos[0] - me[0], dy = r->pos[1] - me[1], dz = r->pos[2] - me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
    char area[96];
    local_area(area, sizeof area);
    bool overworld = !strcmp(area, "MainField");
    if (!local_in_world() || !overworld || d > GATE_RADIUS + 15.0f) {
        gate_pending.until_ns = 0;
        LOG("[coop] gate: the other player started a tower %.0f m away; not following%s\n",
            d, overworld ? "" : " (we are not in the overworld)");
        return;
    }
    {
        const char *ev = framepace_event_name();
        char my_as[32] = "";
        uint64_t pa = LD64(player_info_ptr() + 0x60);
        if (pa) player_as_name(c, pa, my_as, sizeof my_as);
        if (!strcmp(ev, "Demo004_0") || !strcmp(ev, "Demo164_0") || !strncmp(my_as, "Demo004_", 8)) {
            gate_pending.until_ns = 0;
            LOG("[coop] gate: tower cutscene already running here (%s)\n", ev[0] ? ev : my_as);
            return;
        }
    }
    if (framepace_in_event()) {
        if (!gate_pending.until_ns) {
            gate_pending.until_ns = plat_now_ns() + 15000000000ull;
            LOG("[coop] gate: the other player started a tower; following when the scene here ends\n");
        }
        gate_pending.on = true, gate_pending.r = *r;
        return;
    }
    {
        struct Gate *g = NULL;
        for (int i = 0; i < gate_n; i++) {
            float ex = gates[i].pos[0] - r->pos[0], ez = gates[i].pos[2] - r->pos[2];
            if (gates[i].actor && gates[i].tower && ex * ex + ez * ez < 25.0f && gate_alive(&gates[i])) g = &gates[i];
        }
        if (!g) {
            struct NamedCtx x = {"MainField", plat_now_ns(), {me[0], me[1], me[2]}};
            walk_actors(c, gate_tower_name, named_seen, &x);
            for (int i = 0; i < gate_n; i++) {
                float ex = gates[i].pos[0] - r->pos[0], ez = gates[i].pos[2] - r->pos[2];
                if (gates[i].actor && gates[i].tower && ex * ex + ez * ez < 25.0f && gate_alive(&gates[i])) g = &gates[i];
            }
        }
        if (!g) {
            uint64_t now = plat_now_ns();
            if (!gate_pending.until_ns || now > gate_pending.until_ns) gate_pending.until_ns = now + 15000000000ull;
            gate_pending.on = true, gate_pending.r = *r;
            return;
        }
        gate_pending.until_ns = 0;
        if (g->frozen) gate_freeze(g, false);
        g->activated = true;
        Cpu saved = *c;
        c->x[0] = g->actor; c->x[1] = 0; c->x[2] = 1;
        guest_call(c, 0x70015335d8ull);  // A_SET_OUT_SIGNAL (BasicSig -> LinkTagAnd -> EventTag Demo004_0)
        *c = saved;
        LOG("[coop] gate: activating tower pedestal at %.1f %.1f %.1f with the other player\n", g->pos[0], g->pos[1],
            g->pos[2]);
    }
}

static void gate_follow_ev(Cpu *c, const uint8_t *data, int len) {
    if (len != (int)sizeof(EvRide) || !gate_on()) return;
    EvRide r;
    memcpy(&r, data, sizeof r);
    if (memcmp(r.event, "Demo004_0", sizeof "Demo004_0")) return;
    for (int i = 0; i < 3; i++) if (!isfinite(r.pos[i]) || fabsf(r.pos[i]) > 100000) return;
    gate_follow(c, &r);
}

// ---- leaving a shrine together (2026-11-08). When our player leaves a shrine (any way: the exit
// elevator, the warp after the monk, fast travel) while the other player is still in it, they are told; there,
// their Link is put onto that shrine's exit elevator and pressing A rides it up.
// SAVAGEWIND_COOP_GATE=0 turns it off too.
typedef struct EvLeave { uint32_t area; char name[64]; } EvLeave;
static struct { bool on; uint64_t until_ns; EvLeave l; } leave_pending;
static void leave_watch(void) {  // (every frame, also while loading: the area name changes when the load starts)
    static uint32_t was;
    static char was_name[96];
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (here == was) return;
    // (2026-11-12: a Divine Beast too - MainFieldDungeon/Remains*)
    bool beast = beast_on() && !strncmp(was_name, "MainFieldDungeon/Remains", 24);
    if (was && (!strncmp(was_name, "CDungeon", 8) || beast) && gate_on()) {
        CoopState rs;
        double age = 99;
        bool peer_inside = coop_net_get_remote(&rs, &age) && age < 5.0 && rs.area == was;
        uint32_t others = world_others();  // (58: or any other world player still in it)
        for (int pid = 0; pid < 8 && !peer_inside; pid++) {
            if (!(others & (1u << pid))) continue;
            coop_net_view(pid);
            peer_inside = coop_net_get_remote(&rs, &age) && age < 5.0 && rs.area == was;
            coop_net_view(-1);
        }
        if (peer_inside) {
            EvLeave l = {was, ""};
            snprintf(l.name, sizeof l.name, "%s", was_name);
            bool sent = coop_net_send_event(EV_GATE_LEAVE, &l, sizeof l);
            LOG("[coop] gate: we left %s, the other player is still in it%s\n", was_name,
                sent ? ": told to come along" : " (NOT sent)");
        }
    }
    was = here;
    snprintf(was_name, sizeof was_name, "%s", area);
}
static bool monk_share_on(void);  // (coop/story.inc)
struct FindGate { uint64_t a; };
static void find_gate(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)name;
    struct FindGate *f = ctx;
    if (!f->a) f->a = a;
}
static void leave_follow(Cpu *c) {
    if (!leave_pending.on) return;
    uint64_t now = plat_now_ns();
    if (now > leave_pending.until_ns) {
        leave_pending.on = false;
        LOG("[coop] gate: not following the other player out (a scene kept running here)\n");
        return;
    }
    if (!local_in_world() || framepace_in_event()) return;
    leave_pending.on = false;
    char area[96];
    if (local_area(area, sizeof area) != leave_pending.l.area) {
        LOG("[coop] gate: the other player left %s; we are not in it\n", leave_pending.l.name);
        return;
    }
    // A Divine Beast (2026-11-12): no elevator; Link goes to the spot where the beast is entered, next to its
    // entrance terminal, whose A asks to leave (the game's own leave scene, Demo005).
    if (!strncmp(area, "MainFieldDungeon/Remains", 24)) {
        static const struct { const char *n; float p[3]; } entr[] = {
            {"Water", {48.0f, 6.3f, 3.0f}}, {"Fire", {0.0f, 16.8f, 69.5f}},
            {"Wind", {0.0f, 3.4f, -77.7f}}, {"Electric", {0.0f, 71.9f, 0.0f}},
        };
        for (size_t i = 0; i < sizeof entr / sizeof entr[0]; i++) {
            if (strcmp(area + 24, entr[i].n)) continue;
            uint64_t pa = control_cell_addr(&control_cell_player_pos);
            bool ok = pa && control_mem_poke(pa, entr[i].p, sizeof entr[i].p);
            LOG("[coop] gate: the other player left %s: to its entrance at %.1f %.1f %.1f (A at the terminal "
                "leaves)%s\n", area, entr[i].p[0], entr[i].p[1], entr[i].p[2], ok ? "" : " (could not move Link)");
            return;
        }
        LOG("[coop] gate: the other player left %s (no entrance spot known)\n", area);
        return;
    }
    // (0.1.1, playtest 2026-11-26: after the partner's monk scene the waiting player got teleported back to the
    // beginning of the shrine and could not get out) A shrine that is cleared here: no ride down to the entrance. The
    // game's own exit platform at the monk (DgnObj_GoalExit) asks "leave?" by itself when Link stands on it (the
    // shared monk scene already puts Link there): we only move Link onto it when it is elsewhere. Starting its scene
    // ourselves (Demo033_1 / GoalExit) went wrong both ways: on our Link it crashed the next load (test_monk run 4,
    // read at 0x1f0); on the platform it never ended and held Link (split-screen playtest, same day).
    if (!strncmp(area, "CDungeon/Dungeon", 16) && monk_share_on()) {
        char clear[48];
        snprintf(clear, sizeof clear, "Clear_%s", area + 9);
        float me[3] = {0};
        uint64_t pi = player_info_ptr();
        if (pi) memcpy(me, (const void *)(uintptr_t)(pi + 0x88), 12);
        struct PedestalCtx x = {{me[0], me[1], me[2]}, 1e9f, 0};
        if (gdt_get_bool(clear) == 1) {
            walk_actors(c, goal_exit_name, shrine_pedestal_seen, &x);
            if (!x.actor) {
                LOG("[coop] gate: the other player left %s (cleared here): its exit platform is not loaded; stay\n", area);
                return;
            }
            if (x.best < 3.0f) {
                LOG("[coop] gate: the other player left %s (cleared here): we are on the exit platform (%.1f m)\n", area,
                    x.best);
                return;
            }
            float p[3], to[3];
            actor_dist(x.actor, me, p);
            to[0] = p[0], to[1] = p[1] + 0.5f, to[2] = p[2];
            uint64_t pa = control_cell_addr(&control_cell_player_pos);
            bool ok = pa && control_mem_poke(pa, to, sizeof to);
            LOG("[coop] gate: the other player left %s (cleared here): onto the exit platform at %.1f %.1f %.1f%s\n", area,
                to[0], to[1], to[2], ok ? "" : " (could not move Link)");
            return;
        }
    }
    struct FindGate f = {0};
    walk_actors(c, gate_actor_name, find_gate, &f);
    if (!f.a) {
        LOG("[coop] gate: the other player left %s, but its elevator is not loaded here\n", area);
        return;
    }
    float p[3], me[3] = {0, 0, 0};
    actor_dist(f.a, me, p);
    float to[3] = {p[0], p[1] + 1.5f, p[2]};
    uint64_t pa = control_cell_addr(&control_cell_player_pos);
    bool ok = pa && control_mem_poke(pa, to, sizeof to);
    LOG("[coop] gate: the other player left %s: onto its elevator at %.1f %.1f %.1f (A rides up)%s\n", area, to[0],
        to[1], to[2], ok ? "" : " (could not move Link)");
}
static void leave_ev(const uint8_t *data, int len) {
    if (len != (int)sizeof(EvLeave) || !gate_on()) return;
    memcpy(&leave_pending.l, data, sizeof leave_pending.l);
    leave_pending.l.name[sizeof leave_pending.l.name - 1] = 0;
    leave_pending.on = true, leave_pending.until_ns = plat_now_ns() + 30000000000ull;
}

static bool revive_ground(const CoopState *rs) {  // (standing, walking, fighting: on the ground)
    return rs->anim <= COOP_ANIM_DASH || coop_anim_is_cut(rs->anim) || rs->anim == COOP_ANIM_SQUAT_WAIT ||
           rs->anim == COOP_ANIM_SQUAT_MOVE || rs->anim == COOP_ANIM_GUARD || rs->anim == COOP_ANIM_BOW ||
           rs->anim == COOP_ANIM_GUARD_JUST || rs->anim == COOP_ANIM_BOW_SHOOT || rs->anim == COOP_ANIM_LAND;
}
// The death rule's second half (see revive_died): once back in the world after a death, next to the partner.
static void revive_frame(uint64_t now) {
    if (!revive.pending) return;
    if (!revive_on()) { revive.pending = false; return; }
    if (!local_in_world() || framepace_in_event()) return;  // (the reload, or the game's own scene after it)
    if (!revive.since_ns) revive.since_ns = now;
    CoopState rs;
    double age = 99;
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    // (58) more than two players: next to the nearest one alive and on the ground in our area (else the partner,
    // with the reasons below)
    int pick = -1;
    uint32_t others = world_others();
    if (others) {
        const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
        uint32_t w = coop_net_world_players(2.0);
        float bd = 1e30f;
        for (int pid = 0; pid < 8; pid++) {
            if (!(w & (1u << pid))) continue;
            coop_net_view(pid);
            bool ok = coop_net_get_remote(&rs, &age) && age < 2.0 && (rs.flags & COOP_FLAG_IN_WORLD) &&
                      rs.area == here && revive_ground(&rs);
            coop_net_view(-1);
            float dx = rs.pos[0] - me[0], dy = rs.pos[1] - me[1], dz = rs.pos[2] - me[2], d2 = dx * dx + dy * dy + dz * dz;
            if (ok && d2 < bd) bd = d2, pick = pid;
        }
    }
    if (pick >= 0) coop_net_view(pick);
    bool have = coop_net_get_remote(&rs, &age) && age < 2.0;
    coop_net_view(-1);
    const char *why = NULL;
    if (!have) why = "no other player";
    else if (!(rs.flags & COOP_FLAG_IN_WORLD)) why = "the other player is not in the world (loading or dead too)";
    else if (rs.area != here) why = "the other player is in another area";
    else if (!revive_ground(&rs)) why = "the other player is not on the ground";
    if (why) {
        if (now - revive.since_ns > 30000000000ull) {
            revive.pending = false;
            LOG("[coop] revive: staying here (%s)\n", why);
        }
        return;
    }
    revive.pending = false;
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    float dx = rs.pos[0] - me[0], dy = rs.pos[1] - me[1], dz = rs.pos[2] - me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d <= REVIVE_MIN_M) {
        LOG("[coop] revive: the other player is %.0f m away; staying here\n", d);
        return;
    }
    // 1.5 m beside them (behind, from their facing) and 1 m up: Link drops onto the ground they stand on.
    float yaw = rs.yaw * 3.14159265f / 180.0f;
    float to[3] = {rs.pos[0] - 1.5f * sinf(yaw), rs.pos[1] + 1.0f, rs.pos[2] - 1.5f * cosf(yaw)};
    uint64_t pa = control_cell_addr(&control_cell_player_pos);
    bool ok = pa && control_mem_poke(pa, to, sizeof to);
    LOG("[coop] revive: back next to the other player (%.0f m from the save point) at %.1f %.1f %.1f%s (player %d; we were at %.0f %.0f %.0f)\n",
        d, to[0], to[1], to[2], ok ? "" : " (could not move Link)", pick >= 0 ? pick : coop_net_partner_id(), me[0], me[1],
        me[2]);
}

// `coop bodies <actor>` (dev, RE): every rigid body (vtable RIGID_BODY_VT) reachable from the actor through at most
// three pointers found in the first 0x1000 bytes of each object, with its position, to the log. (Read-only.)
static void bodies_walk(uint64_t obj, int depth, uint32_t *path, uint64_t actor, int *found) {
    if (*found >= 24) return;
    for (uint32_t off = 0; off < 0x1000; off += 8) {
        uint64_t p = LD64(obj + off);
        if (!in_guest(p) || (p & 7)) continue;
        path[depth] = off;
        if (LD64(p) == RIGID_BODY_VT) {
            const float *bp = (const float *)(uintptr_t)(p + 0x1a0);
            const float *m = (const float *)(uintptr_t)(actor + ACTOR_MTX);
            char s[64] = "";
            for (int k = 0; k <= depth; k++) snprintf(s + strlen(s), sizeof s - strlen(s), "+0x%x%s", path[k], k < depth ? " ->" : "");
            LOG("[coop] bodies %llx: %s = %llx at %.2f %.2f %.2f (actor at %.2f %.2f %.2f)\n", (unsigned long long)actor, s,
                (unsigned long long)p, bp[0], bp[1], bp[2], m[3], m[7], m[11]);
            if (++*found >= 24) return;
            continue;
        }
        if (depth < 2 && in_guest(p + 0x1000)) bodies_walk(p, depth + 1, path, actor, found);
    }
}
// `coop aiobj <actor> [f32 value]` (dev, RE): objects reachable from the actor (2 pointers deep, first 0x1000 bytes)
// that point back to it at +8 (AI actions/roots do), with their vtable; "*" when one of their first 0x100 bytes
// is a pointer to a float equal to the value (an AI parameter, e.g. the pinwheel's SwRadTh 0.2618).
static void aiobj_walk(uint64_t obj, int depth, uint64_t actor, float want, int *found) {
    for (uint32_t off = 0; off < 0x1000 && *found < 40; off += 8) {
        uint64_t p = LD64(obj + off);
        if (!in_guest(p) || (p & 7) || !in_guest(p + 0x100)) continue;
        uint64_t vt = LD64(p);
        if (LD64(p + 8) == actor && vt >= 0x7000000000ull && vt < 0x7003000000ull) {
            bool has = false;
            for (uint32_t k = 0; k < 0x100 && !has; k += 8) {
                uint64_t q = LD64(p + k);
                if (in_guest(q) && !(q & 3)) {
                    float f;
                    memcpy(&f, (void *)(uintptr_t)q, 4);
                    has = fabsf(f - want) < 1e-5f;
                }
            }
            LOG("[coop] aiobj %llx (depth %d, +0x%x): vtable %llx%s\n", (unsigned long long)p, depth, off,
                (unsigned long long)vt, has ? " *" : "");
            ++*found;
        }
        if (depth < 1) aiobj_walk(p, depth + 1, actor, want, found);
    }
}
static void bodies_report(uint64_t a) {
    if (!in_guest(a) || !in_guest(a + 0x1000)) { LOG("[coop] bodies: bad actor\n"); return; }
    uint32_t path[3];
    int found = 0;
    bodies_walk(a, 0, path, a, &found);
    LOG("[coop] bodies %llx: %d found\n", (unsigned long long)a, found);
}
