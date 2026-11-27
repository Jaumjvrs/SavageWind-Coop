// BOTW 1.9.0 addresses. Keeping these in co-op preserves the installed game library ID.
#define A_STASIS_START_KEY 0x70015f59f6ull
#define A_STASIS_ARROW_KEY 0x70015cb8deull
#define A_STASIS_WORLD_SLOT 0x7001e3f2b0ull
#define A_STASIS_START_HANDLE 0x7000b3d780ull
#define A_STASIS_EVENT_INIT 0x700058194cull
#define A_STASIS_START_EFFECT 0x70011f58acull
#define A_STASIS_TARGET_MTX 0x7001093d14ull
#define A_STASIS_TARGET_ANCHOR 0x7001093c5cull
#define A_STASIS_EMIT 0x700159f00cull
#define A_STASIS_SET_PROP 0x7000f515ecull
#define STASIS_ARROW_HANDLE 0x2b0
#define STASISFX_RANGE 60.0f

static struct {
    uint64_t actor;
    uint32_t hash;
    uint16_t serial;
    float first_left;
    bool enemy;
} stasisfx_local;
static struct {
    uint64_t handles;  // 16-byte start handle followed by the 32-byte effect pair for the arrow
    uint32_t hash, area;
    uint16_t serial;
    unsigned chain_wait;
    bool chain_started;
    uint64_t retry_ns;
    CoopStasisFx arrow;
} stasisfx;
static uint64_t stasisfx_scratch;
static volatile uint32_t stasisfoe_dev_hash;
static struct {
    uint32_t hash, unit_id, area;
    uint64_t actor;
    bool confirmed, releasing;
} stasisfoe;

// The follower's skip-AI bit is also BOTW's native Stasis bit. Leave it to the
// game between the incoming lock and its release, including in animation jobs.
static uint64_t stasisfoe_job_id;
static bool stasisfoe_controls_actor(uint64_t actor) {
    uint64_t id = __atomic_load_n(&stasisfoe_job_id, __ATOMIC_RELAXED);
    return (id && actor && id == 1ull + LD32(actor + PROC_ID)) || stasisfoe_aim_actor(actor);
}

