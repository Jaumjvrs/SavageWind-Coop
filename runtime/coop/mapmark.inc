// Part of runtime/coop.c (included there, same translation unit), after mapicon.inc and looks.inc.
// (2026-11-21: change the map star into an arrow like Link's, in the colour the player picks; markers for
// everyone would be a nice option too)
//
// A. The other players on our map as an ARROW in their tunic colour, pointing where they face (minimap and full map).
//    The star stamp of mapicon.inc stays (it is our anchor: the game keeps its map-local position, already rotated /
//    scrolled / zoomed); its batched picture is hidden and the overlay (ui/menu.mm coop_arrows_draw) draws the arrow
//    at the same spot. RE (live, 2026-11-21):
//    - star object +0x08 wrapper, wrapper +0x10 = the batched picture pane "P_StampIcon_00"; its parent
//      "N_MapStampBtn_00" is the stamps' layout pane. Batch pane +0x1f0 instance count, +0x1f8 -> visibility bytes
//      (0xff shown) indexed by the object's +0x40. Writing 0 hides that one stamp (stays hidden; checked on the map).
//    - object +0x34/+0x38: map-local x/y; layout (1280x720, centre origin, y up) = N_MapStampBtn_00's global matrix
//      (pane +0x70, 3x4 row-major) times (x, y). Minimap: that pane is translated (510,-240) and scaled 0.9; full map:
//      at the origin, scale 1 (scroll and zoom are in the local coordinates).
//    - shown: every ancestor visible (pane +0x58 bit 0) with alpha (+0x59) > 0 (an open inventory hides the root).
//    - rotation: minimap = Link's own arrow pane "N_PlayerRotate_00" (under Pa_MapIcon_00): its screen angle theta_L
//      = atan2(m4, m0) = Link's yaw - camera yaw (seen -25.8 = 64.2 - 90). The partner's = theta_L + (yaw_P - yaw_L).
//      Full map (north up): theta = yaw - 180. Yaw: 0 = +z, 90 = +x (CoopState.yaw).
//    The star's own picture comes back if anything does not check out (pane names, counts): the star is the fallback.
//    SAVAGEWIND_COOP_MAPARROW=0: the old star only.
// B. Shared map markers (protocol 95, COOP_EV_MAPMARK): stamps and pins we place go to the others (Options > Co-op
//    "Shared map markers": Off / Pins only / All, g_settings.coop_sharemarks). The marker pools are polled: a new one
//    is sent (ADD), a shared one that went away is sent as DEL. Received ones are placed with the game's own placing
//    function (a real stamp / pin, saved like ours) and remembered, so they never echo back. Off = we neither send nor
//    take markers. SAVAGEWIND_COOP_SHAREMARKS=0: the same, whatever the setting.
#define A_MAPMARK_PLACE 0x7000d8a960ull  // (x0 mgr, w1 picker index, x2 &Vec3f, w3 0): the stamp UI's place
// picker index: 0..5 -> stamps No 27..32, 6..10 -> pins No 37..41 (moves that colour's pin), 11..13 -> stamps No 33..35

static int mapmark_pick_of(int no) {
    if (no >= 27 && no <= 32) return no - 27;
    if (no >= 37 && no <= 41) return 6 + (no - 37);
    if (no >= 33 && no <= 35) return 11 + (no - 33);
    return -1;
}
static bool mapmark_is_pin(int no) { return no >= 37 && no <= 41; }

// ------------------------------------------------------------------ A: the arrows
static struct {
    volatile int seq;  // odd while written (game thread); the overlay reads between two equal even values
    int n;
    CoopMapArrow a[MAPICON_SLOTS];
} maparrows;

