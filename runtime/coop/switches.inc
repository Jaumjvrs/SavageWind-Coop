// Part of runtime/coop.c (included there, same translation unit): floor switches, crystals, torches, pinwheels, electric switches.
// ---- floor switches (2026-11-08: puzzles should be shared). A floor switch (SwitchStep,
// SwitchStepOnce, SwitchStepLOnce, DgnObj_SwitchStepOnce_*) is a plate - the "Button" body, dynamic, on a slider
// constraint that an impulse pushes up (AI SwitchStepSliderConstraint[Once]: limit -0.36 m, large ones -1.4 m; on
// past SwTh 0.1) - that anything heavy pushes down. The other player's avatar has no body in our game, so a switch
// they stand on was pressed only in theirs. Now each game tells the other which of its switches are pressed by
// something other than our own hold (EV_SWITCH; resent every 2 s while pressed), and the other game holds its plate
// down (a downward velocity every frame: the constraint stops it at its limit) until told it is released, 5 s pass
// without a refresh, or the area changes. A box on a switch is a shared object (both games have it there), so that
// case presses both anyway and only adds a few events. After a hold ends the plate needs a moment to come back up:
// no report of ours for SW_GRACE_NS (else the two games would keep pressing each other's switch).
// SAVAGEWIND_COOP_SWITCH=0 turns this off.
#define SW_MAX 32
#define SW_PRESSED_M 0.08f              // the plate this far below its rest height: pressed
#define SW_REFRESH_NS 2000000000ull     // "still pressed" again this often
#define SW_HOLD_NS 5000000000ull        // a press of theirs not refreshed for this long: let go
#define SW_GRACE_NS 1500000000ull       // after our hold ends: no report (the plate is still coming up)
typedef struct EvSwitch { uint32_t hash, area; uint8_t on, pad[3]; } EvSwitch;
static struct Sw {
    uint64_t actor, unit, rb, body;
    uint32_t unit_id, hash;
    char name[40];
    float rest_y;          // the plate's highest height seen while not held (it rests at the top)
    bool pressed_sent;     // what we last told the other player
    bool holding;
    bool hit;              // a hit switch (no plate: rb/body are 0)
    bool hit_on;           // its output signal was on at the last look
    uint64_t injected_ns;  // we gave it the other player's hit then (its "On" that follows is not ours to send)
    bool ai_frozen;        // (a pinwheel held on for the other player: its AI is frozen)
    uint64_t windmill;     // (2026-11-14) a pinwheel's SwitchWindmill action (we set its blade speed)
    bool windmill_tried, spinning;
    bool remote_on;        // the other player's copy, as last told
    uint64_t frozen_ns;
    uint64_t sent_ns, found_ns, released_ns;
    uint64_t dev_until;    // `coop switch hold`
} sws[SW_MAX];
static int sw_n;
static uint32_t sw_area;
static bool sw_probe;  // (`coop switch` was used: look for switches even when playing alone)
static struct { uint32_t hash; uint64_t until; } sw_remote[SW_MAX];  // pressed in their game (by hash)
static bool switch_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_SWITCH") && !strcmp(getenv("SAVAGEWIND_COOP_SWITCH"), "0"));
    return on;
}
// Hit switches (crystals: SwitchHit, SwitchHitOnce; arrow targets DgnObj_SwitchTarget*) have no plate. The Dungeon034
// crystal toggles on each hit (blue = on, orange = off). When ours changes by itself we send EV_SWITCH_HIT with the
// new state; the other game sets its copy's output signal to it (the game's setEmittingOutputSignal: the doors and
// fences wired to it follow). Known limit: the copy's own colour does not change, and its next real hit here may only
// bring it back in line with its signal (seen as "nothing happened" once).
// (2026-11-09) Torches work the same way - their state is their output signal - so they are synced the same way:
// SwitchCandlestick* (AI SwitchTorch: lit by fire), Test_Candlestick, DgnObj_Candle. Their flame does not follow on
// the copy. Pinwheels (SwitchWind, AI SwitchWindmill) are NOT: their signal is on only while they spin (~2.5 s per
// Korok Leaf swing, actor+0x71c 0x2200 -> 0x2201) and their AI sets it back every frame, so a set signal is only a
// pulse that did not reach the shrine's timer (Dungeon035, real leaf swing on the host). They need the copy to spin.
// (later 2026-11-09: pinwheels ARE synced now - while the other player's is on, ours has its AI frozen so the signal
// we set stays; see sw_hit_frame)
// Electric switches (SwitchElectric*, AI ChmCheck: on while something electric touches them) are "held" the same way
// (their AI sets the signal every frame too). SwitchTilt (a crystal on a tilting stand: AI SwitchHit) is a crystal.
// (2026-11-09: neither tested for real - no scripted way yet to power one or to hit a SwitchTilt in a test shrine.)
#define SW_PINWHEEL_MAX_NS 20000000000ull
static bool sw_is_pinwheel(const char *n) {
    return !strncmp(n, "SwitchWind", 10) || !strncmp(n, "SwitchElectric", 14) || !strncmp(n, "DgnObj_SwitchElectric", 21);
}
static bool sw_is_torch(const char *n) {
    return !strncmp(n, "SwitchCandlestick", 17) || !strcmp(n, "Test_Candlestick") || !strcmp(n, "DgnObj_Candle");
}
static bool sw_hit_name(const char *n) {
    return !strncmp(n, "SwitchHit", 9) || !strncmp(n, "DgnObj_SwitchTarget", 19) || sw_is_torch(n) ||
           !strncmp(n, "SwitchTilt", 10) || sw_is_pinwheel(n);
}
static bool sw_name(const char *n) {
    return !strncmp(n, "SwitchStep", 10) || !strncmp(n, "DgnObj_SwitchStep", 17) || sw_hit_name(n);
}
// The plate: among the actor's non-sensor bodies (reachable through at most three pointers) right above or below
// the actor's own position, the one whose wrapper is not keyframed/fixed... found live (2026-11-08): see sw_found's log.
struct SwBodies { uint64_t actor; int n; uint64_t rb[6]; };
static void sw_walk(uint64_t obj, int depth, struct SwBodies *f) {
    const float *m = (const float *)(uintptr_t)(f->actor + ACTOR_MTX);
    for (uint32_t off = 0; off < 0x1000 && f->n < 6; off += 8) {
        uint64_t p = LD64(obj + off);
        if (!in_guest(p) || (p & 7)) continue;
        if (LD64(p) == RIGID_BODY_VT) {
            const float *bp = (const float *)(uintptr_t)(p + 0x1a0);
            float dx = bp[0] - m[3], dy = bp[1] - m[7], dz = bp[2] - m[11];
            uint64_t rb = LD64(p + 0x18);
            if (dx * dx + dz * dz < 0.5f && dy > -3 && dy < 3 && in_guest(rb) && LD64(rb + 0x78) == p &&
                !(LD32(rb + 0x68) & 1)) {
                bool dup = false;
                for (int i = 0; i < f->n; i++) dup |= f->rb[i] == rb;
                if (!dup) f->rb[f->n++] = rb;
            }
            continue;
        }
        if (depth < 2 && in_guest(p + 0x1000)) sw_walk(p, depth + 1, f);
    }
}
static int sw_pick(const struct SwBodies *f);
static void sw_found(Cpu *c, uint64_t a, const char *name, void *ctx) {
    int *budget = ctx;
    for (int i = 0; i < sw_n; i++)
        if (sws[i].actor == a) return;
    if (*budget <= 0 || sw_n == SW_MAX) return;
    --*budget;
    uint32_t h = actor_hash(c, a);
    if (!h) return;
    if (sw_hit_name(name)) {
        struct Sw *s = &sws[sw_n++];
        memset(s, 0, sizeof *s);
        s->actor = a, s->unit = LD64(a + PROC_UNIT), s->unit_id = LD32(a + PROC_ID), s->hash = h, s->hit = true;
        snprintf(s->name, sizeof s->name, "%s", name);
        s->hit_on = LD32(a + 0x71c) & 1;
        s->found_ns = plat_now_ns();
        LOG("[coop] switch %s %u: a hit switch (%s)\n", name, h, s->hit_on ? "on" : "off");
        return;
    }
    struct SwBodies f = {a, 0, {0}};
    sw_walk(a, 0, &f);
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX);
    for (int i = 0; i < f.n; i++) {
        const float *bp = (const float *)(uintptr_t)(LD64(f.rb[i] + 0x78) + 0x1a0);
        LOG("[coop] switch %s %u: body %d rb %llx flags %08x %08x at %.2f %.2f %.2f (actor y %.2f)\n", name, h, i,
            (unsigned long long)f.rb[i], LD32(f.rb[i] + 0x68), LD32(f.rb[i] + 0x6c), bp[0], bp[1], bp[2], m[7]);
    }
    int k = sw_pick(&f);
    if (k < 0) return;
    struct Sw *s = &sws[sw_n++];
    memset(s, 0, sizeof *s);
    s->actor = a, s->unit = LD64(a + PROC_UNIT), s->unit_id = LD32(a + PROC_ID), s->hash = h, s->rb = f.rb[k];
    s->body = LD64(s->rb + 0x78);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->rest_y = *(const float *)(uintptr_t)(s->body + 0x1a4);
    s->found_ns = plat_now_ns();
    LOG("[coop] switch %s %u: plate = body %d (rest %.2f)\n", name, h, k, s->rest_y);
}
static int sw_pick(const struct SwBodies *f) {
    // (first guess, to be checked live: the highest body - the plate sits on its base)
    int best = -1;
    float by = -1e9f;
    for (int i = 0; i < f->n; i++) {
        float y = *(const float *)(uintptr_t)(LD64(f->rb[i] + 0x78) + 0x1a4);
        if (y > by) by = y, best = i;
    }
    return best;
}
static bool sw_alive(const struct Sw *s) {
    return in_guest(s->actor) && in_guest(s->unit) && LD32(s->unit + UNIT_ID) == s->unit_id &&
           LD64(s->unit + UNIT_ACTOR) == s->actor && LD8(s->actor + PROC_STATE) != 3 &&
           (s->hit || LD64(s->rb + 0x78) == s->body);
}
// A hit switch's state: its output signal (live 2026-11-08 on the Dungeon034 crystal: a sword hit sets actor+0x71c bit
// 0 and a second hit clears it - the crystal toggles - and the fences it drives follow). +0x71c is ksys Actor's
// output-signal mask (setEmittingOutputSignal 0x70015335d8 sets a bit and notifies the linked actors); poking it does
// not change the switch.
#define ACTOR_OUT_SIGNALS 0x71c
#define A_SET_OUT_SIGNAL 0x70015335d8ull  // (actor, bit, bool on)
#define A_CHM_IGNITE 0x70010f1ab4ull      // SwitchTorch (0x7100a89090): ignite chemical component
#define A_CHM_EXTINGUISH 0x70010f1f44ull  // SwitchTorch (0x7100a89144): extinguish chemical component
static bool sw_signal(uint64_t actor) { return LD32(actor + ACTOR_OUT_SIGNALS) & 1; }
static void sw_set_visual(Cpu *c, struct Sw *s, bool on) {
    if (sw_is_torch(s->name)) {
        uint64_t vt = LD64(s->actor), fn = in_guest(vt) ? LD64(vt + 0x308) : 0;
        if (!in_guest(fn)) return;
        Cpu saved = *c;
        c->x[0] = s->actor;
        guest_call(c, fn);
        uint64_t chm = c->x[0];
        *c = saved;
        if (!in_guest(chm)) return;
        if (on) {
            uint64_t p = LD64(chm + 0x20);
            uint32_t flag = in_guest(p) ? ((LD32(p + 0x1f0) >> 0x15) & 1) : 0;
            c->x[0] = chm; c->x[1] = flag; c->x[2] = 2; c->x[3] = 0; c->x[4] = 1; c->x[5] = 0;
            guest_call(c, A_CHM_IGNITE);
        } else {
            c->x[0] = chm;
            guest_call(c, A_CHM_EXTINGUISH);
        }
        *c = saved;
    } else {
        play_anim(c, s->actor, on ? "On" : "Off", 0);
    }
}
// A received sword hit as the crystal's receiver *(actor+0x840) holds it for one frame (`coop watch` during a real
// hit, 2026-11-08): +0x48 5 (damage), +0x50 0, +0x54 1, +0x5c 2, +0x6c 0, +0x7c 5. (The enemy injection's values -
// +0x44, +0x7c 2, +0x8c 0x10, kind 2 - did nothing to the crystal.)
// The same frame a real hit also sets the low byte of actor+0x684 to 2 (then 1, 0 on the next frames).
static int sw_hit_mode = 3;  // (bit 0: the receiver fields, bit 1: actor+0x684 = 2; `coop swhit` tries others)
static void sw_hit_inject(uint64_t actor) {
    uint64_t r = LD64(actor + ENEMY_RECEIVER);
    if (!in_guest(r)) return;
    if (sw_hit_mode & 1) {
        ST32(r + 0x48, 5);
        ST32(r + 0x50, 0);
        ST32(r + 0x54, 1);
        ST32(r + 0x5c, 2);
        ST32(r + 0x6c, 0);
        ST32(r + 0x7c, 5);
    }
    if (sw_hit_mode & 2) ST8(actor + 0x684, 2);
}
static struct { volatile int frames; uint64_t actor; } swhit_dev;  // `coop swhit <actor> <mode> [frames]` (dev)
static void swhit_dev_frame(void) {
    if (swhit_dev.frames <= 0) return;
    swhit_dev.frames--;
    sw_hit_inject(swhit_dev.actor);
    LOG("[coop] swhit %llx mode %d: signal now %d\n", (unsigned long long)swhit_dev.actor, sw_hit_mode,
        LD32(swhit_dev.actor + 0x71c) & 1);
}
// The other player's hit switches turned on/off (by hash, with the state): ours gets a hit while it differs.
static struct { uint32_t hash; bool on; uint64_t at; } sw_hits_in[8];
static void switch_hit_ev(const uint8_t *data, int len) {
    if (len != (int)sizeof(EvSwitch)) return;
    EvSwitch e;
    memcpy(&e, data, sizeof e);
    char area[96];
    if (!switch_on() || e.area != local_area(area, sizeof area) || e.on > 1) return;
    uint64_t now = plat_now_ns();
    int k = -1;
    for (int i = 0; i < 8 && k < 0; i++)
        if (sw_hits_in[i].hash == e.hash) k = i;
    for (int i = 0; i < 8 && k < 0; i++)
        if (!sw_hits_in[i].hash || now - sw_hits_in[i].at > 3000000000ull) k = i;
    if (k < 0) return;
    sw_hits_in[k].hash = e.hash, sw_hits_in[k].on = e.on, sw_hits_in[k].at = now;
    LOG("[coop] switch %u: the other player hit it (now %s there)\n", e.hash, e.on ? "on" : "off");
}
// (2026-11-14) The pinwheel copy SPINS. Found (Ghidra): the SwitchWindmill AI action's calc FUN_710076825c (runtime
// 0x700076825c, in its vtable) keeps the blade speed at action+0x134 (rad / frame): toward MaxRotSpeed (**(action+0x118))
// while wind touches its chemical component (actor vfunc +0x308, wind vector +0xf0), else down by RotAccel; it turns
// the bone "kaiten" by that speed, and while speed >= SwRadTh (**(action+0x100), 0.26) it plays "On" and sets its own
// output signal (off again below SwRadTh - SwRadAllowance). So the copy needs no frozen AI: while the other player's
// pinwheel is on we keep its speed at MaxRotSpeed; its own AI then spins the blades, plays On and holds the signal
// (the shrine's timer sees a normal pinwheel), and when theirs stops it slows down and goes off by itself.
// SAVAGEWIND_COOP_PINSPIN=0: the old way (AI frozen, signal set).
#define A_WINDMILL_CALC 0x700076825cull
#define WINDMILL_SPEED 0x134
#define WINDMILL_MAXSPEED 0x118
static bool sw_windmill_is(uint64_t p, uint64_t actor) {
    if (!in_guest(p) || (p & 7) || LD64(p + 8) != actor) return false;
    uint64_t vt = LD64(p);
    if (!in_guest(vt)) return false;
    for (int k = 0; k < 0x60; k++)
        if (LD64(vt + (uint64_t)k * 8) == A_WINDMILL_CALC) return true;
    return false;
}
// The pinwheel's SwitchWindmill action: within two pointers of its AI (actor+0x558), the object whose +8 is the actor
// and whose vtable holds the calc above; 0 when none.
static uint64_t sw_windmill_action(uint64_t actor) {
    uint64_t ai = LD64(actor + 0x558);
    if (!in_guest(ai)) return 0;
    if (sw_windmill_is(ai, actor)) return ai;
    for (uint32_t o = 0; o < 0x300; o += 8) {
        uint64_t p = LD64(ai + o);
        if (!in_guest(p) || (p & 7)) continue;
        if (sw_windmill_is(p, actor)) return p;
        for (uint32_t o2 = 0; o2 < 0x200 && in_guest(p + 0x200); o2 += 8) {
            uint64_t q = LD64(p + o2);
            if (sw_windmill_is(q, actor)) return q;
        }
    }
    return 0;
}
static bool sw_spin_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_PINSPIN") && !strcmp(getenv("SAVAGEWIND_COOP_PINSPIN"), "0"));
    return on;
}
// true: this switch is a pinwheel copy we spin (its action found, PINSPIN on)
static bool sw_spin_ready(struct Sw *s) {
    if (!sw_spin_on() || strncmp(s->name, "SwitchWind", 10)) return false;
    if (!s->windmill && !s->windmill_tried) {
        s->windmill_tried = true;
        s->windmill = sw_windmill_action(s->actor);
        LOG("[coop] switch %s %u: pinwheel action %llx%s\n", s->name, s->hash, (unsigned long long)s->windmill,
            s->windmill ? " (the copy will spin)" : " not found: AI freeze instead");
    }
    if (s->windmill && !sw_windmill_is(s->windmill, s->actor)) s->windmill = 0;
    return s->windmill != 0;
}
static void sw_spin(struct Sw *s) {
    uint64_t mp = LD64(s->windmill + WINDMILL_MAXSPEED);
    float mx = in_guest(mp) ? *(const float *)(uintptr_t)mp : 0.5236f, v;
    if (!isfinite(mx) || mx <= 0 || mx > 3) mx = 0.5236f;
    memcpy(&v, (void *)(uintptr_t)(s->windmill + WINDMILL_SPEED), 4);
    if (!(v >= mx)) memcpy((void *)(uintptr_t)(s->windmill + WINDMILL_SPEED), &mx, 4);
}
// One hit switch, each frame: while it differs from what the other player's hit made of theirs, give it a hit (at most
// one per 0.5 s, for up to 3 s); tell them when ours changes by itself (a hit here, or its own time-out).
static void sw_hit_frame(Cpu *c, struct Sw *s, bool together, uint32_t here, uint64_t now) {
    (void)c;
    bool on = sw_signal(s->actor);
    bool spin = sw_spin_ready(s);
    for (int i = 0; i < 8; i++) {
        if (sw_hits_in[i].hash != s->hash) continue;
        s->remote_on = sw_hits_in[i].on;  // (what theirs is now: a held switch stays held until they say "off")
        if (spin) {  // (its own AI follows the speed we give it: nothing to inject)
            if (on == sw_hits_in[i].on || now - sw_hits_in[i].at > 5000000000ull) {
                LOG("[coop] switch %s %u: %s here too (spinning copy)\n", s->name, s->hash, on ? "on" : "off");
                sw_hits_in[i].hash = 0;
            }
            continue;
        }
        if (now - sw_hits_in[i].at > 3000000000ull || on == sw_hits_in[i].on) {
            if (on == sw_hits_in[i].on) LOG("[coop] switch %s %u: %s here too\n", s->name, s->hash, on ? "on" : "off");
            else LOG("[coop] switch %s %u: still %s here after 3 s of hits; giving up\n", s->name, s->hash, on ? "on" : "off");
            sw_hits_in[i].hash = 0;
            continue;
        }
        if (now - s->injected_ns > 500000000ull) {
            // (a faked hit - sw_hit_inject, every variant - does not toggle the crystal: those fields are what the
            // game writes after it took the hit. The game's own setEmittingOutputSignal does drive everything wired
            // to it, verified live; the crystal's own colour then stays as it was here.)
            // A pinwheel's AI sets its signal back every frame (it is on only while it spins): while the other
            // player's pinwheel is on, ours has its AI frozen (the actor freeze bit, as the gate uses) so the signal
            // we set stays; it is unfrozen when theirs goes off (or after SW_PINWHEEL_MAX_NS without word).
            if (sw_is_pinwheel(s->name) && sw_hits_in[i].on && !s->ai_frozen) {
                ST8(s->actor + ENEMY_FLAGS, (uint8_t)(LD8(s->actor + ENEMY_FLAGS) | ENEMY_SKIP_AI));
                s->ai_frozen = true, s->frozen_ns = now;
            }
            Cpu saved = *c;
            c->x[0] = s->actor; c->x[1] = 0; c->x[2] = sw_hits_in[i].on;
            guest_call(c, A_SET_OUT_SIGNAL);
            *c = saved;
            sw_set_visual(c, s, sw_hits_in[i].on);
            s->injected_ns = now;
            LOG("[coop] switch %s %u: the other player's switch state set here (%s)\n", s->name, s->hash,
                sw_hits_in[i].on ? "on" : "off");
        }
    }
    // (a held switch: let go once the other player's is off - our signal then matches - or when they are gone from
    // here; a fixed time-out let an electric switch go after 20 s while theirs stayed powered, seen 2026-11-09)
    if (s->ai_frozen && ((!s->remote_on && !on) || !together)) {
        if (on && !s->remote_on) {  // (they went off but ours did not follow yet)
            Cpu saved = *c;
            c->x[0] = s->actor; c->x[1] = 0; c->x[2] = 0;
            guest_call(c, A_SET_OUT_SIGNAL);
            *c = saved;
            s->injected_ns = now;
        }
        sw_set_visual(c, s, false);
        ST8(s->actor + ENEMY_FLAGS, (uint8_t)(LD8(s->actor + ENEMY_FLAGS) & ~ENEMY_SKIP_AI));
        s->ai_frozen = false;
        LOG("[coop] switch %s %u: our pinwheel runs again\n", s->name, s->hash);
    }
    if (spin && s->remote_on && together) {
        if (!s->spinning) LOG("[coop] switch %s %u: the copy spins for the other player\n", s->name, s->hash);
        s->spinning = true;
        sw_spin(s);
        s->injected_ns = now;  // (its On / Off that follow are not ours to send)
    } else if (s->spinning) {
        s->spinning = false;
        s->injected_ns = now;
        LOG("[coop] switch %s %u: the copy slows down by itself\n", s->name, s->hash);
    }
    if (on != s->hit_on) {
        bool ours = now - s->injected_ns > 1500000000ull && now - s->found_ns > 1000000000ull;
        LOG("[coop] switch %s %u: %s%s\n", s->name, s->hash, on ? "on" : "off", ours ? " (here)" : "");
        if (ours && together) {
            EvSwitch e = {s->hash, here, (uint8_t)on, {0}};
            bool sent = coop_net_send_event(EV_SWITCH_HIT, &e, sizeof e);
            LOG("[coop] switch %s %u: told the other player%s\n", s->name, s->hash, sent ? "" : " (queue full)");
        }
    }
    s->hit_on = on;
}
static void sw_send(struct Sw *s, bool on, uint32_t area, uint64_t now) {
    EvSwitch e = {s->hash, area, (uint8_t)on, {0}};
    if (coop_net_send_event(EV_SWITCH, &e, sizeof e)) {
        if (on != s->pressed_sent) LOG("[coop] switch %s %u: pressed here: %s (told the other player)\n", s->name, s->hash, on ? "on" : "off");
        s->pressed_sent = on, s->sent_ns = now;
    }
}
static void switch_ev(const uint8_t *data, int len) {
    if (len != (int)sizeof(EvSwitch)) return;
    EvSwitch e;
    memcpy(&e, data, sizeof e);
    char area[96];
    if (!switch_on() || e.area != local_area(area, sizeof area) || e.on > 1) return;
    uint64_t now = plat_now_ns();
    int k = -1;
    for (int i = 0; i < SW_MAX; i++)
        if (sw_remote[i].hash == e.hash) { k = i; break; }
    if (k < 0)
        for (int i = 0; i < SW_MAX; i++)
            if (!sw_remote[i].hash || sw_remote[i].until < now) { k = i; break; }
    if (k < 0) return;
    if (!e.on) { sw_remote[k].hash = 0, sw_remote[k].until = 0; }
    else sw_remote[k].hash = e.hash, sw_remote[k].until = now + SW_HOLD_NS;
    LOG("[coop] switch %u: the other player's is %s\n", e.hash, e.on ? "pressed" : "released");
}
static void switches_frame(Cpu *c) {
    if (!switch_on()) return;
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    if (here != sw_area || !local_in_world()) {  // (a load: the actors are gone)
        if (sw_n) LOG("[coop] switches: %d forgotten (area change or load)\n", sw_n);
        sw_n = 0, sw_area = here;
        memset(sw_remote, 0, sizeof sw_remote);
        memset(sw_hits_in, 0, sizeof sw_hits_in);
        return;
    }
    CoopState rs;
    double rage = 99;
    bool together = coop_net_get_remote(&rs, &rage) && rage < 3.0 && (rs.flags & COOP_FLAG_IN_WORLD) && rs.area == here;
    static bool sw_was_together;
    bool joined_shrine = together && !sw_was_together && !strncmp(area, "CDungeon", 8) && coop_net_is_host() && waterpuz_on();
    sw_was_together = together;
    if (!together && !sw_probe) {
        for (int i = 0; i < sw_n; i++) {
            sws[i].holding = false, sws[i].pressed_sent = false, sws[i].remote_on = false;
            if (sws[i].ai_frozen && sw_alive(&sws[i])) {  // (the other player is gone: our held switch runs again)
                ST8(sws[i].actor + ENEMY_FLAGS, (uint8_t)(LD8(sws[i].actor + ENEMY_FLAGS) & ~ENEMY_SKIP_AI));
                sws[i].ai_frozen = false;
                LOG("[coop] switch %s %u: the other player is gone; ours runs again\n", sws[i].name, sws[i].hash);
            }
        }
        memset(sw_remote, 0, sizeof sw_remote);
        return;
    }
    static uint64_t look_ns;
    if (now >= look_ns) {
        int budget = 4;
        walk_actors(c, sw_name, sw_found, &budget);
        look_ns = now + (budget > 0 ? 1000000000ull : 100000000ull);
    }
    for (int i = 0; i < sw_n; i++) {
        struct Sw *s = &sws[i];
        if (!sw_alive(s)) { sws[i--] = sws[--sw_n]; continue; }
        if (s->hit) {
            if (joined_shrine && s->hit_on && !sw_is_pinwheel(s->name) && now - s->found_ns > 1500000000ull) {
                EvSwitch e = {s->hash, here, 1, {0}};
                coop_net_send_event(EV_SWITCH_HIT, &e, sizeof e);
                LOG("[coop] switch %s %u: host shared active shrine switch on join\n", s->name, s->hash);
            }
            sw_hit_frame(c, s, together, here, now);
            continue;
        }
        float y =*(const float *)(uintptr_t)(s->body + 0x1a4);
        bool theirs = false;
        for (int k = 0; k < SW_MAX; k++)
            theirs |= sw_remote[k].hash == s->hash && sw_remote[k].until > now;
        bool hold = (together && theirs) || now < s->dev_until;
        if (hold != s->holding) {
            LOG("[coop] switch %s %u: %s (plate %.2f, rest %.2f)\n", s->name, s->hash,
                hold ? "held down for the other player" : "let go", y, s->rest_y);
            if (!hold) s->released_ns = now;
            s->holding = hold;
        }
        if (hold) {
            if (LD32(s->rb + 0x68) & 0x80000) {  // (fixed: a velocity would be ignored)
                Cpu saved = *c;
                c->x[0] = s->rb; c->x[1] = 0; c->x[2] = 0;
                guest_call(c, A_RB_SET_FIXED);
                *c = saved;
            }
            static const float down[3] = {0, -3.0f, 0};
            rb_set_linvel(c, s->rb, down);
        } else if (y > s->rest_y && now - s->released_ns > SW_GRACE_NS) {
            s->rest_y = y;
        }
        if (!together) continue;
        bool pressed = !hold && now - s->released_ns > SW_GRACE_NS && s->rest_y - y > SW_PRESSED_M;
        if (pressed && (!s->pressed_sent || now - s->sent_ns > SW_REFRESH_NS)) sw_send(s, true, here, now);
        else if (!pressed && s->pressed_sent && !hold) sw_send(s, false, here, now);
    }
}
// `coop switch` (dev): the floor switches known here, to the log; `coop switch hold <hash> <s>`: hold one down.
static struct { volatile int pending; uint32_t hash; float secs; } switch_dev;
static void switch_dev_frame(void) {
    swhit_dev_frame();
    if (!switch_dev.pending) return;
    switch_dev.pending = 0;
    sw_probe = true;
    uint64_t now = plat_now_ns();
    for (int i = 0; i < sw_n; i++) {
        struct Sw *s = &sws[i];
        if (switch_dev.hash && s->hash == switch_dev.hash) s->dev_until = now + (uint64_t)(switch_dev.secs * 1e9);
        if (s->hit) { LOG("[coop] switch %s %u: hit switch, %s\n", s->name, s->hash, s->hit_on ? "on" : "off"); continue; }
        const float *bp = (const float *)(uintptr_t)(s->body + 0x1a0);
        LOG("[coop] switch %s %u: plate %.2f %.2f %.2f rest %.2f flags %08x%s%s%s\n", s->name, s->hash, bp[0], bp[1],
            bp[2], s->rest_y, LD32(s->rb + 0x68), s->holding ? " held" : "", s->pressed_sent ? " reported" : "",
            now < s->dev_until ? " dev-hold" : "");
    }
    LOG("[coop] switches: %d known\n", sw_n);
}
