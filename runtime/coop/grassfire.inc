// Part of runtime/coop.c (included there, same translation unit): burning grass. Grass set on fire in one game starts
// burning at the same spot in the others; from there each game's fire spreads by itself. (2026-11-19, worker fire;
// protocol 83, reliable event 28 COOP_EV_GRASSFIRE; audit item S2 part 3)
//
// Research (Ghidra 1.9.0, 2026-11-19):
// - Grass fire is not actors: the chemical system keeps a grid of 1.5 m terrain cells around the player (128 x 128
//   pointer slots at mgr+8, origin cell mgr+0x22c40/44, ring offset +0x22c48/4c, active cells: count +0x20008, capacity
//   +0x2000c, array +0x20010). mgr = *(*(*(ChemicalMgr+0x360)+0x138)+0x30), ChemicalMgr = *0x7101e57bd8.
// - A cell: +8 flags (bit 0 live, 0x08 burning, 0x10 / 0x20 cannot burn (wet / no grass; also set when burnt out)),
//   +0xc/+0x10/+0x14 its centre on the ground, +0x18/+0x1c absolute cell, +0x30 fire level (1 = burning, 2 = burnt).
//   Its update FUN_71013a16d8 makes a cell at level >= 1 burn (effects, damage sensors "grass_fire",
//   "GrassFireWarm" updraft) and heat its neighbours: that is the spread.
// - A burning chemical shape (fire arrow, torch, campfire...) lights grass through FUN_71013a231c: for each cell in
//   its radius it creates the cell (FUN_71013a2648(mgr, rel cell)) and heats it (FUN_71013a27a8 ->
//   FUN_71013a3800(heat, cell, flag): level += heat, at most 1; refused for a cell that cannot burn).
// What is shared: a cell that starts burning with no burning / burnt cell of ours (or one the other player sent) within
// GF_NEAR cells in the last GF_KEEP_NS is a new fire ("seed"), sent with its position; one next to fire is spread and
// is not sent. The receiver creates and heats the 3 x 3 cells there with the same two calls (retried for GF_WAIT_NS
// while that spot is outside its grid / not yet loaded); the cells it lights count as fire for its own seed check, so
// nothing echoes back. Damage: each game's grass fire hurts only that game's own Link (the other player's avatar is a
// puppet), so a player is hurt once, by their own copy of the fire.
// SAVAGEWIND_COOP_GRASSFIRE=0: off. Dev: `coop grassfire` logs the grid and the cell under Link; `coop grassfire
// light [dx dz]` sets the grass at Link + (dx, dz) metres (default 4 0) on fire here as a player would (it is sent).
#define A_CHM_MGR 0x7001e57bd8ull        // ChemicalMgr* (Ghidra DAT_7101e57bd8)
#define A_GF_TOUCH 0x70013a2648ull       // (mgr, const u32 rel[2]): create / refresh the cell
#define A_GF_CELL 0x70013a1648ull        // (mgr, const u32 rel[2]) -> cell or 0
#define A_GF_HEAT 0x70013a3800ull        // (s0 heat, cell, w1 flag): fire level += heat (max 1)
#define A_GF_FIELD_OK 0x7001491f40ull    // (*(mgr+0x22c28)) -> bit 0: the field may burn now
#define GF_CELL_M 1.5f
#define GF_REC 8192                      // (power of two)
#define GF_PEND 16
#define GF_NEAR 3                        // cells: a new fire this close to one we know is spread, not a new one
#define GF_KEEP_NS 30000000000ull        // how long a burning cell counts as known fire
#define GF_SCAN_NS 100000000ull
#define GF_WAIT_NS 6000000000ull         // a received fire not lit here yet is retried this long
#define GF_SEND_MAX 4                    // new fires sent per second at most