static void maparrows_store(const CoopMapArrow *a, int n) {
    if (!n && !maparrows.n) return;
    __atomic_add_fetch(&maparrows.seq, 1, __ATOMIC_SEQ_CST);
    if (n > 0) memcpy(maparrows.a, a, sizeof(CoopMapArrow) * (size_t)n);
    maparrows.n = n;
    __atomic_add_fetch(&maparrows.seq, 1, __ATOMIC_SEQ_CST);
}
// (any thread: the overlay) out NULL: only how many
int coop_map_arrows(CoopMapArrow *out, int max) {
    for (int tries = 0; tries < 4; tries++) {
        int s0 = __atomic_load_n(&maparrows.seq, __ATOMIC_SEQ_CST);
        if (s0 & 1) continue;
        int n = maparrows.n;
        if (n > max && out) n = max;
        if (out && n > 0) memcpy(out, maparrows.a, sizeof(CoopMapArrow) * (size_t)n);
        if (__atomic_load_n(&maparrows.seq, __ATOMIC_SEQ_CST) == s0) return n;
    }
    return 0;
}

static bool pane_named(uint64_t p, const char *name) {
    if (!in_guest(p) || !in_guest(LD64(p))) return false;
    return !strncmp((const char *)(uintptr_t)(p + 0xb8), name, 24);
}
static const float *pane_mtx(uint64_t p) { return (const float *)(uintptr_t)(p + 0x70); }
// Every ancestor shown? alpha: the product of their alphas (0..1).
static bool pane_shown(uint64_t p, float *alpha) {
    float a = 1;
    for (int d = 0; d < 40 && in_guest(p); d++) {
        if (!(LD8(p + 0x58) & 1) || !LD8(p + 0x59)) return false;
        a *= LD8(p + 0x59) / 255.0f;
        p = LD64(p + 0x18);
    }
    *alpha = a;
    return true;
}
// Depth-first search under `root` for a pane named `name` (bounded).
static uint64_t pane_find(uint64_t root, const char *name, int depth, int *budget) {
    if (!in_guest(root) || --*budget < 0) return 0;
    if (pane_named(root, name)) return root;
    if (depth <= 0) return 0;
    uint64_t head = root + 0x20, node = LD64(root + 0x28);
    for (int k = 0; k < 64 && in_guest(node) && node != head; k++) {
        uint64_t r = pane_find(node - 8, name, depth - 1, budget);
        if (r) return r;
        node = LD64(node + 8);
    }
    return 0;
}
// Link's own minimap arrow pane, found from the stamps' layout pane (cached).
static uint64_t maparrow_link_pane(uint64_t layout) {
    static uint64_t from, found;
    static uint64_t last_try_ns;
    if (from == layout && pane_named(found, "N_PlayerRotate_00")) return found;
    if (from == layout && !found && plat_now_ns() - last_try_ns < 1000000000ull) return 0;
    from = layout, found = 0, last_try_ns = plat_now_ns();
    uint64_t top = layout;
    for (int d = 0; d < 8 && in_guest(top) && !pane_named(top, "Pa_MapIcon_00"); d++) top = LD64(top + 0x18);
    if (!pane_named(top, "Pa_MapIcon_00")) return 0;
    int budget = 400;
    found = pane_find(top, "N_PlayerRotate_00", 8, &budget);
    LOG("[coop] maparrow: Link's map arrow pane %s (%llx)\n", found ? "found" : "NOT found", (unsigned long long)found);
    return found;
}
static bool maparrow_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_MAPARROW");
    return on;
}
static bool mapedge_on(void) {  // (the rim pin for a player off the minimap; SAVAGEWIND_COOP_MAPEDGE=0: off)
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_MAPEDGE");
    return on;
}
static float local_link_yaw(bool *ok) {
    *ok = false;
    uint64_t pi = player_info_ptr();
    uint64_t a = pi ? LD64(pi + 0x60) : 0;
    if (!in_guest(a)) return 0;
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    if (!isfinite(m[0]) || !isfinite(m[2]) || (m[0] == 0 && m[2] == 0)) return 0;
    *ok = true;
    return atan2f(m[2], m[0]) * 180 / 3.14159265f;
}

