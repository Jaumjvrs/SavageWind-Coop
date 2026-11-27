// Mirror the native effect on a shared enemy without submitting another attack or chemical damage tick.
static uint64_t elemfoe_handles;
#define ELEMFOE_STATUS_VT 0x7001cbab98ull
#define A_ELEMFOE_START 0x7000b873d4ull
#define A_ICE_MATERIAL 0x700124da60ull
#define ELEMFOE_HOLD_NS 800000000ull  // (a started burn / freeze / shock is sent at least this long)
#define ELEMFOE_STATUS 0xe78  // 1.9.0: actor init 0x700057b148 stores the controller made by vfunc +0x590
static struct { uint32_t hash; volatile unsigned kind; } elemfoe_dev;
static const struct {
    uint32_t flag;
    const char *name, *effect;
} elemfoe_fx[] = {
    {COOP_ENEMY_BURNING, "fire", "Chemical_Fire"},
    {COOP_ENEMY_FROZEN, "ice", "Frozen"},
    {COOP_ENEMY_SHOCKED, "shock", "ElectricShock"},
};
#define ELEMFOE_FX_COUNT (sizeof elemfoe_fx / sizeof elemfoe_fx[0])

static bool elemfoe_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_ELEMFOE");
    return on;
}

static void actor_ice_material(Cpu *c, uint64_t actor, bool show) {
    uint64_t model = LD64(actor + 0x4e0);
    if (!in_guest(model + 0x40) || !in_guest(LD64(model + 0x40))) return;
    // Set the same ice uniform as native freeze without queuing damage.
    float ratio = show ? 1.0f : 0.0f;
    Cpu saved = *c;
    memcpy(&c->v[0], &ratio, sizeof ratio);
    c->x[0] = 0;
    c->x[1] = model;
    guest_call(c, A_ICE_MATERIAL);
    *c = saved;
}

static void elemfoe_ice_look(Cpu *c, struct Enemy *e, bool show, bool native) {
    static int on = -1;
    if (!env_on("SAVAGEWIND_COOP_ICELOOK", &on) || (!show && !e->ice_held)) return;
    if (show) {
        if (!e->ice_held) {
            uint64_t time = as_layer_time(c, as_layer_rec(e->actor, 0, 0));
            e->ice_frame = time ? LDF32(time + 4) : 0.0f;
        }
        if (isfinite(e->ice_frame)) as_set_frame(c, e->actor, 0, 0, e->ice_frame);
    }
    if (!native) actor_ice_material(c, e->actor, show);
    if (e->ice_held != show)
        LOG("[coop] elemfoe: %s %u ice look %s\n", e->name, e->hash, show ? "started" : "ended");
    e->ice_held = show;
}

static uint64_t elemfoe_status(uint64_t actor) {
    uint64_t field = actor + ELEMFOE_STATUS, lo, hi;
    if (!in_guest(field + 8) || !plat_committed_range(field, field + 8, &lo, &hi) || lo != field || hi != field + 8)
        return 0;
    uint64_t status = LD64(field);
    // The short controller used by some actor types has a different vtable.
    if ((status & 7) || !in_guest(status) || !in_guest(status + 0x18) ||
        !plat_committed_range(status, status + 0x18, &lo, &hi) || lo != status || hi != status + 0x18)
        return 0;
    return LD64(status) == ELEMFOE_STATUS_VT && LD64(status + 0x10) == actor ? status : 0;
}

static void elemfoe_visual(Cpu *c, struct Enemy *e, uint32_t show) {
    uint32_t changed = show ^ e->elem_shown;
    if (!changed) return;
    if (show && !elemfoe_handles)
        elemfoe_handles = (uint64_t)(uintptr_t)hle_alloc(ENEMY_TABLE * ELEMFOE_FX_COUNT * 0x20);
    if (!elemfoe_handles) return;
    for (unsigned i = 0; i < ELEMFOE_FX_COUNT; i++) {
        uint32_t flag = elemfoe_fx[i].flag;
        if (!(changed & flag)) continue;
        uint64_t h = elemfoe_handles + ((e - enemies) * ELEMFOE_FX_COUNT + i) * 0x20;
        if (show & flag) {
            if (flag == COOP_ENEMY_FROZEN) fx_emit(c, e->actor, "Frozen_Start");
            memset((void *)(uintptr_t)h, 0, 0x20);
            if (!xl_event_h(c, e->actor, elemfoe_fx[i].effect, XL_EV_ELINK, h)) continue;
            e->elem_shown |= flag;
            LOG("[coop] elemfoe: %s %u mirror %s %s\n", e->name, e->hash, elemfoe_fx[i].name,
                elink_live_effect(h) ? "started" : "requested (no live handle)");
        } else {
            if (elink_live_effect(h)) {
                Cpu saved = *c;
                c->x[0] = h;
                guest_call(c, A_FX_KILL);
                *c = saved;
            }
            memset((void *)(uintptr_t)h, 0, 0x20);
            e->elem_shown &= ~flag;
            LOG("[coop] elemfoe: %s %u mirror %s ended\n", e->name, e->hash, elemfoe_fx[i].name);
        }
    }
}

