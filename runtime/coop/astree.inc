// Part of runtime/coop.c (included from coop/avatar.inc): the player's own AS trees on the avatar (tree mode).
// Every selector / blender of an AS tree reads its input from one table in the actor's AS controller (actor+0x560,
// the "holder"): +0xe0 u32 slot count, +0xe8 -> 8-byte slots, +0xf0 u8 map[67] kind -> slot (negative: the kind is
// not registered, reads give 0 / "" / false). Slot contents by kind: kinds < 30 a float inline (21 GroundNormal: a
// pointer to a vec3); 44..62 and 64: a pointer to a string object (+8 char *, +0x10 int capacity); 66 Bool: a pointer
// to a u64 bit set; the others (42 Charge, 43 Combo...) an int inline. The game's own setters do plain stores
// (0x70014a1fb0 setFloat, 0x70014a1d04 setString...), so raw reads / writes are what the game does.
// Speed / YSpeed come from actor+0x400..0x408 (velocity), not from the table.
#define ASIN_KINDS 67
static void charge_fx(Cpu *c, unsigned lv, const char *as);  // (coop/avatarfx.inc) charge sparks and sword swirl

static bool charge_hold_as(const char *as) {
    return starts(as, "CutChargeWait") || starts(as, "CutChargeMove");
}
static unsigned astree_charge_lv;                         // (protocol 47) the partner's last charge level (grass.inc)
static void fx_fill(uint64_t pa, CoopAsTree *t);          // (protocol 39, coop/avatarfx.inc) effect-set properties
static void fx_write(uint64_t actor, const CoopAsTree *t);
static const char *xl_who = "frame";  // (ripple trace: which fx_write call changed a value)
static void snd_write(uint64_t actor, const CoopAsTree *t);  // (2026-11-14, coop/avatarsnd.inc) the avatar's sound set
#define ASIN_MAP 0xf0
#define ACTOR_VEL 0x400

static uint64_t asin_holder(uint64_t actor) {
    if (!in_guest(actor)) return 0;
    uint64_t h = LD64(actor + ACTOR_AS);
    return in_guest(h) && in_guest(LD64(h + 0xe8)) ? h : 0;
}
// The address of the kind's slot, 0 when the kind is not registered.
static uint64_t asin_slot(uint64_t h, int kind) {
    if (!h || kind < 0 || kind >= ASIN_KINDS) return 0;
    int8_t m = (int8_t)LD8(h + ASIN_MAP + (uint64_t)kind);
    if (m < 0) return 0;
    uint32_t n = LD32(h + 0xe0);
    return LD64(h + 0xe8) + (uint64_t)((uint32_t)m < n ? (uint32_t)m : 0) * 8;
}
enum { ASIN_FLOAT, ASIN_VEC3, ASIN_STRING, ASIN_BOOLS, ASIN_INT };
static int asin_type(int kind) {
    if (kind == 21) return ASIN_VEC3;
    if (kind < 30) return ASIN_FLOAT;
    if ((kind >= 44 && kind <= 62) || kind == 64) return ASIN_STRING;
    if (kind == 66) return ASIN_BOOLS;
    return ASIN_INT;
}
// The string of a string kind ("" when none).
static const char *asin_str(uint64_t slot) {
    uint64_t o = slot ? LD64(slot) : 0, p = in_guest(o) ? LD64(o + 8) : 0;
    return in_guest(p) ? (const char *)(uintptr_t)p : "";
}
static void asin_set_str(uint64_t slot, const char *s) {
    uint64_t o = slot ? LD64(slot) : 0, p = in_guest(o) ? LD64(o + 8) : 0;
    int cap = in_guest(o) ? (int)LD32(o + 0x10) : 0;
    if (!in_guest(p) || cap <= 0) return;
    int n = (int)strlen(s);
    if (n > cap - 1) n = cap - 1;
    if (strncmp((const char *)(uintptr_t)p, s, (size_t)n + 1)) {
        memcpy((void *)(uintptr_t)p, s, (size_t)n);
        ST8(p + (uint64_t)n, 0);
    }
}

// `coop asin <actor>` (dev): the actor's whole input table, to the log.
static void asin_dump(uint64_t actor, const char *who) {
    uint64_t h = asin_holder(actor);
    if (!h) { LOG("[coop] asin %s %llx: no AS controller\n", who, (unsigned long long)actor); return; }
    char line[2400];
    size_t n = 0;
    int regs = 0;
    for (int k = 0; k < ASIN_KINDS && n + 120 < sizeof line; k++) {
        uint64_t s = asin_slot(h, k);
        if (!s) continue;
        regs++;
        switch (asin_type(k)) {
        case ASIN_FLOAT: { float f; memcpy(&f, (void *)(uintptr_t)s, 4); n += (size_t)snprintf(line + n, sizeof line - n, " %d=%g", k, f); break; }
        case ASIN_VEC3: {
            uint64_t v = LD64(s);
            float f[3] = {0, 0, 0};
            if (in_guest(v)) memcpy(f, (void *)(uintptr_t)v, 12);
            n += (size_t)snprintf(line + n, sizeof line - n, " %d=(%.3f,%.3f,%.3f)", k, f[0], f[1], f[2]);
            break;
        }
        case ASIN_STRING: n += (size_t)snprintf(line + n, sizeof line - n, " %d='%.40s'", k, asin_str(s)); break;
        case ASIN_BOOLS: { uint64_t v = LD64(s); n += (size_t)snprintf(line + n, sizeof line - n, " %d=%016llx", k, in_guest(v) ? (unsigned long long)LD64(v) : 0ull); break; }
        default: n += (size_t)snprintf(line + n, sizeof line - n, " %d=%d", k, (int)LD32(s)); break;
        }
    }
    float vel[3];
    memcpy(vel, (void *)(uintptr_t)(actor + ACTOR_VEL), 12);
    LOG("[coop] asin %s %llx: count %u registered %d vel %.3f %.3f %.3f:%s\n", who, (unsigned long long)actor,
        LD32(h + 0xe0), regs, vel[0], vel[1], vel[2], line);
}

// Dev probes (stage 1 of tree mode): `coop asin <actor|player|avatar>`, `coop asw <actor> <kind> <value>` (one
// write: a float, an int, a string, or a hex bit set), `coop asmirror on|off` (each frame the avatar copies OUR OWN
// player's input table, velocity and AS names of slots 0..4: the avatar beside us mimics our Link).
static struct {
    volatile uint64_t dump;
    volatile int write;
    uint64_t w_actor; int w_kind; char w_val[64];
    volatile int mirror;
    volatile int trace;  // `coop astrace <frames>` (dev): per frame, bone heights of the player / avatar / armor puppets
    volatile int rec;    // `coop asrec <frames>` (dev): per frame, the player's 0.0 / 1.0 layer records (restart probe)
} asdev;

static void asin_write_text(uint64_t actor, int kind, const char *val) {
    uint64_t s = asin_slot(asin_holder(actor), kind);
    if (!s) { LOG("[coop] asw %llx kind %d: not registered\n", (unsigned long long)actor, kind); return; }
    switch (asin_type(kind)) {
    case ASIN_FLOAT: { float f = strtof(val, NULL); memcpy((void *)(uintptr_t)s, &f, 4); break; }
    case ASIN_VEC3: {
        float f[3] = {0, 1, 0};
        sscanf(val, "%f,%f,%f", &f[0], &f[1], &f[2]);
        uint64_t v = LD64(s);
        if (in_guest(v)) memcpy((void *)(uintptr_t)v, f, 12);
        break;
    }
    case ASIN_STRING: asin_set_str(s, val); break;
    case ASIN_BOOLS: { uint64_t v = LD64(s); if (in_guest(v)) ST64(v, strtoull(val, NULL, 16)); break; }
    default: ST32(s, (uint32_t)strtol(val, NULL, 0)); break;
    }
    LOG("[coop] asw %llx kind %d = %s\n", (unsigned long long)actor, kind, val);
}

// ---- tree mode (protocol 29). Sender: each frame our player's input table (the kinds below; the rest are computed
// by the game or unused by the player's AS), velocity, and the AS in every slot / layer (index into coop_as_names)
// with a restart counter per slot / layer (the layer's AS instance changed under the same name). Receiver: the
// avatar and its armor puppets (same AS list, Co-op/build_overlay.py) get the inputs written and play the same AS
// by name, so the game picks every variant (direction, slope, weapon...) and blends itself.
// SAVAGEWIND_COOP_ASTREE=0: the classified single-clip path (COOP_ANIM_*), which stays the fallback (no tree packet
// for 1 s, a sender forcing states with `coop look ...`, or the other player dying).
// Setup generates this data from the player's own files (Co-op/astree_tables.py).
static const char *coop_as_names[COOP_AS_NAME_COUNT], *coop_as_strings[COOP_AS_STRING_COUNT];

static bool astree_load_tables(void) {
    static char data[16384];
    const char *dir = getenv("SAVAGEWIND_ROMFS_OVERLAY");
    char path[4096];
    if (!dir || !*dir || snprintf(path, sizeof path, "%s/coop_astree.dat", dir) >= (int)sizeof path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t size = fread(data, 1, sizeof data, f);
    bool complete = feof(f) && !ferror(f);
    fclose(f);

    // The game version and entry order are part of the tree wire format. The checksum is for the 1.9.0 tables.
    static const char header[] = "SWASTREE1\n";
    if (!complete || size < sizeof header - 1 || memcmp(data, header, sizeof header - 1) ||
        crc32(0, (const unsigned char *)data, (uInt)size) != 0x465885fau) return false;
    char *p = data + sizeof header - 1, *end = data + size;
    for (int i = 0; i < COOP_AS_NAME_COUNT + COOP_AS_STRING_COUNT; i++) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) return false;
        *nl = 0;
        if (i < COOP_AS_NAME_COUNT) coop_as_names[i] = p;
        else coop_as_strings[i - COOP_AS_NAME_COUNT] = p;
        p = nl + 1;
    }
    return p == end;
}