static unsigned maparrow_logs;
static void maparrow_frame(void) {
    CoopMapArrow out[MAPICON_SLOTS];
    int n = 0;
    if (!maparrow_on() || !local_in_world()) { maparrows_store(out, 0); return; }
    int partner = coop_net_partner_id();
    bool have_ly;
    float yaw_l = local_link_yaw(&have_ly);
    for (int s = 0; s < MAPICON_SLOTS; s++) {
        const struct Star *t = &stars[s];
        if (!(star_shown >> s & 1) || !star_still_ours(t)) continue;
        int pid = s == 0 ? partner : s - 1;
        if (pid < 0) continue;
        // (2026-11-21 test: with the partner far off the minimap the game detaches the star's picture, +0x08 = 0, and
        // stops updating its map-local spot; the stamps' panes stay. So the last panes seen are kept for the rim pin.)
        static uint64_t last_batch;
        uint64_t o = t->obj, w = LD64(o + 8);
        bool detached = !in_guest(w);
        uint64_t batch = detached ? last_batch : LD64(w + 0x10);
        if (!pane_named(batch, "P_StampIcon_00")) continue;
        uint64_t layout = LD64(batch + 0x18);
        if (!pane_named(layout, "N_MapStampBtn_00")) continue;
        if (!detached) last_batch = batch;
        uint32_t idx = LD32(o + 0x40);
        uint64_t cnt = LD64(batch + 0x1f0), vis = LD64(batch + 0x1f8);
        if (cnt == 0 || cnt > 1024 || idx >= cnt || !in_guest(vis)) continue;
        if (!detached) ST8(vis + idx, 0);  // the star's own picture off (the arrow is drawn in its place)
        float alpha;
        if (!pane_shown(batch, &alpha) || alpha < 0.02f) continue;
        float lx = detached ? 0 : LDF32(o + 0x34), ly = detached ? 0 : LDF32(o + 0x38);
        if (!isfinite(lx) || !isfinite(ly)) continue;
        const float *m = pane_mtx(layout);
        float x = m[0] * lx + m[1] * ly + m[3], y = m[4] * lx + m[5] * ly + m[7];
        float sc = sqrtf(m[0] * m[0] + m[4] * m[4]);
        if (!isfinite(x) || !isfinite(y) || !(sc > 0.05f && sc < 10)) continue;
        bool mini = fabsf(m[3]) > 50 || fabsf(m[7]) > 50;
        if (!mini && (detached || fabsf(x) > 640 || fabsf(y) > 340)) continue;
        // the other player's yaw and place
        CoopState rs;
        double age = 99;
        coop_net_view(pid == partner ? -1 : pid);
        bool have = coop_net_get_remote(&rs, &age) && age < 10.0 && isfinite(rs.yaw) && isfinite(rs.pos[0]) &&
                    isfinite(rs.pos[2]);
        coop_net_view(-1);
        float yaw_p = have ? rs.yaw : 180;
        float theta_deg, map_rot = 0;  // map_rot: the minimap's turn (degrees, CCW) from north-up
        bool rim = false;
        if (!mini) theta_deg = yaw_p - 180;
        else {
            uint64_t lp = maparrow_link_pane(layout);
            float eye[3], at[3];
            if (lp && have_ly) {
                const float *lm = pane_mtx(lp);
                float theta_l = atan2f(lm[4], lm[0]) * 180 / 3.14159265f;
                theta_deg = theta_l + (yaw_p - yaw_l);
                map_rot = theta_l - (yaw_l - 180);
            } else if (freecam_game_view(eye, at)) {
                float cam = atan2f(at[0] - eye[0], at[2] - eye[2]) * 180 / 3.14159265f;
                theta_deg = yaw_p - cam;
                map_rot = 180 - cam;
            } else {
                theta_deg = yaw_p - 180;
            }
            // Where they should be on the minimap, from the world: the game's map-local position of the star stops
            // following once it is far off the minimap (2026-11-21 test: stuck at the 25 m spot with the partner
            // 300 m away). Scale (local units per metre) learnt from the star while close, else 0.56 (seen: 25 m = 14).
            float me[3], pvx = 0, pvy = 0, d = 0;
            bool pred = have && player_pos(me);
            if (pred) {
                float dx = rs.pos[0] - me[0], dz = rs.pos[2] - me[2];
                d = sqrtf(dx * dx + dz * dz);
                pred = d > 1e-3f;
                if (pred) {
                    float ux = dx / d, uy = -dz / d;  // (north-up map: right = +x, up = -z)
                    float r = map_rot * 3.14159265f / 180, cs = cosf(r), sn = sinf(r);
                    pvx = ux * cs - uy * sn, pvy = ux * sn + uy * cs;
                }
            }
            static float k_cal;
            float lr = sqrtf(lx * lx + ly * ly);
            if (pred && d > 5 && d < 100 && lr > 2 && lr < 80 && (lx * pvx + ly * pvy) > 0.95f * lr) {
                float kn = lr / d;
                if (kn > 0.05f && kn < 20) k_cal = k_cal > 0 ? 0.9f * k_cal + 0.1f * kn : kn;
            }
            float k = k_cal > 0 ? k_cal : 0.56f;
            if (pred && d * k > 86.0f) {  // (outside the minimap's circle)
                // (2026-11-21: the other player always shows up in the minimap) pinned on the rim, toward them
                if (!mapedge_on()) continue;
                x = m[3] + (m[0] * pvx + m[1] * pvy) * 80, y = m[7] + (m[4] * pvx + m[5] * pvy) * 80;
                rim = true;
            } else if (pred && fabsf(lr - d * k) > 12.0f) {  // (the star's local spot is stale: use ours)
                x = m[3] + (m[0] * pvx + m[1] * pvy) * d * k, y = m[7] + (m[4] * pvx + m[5] * pvy) * d * k;
            } else if (lr > 86.0f || detached) {
                continue;  // (no world position: hide rather than draw it outside the circle)
            }
        }
        CoopMapArrow *g = &out[n++];
        g->x = x, g->y = y, g->angle = theta_deg * 3.14159265f / 180, g->size = 26 * sc * (rim ? 0.8f : 1),
        g->alpha = alpha * (rim ? 0.7f : 1);
        g->tint = coop_net_player_tint(pid);
        static int last_mode[MAPICON_SLOTS];
        int mode = rim ? 3 : mini ? 2 : 1;
        if (mode != last_mode[s] || maparrow_logs % 120 == 0) {
            last_mode[s] = mode;
            LOG("[coop] maparrow: player %d (%s) at layout %.1f %.1f, angle %.1f, size %.1f, tint %d\n", pid,
                rim ? "minimap rim" : mini ? "minimap" : "map", x, y, theta_deg, g->size, g->tint);
        }
        maparrow_logs++;
    }
    maparrows_store(out, n);
}

