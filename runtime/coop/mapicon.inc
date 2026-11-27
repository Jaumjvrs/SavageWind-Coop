// Part of runtime/coop.c (included there, same translation unit): the other player as a star on our map, moving live.
// (2026-11-15, play report: the other player's icon was missing on the map, and the star should move in real time)
//
// RE (offline): the map's icon manager LD64(0x7001e17ef8) places a stamp with
// FUN 0x7000d8aaa0(mgr, No, &pos, 0): it takes the pool's next free object LD64(mgr+0x650), sets its position through
// obj->vt[0xb8](obj, &pos), No 31 = star. Moving it later = the same vt[0xb8] call, then 0x7000d7b8e4(obj, 1) puts it
// on the manager's redraw list. Placed this way the star is (G) not in the saved-stamp list, so it is not written to
// the save (the saved list is filled by the stamp UI's own handlers). We move it every frame the partner moved, also
// while our map is open (the frame hook runs while paused).
// Safety: the icon list is rebuilt at a stage change / world reload (all objects cleared, the pool reused): we drop our
// object while not in the world, and before each move check that the object still holds the position we last gave it
// (offset found once by searching the object for those floats); if not, it was reused for something else: place anew.
// Partner gone / other area: the star is moved off the map (x -100000, the game's "empty stamp" value).
// Not verified in game yet (2026-11-15). SAVAGEWIND_COOP_MAPICON=0: off. Dev: `coop mapicon <x> <y> <z> | clear`.
#define A_MAPICON_MGR 0x7001e17ef8ull
#define A_MAPICON_PLACE 0x7000d8aaa0ull  // (x0 mgr, w1 No, x2 &Vec3f, w3 0)
#define A_MAPICON_DIRTY 0x7000d7b8e4ull  // (x0 obj, w1 1)
#define MAPICON_STAR 31
// Seen in play (2026-11-15): the star IS saved (each save + load showed one more star). So the star's height is a tag
// no real stamp has (the map is 2D, the height is not shown), and after every load / stage change, before placing ours,
// GameData slots holding a tagged star are emptied and the icon list reloaded from GameData (0x7000d89d70).
#define MAPICON_TAG_Y (-7777.25f)
#define A_MAPICON_GET_NO 0x7000d37770ull   // (w0 index, w1 0) -> w0
#define A_MAPICON_GET_POS 0x7000d377d0ull  // (x0 &out Vec3f, w1 index, w2 0)
#define A_MAPICON_SET_NO 0x7000d37790ull   // (w0 value, w1 index, w2 0)
#define A_MAPICON_SET_POS 0x7000d37810ull  // (x0 &Vec3f, w1 index, w2 0)
#define A_MAPICON_RELOAD 0x7000d89d70ull        // (x0 mgr) save-load-only rebuild (orphans live panes mid-game)
#define A_MAPICON_TO_GDT 0x7000d8a034ull        // (x0 mgr) icon list -> all 200 GameData slots (the game's stamp-edit save)
// (2026-11-19, questmark: RE of FUN_7100dba7b4 + FUN_7100d868d0) Calling A_MAPICON_RELOAD mid-game while the minimap
// is live leaves Challenge pool[0]'s Pa_MapIconChallenge_00 UI pane orphaned & frozen on both the minimap and full map
// (a second main-quest yellow circle). Native stamp deletion deletes only our tagged star objects without reloading the
// icon manager: 0x7000d7b7a4(obj, 0) sets bit 3 and calls Stamp::vt[0xc8](0) = 0x7000d81d04 (clears bit 4, unlinks from
// saved list mgr+0x6a0, rewrites all 200 GameData slots via 0x7000d8a034), and 0x7000d8ad10(mgr) refreshes mgr+0x650.
// SAVAGEWIND_COOP_MAPCLEAN=0 reverts to the old A_MAPICON_RELOAD cleanup path.
#define A_MAPICON_DELETE_OBJ 0x7000d7b7a4ull    // (x0 obj, w1 0)
#define A_MAPICON_REFRESH_FREE 0x7000d8ad10ull  // (x0 mgr)