static unsigned stasisfoe_aim_grace, stasisfoe_press_wait;
static uint32_t stasisfoe_press_hash, stasisfoe_last_aim_hash;
static uint64_t stasisfoe_last_aim_ns, stasisfoe_a_edge_ns;
static bool stasisfoe_a_down, stasisfoe_edge_used;
#define STASISFOE_PRESS_NS 300000000ull  // an A press and the crosshair on a follower count together within this
extern uint64_t input_buttons_now(void);  // (hle/input.c: bit 0 = A)
// (2026-11-20) The native cast (0x7000b3d68c) checks the skip-AI bit, which the follower code flips around its AI
// job; a real A press missed every time in test_stasisfoe while the dev message worked. An A press while aiming
// Stasis, with the crosshair on a follower up to STASISFOE_PRESS_NS before or after it: if the manager has not
// locked within 3 frames, send the same lock message as the dev cast (the highlight flickers, so the press and the
// highlight were often not in the same frame). Off: SAVAGEWIND_COOP_STASISPRESS=0.
static void stasisfoe_press(uint64_t now) {
    static int press_on = -1;
    if (stasisfoe_edge_used || stasisfoe_press_wait || !stasisfoe_last_aim_hash) return;
    if (now - stasisfoe_a_edge_ns > STASISFOE_PRESS_NS || now - stasisfoe_last_aim_ns > STASISFOE_PRESS_NS) return;
    if (!env_on("SAVAGEWIND_COOP_STASISPRESS", &press_on)) return;
    stasisfoe_edge_used = true;
    stasisfoe_press_wait = 4, stasisfoe_press_hash = stasisfoe_last_aim_hash;
}
static void stasisfoe_local_frame(Cpu *c, bool world, uint32_t dev) {
    if (!stasisfoe_ai_orig) return;
    uint64_t mgr = world ? stasis_mgr() : 0, now = plat_now_ns();
    uint64_t actor = 0;
    bool a = mgr && LD8(mgr + 0x40) && (input_buttons_now() & 1);  // (A held while aiming Stasis)
    if (a && !stasisfoe_a_down) stasisfoe_a_edge_ns = now, stasisfoe_edge_used = false;
    stasisfoe_a_down = a;
    if (mgr && !stasis_shown) {
        actor = stasis_manager_target(c);
        if (!actor && LD8(mgr + 0x40)) actor = stasis_link_actor(c, mgr + 0x3360);
        if (actor || dev) {
            for (int i = 0; i < enemy_count; i++) {
                struct Enemy *e = &enemies[i];
                if ((actor ? e->actor != actor : e->hash != dev) || !enemy_alive(e)) continue;
                if (!stasisfoe_aim_actor(e->actor)) {
                    if (!e->following || LD8(mgr + STASIS_LOCKED)) break;
                    enemy_ai(e, true);
                    __atomic_store_n(&stasisfoe_aim_id, 1ull + e->unit_id, __ATOMIC_RELAXED);
                    LOG("[coop] stasisfoe: %s follower %s %u\n", dev ? "dev cast on" : "rune aimed at",
                        e->name, e->hash);
                }
                stasisfoe_aim_grace = 2;  // allow the queued lock to reach the manager
                if (!dev) stasisfoe_last_aim_hash = e->hash, stasisfoe_last_aim_ns = now;
                stasisfoe_press(now);
                return;
            }
        }
        stasisfoe_press(now);
        if (!actor && stasisfoe_aim_grace) { stasisfoe_aim_grace--; return; }
    }
    __atomic_store_n(&stasisfoe_aim_id, 0, __ATOMIC_RELAXED);
    stasisfoe_aim_grace = 0;
}

static struct Enemy *stasisfoe_enemy(uint32_t hash) {
    for (int i = 0; i < enemy_count; i++)
        if (enemies[i].hash == hash && enemy_alive(&enemies[i]) &&
            (int32_t)LD32(enemies[i].actor + ENEMY_LIFE) > 0) return &enemies[i];
    return NULL;
}

