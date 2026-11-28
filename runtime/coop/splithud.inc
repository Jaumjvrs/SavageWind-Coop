// Part of runtime/coop.c (included there, same translation unit).
// (split map, 2026-11-27) With three split-screen players the empty quarter shows a map with everyone's dot, hearts and
// stamina (runtime/ui/coop_banner.mm). Each game writes its own player's line into the window's shared block
// (runtime/coop_split_hud.h) a few times a second; player 1 also writes its Map zoom choice. Read from the game's
// memory (found 2026-11-27, local/probe_hud*.sh): life = player actor +0x848 (s32, quarter hearts), PlayerInfo +0xa0
// max life (float, quarter hearts; stays when the life drops), +0xa4 stamina, +0xa8 max stamina (floats, 1000 a wheel).
// Player 1 also writes the map's stamps and pins (runtime/coop/mapmark.inc mapmark_list).
// SAVAGEWIND_COOP_SPLITMAP=0: nothing written (the banner then shows only the picture).
// (map fog, 2026-11-27) Each line also carries which Sheikah Towers that player has activated (bool flags
// MapTower_01..15, bit n = MapTower_<n+1>); the banner draws the regions nobody has revealed in the game's blueprint
// look. SAVAGEWIND_COOP_MAPFOG=0: not sent (the banner then shows the whole map).
#define HUD_LIFE 0x848
#define HUD_PI_LIFE_MAX 0xa0
#define HUD_PI_STAMINA 0xa4
#define HUD_PI_STAMINA_MAX 0xa8

static int hud_where(void) {
    uint64_t t = LD64(A_MAP_TYPE + 8), n = LD64(A_MAP_NAME + 8);
    const char *type = in_guest(t) ? (const char *)(uintptr_t)t : "", *name = in_guest(n) ? (const char *)(uintptr_t)n : "";
    if (!strcmp(type, "MainField")) return SPLIT_WHERE_FIELD;
    if (!strcmp(type, "CDungeon")) return SPLIT_WHERE_SHRINE;
    if (!strncmp(name, "Remains", 7)) return SPLIT_WHERE_BEAST;
    return type[0] ? SPLIT_WHERE_OTHER : SPLIT_WHERE_NONE;
}

// The activated towers as bits (SPLIT_TOWERS_KNOWN set), read once a second in one pass over the bool flags; the last
// value meanwhile and while loading (0: not read yet)
static uint32_t hud_towers(uint64_t now) {
    static uint32_t crc[15], last;
    static uint64_t next_ns;
    static int on = -1;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_MAPFOG");
        for (int t = 0; t < 15; t++) {
            char n[16];
            snprintf(n, sizeof n, "MapTower_%02d", t + 1);
            crc[t] = crc_of(n);
        }
    }
    if (!on || now < next_ns || !local_in_world()) return on ? last : 0;
    next_ns = now + 1000000000ull;
    uint64_t mgr = LD64(A_GDT_MGR), tp = in_guest(mgr) ? LD64(mgr + 0xc00) : 0;
    if (!in_guest(tp)) return last;
    uint32_t count = LD32(tp + 0x08), bits = 0, found = 0;
    uint64_t ptrs = LD64(tp + 0x10);
    for (uint32_t i = 0; i < count && i < FLAG_MAX && in_guest(ptrs); i++) {
        uint64_t f = LD64(ptrs + 8ull * i);
        if (!in_guest(f)) continue;
        uint32_t k = LD32(f + 0xc);
        for (int t = 0; t < 15; t++)
            if (k == crc[t]) {
                found |= 1u << t;
                if (LD8(f + 0xb) & 1) bits |= 1u << t;
            }
    }
    if (found != SPLIT_TOWERS_ALL) return last;  // (the flag store not complete: keep the last)
    if ((bits | SPLIT_TOWERS_KNOWN) != last) LOG("[coop] split map: towers %04x\n", bits);
    return last = bits | SPLIT_TOWERS_KNOWN;
}

static void splithud_frame(void) {
    static int on = -1;
    static uint64_t next_ns;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_SPLITMAP") && !strcmp(getenv("SAVAGEWIND_COOP_SPLITMAP"), "0"));
    uint64_t now = plat_now_ns();
    if (!on || now < next_ns) return;
    next_ns = now + 50000000ull;  // (20 a second; the banner glides the dots between lines)
    int slot = coop_split_hud_slot();
    SplitHud *hud = slot >= 0 && slot < SPLIT_HUD_PLAYERS && (slot || coop_split_count()) ? coop_split_hud() : NULL;
    if (!hud) return;
    if (!slot) hud->zoom = g_settings.split_map_zoom;
    SplitHudPlayer v;
    memset(&v, 0, sizeof v);
    const char *env_name = getenv("SAVAGEWIND_COOP_NAME");
    snprintf(v.name, sizeof v.name, "%s", env_name && *env_name ? env_name : g_settings.coop_name[0] ? g_settings.coop_name : "Player 1");
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    if (pinfo && in_guest(pa) && local_in_world()) {
        memcpy(v.pos, (void *)(uintptr_t)(pinfo + 0x88), 12);
        float m[12];
        memcpy(m, (void *)(uintptr_t)(pa + ACTOR_MTX), sizeof m);
        v.fwd[0] = m[2], v.fwd[1] = m[10];
        v.life = (float)(int32_t)LD32(pa + HUD_LIFE);
        v.life_max = LDF32(pinfo + HUD_PI_LIFE_MAX);
        v.stamina = LDF32(pinfo + HUD_PI_STAMINA);
        v.stamina_max = LDF32(pinfo + HUD_PI_STAMINA_MAX);
        const float check[] = {v.pos[0], v.pos[1], v.pos[2], v.fwd[0], v.fwd[1], v.life, v.life_max, v.stamina, v.stamina_max};
        bool sane = true;
        for (size_t i = 0; i < sizeof check / sizeof *check; i++) sane = sane && isfinite(check[i]);
        v.where = sane && v.life_max > 0 && v.life_max <= 4 * 40 + 4 ? hud_where() : SPLIT_WHERE_NONE;
    }
    v.towers = hud_towers(now);
    SplitHudPlayer *p = &hud->p[slot];
    uint32_t seq = p->seq;
    __atomic_store_n(&p->seq, seq | 1, __ATOMIC_RELEASE);  // (odd: being written)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy((char *)p + sizeof p->seq, (char *)&v + sizeof v.seq, sizeof v - sizeof v.seq);
    p->tick_ms = (uint32_t)(now / 1000000ull);  // (informative: the banner times a line out by its own clock)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&p->seq, (seq | 1) + 1, __ATOMIC_RELEASE);
    // player 1: the stamps and pins on its map (with shared markers on, everyone's), read only on the overworld (the
    // marker pools are the overworld map's); the banner keeps the last list meanwhile
    if (slot || v.where != SPLIT_WHERE_FIELD) return;
    MarkNow now_marks[SPLIT_HUD_MARKS];
    int n = mapmark_list(now_marks, SPLIT_HUD_MARKS);
    if (n < 0) return;
    uint32_t ms = hud->mark_seq;
    __atomic_store_n(&hud->mark_seq, ms | 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int i = 0; i < n; i++)
        hud->marks[i].x = now_marks[i].pos[0], hud->marks[i].z = now_marks[i].pos[2], hud->marks[i].no = now_marks[i].no;
    hud->mark_n = n;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&hud->mark_seq, (ms | 1) + 1, __ATOMIC_RELEASE);
}