static bool grassfire_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_GRASSFIRE");
    return on;
}
// Cells we know burned (ours and theirs): a hash set of absolute cells with the time first seen burning (expired
// entries are reused on insert and skipped on lookup).
static struct { int32_t gx, gz; uint64_t t; } gf_rec[GF_REC];
static struct { float pos[3]; uint32_t area; uint64_t until_ns, try_ns; int tries; } gf_pend[GF_PEND];
static struct { volatile int pending; int mode; float dx, dz; } gf_dev;  // mode 0 status, 1 light
static uint64_t gf_buf;  // guest scratch: the rel cell argument

static uint64_t gf_mgr(void) {
    uint64_t chm = LD64(A_CHM_MGR);
    if (!in_guest(chm) || !(LD8(chm + 0xa58) & 2)) return 0;
    uint64_t a = LD64(chm + 0x360);
    if (!in_guest(a)) return 0;
    uint64_t h = LD64(a + 0x138);
    if (!in_guest(h)) return 0;
    uint64_t m = LD64(h + 0x30);
    if (!in_guest(m) || !in_guest(m + 0x22c50) || !in_guest(LD64(m + 0x22c28)) || !in_guest(LD64(m + 0x22c30)))
        return 0;
    return m;
}
static int32_t gf_cell_of(float v) { return (int32_t)floorf(v / GF_CELL_M); }
static uint32_t gf_slot(int32_t gx, int32_t gz) {
    return ((uint32_t)gx * 73856093u ^ (uint32_t)gz * 19349663u) & (GF_REC - 1);
}
// When this cell was first seen burning (0: not known / expired).
static uint64_t gf_seen(int32_t gx, int32_t gz, uint64_t now) {
    for (uint32_t i = gf_slot(gx, gz), k = 0; k < GF_REC; i = (i + 1) & (GF_REC - 1), k++) {
        if (!gf_rec[i].t) return 0;  // (never used: end of the chain)
        if (gf_rec[i].gx == gx && gf_rec[i].gz == gz) return now - gf_rec[i].t <= GF_KEEP_NS ? gf_rec[i].t : 0;
    }
    return 0;
}
static void gf_remember(int32_t gx, int32_t gz, uint64_t now) {
    uint32_t free_at = GF_REC;
    for (uint32_t i = gf_slot(gx, gz), k = 0; k < GF_REC; i = (i + 1) & (GF_REC - 1), k++) {
        if (gf_rec[i].t && gf_rec[i].gx == gx && gf_rec[i].gz == gz) { gf_rec[i].t = now; return; }
        if (free_at == GF_REC && (!gf_rec[i].t || now - gf_rec[i].t > GF_KEEP_NS)) free_at = i;
        if (!gf_rec[i].t) break;
    }
    if (free_at == GF_REC) return;  // (full of live fire: then nothing more is sent, which is the safe side)
    gf_rec[free_at].gx = gx, gf_rec[free_at].gz = gz, gf_rec[free_at].t = now;
}
// Known fire within `near` cells (the cell itself included)?
static bool gf_known(int32_t gx, int32_t gz, int near, uint64_t now) {
    for (int dx = -near; dx <= near; dx++)
        for (int dz = -near; dz <= near; dz++)
            if (gf_seen(gx + dx, gz + dz, now)) return true;
    return false;
}
static bool gf_field_ok(Cpu *c, uint64_t m) {
    Cpu saved = *c;
    c->x[0] = LD64(m + 0x22c28);
    guest_call(c, A_GF_FIELD_OK);
    bool ok = c->x[0] & 1;
    *c = saved;
    return ok;
}
// Create + heat the 3 x 3 cells around pos with the game's own calls. Returns how many burn now (-1: outside the grid).
static int gf_light(Cpu *c, uint64_t m, const float pos[3], uint64_t now, bool remember) {
    if (!gf_buf) gf_buf = (uint64_t)(uintptr_t)hle_alloc(0x10);
    if (!gf_buf) return -1;
    int32_t cx = gf_cell_of(pos[0]), cz = gf_cell_of(pos[2]);
    int32_t ox = (int32_t)LD32(m + 0x22c40), oz = (int32_t)LD32(m + 0x22c44);
    int burning = 0, inside = 0;
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) {
            uint32_t rx = (uint32_t)(cx + dx - ox), rz = (uint32_t)(cz + dz - oz);
            if (rx >= 0x80 || rz >= 0x80) continue;
            inside++;
            ST32(gf_buf, rx), ST32(gf_buf + 4, rz);
            Cpu saved = *c;
            c->x[0] = m, c->x[1] = gf_buf;
            guest_call(c, A_GF_TOUCH);
            *c = saved;
            c->x[0] = m, c->x[1] = gf_buf;
            guest_call(c, A_GF_CELL);
            uint64_t cell = c->x[0];
            *c = saved;
            if (!in_guest(cell) || !(LD8(cell + 8) & 1)) continue;
            if (remember) gf_remember(cx + dx, cz + dz, now);
            float heat = 1.0f;
            ST8(cell + 8, LD8(cell + 8) | 2);  // (heated this frame: as FUN_71013a27a8 does)
            if (*(float *)(uintptr_t)(cell + 0x30) < 0) *(float *)(uintptr_t)(cell + 0x30) = 0;
            memset(&c->v[0], 0, 16);
            memcpy(&c->v[0], &heat, 4);
            c->x[0] = cell, c->x[1] = 0;
            guest_call(c, A_GF_HEAT);
            *c = saved;
            if (*(float *)(uintptr_t)(cell + 0x30) >= 1.0f) burning++;
        }
    return inside ? burning : -1;
}
static void gf_send(const float pos[3], uint32_t area) {
    CoopGrassFire e;
    memset(&e, 0, sizeof e);
    e.area = area;
    memcpy(e.pos, pos, 12);
    bool ok = coop_grassfire_ok(&e) && coop_net_send_event(COOP_EV_GRASSFIRE, &e, sizeof e);
    LOG("[coop] grassfire: grass caught fire here at %.1f %.1f %.1f: %s\n", pos[0], pos[1], pos[2],
        ok ? "sent" : "NOT sent (no peer / queue full)");
}
static void gf_dev_run(Cpu *c, uint64_t m, uint64_t now) {
    if (!gf_dev.pending) return;
    gf_dev.pending = 0;
    float p[3];
    if (!player_pos(p)) return;
    if (!m) { LOG("[coop] grassfire: dev: no grass fire grid here\n"); return; }
    int32_t ox = (int32_t)LD32(m + 0x22c40), oz = (int32_t)LD32(m + 0x22c44);
    if (gf_dev.mode == 0) {
        int n = (int)LD32(m + 0x20008), burning = 0;
        uint64_t arr = LD64(m + 0x20010);
        for (int i = 0; i < n && in_guest(arr); i++) {
            uint64_t cell = LD64(arr + 8ull * i);
            if (in_guest(cell) && *(float *)(uintptr_t)(cell + 0x30) >= 1.0f) burning++;
        }
        LOG("[coop] grassfire: grid origin %d %d, %d live cells (%d burning / burnt), field %s; Link at cell %d %d\n",
            ox, oz, n, burning, gf_field_ok(c, m) ? "may burn" : "may NOT burn", gf_cell_of(p[0]), gf_cell_of(p[2]));
        return;
    }
    float at[3] = {p[0] + gf_dev.dx, p[1], p[2] + gf_dev.dz};
    int k = gf_light(c, m, at, now, false);
    LOG("[coop] grassfire: dev: light grass at %.1f %.1f %.1f: %d of 9 cells burning%s\n", at[0], at[1], at[2], k,
        k < 0 ? " (outside the grid)" : k == 0 ? " (no grass / wet / field may not burn)" : "");
}
static void grassfire_frame(Cpu *c) {
    static uint64_t next_ns, sec_ns;
    static int sent_sec;
    static uint32_t last_area;
    if (!grassfire_on()) return;
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (!local_in_world() || !player_info_ptr()) return;
    if (here != last_area) {
        memset(gf_rec, 0, sizeof gf_rec);
        last_area = here;
    }
    if (now < next_ns && !gf_dev.pending) return;
    next_ns = now + GF_SCAN_NS;
    if (now - sec_ns > 1000000000ull) sec_ns = now, sent_sec = 0;
    uint64_t m = gf_mgr();
    gf_dev_run(c, m, now);
    if (!m) return;
    // (1.) New fires here: cells burning now that no known fire is next to.
    int n = (int)LD32(m + 0x20008), cap = (int)LD32(m + 0x2000c);
    uint64_t arr = LD64(m + 0x20010);
    if (n > 0 && n <= cap && in_guest(arr)) {
        for (int i = 0; i < n; i++) {
            uint64_t cell = LD64(arr + 8ull * i);
            if (!in_guest(cell) || !(LD8(cell + 8) & 1)) continue;
            float lv = *(float *)(uintptr_t)(cell + 0x30);
            if (!(lv >= 1.0f && lv < 2.0f)) continue;
            int32_t gx = (int32_t)LD32(cell + 0x18), gz = (int32_t)LD32(cell + 0x1c);
            if (gf_seen(gx, gz, now)) continue;  // (burning since an earlier scan)
            bool known = gf_known(gx, gz, GF_NEAR, now);
            gf_remember(gx, gz, now);
            if (known) continue;
            float pos[3];
            memcpy(pos, (void *)(uintptr_t)(cell + 0xc), 12);
            if (sent_sec < GF_SEND_MAX) sent_sec++, gf_send(pos, here);
        }
    }
    // (2.) The other player's new fires: light them here.
    for (int k = 0; k < GF_PEND; k++) {
        if (!gf_pend[k].until_ns) continue;
        if (now > gf_pend[k].until_ns) {
            LOG("[coop] grassfire: the other player's fire at %.1f %.1f %.1f did not catch here (%d tries); dropped\n",
                gf_pend[k].pos[0], gf_pend[k].pos[1], gf_pend[k].pos[2], gf_pend[k].tries);
            gf_pend[k].until_ns = 0;
            continue;
        }
        if (gf_pend[k].area != here || now - gf_pend[k].try_ns < 500000000ull) continue;
        gf_pend[k].try_ns = now, gf_pend[k].tries++;
        int b = gf_light(c, m, gf_pend[k].pos, now, true);
        if (b > 0) {
            LOG("[coop] grassfire: the other player's fire at %.1f %.1f %.1f burns here too (%d cells)\n",
                gf_pend[k].pos[0], gf_pend[k].pos[1], gf_pend[k].pos[2], b);
            gf_pend[k].until_ns = 0;
        }
    }
}
// EV 28 from the other game (gate.inc world_event, game thread).
static void grassfire_ev(Cpu *c, const uint8_t *data, int len) {
    (void)c;
    if (!grassfire_on() || len != (int)sizeof(CoopGrassFire)) return;
    CoopGrassFire e;
    memcpy(&e, data, sizeof e);
    if (!coop_grassfire_ok(&e)) { LOG("[coop] grassfire: bad event dropped\n"); return; }
    uint64_t now = plat_now_ns();
    // Known fire there already (both games lit it, e.g. from a shared campfire): nothing to do.
    int32_t gx = gf_cell_of(e.pos[0]), gz = gf_cell_of(e.pos[2]);
    bool known = gf_known(gx, gz, 1, now);
    LOG("[coop] grassfire: the other player's grass caught fire at %.1f %.1f %.1f%s\n", e.pos[0], e.pos[1], e.pos[2],
        known ? " (burning here already)" : "");
    if (known) return;
    gf_remember(gx, gz, now);  // (what we light there is theirs: not sent back)
    int k = 0;
    for (int i = 0; i < GF_PEND; i++) {
        if (!gf_pend[i].until_ns) { k = i; break; }
        if (gf_pend[i].until_ns < gf_pend[k].until_ns) k = i;
    }
    memcpy(gf_pend[k].pos, e.pos, 12);
    gf_pend[k].area = e.area, gf_pend[k].until_ns = now + GF_WAIT_NS, gf_pend[k].try_ns = 0, gf_pend[k].tries = 0;
}
