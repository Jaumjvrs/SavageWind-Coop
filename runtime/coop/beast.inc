// Part of runtime/coop.c (included there, same translation unit): the Divine Beasts (2026-11-12).
// A beast's interior is its own area "MainFieldDungeon/Remains<Water|Fire|Wind|Electric>". Its per-object flags are
// "MainFieldDungeon_<actor>_<HashId>" (gate.inc builds them; the 5 terminals FldObj_Terminal_A_01 are shared there like
// chests), and any flag from a partner inside the same beast is taken at any distance (gate.inc whole_beast).
// The terminal count: each terminal's event (Demo044_0 Access00..04) turns its own bool on and lowers the s32
// Counter_Terminal<Beast> (5 at the start); at 0 the main control unit opens (Access_AllTerminal<Beast>, shared as a
// named flag). A terminal the other player activated only reaches us as its bool, so here the counter is put back to
// "5 - terminals on" whenever it disagrees for 2 s and no event runs (an event may be between the bool and the
// counter). The terminal events themselves are not replayed for the partner (story.inc: they would count twice).
// SAVAGEWIND_COOP_BEAST=0 turns the beast parts off (flag prefix fix stays: it is just the right name).
static bool beast_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_BEAST") && !strcmp(getenv("SAVAGEWIND_COOP_BEAST"), "0"));
    return on;
}
static const struct { const char *name; uint32_t term[5]; } beast_tab[] = {
    {"Water", {1474232055u, 2841367266u, 969080608u, 3878009531u, 2418182701u}},
    {"Fire", {2189178038u, 830334165u, 114051416u, 2028699826u, 2019624366u}},
    {"Wind", {346006798u, 3082533551u, 646222239u, 260772032u, 2384676482u}},
    {"Electric", {2032252909u, 2536494785u, 1770834567u, 1283068278u, 1285400923u}},
};
// index into beast_tab of the beast we are inside, or -1
static int beast_here(void) {
    char area[96];
    local_area(area, sizeof area);
    if (strncmp(area, "MainFieldDungeon/Remains", 24)) return -1;
    for (int b = 0; b < 4; b++)
        if (!strcmp(area + 24, beast_tab[b].name)) return b;
    return -1;
}
static bool event_running(void) {
    uint64_t mgr = g_site.event_mgr ? LD64(g_site.event_mgr) : 0;
    return in_guest(mgr) && in_guest(LD64(mgr + 0x1d2b8));
}