// Every participant, including the one running the AI, receives the caster's snapshot.
// Only the game's one native manager owns the lock. Never replace a local cast.
static void stasisfoe_frame(Cpu *c) {
    if (!stasisfoe_on() || !stasis_on()) return;
    bool world = local_in_world();
    struct Enemy *held = world && stasisfoe.hash && enemy_area == stasisfoe.area ?
                         stasisfoe_enemy(stasisfoe.hash) : NULL;
    if (stasisfoe.hash && (!held || held->unit_id != stasisfoe.unit_id || held->actor != stasisfoe.actor ||
                           stasis_shown != stasisfoe.actor)) {
        if (stasis_shown == stasisfoe.actor) stasis_shown = 0, stasis_cd.armed = false;
        __atomic_store_n(&stasisfoe_job_id, 0, __ATOMIC_RELAXED);
        memset(&stasisfoe, 0, sizeof stasisfoe);
        held = NULL;
    }
    uint32_t dev = stasisfoe_dev_hash;
    stasisfoe_dev_hash = 0;
    stasisfoe_local_frame(c, world && enemy_area, dev);
    if (!world || !enemy_area) return;
    if (dev && !held && !stasis_busy(c)) {
        struct Enemy *e = stasisfoe_enemy(dev);
        if (e) stasis_msg(c, e->actor, STASIS_MSG_LOCK);
    }
    if (stasisfoe_press_wait && !--stasisfoe_press_wait) {  // (the A press above: the native cast did not lock)
        struct Enemy *e = stasisfoe_enemy(stasisfoe_press_hash);
        uint64_t mgr = stasis_mgr();
        float recharge = 0;
        if (mgr) memcpy(&recharge, (const void *)(uintptr_t)(mgr + STASIS_RECHARGE), 4);
        bool busy = held || stasis_busy(c) || !mgr || recharge > 0;  // (never skip the rune's recharge)
        // (still aiming Stasis, and the crosshair was on it a moment ago: the highlight flickers)
        bool aimed = e && mgr && LD8(mgr + 0x40) && e->following &&
                     plat_now_ns() - stasisfoe_last_aim_ns < 2 * STASISFOE_PRESS_NS;
        if (aimed && !busy) {
            if (!stasisfoe_aim_actor(e->actor)) {  // (as the dev cast: the native manager owns its bit from here)
                enemy_ai(e, true);
                __atomic_store_n(&stasisfoe_aim_id, 1ull + e->unit_id, __ATOMIC_RELAXED);
                stasisfoe_aim_grace = 2;
            }
            stasis_msg(c, e->actor, STASIS_MSG_LOCK);
        }
        LOG("[coop] stasisfoe: A press on follower %u: %s\n", stasisfoe_press_hash,
            busy ? "the game locked it itself" : aimed ? "lock sent" : "no longer aimed");
    }
    if (held && stasisfoe.releasing) {
        if (stasis_manager_target(c) == held->actor) return;
        if (stasisfoe.confirmed)
            LOG("[coop] stasisfoe: native manager cleared %s %u\n", held->name, held->hash);
        if (stasis_shown == held->actor) stasis_shown = 0;
        __atomic_store_n(&stasisfoe_job_id, 0, __ATOMIC_RELAXED);
        memset(&stasisfoe, 0, sizeof stasisfoe);
        held = NULL;
    }
    uint32_t peers = coop_net_world_players(0.5);
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
        if (!(peers & (1u << p))) continue;
        CoopState rs;
        double age;
        coop_net_view(p);
        bool ok = coop_net_get_remote(&rs, &age);
        coop_net_view(-1);
        if (!ok || rs.area != enemy_area || !rs.stasisfx.foe_ticks || age >= 0.5 ||
            (held && rs.stasisfx.hash != held->hash)) continue;
        struct Enemy *e = stasisfoe_enemy(rs.stasisfx.hash);
        float pos[3];
        if (!e || !(actor_dist(e->actor, rs.pos, pos) <= STASISFX_RANGE)) continue;
        if (!held) {
            if (stasis_busy(c)) return;
            if (e->following) enemy_ai(e, true);
            stasis_native_copy(c, e->actor, true, STASIS_MSG_LOCK);
            if (stasis_shown != e->actor) return;
            stasisfoe.hash = e->hash, stasisfoe.actor = e->actor;
            stasisfoe.unit_id = e->unit_id, stasisfoe.area = enemy_area;
            __atomic_store_n(&stasisfoe_job_id, 1ull + e->unit_id, __ATOMIC_RELAXED);
            LOG("[coop] stasisfoe: player %d requested native lock on %s %u (%s)\n",
                p, e->name, e->hash, e->mine ? "AI owner" : "follower");
        }
        if (stasis_manager_target(c) == e->actor) {
            if (!stasisfoe.confirmed) {
                stasisfoe.confirmed = true;
                LOG("[coop] stasisfoe: native manager holds %s %u (%s), caster has %u ticks left\n",
                    e->name, e->hash, e->mine ? "AI owner" : "follower", rs.stasisfx.foe_ticks);
            }
            float left = fmaxf(1, rs.stasisfx.foe_ticks - (float)age * 30);
            memcpy((void *)(uintptr_t)(stasis_mgr() + STASIS_LEFT), &left, sizeof left);
        }
        return;
    }
    if (held) {
        stasis_native_copy(c, held->actor, false, STASIS_MSG_RELEASE);
        // Suppress the echo until the queued release reaches the native manager.
        stasis_shown = held->actor, stasisfoe.releasing = true;
        LOG("[coop] stasisfoe: released %s %u\n", held->name, held->hash);
    }
}