// ------------------------------------------------------------------ B: shared markers
#define MAPMARK_MAX 128
static struct MarkKnown { int8_t no; bool shared; float pos[3]; } mm_known[MAPMARK_MAX];
static int mm_known_n;
static bool mm_primed;  // (the first poll in the world only records what is there)
static uint64_t mapmark_mem;
static int mm_sent, mm_got;

static bool sharemarks_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_SHAREMARKS");
    return on && g_settings.coop_sharemarks > 0;
}
static bool mm_near(const float a[3], const float b[3]) { return fabsf(a[0] - b[0]) < 0.5f && fabsf(a[2] - b[2]) < 0.5f; }

typedef struct { uint64_t obj; int no; float pos[3]; } MarkNow;
// The enabled markers now: stamps (our player stars left out) and pins. Returns the count.
static int mapmark_list(MarkNow *out, int max) {
    uint64_t mgr = LD64(A_MAPICON_MGR);
    if (!in_guest(mgr)) return -1;
    int n = 0;
    for (int pool = 0; pool < 2; pool++) {
        uint64_t arr = LD64(mgr + (pool ? 0x660 : 0x618));
        int cap = (int)LD32(mgr + (pool ? 0x658 : 0x610));
        if (!in_guest(arr) || cap <= 0 || cap > 1024) continue;
        for (int i = 0; i < cap && n < max; i++) {
            uint64_t o = LD64(arr + 8 * (uint64_t)i);
            if (!in_guest(o) || !in_guest(LD64(o)) || !(LD32(o + 0x3c) & 0x10)) continue;
            int no = (int)LD32(o + 0x1c);
            if (pool) {
                if (!mapmark_is_pin(no)) {
                    uint32_t col = LD32(o + 0x40);
                    if (col > 4) continue;
                    no = 37 + (int)col;
                }
            } else {
                if (no < 27 || no > 35 || mapicon_is_tagged_star(o, NULL)) continue;
            }
            float p[3] = {LDF32(o + 0x28), LDF32(o + 0x2c), LDF32(o + 0x30)};
            if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || p[0] <= -99999.0f) continue;
            out[n].obj = o, out[n].no = no;
            memcpy(out[n].pos, p, 12);
            n++;
        }
    }
    return n;
}
static void mm_known_add(int no, const float pos[3], bool shared) {
    if (mm_known_n >= MAPMARK_MAX) return;
    struct MarkKnown *k = &mm_known[mm_known_n++];
    k->no = (int8_t)no, k->shared = shared;
    memcpy(k->pos, pos, 12);
}
static void mm_known_del(int i) { mm_known[i] = mm_known[--mm_known_n]; }

