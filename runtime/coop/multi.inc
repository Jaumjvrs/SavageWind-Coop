// (coop-multi, protocol 56) More than two players: one avatar per extra player.
// The partner's avatar (the world partner: coop_net.c) uses the avatar code's file-scope state as before. Every other
// player gets a saved copy of all that state (a "context"): each frame, for each such player, the context is swapped
// into the statics, coop_net_view(pid) points the remote-state calls at that player, the receive half of the avatar
// pipeline runs (as in rt_coop_game_frame), then everything is swapped back. So the 2-player path is unchanged and
// the avatar code is not rewritten. What the extra avatars do not do (phase 1): enemies, pvp, grass, map star, gates.
// SAVAGEWIND_COOP_MULTI=0: no extra avatars (the network still relays). SAVAGEWIND_COOP_MAX_AVATARS=<n>: at most n
// extra avatars drawn (default 7).

// Everything one avatar's receive path keeps between frames (coop.c avl: its function-local statics).
#define MULTI_SWAP(X) X(avatar) X(astree) X(astree_end) X(astree_charge_lv) X(nodepos_avatar) X(gear_pup) X(armor_pup) X(held_pup) \
    X(gtail) X(hurt_flash) X(outfit_done) X(armor_resync) X(glidefx_pup) X(ears) X(hr) X(rag) X(rag_pup) X(aim) \
    X(aim_pup) X(fx_avatar_map) X(rip) X(rip_water_set) X(wfx) X(step_pup) X(snd_avatar_map) X(steps) X(avl) \
    X(col) X(body_pup) X(hcol) X(hbody_pup) X(grass) X(magbeam) X(stasisfx) X(champ) X(fire_torch) \
    X(grip) X(grip_node)
#define MULTI_FIELD(v) __typeof__(v) v;
typedef struct { MULTI_SWAP(MULTI_FIELD) } MultiCtx;
#undef MULTI_FIELD

static MultiCtx multi_pristine;             // the state as at start (a new context starts from it)
static bool multi_sampled;
static MultiCtx *multi_ctx[COOP_MAX_PLAYERS];
static uint32_t multi_wanted;
static volatile int multi_in;               // (game thread) a context is swapped in now
static int multi_cur = -1;                  // its player id

static bool multi_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_MULTI");
    return on;
}
static int multi_max_avatars(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_MAX_AVATARS");
        n = e && *e ? atoi(e) : COOP_MAX_PLAYERS - 1;
        if (n < 0) n = 0;
        if (n > COOP_MAX_PLAYERS - 1) n = COOP_MAX_PLAYERS - 1;
    }
    return n;
}
static void multi_save(MultiCtx *m) {
#define MULTI_X(v) memcpy((void *)&m->v, (const void *)&v, sizeof v);
    MULTI_SWAP(MULTI_X)
#undef MULTI_X
}
static void multi_restore(const MultiCtx *m) {
#define MULTI_X(v) memcpy((void *)&v, (const void *)&m->v, sizeof v);
    MULTI_SWAP(MULTI_X)
#undef MULTI_X
}
static void multi_exchange(MultiCtx *m) {
    static MultiCtx tmp;  // (game thread only; too big for the stack with every map)
    multi_save(&tmp);
    multi_restore(m);
    *m = tmp;
}
// First game frame, before any avatar code ran: keep the initial state.
static void multi_snapshot(void) {
    if (multi_sampled) return;
    multi_save(&multi_pristine);
    multi_sampled = true;
    if (multi_on())
        LOG("[coop] multi: up to %d extra avatars (%u bytes of state each)\n", multi_max_avatars(), (unsigned)sizeof(MultiCtx));
}

static bool multi_av_alive(const MultiCtx *m);
// The NPC class's input hook (coop/astree.inc astree_npc_hook): the avatars and horse puppets of the saved contexts.
// (Their effect / sound set inputs are not written: phase 1.)
static void multi_npc_inputs(uint64_t a) {
    if (!a || multi_in) return;  // (in a swapped frame the statics are that context: the main hook path covers it)
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
        MultiCtx *m = multi_ctx[p];
        if (!m) continue;
        if (m->astree.have)
            for (int i = 0; i < ASTREE_TARGETS; i++)
                if (m->astree.targets[i] == a && multi_av_alive(m) &&
                    (!i || (m->armor_pup[i - 1].actor == a && pup_alive(&m->armor_pup[i - 1])))) {
                    astree_write(a, &m->astree.rx);
                    return;
                }
        if (m->hr.have)
            for (int k = 0; k < HORSE_PARTS; k++)
                if (m->hr.targets[k] == a && horse_target_alive(&m->hr.p[k], a)) {
                    horse_write_inputs(a, &m->hr.rx);
                    return;
                }
    }
}