static bool stasisfx_on(void) {
    static int on = -1;
    if (!stasis_on() || stasis_fx_mode() != 1) return false;
    if (on < 0) {
        on = in_guest(A_STASIS_START_KEY) && in_guest(A_STASIS_ARROW_KEY) &&
             !memcmp((const void *)(uintptr_t)A_STASIS_START_KEY, "BitaLocked_Start", sizeof "BitaLocked_Start") &&
             !memcmp((const void *)(uintptr_t)A_STASIS_ARROW_KEY, "BitaLocked_Vector", sizeof "BitaLocked_Vector");
        if (!on) LOG("[coop] stasisfx: the effect keys do not match game 1.9.0\n");
    }
    return on;
}

static bool stasisfx_log(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_STASISFX_LOG");
        on = e && !strcmp(e, "1");
    }
    return on;
}

static void stasisfx_publish(Cpu *c, CoopState *s) {
    bool visuals = stasisfx_on();
    uint64_t actor = (s->flags & COOP_FLAG_IN_WORLD) && s->area && (visuals || stasisfoe_on()) ? stasis_target(c) : 0;
    if (actor != stasisfx_local.actor && stasisfx_local.enemy) {
        LOG("[coop] stasisfoe: caster manager cleared enemy %u\n", stasisfx_local.hash);
        stasisfx_local.enemy = false;
    }
    if (!actor) {
        stasisfx_local.actor = 0;
        return;
    }
    uint64_t mgr = stasis_mgr();
    if (!mgr) return;
    float left = LDF32(mgr + STASIS_LEFT);
    if (actor != stasisfx_local.actor) {
        stasisfx_local.actor = actor;
        stasisfx_local.hash = actor_hash(c, actor);
        stasisfx_local.first_left = left;
        if (!++stasisfx_local.serial) ++stasisfx_local.serial;
    }
    CoopStasisFx *sfx = &s->stasisfx;
    if (stasisfoe_on()) {
        // Run-time enemies acquire their shared id when the enemy roster attaches.
        for (int i = 0; i < enemy_count; i++) {
            struct Enemy *e = &enemies[i];
            if (e->actor != actor || !enemy_alive(e)) continue;
            stasisfx_local.hash = e->hash;
            if (!stasisfx_local.enemy) {
                LOG("[coop] stasisfoe: caster manager holds %s %u (%s)\n", e->name, e->hash,
                    e->following ? "follower" : "local AI");
                stasisfx_local.enemy = true;
            }
            sfx->foe_ticks = (uint8_t)fminf(255, ceilf(left));
            break;
        }
    }
    if (!stasisfx_local.hash || (!visuals && !sfx->foe_ticks)) return;
    sfx->hash = stasisfx_local.hash;
    sfx->serial = stasisfx_local.serial;
    if (!visuals) return;
    // The start effect is brief. A late snapshot of a lock must not restart it.
    if (stasisfx_local.first_left - left < 35.0f) sfx->flags |= COOP_STASISFX_START;
    uint64_t fx = elink_live_effect(mgr + STASIS_ENTRY0 + STASIS_ARROW_HANDLE);
    if (!fx) return;
    float x = LDF32(fx + 0xc8), y = LDF32(fx + 0xd8), z = LDF32(fx + 0xe8);
    float len = sqrtf(x * x + y * y + z * z);
    float q = (LDF32(fx + 0xbc) - 1.0f) / 19.0f;
    float alpha = LDF32(fx + 0xa4);
    if (!(len > 0.99f && len < 1.01f && q >= 0.009f && q <= 1.001f && (alpha == 0 || alpha == 1))) return;
    sfx->direction[0] = (int8_t)lroundf(x * 127.0f / len);
    sfx->direction[1] = (int8_t)lroundf(y * 127.0f / len);
    sfx->direction[2] = (int8_t)lroundf(z * 127.0f / len);
    sfx->charge = (uint8_t)lroundf(q * 100.0f);
    sfx->flags |= COOP_STASISFX_ARROW | (alpha == 1 ? COOP_STASISFX_VISIBLE : 0);
}