static bool astree_table_ready(void) {
    static int loaded = -1;
    if (loaded < 0) {
        loaded = astree_load_tables();
        if (loaded) LOG("[coop] tree mode: loaded %d animation names and %d strings from the local overlay\n",
                        COOP_AS_NAME_COUNT, COOP_AS_STRING_COUNT);
        else LOG("[coop] tree mode disabled: missing or incompatible coop_astree.dat (run Co-op Setup again)\n");
    }
    return loaded > 0;
}
static const uint8_t astree_float_kinds[COOP_ASIN_FLOATS] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 15, 16, 17, 23, 24, 25, 26, 28};
static const uint8_t astree_str_kinds[COOP_ASIN_STRS] = {44, 45, 47, 48, 49, 54, 57, 58, 59, 60, 61};
#define ASTREE_N (COOP_AS_SLOTS * COOP_AS_LAYERS)
#ifndef ASTREE_DEFAULT
#define ASTREE_DEFAULT 1  // on (2026-11-10): the one silent host exit seen in a tree-on run matched a stray test `exit`; 4 more runs clean. SAVAGEWIND_COOP_ASTREE=0 = the classified path
#endif

static bool astree_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_ASTREE");
        on = e && *e ? strcmp(e, "0") != 0 : ASTREE_DEFAULT;
        if (!coop_anim_expanded_on() || !astree_table_ready()) on = 0;  // (SAVAGEWIND_COOP_ANIMATIONS=0: the oldest, plainest path)
        LOG("[coop] tree mode (the player's own AS trees on the avatar): %s\n", on ? "on" : "off");
    }
    return on;
}

// An AS slot's layer record (the controller's slots at +0xc0, 0x50 bytes each: +0x20 layer count, +0x28 the layers,
// 0x98 bytes each; what 0x70014a0040 reads): +0 the AS resource (its name at +0x10), +0x18 the playing instance.
static uint64_t as_layer_rec(uint64_t actor, int s, int l) {
    uint64_t as = in_guest(actor) ? LD64(actor + ACTOR_AS) : 0;
    if (!in_guest(as) || s >= (int)LD32(as + 0xb8)) return 0;
    uint64_t sl = LD64(as + 0xc0);
    if (!in_guest(sl)) return 0;
    sl += (uint64_t)s * 0x50;
    if (l >= (int)LD32(sl + 0x20)) return 0;
    uint64_t arr = LD64(sl + 0x28);
    return in_guest(arr) ? arr + (uint64_t)l * 0x98 : 0;
}
static const char *as_layer_name(uint64_t rec) {
    if (!rec || !LD64(rec + 0x18)) return "";
    uint64_t r = LD64(rec), n = in_guest(r) ? LD64(r + 0x10) : 0;
    return in_guest(n) ? (const char *)(uintptr_t)n : "";
}
static int table_index(const char *const *tab, int count, const char *n) {
    if (!n[0]) return 0;
    for (int i = 1; i < count; i++)
        if (!strcmp(n, tab[i])) return i;
    return 0;
}

// The time object of a layer's playing instance (what FUN_71014bc59c reads: inst vfunc +200 (inst, res,
// FUN_71014a4d3c(res))): +4 current frame, +8 the frame before, +0x14 end; 0 when none.
#define A_AS_RES_CTX 0x70014a4d3cull
static uint64_t as_layer_time(Cpu *c, uint64_t rec) {
    uint64_t inst = rec ? LD64(rec + 0x18) : 0, res = rec ? LD64(rec) : 0;
    if (!in_guest(inst) || !in_guest(res) || !in_guest(LD64(inst))) return 0;
    uint64_t fn = LD64(LD64(inst) + 200);
    if (fn < 0x7000000000ull || fn >= 0x7003000000ull) return 0;
    Cpu saved = *c;
    c->x[0] = res;
    guest_call(c, A_AS_RES_CTX);
    uint64_t ctx = c->x[0];
    *c = saved;
    c->x[0] = inst, c->x[1] = res, c->x[2] = ctx;
    guest_call(c, fn);
    uint64_t t = c->x[0];
    *c = saved;
    return in_guest(t) ? t : 0;
}

// Restart of the same AS (2026-11-12). The restart counter `seq` used to count a new instance pointer (rec+0x18) under
// the same name, and never fired in any log: probed live (`coop asrec`, SwimDash pressed 3 times while swimming) the
// instance pointer stays the SAME on a restart (each play rebuilds it at the same address), while the instance's time
// object (vfunc +200) is a new one and its frame starts again at 0 (f+4 0.00, f+8 -0.50). A looping clip wrapping
// round keeps its time object. So: same name, a new time object, and its frame back near the start = restarted.
// SAVAGEWIND_COOP_ASRESTART=0: off (the old instance-pointer rule only).
#define ASRESTART_START_F 1.6f  // (frames: the new time object's frame at the first sample after the restart)
static CoopAsTree astree_sent;  // what we sent last (dev `coop asmirror on`: our own avatar plays it)
static struct { volatile uint16_t as; volatile uint8_t seq; volatile uint64_t until_ns; } look_tree;  // `coop look tree` (dev)
static struct { volatile uint16_t as[3]; } pad_dev;  // `coop padas <0.0> [1.0 [1.1]]` (dev): the menu pose's AS
static void astree_build(Cpu *c, uint64_t pa, CoopAsTree *t) {
    static char str_last[COOP_ASIN_STRS][48], as_last[ASTREE_N][48];
    static uint8_t str_idx[COOP_ASIN_STRS], seq[ASTREE_N];
    static uint16_t as_idx[ASTREE_N];
    static uint64_t inst_last[ASTREE_N], time_last[ASTREE_N];
    static float frame_last[ASTREE_N];
    static int restart_on = -1;
    if (restart_on < 0) {
        restart_on = look_on("SAVAGEWIND_COOP_ASRESTART");
        LOG("[coop] tree: restart of the same AS detected by its time object: %s\n", restart_on ? "on" : "off");
    }
    memset(t, 0, sizeof *t);
    t->normal[1] = 1;
    uint64_t h = asin_holder(pa);
    if (!h) return;
    for (int i = 0; i < COOP_ASIN_FLOATS; i++) {
        uint64_t s = asin_slot(h, astree_float_kinds[i]);
        float f = 0;
        if (s) memcpy(&f, (void *)(uintptr_t)s, 4);
        t->f[i] = isfinite(f) && fabsf(f) <= 100000 ? f : 0;
    }
    uint64_t s = asin_slot(h, 21), v = s ? LD64(s) : 0;
    if (in_guest(v)) {
        float n[3];
        memcpy(n, (void *)(uintptr_t)v, 12);
        if (isfinite(n[0]) && isfinite(n[1]) && isfinite(n[2]) && fabsf(n[0]) <= 2 && fabsf(n[1]) <= 2 && fabsf(n[2]) <= 2)
            memcpy(t->normal, n, 12);
    }
    s = asin_slot(h, 66), v = s ? LD64(s) : 0;
    if (in_guest(v)) t->bools = LD64(v);
    s = asin_slot(h, 42);
    int ci = s ? (int)LD32(s) : 0, co = (s = asin_slot(h, 43)) ? (int)LD32(s) : 0;
    t->charge = (int16_t)(ci < -1000 ? -1000 : ci > 1000 ? 1000 : ci);
    t->combo = (int16_t)(co < -1000 ? -1000 : co > 1000 ? 1000 : co);
    for (int i = 0; i < COOP_ASIN_STRS; i++) {
        const char *str = asin_str(asin_slot(h, astree_str_kinds[i]));
        if (strncmp(str, str_last[i], sizeof str_last[i])) {
            snprintf(str_last[i], sizeof str_last[i], "%s", str);
            // (a value no case names selects like "": the "default" case, or none: same choice on both sides)
            str_idx[i] = (uint8_t)table_index(coop_as_strings, COOP_AS_STRING_COUNT, str_last[i]);
        }
        t->str[i] = str_idx[i];
    }
    float vel[3];
    memcpy(vel, (void *)(uintptr_t)(pa + ACTOR_VEL), 12);
    for (int i = 0; i < 3; i++) t->vel[i] = isfinite(vel[i]) && fabsf(vel[i]) <= 20 ? vel[i] : 0;
    for (int i = 0; i < ASTREE_N; i++) {
        uint64_t rec = as_layer_rec(pa, i / COOP_AS_LAYERS, i % COOP_AS_LAYERS);
        const char *n = as_layer_name(rec);
        uint64_t inst = rec ? LD64(rec + 0x18) : 0;
        uint64_t tm = 0;
        float fr = -1;
        if (restart_on && inst && as_idx[i] && (tm = as_layer_time(c, rec)) != 0) {
            memcpy(&fr, (void *)(uintptr_t)(tm + 4), 4);
            if (!isfinite(fr)) fr = -1;
        }
        if (strncmp(n, as_last[i], sizeof as_last[i])) {
            snprintf(as_last[i], sizeof as_last[i], "%s", n);
            as_idx[i] = (uint16_t)table_index(coop_as_names, COOP_AS_NAME_COUNT, as_last[i]);
            // (sitcook, 2026-11-19) event / scene ASes the name table does not know (resting at a fire, the cooking
            // demo): their name was never seen; log it once per change so a play log shows what the real Link plays.
            static unsigned unk_said;
            if (n[0] && !as_idx[i] && (unk_said++ < 120 || coop_look_log()))
                LOG("[coop] sitcook: layer %d.%d plays '%s' (not in the AS name table; clip frame %.1f)\n", i / COOP_AS_LAYERS,
                    i % COOP_AS_LAYERS, n, fr);
        } else if (as_idx[i] && inst && ((inst != inst_last[i] && inst_last[i]) ||
                   (tm && time_last[i] && tm != time_last[i] && fr >= 0 && fr <= ASRESTART_START_F && frame_last[i] > fr + 0.5f))) {
            seq[i]++;  // (the same AS started again)
            static unsigned said;
            if (said++ < 200 || coop_look_log())
                LOG("[coop] tree: %d.%d %s restarted (seq %u; frame %.2f -> %.2f)\n", i / COOP_AS_LAYERS, i % COOP_AS_LAYERS, n,
                    seq[i], frame_last[i], fr);
        }
        inst_last[i] = inst, time_last[i] = tm, frame_last[i] = fr;
        t->as[i] = as_idx[i];
        t->seq[i] = seq[i];
    }
    // (protocol 46) Horseback bow: the body twist toward the aim is the frame of AimHorseLowerF/B (layer 0.1, a Rate 0
    // clip) that the game sets by code every frame (FUN_71014a2274, ratio from the aim azimuth): send that frame.
    {
        uint64_t rec = as_layer_rec(pa, 0, 1), tm = 0;
        float fr = -1;
        if (rec && starts(as_layer_name(rec), "AimHorseLower") && (tm = as_layer_time(c, rec)) != 0)
            memcpy(&fr, (void *)(uintptr_t)(tm + 4), 4);
        unsigned v = isfinite(fr) && fr >= 0 && fr < 1000 ? 1 + (unsigned)(fr * 64 + 0.5f) : 0;
        t->horse_aim[0] = (uint8_t)v, t->horse_aim[1] = (uint8_t)(v >> 8);
    }
    // (protocol 47) Spin-attack charge: the game's charge state handler (FUN_7100cb2a7c)
    // fires the "ChargeLvUp" sparks on Link's Weapon_R by code when its level player+0x1f94 goes
    // 0 -> 1 and 1 -> 2; no AS triggers them, so the avatar playing CutChargeWait shows nothing. Send the level.
    {
        uint64_t r0 = as_layer_rec(pa, 0, 0);
        const char *cn = as_layer_name(r0);
        unsigned phase = LD32(pa + 0x1f88);
        int level = (int)LD32(pa + 0x1f94);
        int lv = starts(cn, "CutCharge") ? level : 0;
        t->charge_lv = (uint8_t)(lv >= 0 && lv <= 2 ? lv : 0);
        // Log level changes inside the same hold animation as well as transitions to another Cut AS.
        static char cut_was[48];
        static int level_was;
        if (aim_diag_on()) {
            if (starts(cn, "Cut") && (level != level_was || strncmp(cn, cut_was, sizeof cut_was - 1)))
                LOG("[coop] chargediag: %s d24 %d 17f1 %u 1f94 %d phase %u sent %u\n", cn, (int)LD32(pa + 0xd24),
                    (unsigned)LD8(pa + 0x17f1), level, phase, (unsigned)t->charge_lv);
            snprintf(cut_was, sizeof cut_was, "%s", cn);
            level_was = level;
        }
    }
    t->flags = COOP_ASTREE_VALID;
    fx_fill(pa, t);
}