// (59, coop-n3: show map stars too) One star per other player. Each star has a slot: slot 0 = the world
// partner (as before: tag MAPICON_TAG_Y), slot 1 + pid = any other player pid (tag MAPICON_TAG_Y - slot, so a star on
// the map tells whose slot it is). A slot whose player is gone (or in another area) has its star removed cleanly
// (with MAPCLEAN=0: moved off the map to x -100000). The sweep keeps at most one tagged star per shown slot.
// SAVAGEWIND_COOP_MULTISTARS=0: the partner's star only (slot 0, as before).
#define MAPICON_SLOTS (COOP_MAX_PLAYERS + 1)
static uint64_t mapicon_mem;
static struct Star {
    uint64_t obj, mgr;
    int posoff;  // where the object keeps its position (found at run time), -1 unknown
    float last[3];
    uint64_t extra[128];  // tagged stars stacked under this one (legacy MAPCLEAN=0 path)
    int extra_n;
} stars[MAPICON_SLOTS];
static struct Star *S = &stars[0];  // (the slot the calls below work on)
static uint32_t star_shown;         // slots with a live player this frame (bit per slot)
static float star_tag(int slot) { return MAPICON_TAG_Y - (float)slot; }
static int star_slot_of_tag(float y) {  // -1: not one of our tags
    for (int s = 0; s < MAPICON_SLOTS; s++)
        if (y == star_tag(s)) return s;
    return -1;
}
static int star_slot(int pid) { return pid == coop_net_partner_id() ? 0 : 1 + pid; }
static bool multistars_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_MULTISTARS");
    return on;
}
static bool mapclean_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_MAPCLEAN");
    return on;
}

static void mapicon_drop(void) { S->obj = 0, S->mgr = 0, S->posoff = -1, S->extra_n = 0; }
static void mapicon_drop_all(void) {
    for (int s = 0; s < MAPICON_SLOTS; s++) stars[s].obj = 0, stars[s].mgr = 0, stars[s].posoff = -1, stars[s].extra_n = 0;
}

// Does the slot's object still hold the position we gave it? (false: reused, disabled, or freed by a rebuild)
static bool star_still_ours(const struct Star *t) {
    if (!in_guest(t->obj) || t->mgr != LD64(A_MAPICON_MGR)) return false;
    if (mapclean_on() && !(LD32(t->obj + 0x3c) & 0x10)) return false;
    if (t->posoff < 0) return true;  // (offset not found: trust the stage-change drop only)
    const float *p = (const float *)(uintptr_t)(t->obj + (uint64_t)t->posoff);
    return p[0] == t->last[0] && p[2] == t->last[2];
}
static bool mapicon_still_ours(void) { return star_still_ours(S); }
static void mapicon_find_off(void) {  // the first 0x200 bytes of the object, 4-aligned, the last position's floats
    S->posoff = -1;
    for (int off = 0x20; off + 12 <= 0x200; off += 4) {
        const float *p = (const float *)(uintptr_t)(S->obj + (uint64_t)off);
        if (p[0] == S->last[0] && p[1] == S->last[1] && p[2] == S->last[2]) { S->posoff = off; break; }
    }
}

static bool mapicon_is_tagged_star(uint64_t o, int *out_slot) {
    if (!in_guest(o) || !in_guest(LD64(o))) return false;
    if ((int32_t)LD32(o + 0x1c) != MAPICON_STAR) return false;
    bool sent_away = LDF32(o + 0x28) == -100000.0f && LDF32(o + 0x2c) == 0.0f;  // (by the old 22:30 build)
    int slot = star_slot_of_tag(LDF32(o + 0x2c));
    if (slot < 0 && !sent_away) return false;
    if (out_slot) *out_slot = slot;
    return true;
}

// Delete one live stamp object natively using the game's own stamp-delete path (FUN_7100dba7b4):
// 0x7000d7b7a4(o, 0) sets bit 3 (0x8) and calls Stamp::vt[0xc8](0) = 0x7000d81d04 (clears bit 4, sets bit 5,
// unlinks from saved list mgr+0x6a0, rewrites all 200 GameData slots via 0x7000d8a034). On the next UI tick,
// FUN_7100dab87c hides the batched sprite and FUN_7100dada28 unlinks o from shown list mgr+0x620 and detaches
// its pane via vt[0xc0](0) + vt[0xb0](). Never touches Challenge/quest markers or calls A_MAPICON_RELOAD.
static bool mapicon_delete_obj(Cpu *c, uint64_t o) {
    if (!in_guest(o) || !in_guest(LD64(o))) return false;
    Cpu saved = *c;
    c->x[0] = o; c->x[1] = 0;
    guest_call(c, A_MAPICON_DELETE_OBJ);
    *c = saved;
    return true;
}