static void stasisfx_fade(Cpu *c, uint64_t handle, size_t size) {
    if (elink_live_effect(handle)) {
        Cpu saved = *c;
        c->x[0] = handle;
        guest_call(c, A_FX_KILL);
        *c = saved;
    }
    memset((void *)(uintptr_t)handle, 0, size);
}

static void stasisfx_stop_arrow(Cpu *c) {
    if (stasisfx.handles) stasisfx_fade(c, stasisfx.handles + 16, 32);
    if (stasisfx.arrow.flags && stasisfx_log()) LOG("[coop] stasisfx: stopped arrow for %u\n", stasisfx.hash);
    stasisfx.arrow = (CoopStasisFx){0};
    stasisfx.retry_ns = 0;
}

static void stasisfx_clear(Cpu *c, bool in_world) {
    uint64_t handles = stasisfx.handles;
    if (handles && in_world) {
        stasisfx_fade(c, handles, 16);
        stasisfx_stop_arrow(c);
    } else if (handles) {
        // Scene teardown owns these effects.
        memset((void *)(uintptr_t)handles, 0, 48);
    }
    memset(&stasisfx, 0, sizeof stasisfx);
    stasisfx.handles = handles;
}

static uint64_t stasisfx_user(uint64_t actor) {
    uint64_t link = in_guest(actor) ? LD64(actor + 0x568) : 0;
    uint64_t user = in_guest(link) ? LD64(link + 0x48) : 0;
    return in_guest(user) ? user : 0;
}

static float stasisfx_prop(uint64_t user) {
    uint64_t values = user ? LD64(user + 0x70) : 0;
    return in_guest(values + 16) ? LDF32(values + 16) : NAN;
}

static void stasisfx_set_prop(Cpu *c, uint64_t user, float value) {
    if (!user || !isfinite(value)) return;
    Cpu saved = *c;
    c->x[0] = user, c->x[1] = 4;
    memcpy(&c->v[0], &value, sizeof value);
    guest_call(c, A_STASIS_SET_PROP);
    *c = saved;
}

static bool stasisfx_native_chains(Cpu *c, uint64_t actor) {
    uint64_t mgr = stasis_mgr();
    if (!mgr) return false;
    Cpu saved = *c;
    c->x[0] = mgr, c->x[1] = actor;
    guest_call(c, A_STASIS_START_HANDLE);
    uint64_t handle = c->x[0];
    *c = saved;
    return in_guest(handle) && elink_live_effect(handle);
}

static uint64_t stasisfx_start_chains(Cpu *c, uint64_t actor) {
    uint64_t world = LD64(A_STASIS_WORLD_SLOT);
    if (!in_guest(world) || !LD8(world + 0xb0)) return 0;
    uint64_t events = LD64(world + 0xa0);
    uint64_t emitter = in_guest(events) ? LD64(events + 0x38) : 0;
    uint64_t fallback = stasisfx_user(emitter), target = stasisfx_user(actor);
    if (!fallback) return 0;
    float was_fallback = stasisfx_prop(fallback), was_target = stasisfx_prop(target);
    uint64_t event = stasisfx_scratch;
    memset((void *)(uintptr_t)event, 0, 0xc8);
    Cpu saved = *c;
    c->x[0] = event;
    guest_call(c, A_STASIS_EVENT_INIT);
    *c = saved;
    c->x[0] = event, c->x[1] = actor;
    guest_call(c, A_STASIS_TARGET_MTX);
    *c = saved;
    ST64(event + 0xb8, actor);
    ST32(event + 0x90, 0x1b);
    c->x[0] = events, c->x[1] = stasisfx.handles, c->x[2] = event;
    guest_call(c, A_STASIS_START_EFFECT);
    *c = saved;
    stasisfx_set_prop(c, target, was_target);
    stasisfx_set_prop(c, fallback, was_fallback);
    return elink_live_effect(stasisfx.handles);
}