static void astree_names(const CoopAsTree *t, char *out, size_t cap) {
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < ASTREE_N && n + 50 < cap; i++)
        if (t->as[i] && t->as[i] < COOP_AS_NAME_COUNT)
            n += (size_t)snprintf(out + n, cap - n, " %d.%d=%s", i / COOP_AS_LAYERS, i % COOP_AS_LAYERS, coop_as_names[t->as[i]]);
}

static uint16_t as_name_index(const char *n) {
    return astree_table_ready() ? (uint16_t)table_index(coop_as_names, COOP_AS_NAME_COUNT, n) : 0;
}

// The game is paused (pause menu, inventory, map: pause_state != 0). The player's AS and inputs stand still meanwhile,
// so the tree we send would freeze on whatever was playing: the other game's avatar kept repeating it (2026-11-11:
// opened the inventory mid-animation, the avatar looped it). While paused we send a calm version instead: the matching
// idle of the family (swimming -> SwimWait, climbing -> ClimbWait, in the air -> Fall, else Wait), no speed, no stick,
// no upper-body / arm / face extras. Sender side only (no protocol change). SAVAGEWIND_COOP_PAUSE_IDLE=0: off.
static bool local_paused(void) {
    return g_site.pause_state && LD32(g_site.pause_state) != 0;
}
static void astree_settle(CoopAsTree *t) {
    const char *n0 = t->as[0] && t->as[0] < COOP_AS_NAME_COUNT ? coop_as_names[t->as[0]] : "";
    const char *idle = "Wait";
    if (!n0[0] || starts(n0, "Wait") || starts(n0, "Dead") || starts(n0, "Die") || starts(n0, "Horse") ||
        starts(n0, "Parashawl") || !strcmp(n0, "Fall") || !strcmp(n0, "SwimWait") || !strcmp(n0, "ClimbWait"))
        idle = n0;  // (already calm, or a state the avatar shows on its own: keep it)
    else if (starts(n0, "Swim") || starts(n0, "WaterDive")) idle = "SwimWait";
    else if (starts(n0, "Climb") || starts(n0, "WallOff")) idle = "ClimbWait";
    else if (starts(n0, "Ladder")) idle = "LadderWait";
    else if (starts(n0, "Squat")) idle = "SquatWait";
    else if (starts(n0, "Jump") || strstr(n0, "Fall") || starts(n0, "SkyDive")) idle = "Fall";
    // (2026-11-15, play report: the partner stood still while in the map / inventory) on the ground the avatar holds
    // up the Sheikah Slate instead of standing. SAVAGEWIND_COOP_PAUSE_PAD=0: plain Wait as before.
    // (2026-11-16, polish2: SheikPadLookPad, a cutscene AS, showed the arms hanging with the Slate at the side on the
    // avatar - a probe shots; its Demo_Common_* clips do not play there. The pose is now the one the player's
    // own tree sends while a rune is held ready: ItemCommonReady in 0.0 and 1.0 (the Slate raised in the left hand).
    // SAVAGEWIND_COOP_PAUSE_PAD_AS=<AS> picks another one, e.g. SheikPadLookPad as before.)
    static int pad_on = -1;
    static uint16_t pad_as;
    if (pad_on < 0) {
        pad_on = look_on("SAVAGEWIND_COOP_PAUSE_PAD");
        const char *e = getenv("SAVAGEWIND_COOP_PAUSE_PAD_AS");
        // (2026-11-19, user screenshot user_slate_pose_1118.webp: "bent with arms spread, not using the Slate")
        // ItemCommonReady is one clip, Item_IceMaker_Ready: the Cryonis stance. ItemScopeReady (clip Item_Scope_Ready,
        // in the player's own animations) holds the Slate up in front of the face like the real scope / camera.
        // SAVAGEWIND_COOP_SLATEPOSE=0: ItemCommonReady as before. (The SheikPad* AS use Demo_Common_* clips that do
        // not play on the avatar, polish2.)
        const char *def = look_on("SAVAGEWIND_COOP_SLATEPOSE") ? "ItemScopeReady" : "ItemCommonReady";
        pad_as = as_name_index(e && *e ? e : def);
        if (!pad_as) pad_as = as_name_index(def);
        if (!pad_as) pad_as = as_name_index("ItemCommonReady");
        LOG("[coop] tree: menu pose %s (SLATEPOSE)\n", pad_as ? coop_as_names[pad_as] : "-");
    }
    bool pad = pad_on && pad_as && (!strcmp(idle, "Wait") || (idle == n0 && starts(n0, "Wait")));
    if (pad) idle = coop_as_names[pad_dev.as[0] ? pad_dev.as[0] : pad_as];
    if (pad) astree_pad_until_ns = plat_now_ns() + 300000000ull;  // (avatar.inc: Slate in hand)
    uint16_t body = idle == n0 ? t->as[0] : as_name_index(idle);
    if (!body) return;  // (not in the table: leave it)
    uint16_t face = as_name_index("FaceDefault"), skin = t->as[4 * COOP_AS_LAYERS];
    for (int i = 0; i < ASTREE_N; i++) t->as[i] = 0;
    t->as[0] = t->as[1 * COOP_AS_LAYERS] = body;
    if (pad && pad_dev.as[0]) {  // (`coop padas`, dev: try other poses for the menu)
        if (pad_dev.as[1]) t->as[1 * COOP_AS_LAYERS] = pad_dev.as[1];
        t->as[1 * COOP_AS_LAYERS + 1] = pad_dev.as[2];
    }
    t->as[3 * COOP_AS_LAYERS] = face;
    t->as[4 * COOP_AS_LAYERS] = skin;
    for (int i = 0; i < 5; i++) t->f[i] = 0;  // (kinds 0..4: the sticks)
    memset(t->vel, 0, sizeof t->vel);
}

static void eat_publish(Cpu *c, CoopAsTree *t, bool paused);  // (protocol 81, coop/eat.inc) eating from the menu
static void champ_publish(Cpu *c, uint64_t pa, CoopAsTree *t);  // (protocol 89, coop/champ.inc) Champion abilities
static void astree_publish(Cpu *c, uint64_t pa) {
    CoopAsTree t;
    if (pa && astree_table_ready()) astree_build(c, pa, &t);
    else memset(&t, 0, sizeof t);
    static int settle_on = -1;
    if (settle_on < 0) settle_on = look_on("SAVAGEWIND_COOP_PAUSE_IDLE");
    static bool was_paused;
    bool paused = pa && settle_on && local_paused() && (t.flags & COOP_ASTREE_VALID);
    if (paused != was_paused) LOG("[coop] tree: game %s\n", paused ? "paused (menu): the avatar is sent idle" : "running again");
    was_paused = paused;
    if (paused) astree_settle(&t);
    if (t.flags & COOP_ASTREE_VALID) eat_publish(c, &t, paused);
    // `coop look tree <AS> [ms]` (dev, tests): the tree we send plays that AS in the body layers 0.0 and 1.0 for a
    // while (what an event's AS on the player sends, e.g. TalkHello: seen 2026-11-12, an NPC talk sends Talk / DemoTurn
    // in 0.0 and 1.0), started fresh each call (restart counter), then the player's own tree again.
    if (look_tree.as && plat_now_ns() < look_tree.until_ns && (t.flags & COOP_ASTREE_VALID)) {
        t.as[0] = t.as[1 * COOP_AS_LAYERS] = look_tree.as;
        t.as[1 * COOP_AS_LAYERS + 1] = 0;
        t.seq[0] = t.seq[1 * COOP_AS_LAYERS] = (uint8_t)(t.seq[0] + look_tree.seq);
    }
    // `coop look anim|upper|face` (dev, tests) force classified states: the other side must show those
    if (look_dev.anim >= 0 || look_dev.anim_upper >= 0 || look_dev_face >= 0) {
        t.flags = 0, t.fx_mask = 0;  // (no FX flag: no values either, coop_astree_ok)
        memset(t.fx, 0, sizeof t.fx);
    }
    champ_publish(c, pa, &t);  // (last: only with a valid tree, coop_champ_ok)
    static uint16_t last[ASTREE_N];
    if (memcmp(last, t.as, sizeof last)) {
        char names[400];
        astree_names(&t, names, sizeof names);
        LOG("[coop] tree sent:%s\n", names);
        memcpy(last, t.as, sizeof last);
    }
    astree_sent = t;
    coop_net_set_astree(&t);
}