// A bool flag's value by name (-1: no such flag). Same store as world_flags: mgr+0xc00, bools count +0x08, ptrs +0x10.
static int gdt_get_bool(const char *name) {
    uint64_t mgr = LD64(A_GDT_MGR), tp = in_guest(mgr) ? LD64(mgr + 0xc00) : 0;
    if (!in_guest(tp)) return -1;
    uint32_t count = LD32(tp + 0x08), crc = crc_of(name);
    uint64_t ptrs = LD64(tp + 0x10);
    for (uint32_t i = 0; i < count && i < FLAG_MAX && in_guest(ptrs); i++) {
        uint64_t f = LD64(ptrs + 8ull * i);
        if (in_guest(f) && LD32(f + 0xc) == crc) return LD8(f + 0xb) & 1;
    }
    return -1;
}
// The s32 flags: count +0x18, ptrs +0x20 (FUN_7101138e00 indexes the s32 list there). The value offset inside a flag
// object: GDT_S32_VALUE (found with `coop s32 <name>`, which logs the first bytes).
#define GDT_S32_VALUE 0x18  // (seen 2026-11-12: +0x18 value, +0x1c name crc32, +0x14 initial value)
static uint64_t gdt_s32_obj(const char *name) {
    uint64_t mgr = LD64(A_GDT_MGR), tp = in_guest(mgr) ? LD64(mgr + 0xc00) : 0;
    if (!in_guest(tp)) return 0;
    uint32_t count = LD32(tp + 0x18), crc = crc_of(name);
    uint64_t ptrs = LD64(tp + 0x20);
    for (uint32_t i = 0; i < count && i < 20000 && in_guest(ptrs); i++) {
        uint64_t f = LD64(ptrs + 8ull * i);
        if (in_guest(f) && LD32(f + 0x1c) == crc) return f;
    }
    return 0;
}
static bool gdt_get_s32(const char *name, int32_t *out) {
    uint64_t f = gdt_s32_obj(name);
    if (!f) return false;
    *out = (int32_t)LD32(f + GDT_S32_VALUE);
    return true;
}
// GameDataMgr setS32 internal helper (0x7000d3b46c, the s32 twin of A_GDT_SET_BOOL_EX: same arguments).
#define A_GDT_SET_S32_EX 0x7000d3b46cull
static bool gdt_set_s32(Cpu *c, const char *name, int32_t val) {
    uint64_t mgr = LD64(A_GDT_MGR), s = world_safestring(name);
    if (!in_guest(mgr) || !s) return false;
    Cpu saved = *c;
    c->x[0] = mgr + 0xbb0;
    c->x[1] = (uint64_t)(uint32_t)val;
    c->x[2] = s;
    c->x[3] = 1;
    c->x[4] = 1;
    guest_call(c, A_GDT_SET_S32_EX);
    bool ok = (c->x[0] & 1) != 0;
    *c = saved;
    return ok;
}

static struct { volatile int pending; char name[64]; int set; int32_t value; } s32_dev;  // `coop s32 <name> [value]`
static void s32_dev_frame(Cpu *c) {
    if (!s32_dev.pending) return;
    s32_dev.pending = 0;
    uint64_t f = gdt_s32_obj(s32_dev.name);
    if (!f) {
        uint64_t mgr = LD64(A_GDT_MGR), tp = in_guest(mgr) ? LD64(mgr + 0xc00) : 0;
        LOG("[coop] s32 %s: not found (crc %08x); store %llx:\n", s32_dev.name, crc_of(s32_dev.name),
            (unsigned long long)tp);
        if (in_guest(tp)) {  // where is the crc in an s32 flag object?
            uint32_t count = LD32(tp + 0x18), crc = crc_of(s32_dev.name);
            uint64_t ptrs = LD64(tp + 0x20);
            for (uint32_t i = 0; i < count && i < 20000 && in_guest(ptrs); i++) {
                uint64_t e = LD64(ptrs + 8ull * i);
                for (int o = 4; in_guest(e) && o < 0x40; o += 4)
                    if (LD32(e + o) == crc) {
                        char hex[200] = "";
                        for (int k = 0; k < 0x30; k += 4) {
                            size_t l = strlen(hex);
                            snprintf(hex + l, sizeof hex - l, " +%x=%08x", k, LD32(e + k));
                        }
                        LOG("[coop]   found: element %u at %llx, crc at +0x%x:%s\n", i, (unsigned long long)e, o, hex);
                    }
            }
        }
        if (in_guest(tp))
            for (int o = 8; o < 0x88; o += 0x10) {
                uint32_t n = LD32(tp + o);
                uint64_t p = LD64(tp + o + 8), e = in_guest(p) ? LD64(p) : 0;
                char hex[160] = "";
                for (int k = 0; in_guest(e) && k < 0x20; k += 4) {
                    size_t l = strlen(hex);
                    snprintf(hex + l, sizeof hex - l, " %08x", LD32(e + k));
                }
                LOG("[coop]   +%02x count %u ptrs %llx first %llx:%s\n", o, n, (unsigned long long)p,
                    (unsigned long long)e, hex);
            }
        return;
    }
    char hex[200] = "";
    for (int o = 0; o < 0x28; o += 4) {
        size_t n = strlen(hex);
        snprintf(hex + n, sizeof hex - n, " +%x=%08x", o, LD32(f + o));
    }
    int32_t now = -999;
    gdt_get_s32(s32_dev.name, &now);
    LOG("[coop] s32 %s at %llx:%s value %d\n", s32_dev.name, (unsigned long long)f, hex, now);
    if (s32_dev.set) {
        bool ok = gdt_set_s32(c, s32_dev.name, s32_dev.value);
        int32_t v = -999;
        gdt_get_s32(s32_dev.name, &v);
        LOG("[coop] s32 %s set %d: %s, reads %d\n", s32_dev.name, s32_dev.value, ok ? "ok" : "failed", v);
    }
}