static void stasisfx_chains(Cpu *c, const CoopStasisFx *s, uint64_t actor) {
    if (!stasisfx.chain_started && (s->flags & COOP_STASISFX_START)) {
        // The normal object lock message produces these chains. Allow its
        // actor update to run before using a private handle for a missed lock.
        if (stasisfx_native_chains(c, actor)) {
            stasisfx.chain_started = true;
            if (stasisfx_log()) LOG("[coop] stasisfx: native chains for %u\n", s->hash);
        } else if (++stasisfx.chain_wait >= 8) {
            stasisfx.chain_wait = 0;
            if (stasisfx_start_chains(c, actor)) {
                stasisfx.chain_started = true;
                if (stasisfx_log()) LOG("[coop] stasisfx: replayed chains for %u\n", s->hash);
                return;  // the native start helper supplied the initial bounds matrix
            }
        }
    }
    uint64_t fx = elink_live_effect(stasisfx.handles);
    if (!fx) return;
    Cpu saved = *c;
    c->x[0] = stasisfx_scratch, c->x[1] = actor;
    guest_call(c, A_STASIS_TARGET_MTX);
    *c = saved;
    memcpy((void *)(uintptr_t)(fx + 0xc0), (const void *)(uintptr_t)stasisfx_scratch, 48);
    ST32(fx + 0x180, LD32(fx + 0x180) | 8);
}

static uint64_t stasisfx_start_arrow(Cpu *c, uint64_t actor, uint64_t handle) {
    uint64_t world = LD64(A_STASIS_WORLD_SLOT);
    uint64_t emitter = in_guest(world) ? LD64(world + 0x30) : 0;
    uint64_t user = stasisfx_user(emitter);
    if (!user) return 0;
    float was = stasisfx_prop(user), property = NAN;
    // The local copy has the same component used by the owner's native arrow variant selection.
    Cpu saved = *c;
    uint64_t vt = LD64(actor);
    if (in_guest(vt) && in_guest(LD64(vt + 0x3f8))) {
        c->x[0] = actor;
        guest_call(c, LD64(vt + 0x3f8));
        uint64_t component = c->x[0];
        *c = saved;
        vt = in_guest(component) ? LD64(component) : 0;
        if (in_guest(vt) && in_guest(LD64(vt + 0x68))) {
            c->x[0] = component;
            guest_call(c, LD64(vt + 0x68));
            memcpy(&property, &c->v[0], sizeof property);
            *c = saved;
        }
    }
    if (isfinite(was)) stasisfx_set_prop(c, user, property);
    memset((void *)(uintptr_t)handle, 0, 32);
    c->x[0] = emitter, c->x[1] = A_STASIS_ARROW_KEY, c->x[2] = 0, c->x[3] = handle;
    guest_call(c, A_STASIS_EMIT);
    *c = saved;
    stasisfx_set_prop(c, user, was);
    return elink_live_effect(handle);
}

