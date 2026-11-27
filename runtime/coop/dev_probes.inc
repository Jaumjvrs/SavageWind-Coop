// Part of runtime/coop.c (included there, same translation unit): dev probes run on the game thread (rbscan, rbmove, setflag, flag).
static void rbscan_one(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c;
    const float *me = ctx;
    float p[3];
    if (actor_dist(a, me, p) > rbscan_dev.radius) return;
    uint64_t rb = actor_rb(a), pi = LD64(a + 0x190);
    char where[160] = "";
    if (rb && in_guest(pi))
        for (uint32_t o = 0; o < 0x800; o += 8) {
            if (LD64(pi + o) == rb) snprintf(where + strlen(where), sizeof where - strlen(where), " pi+0x%x", o);
            uint64_t q = LD64(pi + o);
            if (in_guest(q) && !(q & 7) && in_guest(q + 0x100))
                for (uint32_t o2 = 0; o2 < 0x100; o2 += 8)
                    if (LD64(q + o2) == rb && strlen(where) < 120)
                        snprintf(where + strlen(where), sizeof where - strlen(where), " pi+0x%x->+0x%x", o, o2);
        }
    LOG("[coop] rbscan %-34s hash %10u at %.1f %.1f %.1f rb %llx flags %08x path%s\n", name, actor_hash(c, a), p[0],
        p[1], p[2], (unsigned long long)rb, rb ? LD32(rb + 0x68) : 0, where[0] ? where : " -");
}
static void rbscan_frame(Cpu *c) {
    if (!rbscan_dev.pending) return;
    rbscan_dev.pending = 0;
    float me[3];
    memcpy(me, (void *)(uintptr_t)(player_info_ptr() + 0x88), 12);
    walk_actors(c, rbscan_want, rbscan_one, me);
    LOG("[coop] rbscan done\n");
}

// `coop bodyfind <actor hex>` (dev, RE 2026-11-16 world2): every rigid body (RIGID_BODY_VT) reachable from the actor
// within 3 pointer hops (0x800 bytes per object), with its path and its distance from the actor's matrix position.
static struct { volatile int pending; uint64_t actor; int found; } bodyfind_dev;
static void bodyfind_walk(uint64_t obj, int depth, const char *path) {
    const float *m = (const float *)(uintptr_t)(bodyfind_dev.actor + ACTOR_MTX);
    for (uint32_t off = 0; off < 0x800 && bodyfind_dev.found < 24; off += 8) {
        uint64_t p = LD64(obj + off);
        if (!in_guest(p) || (p & 7) || !in_guest(p + 0x800)) continue;
        char np[96];
        snprintf(np, sizeof np, "%s+%x", path, off);
        if (LD64(p) == RIGID_BODY_VT) {
            const float *bp = (const float *)(uintptr_t)(p + 0x1a0);
            float dx = bp[0] - m[3], dy = bp[1] - m[7], dz = bp[2] - m[11];
            uint64_t rb = LD64(p + 0x18);
            LOG("[coop] bodyfind %llx: body %llx at %s, %.2f m off (%.2f %.2f %.2f), wrapper %llx flags %08x\n",
                (unsigned long long)bodyfind_dev.actor, (unsigned long long)p, np, sqrtf(dx * dx + dy * dy + dz * dz),
                bp[0], bp[1], bp[2], (unsigned long long)rb, in_guest(rb) ? LD32(rb + 0x68) : 0);
            bodyfind_dev.found++;
            continue;
        }
        if (depth < 2) bodyfind_walk(p, depth + 1, np);
    }
}
static void bodyfind_frame(void) {
    if (!bodyfind_dev.pending) return;
    bodyfind_dev.pending = 0;
    uint64_t a = bodyfind_dev.actor;
    if (!in_guest(a) || !in_guest(a + 0x1000)) { LOG("[coop] bodyfind %llx: no actor\n", (unsigned long long)a); return; }
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    LOG("[coop] bodyfind %llx %.40s at %.2f %.2f %.2f (vt %llx)\n", (unsigned long long)a, (const char *)(uintptr_t)(a + 0x1c),
        m[3], m[7], m[11], (unsigned long long)LD64(a));
    bodyfind_dev.found = 0;
    bodyfind_walk(a, 0, "");
    LOG("[coop] bodyfind done: %d bodies\n", bodyfind_dev.found);
}