static bool mapmark_place(Cpu *c, int no, const float pos[3]) {
    int pick = mapmark_pick_of(no);
    uint64_t mgr = LD64(A_MAPICON_MGR);
    if (pick < 0 || !in_guest(mgr)) return false;
    if (!mapmark_is_pin(no)) {
        if (!in_guest(LD64(mgr + 0x650))) mapicon_refresh_free(c, mgr);
        if (!in_guest(LD64(mgr + 0x650))) return false;  // (no free stamp)
    }
    if (!mapmark_mem) mapmark_mem = (uint64_t)(uintptr_t)hle_alloc(0x20);
    memcpy((void *)(uintptr_t)mapmark_mem, pos, 12);
    Cpu saved = *c;
    c->x[0] = mgr; c->x[1] = (uint64_t)pick; c->x[2] = mapmark_mem; c->x[3] = 0;
    guest_call(c, A_MAPMARK_PLACE);
    *c = saved;
    return true;
}
static bool mapmark_remove(Cpu *c, int no, const float pos[3]) {
    MarkNow now[256];
    int n = mapmark_list(now, 256);
    for (int i = 0; i < n; i++) {
        if (now[i].no != no || !mm_near(now[i].pos, pos)) continue;
        mapicon_delete_obj(c, now[i].obj);
        if (!mapmark_is_pin(no)) mapicon_refresh_free(c, LD64(A_MAPICON_MGR));
        return true;
    }
    return false;
}

static void mapmark_send(int what, int no, const float pos[3]) {
    CoopMapMark e = {0};
    e.what = (uint8_t)what, e.no = (uint8_t)no;
    memcpy(e.pos, pos, 12);
    if (coop_net_send_event(COOP_EV_MAPMARK, &e, sizeof e)) mm_sent++;
}

// (gate.inc's event dispatch, game thread, in the world)
static void mapmark_ev(Cpu *c, const uint8_t *data, int len) {
    if (len != (int)sizeof(CoopMapMark)) return;
    CoopMapMark e;
    memcpy(&e, data, sizeof e);
    if (!coop_mapmark_ok(&e)) { LOG("[coop] mapmark: a bad marker event dropped\n"); return; }
    if (!sharemarks_on()) { LOG("[coop] mapmark: marker event ignored (shared markers off)\n"); return; }
    mm_got++;
    if (e.what == COOP_MAPMARK_ADD) {
        for (int i = 0; i < mm_known_n; i++)
            if (mm_known[i].no == e.no && mm_near(mm_known[i].pos, e.pos)) return;  // (have it already)
        if (mapmark_is_pin(e.no))  // (placing moves our pin of that colour: forget the old spot without a DEL)
            for (int i = mm_known_n - 1; i >= 0; i--)
                if (mm_known[i].no == e.no) mm_known_del(i);
        bool ok = mapmark_place(c, e.no, e.pos);
        if (ok) mm_known_add(e.no, e.pos, true);
        LOG("[coop] mapmark: got marker No %d at %.1f %.1f %.1f: %s\n", e.no, e.pos[0], e.pos[1], e.pos[2],
            ok ? "placed" : "NOT placed (no map / no free stamp)");
    } else {
        for (int i = mm_known_n - 1; i >= 0; i--)
            if (mm_known[i].no == e.no && mm_near(mm_known[i].pos, e.pos)) mm_known_del(i);
        bool ok = mapmark_remove(c, e.no, e.pos);
        LOG("[coop] mapmark: marker No %d at %.1f %.1f removed by the other player: %s\n", e.no, e.pos[0], e.pos[2],
            ok ? "removed here" : "not here");
    }
}

