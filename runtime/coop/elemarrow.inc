// Native GameROMPlayer chemical status (BOTW 1.9.0). The player constructor at
// 0x7000ccdc18 builds this controller inline. Starting a status can enqueue
// a chemical hit immediately, and the burn update can enqueue further ticks.
#define ELEMARROW_STATUS 0x2550
#define ELEMARROW_STATUS_VT 0x7001cd76e0ull
#define A_ELEMARROW_START 0x7000cc41f4ull
#define A_ELEMARROW_DAMAGE 0x7000b8ebd4ull
#define ELEMARROW_FIRST 2
#define ELEMARROW_ICE 3
#define ELEMARROW_COUNT 3
#define ELEMARROW_KEEP_CHEM 0x04
#define ELEMARROW_FREEZE_REACTION 0x02

static const char *const elemarrow_states[ELEMARROW_COUNT] = {"burning", "frozen", "shocked"};
static const uint8_t elemarrow_flags[ELEMARROW_COUNT] = {COOP_FLAG_LINK_FIRE, COOP_FLAG_LINK_ICE, COOP_FLAG_LINK_SHOCK};
static GuestFn elemarrow_damage_orig;
static __thread uint64_t elemarrow_starting;
static pthread_mutex_t elemarrow_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t player, receiver;
    uint32_t unit_id;
    unsigned owned;
    int remaining[ELEMARROW_COUNT];
    unsigned foe;  // (FOE_ELEM) statuses an enemy's shot / arrow started: the game's own damage, no friendly cap
    bool keep_ice;
} elemarrow;

static bool elemarrow_on(void) {
    static int on = -1;
    return env_on("SAVAGEWIND_COOP_ELEMARROW", &on);
}

// The controller sends {kind, damage, ...} to the receiver's +0x168 vfunc.
// Its native handler adds damage to +0x44 after resistance / immunity checks.
// Cap only what this call added, before the player consumes the queue. The
// same budget covers every tick of one friendly burn.
static void elemarrow_damage(Cpu *c) {
    uint64_t r = c->x[0], args = c->x[1];
    uint32_t kind = LD32(args);
    if (kind < ELEMARROW_FIRST || kind >= ELEMARROW_FIRST + ELEMARROW_COUNT) {
        elemarrow_damage_orig(c);
        return;
    }
    unsigned i = kind - ELEMARROW_FIRST;
    pthread_mutex_lock(&elemarrow_lock);
    uint64_t pa = r == elemarrow.receiver && (elemarrow.owned & (1u << i)) ? elemarrow.player : 0;
    uint32_t id = elemarrow.unit_id;
    bool owned = pa && LD64(r + 0x18) == pa && LD32(pa + PROC_ID) == id &&
                 (r == elemarrow_starting || (LD16(pa + ELEMARROW_STATUS + 8) & (1u << kind)));
    pthread_mutex_unlock(&elemarrow_lock);
    int queued = owned ? (int)LD32(r + 0x44) : 0;
    // The plain ice chemical request only takes life. The receiver's typed
    // freeze reaction (0x7000b8ecf0) selects the player action.
    bool freeze = owned && kind == ELEMARROW_ICE;
    uint32_t reaction = freeze ? LD32(args + 0xc) : 0;
    if (freeze) ST32(args + 0xc, reaction | ELEMARROW_FREEZE_REACTION);
    elemarrow_damage_orig(c);
    if (freeze) ST32(args + 0xc, reaction);
    if (!owned) return;

    int damage = (int)LD32(r + 0x44) - queued;
    if (damage <= 0) return;
    pthread_mutex_lock(&elemarrow_lock);
    if (elemarrow.foe & (1u << i)) {
        pthread_mutex_unlock(&elemarrow_lock);
        return;
    }
    if (r == elemarrow.receiver && pa == elemarrow.player && id == elemarrow.unit_id) {
        int life = (int)LD32(pa + PVP_LIFE), allowed = damage;
        int room = life > queued + 1 ? life - queued - 1 : 0;
        if (allowed > room) allowed = room;
        if (allowed > elemarrow.remaining[i]) allowed = elemarrow.remaining[i];
        if (allowed != damage) ST32(r + 0x44, (uint32_t)(queued + allowed));
        elemarrow.remaining[i] -= allowed;
        LOG("[coop] elemarrow: victim %s damage %d of %d (life %d, budget %d left)\n",
            elemarrow_states[i], allowed, damage, life, elemarrow.remaining[i]);
    }
    pthread_mutex_unlock(&elemarrow_lock);
}

static void elemarrow_hooks_init(void) {
    if (!elemarrow_on() || !dispatch_lookup(A_ELEMARROW_START)) return;
    elemarrow_damage_orig = dispatch_lookup(A_ELEMARROW_DAMAGE);
    if (elemarrow_damage_orig)
        hle_override(A_ELEMARROW_DAMAGE, elemarrow_damage, "coop elemental arrow damage cap");
}

static uint64_t elemarrow_status(uint64_t pa) {
    uint64_t status = pa + ELEMARROW_STATUS;
    return in_guest(pa) && LD64(status) == ELEMARROW_STATUS_VT && LD64(status + 0x10) == pa ? status : 0;
}

