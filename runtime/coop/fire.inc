// Part of runtime/coop.c (included there, same translation unit): campfires, cooking pots, firewood bundles and torch
// stands lit / put out in one game are lit / put out in the others too. (2026-11-19, worker fire; protocol 82,
// reliable event 27 COOP_EV_FIRE; audit item S2)
//
// Research (Ghidra + pack data, 2026-11-19):
// - Every one of these actors has a chemical component (actor vfunc +0x308, like the puzzle torches in switches.inc):
//   FireWood / SignalFireWood (AI FireWood), FireWoodFromBundle (AI DynamicFireWood: what a lit bundle turns into),
//   Obj_FireWoodBundle, Item_CookSet* (AI CookPotRoot; the pot's fire is its own chemical), FidObj/FldObj/TwnObj torch
//   stands (chemical Candle / SwitchCandlestick).
// - The chemical's element state: byte +0xc0 (current) / +0xc1 (requested), 2 = fire (extinguish 0x70010f1f44 acts only
//   when one of them is 2 and clears +0xc1). Ignite 0x70010f1ab4(chm, flag, 2, 0, 1, 0) is what SwitchTorch calls
//   (flag = bit 21 of *(chm+0x20)+0x1f0); it lights only a burnable chemical (bit 0 of that word).
// What is shared: a fire object whose state changes in our game by itself (a player's flint, fire arrow, torch, rain,
// wind...) is sent (map HashId, or the position for run-time ones); the other games light / put out their own copy
// with the same native calls. At any distance: an event for an object that is not loaded there waits (15 min; a
// run-time one 60 s) and is applied when it loads (each event is applied once, then each game's fire lives on its
// own: a fire put out by rain in one game is sent as such). First sight of an object (a load) is never sent, nor a
// change in its first 15 s (campfires that light themselves on load).
// SAVAGEWIND_COOP_FIRE=0: off. Dev: `coop fire` lists the fire objects here; `coop fire on|off <hash>` lights / puts
// one out here as if a player did (it is sent); `coop fire held on|off`: our weapon in hand.
#define FIRE_MAX 96
#define FIRE_PEND 32
#define FIRE_SCAN_NS 500000000ull
#define FIRE_NEAR_M 2.5f                 // a run-time fire (hash 0) matches an object of ours this close
#define FIRE_ECHO_NS 2500000000ull       // our state change this soon after applying theirs is not ours to send
#define FIRE_SETTLE_NS 15000000000ull    // nor one this soon after the object loaded (test 2026-11-19: plateau campfires
                                         // light themselves a few seconds after they load, in each game)
#define FIRE_WAIT_NS 900000000000ull     // a placed fire not loaded here waits this long
#define FIRE_WAIT_RT_NS 60000000000ull   // a run-time one
#define A_FIRE_IGNITE 0x70010f1ab4ull    // chemical ignite (as SwitchTorch 0x7100a89090 calls it)
#define A_FIRE_EXTINGUISH 0x70010f1f44ull

static bool fire_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_FIRE");
    return on;
}
static bool fire_name(const char *n) {
    if (sw_is_torch(n) || !strncmp(n, "Dm_", 3) || !strncmp(n, "Weapon_", 7) || strstr(n, "Die") ||
        strstr(n, "AreaTag"))
        return false;
    if (strstr(n, "FireWood") || !strncmp(n, "Item_CookSet", 12)) return true;
    return (!strncmp(n, "FidObj_", 7) || !strncmp(n, "FldObj_", 7) || !strncmp(n, "TwnObj_", 7)) && strstr(n, "Torch");
}
struct Fire {
    uint64_t actor, unit;
    uint32_t unit_id, hash;
    char name[48];
    float pos[3];
    bool on, seen;
    uint64_t found_ns, injected_ns;
};
static struct Fire fires[FIRE_MAX];
static int fire_n;
static struct { uint32_t hash, area; float pos[3]; bool want; uint8_t tries; uint64_t until_ns, try_ns; } fire_pend[FIRE_PEND];
static uint64_t fire_area_since;
enum { FIRE_LIST, FIRE_ON, FIRE_OFF, FIRE_HELD_ON, FIRE_HELD_OFF, FIRE_HELD_BLUE, FIRE_HELD_STATUS };
static struct { volatile int pending; int mode; uint32_t hash; } fire_dev;