static struct { volatile int pending; int op; int no; float pos[3]; } mapmark_dev;  // op 1 dump, 2 place, 3 del, 4 policy
static void mapmark_poll(Cpu *c) {
    MarkNow now[256];
    int n = mapmark_list(now, 256);
    if (n < 0) return;
    if (!mm_primed) {
        mm_known_n = 0;
        for (int i = 0; i < n; i++) mm_known_add(now[i].no, now[i].pos, false);
        mm_primed = true;
        LOG("[coop] mapmark: %d marker(s) on our map (not sent: they were there before)\n", n);
        return;
    }
    int policy = sharemarks_on() ? g_settings.coop_sharemarks : 0;
    bool peers = coop_net_players(10.0) != 0;
    // gone first (a moved pin = its old spot gone, then the new one)
    bool seen[MAPMARK_MAX] = {0};
    for (int i = 0; i < n; i++)
        for (int k = 0; k < mm_known_n; k++)
            if (!seen[k] && mm_known[k].no == now[i].no && mm_near(mm_known[k].pos, now[i].pos)) { seen[k] = true; break; }
    for (int k = mm_known_n - 1; k >= 0; k--) {
        if (seen[k]) continue;
        if (mm_known[k].shared && policy && peers) {
            mapmark_send(COOP_MAPMARK_DEL, mm_known[k].no, mm_known[k].pos);
            LOG("[coop] mapmark: shared marker No %d at %.1f %.1f removed: sent\n", mm_known[k].no, mm_known[k].pos[0],
                mm_known[k].pos[2]);
        }
        seen[k] = seen[mm_known_n - 1];
        mm_known_del(k);
    }
    for (int i = 0; i < n; i++) {
        bool known = false;
        for (int k = 0; k < mm_known_n && !known; k++)
            known = mm_known[k].no == now[i].no && mm_near(mm_known[k].pos, now[i].pos);
        if (known) continue;
        bool share = peers && (policy == 2 || (policy == 1 && mapmark_is_pin(now[i].no)));
        if (share) mapmark_send(COOP_MAPMARK_ADD, now[i].no, now[i].pos);
        mm_known_add(now[i].no, now[i].pos, share);
        LOG("[coop] mapmark: new marker No %d at %.1f %.1f %.1f: %s\n", now[i].no, now[i].pos[0], now[i].pos[1],
            now[i].pos[2], share ? "sent to the others" : policy ? "kept (not shared: setting)" : "kept (sharing off)");
    }
}

static void mapmark_frame(Cpu *c) {
    maparrow_frame();
    if (!local_in_world()) { mm_primed = false; mm_known_n = 0; return; }
    if (mapmark_dev.pending) {
        mapmark_dev.pending = 0;
        if (mapmark_dev.op == 1) {
            MarkNow now[256];
            int n = mapmark_list(now, 256);
            LOG("[coop] mapmark (dev): %d marker(s), %d known, sent %d, got %d, setting %d\n", n, mm_known_n, mm_sent,
                mm_got, g_settings.coop_sharemarks);
            for (int i = 0; i < n; i++)
                LOG("[coop] mapmark (dev): marker No %d at %.2f %.2f %.2f obj %llx\n", now[i].no, now[i].pos[0],
                    now[i].pos[1], now[i].pos[2], (unsigned long long)now[i].obj);
        } else if (mapmark_dev.op == 2) {
            bool ok = mapmark_place(c, mapmark_dev.no, mapmark_dev.pos);
            LOG("[coop] mapmark (dev): place No %d: %s\n", mapmark_dev.no, ok ? "ok" : "FAILED");
        } else if (mapmark_dev.op == 3) {
            bool ok = mapmark_remove(c, mapmark_dev.no, mapmark_dev.pos);
            LOG("[coop] mapmark (dev): remove No %d: %s\n", mapmark_dev.no, ok ? "ok" : "not found");
        } else if (mapmark_dev.op == 4) {
            g_settings.coop_sharemarks = mapmark_dev.no;
            LOG("[coop] mapmark (dev): setting -> %d\n", mapmark_dev.no);
        }
    }
    static int tick;
    if (++tick % 6) return;
    mapmark_poll(c);
}