// (n4, 2026-11-17) The extra players' bodies: each extra avatar also gets the partner's collision body (objects.inc
// collide_frame: our Link and loose objects bump into it), a solid horse (horse_body_frame) and cuts grass with its
// sword (grass.inc); the physics-object claim check (col_near_avatar) looks at every body. SAVAGEWIND_COOP_MULTIBODY=0:
// extra avatars have no body (as before n4).
static int multibody_c = -1;
static bool multibody_on(void) { return env_on("SAVAGEWIND_COOP_MULTIBODY", &multibody_c); }

// The receive half of the avatar pipeline (rt_coop_game_frame), for the player coop_net_view points at.
static void multi_run(Cpu *c) {
    avatar_frame(c);
    horse_frame(c);
    aim_frame(c);
    rag_recv(c);
    CoopState rs;
    double rage;
    bool have = coop_net_get_remote(&rs, &rage);
    armor_frame(c, &rs, have);
    bool weapon_placed = gear_frame(c, &rs, have);
    grip_frame(c, &rs, have);  // (2026-11-21, HOLDWRIST for extra players: their wrist follows their two-handed weapon)
    fire_torch_frame(c, &rs, have && rage < 1.0, weapon_placed);
    glidefx_frame(c, &rs, have);
    magbeam_frame(c, &rs, have && rage < 0.5);
    stasisfx_frame(c, &rs, have && rage < 0.5);
    champ_frame(c);
    hurt_flash_frame(c, &rs, have);
    ears_frame(c, &rs, have);
    doors_frame(c, have ? avatar.actor : 0);  // (2026-11-21) doors the other player opens
    avatarsnd_frame(c, &rs, have);
    if (multibody_on()) {  // (n4) its body, its horse's body, its sword in our grass
        collide_frame(c);
        horse_body_frame(c);
        grass_frame(c);
    }
}

// (n4) Is an extra player's avatar body right there (objects.inc col_near_avatar, which checks the partner's)?
static bool multi_col_near(const float p[3], float r) {
    if (!multibody_on() || multi_in) return false;
    for (int k = 0; k < COOP_MAX_PLAYERS; k++) {
        MultiCtx *m = multi_ctx[k];
        if (m && col_body_near(m->col.on, m->col.actor, p, r)) return true;
    }
    return false;
}

// (n4) rav: every other player's avatar drawn here (the partner's first) and that player's newest state, once a frame
// after multi_frame (arrows.inc, enemies.inc use it).
static bool multi_av_alive(const MultiCtx *m) {
    return m->avatar.frames >= 6 && av_ptr_alive(m->avatar.actor, m->avatar.unit, m->avatar.unit_id);
}
static void rav_update(void) {
    rav_n = 0;
    double age = 99;
    if (avatar.actor && avatar_alive() && avatar.frames >= 6) {
        struct RemoteAv *r = &rav[rav_n++];
        r->actor = avatar.actor, r->pid = coop_net_partner_id(), r->partner = true;
        r->fresh = coop_net_get_remote(&r->rs, &age) && age < 1.0;
    }
    if (!multi_on() || multi_in) return;
    for (int p = 0; p < COOP_MAX_PLAYERS && rav_n < COOP_MAX_PLAYERS; p++) {
        MultiCtx *m = multi_ctx[p];
        if (!m || p == coop_net_partner_id() || !multi_av_alive(m)) continue;
        struct RemoteAv *r = &rav[rav_n++];
        r->actor = m->avatar.actor, r->pid = p, r->partner = false;
        coop_net_view(p);
        r->fresh = coop_net_get_remote(&r->rs, &age) && age < 1.0;
        coop_net_view(-1);
    }
}

// (n4, memory cap) Which extra players get an avatar drawn now: those heard, in our area and within
// MULTI_DRAW_M (kept up to MULTI_KEEP_M once drawn), nearest first, at most SAVAGEWIND_COOP_MAX_AVATARS. A far
// player's avatar (and its gear, horse, bodies) is deleted the usual way and comes back when it is near again, so
// many players spread over the map cost only the ones nearby. SAVAGEWIND_COOP_MULTIFAR=0: every heard player, lowest
// id first (as before n4). The partner's avatar is not affected.
#define MULTI_DRAW_M 250.0f
#define MULTI_KEEP_M 300.0f
static int multifar_c = -1;
static bool multifar_on(void) { return env_on("SAVAGEWIND_COOP_MULTIFAR", &multifar_c); }
static uint32_t multi_pick(int me, int partner, uint32_t live, int max) {
    float d[COOP_MAX_PLAYERS];
    uint32_t cand = 0, out = 0;
    float here[3];
    char area[96];
    drop_me(here);
    uint32_t my_area = local_area(area, sizeof area);
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
        if (p == me || p == partner || !(live >> p & 1)) continue;
        d[p] = 0;
        if (multifar_on()) {
            CoopState rs;
            double age = 99;
            coop_net_view(p);
            bool have = coop_net_get_remote(&rs, &age);
            coop_net_view(-1);
            if (!have || rs.area != my_area) continue;
            float dx = rs.pos[0] - here[0], dy = rs.pos[1] - here[1], dz = rs.pos[2] - here[2];
            d[p] = sqrtf(dx * dx + dy * dy + dz * dz);
            bool drawn = multi_ctx[p] && multi_ctx[p]->avatar.actor;
            if (!(d[p] < (drawn ? MULTI_KEEP_M : MULTI_DRAW_M))) continue;
        } else {
            d[p] = (float)p;  // (lowest id first)
        }
        cand |= 1u << p;
    }
    for (int n = 0; n < max && cand; n++) {  // nearest first
        int best = -1;
        for (int p = 0; p < COOP_MAX_PLAYERS; p++)
            if (cand >> p & 1 && (best < 0 || d[p] < d[best])) best = p;
        out |= 1u << best, cand &= ~(1u << best);
    }
    return out;
}