// `coop watchai <actor hex> [seconds]` (dev, RE, 2026-11-12): the objects reachable from the actor that point back at it
// (its AI/actions, like aiobj), their first 0x100 bytes snapshotted; every frame, any 4-byte word that changes is
// logged (as hex and float), up to 400 lines. For finding where a beast's moving part keeps its state.
#define WATCH_MAX 48
static struct {
    volatile int pending;
    uint64_t actor, until_ns;
    int n, lines;
    uint64_t obj[WATCH_MAX];
    uint32_t off[WATCH_MAX];
    uint32_t snap[WATCH_MAX][128];
} watch_dev;
static void watch_collect(uint64_t o, int depth, uint32_t at) {
    for (uint32_t off = 0; off < 0x1000 && watch_dev.n < WATCH_MAX; off += 8) {
        uint64_t p = LD64(o + off);
        if (!in_guest(p) || (p & 7) || !in_guest(p + 0x200)) continue;
        uint64_t vt = LD64(p);
        if (LD64(p + 8) == watch_dev.actor && vt >= 0x7000000000ull && vt < 0x7003000000ull) {
            bool dup = false;
            for (int k = 0; k < watch_dev.n && !dup; k++) dup = watch_dev.obj[k] == p;
            if (!dup) {
                int k = watch_dev.n++;
                watch_dev.obj[k] = p, watch_dev.off[k] = depth ? at : off;
                for (int w = 0; w < 128; w++) watch_dev.snap[k][w] = LD32(p + 4u * w);
                LOG("[coop] watch obj %d: %llx (depth %d, +0x%x) vtable %llx\n", k, (unsigned long long)p, depth, off,
                    (unsigned long long)vt);
            }
        }
        if (depth < 1) watch_collect(p, depth + 1, off);
    }
}
static void watch_frame(void) {
    uint64_t now = plat_now_ns();
    if (watch_dev.pending == 1) {
        watch_dev.pending = 2;
        watch_dev.n = 0, watch_dev.lines = 0;
        if (in_guest(watch_dev.actor) && in_guest(watch_dev.actor + 0x1000)) watch_collect(watch_dev.actor, 0, 0);
        LOG("[coop] watch: %d objects\n", watch_dev.n);
    }
    if (watch_dev.pending != 2) return;
    if (now > watch_dev.until_ns || watch_dev.lines >= 400) {
        watch_dev.pending = 0;
        LOG("[coop] watch over (%d lines)\n", watch_dev.lines);
        return;
    }
    for (int k = 0; k < watch_dev.n; k++)
        for (int w = 0; w < 128; w++) {
            uint32_t v = LD32(watch_dev.obj[k] + 4u * w);
            if (v == watch_dev.snap[k][w]) continue;
            float fo, fn;
            memcpy(&fo, &watch_dev.snap[k][w], 4), memcpy(&fn, &v, 4);
            if (watch_dev.lines++ < 400)
                LOG("[coop] watch %d +0x%02x: %08x -> %08x (%g -> %g)\n", k, w * 4, watch_dev.snap[k][w], v, fo, fn);
            watch_dev.snap[k][w] = v;
        }
}