// -- the receiver
#define ASTREE_TARGETS (1 + COOP_ARMOR_SLOTS)
static struct {
    bool active;                 // the avatar plays the trees now
    uint16_t as[ASTREE_N];       // what the avatar plays per slot / layer
    uint8_t seq[ASTREE_N];
    CoopAsTree rx;               // the inputs it gets (also written again after the NPC's own update, hook below)
    volatile uint64_t targets[ASTREE_TARGETS];  // the avatar and its armor puppets (read by the hook)
    volatile int have;
    volatile unsigned hook_hits;
    const char *clip_seen[ASTREE_TARGETS][COOP_AS_SLOTS][COOP_AS_LAYERS];
    uint8_t clip_check[ASTREE_TARGETS][COOP_AS_SLOTS][COOP_AS_LAYERS], clip_replayed[ASTREE_TARGETS][COOP_AS_SLOTS][COOP_AS_LAYERS];
    const char *clip_back[ASTREE_TARGETS][COOP_AS_SLOTS][COOP_AS_LAYERS];
    struct { bool active, had; uint8_t seq, kind; uint64_t start_ns; } eat;  // (protocol 81, coop/eat.inc)
} astree;

static void astree_forget_target(uint64_t actor) {
    for (int i = 1; i < ASTREE_TARGETS; i++)
        if (astree.targets[i] == actor) astree.targets[i] = 0;
}

static void astree_write(uint64_t actor, const CoopAsTree *t) {
    uint64_t h = asin_holder(actor);
    if (!h) return;
    for (int i = 0; i < COOP_ASIN_FLOATS; i++) {
        uint64_t s = asin_slot(h, astree_float_kinds[i]);
        if (s) memcpy((void *)(uintptr_t)s, &t->f[i], 4);
    }
    uint64_t s = asin_slot(h, 21), v = s ? LD64(s) : 0;
    if (in_guest(v)) memcpy((void *)(uintptr_t)v, t->normal, 12);
    s = asin_slot(h, 66), v = s ? LD64(s) : 0;
    if (in_guest(v)) ST64(v, t->bools);
    if ((s = asin_slot(h, 42))) ST32(s, (uint32_t)(int32_t)t->charge);
    if ((s = asin_slot(h, 43))) ST32(s, (uint32_t)(int32_t)t->combo);
    for (int i = 0; i < COOP_ASIN_STRS; i++)
        if ((s = asin_slot(h, astree_str_kinds[i])) && t->str[i] < COOP_AS_STRING_COUNT) asin_set_str(s, coop_as_strings[t->str[i]]);
    memcpy((void *)(uintptr_t)(actor + ACTOR_VEL), t->vel, 12);
}

// The NPC class's own per-frame AS input update (vtable+0x260 of 0x7001bc5f28, the class of the avatar and its
// puppets; seen live 2026-11-10: it sets Weapon to "" and Bool bits 29/30 every frame, also GroundNormal and bits
// 13/18/28): wrapped, so that the avatar's inputs are ours again right after it (coop_hooks_init).
#define A_NPC_AS_INPUTS 0x7000585a3cull
static GuestFn astree_npc_orig;
static void horse_npc_inputs(uint64_t a);  // (coop/horse.inc: the horse puppets, same NPC class)
static void multi_npc_inputs(uint64_t a);  // (coop/multi.inc: the same for the extra players' avatars)
static void astree_npc_hook(Cpu *c) {
    uint64_t a = c->x[0];
    astree_npc_orig(c);
    horse_npc_inputs(a);
    multi_npc_inputs(a);  // (coop-multi: the extra players' avatars, coop/multi.inc)
    if (!astree.have || !a) return;
    for (int i = 0; i < ASTREE_TARGETS; i++)
        if (astree.targets[i] == a) {
            bool live = i ? armor_pup[i - 1].actor == a && pup_alive(&armor_pup[i - 1])
                          : avatar.actor == a && avatar_alive();
            if (!live) continue;
            astree_write(a, &astree.rx);
            if (i == 0) xl_who = "hook", fx_write(a, &astree.rx), xl_who = "frame", snd_write(a, &astree.rx);  // (effect / sound sets: avatarfx/avatarsnd.inc)
            astree.hook_hits++;
            break;
        }
}
// The NodePos selector (Jump, Land, Wait, Turn, Brake, climbing...: "the child whose bone, e.g. Ankle_R / Ankle_L, is
// most forward"; FUN_71014b587c, only reached through the element's vtable) measures those bones on the actor's own
// model. An armor puppet's model (a shirt, a pair of trousers) has no ankle bones: the selector returns -1 there, the
// AS plays nothing and the puppet keeps its last pose while the avatar jumps (seen 2026-11-10: the shirt stays at the
// standing pose, the head floats above the collar). Wrapped: a -1 takes the choice made just before (the avatar
// starts each AS one call before its puppets, in the same frame, coop/astree.inc astree_play_all), else child 0.
// 2026-11-12: "the choice made just before" could be ANY actor's (the local player's own Link, an NPC) and the puppet
// then plays another foot / turn variant than the avatar (reported: the head came off the body when running in tight
// circles close by; Turn / Brake / Wait pick by NodePos). The selector's model (x1 ctx: *(*ctx + 8), FUN_71014a4e84)
// is compared with the avatar's model (actor+0x4e0): the avatar's own choice is kept apart, and a puppet's -1 takes
// that (fresh, < 250 ms, and a valid child of this selector). Until the avatar's model was seen once, the old rule.
// SAVAGEWIND_COOP_NODEPOS_AVATAR=0: the old rule only.
#define A_NODEPOS_SELECT 0x70014b587cull
static GuestFn nodepos_orig;
static struct { int last; uint64_t last_ns; unsigned fixed; } nodepos = {.last = -1};
static struct NodePosAvatar {
    int choice;
    uint64_t at_ns;
    bool seen;
    unsigned probe;
} nodepos_avatar = {.choice = -1};
static const struct NodePosAvatar nodepos_avatar_initial = {.choice = -1};
static int nodepos_av_on = -1;  // (set on the game thread before the hook can see a puppet: astree_frame)
static uint64_t nodepos_model(uint64_t ctx) {
    uint64_t p = in_guest(ctx) ? LD64(ctx) : 0;
    uint64_t m = in_guest(p) ? LD64(p + 8) : 0;
    return in_guest(m) ? m : 0;
}
static void nodepos_hook(Cpu *c) {
    uint64_t sel = c->x[0], ctx = c->x[1];
    nodepos_orig(c);
    int r = (int)(uint32_t)c->x[0];
    uint64_t now = plat_now_ns();
    uint64_t av = astree.have && nodepos_av_on > 0 && astree.targets[0] == avatar.actor && avatar_alive() ? avatar.actor : 0;
    uint64_t avm = av ? LD64(av + ACTOR_MODEL) : 0, m = av ? nodepos_model(ctx) : 0;
    if (r >= 0) {
        nodepos.last = r, nodepos.last_ns = now;
        if (avm && m == avm) {
            nodepos_avatar.choice = r, nodepos_avatar.at_ns = now;
            if (!nodepos_avatar.seen) { nodepos_avatar.seen = true; LOG("[coop] tree: NodePos on the avatar's own model seen (child %d): puppets follow it\n", r); }
        } else if (av && !nodepos_avatar.seen && nodepos_avatar.probe < 6 && coop_look_log()) {
            nodepos_avatar.probe++;
            LOG("[coop] tree: NodePos model %llx (avatar model %llx) child %d\n", (unsigned long long)m, (unsigned long long)avm, r);
        }
        return;
    }
    int n = in_guest(sel) ? (int)LD32(sel + 0x18) : 0;
    if (n <= 0 || !astree.have) return;
    int pick;
    if (nodepos_avatar.seen && av)
        pick = nodepos_avatar.choice >= 0 && nodepos_avatar.choice < n && now - nodepos_avatar.at_ns < 250000000ull ? nodepos_avatar.choice : 0;
    else
        pick = nodepos.last >= 0 && nodepos.last < n && now - nodepos.last_ns < 50000000ull ? nodepos.last : 0;
    c->x[0] = (uint64_t)(uint32_t)pick;
    if (nodepos.fixed++ < 20 && coop_look_log()) LOG("[coop] tree: NodePos found no bone, child %d taken%s\n", pick, nodepos_avatar.seen ? " (the avatar's)" : "");
}

static void astree_hooks_init(void) {
    if (LD32(A_NODEPOS_SELECT) != 0xd10303ffu) LOG("[coop] tree mode: NodePos selector differs, not wrapped\n");
    else if (!(nodepos_orig = dispatch_lookup(A_NODEPOS_SELECT))) LOG("[coop] tree mode: NodePos selector not found\n");
    else hle_override(A_NODEPOS_SELECT, nodepos_hook, "coop tree mode NodePos selector");
    if (LD32(A_NPC_AS_INPUTS) != 0xd10143ffu) { LOG("[coop] tree mode: NPC input update differs, not wrapped\n"); return; }
    if (!(astree_npc_orig = dispatch_lookup(A_NPC_AS_INPUTS))) { LOG("[coop] tree mode: NPC input update not found\n"); return; }
    hle_override(A_NPC_AS_INPUTS, astree_npc_hook, "coop tree mode NPC inputs");
}

// The name an AS of the player's list has on the avatar and its puppets: the Face* / Voice* ones are CoopX<name> there
// (Co-op/build_overlay.py: the player's AS events start those by name and, on this NPC class, put them in the
// upper-body slot; renamed, the events find nothing and we play them in slot 3 ourselves).
static const char *avatar_as_alias(const char *n) {
    static char buf[4][80];
    static int k;
    if (!starts(n, "Face") && !starts(n, "Voice")) return n;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "CoopX%.70s", n);
    return buf[k];
}

// (2026-11-18, guard-down) The armor puppets' "Wait" never takes Player_Wait.bas's bone-tested transition
// Nml_LockonBattleToWait (they have no Toe_R and took it always); "CoopArmWaitT" (overlay, CoopArmor list) always
// does. A puppet playing CoopArmWaitT while the avatar plays "Wait" counts as playing the same AS.
static bool puppet_as_same(const char *now, const char *want) {
    return !strcmp(now, want) || (!strcmp(want, "Wait") && !strcmp(now, "CoopArmWaitT"));
}