// `coop rbmove <actor hex> dx dy dz [vel]` (dev, RE): move an object through the game's own setTransform (or, with
// "vel", give it that linear velocity), on the game thread.
static struct { volatile int pending; uint64_t actor; float d[3]; bool vel, unfix; } rbmove_dev;
static void rbmove_frame(Cpu *c) {
    if (!rbmove_dev.pending) return;
    rbmove_dev.pending = 0;
    uint64_t a = rbmove_dev.actor, rb = actor_rb(a);
    if (!rb) { LOG("[coop] rbmove %llx: no rigid body found\n", (unsigned long long)a); return; }
    float m[12];
    memcpy(m, (void *)(uintptr_t)(a + ACTOR_MTX), sizeof m);
    if (rbmove_dev.unfix || (LD32(rb + 0x68) & 0x80000)) {
        Cpu saved = *c;
        c->x[0] = rb; c->x[1] = 0; c->x[2] = 0;
        guest_call(c, A_RB_SET_FIXED);
        *c = saved;
        LOG("[coop] rbmove %llx: unfixed, flags now %08x %08x\n", (unsigned long long)a, LD32(rb + 0x68), LD32(rb + 0x6c));
    }
    if (rbmove_dev.vel) {
        rb_set_linvel(c, rb, rbmove_dev.d);
    } else {
        m[3] += rbmove_dev.d[0], m[7] += rbmove_dev.d[1], m[11] += rbmove_dev.d[2];
        rb_set_transform(c, rb, m);
    }
    LOG("[coop] rbmove %llx: rb %llx (vt %llx) %s %.2f %.2f %.2f\n", (unsigned long long)a, (unsigned long long)rb,
        (unsigned long long)LD64(rb), rbmove_dev.vel ? "velocity" : "moved by", rbmove_dev.d[0], rbmove_dev.d[1],
        rbmove_dev.d[2]);
}

// `coop arrowtrace <seconds> [filter]` (dev, RE): every frame, walk the process ring WITHOUT the actor type test or
// the state filter and log processes whose name contains the filter (default "Arrow"): new ones, gone ones, and
// their position every 4th frame.
static struct { uint64_t until_ns; char filter[32]; uint64_t seen[64]; uint32_t n; uint64_t frame; } arrowtrace_dev;
static void arrowtrace_frame(void) {
    if (!arrowtrace_dev.until_ns) return;
    if (plat_now_ns() > arrowtrace_dev.until_ns) { arrowtrace_dev.until_ns = 0; LOG("[coop] arrowtrace done\n"); return; }
    uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t start = in_guest(pa) ? LD64(pa + PROC_UNIT) : 0;
    if (!in_guest(start)) return;
    arrowtrace_dev.frame++;
    uint64_t now_seen[64];
    uint32_t nn = 0;
    uint64_t u = start, unit_vt = LD64(start);
    for (unsigned step = 0; step < 16384; step++) {
        uint64_t next = LD64(u + UNIT_NODE + 8);
        uint64_t a = LD64(u) == unit_vt ? LD64(u + UNIT_ACTOR) : 0;
        if (in_guest(a) && in_guest(a + 0x900)) {
            const char *name = (const char *)(uintptr_t)(a + 0x1c);
            char nm[48];
            snprintf(nm, sizeof nm, "%.47s", name);
            if (strstr(nm, arrowtrace_dev.filter)) {
                bool known = false;
                for (uint32_t i = 0; i < arrowtrace_dev.n; i++) known |= arrowtrace_dev.seen[i] == a;
                const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
                if (!known || arrowtrace_dev.frame % 4 == 0)
                    LOG("[coop] arrowtrace f%llu %s %-24s %llx state %d id %08x at %.2f %.2f %.2f\n",
                        (unsigned long long)arrowtrace_dev.frame, known ? "   " : "NEW", nm, (unsigned long long)a,
                        LD8(a + PROC_STATE), LD32(a + PROC_ID), m[3], m[7], m[11]);
                if (nn < 64) now_seen[nn++] = a;
            }
        }
        if (!in_guest(next)) break;
        u = next - UNIT_NODE;
        if (u == start) break;
    }
    for (uint32_t i = 0; i < arrowtrace_dev.n; i++) {
        bool still = false;
        for (uint32_t j = 0; j < nn; j++) still |= now_seen[j] == arrowtrace_dev.seen[i];
        if (!still) LOG("[coop] arrowtrace f%llu GONE %llx\n", (unsigned long long)arrowtrace_dev.frame,
                        (unsigned long long)arrowtrace_dev.seen[i]);
    }
    memcpy(arrowtrace_dev.seen, now_seen, sizeof now_seen[0] * nn), arrowtrace_dev.n = nn;
}

// `coop setflag <name> 0|1` (dev): set on the game thread with the game's setBool helper.
static struct { volatile int pending; bool value; char name[96]; } setflag_dev;
static void setflag_frame(Cpu *c) {
    if (!setflag_dev.pending) return;
    setflag_dev.pending = 0;
    bool ok = gdt_set_bool(c, setflag_dev.name, setflag_dev.value);
    LOG("[coop] setflag %s %d (%s)\n", setflag_dev.name, setflag_dev.value, ok ? "ok" : "failed");
}

// `coop flag <name>` (dev): a bool game flag's value, by name (crc32 lookup in the read buffer), to the log.
static void flag_report(const char *name) {
    uint64_t mgr = LD64(A_GDT_MGR), tp = in_guest(mgr) ? LD64(mgr + 0xc00) : 0;
    if (!in_guest(tp)) { LOG("[coop] flag %s: no flag store\n", name); return; }
    uint32_t count = LD32(tp + 0x08), crc = crc_of(name);
    uint64_t ptrs = LD64(tp + 0x10);
    for (uint32_t i = 0; i < count && i < FLAG_MAX && in_guest(ptrs); i++) {
        uint64_t f = LD64(ptrs + 8ull * i);
        if (in_guest(f) && LD32(f + 0xc) == crc) {
            LOG("[coop] flag %s = %d\n", name, LD8(f + 0xb) & 1);
            return;
        }
    }
    LOG("[coop] flag %s: not a bool flag\n", name);
}