static void mapicon_refresh_free(Cpu *c, uint64_t mgr) {
    if (!in_guest(mgr)) return;
    Cpu saved = *c;
    c->x[0] = mgr;
    guest_call(c, A_MAPICON_REFRESH_FREE);
    *c = saved;
}

// Delete every enabled tagged player star from the live stamp pool (no A_MAPICON_RELOAD). Returns count deleted.
static int mapicon_clean_live(Cpu *c) {
    uint64_t mgr = LD64(A_MAPICON_MGR);
    if (!in_guest(mgr)) return -1;
    uint64_t arr = LD64(mgr + 0x618);
    int cap = (int)LD32(mgr + 0x610);
    if (!in_guest(arr) || cap <= 0 || cap > 1024) return 0;
    int n = 0;
    for (int i = 0; i < cap; i++) {
        uint64_t o = LD64(arr + 8 * (uint64_t)i);
        if (!in_guest(o) || !(LD32(o + 0x3c) & 0x10)) continue;
        if (!mapicon_is_tagged_star(o, NULL)) continue;
        if (mapicon_delete_obj(c, o)) n++;
    }
    if (n) mapicon_refresh_free(c, mgr);
    return n;
}

// Empty every GameData stamp slot holding a tagged (saved) player star; reload the icons if any. Returns the count.
static int mapicon_clean_saved(Cpu *c) {
    if (!mapicon_mem) mapicon_mem = (uint64_t)(uintptr_t)hle_alloc(0x20);
    uint64_t mgr = LD64(A_MAPICON_MGR);
    if (!in_guest(mgr)) return -1;
    Cpu saved = *c;
    int n = 0;
    float *buf = (float *)(uintptr_t)(mapicon_mem + 0x10);
    for (int i = 0; i < 200; i++) {
        c->x[0] = (uint64_t)i; c->x[1] = 0;
        guest_call(c, A_MAPICON_GET_NO);
        int32_t no = (int32_t)(uint32_t)c->x[0];
        *c = saved;
        if (no != MAPICON_STAR) continue;
        buf[0] = buf[1] = buf[2] = 0;
        c->x[0] = mapicon_mem + 0x10; c->x[1] = (uint64_t)i; c->x[2] = 0;
        guest_call(c, A_MAPICON_GET_POS);
        *c = saved;
        if (star_slot_of_tag(buf[1]) < 0) continue;  // (59: any slot's tag)
        c->x[0] = (uint64_t)(uint32_t)-1; c->x[1] = (uint64_t)i; c->x[2] = 0;
        guest_call(c, A_MAPICON_SET_NO);
        *c = saved;
        buf[0] = -100000.0f, buf[1] = 0, buf[2] = 0;
        c->x[0] = mapicon_mem + 0x10; c->x[1] = (uint64_t)i; c->x[2] = 0;
        guest_call(c, A_MAPICON_SET_POS);
        *c = saved;
        n++;
    }
    if (n) {
        c->x[0] = mgr;
        guest_call(c, A_MAPICON_RELOAD);
        *c = saved;
    }
    return n;
}