static void astree_play_all(Cpu *c, const char *name, int s, int l) {
    if (name) name = avatar_as_alias(name);
    for (int k = 0; k < ASTREE_TARGETS; k++)
        if (astree.targets[k]) {
            if (name) play_anim_l(c, astree.targets[k], name, s, l);
            else stop_anim_l(c, astree.targets[k], s, l);
        }
}

// An upper-body loop of sub-layer 1.1 that runs alongside the walk / idle of 0.0 (MoveUpper, WaitUpper,
// MoveAttentionUpper, SquatMoveUpper...): started together with the body layers (astree_frame).
static bool astree_upper_loop(const char *n) {
    size_t k = strlen(n);
    return k > 5 && !strcmp(n + k - 5, "Upper") && (starts(n, "Move") || starts(n, "Wait") || starts(n, "Squat"));
}

// Shield guard / lock-on (2026-11-16, play report: "shield block transition desynced: legs, torso and head
// separate"). WaitAttentionUpper / MoveAttentionUpper (1.1) are the ONLY AS with the guard and lock-on upper-body
// clips (Shield_Guard_Wait, Sword_Guard_Wait, *_Lockon_Wait; WaitAttention / MoveAttention in 0.0 / 1.0 have none, AS
// data checked). The player plays them like every 1.1 AS through FUN_7100cc80c8: 1.1 weight 1, 1.0 weight 0 (stopped
// by FUN_7100cc8224: 1.0 back to 1); while standing (FUN_7100ce1714 -> FUN_7100cc8078, every frame) 0.0 and 1.0 take
// 1.1's phase; and 1.1 takes 0.0's phase (FUN_7100cc8350) only when the game's own check on the 1.1 layer says so
// (FUN_71014a2618(as, 1, 1, FUN_71014bc108): the instance's vfunc +0xd8). All Ghidra-read. The avatar did none of
// that: 1.0 (no guard) stayed at weight 1 over the guard, 1.1 was forced to the legs' phase every frame (also mid
// raise), and raising the shield (1.1 starting alone) restarted the legs and 1.0.
// SAVAGEWIND_COOP_GUARDSYNC=0: the old behaviour (attention uppers treated as plain loops).
static bool astree_attn_upper(const char *n) {
    size_t k = strlen(n);
    return k > 14 && !strcmp(n + k - 14, "AttentionUpper");
}
static bool astree_guardsync_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_GUARDSYNC"),
                LOG("[coop] tree: shield guard / lock-on upper body like the player (weights, phase order): %s\n", on ? "on" : "off");
    return on;
}

// The calm loop of an AS's family (for a body layer whose one-shot ended while the sender still reports it).
static const char *astree_family_idle(const char *n) {
    if (starts(n, "WaterDive")) return "Fall";  // (the dive into water: still in the air)
    if (starts(n, "Swim")) return "SwimWait";
    if (starts(n, "Climb") || starts(n, "WallOff")) return "ClimbWait";
    if (starts(n, "Ladder")) return "LadderWait";
    if (starts(n, "Squat")) return "SquatWait";
    if (starts(n, "Jump") || strstr(n, "Fall") || starts(n, "SkyDive") || starts(n, "Parashawl")) return "Fall";
    return "Wait";
}

// A one-shot that ENDED on the avatar while the sender still plays it (2026-11-11, user: swimming and pressing jump
// again and again, the avatar floated over the water arms spread: Link's bind pose). Seen in a probe (2026-11-11
// evening): the single-clip swim dash (Swim_Move_Dash) played once on the avatar showed the bind pose from ~0.4 s on,
// and in tree mode a swim dash showed the dash, then the bind pose. That an ended AS leaves its layer record without an
// instance (+0x18 == 0) is what the code above assumes ("an ended one is left alone"), not re-checked. The sender's
// copy goes on: SwimDash pressed again restarts the SAME AS (the restart counter `seq` was never seen to fire in any
// log: the game seems to reuse the instance), or the player code keeps an AS whose clip is over. So an ended body
// layer (0.0 / 1.0) is played again, both together, at
// most twice per received AS; after that it gets the calm loop of its family until the sender changes state.
// (A grace of 3 frames first: an ordinary one-shot can end a frame or two before the sender's next state arrives.)
#define ASTREE_END_GRACE 3
static struct { uint16_t as; uint8_t seq, replays, ended_frames; bool idle; } astree_end;
static void astree_ended_guard(Cpu *c) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_ENDGUARD");
    uint64_t a = astree.targets[0];
    int l0 = 0, u0 = 1 * COOP_AS_LAYERS;
    if (!on || !a || !astree.as[l0]) return;
    if (astree_end.as != astree.as[l0] || astree_end.seq != astree.seq[l0])
        astree_end.as = astree.as[l0], astree_end.seq = astree.seq[l0], astree_end.replays = 0, astree_end.idle = false,
        astree_end.ended_frames = 0;
    if (astree_end.idle) return;
    uint64_t r0 = as_layer_rec(a, 0, 0), r1 = as_layer_rec(a, 1, 0);
    bool ended0 = r0 && !LD64(r0 + 0x18), ended1 = astree.as[u0] && r1 && !LD64(r1 + 0x18);
    if (!ended0 && !ended1) { astree_end.ended_frames = 0; return; }
    if (++astree_end.ended_frames < ASTREE_END_GRACE) return;
    astree_end.ended_frames = 0;
    const char *n = coop_as_names[astree.as[l0]];
    static unsigned said;
    if (astree_end.replays < 2 && !starts(n, "WaterDive")) {
        astree_end.replays++;
        if (said++ < 60) LOG("[coop] tree: %s ended on the avatar while still sent: played again (%u)\n", n, astree_end.replays);
        astree_play_all(c, n, 0, 0);
        if (astree.as[u0]) astree_play_all(c, coop_as_names[astree.as[u0]], 1, 0);
        return;
    }
    const char *idle = astree_family_idle(n);
    if (said++ < 60) LOG("[coop] tree: %s ended again: the avatar holds %s until the next state\n", n, idle);
    astree_end.idle = true;
    astree_play_all(c, idle, 0, 0);
    astree_play_all(c, idle, 1, 0);
}