static void elemfoe_frame(Cpu *c, struct Enemy *e, unsigned dev_kind) {
    bool alive = e->actor && e->last > 0 && !e->animal;
    uint32_t uid = alive ? e->unit_id : 0;
    if (uid != e->elem_unit_id) {
        if (e->ice_held && e->actor && e->unit_id == e->elem_unit_id)
            elemfoe_ice_look(c, e, false, e->elem_native & COOP_ENEMY_FROZEN);
        elemfoe_visual(c, e, 0);
        e->ice_held = false;
        e->elem_native = e->elem_remote = e->elem_send = e->elem_recent = 0;
        e->elem_unit_id = uid;
        e->elem_status = uid ? elemfoe_status(e->actor) : 0;
    }
    uint64_t status = e->elem_status;
    if (alive && dev_kind) {
        if (status) {
            Cpu saved = *c;
            c->x[0] = status;
            c->x[1] = dev_kind + 1;  // native status 2 = burn, 3 = freeze, 4 = shock
            guest_call(c, A_ELEMFOE_START);
            *c = saved;
        }
        LOG("[coop] elemfoe: dev %s %s %u %s\n", elemfoe_fx[dev_kind - 1].name, e->name, e->hash,
            status ? "requested" : "native controller unavailable");
    }
    uint32_t native = alive && fire_burning(fire_chem(c, e->actor)) ? COOP_ENEMY_BURNING : 0;
    uint16_t flags = status ? LD16(status + 8) : 0;
    for (unsigned i = 0; i < ELEMFOE_FX_COUNT; i++) {
        uint32_t flag = elemfoe_fx[i].flag;
        if (flags & (1u << (i + 2))) native |= flag;
        if ((native ^ e->elem_native) & flag)
            LOG("[coop] elemfoe: %s %u native %s %s\n", e->name, e->hash, elemfoe_fx[i].name,
                native & flag ? "started" : "ended");
    }
    // (2026-11-20) A freeze broken by the next hit can last less than one enemy packet, so the partner never saw it
    // (test_elem: ice started/ended on the host, nothing received). A flag that started is sent for at least
    // ELEMFOE_HOLD_NS. Off: SAVAGEWIND_COOP_ELEMHOLD=0.
    static int hold_on = -1;
    uint64_t now = plat_now_ns();
    uint32_t started = native & ~e->elem_native;
    if (started && env_on("SAVAGEWIND_COOP_ELEMHOLD", &hold_on)) e->elem_recent |= started, e->elem_recent_ns = now;
    if (e->elem_recent && now - e->elem_recent_ns > ELEMFOE_HOLD_NS) e->elem_recent = 0;
    e->elem_native = native;
    e->elem_send = native | e->elem_recent;
    uint32_t remote = alive ? coop_net_remote_enemy_elem(e->hash) : 0;
    if (remote != e->elem_remote) {
        LOG("[coop] elemfoe: %s %u received flags %x\n", e->name, e->hash, remote);
        e->elem_remote = remote;
    }
    uint32_t show = remote & ~native;
    if (uid)
        elemfoe_ice_look(c, e, (show & COOP_ENEMY_FROZEN) && e->following, native & COOP_ENEMY_FROZEN);
    elemfoe_visual(c, e, show);
}

RT_ON_RESET(coop_elemfoe_reset) {
    elemfoe_handles = 0;
    elemfoe_dev.kind = 0;
    for (int i = 0; i < enemy_count; i++) {
        enemies[i].elem_native = enemies[i].elem_remote = enemies[i].elem_shown = enemies[i].elem_unit_id = 0;
        enemies[i].elem_send = enemies[i].elem_recent = 0;
        enemies[i].elem_status = 0;
        enemies[i].ice_held = false;
    }
}