// (2026-11-15 play log: after each save "N partner stars removed" grew by one and every new star was the NEXT pool
// object (+0x50 each time): the reload does not free the live icons, the old tagged stars stay on the map.) So the
// live stamp pool (RE: mgr+0x614 count, mgr+0x618 object array; object No at +0x1c, position at +0x28) is swept.
// (2026-11-15 23:00, partner's log + video: her save held 41 stars; the sweep sent 40 to x -100000, and then on her
// open map stars moved along with her cursor and the map scrolled by itself; after the map closed our star had been
// changed and all 41 were back.) A live star at the empty-stamp x is not something the map UI expects (hypothesis). So
// extras are not sent away: they are stacked exactly under a shown star (tagged) and move with it.
static void mapicon_move_obj(Cpu *c, uint64_t o, const float pos[3]) {
    uint64_t vt = LD64(o);
    if (!in_guest(vt) || !in_guest(LD64(vt + 0xb8))) return;
    Cpu saved = *c;
    memcpy((void *)(uintptr_t)(mapicon_mem + 0x10), pos, 12);
    c->x[0] = o; c->x[1] = mapicon_mem + 0x10;
    guest_call(c, LD64(vt + 0xb8));
    *c = saved;
    c->x[0] = o; c->x[1] = 1;
    guest_call(c, A_MAPICON_DIRTY);
    *c = saved;
}
// (59) All slots at once: a tagged star of a shown slot that has none is adopted by it; every other tagged star (a
// second one of a slot, one of a slot not shown now, one sent away by the 22:30 build) is stacked under a shown star
// (its own slot's when that one shows, else the first shown; with none shown, under a hidden one of ours, as the
// partner's star did before 59). Returns how many were moved.
static int mapicon_sweep(Cpu *c) {
    uint64_t mgr = LD64(A_MAPICON_MGR);
    if (!in_guest(mgr)) return 0;
    uint64_t arr = LD64(mgr + 0x618);
    int cap = (int)LD32(mgr + (mapclean_on() ? 0x610 : 0x614));
    if (!in_guest(arr) || cap <= 0 || cap > 1024) return 0;
    if (!mapicon_mem) mapicon_mem = (uint64_t)(uintptr_t)hle_alloc(0x20);
    bool keep[MAPICON_SLOTS];
    for (int s = 0; s < MAPICON_SLOTS; s++) keep[s] = star_still_ours(&stars[s]), stars[s].extra_n = 0;
    bool clean = mapclean_on();
    int moved = 0, removed = 0;
    for (int i = 0; i < cap; i++) {
        uint64_t o = LD64(arr + 8 * (uint64_t)i);
        if (!in_guest(o) || !in_guest(LD64(o))) continue;
        if (clean && !(LD32(o + 0x3c) & 0x10)) continue;  // (disabled pool slot)
        int slot = -1;
        if (!mapicon_is_tagged_star(o, &slot)) continue;
        bool mine = false;
        for (int s = 0; s < MAPICON_SLOTS && !mine; s++) mine = keep[s] && o == stars[s].obj;
        if (mine) continue;
        if (slot >= 0 && !keep[slot] && (star_shown >> slot & 1)) {  // adopt it
            struct Star *t = &stars[slot];
            t->obj = o, t->mgr = mgr, t->posoff = 0x28, keep[slot] = true;
            memcpy(t->last, (const void *)(uintptr_t)(o + 0x28), 12);
            continue;
        }
        if (clean) {
            if (mapicon_delete_obj(c, o)) removed++;
            continue;
        }
        int under = slot >= 0 && keep[slot] && (star_shown >> slot & 1) ? slot : -1;
        for (int s = 0; s < MAPICON_SLOTS && under < 0; s++)
            if (keep[s] && (star_shown >> s & 1)) under = s;
        for (int s = 0; s < MAPICON_SLOTS && under < 0; s++)  // (none shown: under a hidden one, as before 59)
            if (keep[s]) under = s;
        if (under < 0) continue;
        struct Star *t = &stars[under];
        if (t->extra_n >= 128) continue;
        t->extra[t->extra_n++] = o;
        if (LDF32(o + 0x28) != t->last[0] || LDF32(o + 0x2c) != t->last[1] || LDF32(o + 0x30) != t->last[2])
            mapicon_move_obj(c, o, t->last), moved++;
    }
    if (removed) mapicon_refresh_free(c, mgr);
    return clean ? removed : moved;
}