// ---- moving parts (protocol 36). Vah Ruta's trunk is turned by the Slate's beast map ("move trunk"): the rotate tag
// actor DungeonRotateTagRemainsWater's AI (actor +0x558) has at +0x190 the turning action (vtable BEAST_ROT_VT) with
// the current angle at +0x80 (radians: 0 down, -0.349 up) and its speed at +0x84 (non-zero only while it turns; found
// with `coop watchai` 2026-11-12). Writing the angle moves the trunk (the beast map shows it) and it stays.
// Owner = the last mover: a game whose own tag turns (speed != 0) sends the angle (every 0.2 s while it turns, every
// 4 s after); the other game, while its own tag is still, swings its angle toward the received one at the game's own
// top speed. SAVAGEWIND_COOP_BEASTROT=0 turns only this off.
// 2026-11-12: Vah Rudania's tilt (DungeonRotateTagRemainsFire, Slate map "tilt 90 degrees") is a sibling class of the
// same base (vtable BEAST_ROT_VT_FIRE): same angle/axis/speed fields and the same apply call from its per-frame code
// (Ghidra FUN_71006327fc), only its target sits at +0xd8 instead of +0x110 (vtables read from main's relocations).
#define BEAST_ROT_VT 0x7001be4ab8ull
#define BEAST_ROT_VT_FIRE 0x7001be4d18ull
static uint32_t brot_target_off(uint64_t vt) { return vt == BEAST_ROT_VT ? 0x110 : vt == BEAST_ROT_VT_FIRE ? 0xd8 : 0; }
static float LDF(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }
static void STF(uint64_t a, float f) { memcpy((void *)(uintptr_t)a, &f, 4); }
#define BEAST_ROT_MAX 8
#define BEAST_ROT_STEP 0.00698f  // rad per frame (the action's own +0x88)
static bool beast_rot_on(void) {
    static int on = -1;
    if (on < 0) on = beast_on() && !(getenv("SAVAGEWIND_COOP_BEASTROT") && !strcmp(getenv("SAVAGEWIND_COOP_BEASTROT"), "0"));
    return on;
}
static struct BeastRot {
    uint64_t actor, unit, obj, vt;
    uint32_t id, hash, tgt;           // tgt: the action's target angle field (brot_target_off)
    bool mine, moving, have_remote, seen;
    float remote, sent, last, wrote;  // last: the angle last frame; wrote: what we wrote there last frame (follower)
    int still;                        // frames the angle has not changed
    uint64_t sent_ns;
} brot[BEAST_ROT_MAX];
static int brot_n;
static uint32_t brot_area;
static bool brot_name(const char *n) { return !strncmp(n, "DungeonRotateTag", 16); }
static void brot_add(Cpu *c, uint64_t a, const char *name, void *ctx) {
    bool fresh = ctx && *(bool *)ctx;
    if (brot_n >= BEAST_ROT_MAX) return;
    uint64_t ai = LD64(a + 0x558), obj = in_guest(ai) ? LD64(ai + 0x190) : 0;
    if (!in_guest(obj) || !brot_target_off(LD64(obj)) || LD64(obj + 8) != a) return;
    struct BeastRot *r = &brot[brot_n++];
    memset(r, 0, sizeof *r);
    r->actor = a, r->obj = obj, r->vt = LD64(obj), r->tgt = brot_target_off(r->vt), r->unit = LD64(a + PROC_UNIT), r->id = LD32(r->unit + UNIT_ID), r->hash = actor_hash(c, a);
    r->last = r->wrote = LDF(obj + 0x80);
    if (fresh) LOG("[coop] beast: moving part %s (hash %u) angle %.3f\n", name, r->hash, r->last);
}
static bool brot_alive(const struct BeastRot *r) {
    return in_guest(r->unit) && LD32(r->unit + UNIT_ID) == r->id && LD64(r->unit + UNIT_ACTOR) == r->actor &&
           LD8(r->actor + PROC_STATE) != 3 && LD64(r->obj) == r->vt;
}
static void brot_send(struct BeastRot *r, float ang, uint64_t now) {
    CoopBeastRot e = {r->hash, brot_area, ang};
    coop_net_send_event(COOP_EV_BEAST_ROT, &e, sizeof e);
    r->sent = ang, r->sent_ns = now;
}
// The angle alone is the action's bookkeeping: the trunk the player sees (scenery, physics) is moved by the action's
// per-frame code (Ghidra FUN_7100631ecc) only while it runs, through FUN_7100633638(action, angle * axis, past target)
// - found 2026-11-12 after the first two-game run showed the follower's map trunk still down. The axis (+0x70..+0x78)
// is set on the action's enter (FUN_7100632c1c): column *(int *)(action+0x20) of the actor's matrix rows at
// actor+0x3d0/+0x3e0/+0x3f0. The follower's action never entered, so it is computed here the same way.
#define A_BEAST_ROT_APPLY 0x7000633638ull
static uint64_t brot_mem;  // guest scratch: the rotation vector (reset on reboot)
static bool brot_apply(Cpu *c, const struct BeastRot *r, float ang) {
    uint64_t o = r->obj, a = r->actor, kp = LD64(o + 0x20);
    if (!LD64(o + 0x90) || !in_guest(kp)) return false;  // (the game skips it too without +0x90)
    uint32_t k = LD32(kp);
    if (k > 2) return false;
    float axis[3] = {LDF(a + 0x3d0 + 4 * k), LDF(a + 0x3e0 + 4 * k), LDF(a + 0x3f0 + 4 * k)};
    if (!brot_mem) brot_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
    if (!brot_mem) return false;
    for (int i = 0; i < 3; i++) STF(o + 0x70 + 4 * i, axis[i]);
    STF(o + 0x80, ang);
    float v[3] = {ang * axis[0], ang * axis[1], ang * axis[2]};
    memcpy((void *)(uintptr_t)brot_mem, v, 12);
    Cpu saved = *c;
    c->x[0] = o; c->x[1] = brot_mem; c->x[2] = LDF(o + r->tgt) < ang;
    guest_call(c, A_BEAST_ROT_APPLY);
    *c = saved;
    return true;
}
static void beast_rot_frame(Cpu *c, uint64_t now) {
    static uint64_t scan_ns;
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (!beast_rot_on() || strncmp(area, "MainFieldDungeon/Remains", 24) || !local_in_world()) {
        brot_n = 0, brot_area = 0;
        return;
    }
    if (here != brot_area || now > scan_ns) {  // (re-found every 5 s: cheap, and the actors may be reloaded)
        bool fresh = here != brot_area;
        struct BeastRot keep[BEAST_ROT_MAX];
        int kn = fresh ? 0 : brot_n;
        memcpy(keep, brot, sizeof keep);
        brot_n = 0, brot_area = here, scan_ns = now + 5000000000ull;
        walk_actors(c, brot_name, brot_add, &fresh);
        for (int i = 0; i < brot_n; i++)  // (keep what we knew about each part)
            for (int k = 0; k < kn; k++)
                if (keep[k].hash == brot[i].hash) {
                    struct BeastRot n = brot[i];
                    brot[i] = keep[k];
                    brot[i].actor = n.actor, brot[i].obj = n.obj, brot[i].unit = n.unit, brot[i].id = n.id;
                    brot[i].vt = n.vt, brot[i].tgt = n.tgt;
                }
    }
    // (2026-11-12 first two-game run: the speed at +0x84 is not 0 at rest in a fresh game (0.00698 until the first
    // use), so "turning" is told from the angle itself: it changed, and not to the value we wrote.)
    for (int i = 0; i < brot_n; i++) {
        struct BeastRot *r = &brot[i];
        if (!brot_alive(r)) continue;
        float ang = LDF(r->obj + 0x80);
        if (!isfinite(ang)) continue;
        bool ours = isfinite(r->wrote) && fabsf(ang - r->wrote) <= 1e-6f;
        bool changed = fabsf(ang - r->last) > 1e-6f && !ours;
        r->last = ang, r->wrote = NAN;
        if (changed) {  // our player turns it
            if (!r->moving) LOG("[coop] beast: part %u turning here (from %.3f): we lead it\n", r->hash, ang);
            r->mine = true, r->moving = true, r->have_remote = false, r->still = 0;
            if (now - r->sent_ns > 200000000ull) brot_send(r, ang, now);
            continue;
        }
        if (r->moving && ++r->still < 20) continue;  // (a turn may pause for a few frames)
        if (r->moving) {
            r->moving = false;
            brot_send(r, ang, now);
            LOG("[coop] beast: part %u stopped at %.3f: sent\n", r->hash, ang);
        } else if (r->mine && now - r->sent_ns > 4000000000ull) {
            brot_send(r, ang, now);
        }
        if (!r->have_remote || r->mine) continue;
        float d = r->remote - ang;
        if (fabsf(d) < 1e-5f) continue;
        float step = LDF(r->obj + 0x88);  // (the part's own top speed: Ruta 0.00698; Rudania turned ~0.004/frame)
        if (!(step > 1e-4f && step < 0.05f)) step = BEAST_ROT_STEP;
        float to = fabsf(d) <= step ? r->remote : ang + (d > 0 ? step : -step);
        if (!brot_apply(c, r, to)) STF(r->obj + 0x80, to);
        r->last = r->wrote = to;
        if (to == r->remote) LOG("[coop] beast: part %u now at the other player's angle %.3f\n", r->hash, to);
    }
}
static void beast_rot_ev(const uint8_t *data, int len) {
    if (len != (int)sizeof(CoopBeastRot) || !beast_rot_on()) return;
    CoopBeastRot e;
    memcpy(&e, data, sizeof e);
    if (!coop_beast_rot_ok(&e) || e.area != brot_area) return;
    for (int i = 0; i < brot_n; i++) {
        struct BeastRot *r = &brot[i];
        if (r->hash != e.hash || r->moving) continue;  // (our own player turning it right now wins)
        if (!r->have_remote || fabsf(r->remote - e.angle) > 1e-4f)
            LOG("[coop] beast: the other player turned part %u to %.3f\n", r->hash, e.angle);
        r->mine = false, r->have_remote = true, r->remote = e.angle;
    }
}