static void elemarrow_ice_release(uint64_t status) {
    if (elemarrow.keep_ice && status) ST8(status + 0xa, LD8(status + 0xa) & ~ELEMARROW_KEEP_CHEM);
    elemarrow.keep_ice = false;
}

static uint8_t elemarrow_publish(uint64_t pa) {
    uint8_t flags = 0;
    if (!elemarrow_damage_orig) return 0;
    pthread_mutex_lock(&elemarrow_lock);
    uint64_t status = pa == elemarrow.player && elemarrow.owned ? elemarrow_status(pa) : 0;
    if (!status || LD32(pa + PROC_ID) != elemarrow.unit_id) {
        elemarrow.owned = 0;
        elemarrow.keep_ice = false;
    } else {
        unsigned active = (LD16(status + 8) >> ELEMARROW_FIRST) & ((1u << ELEMARROW_COUNT) - 1);
        if (!(active & (1u << (ELEMARROW_ICE - ELEMARROW_FIRST)))) elemarrow_ice_release(status);
        for (unsigned i = 0; i < ELEMARROW_COUNT; i++) {
            if (!(elemarrow.owned & (1u << i))) continue;
            if (active & (1u << i)) flags |= elemarrow_flags[i];
            else LOG("[coop] elemarrow: victim %s ended\n", elemarrow_states[i]);
        }
        elemarrow.owned &= active;
    }
    pthread_mutex_unlock(&elemarrow_lock);
    return flags;
}

static bool elemarrow_hit_ex(Cpu *c, uint64_t pa, unsigned arrow, bool foe) {
    if (!elemarrow_damage_orig || arrow < COOP_ARROW_FIRE || arrow > COOP_ARROW_SHOCK) return false;
    uint64_t status = elemarrow_status(pa), r = LD64(pa + PVP_RECEIVER);
    if (!status || !in_guest(r)) return false;
    uint64_t vt = LD64(r);
    if (!in_guest(vt) || LD64(vt + 0x168) != A_ELEMARROW_DAMAGE || LD64(r + 0x18) != pa) return false;
    unsigned i = arrow - COOP_ARROW_FIRE, kind = i + ELEMARROW_FIRST;
    // The game owns any status already in progress, including a natural burn.
    if (LD16(status + 8) & (1u << kind)) return false;
    pthread_mutex_lock(&elemarrow_lock);
    if (elemarrow.player != pa || elemarrow.receiver != r || elemarrow.unit_id != LD32(pa + PROC_ID)) {
        elemarrow.owned = 0;
        elemarrow.foe = 0;
        elemarrow.keep_ice = false;
        elemarrow.player = pa;
        elemarrow.receiver = r;
        elemarrow.unit_id = LD32(pa + PROC_ID);
    }
    elemarrow.owned |= 1u << i;
    elemarrow.remaining[i] = PVP_DMG_MAX;
    elemarrow.foe = foe ? elemarrow.foe | (1u << i) : elemarrow.foe & ~(1u << i);
    if (kind == ELEMARROW_ICE && !(LD8(status + 0xa) & ELEMARROW_KEEP_CHEM)) {
        // 0x7000b8818c normally clears ice as soon as the body has no ice
        // contact. Keep it until the native freeze timer / breakout ends it.
        ST8(status + 0xa, LD8(status + 0xa) | ELEMARROW_KEEP_CHEM);
        elemarrow.keep_ice = true;
    }
    pthread_mutex_unlock(&elemarrow_lock);

    Cpu saved = *c;
    elemarrow_starting = r;
    c->x[0] = status;
    c->x[1] = kind;
    guest_call(c, A_ELEMARROW_START);
    *c = saved;
    elemarrow_starting = 0;
    bool active = LD16(status + 8) & (1u << kind);
    if (active) {
        LOG("[coop] elemarrow: victim %s started%s\n", elemarrow_states[i], foe ? " (an enemy's)" : "");
    } else {
        pthread_mutex_lock(&elemarrow_lock);
        elemarrow.owned &= ~(1u << i);
        if (kind == ELEMARROW_ICE) elemarrow_ice_release(status);
        pthread_mutex_unlock(&elemarrow_lock);
    }
    return active;
}

static bool elemarrow_hit(Cpu *c, uint64_t pa, unsigned arrow) { return elemarrow_hit_ex(c, pa, arrow, false); }

// (0.1.1, 2026-11-26, the HOWTO's "known holes") An enemy's fire / ice / shock shot or elemental arrow that reached us
// from the other game (shots.inc, arrows.inc) gave a plain hit only: now it also burns / freezes / shocks our Link with
// the game's own status, its damage uncapped (an enemy's, not the friendly budget). SAVAGEWIND_COOP_FOE_ELEM=0: off.
static int foe_elem_c = -1;
static bool foe_elem(Cpu *c, uint64_t pa, unsigned arrow) {
    if (!env_on("SAVAGEWIND_COOP_FOE_ELEM", &foe_elem_c) || !in_guest(pa) || (int)LD32(pa + PVP_LIFE) <= 0) return false;
    return elemarrow_hit_ex(c, pa, arrow, true);
}

RT_ON_RESET(coop_elemarrow_reset) {
    memset(&elemarrow, 0, sizeof elemarrow);
}