static void stasisfx_arrow_frame(Cpu *c, const CoopStasisFx *s, uint64_t actor) {
    if (!(s->flags & COOP_STASISFX_ARROW)) {
        if (stasisfx.arrow.flags) stasisfx_stop_arrow(c);
        return;
    }
    uint64_t handle = stasisfx.handles + 16;
    uint64_t fx = elink_live_effect(handle);
    if (!fx) {
        uint64_t now = plat_now_ns();
        if (now < stasisfx.retry_ns) return;
        fx = stasisfx_start_arrow(c, actor, handle);
        stasisfx.retry_ns = now + 250000000ull;
        if (!fx) return;
    }

    Cpu saved = *c;
    c->x[0] = stasisfx_scratch, c->x[1] = actor;
    guest_call(c, A_STASIS_TARGET_ANCHOR);
    *c = saved;
    float x = s->direction[0], y = s->direction[1], z = s->direction[2];
    float len = sqrtf(x * x + y * y + z * z);
    x /= len; y /= len; z /= len;
    float rx = z, ry = 0, rz = -x;
    if (fabsf(y) > 0.999f) { rx = 0; ry = -z; rz = y; }
    float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
    rx /= rlen; ry /= rlen; rz /= rlen;
    const float *pos = (const float *)(uintptr_t)stasisfx_scratch;
    const float matrix[] = {
        rx, y * rz - z * ry, x, pos[0],
        ry, z * rx - x * rz, y, pos[1],
        rz, x * ry - y * rx, z, pos[2],
    };
    float q = s->charge / 100.0f;
    const float scale[] = {1.0f + 1.5f * q, 1.0f, 1.0f + 19.0f * q};
    memcpy((void *)(uintptr_t)(fx + 0xb4), scale, sizeof scale);
    memcpy((void *)(uintptr_t)(fx + 0xc0), matrix, sizeof matrix);
    ST32(fx + 0xa4, s->flags & COOP_STASISFX_VISIBLE ? 0x3f800000 : 0);
    ST32(fx + 0x180, LD32(fx + 0x180) | 10);
    if (stasisfx_log() &&
        (s->charge != stasisfx.arrow.charge || ((s->flags ^ stasisfx.arrow.flags) & COOP_STASISFX_VISIBLE) ||
         memcmp(s->direction, stasisfx.arrow.direction, sizeof s->direction)))
        LOG("[coop] stasisfx: arrow for %u charge=%u direction=%d,%d,%d visible=%d\n", s->hash, s->charge,
            s->direction[0], s->direction[1], s->direction[2], !!(s->flags & COOP_STASISFX_VISIBLE));
    stasisfx.arrow = *s;
}

static void stasisfx_frame(Cpu *c, const CoopState *rs, bool fresh) {
    if (!stasisfx_on()) return;
    bool in_world = local_in_world();
    if (!in_world || !fresh || !rs->stasisfx.hash || !avatar.actor || !avatar_alive() ||
        rs->area != avatar.area || rs->area != obj_area || spawn_hold()) {
        if (stasisfx.hash) stasisfx_clear(c, in_world);
        return;
    }
    const CoopStasisFx *s = &rs->stasisfx;
    if (s->hash != stasisfx.hash || s->serial != stasisfx.serial || rs->area != stasisfx.area) {
        stasisfx_clear(c, true);
        stasisfx.hash = s->hash, stasisfx.serial = s->serial, stasisfx.area = rs->area;
    }
    uint64_t actor = 0;
    for (int i = 0; i < obj_n; i++) {
        const struct Obj *o = &objs[i];
        if (o->hash == s->hash && !o->parked && obj_alive(o)) {
            actor = o->actor;
            break;
        }
    }
    if (actor && actor != stasisfx_local.actor) {
        uint64_t m = actor + ACTOR_MTX;
        const float pos[] = {LDF32(m + 12), LDF32(m + 28), LDF32(m + 44)};
        if (coop_magbeam_dist2(pos, rs->pos) <= STASISFX_RANGE * STASISFX_RANGE) {
            if (!stasisfx.handles) stasisfx.handles = (uint64_t)(uintptr_t)hle_alloc(48);
            if (!stasisfx_scratch) stasisfx_scratch = (uint64_t)(uintptr_t)hle_alloc(0xc8);
            if (stasisfx.handles && stasisfx_scratch) {
                stasisfx_chains(c, s, actor);
                stasisfx_arrow_frame(c, s, actor);
            }
            return;
        }
    }
    stasisfx_clear(c, true);
}