// (n4) A status line for the co-op window (coop_window.ps1 reads the newest one from the log): who is connected.
static void players_status_frame(void) {
    static uint32_t last = 0xffffffffu;
    int me = coop_net_my_id();
    if (me < 0) return;  // (a guest before the host answered)
    uint32_t live = coop_net_players(5.0) | (me >= 0 && me < COOP_MAX_PLAYERS ? 1u << me : 0);
    if (live == last) return;
    last = live;
    int n = 0;
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) n += live >> p & 1;
    LOG("[coop] status: %d players connected (mask %02x, me %d)\n", n, live, me);
}

static void multi_frame(Cpu *c) {
    players_status_frame();
    if (!multi_on()) return;
    int me = coop_net_my_id(), partner = coop_net_partner_id();
    uint32_t live = coop_net_players(5.0);
    uint32_t pick = multi_pick(me, partner, live, multi_max_avatars());
    if (pick != multi_wanted) LOG("[coop] multi: extra avatars drawn: %02x (heard %02x)\n", pick, live);
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
        bool want = pick >> p & 1;
        if (!want && !(multi_wanted >> p & 1)) continue;
        if (!multi_ctx[p]) {
            if (!want) continue;
            multi_ctx[p] = malloc(sizeof *multi_ctx[p]);
            if (!multi_ctx[p]) continue;
            *multi_ctx[p] = multi_pristine;
            LOG("[coop] multi: player %d gets an avatar of its own\n", p);
        }
        multi_in = 1, multi_cur = p;
        multi_exchange(multi_ctx[p]);
        // Drain the avatar and its puppets once before parking the slot. Keep only reusable effect-handle buffers
        // (hle_alloc cannot free them). A new avatar must not inherit pointers or animation choices from this one.
        coop_net_view(want ? p : COOP_VIEW_NONE);
        multi_run(c);
        coop_net_view(-1);
        multi_exchange(multi_ctx[p]);
        if (!want) {
            MultiCtx *m = multi_ctx[p];
            uint64_t charge = m->avl.cf_h, ice = m->avl.ice_h, fire = m->avl.fire_h, ripple = m->rip.h, beam = m->magbeam.handle;
            uint64_t stasis = m->stasisfx.handles, ears_mem = m->ears.mem, champ_mem = m->champ.mem;
            uint64_t torch = m->fire_torch.h;
            uint64_t weapon[2] = {m->wfx.h[0], m->wfx.h[1]};
            *m = multi_pristine;
            m->avl.cf_h = charge, m->avl.ice_h = ice, m->avl.fire_h = fire, m->rip.h = ripple;
            m->magbeam.handle = beam;
            m->stasisfx.handles = stasis;
            m->ears.mem = ears_mem;
            m->champ.mem = champ_mem;
            m->fire_torch.h = torch;
            m->wfx.h[0] = weapon[0], m->wfx.h[1] = weapon[1];
            LOG("[coop] multi: cleared dormant avatar state for player %d%s\n", p, p == partner ? " (now partner)" : "");
        }
        multi_in = 0, multi_cur = -1;
    }
    multi_wanted = pick;
}

// `coop players` (dev): ids, the partner, who is heard, each extra avatar
static void multi_report(ControlReply *r) {
    int me = coop_net_my_id(), partner = coop_net_partner_id();
    uint32_t live = coop_net_players(5.0);
    control_field_int(r, "my_id", me);
    control_field_int(r, "partner", partner);
    control_field_int(r, "heard_mask", live);
    control_field_bool(r, "multi", multi_on());
    char buf[256] = "";
    int n = 0;
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
        if (p == me || !(live >> p & 1)) continue;
        uint64_t a = p == partner ? avatar.actor : multi_ctx[p] ? multi_ctx[p]->avatar.actor : 0;
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%d:%s:%llx", n ? " " : "", p, p == partner ? "partner" : "extra",
                      (unsigned long long)a);
        if (n >= (int)sizeof buf) break;
    }
    control_field_str(r, "avatars", buf);
    LOG("[coop] players: me %d, partner %d, heard %02x, avatars %s\n", me, partner, live, buf);
}