// Place (first time) or move slot S's star to pos (its height replaced by the slot's tag). False: no manager / no
// free icon.
static bool mapicon_put(Cpu *c, const float pos_in[3]) {
    if (!mapicon_mem) mapicon_mem = (uint64_t)(uintptr_t)hle_alloc(0x20);
    if (!mapicon_still_ours() && look_on("SAVAGEWIND_COOP_MAPSWEEP")) {
        int n = mapicon_sweep(c);
        if (n || S->obj) LOG("[coop] mapicon: pool sweep: %d extra star(s) %s%s\n", n,
                             mapclean_on() ? "removed cleanly" : "stacked under a shown one",
                             mapicon_still_ours() ? ", an existing star adopted for this player" : "");
    }
    int slot = (int)(S - stars);
    float pos[3] = {pos_in[0], star_tag(slot), pos_in[2]};
    memcpy((void *)(uintptr_t)mapicon_mem, pos, 12);
    Cpu saved = *c;
    if (!mapicon_still_ours()) {
        uint64_t mgr = LD64(A_MAPICON_MGR);
        if (!in_guest(mgr)) return false;
        uint64_t obj = LD64(mgr + 0x650);
        if (!in_guest(obj) && mapclean_on()) { mapicon_refresh_free(c, mgr); obj = LD64(mgr + 0x650); }
        if (!in_guest(obj)) return false;  // (pool full)
        c->x[0] = mgr; c->x[1] = MAPICON_STAR; c->x[2] = mapicon_mem; c->x[3] = 0;
        guest_call(c, A_MAPICON_PLACE);
        *c = saved;
        S->obj = obj, S->mgr = mgr, S->extra_n = 0;
        memcpy(S->last, pos, 12);
        mapicon_find_off();
        LOG("[coop] mapicon: star placed (slot %d, icon %llx, position at +%#x)\n", slot, (unsigned long long)obj,
            S->posoff);
        return true;
    }
    uint64_t vt = LD64(S->obj);
    if (!in_guest(vt) || !in_guest(LD64(vt + 0xb8))) { mapicon_drop(); return false; }
    c->x[0] = S->obj; c->x[1] = mapicon_mem;
    guest_call(c, LD64(vt + 0xb8));
    *c = saved;
    c->x[0] = S->obj; c->x[1] = 1;
    guest_call(c, A_MAPICON_DIRTY);
    *c = saved;
    memcpy(S->last, pos, 12);
    for (int i = 0; i < S->extra_n; i++)  // (the stacked extras move with it, legacy MAPCLEAN=0 path)
        if (in_guest(S->extra[i]) && (int32_t)LD32(S->extra[i] + 0x1c) == MAPICON_STAR &&
            star_slot_of_tag(LDF32(S->extra[i] + 0x2c)) >= 0)
            mapicon_move_obj(c, S->extra[i], pos);
    return true;
}