static void beast_frame(Cpu *c) {
    s32_dev_frame(c);
    watch_frame();
    beast_rot_frame(c, plat_now_ns());
    static uint64_t next_ns, wrong_since;
    uint64_t now = plat_now_ns();
    if (now < next_ns) return;
    next_ns = now + 500000000ull;
    int b = beast_here();
    if (b < 0 || !beast_on() || !local_in_world() || event_running()) { wrong_since = 0; return; }
    int on = 0;
    char flag[96];
    for (int t = 0; t < 5; t++) {
        snprintf(flag, sizeof flag, "MainFieldDungeon_FldObj_Terminal_A_01_%u", beast_tab[b].term[t]);
        if (gdt_get_bool(flag) == 1) on++;
    }
    char counter[48];
    snprintf(counter, sizeof counter, "Counter_Terminal%s", beast_tab[b].name);
    int32_t cur;
    if (!gdt_get_s32(counter, &cur)) return;
    int32_t want = 5 - on;
    if (cur == want) { wrong_since = 0; return; }
    if (!wrong_since) { wrong_since = now; return; }
    if (now - wrong_since < 2000000000ull) return;
    wrong_since = 0;
    bool ok = gdt_set_s32(c, counter, want);
    LOG("[coop] beast: %d of 5 terminals on (some by the other player): %s %d -> %d%s\n", on, counter, cur, want,
        ok ? "" : " - FAILED");
}