// Phase lock (2026-11-12, user: in the slow walk the waist / head move out of step with the legs, "joints off"). The
// player keeps its layers in step itself every frame: its Move state (FUN_7100cf1610 -> FUN_7100cc8350) syncs 1.1
// (MoveUpper / WaitUpper...) to 0.0's phase, the attention states sync 1.0 / 0.0 to 1.1 (FUN_7100cc8078), and an
// armor piece syncs its own AS to its wearer's slot (FUN_710117282c). All through FUN_71014a21dc(asDst, asSrc, slotDst,
// slotSrc, layerDst, layerSrc) -> FUN_71014bc59c: the source instance's normalized phase set on the destination
// (Ghidra-verified; checks slot / layer counts and empty layers itself). The avatar never ran that, so its layers and
// its armor puppets' copies drift apart (separate instances, started a packet apart or on different frame steps).
// Now each frame: avatar 1.0 <- 0.0 when both play the same AS, avatar 1.1 <- 0.0 when 1.1 is an upper-body loop,
// then each armor puppet's layer <- the avatar's same layer when both play the same AS.
// SAVAGEWIND_COOP_PHASELOCK=0: off.
#define A_AS_SYNC_LAYER 0x70014a21dcull
static void as_sync_layer(Cpu *c, uint64_t dst, uint64_t src, int sd, int ss, int ld, int ls) {
    uint64_t asd = LD64(dst + ACTOR_AS), ass = LD64(src + ACTOR_AS);
    if (!in_guest(asd) || !in_guest(ass)) return;
    Cpu saved = *c;
    c->x[0] = asd, c->x[1] = ass, c->x[2] = (uint64_t)sd, c->x[3] = (uint64_t)ss, c->x[4] = (uint64_t)ld, c->x[5] = (uint64_t)ls;
    guest_call(c, A_AS_SYNC_LAYER);
    *c = saved;
}
// The game's check before it gives 1.1 the phase of 0.0 (Move state FUN_7100cf1610, attention FUN_7100cf1adc:
// FUN_71014a2618(as, 1, 1, FUN_71014bc108, 0) = FUN_71014bc108(layer record): the playing instance's vfunc +0xd8
// (inst, res, FUN_71014a4d3c(res)); false without an instance). What it tests inside the instance is not known.
// SAVAGEWIND_COOP_PHASEGATE=0: 1.1 always synced (as before 2026-11-16).
#define A_AS_LAYER_SYNC_OK 0x70014bc108ull
static bool as_layer_sync_ok(Cpu *c, uint64_t rec) {
    uint64_t inst = rec ? LD64(rec + 0x18) : 0, res = rec ? LD64(rec) : 0;
    if (!in_guest(inst) || !in_guest(res) || !in_guest(LD64(inst))) return false;
    uint64_t fn = LD64(LD64(inst) + 0xd8);
    if (fn < 0x7000000000ull || fn >= 0x7003000000ull) return false;
    Cpu saved = *c;
    c->x[0] = rec;
    guest_call(c, A_AS_LAYER_SYNC_OK);
    bool ok = (c->x[0] & 0xff) != 0;
    *c = saved;
    return ok;
}
static void astree_phase_lock(Cpu *c) {
    static int on = -1, gate = -1;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_PHASELOCK");
        if (on && LD32(A_AS_SYNC_LAYER) != 0xb940b808u) { on = 0; LOG("[coop] tree: layer phase sync function differs: phase lock off\n"); }
        LOG("[coop] tree: phase lock (avatar layers + armor puppets kept in step) %s\n", on ? "on" : "off");
        gate = look_on("SAVAGEWIND_COOP_PHASEGATE");
        if (gate && LD32(A_AS_LAYER_SYNC_OK) != 0xa9be7bfdu) { gate = 0; LOG("[coop] tree: layer sync check differs: phase gate off\n"); }
        LOG("[coop] tree: 1.1 takes 0.0's phase only when the game's check allows (phase gate) %s\n", gate ? "on" : "off");
    }
    uint64_t a = astree.targets[0];
    if (!on || !a) return;
    const char *n00 = as_layer_name(as_layer_rec(a, 0, 0)), *n10 = as_layer_name(as_layer_rec(a, 1, 0));
    uint64_t r11 = as_layer_rec(a, 1, 1);
    const char *n11 = as_layer_name(r11);
    // (guard sync, astree_attn_upper above) standing in the attention / guard pose the legs and 1.0 follow 1.1
    bool attn_wait = astree_guardsync_on() && !strcmp(n00, "WaitAttention") && !strcmp(n11, "WaitAttentionUpper");
    if (attn_wait) {
        if (n10[0]) as_sync_layer(c, a, a, 1, 1, 0, 1);  // (1.0 <- 1.1, FUN_7100cc8078)
        as_sync_layer(c, a, a, 0, 1, 0, 1);              // (0.0 <- 1.1)
    } else if (n00[0] && !strcmp(n00, n10))
        as_sync_layer(c, a, a, 1, 0, 0, 0);
    if (n00[0] && n11[0] && astree_upper_loop(n11)) {
        bool ok = !gate || as_layer_sync_ok(c, r11);
        static char said_n[48];
        static int said_ok = -1;
        static unsigned said;
        if ((ok != said_ok || strncmp(said_n, n11, sizeof said_n - 1)) && (said++ < 60 || coop_look_log()))
            LOG("[coop] tree: phase gate: 1.1 %s %s 0.0's phase (%s)\n", n11, ok ? "takes" : "keeps its own, not", n00);
        said_ok = ok, snprintf(said_n, sizeof said_n, "%s", n11);
        if (ok) as_sync_layer(c, a, a, 1, 0, 1, 0);
    }
    static unsigned locks;
    // Same AS, different clip (2026-11-18, guard-down probe: the head / upper armor puppets play "Wait" 1.0 as the
    // 10-frame transition Nml_LockonBattleToWait (rate 0.4) after WaitAttention while the avatar, the lower puppet and
    // the real player play the 48-frame idle: Player_Wait.bas picks the transition by previous AS + a bone test
    // (Skl_Root,Z / Toe_R,Z), which the upper puppets answer differently. With the phase lock the transition then ran
    // ~5x slower: torso / head bent for ~75 frames while the legs stood up in ~14). For the first frames of a new AS on
    // a puppet layer (body slots 0 / 1) the clip end and rate are compared with the avatar's; on a difference the
    // puppet plays CoopWait (one Nml_Wait clip) for one frame and then the AS again, once: with a previous AS that is
    // not WaitAttention the tree takes the plain branch. (Playing the same AS again directly was tried first, probe
    // g3: the puppets picked the same transition again.)
    // SAVAGEWIND_COOP_CLIPMATCH=0: off.
    static int clipmatch = -1;
    if (clipmatch < 0) clipmatch = look_on("SAVAGEWIND_COOP_CLIPMATCH");
    static unsigned clip_said;
    for (int k = 1; k < ASTREE_TARGETS; k++) {
        uint64_t p = astree.targets[k];
        if (!p) {
            memset(astree.clip_back[k], 0, sizeof astree.clip_back[k]);
            continue;
        }
        for (int s = 0; s < COOP_AS_SLOTS; s++)
            for (int l = 0; l < COOP_AS_LAYERS; l++) {
                const char *na = as_layer_name(as_layer_rec(a, s, l));
                if (astree.clip_back[k][s][l]) {  // (the frame after CoopWait: the AS again, if the avatar still plays it)
                    const char *b = astree.clip_back[k][s][l];
                    astree.clip_back[k][s][l] = NULL;
                    if (b == na && strcmp(as_layer_name(as_layer_rec(p, s, l)), na)) {
                        play_anim_l(c, p, na, s, l);
                        astree.clip_check[k][s][l] = 3;
                        continue;
                    }
                }
                if (!na[0] || !puppet_as_same(as_layer_name(as_layer_rec(p, s, l)), na)) continue;
                if (clipmatch && astree.clip_seen[k][s][l] != na)
                    astree.clip_seen[k][s][l] = na, astree.clip_check[k][s][l] = 4, astree.clip_replayed[k][s][l] = 0;
                if (clipmatch && astree.clip_check[k][s][l] && s <= 1) {
                    astree.clip_check[k][s][l]--;
                    uint64_t ta = as_layer_time(c, as_layer_rec(a, s, l)), tp = as_layer_time(c, as_layer_rec(p, s, l));
                    float fa[6], fp[6];
                    if (ta && tp) {
                        memcpy(fa, (void *)(uintptr_t)ta, sizeof fa), memcpy(fp, (void *)(uintptr_t)tp, sizeof fp);
                        if ((fabsf(fa[5] - fp[5]) > 0.5f || fabsf(fa[3] - fp[3]) > 0.01f) && !astree.clip_replayed[k][s][l]) {
                            // The avatar's body took the transition (shorter clip) and the puppet's "Wait" (overlay
                            // copy) never does: the puppet plays the copy that always does (SAVAGEWIND_COOP_ARMWAITT=0
                            // off; with an old overlay without it the CoopWait replay below).
                            static int armwait_t = -1;
                            if (armwait_t < 0) armwait_t = look_on("SAVAGEWIND_COOP_ARMWAITT");
                            if (armwait_t && !strcmp(na, "Wait") && fa[5] + 0.5f < fp[5]) {
                                play_anim_l(c, p, "CoopArmWaitT", s, l);
                                if (!strcmp(as_layer_name(as_layer_rec(p, s, l)), "CoopArmWaitT")) {
                                    if (clip_said++ < 60 || coop_look_log())
                                        LOG("[coop] tree: clip match: puppet %d layer %d.%d Wait plays a %.0f-frame clip, the avatar the %.0f-frame transition: CoopArmWaitT\n",
                                            k, s, l, fp[5], fa[5]);
                                    astree.clip_replayed[k][s][l] = 1, astree.clip_check[k][s][l] = 0;
                                    as_sync_layer(c, p, a, s, s, l, l);
                                    continue;
                                }
                            }
                            if (clip_said++ < 60 || coop_look_log())
                                LOG("[coop] tree: clip match: puppet %d layer %d.%d %s plays a %.0f-frame clip (rate %.2f), the avatar %.0f (%.2f): played again after CoopWait\n",
                                    k, s, l, na, fp[5], fp[3], fa[5], fa[3]);
                            astree.clip_replayed[k][s][l] = 1, astree.clip_check[k][s][l] = 0;
                            play_anim_l(c, p, "CoopWait", s, l);
                            astree.clip_back[k][s][l] = na;
                            continue;
                        }
                    }
                }
                as_sync_layer(c, p, a, s, s, l, l);
                if (!locks++) LOG("[coop] tree: phase lock: first armor puppet layer %d.%d (%s) synced to the avatar\n", s, l, na);
            }
    }
}

// Avatar-side AS mapping (2026-11-12). Some of the player's AS look wrong on the avatar:
// - SwimDash (one 60-frame clip, Swim_Move_Dash): swimming and pressing dash again and again, one saw the avatar
//   float in the bind pose (also seen in a probe 2026-11-11). Why the clip ends in the bind pose there is NOT known
//   (the ended-AS guard never fired in the play-test logs). The avatar plays SwimMove (the swim stroke loop) at full
//   stick instead: a fast crawl, no dash kick, never the bind pose.
// - SlippingDown (after a knock-down, DamageL): one static 1-frame pose "Rolldown", the target pose of the player's
//   ragdoll; without the ragdoll the avatar holds that stiff pose in the air (it looked ragdoll-like). The avatar
//   plays SlippingDownLand instead (the lying poses Rolldown_R/U/L/D by UserAngle), so it shows fly back (DamageL),
//   lie on the ground, get up (DamageLStand).
// A mapped layer keeps its restart counter at 0 (a dash pressed again does not restart the stroke).
// SAVAGEWIND_COOP_ASMAP=0: off (the player's AS as sent).
static void astree_map(CoopAsTree *t) {
    static int on = -1;
    static uint16_t swimdash, swimmove, slip, slipland;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_ASMAP");
        swimdash = as_name_index("SwimDash"), swimmove = as_name_index("SwimMove");
        slip = as_name_index("SlippingDown"), slipland = as_name_index("SlippingDownLand");
        LOG("[coop] tree: AS mapping (SwimDash -> SwimMove, SlippingDown -> SlippingDownLand) %s\n", on ? "on" : "off");
    }
    if (!on) return;
    static unsigned said;
    for (int s = 0; s <= 1; s++) {
        int i = s * COOP_AS_LAYERS;
        uint16_t from = t->as[i], to = 0;
        // (2026-11-15: while swimming faster the partner's Link used the normal swim animation, only faster.
        // The bind-pose problem that
        // made this mapping was seen before the restart rule and the ended-AS guard above existed: the avatar plays
        // SwimDash again. SAVAGEWIND_COOP_SWIMDASHMAP=1: the old SwimMove stand-in, if the bind pose comes back.)
        static int dashmap = -1;
        if (dashmap < 0) dashmap = getenv("SAVAGEWIND_COOP_SWIMDASHMAP") && getenv("SAVAGEWIND_COOP_SWIMDASHMAP")[0] == '1';
        if (dashmap && swimdash && swimmove && from == swimdash) to = swimmove, t->f[0] = 1.0f;  // (kind 0 StickValue: the fast stroke)
        else if (slip && slipland && from == slip) to = slipland;
        if (!to) continue;
        // (the receiver's restart counter stays as it is: a dash restarted, or SwimMove -> SwimDash, does not restart the stroke)
        t->as[i] = to, t->seq[i] = astree.as[i] == to ? astree.seq[i] : 0;
        if (said++ < 40 && s == 0) LOG("[coop] tree: avatar plays %s for %s\n", coop_as_names[to], coop_as_names[from]);
    }
}