static struct { volatile int pending; float pos[3]; int clear; } mapicon_dev;
static void mapicon_frame(Cpu *c) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_MAPICON");
    static const float away[3] = {-100000.0f, 0, 0};
    static int world_frames;  // (frames since we are in the world: the saved-star cleanup waits a second)
    static bool cleaned;      // (the saved-star cleanup ran since we entered the world)
    static int clean_wait;    // (UI ticks to wait after deleting live stars so FUN_7100dada28 detaches their panes)
    S = &stars[0];
    if (!local_in_world()) { mapicon_drop_all(); world_frames = 0; cleaned = false; clean_wait = 0; return; }
    if (world_frames < 60) ++world_frames;
    // (2026-11-18, research for a tester's map crash, not its proven cause: the cleanup below rewrote the icon list even
    // with MAPICON=0 and in an empty lobby. Now it runs only with the feature on, once another player is heard of -
    // old saved stars then go on the first co-op contact instead of at once.)
    if (!cleaned && world_frames >= 60 && on && coop_net_players(10.0)) {
        cleaned = true;
        if (mapclean_on()) {
            // (2026-11-19, questmark) Delete saved/stale tagged stars natively without calling A_MAPICON_RELOAD
            // mid-game (which orphaned Challenge pool[0]'s Pa_MapIconChallenge_00 pane as a frozen second quest ring).
            int n = mapicon_clean_live(c);
            mapicon_drop_all();
            if (n > 0) {
                LOG("[coop] mapicon: %d player star(s) removed from the map cleanly (no reload)\n", n);
                clean_wait = 2;
                return;  // let UI ticks hide + unlink from mgr+0x620 + detach panes before placing fresh stars
            }
        } else {
            uint64_t mgr = LD64(A_MAPICON_MGR);
            if (in_guest(mgr)) {
                Cpu saved = *c;
                c->x[0] = mgr;
                guest_call(c, A_MAPICON_TO_GDT);
                *c = saved;
            }
            int n = mapicon_clean_saved(c);
            mapicon_drop_all();  // (the reload freed every icon: place ours anew)
            if (n) LOG("[coop] mapicon: %d player star(s) removed from the map (placed anew)\n", n);
        }
    }
    if (!cleaned && !mapicon_dev.pending) return;  // (no star placed before the cleanup)
    if (clean_wait > 0 && !mapicon_dev.pending) { --clean_wait; return; }
    if (mapicon_dev.pending) {
        mapicon_dev.pending = 0;
        if (mapicon_dev.clear == 3) {  // dump (RE): Challenge pool, stamp pool, shown & saved lists
            uint64_t mgr = LD64(A_MAPICON_MGR);
            if (!in_guest(mgr)) { LOG("[coop] mapicon (dev): no manager\n"); return; }
            uint64_t carr = LD64(mgr + 0x398);
            int ccap = (int)LD32(mgr + 0x390);
            LOG("[coop] mapicon (dev): challenge count=%d quest=%llx shown_list_n=%d saved_list_n=%d\n", ccap,
                (unsigned long long)LD64(mgr + 0x3a0), (int)LD32(mgr + 0x630), (int)LD32(mgr + 0x6b0));
            for (int i = 0; in_guest(carr) && i < ccap && i < 32; i++) {
                uint64_t o = LD64(carr + 8 * (uint64_t)i);
                if (!in_guest(o)) continue;
                uint64_t pane = LD64(o + 8), ind = LD64(o + 0x50);
                uint32_t fl = LD32(o + 0x3c);
                if (pane || ind || fl)
                    LOG("[coop] mapicon (dev): challenge[%d] %llx pane %llx ind %llx pos %.2f %.2f %.2f +0x3c %08x\n",
                        i, (unsigned long long)o, (unsigned long long)pane, (unsigned long long)ind,
                        LDF32(o + 0x28), LDF32(o + 0x2c), LDF32(o + 0x30), fl);
            }
            for (int off = 0x600; off < 0x6e0; off += 0x10)
                LOG("[coop] mapicon (dev): mgr+%#x: %016llx %016llx\n", off, (unsigned long long)LD64(mgr + off),
                    (unsigned long long)LD64(mgr + off + 8));
            uint64_t arr = LD64(mgr + 0x618);
            int cap = (int)LD32(mgr + 0x614);
            for (int i = 0; in_guest(arr) && i < cap && i < 128; i++) {
                uint64_t o = LD64(arr + 8 * (uint64_t)i);
                if (!in_guest(o)) continue;
                int slot = -1;
                for (int s = 0; s < MAPICON_SLOTS && slot < 0; s++)
                    if (o == stars[s].obj) slot = s;
                if (slot >= 0 || (LD32(o + 0x3c) & 0x11) || (int)LD32(o + 0x1c) == MAPICON_STAR)
                    LOG("[coop] mapicon (dev): pool[%d] %llx pane %llx No %d pos %.2f %.2f %.2f +0x3c %08x slot %d\n", i,
                        (unsigned long long)o, (unsigned long long)LD64(o + 8), (int)LD32(o + 0x1c), LDF32(o + 0x28),
                        LDF32(o + 0x2c), LDF32(o + 0x30), LD32(o + 0x3c), slot);
            }
            return;
        }
        if (mapicon_dev.clear == 2) {  // flush: clean tagged stars (natively with MAPCLEAN, legacy reload if =0)
            if (mapclean_on()) {
                int n = mapicon_clean_live(c);
                mapicon_drop_all();
                if (n > 0) clean_wait = 2;
                LOG("[coop] mapicon (dev): flush -> %d tagged star(s) removed cleanly (no reload)\n", n);
                return;
            }
            uint64_t mgr = LD64(A_MAPICON_MGR);
            Cpu saved = *c;
            if (in_guest(mgr)) { c->x[0] = mgr; guest_call(c, 0x7000d8a034ull); *c = saved; }
            int n = mapicon_clean_saved(c);
            mapicon_drop_all();
            LOG("[coop] mapicon (dev): flush -> %d tagged star(s) found in GameData and removed\n", n);
            return;
        }
        if (mapicon_dev.clear == 1 && mapclean_on()) {
            bool ok = true;
            if (mapicon_still_ours()) {
                ok = mapicon_delete_obj(c, S->obj);
                mapicon_refresh_free(c, LD64(A_MAPICON_MGR));
                mapicon_drop();
            }
            LOG("[coop] mapicon (dev): removed cleanly %s\n", ok ? "ok" : "FAILED");
            return;
        }
        bool ok = mapicon_put(c, mapicon_dev.clear ? away : mapicon_dev.pos);
        LOG("[coop] mapicon (dev): %s %s\n", mapicon_dev.clear ? "moved away" : "star", ok ? "ok" : "FAILED");
        return;
    }
    if (!on) return;
    // Who is shown: the partner (slot 0), and with MULTISTARS every other player heard of (slot 1 + id), in our area.
    float want_pos[MAPICON_SLOTS][3];
    uint32_t shown = 0;
    {
        char area[96];
        uint32_t here = local_area(area, sizeof area);
        int me = coop_net_my_id(), partner = coop_net_partner_id();
        uint32_t heard = multistars_on() ? coop_net_players(10.0) : partner >= 0 ? 1u << partner : 0;
        for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
            if (p == me || !(heard >> p & 1)) continue;
            CoopState rs;
            double age = 99;
            coop_net_view(p == partner ? -1 : p);
            bool have = coop_net_get_remote(&rs, &age) && age < 10.0 && rs.area == here && isfinite(rs.pos[0]) &&
                        isfinite(rs.pos[1]) && isfinite(rs.pos[2]);
            coop_net_view(-1);
            if (!have) continue;
            int s = star_slot(p);
            shown |= 1u << s;
            memcpy(want_pos[s], rs.pos, 12);
        }
    }
    star_shown = shown;
    // (2026-11-15 play: "after a save the marker stops working"; the log showed "star placed" again after each save,
    // with no cleanup: a save rewrites the icons, our object is gone and the saved copy stays as a dead star.) So a lost
    // star while in the world gets the same cleanup as a world entry next frame (at most every 3 s).
    for (int s = 0; s < MAPICON_SLOTS; s++) {
        if (!stars[s].obj || star_still_ours(&stars[s])) continue;
        static uint64_t last_ns;
        if (plat_now_ns() - last_ns > 3000000000ull) {
            last_ns = plat_now_ns();
            mapicon_drop_all();
            cleaned = false;  // (the cleanup runs again next frame)
            LOG("[coop] mapicon: a star of ours was replaced (a save?): cleaning the map and placing them anew\n");
            return;
        }
        break;
    }
    // (2026-11-15 22:42, video from the partner's PC: three stars side by side on her map while ours showed one.) The
    // sweep runs every 5 s while we have a star: extra tagged stars are removed cleanly (or stacked with MAPCLEAN=0).
    bool any = false;
    for (int s = 0; s < MAPICON_SLOTS; s++) any |= star_still_ours(&stars[s]);
    if (any && look_on("SAVAGEWIND_COOP_MAPSWEEP")) {
        static uint64_t sweep_ns;
        if (plat_now_ns() - sweep_ns > 5000000000ull) {
            sweep_ns = plat_now_ns();
            int n = mapicon_sweep(c);
            if (n) LOG("[coop] mapicon: %d extra player star(s) found on the map: %s\n", n,
                       mapclean_on() ? "removed cleanly" : "stacked under a shown one");
        }
    }
    for (int s = 0; s < MAPICON_SLOTS; s++) {
        S = &stars[s];
        bool have = shown >> s & 1;
        bool ours = mapicon_still_ours();
        if (!have && !ours) { mapicon_drop(); continue; }  // (nothing shown, nothing to hide)
        if (!have && ours && mapclean_on()) {
            mapicon_delete_obj(c, S->obj);
            mapicon_refresh_free(c, LD64(A_MAPICON_MGR));
            mapicon_drop();
            LOG("[coop] mapicon: star of slot %d hidden (removed; that player is gone or in another area)\n", s);
            continue;
        }
        const float *want = have ? want_pos[s] : away;
        float dx = want[0] - S->last[0], dz = want[2] - S->last[2];
        if (ours && dx * dx + dz * dz < 0.25f) continue;  // (moved less than half a metre)
        if (!mapicon_put(c, want)) {
            static int said;
            if (said++ < 3) LOG("[coop] mapicon: could not place a star (no map manager or no free stamp)\n");
        } else if (!have) {
            LOG("[coop] mapicon: star of slot %d hidden (that player is gone or in another area)\n", s);
        }
    }
    S = &stars[0];
}