static uint64_t fire_chem(Cpu *c, uint64_t a) {
    uint64_t vt = LD64(a), fn = in_guest(vt) ? LD64(vt + 0x308) : 0;
    if (!in_guest(fn)) return 0;
    Cpu saved = *c;
    c->x[0] = a;
    guest_call(c, fn);
    uint64_t chm = c->x[0];
    *c = saved;
    return in_guest(chm) && in_guest(chm + 0x190) ? chm : 0;
}
static bool fire_burning(uint64_t chm) { return chm && (LD8(chm + 0xc0) == 2 || LD8(chm + 0xc1) == 2); }
static bool fire_alive(const struct Fire *f) {
    return in_guest(f->actor) && in_guest(f->unit) && LD32(f->unit + UNIT_ID) == f->unit_id &&
           LD64(f->unit + UNIT_ACTOR) == f->actor && LD8(f->actor + PROC_STATE) != 3;
}
// Light (on) / put out our copy with the game's own chemical calls; false: it has no chemical / is not burnable.
static bool fire_set_chem(Cpu *c, uint64_t chm, bool on, bool blue) {
    if (!chm) return false;
    Cpu saved = *c;
    if (on) {
        uint64_t p = LD64(chm + 0x20);
        if (!in_guest(p) || !in_guest(p + 0x1f0) || !(LD32(p + 0x1f0) & 1) || (LD8(chm + 0xbe) & 1)) return false;
        c->x[0] = chm; c->x[1] = blue || (LD32(p + 0x1f0) & (1u << 21));  // native blue-fire input
        c->x[2] = 2; c->x[3] = 0; c->x[4] = 1; c->x[5] = 0;
        guest_call(c, A_FIRE_IGNITE);
    } else {
        c->x[0] = chm;
        guest_call(c, A_FIRE_EXTINGUISH);
    }
    *c = saved;
    return true;
}
static bool fire_set(Cpu *c, uint64_t a, bool on) { return fire_set_chem(c, fire_chem(c, a), on, false); }
static void fire_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)ctx;
    uint32_t uid = LD32(a + PROC_ID);
    for (int i = 0; i < fire_n; i++)
        if (fires[i].actor == a && fires[i].unit_id == uid) { fires[i].seen = true; return; }
    if (fire_n == FIRE_MAX) return;
    uint64_t chm = fire_chem(c, a);
    struct Fire *f = &fires[fire_n++];
    memset(f, 0, sizeof *f);
    f->actor = a, f->unit = LD64(a + PROC_UNIT), f->unit_id = uid, f->hash = actor_hash(c, a), f->seen = true;
    snprintf(f->name, sizeof f->name, "%s", name);
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    f->pos[0] = m[3], f->pos[1] = m[7], f->pos[2] = m[11];
    f->on = fire_burning(chm);
    f->found_ns = plat_now_ns();
    // A run-time fire (no HashId) already burning when it appears is a bundle that was just lit here (it becomes
    // FireWoodFromBundle) - unless we lit it for the other player (an event near it is pending / just applied).
    if (f->on && !f->hash && strstr(name, "FromBundle") && plat_now_ns() - fire_area_since > 5000000000ull) {
        bool theirs = false;
        for (int k = 0; k < FIRE_PEND; k++) {
            float dx = fire_pend[k].pos[0] - f->pos[0], dy = fire_pend[k].pos[1] - f->pos[1], dz = fire_pend[k].pos[2] - f->pos[2];
            theirs |= fire_pend[k].until_ns && dx * dx + dy * dy + dz * dz < 25.0f;
        }
        if (!theirs) f->found_ns = 0;  // (fire_frame sends it)
        else f->injected_ns = plat_now_ns();
    }
}
static void fire_send(struct Fire *f, uint32_t area) {
    CoopFire e;
    memset(&e, 0, sizeof e);
    e.hash = f->hash, e.area = area, e.on = f->on;
    memcpy(e.pos, f->pos, 12);
    bool ok = coop_fire_ok(&e) && coop_net_send_event(COOP_EV_FIRE, &e, sizeof e);
    LOG("[coop] fire: %s %u %s here at %.1f %.1f %.1f: %s\n", f->name, f->hash, f->on ? "lit" : "put out", f->pos[0],
        f->pos[1], f->pos[2], ok ? "sent" : "NOT sent (no peer / queue full)");
}
static struct Fire *fire_match(uint32_t hash, const float pos[3]) {
    struct Fire *best = NULL;
    float bd = FIRE_NEAR_M * FIRE_NEAR_M;
    for (int i = 0; i < fire_n; i++) {
        struct Fire *f = &fires[i];
        if (!fire_alive(f)) continue;
        if (hash) {
            if (f->hash == hash) return f;
            continue;
        }
        float dx = f->pos[0] - pos[0], dy = f->pos[1] - pos[1], dz = f->pos[2] - pos[2], d = dx * dx + dy * dy + dz * dz;
        if (d < bd) bd = d, best = f;
    }
    return best;
}
static void fire_pending_frame(Cpu *c, uint32_t here, uint64_t now) {
    for (int k = 0; k < FIRE_PEND; k++) {
        if (!fire_pend[k].until_ns) continue;
        if (now > fire_pend[k].until_ns) {
            LOG("[coop] fire: %u (the other player's) never matched here; dropped\n", fire_pend[k].hash);
            fire_pend[k].until_ns = 0;
            continue;
        }
        if (fire_pend[k].area != here) continue;
        struct Fire *f = fire_match(fire_pend[k].hash, fire_pend[k].pos);
        if (!f) continue;
        bool cur = fire_burning(fire_chem(c, f->actor));
        if (cur == fire_pend[k].want) {
            LOG("[coop] fire: %s %u %s here too\n", f->name, f->hash, cur ? "lit" : "out");
            f->on = cur, f->injected_ns = now;
            fire_pend[k].until_ns = 0;
            continue;
        }
        if (now - fire_pend[k].try_ns < 1000000000ull) continue;
        if (fire_pend[k].tries >= 4) {
            LOG("[coop] fire: %s %u still %s here after %d tries; giving up\n", f->name, f->hash, cur ? "lit" : "out",
                fire_pend[k].tries);
            fire_pend[k].until_ns = 0;
            continue;
        }
        fire_pend[k].tries++, fire_pend[k].try_ns = now;
        f->injected_ns = now;
        bool ok = fire_set(c, f->actor, fire_pend[k].want);
        LOG("[coop] fire: %s %u: the other player's fire %s here%s\n", f->name, f->hash,
            fire_pend[k].want ? "lit" : "put out", ok ? "" : " (FAILED: no burnable chemical)");
        if (!ok) fire_pend[k].until_ns = 0;
    }
}
static void fire_held_dev_run(Cpu *c);
static void fire_dev_run(Cpu *c) {
    if (!fire_dev.pending) return;
    fire_dev.pending = 0;
    for (int i = 0; i < fire_n; i++) {
        struct Fire *f = &fires[i];
        if (!fire_alive(f)) continue;
        uint64_t chm = fire_chem(c, f->actor), p = chm ? LD64(chm + 0x20) : 0;
        if (fire_dev.mode == FIRE_LIST)
            LOG("[coop] fire list: %s %u actor %llx at %.1f %.1f %.1f chem %llx c0 %d c1 %d burnable %d%s\n", f->name,
                f->hash, (unsigned long long)f->actor, f->pos[0], f->pos[1], f->pos[2], (unsigned long long)chm,
                chm ? LD8(chm + 0xc0) : -1, chm ? LD8(chm + 0xc1) : -1, in_guest(p) ? (int)(LD32(p + 0x1f0) & 1) : -1,
                fire_burning(chm) ? " BURNING" : "");
        else if (f->hash == fire_dev.hash) {
            bool ok = fire_set(c, f->actor, fire_dev.mode == FIRE_ON);
            LOG("[coop] fire: dev %s %s %u: %s\n", fire_dev.mode == FIRE_ON ? "light" : "put out", f->name, f->hash,
                ok ? "done" : "FAILED");
        }
    }
    if (fire_dev.mode == FIRE_LIST) LOG("[coop] fire list: %d known\n", fire_n);
}
static void fire_frame(Cpu *c) {
    static uint64_t next_ns;
    static uint32_t last_area;
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (local_in_world() && fire_dev.pending && fire_dev.mode >= FIRE_HELD_ON) fire_held_dev_run(c);
    if (!fire_on() || !local_in_world() || !player_info_ptr() || here != last_area) {
        if (fire_n) LOG("[coop] fire: %d forgotten (area change or load)\n", fire_n);
        fire_n = 0, last_area = here, fire_area_since = now;
        return;
    }
    if (now < next_ns && !fire_dev.pending) return;
    next_ns = now + FIRE_SCAN_NS;
    for (int i = 0; i < fire_n; i++) fires[i].seen = false;
    walk_actors(c, fire_name, fire_seen, NULL);
    for (int i = 0; i < fire_n; i++) {
        struct Fire *f = &fires[i];
        if (!f->seen || !fire_alive(f)) { fires[i--] = fires[--fire_n]; continue; }
        if (!f->found_ns) {  // (a run-time fire lit here: fire_seen)
            f->found_ns = now;
            fire_send(f, here);
            continue;
        }
        uint64_t chm = fire_chem(c, f->actor);
        if (!chm) continue;
        bool cur = fire_burning(chm);
        if (cur == f->on) continue;
        f->on = cur;
        bool ours = now - f->injected_ns > FIRE_ECHO_NS && now - f->found_ns > FIRE_SETTLE_NS;
        LOG("[coop] fire: %s %u now %s%s\n", f->name, f->hash, cur ? "burning" : "out", ours ? " (here)" : "");
        if (ours) fire_send(f, here);
    }
    fire_pending_frame(c, here, now);
    fire_dev_run(c);
}
// ---- (2.) Held flames. The weapon puppet has the weapon's ELink user but no chemical or physics.
// Keep the native flame effect on the puppet and pass close contact to the local weapon's chemical.
enum { FLAME_OUT, FLAME_RED, FLAME_BLUE };
#define TORCH_CONTACT_M 0.5f
static struct { uint64_t h, actor; uint32_t id; int kind; bool playing; unsigned said; } fire_torch;
static bool fire_torch_on(void) {
    static int on = -1;
    if (on < 0) on = fire_on() && look_on("SAVAGEWIND_COOP_FIRETORCH");
    return on;
}
static int fire_held_kind(uint64_t chm) {
    if (!fire_burning(chm)) return FLAME_OUT;
    // The native chemical effects selector (0x70011827ac) uses bit 20 of the state word.
    return LD32(chm + 0xc) & (1u << 20) ? FLAME_BLUE : FLAME_RED;
}
static const char *fire_kind_name(int kind) {
    return kind == FLAME_BLUE ? "blue" : kind == FLAME_RED ? "red" : "out";
}
static bool fire_held_bone(int bone) { return hand_bone(bone) || bone == COOP_BONE_ARM_R; }
static uint64_t fire_held_chem(Cpu *c, const CoopGear *g) {
    const struct Held *w = &held[COOP_GEAR_WEAPON];
    return fire_held_bone(g->bone) && hand_bone(w->bone) && !strcmp(w->name, g->name) && held_alive(w) ?
           fire_chem(c, w->actor) : 0;
}
static void fire_held_publish(Cpu *c, CoopState *s) {
    if (!fire_torch_on() || !(s->flags & COOP_FLAG_IN_WORLD)) return;
    int kind = fire_held_kind(fire_held_chem(c, &s->gear[COOP_GEAR_WEAPON]));
    if (kind != FLAME_OUT) s->flags |= COOP_FLAG_BURNING;
    if (kind == FLAME_BLUE) s->flags |= COOP_FLAG_BLUE_FIRE;
}
static float fire_tip_offset(const char *name) {
    // The torch head is around +0.85 m in its native collision hull. For other melee weapons use the
    // exposed end, away from the grip at the actor origin.
    if (!strcmp(name, "Weapon_Sword_043")) return 0.85f;
    if (!strcmp(name, "Weapon_Sword_044")) return 0.9f;  // tree branch
    if (!strncmp(name, "Weapon_Lsword_", 14)) return 1.0f;
    if (!strncmp(name, "Weapon_Spear_", 13)) return 1.5f;
    return 0.65f;
}
static float fire_tip_delta(const struct Held *w, const struct Pup *p, const char *source, float delta[3]) {
    const float *local = (const float *)(uintptr_t)(w->actor + ACTOR_MTX);
    const float *remote = (const float *)(uintptr_t)(p->actor + ACTOR_MTX);
    float lz = fire_tip_offset(w->name), rz = fire_tip_offset(source);
    for (int i = 0; i < 3; i++)
        delta[i] = remote[i * 4 + 3] + remote[i * 4 + 2] * rz - (local[i * 4 + 3] + local[i * 4 + 2] * lz);
    return delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
}
static void fire_held_dev_run(Cpu *c) {
    fire_dev.pending = 0;
    const struct Held *w = &held[COOP_GEAR_WEAPON];
    uint64_t chm = fire_held_chem(c, &look_sent.gear[COOP_GEAR_WEAPON]);
    if (fire_dev.mode == FIRE_HELD_STATUS) {
        const struct Pup *p = &gear_pup[COOP_GEAR_WEAPON];
        CoopState rs;
        double age;
        float delta[3] = {0}, d2 = -1;
        if (chm && pup_alive(p) && coop_net_get_remote(&rs, &age) && age < 1.0)
            d2 = fire_tip_delta(w, p, rs.gear[COOP_GEAR_WEAPON].name, delta);
        LOG("[coop] fire: held status: %s kind=%s distance=%.3f delta=%.3f,%.3f,%.3f\n", chm ? w->name : "-",
            fire_kind_name(fire_held_kind(chm)), d2 >= 0 ? sqrtf(d2) : -1.0f, delta[0], delta[1], delta[2]);
        return;
    }
    bool blue = fire_dev.mode == FIRE_HELD_BLUE, on = fire_dev.mode != FIRE_HELD_OFF;
    bool ok = fire_set_chem(c, chm, on, blue);
    LOG("[coop] fire: dev %s our weapon in hand (%s): %s\n", on ? (blue ? "light blue" : "light") : "put out",
        w->name, ok ? "done" : "FAILED");
}
static void fire_torch_stop(Cpu *c) {
    if (fire_torch.playing && elink_live_effect(fire_torch.h)) {
        Cpu saved = *c;
        c->x[0] = fire_torch.h;
        guest_call(c, A_FX_KILL);
        *c = saved;
    }
    if (fire_torch.playing && fire_torch.said++ < 40) LOG("[coop] fire: the other player's weapon flame off\n");
    fire_torch.playing = false, fire_torch.actor = 0, fire_torch.kind = FLAME_OUT;
}
static void fire_torch_contact(Cpu *c, const struct Pup *p, const CoopGear *g, int kind) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_TORCHLIGHT");
    if (!on) return;
    const struct Held *w = &held[COOP_GEAR_WEAPON];
    if (!held_alive(w) || !hand_bone(w->bone)) return;
    float delta[3];
    float d2 = fire_tip_delta(w, p, g->name, delta);
    if (!(d2 <= TORCH_CONTACT_M * TORCH_CONTACT_M)) return;
    uint64_t chm = fire_held_chem(c, &look_sent.gear[COOP_GEAR_WEAPON]);
    // The native contact path (0x70010f0a48) starts from neutral chemical state and checks water and suppression.
    if (!chm || LD8(chm + 0xc0) || LD8(chm + 0xc1) || LDF32(chm + 0x190) > 0.75f ||
        LDF32(chm + 0x18c) > 0.0f || (LDF32(chm + 0xd0) > 0.2f && LD8(chm + 0xd4) != 2)) return;
    if (fire_set_chem(c, chm, true, kind == FLAME_BLUE) && fire_burning(chm)) {
        LOG("[coop] fire: torch contact lit %s %s at %.3f m\n", w->name, fire_kind_name(fire_held_kind(chm)), sqrtf(d2));
    }
}
static void fire_torch_frame(Cpu *c, const CoopState *rs, bool fresh, bool placed) {
    static int colour = -1;
    if (colour < 0) colour = look_on("SAVAGEWIND_COOP_FLAMEKIND");
    struct Pup *p = &gear_pup[COOP_GEAR_WEAPON];
    if (!fire_torch_on() || !local_in_world() || !fresh || !placed || !(look_sent.flags & COOP_FLAG_IN_WORLD) ||
        !(rs->flags & COOP_FLAG_IN_WORLD) || !(rs->flags & COOP_FLAG_BURNING) || rs->area != look_sent.area ||
        !fire_held_bone(rs->gear[COOP_GEAR_WEAPON].bone)) {
        if (fire_torch.kind != FLAME_OUT) fire_torch_stop(c);
        return;
    }
    int native_kind = rs->flags & COOP_FLAG_BLUE_FIRE ? FLAME_BLUE : FLAME_RED;
    fire_torch_contact(c, p, &rs->gear[COOP_GEAR_WEAPON], native_kind);
    int kind = colour ? native_kind : FLAME_RED;
    if (fire_torch.kind != kind || fire_torch.actor != p->actor || fire_torch.id != p->id ||
        (fire_torch.playing && !elink_live_effect(fire_torch.h)))
        fire_torch_stop(c);
    if (fire_torch.kind == kind) return;
    if (!fire_torch.h) fire_torch.h = (uint64_t)(uintptr_t)hle_alloc(0x20);
    if (!fire_torch.h) return;
    memset((void *)(uintptr_t)fire_torch.h, 0, 0x20);
    const char *key = kind == FLAME_BLUE ? "Chemical_BlueFire" : "Chemical_Fire";
    bool ok = xl_event_h(c, p->actor, key, XL_EV_ELINK, fire_torch.h);
    fire_torch.playing = ok && elink_live_effect(fire_torch.h);
    fire_torch.actor = p->actor, fire_torch.id = p->id, fire_torch.kind = kind;
    if (fire_torch.said++ < 40)
        LOG("[coop] fire: the other player's weapon burns: flame on %s %s (%s)\n", p->name,
            fire_torch.playing ? "playing" : "unavailable", key);
}
RT_ON_RESET(coop_torch_reset) {
    memset(&fire_torch, 0, sizeof fire_torch);
    fire_dev.pending = 0;
}
// EV 27 from the other game (gate.inc world_event, game thread).
static void fire_ev(Cpu *c, const uint8_t *data, int len) {
    (void)c;
    if (!fire_on() || len != (int)sizeof(CoopFire)) return;
    CoopFire e;
    memcpy(&e, data, sizeof e);
    if (!coop_fire_ok(&e)) { LOG("[coop] fire: bad event dropped\n"); return; }
    uint64_t now = plat_now_ns();
    int k = -1;
    for (int i = 0; i < FIRE_PEND && k < 0; i++) {
        if (!fire_pend[i].until_ns) continue;
        float dx = fire_pend[i].pos[0] - e.pos[0], dy = fire_pend[i].pos[1] - e.pos[1], dz = fire_pend[i].pos[2] - e.pos[2];
        if (e.hash ? fire_pend[i].hash == e.hash : (!fire_pend[i].hash && dx * dx + dy * dy + dz * dz < 1.0f)) k = i;
    }
    for (int i = 0; i < FIRE_PEND && k < 0; i++)
        if (!fire_pend[i].until_ns) k = i;
    if (k < 0) {  // (full: the oldest goes)
        k = 0;
        for (int i = 1; i < FIRE_PEND; i++)
            if (fire_pend[i].until_ns < fire_pend[k].until_ns) k = i;
    }
    fire_pend[k].hash = e.hash, fire_pend[k].area = e.area, fire_pend[k].want = e.on, fire_pend[k].tries = 0;
    memcpy(fire_pend[k].pos, e.pos, 12);
    fire_pend[k].try_ns = 0, fire_pend[k].until_ns = now + (e.hash ? FIRE_WAIT_NS : FIRE_WAIT_RT_NS);
    LOG("[coop] fire: the other player's %u at %.1f %.1f %.1f is %s (applied when it is here)\n", e.hash, e.pos[0],
        e.pos[1], e.pos[2], e.on ? "lit" : "out");
}