// Upper-body actions in layer 1.1 (2026-11-12). The whistle (HorseCall) and the bow aim (BowCharge), and in the logs
// also BowShoot, BowReload, Get (item pick-up), WeaponThrowCharge, ParaEquipOff, ItemBomb*, DamageSUpper, play in the
// sub-layer 1.1 over 1.0. On the avatar they showed nothing (screenshots: no hand at the mouth, the bow held low).
// Layer dump: on the player the 1.0 record's weight (+0x10) is 0 while 1.1 plays; on the avatar it stays 1.0, so 1.0
// covers 1.1. VERIFIED live: writing 0 there on the avatar during a bow aim raised the bow (sheet_os_w0.png).
// (What sets it to 0 on the player is not known.) So while a target's 1.1 plays such an action, its 1.0 weight is 0;
// back to 1 when the action is over. The upper-body loops (WaitUpper, MoveUpper, *AttentionUpper, SquatUpper) are left
// as they were (the avatar's idle / walk look stays the same). SAVAGEWIND_COOP_UPPER11=0: off.
#define AS_REC_WEIGHT 0x10
static void astree_upper_weight(void) {
    static int on = -1;
    // (coop-multi: static bool zeroed[ASTREE_TARGETS]; -> per avatar, avl)
    // (coop-multi: static uint64_t was[ASTREE_TARGETS]; -> per avatar, avl)
    if (on < 0) on = look_on("SAVAGEWIND_COOP_UPPER11"),
                LOG("[coop] tree: upper-body actions of 1.1 shown (1.0 weight 0 under them): %s\n", on ? "on" : "off");
    if (!on) return;
    static unsigned said;
    for (int k = 0; k < ASTREE_TARGETS; k++) {
        uint64_t a = astree.targets[k];
        if (a != avl.uw_was[k]) avl.uw_zeroed[k] = false, avl.uw_was[k] = a;  // (a new puppet: nothing of ours to undo)
        if (!a) continue;
        uint64_t r0 = as_layer_rec(a, 1, 0), r1 = as_layer_rec(a, 1, 1);
        if (!r0 || !r1) continue;
        const char *n1 = as_layer_name(r1);
        // (guard sync: the shield guard / lock-on of *AttentionUpper shows only with 1.0 at weight 0, as on the player)
        bool action = n1[0] && (!astree_upper_loop(n1) || (astree_guardsync_on() && astree_attn_upper(n1))) && as_layer_name(r0)[0];
        float w;
        memcpy(&w, (void *)(uintptr_t)(r0 + AS_REC_WEIGHT), 4);
        if (action && w != 0.0f) {
            float z = 0.0f;
            memcpy((void *)(uintptr_t)(r0 + AS_REC_WEIGHT), &z, 4);
            if (!avl.uw_zeroed[k] && (said++ < 100 || coop_look_log()))
                LOG("[coop] tree: %llx shows 1.1 %s (1.0 weight %.2f -> 0)\n", (unsigned long long)a, n1, w);
            avl.uw_zeroed[k] = true;
        } else if (!action && avl.uw_zeroed[k]) {
            float one = 1.0f;
            if (w == 0.0f) memcpy((void *)(uintptr_t)(r0 + AS_REC_WEIGHT), &one, 4);
            avl.uw_zeroed[k] = false;
            if (said++ < 100 || coop_look_log()) LOG("[coop] tree: %llx 1.1 action over (1.0 weight back to 1)\n", (unsigned long long)a);
        }
    }
}

// Horseback bow aim (protocol 46, 2026-11-14; only the head and the bow moved, the rest of the body did not
// follow). On the player, FUN_7100cc836c plays AimHorseLowerF/B/... in layer 0.1 and
// sets the record weights 0.0 = 0, 0.1 = 1 (FUN_7100cc846c puts back 0.0 = 1, 0.1 = 0); every frame the game sets the
// frame of that Rate 0 clip from the aim azimuth (the body twist). On the avatar the clip stayed at frame 0 under a
// full-weight 0.0. So while 0.1 plays AimHorseLower*: the sender's frame, and the same weights (ours put back after).
// SAVAGEWIND_COOP_HORSEAIM=0: off.
static void as_set_frame(Cpu *c, uint64_t actor, int slot, int layer, float frame);  // (coop/horse.inc)
static void astree_horse_aim(Cpu *c, const CoopAsTree *t) {
    static int on = -1;
    // (coop-multi: static bool set[ASTREE_TARGETS]; -> per avatar, avl)
    // (coop-multi: static uint64_t was[ASTREE_TARGETS]; -> per avatar, avl)
    // (coop-multi: static float w01_was[ASTREE_TARGETS]; -> per avatar, avl)
    if (on < 0) on = look_on("SAVAGEWIND_COOP_HORSEAIM"), LOG("[coop] tree: horseback bow body twist %s\n", on ? "on" : "off");
    unsigned v = t ? t->horse_aim[0] | (unsigned)t->horse_aim[1] << 8 : 0;
    static unsigned said;
    for (int k = 0; k < ASTREE_TARGETS; k++) {
        uint64_t a = astree.targets[k];
        if (a != avl.ha_was[k]) avl.ha_set[k] = false, avl.ha_was[k] = a;
        if (!a) continue;
        uint64_t r0 = as_layer_rec(a, 0, 0), r1 = as_layer_rec(a, 0, 1);
        if (!r0 || !r1) continue;
        bool aim = on && v && starts(as_layer_name(r1), "AimHorseLower");
        float zero = 0.0f, one = 1.0f;
        if (aim) {
            if (!avl.ha_set[k]) {
                memcpy(&avl.ha_w01_was[k], (void *)(uintptr_t)(r1 + AS_REC_WEIGHT), 4);
                avl.ha_set[k] = true;
                if (said++ < 40 || coop_look_log())
                    LOG("[coop] tree: %llx horseback aim %s frame %.1f (0.0 weight -> 0, 0.1 -> 1)\n", (unsigned long long)a,
                        as_layer_name(r1), (v - 1) / 64.0f);
            }
            memcpy((void *)(uintptr_t)(r0 + AS_REC_WEIGHT), &zero, 4);
            memcpy((void *)(uintptr_t)(r1 + AS_REC_WEIGHT), &one, 4);
            as_set_frame(c, a, 0, 1, (v - 1) / 64.0f);
        } else if (avl.ha_set[k]) {
            memcpy((void *)(uintptr_t)(r0 + AS_REC_WEIGHT), &one, 4);
            memcpy((void *)(uintptr_t)(r1 + AS_REC_WEIGHT), &avl.ha_w01_was[k], 4);
            avl.ha_set[k] = false;
            if (said++ < 40 || coop_look_log()) LOG("[coop] tree: %llx horseback aim over (weights back)\n", (unsigned long long)a);
        }
    }
}

#include "eat.inc"  // (protocol 81) eating / drinking from the menu shown on the avatar

// Each frame (game thread, from avatar_frame once the avatar exists): true while the avatar plays the trees.
// resync: replay every slot on everyone in the same frame (a new armor puppet is ready).
static bool astree_frame(Cpu *c, bool resync) {
    CoopAsTree t;
    double age = 0;
    bool use = false;
    if (asdev.mirror) t = astree_sent, use = (t.flags & COOP_ASTREE_VALID) != 0;  // (dev: our own player)
    else use = astree_on() && coop_net_remote_astree(&t, &age) && age < 1.0 && (t.flags & COOP_ASTREE_VALID);
    // (dying: the classified death keeps its timing and pose, COOP_ANIM_DIE)
    if (use && t.as[0] && (starts(coop_as_names[t.as[0]], "Dead") || starts(coop_as_names[t.as[0]], "Die"))) use = false;
    // (2026-11-13: when someone talked with an NPC the animations stopped and Link fell into a T-pose-like state)
    // In talks and cutscenes the event drives Link with its own scene ASes ("C01-Link-A-0", "Demo008_2-C01-Link-A-0",
    // or none): not in the name table, so the body slot came as 0 and every body layer was stopped = bind pose.
    // Then use the classified state instead (CoopWait / CoopTalk / CoopDemoTurn...).
    if (use && !t.as[0]) use = false;
    astree.targets[0] = avatar.actor;
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++)
        astree.targets[1 + i] = armor_pup[i].actor && armor_pup[i].frames >= 6 && pup_alive(&armor_pup[i]) ? armor_pup[i].actor : 0;
    if (!use) {
        avl.cf_armed = false, avl.cf_last = 0;
        astree.eat.active = false;
        if (astree.active) {  // back to the classified path: stop the layers it never plays (it replays the rest)
            astree.have = 0;
            for (int i = 0; i < ASTREE_N; i++) {
                int s = i / COOP_AS_LAYERS, l = i % COOP_AS_LAYERS;
                if (astree.as[i] && (l || s == 2 || s == 4)) astree_play_all(c, NULL, s, l);
            }
            memset(astree.as, 0, sizeof astree.as);
            memset(astree.clip_seen, 0, sizeof astree.clip_seen);
            memset(astree.clip_back, 0, sizeof astree.clip_back);
            astree.active = false;
            LOG("[coop] tree mode: avatar back on the classified states\n");
        }
        astree_upper_weight();  // (puts back a weight it changed)
        astree_horse_aim(c, NULL);  // (same)
        return false;
    }
    // Carrying (GrabWait, any Grab* AS in the upper-body sub-layer 1.1 over Wait / Move in 1.0): on the avatar that
    // sub-layer does not show (seen 2026-11-11: the avatar holds its arms low and forward, the barrel puppet floats
    // tilted beside its head), so the avatar plays it in 1.0, the upper body's main layer; the legs stay on 0.0.
    // (Other 1.1 actions: astree_upper_weight.)
    {
        int up = 1 * COOP_AS_LAYERS + 1, main = 1 * COOP_AS_LAYERS;
        if (t.as[up] && t.as[up] < COOP_AS_NAME_COUNT && starts(coop_as_names[t.as[up]], "Grab"))
            t.as[main] = t.as[up], t.seq[main] = t.seq[up], t.as[up] = 0;
    }
    astree_map(&t);
    if (nodepos_av_on < 0) {
        nodepos_av_on = look_on("SAVAGEWIND_COOP_NODEPOS_AVATAR");
        LOG("[coop] tree: NodePos puppets follow the avatar's choice: %s\n", nodepos_av_on ? "on" : "off");
    }
    if (astree.active && eat_frame(c, &t, &resync)) return true;  // (coop/eat.inc: the other player eats in the menu)
    bool enter = !astree.active;
    astree.rx = t;
    astree.have = 1;
    for (int k = 0; k < ASTREE_TARGETS; k++)
        if (astree.targets[k]) astree_write(astree.targets[k], &t);
    bool changed = false;
    bool start[ASTREE_N];
    for (int i = 0; i < ASTREE_N; i++) start[i] = enter || resync || t.as[i] != astree.as[i] || t.seq[i] != astree.seq[i];
    // The body layers start together (2026-11-11, user: in the slow walk the waist / head move out of step with the
    // legs). 0.0 (legs), 1.0 (upper body) and an upper-body loop in 1.1 (MoveUpper, WaitUpper...) run separate AS
    // instances; on the player they begin in the same frame, but our copies can begin a packet apart (the sender's
    // slot 1 often switches a frame after slot 0, and a packet goes every 50 ms), and then stay out of step for the
    // whole walk. So when one of them starts, the others that keep playing start again in the same frame.
    {
        int l0 = 0, u0 = 1 * COOP_AS_LAYERS, u1 = 1 * COOP_AS_LAYERS + 1;
        bool loop1 = t.as[u1] && astree_upper_loop(coop_as_names[t.as[u1]]);
        // (guard sync) raising the shield / locking on starts *AttentionUpper in 1.1 alone: the player keeps its legs
        // and 1.0 going then (phase order: astree_phase_lock), so that start alone restarts nothing else
        bool attn1 = loop1 && astree_guardsync_on() && astree_attn_upper(coop_as_names[t.as[u1]]);
        if (t.as[l0] && t.as[l0] == t.as[u0] && (start[l0] || start[u0] || (start[u1] && loop1 && !attn1))) {
            if (t.as[l0]) start[l0] = true;
            if (t.as[u0]) start[u0] = true;
            if (loop1 && !attn1) start[u1] = true;  // (the guard is not restarted by the legs either: FUN_7100cf1adc)
        }
    }
    for (int i = 0; i < ASTREE_N; i++) {
        if (!start[i]) continue;
        int s = i / COOP_AS_LAYERS, l = i % COOP_AS_LAYERS;
        if (t.as[i]) astree_play_all(c, coop_as_names[t.as[i]], s, l);
        else if (astree.as[i] || enter) astree_play_all(c, NULL, s, l);
        if (t.as[i] != astree.as[i]) changed = true;
        else if (t.as[i] && t.seq[i] != astree.seq[i] && !enter && !resync) {
            static unsigned said;
            if (said++ < 200) LOG("[coop] tree avatar: %d.%d %s started again (the other player restarted it, seq %u)\n", s, l,
                                  coop_as_names[t.as[i]], t.seq[i]);
        }
        astree.as[i] = t.as[i], astree.seq[i] = t.seq[i];
    }
    // An AS event can play another AS in a slot of the puppet (seen 2026-11-10: Nml_Wait's "FaceDefault" trigger
    // event ended up in the avatar's slot 1, the upper body; on the player it goes to the face slot). A layer that
    // plays some other AS than ours gets ours again (an ended one, "", is left alone: one-shots end by themselves).
    for (int i = 0; i < ASTREE_N; i++) {
        if (!astree.as[i]) continue;
        int s = i / COOP_AS_LAYERS, l = i % COOP_AS_LAYERS;
        if (!l && s <= 1 && astree_end.idle && astree_end.as == astree.as[0] && astree_end.seq == astree.seq[0])
            continue;  // (the body holds its family's calm loop: astree_ended_guard)
        const char *want = avatar_as_alias(coop_as_names[astree.as[i]]);
        for (int k = 0; k < ASTREE_TARGETS; k++) {
            if (!astree.targets[k]) continue;
            const char *now = as_layer_name(as_layer_rec(astree.targets[k], s, l));
            if (!now[0] || puppet_as_same(now, want)) continue;
            static unsigned said;
            if (said++ < 40) LOG("[coop] tree: %d.%d of %llx plays %s, not %s: replayed\n", s, l,
                                 (unsigned long long)astree.targets[k], now, want);
            play_anim_l(c, astree.targets[k], want, s, l);
        }
    }
    astree_ended_guard(c);
    astree_horse_aim(c, &t);
    astree_charge_lv = t.charge_lv;
    charge_fx(c, t.charge_lv, coop_as_names[t.as[0]]);
    astree_phase_lock(c);
    astree_upper_weight();
    if (enter) LOG("[coop] tree mode: avatar plays the other player's own AS\n");
    if (changed || enter) {
        char names[400];
        astree_names(&t, names, sizeof names);
        LOG("[coop] tree avatar:%s\n", names);
    }
    astree.active = true;
    return true;
}

// One trace line per actor: its AS in 0.0 / 1.0 / 3.0 and the height of some bones above the actor's origin, and
// their distance from Spine_2 (a head that "floats" shows as a longer Spine_2 -> Head distance).
// For the armor puppets also "@d": how far that bone is from the same bone of the avatar (should be ~0: a puppet whose
// skeleton drifts from the avatar's shows its clothes away from the body / head).
static void astrace_one(Cpu *c, uint64_t a, const char *who) {
    static const char *const bones[] = {"Skl_Root", "Spine_1", "Spine_2", "Neck", "Head", "Clavicle_L", "Ankle_R"};
    enum { NB = sizeof bones / sizeof bones[0] };
    static float av[NB][3];
    static bool av_have[NB];
    bool is_avatar = !strcmp(who, "avatar");
    float m[12], sp[3] = {0, 0, 0};
    char line[700];
    size_t n = (size_t)snprintf(line, sizeof line, "%s %s|%s|%s", who, as_layer_name(as_layer_rec(a, 0, 0)),
                                as_layer_name(as_layer_rec(a, 1, 0)), as_layer_name(as_layer_rec(a, 3, 0)));
    float oy;
    memcpy(&oy, (void *)(uintptr_t)(a + ACTOR_MTX + 28), 4);
    for (int b = 0; b < NB && n + 60 < sizeof line; b++) {
        if (is_avatar) av_have[b] = false;
        if (!bone_matrix(c, a, bones[b], m)) { n += (size_t)snprintf(line + n, sizeof line - n, " %s=-", bones[b]); continue; }
        if (b == 2) sp[0] = m[3], sp[1] = m[7], sp[2] = m[11];
        float dx = m[3] - sp[0], dy = m[7] - sp[1], dz = m[11] - sp[2];
        n += (size_t)snprintf(line + n, sizeof line - n, " %s=%.3f/%.3f", bones[b], m[7] - oy, b >= 2 ? sqrtf(dx * dx + dy * dy + dz * dz) : 0.0f);
        if (b == 0) {  // (the root bone's horizontal offset from the actor's matrix: where its model really is)
            float ox, oz;
            memcpy(&ox, (void *)(uintptr_t)(a + ACTOR_MTX + 12), 4), memcpy(&oz, (void *)(uintptr_t)(a + ACTOR_MTX + 44), 4);
            n += (size_t)snprintf(line + n, sizeof line - n, " (org %.2f %.2f, root off %.3f %.3f)", ox, oz, m[3] - ox, m[11] - oz);
        }
        if (is_avatar) av_have[b] = true, av[b][0] = m[3], av[b][1] = m[7], av[b][2] = m[11];
        else if (av_have[b]) {
            float ex = m[3] - av[b][0], ey = m[7] - av[b][1], ez = m[11] - av[b][2];
            n += (size_t)snprintf(line + n, sizeof line - n, "@%.3f", sqrtf(ex * ex + ey * ey + ez * ez));
        }
    }
    LOG("[coop] astrace %s\n", line);
}

static void asrec_one(Cpu *c, uint64_t a, int s, int l, const char *who) {
    uint64_t rec = as_layer_rec(a, s, l);
    if (!rec) return;
    float f[8] = {0};
    uint64_t t = as_layer_time(c, rec);
    if (t) memcpy(f, (void *)(uintptr_t)t, 32);
    float r10, r30, r38;
    memcpy(&r10, (void *)(uintptr_t)(rec + 0x10), 4), memcpy(&r30, (void *)(uintptr_t)(rec + 0x30), 4), memcpy(&r38, (void *)(uintptr_t)(rec + 0x38), 4);
    LOG("[coop] asrec %s %d.%d %s inst %llx prev %llx r10 %.3f r30 %.3f r38 %.3f fl %04x b44 %02x%02x r48 %llx | t %llx: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n",
        who, s, l, as_layer_name(rec), (unsigned long long)LD64(rec + 0x18), (unsigned long long)LD64(rec + 0x20), r10, r30, r38,
        LD16(rec + 0x40), LD8(rec + 0x44), LD8(rec + 0x45), (unsigned long long)LD64(rec + 0x48), (unsigned long long)t,
        f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}

static void asdev_frame(Cpu *c, uint64_t avatar_actor) {
    if (asdev.rec > 0) {
        asdev.rec--;
        uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
        // (2026-11-18: also 1.1 and the armor puppets, for the guard-down probe)
        if (in_guest(pa)) asrec_one(c, pa, 0, 0, "player"), asrec_one(c, pa, 1, 0, "player"), asrec_one(c, pa, 1, 1, "player");
        if (avatar_actor) asrec_one(c, avatar_actor, 0, 0, "avatar"), asrec_one(c, avatar_actor, 1, 0, "avatar"), asrec_one(c, avatar_actor, 1, 1, "avatar");
        for (int i = 0; avatar_actor && i < COOP_ARMOR_SLOTS; i++)
            if (armor_pup[i].actor && pup_alive(&armor_pup[i])) {
                const char *w = i == 0 ? "armor0" : i == 1 ? "armor1" : "armor2";
                asrec_one(c, armor_pup[i].actor, 1, 0, w), asrec_one(c, armor_pup[i].actor, 1, 1, w);
            }
    }
    if (asdev.trace > 0) {
        asdev.trace--;
        uint64_t pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
        if (in_guest(pa)) astrace_one(c, pa, "player");
        if (avatar_actor) astrace_one(c, avatar_actor, "avatar");
        for (int i = 0; avatar_actor && i < COOP_ARMOR_SLOTS; i++)
            if (armor_pup[i].actor && pup_alive(&armor_pup[i])) astrace_one(c, armor_pup[i].actor, i == 0 ? "armor0" : i == 1 ? "armor1" : "armor2");
    }
    if (asdev.dump) {
        uint64_t a = asdev.dump;
        asdev.dump = 0;
        asin_dump(a, "dev");
        if (a == avatar.actor) LOG("[coop] tree: NPC input hook hits so far %u\n", astree.hook_hits);
    }
    if (asdev.write) {
        asdev.write = 0;
        asin_write_text(asdev.w_actor, asdev.w_kind, asdev.w_val);
    }
}

