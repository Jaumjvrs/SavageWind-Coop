// Part of runtime/coop.c (included there, same translation unit): per-frame work of the shared enemies (uses the world and gate code above).
// The bool flags that turned on since the last look: one of ours opens an acquisition.
static void world_flags(uint64_t now, const float *me) {
    uint64_t mgr = LD64(A_GDT_MGR), tp = in_guest(mgr) ? LD64(mgr + 0xc00) : 0;
    if (!in_guest(tp)) return;
    uint32_t count = LD32(tp + 0x08);
    uint64_t ptrs = LD64(tp + 0x10);
    if (!count || count > FLAG_MAX || !in_guest(ptrs)) return;
    bool baseline = (int)count != flag_n;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t f = LD64(ptrs + 8ull * i);
        uint8_t v = in_guest(f) ? (LD8(f + 0xb) & 1) : 0;
        if (!baseline && v && !flag_val[i]) {
            uint32_t crc = LD32(f + 0xc);
            bool ours = true;
            for (int k = 0; k < 64; k++)
                if (applied[k].crc == crc && now - applied[k].at_ns < 30000000000ull) ours = false;
            const struct Named *nm = NULL;
            for (int k = 0; k < named_n && !nm; k++)
                if (named[k].crc == crc) nm = &named[k];
            // (0.1.1 diagnosis: in a split-screen playtest player 2 opened a shrine and never told player 1)
            if (!ours || !nm) {
                char fname[24];
                for (int d = 0; d < 140; d++) {
                    snprintf(fname, sizeof fname, "Open_Dungeon%03d", d);
                    if (crc_of(fname) != crc) continue;
                    LOG("[coop] shrine: %s turned on here but not sent (ours %d, named %d of %d)\n", fname, ours,
                        nm != NULL, named_n);
                    break;
                }
            }
            if (ours && nm && crc == chest_early_sent_crc && now - chest_early_sent_ns < 300000000000ull) {
                LOG("[coop] share: %s turned on here (already shared when its item reached the pouch)\n", nm->name);
                chest_early_sent_crc = 0;
            } else if (ours && nm && !strncmp(nm->name, "Open_Dungeon", 12)) {
                shrine_door_send(nm->name, me);
            } else if (ours && nm) {
                acq_send();  // (one still open goes first)
                memset(&acq, 0, sizeof acq);
                acq.open = true, acq.opened_ns = now;
                memcpy(acq.w.pos, me, 12);
                acq.w.hashid = nm->hashid;
                char area[96];
                acq.w.area = local_area(area, sizeof area);
                snprintf(acq.w.flag, sizeof acq.w.flag, "%s", nm->name);
                LOG("[coop] share: %s turned on here\n", nm->name);
            }
        }
        flag_val[i] = v;
    }
    flag_n = (int)count;
}

#define ENEMY_HURT_FIRST_NS 5000000000ull  // enemies our player hurt this recently go first in the packet
#define ENEMY_KEEP_SLOTS 3                 // packet slots kept for unloaded enemies we hurt (their totals)

// One packet entry: our damage total; for a loaded one the LOADED flag (and the host's hand-off decision); for one
// we run, where its body is and its AS (the other side's copy follows it).
static void enemy_entry(Cpu *c, struct Enemy *e, CoopEnemyDamage *o) {
    *o = (CoopEnemyDamage){.hash = e->hash, .damage = e->own};
    if (e->spawned && e->native) o->flags = COOP_ENEMY_SPAWNED;  // (also once gone: the peer removes its copy)
    // (58, host only, never on the wire) our copy of a guest's run-time enemy: whose it is; who runs it (below)
    int native_pid = e->spawned && !e->native ? coop_net_id_origin(e->hash) : -1;
    if (coop_net_is_host() && native_pid > 0) o->flags |= COOP_ENEMY_COPY | ((uint32_t)native_pid << COOP_ENEMY_OWNER_SHIFT);
    if (!e->actor) return;
    o->flags |= COOP_ENEMY_LOADED;
    o->flags |= e->elem_send;
    if (coop_net_is_host() && e->guest_runs) {
        o->flags |= COOP_ENEMY_GUEST_RUNS;
        if (native_pid <= 0 && e->owner_pid > 0) o->flags |= (uint32_t)(e->owner_pid & 7) << COOP_ENEMY_OWNER_SHIFT;
    }
    if (e->mine) {
        uint64_t body = enemy_body(e->actor);
        if (body) {
            o->flags |= COOP_ENEMY_POS | enemy_look_flags(e->actor);
            memcpy(o->pos, (void *)(uintptr_t)(body + 0x1a0), 12);
            memcpy(o->rot, (void *)(uintptr_t)(body + 0x1d0), 16);
        } else if (e->animal && mtxpos_on()) {  // (MTXPOS: a small animal with no body found - its matrix)
            const float *m = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
            o->flags |= COOP_ENEMY_POS | COOP_ENEMY_MTXPOS;
            o->pos[0] = m[3], o->pos[1] = m[7], o->pos[2] = m[11];
            mtx_quat(m, o->rot);
        }
        player_as_name(c, e->actor, o->as, sizeof o->as);
    }
}

// Copies of the peer's run-time enemies (twice a second): create one for each the peer reports loaded with a fresh
// position near our player; remove a copy the peer stopped reporting (gone or unloaded there) or that is far away.
// Also: which of our own run-time enemies' names to send (in turns, COOP_SPAWN_NAMES per packet).
#define SPAWN_NEAR 100.0f
#define SPAWN_DEDUP_R 30.0f                 // (SPAWNDEDUP) same kind this close = the same random spawn
#define SPAWN_DEDUP_GRACE 5000000000ull     // host: wait this long before copying a guest spawn with a twin here
#define KEESE_HOST_R 60.0f                  // (KEESEHOST) guest: the host this close spawns the Keese for both
#define SPAWN_DEDUP_SETTLE 3000000000ull   // host: ... and this long before copying any guest spawn
// (58) Is the player whose run-time enemy `id` is (the id names it; the partner when it names nobody) in our area?
static bool spawn_native_here(uint32_t id) {
    int pid = coop_net_id_origin(id);
    bool other = pid >= 0 && pid != coop_net_partner_id() && pid != coop_net_my_id();
    if (other) coop_net_view(pid);
    CoopState rs;
    double rage = 99;
    bool here = coop_net_get_remote(&rs, &rage) && rage < 1.0 && (rs.flags & COOP_FLAG_IN_WORLD) && rs.area == enemy_area;
    coop_net_view(-1);
    return here;
}

// 1.9.0 AutoPlacementMgr keeps the actor at the start of each of its 300
// lifetime records. A zero map hash alone also matches scripted spawns.
#define WILDLIFE_POOL_SIZE 300
static uint64_t wildlife_pool(void) { return LD64(0x7001e0c680ull); }
static uint64_t wildlife_slot(uint64_t mgr, int i) { return LD64(mgr + 0x58 + i * 0x38); }
static bool wildlife_ecosystem_actor(uint64_t actor) {
    uint64_t mgr = wildlife_pool();
    if (!in_guest(mgr)) return false;
    for (int i = 0; i < WILDLIFE_POOL_SIZE; i++)
        if (wildlife_slot(mgr, i) == actor) return true;
    return false;
}

static bool wildlife_host_ready(void);
static void wildlife_guest_population(Cpu *c, const float me[3], const float host[3]) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_WILDLIFE");
    if (!on || spawn_hold() || !wildlife_host_ready()) return;
    // Leave a margin inside the 100 m range where host followers are created.
    const float radius = 85.0f;
    float dx = me[0] - host[0], dy = me[1] - host[1], dz = me[2] - host[2];
    if (dx * dx + dy * dy + dz * dz > radius * radius) return;
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (!e->spawned || !e->native || !e->animal || !e->actor || e->own ||
            (strncmp(e->name, "Animal_Fox_", 11) && strncmp(e->name, "Animal_Deer_", 12) &&
             strncmp(e->name, "Animal_Doe_", 11) && strncmp(e->name, "Animal_Boar_", 12))) continue;
        float p[3];
        if (!enemy_alive(e) || (int32_t)LD32(e->actor + ENEMY_LIFE) < e->base) continue;
        float local_dist = actor_dist(e->actor, me, p);
        // A deer at Link's feet can be the one he is mounting or riding.
        if (local_dist < 5.0f && (!strncmp(e->name, "Animal_Deer_", 12) || !strncmp(e->name, "Animal_Doe_", 11)))
            continue;
        if (actor_dist(e->actor, host, p) > radius || local_dist > radius ||
            !wildlife_ecosystem_actor(e->actor) || coop_net_remote_damage(e->hash)) continue;
        if (despawn_now(c, e->actor, ACTOR_DELETE_REASON)) {
            LOG("[coop] wildlife: %s %08x at %.1f %.1f %.1f removed in host radius\n",
                e->name, e->hash, p[0], p[1], p[2]);
            e->actor = 0;
        }
    }
}
static void spawned_frame(Cpu *c, uint64_t now) {
    static uint64_t next_ns;
    static int name_turn;
    if (!spawned_on() || now < next_ns || !local_in_world()) return;  // (never create copies during a load)
    next_ns = now + 500000000ull;
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    CoopState rs;
    double rage = 99;
    bool peer_here = coop_net_get_remote(&rs, &rage) && rage < 1.0 && (rs.flags & COOP_FLAG_IN_WORLD) &&
                     rs.area == enemy_area;
    if (peer_here && !coop_net_is_host() && coop_net_partner_id() == 0)
        wildlife_guest_population(c, me, rs.pos);
    CoopSpawnName names[COOP_SPAWN_NAMES];
    int nn = 0, natives = 0;
    for (int i = 0; i < enemy_count; i++)
        if (enemies[i].spawned && enemies[i].native && enemies[i].actor) natives++;
    for (int i = 0, j = 0; i < enemy_count && nn < COOP_SPAWN_NAMES; i++) {
        const struct Enemy *e = &enemies[i];
        if (!(e->spawned && e->native && e->actor)) continue;
        int turn = j++;
        if (natives > COOP_SPAWN_NAMES && (turn - name_turn + natives) % natives >= COOP_SPAWN_NAMES) continue;
        names[nn] = (CoopSpawnName){.id = e->hash};
        snprintf(names[nn].name, sizeof names[nn].name, "%s", e->name);
        nn++;
    }
    name_turn = natives ? (name_turn + COOP_SPAWN_NAMES) % natives : 0;
    coop_net_set_spawn_names(names, nn);

    static CoopEnemyDamage rem[64];
    static double heard[64];
    int n = coop_net_remote_spawned(rem, heard, 64);
    // (SPAWNDEDUP, see spawndedup_on) Guest: each run-time enemy of the host's replaces at most one of our own random
    // spawns of the same kind within SPAWN_DEDUP_R of it that neither player has hurt; ours is deleted (its entry then
    // goes out without LOADED: the host removes its copy) and the host's copy is created below as usual.
    static struct { uint32_t id; uint64_t ns; } twin[128];  // guest: host ids that replaced one of ours; host: first heard
    static int twin_n, twin_w;
    bool dedup = spawndedup_on() && peer_here;
    if (dedup && !coop_net_is_host()) {
        for (int i = 0; i < n; i++) {
            const CoopEnemyDamage *r = &rem[i];
            if (heard[i] >= 3.0 || !(r->flags & COOP_ENEMY_LOADED)) continue;
            bool used = false;
            for (int t = 0; t < twin_n && !used; t++) used = twin[t].id == r->hash;
            if (used) continue;
            float p[3], rot[4];
            double age = 99;
            char name[COOP_SPAWN_NAME_LEN];
            if (!coop_net_remote_enemy_pos(r->hash, p, rot, &age, NULL, 0) || age >= ENEMY_FOLLOW_MAX_AGE ||
                !coop_net_remote_spawn_name(r->hash, name, sizeof name))
                continue;
            int best = -1;
            float bd = SPAWN_DEDUP_R;
            for (int k = 0; k < enemy_count; k++) {
                struct Enemy *o = &enemies[k];
                if (!(o->spawned && o->native && o->actor) || strcmp(o->name, name) || o->own ||
                    coop_net_remote_damage(o->hash) || !enemy_alive(o))
                    continue;
                float q[3], d = actor_dist(o->actor, p, q);
                if (d < bd) bd = d, best = k;
            }
            if (best < 0) continue;
            struct Enemy *o = &enemies[best];
            LOG("[coop] run-time enemy %s %08x: ours, %.1f m from the host's %08x, removed (the host's is shown)\n",
                o->name, o->hash, bd, r->hash);
            despawn_now(c, o->actor, ACTOR_DELETE_REASON);
            o->actor = 0, o->following = false;
            twin[twin_w++ % 128] = (typeof(twin[0])){r->hash, now};  // (ring: the oldest is overwritten)
            if (twin_n < 128) twin_n++;
        }
    }
    // (2026-11-21, KEESEHOST; 11-20 play: "the bats are different for each of us") Keese swarms are spawned at run time
    // by each game's own spawner around its own player, at other spots than the host's (> SPAWN_DEDUP_R), so the twin
    // rule above rarely matched and each game showed its own swarm plus the other's. Guest, with the host within
    // KEESE_HOST_R: our own untouched run-time Keese are removed at once (before the host's 3 s wait copies them), and
    // only the host's swarms (copied below) fly for both. SAVAGEWIND_COOP_KEESEHOST=0: off.
    static int keese_c = -1;
    if (peer_here && !coop_net_is_host() && env_on("SAVAGEWIND_COOP_KEESEHOST", &keese_c)) {
        float dx = rs.pos[0] - me[0], dy = rs.pos[1] - me[1], dz = rs.pos[2] - me[2];
        if (dx * dx + dy * dy + dz * dz < KEESE_HOST_R * KEESE_HOST_R)
            for (int k = 0; k < enemy_count; k++) {
                struct Enemy *o = &enemies[k];
                if (!(o->spawned && o->native && o->actor) || strncmp(o->name, "Enemy_Keese", 11) || o->own ||
                    coop_net_remote_damage(o->hash) || !enemy_alive(o))
                    continue;
                LOG("[coop] run-time enemy %s %08x: ours, the host is near: removed (the host's swarms are shown)\n",
                    o->name, o->hash);
                despawn_now(c, o->actor, ACTOR_DELETE_REASON);
                o->actor = 0, o->following = false;
            }
    }
    // Our copies whose enemy the peer no longer reports loaded (or at all for 3 s): remove them.
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (!e->spawned || e->native || !e->actor) continue;
        uint32_t fl = 0;
        double fage = 99;
        bool there = spawn_native_here(e->hash) && coop_net_remote_enemy_flags(e->hash, &fl, &fage) && fage < 3.0 &&
                     (fl & COOP_ENEMY_LOADED);
        float q[3];
        int32_t life = (int32_t)LD32(e->actor + ENEMY_LIFE);
        if (life > 0 && (!there || actor_dist(e->actor, me, q) > SPAWN_NEAR + 40.0f)) {
            LOG("[coop] run-time enemy %s %08x: copy removed (%s)\n", e->name, e->hash,
                there ? "far away" : "the other side no longer has it");
            if (e->following) enemy_ai(e, true);
            despawn_now(c, e->actor, ACTOR_DELETE_REASON);
            e->actor = 0, e->following = false;
            e->retry_ns = now + 2000000000ull;
        }
    }
    for (int i = 0; i < n; i++) {
        const CoopEnemyDamage *r = &rem[i];
        int k = 0;
        while (k < enemy_count && enemies[k].hash != r->hash) k++;
        struct Enemy *e = k < enemy_count ? &enemies[k] : NULL;
        if (e && e->native) continue;  // (cannot happen: ids carry the side that made them)
        float p[3] = {0, 0, 0}, rot[4];
        double age = 99;
        bool fresh = coop_net_remote_enemy_pos(r->hash, p, rot, &age, NULL, 0) && age < ENEMY_FOLLOW_MAX_AGE;
        bool there = heard[i] < 3.0 && (r->flags & COOP_ENEMY_LOADED) && spawn_native_here(r->hash);
        float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
        bool near = fresh && dx * dx + dy * dy + dz * dz < SPAWN_NEAR * SPAWN_NEAR;
        if (e && e->actor) continue;  // (our copy exists: kept or removed above)
        if (!there || !near || (e && (e->dead || now < e->retry_ns)) || spawn_hold()) continue;
        char name[COOP_SPAWN_NAME_LEN];
        if (!coop_net_remote_spawn_name(r->hash, name, sizeof name)) continue;  // (comes with a later packet)
        if (strncmp(name, "Enemy_", 6) && !(spawnanimals_on() && animal_name(name))) continue;  // (no birds/fish here)
        if (dedup && coop_net_is_host()) {  // (SPAWNDEDUP) a guest spawn with a twin of ours: give the guest time
            int t = 0;
            while (t < twin_n && twin[t].id != r->hash) t++;
            if (t == twin_n) {
                t = twin_w++ % 128;
                twin[t] = (typeof(twin[0])){r->hash, now};
                if (twin_n < 128) twin_n++;
            }
            bool has_twin = false;
            for (int k2 = 0; k2 < enemy_count && !has_twin; k2++) {
                const struct Enemy *o = &enemies[k2];
                float q[3];
                has_twin = o->spawned && o->native && o->actor && !strcmp(o->name, name) &&
                           actor_dist(o->actor, p, q) < SPAWN_DEDUP_R;
            }
            // (probe run 2: the guest's spawn arrived before our own twin was attached - our scan runs every 2 s -
            // so it was copied at once and removed 2 s later: every guest spawn waits SPAWN_DEDUP_SETTLE first)
            if (now - twin[t].ns < SPAWN_DEDUP_SETTLE) continue;
            if (has_twin && now - twin[t].ns < SPAWN_DEDUP_GRACE) continue;
            if (has_twin && now - twin[t].ns < SPAWN_DEDUP_GRACE + 600000000ull)
                LOG("[coop] run-time enemy %s %08x: the guest kept its own next to ours: copied\n", name, r->hash);
        }
        uint32_t pfl = 0;
        double pfage = 99;
        coop_net_remote_enemy_flags(r->hash, &pfl, &pfage);
        // (the position is the body's, ~1 m above the feet; with MTXPOS it is the feet)
        float at[3] = {p[0], p[1] - ((pfl & COOP_ENEMY_MTXPOS) ? 0.0f : 1.0f), p[2]};
        uint64_t a = spawn_now(c, name, at);
        if (!e) {
            k = enemy_slot(r->hash, name);
            if (k < 0) {
                if (a) despawn_now(c, a, ACTOR_DELETE_REASON);
                continue;
            }
            e = &enemies[k];
            e->spawned = true;
        }
        if (!a) {
            e->retry_ns = now + 5000000000ull;
            LOG("[coop] run-time enemy %s %08x: the game did not create a copy\n", name, r->hash);
            continue;
        }
        e->actor = a, e->unit = LD64(a + PROC_UNIT), e->unit_id = LD32(a + PROC_ID);
        e->following = false, e->seen_ns = now, e->playing[0] = 0, e->born_ns = now;
        e->hp_pct = 0;  // (ENEMYHP) a new actor
        int32_t life = (int32_t)LD32(a + ENEMY_LIFE);
        if (life > e->base && life <= 100000) e->base = life;
        e->last = life;
        LOG("[coop] run-time enemy %s %08x: copy created at %.1f %.1f %.1f (life %d, their damage %u)\n", name,
            r->hash, at[0], at[1], at[2], life, coop_net_remote_damage(r->hash));
    }
}

static void enemies_frame(Cpu *c) {
    // (2026-11-20, coop/npc.inc) townspeople: owners and followers first; our entries go into this packet
    CoopEnemyDamage npc_out[NPC_SEND_MAX];
    int npc_n = npc_frame(c, plat_now_ns(), npc_out, NPC_SEND_MAX);
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    // Only a load or an area change drops the pointers (the totals stay). Not the avatar's settle timer: that once
    // made every enemy detach and re-attach every few seconds, and a hit in that gap would not have been counted.
    // A deleted enemy is caught by enemy_alive (the game's handle test) anyway.
    bool placed = last_local.placed_since_ns && plat_now_ns() - last_local.placed_since_ns > 3000000000ull;
    if (!placed || here != enemy_area) {
        for (int i = 0; i < enemy_count; i++) {
            if (enemies[i].ice_held && enemies[i].actor && enemy_alive(&enemies[i]))
                elemfoe_ice_look(c, &enemies[i], false, enemies[i].elem_native & COOP_ENEMY_FROZEN);
            if (enemies[i].following && enemies[i].actor && enemy_alive(&enemies[i])) enemy_ai(&enemies[i], true);
            enemies[i].following = false;
            enemies[i].actor = 0;
            if (elemfoe_on()) elemfoe_frame(c, &enemies[i], 0);
        }
        for (int k = 0; k < FOLLOW_MAX; k++) follow_actor[k] = 0;  // (stale pointers: their memory gets reused)
        enemy_area = here;
        if (!placed) return;
    }
    uint64_t now = plat_now_ns();
    if (now >= enemy_scan_ns) {
        enemy_scan_ns = now + ENEMY_SCAN_NS;
        walk_actors(c, enemy_name, enemy_seen, NULL);
    }
    spawned_frame(c, now);
    bool elem = elemfoe_on() && local_in_world();
    unsigned elem_dev = elem ? elemfoe_dev.kind : 0;
    uint32_t elem_hash = elemfoe_dev.hash;
    if (elem_dev) elemfoe_dev.kind = 0;
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (e->actor && !enemy_alive(e)) {
            LOG("[coop] enemy %s %u gone (own damage %u)\n", e->name, e->hash, e->own);
            if (e->spawned && !e->native && e->last <= 0) e->dead = true;
            // (probe w2b: a copy of a far squirrel was deleted by the game itself ~1 s after each creation and made
            // again at once, in a loop: a copy the game removes quickly waits 30 s before the next try, else 5 s)
            if (e->spawned && !e->native)
                e->retry_ns = now + (now - e->born_ns < 10000000000ull ? 30000000000ull : 5000000000ull);
            e->actor = 0;
            e->following = false;
        }
        if (e->actor) {
            int32_t life = (int32_t)LD32(e->actor + ENEMY_LIFE);
            // A just-created enemy can still read life 1 until the game sets its real life: the full life is the
            // highest seen, so a late initialisation only raises it (it once cut a fresh Bokoblin from 13 to 1).
            if (life > e->base && life <= 100000) {
                if (e->base) LOG("[coop] enemy %s %u full life %d -> %d\n", e->name, e->hash, e->base, life);
                e->base = life;
                e->last = life;
                e->raw = life, e->hp_pct = 0;  // (ENEMYHP) the game's own full life: raise it again below
            }
            // (ENEMYHP) more life with more players (enemies.inc enemy_hp_scale); not while a new copy settles
            if (!(e->spawned && !e->native && now - e->born_ns < 1500000000ull)) enemy_hp_scale(e, &life);
            // (0.1.1, split-screen playtest 2026-11-26: enemies dead on one screen were still alive on the
            // other.) A Stalkoblin falls apart at life 0 and gets up again with its full life while its head lives;
            // the damage totals never go down, so every time it got up the partner's old damage was written again
            // (P2's log: "hit by the other player: life 1 -> 0" ~20 times) and it stayed in pieces there. Back from
            // 0 to life: a new life, both totals count from now. SAVAGEWIND_COOP_REVIVE=0: off.
            static int revive_c = -1;
            if (e->last == 0 && life > 0 && e->base > 0 && env_on("SAVAGEWIND_COOP_REVIVE", &revive_c)) {
                e->own_base = e->own, e->remote_base = coop_net_remote_damage(e->hash);
                e->last = life;
                LOG("[coop] enemy %s %u got up again: life %d (a new life: earlier damage no longer counts)\n",
                    e->name, e->hash, life);
            }
            if (life < e->last) {
                e->own += (uint32_t)(e->last - life);
                e->hurt_ns = now;
                LOG("[coop] enemy %s %u hit here: life %d -> %d (own damage %u)\n", e->name, e->hash, e->last, life,
                    e->own);
            }
            e->last = life;
            // A copy we just created reads life 1 until the game sets its real life: with the peer's damage applied
            // to that it died at once (seen: "life 1 -> 0", then "full life 1 -> 13"). No life writes for 1.5 s
            // (it still follows).
            bool settling = e->spawned && !e->native && now - e->born_ns < 1500000000ull;
            uint32_t remote = coop_net_remote_damage(e->hash);
            if (e->remote_known && remote > e->applied_remote) e->peer_hit_ns = now;  // (aggro: the peer hit it)
            e->remote_known = true;
            // Who runs it, decided before the life below (aggro, 2026-11-14): when the guest's hit hands the enemy to
            // the guest this frame, the host writes the life instead of injecting a hit (the guest's game plays the
            // reaction, enemy_kick), so the hit is not shown twice.
            if (e->spawned) e->mine = e->native;  // (the native side always runs a run-time enemy)
            else enemy_owner(c, e, now);
            int64_t target = (int64_t)e->base - (e->own - e->own_base) - (remote - (remote >= e->remote_base ? e->remote_base : remote));
            if (target < 0) target = 0;
            // (0.1.1, STALBODY) A Stal body's life is not its fate: it falls apart at 0 and gets up while its head
            // lives, and dies for good when the HEAD dies (playtest: a spin attack killed both heads, then both
            // bodies went 1 -> 0). The heads are shared enemies of their own, so the partner's body damage is not
            // copied: writing it re-killed a body whose head still lived here, and REVIVE alone then got up a body
            // the partner had really killed. Each game's body follows its own head. SAVAGEWIND_COOP_STALBODY=0: off.
            static int stalbody_c = -1;
            bool stal_body = strstr(e->name, "_Bone") && !strstr(e->name, "_Head") &&
                             env_on("SAVAGEWIND_COOP_STALBODY", &stalbody_c);
            if (settling || stal_body) {
                if (stal_body) e->inject_ns = 0;
            } else if (life > target) {
                // The host's enemy is the real one: the other player's new damage arrives as a received hit
                // (enemy_inject), so the game applies it itself and the enemy reacts (flinch, wakes up, death the
                // game's way). The drop to `target` is then ours to expect, not our player's damage. If the game
                // has not applied it after 0.3 s (a state that ignores hits), the life is written directly.
                bool host = e->mine;  // (the side running the enemy's AI: its copy is the real one)
                if (host && remote > e->applied_remote && flinch_on()) {
                    enemy_inject(e->actor, (int)(life - target), 15);
                    e->inject_ns = now;
                    e->last = (int32_t)target;
                    LOG("[coop] enemy %s %u hit by the other player: life %d -> %lld as a hit (their damage %u)\n",
                        e->name, e->hash, life, (long long)target, remote);
                } else if (host && e->inject_ns && now - e->inject_ns < 300000000ull) {
                    e->last = (int32_t)target;  // (waiting for the game to apply the hit)
                } else {
                    ST32(e->actor + ENEMY_LIFE, (uint32_t)target);
                    e->last = (int32_t)target;
                    LOG("[coop] enemy %s %u hit by the other player: life %d -> %lld (their damage %u)%s\n", e->name,
                        e->hash, life, (long long)target, remote, e->inject_ns ? " (the hit was ignored)" : "");
                    e->inject_ns = 0;
                }
            } else {
                e->inject_ns = 0;
            }
            e->applied_remote = remote;
            if (e->last > 0) stal_rise(c, e, now);  // (STALRISE: a buried Stal body here, the other side's up)
            if (!e->mine) {
                enemy_follow(c, e, e->last);
                if (e->following) enemy_look_follow(c, e);  // (whom its head looks at; also with no body found)
            } else if (e->following) {  // it became ours: our AI takes over where the other side had it
                // (softlock, 2026-11-14: WAKE_KICK) Taken over mid-fight - the copy showed the other side's battle
                // AS, or a player hit it within FIGHT_HIT_NS - our frozen AI is still in its pre-fight state (in the
                // play-test it stood idle until hit; a hit always woke it at once). So: a 0-damage received hit (the
                // same as enemy_kick) and our player as its target, then the wake check with idle counted as asleep.
                // This also covers the host taking an enemy back after the asleep fallback (the guest's hit is recent).
                char shown[64];
                player_as_name(c, e->actor, shown, sizeof shown);
                bool battle = wake_kick_on() && e->last > 0 && !e->animal &&
                              (battle_as(shown) || (e->hurt_ns && now - e->hurt_ns < FIGHT_HIT_NS) ||
                               (e->peer_hit_ns && now - e->peer_hit_ns < FIGHT_HIT_NS));
                enemy_ai(e, true);
                e->following = false;
                bool local_hit = e->hurt_ns && now - e->hurt_ns < KICK_NS && e->last > 0 && !e->animal;
                bool kick = kick_on() && local_hit;
                // A real hit can enter SmallDamage while following. Mirroring Hide over its
                // one-shot leaves the reaction waiting for slot 0 to end (0x70005e3768).
                static int deafai_c = -1;
                if (local_hit && !coop_net_is_host() && !e->spawned && asleep_as(shown) &&
                    !strcmp(shown, e->playing) && env_on("SAVAGEWIND_COOP_DEAFAI", &deafai_c)) {
                    stop_anim(c, e->actor, 0);
                    LOG("[coop] enemy %s %u released mirrored %s after local hit\n", e->name, e->hash, shown);
                } else {
                    enemy_wake(c, e);
                }
                LOG("[coop] enemy %s %u: this side runs it now (AI on)%s%s\n", e->name, e->hash,
                    kick ? ", our hit replayed" : "", !kick && battle ? ", mid-fight: kicked awake" : "");
                if (kick || battle) {
                    enemy_kick(e, now);
                    if (battle) enemy_set_target(c, e->actor, 0);  // (the player: 0x7000bcc230)
                    // (0.1.1, split-screen playtest 2026-11-26: handed to us because our Link is the nearer one, the
                    // target rule at once pointed it back at the far partner's avatar - the partner's hit a moment
                    // ago weighs THREAT_HIT - and it searched about. It changed hands for us: our Link it is, held for
                    // TGT_HOLD_NS, and the partner's earlier hit no longer counts. SAVAGEWIND_COOP_TAKEOVERTGT=0: off.)
                    static int tot_c = -1;
                    if (battle && env_on("SAVAGEWIND_COOP_TAKEOVERTGT", &tot_c))
                        e->tgt_partner = false, e->tgt_ns = now, e->peer_hit_ns = 0;
                    if (wake_on() || battle) e->wake_ns = now, e->wake_tries = 0, e->wake_battle = battle;
                }
            } else if (e->wake_ns && e->last > 0) {
                enemy_wake_check(c, e, now);
            }
            if (e->mine && !e->following && e->last > 0 && !e->animal) enemy_target_frame(c, e, now);  // (the partner as a target)
            else e->tgt_partner = false;
            if (e->last > 0 && !e->animal) enemy_look_diag(c, e, now);
        }
        if (elem) elemfoe_frame(c, e, elem_hash == e->hash ? elem_dev : 0);
        else e->elem_native = e->elem_send = e->elem_recent = 0;
        if (e->actor) e->seen_ns = now;
    }
    // What goes into the packet (COOP_ENEMY_MAX entries), in this order:
    //  1. enemies our player hurt in the last 5 s (loaded or not: a kill must reach the other side even when the
    //     dead enemy is already gone here);
    //  2. the loaded enemies: active elemental status, then the ones we run, then nearest first -
    //     leaving ENEMY_KEEP_SLOTS free while there are unloaded hurt ones;
    //  3. in turns, enemies no longer loaded that our player hurt (so the peer still gets their totals: a dead enemy
    //     stays dead when the peer arrives later). Untouched unloaded enemies are not sent.
    // (Bug fixed 2026-11-08: with 16+ enemies loaded - the Pebblit/Chuchu field - steps 1 and 3 never got a slot: the
    // host's kill of a Pebblit the guest ran never reached the guest.)
    static int order[ENEMY_TABLE];
    static float dist[ENEMY_TABLE];
    static bool sent[ENEMY_TABLE];
    static int rotate;
    int n = 0, picked = 0;
    memset(sent, 0, sizeof sent);
    CoopEnemyDamage out[COOP_ENEMY_LOCAL_MAX];
    for (int i = 0; i < enemy_count && picked < COOP_ENEMY_MAX; i++)
        if (enemies[i].hurt_ns && now - enemies[i].hurt_ns < ENEMY_HURT_FIRST_NS) {
            enemy_entry(c, &enemies[i], &out[picked++]);
            sent[i] = true;
        }
    int unloaded_hurt = 0;
    for (int i = 0; i < enemy_count; i++)
        if (!enemies[i].actor && enemies[i].own && !sent[i]) unloaded_hurt++;
    int loaded_max = COOP_ENEMY_MAX - (unloaded_hurt < ENEMY_KEEP_SLOTS ? unloaded_hurt : ENEMY_KEEP_SLOTS);
    const float *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
    for (int i = 0; i < enemy_count; i++)
        if (enemies[i].actor && !sent[i]) {
            float p[3];
            // Keep short status changes in the snapshot even on an enemy the other side runs.
            dist[i] = actor_dist(enemies[i].actor, me, p) - (enemies[i].elem_native ? 200000.0f : 0.0f) +
                      (enemies[i].mine ? 0.0f : 100000.0f) +
                      (enemies[i].animal ? 1000.0f : 0.0f);  // (animals after the enemies)
            int k = n++;  // insertion sort by distance (a few dozen at most)
            while (k > 0 && dist[order[k - 1]] > dist[i]) order[k] = order[k - 1], k--;
            order[k] = i;
        }
    int animal_max = COOP_ENEMY_MAX - npc_n;  // (npc.inc: townspeople before wild animals, after real enemies)
    for (int j = 0; j < n && picked < loaded_max; j++) {
        if (enemies[order[j]].animal && picked >= animal_max) continue;
        enemy_entry(c, &enemies[order[j]], &out[picked++]);
        sent[order[j]] = true;
    }
    for (int j = 0; j < enemy_count && picked < COOP_ENEMY_MAX; j++) {
        rotate = (rotate + 1) % enemy_count;
        struct Enemy *e = &enemies[rotate];
        if (e->actor || !e->own || sent[rotate]) continue;
        enemy_entry(c, e, &out[picked++]);
        sent[rotate] = true;
    }
    for (int j = 0; j < n && picked < COOP_ENEMY_MAX; j++)  // (slots left over: more loaded ones)
        if (!sent[order[j]] && !(enemies[order[j]].animal && picked >= animal_max)) {
            enemy_entry(c, &enemies[order[j]], &out[picked++]);
            sent[order[j]] = true;
        }
    int npc_first = picked;
    for (int j = 0; j < npc_n && picked < COOP_ENEMY_MAX; j++) out[picked++] = npc_out[j];  // (npc.inc)
    if (npclog_on()) {  // (dev, a two-game test: what the packet holds - real enemies must come before townspeople)
        static uint64_t pk_ns;
        if (now - pk_ns > 5000000000ull) {
            pk_ns = now;
            int foes = 0, animals = 0, loaded_foes = 0, unsent_foes = 0;
            for (int j = 0; j < npc_first; j++) {
                int k = 0;
                while (k < enemy_count && enemies[k].hash != out[j].hash) k++;
                if (k < enemy_count && enemies[k].animal) animals++; else foes++;
            }
            for (int i = 0; i < enemy_count; i++)
                if (enemies[i].actor && !enemies[i].animal) { loaded_foes++; if (!sent[i]) unsent_foes++; }
            LOG("[coop] npc packet: %d entries = %d enemies + %d animals + %d npcs (%d npcs wanted); loaded enemies %d, "
                "left out %d\n", picked, foes, animals, picked - npc_first, npc_n, loaded_foes, unsent_foes);
        }
    }
    // (n6) The host: the first COOP_ENEMY_MAX entries as above (the partner's packet with two players), then the
    // loaded enemies that did not fit, nearest first, then unloaded hurt ones (coop_net picks each guest's 16 from
    // these when more than one guest shares the world).
    if (coop_net_is_host()) {
        for (int j = 0; j < n && picked < COOP_ENEMY_LOCAL_MAX; j++)
            if (!sent[order[j]]) {
                enemy_entry(c, &enemies[order[j]], &out[picked++]);
                sent[order[j]] = true;
            }
        for (int j = 0; j < enemy_count && picked < COOP_ENEMY_LOCAL_MAX; j++)
            if (!enemies[j].actor && enemies[j].own && !sent[j]) {
                enemy_entry(c, &enemies[j], &out[picked++]);
                sent[j] = true;
            }
    }
    coop_net_set_enemies(out, picked);
    // For the job threads (enemy_thawed): the actors of the copies that follow the host now.
    int f = 0;
    for (int i = 0; i < enemy_count && f < FOLLOW_MAX; i++)
        if (enemies[i].following && enemies[i].actor) follow_actor[f++] = enemies[i].actor;
    for (int k = f; k < FOLLOW_MAX; k++) follow_actor[k] = 0;
    // (ENEMYLOOK) the same for enemy_look_job, with what it checks; a copy that stopped following (unloaded, gone,
    // reattached, or now ours) drops its look target here and is no longer solved.
    int lf = 0;
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (!(e->following && e->actor)) {
            e->look_req = false, e->look_who = 0, e->look_vt = e->look_o = 0;
            continue;
        }
        if (lf >= FOLLOW_MAX || !e->look_vt || !e->look_o || LD64(e->actor) != e->look_vt ||
            enemy_look_obj(e->actor) != e->look_o)
            continue;
        look_pub_actor[lf] = 0;  // (the entry is invalid while it is rewritten)
        look_pub_vt[lf] = e->look_vt, look_pub_obj[lf] = e->look_o, look_pub_req[lf] = e->look_req;
        look_pub_actor[lf++] = e->actor;
    }
    for (int k = lf; k < FOLLOW_MAX; k++) look_pub_actor[k] = 0;
}

static void report_enemies(Cpu *c) {
    for (int i = 0; i < enemy_count; i++) {
        const struct Enemy *e = &enemies[i];
        float p[3] = {0, 0, 0}, rot[4];
        double age = -1;
        char as[32] = "", mine[64] = "";
        if (!coop_net_remote_enemy_pos(e->hash, p, rot, &age, as, sizeof as)) age = -1;
        char all[400] = "";
        if (e->actor && c) player_as_name(c, e->actor, mine, sizeof mine), as_list(c, e->actor, all, sizeof all);
        LOG("[coop] enemy %-28s hash %10u  life %d/%d  own %u  theirs %u  %s%s  AS %s  their pos %.1f %.1f %.1f "
            "AS %s age %.2f s  all AS: %s\n",
            e->name, e->hash, e->actor ? (int)LD32(e->actor + ENEMY_LIFE) : 0, e->base, e->own,
            coop_net_remote_damage(e->hash), e->actor ? "loaded" : "not loaded", e->following ? ", following" : "",
            mine[0] ? mine : "-", p[0], p[1], p[2], as[0] ? as : "-", age, all[0] ? all : "-");
        uint32_t fl = 0;
        double fage = -1;
        if (!coop_net_remote_enemy_flags(e->hash, &fl, &fage)) fage = -1;
        float here[3] = {0, 0, 0};  // (the copy's own position, same frame as "their pos" above)
        if (e->actor) {
            const float *m = (const float *)(uintptr_t)(e->actor + ACTOR_MTX);
            here[0] = m[3], here[1] = m[7], here[2] = m[11];
        }
        uint64_t now = plat_now_ns();
        uint64_t tgt = e->actor && enemy_alive(e) ? enemy_target_actor(e->actor) : 0;
        LOG("[coop]   ... runs here %d, guest runs %d, their flags %x (age %.2f s), actor %llx at %.1f %.1f %.1f, "
            "our hit %.1f s ago, theirs %.1f s ago, target %.40s%s, body %llx\n",
            e->mine, e->guest_runs, fl, fage, (unsigned long long)e->actor, here[0], here[1], here[2],
            e->hurt_ns ? (now - e->hurt_ns) / 1e9 : -1.0, e->peer_hit_ns ? (now - e->peer_hit_ns) / 1e9 : -1.0,
            tgt ? (const char *)(uintptr_t)(tgt + 0x1c) : "-", e->tgt_partner ? " (steered to the avatar)" : "",
            (unsigned long long)(e->actor && enemy_alive(e) ? enemy_body_kept(e) : 0));
        if (e->actor && enemy_alive(e) && !enemy_body_kept(e)) {  // (no body found: the candidates enemy_body tries)
            uint64_t a = e->actor, p1 = LD64(a + 0x578), q1 = in_guest(p1) ? LD64(p1 + 0x80) : 0,
                     b1 = in_guest(q1) ? LD64(q1 + 0x410) : 0, p2 = LD64(a + 0x198),
                     b2 = in_guest(p2) ? LD64(p2 + 0x920) : 0, p3 = LD64(a + 0x190), b3 = in_guest(p3) ? LD64(p3 + 0x78) : 0;
            LOG("[coop]   ... no body: +578 %llx ->80 %llx ->410 %llx (vt %llx); +198 %llx ->920 %llx (vt %llx); "
                "+190 %llx ->78 %llx (vt %llx); actor vt %llx\n", (unsigned long long)p1, (unsigned long long)q1,
                (unsigned long long)b1, (unsigned long long)(in_guest(b1) ? LD64(b1) : 0), (unsigned long long)p2,
                (unsigned long long)b2, (unsigned long long)(in_guest(b2) ? LD64(b2) : 0), (unsigned long long)p3,
                (unsigned long long)b3, (unsigned long long)(in_guest(b3) ? LD64(b3) : 0), (unsigned long long)LD64(a));
        }
        uint64_t lo =e->actor && enemy_alive(e) ? enemy_look_obj(e->actor) : 0;
        if (lo) {  // (ENEMYLOOK diagnosis: where its head looks, vs our Link and the avatar; first node's yaw/pitch)
            const float *t = (const float *)(uintptr_t)(lo + 0x18), *me = (const float *)(uintptr_t)(player_info_ptr() + 0x88);
            float dav = -1;
            if (avatar.actor && avatar_alive()) {
                const float *am = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
                float ap[3] = {am[3], am[7], am[11]};
                dav = look_dist(t, ap);
            }
            uint64_t nodes = LD64(lo + 0x30);
            float yaw = 0, pitch = 0;
            if (LD32(lo + 0x28) && in_guest(nodes)) memcpy(&yaw, (void *)(uintptr_t)(nodes + 0x134), 4),
                                                    memcpy(&pitch, (void *)(uintptr_t)(nodes + 0x138), 4);
            LOG("[coop]   ... look-at %.1f %.1f %.1f (%.1f m from our Link, %.1f m from the avatar) request %x battle %x "
                "nodes %u yaw %.2f pitch %.2f, sends %x, their flags look %x\n",
                t[0], t[1], t[2], look_dist(t, me), dav, LD32(lo + 0xe4), LD16(lo + 0x9c), LD32(lo + 0x28), yaw, pitch,
                e->mine ? enemy_look_flags(e->actor) : 0, fl & (COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PARTNER | COOP_ENEMY_LOOK_BATTLE));
        }
    }
    LOG("[coop] enemies: %d tracked\n", enemy_count);
    static CoopEnemyDamage rem[64];
    static double heard[64];
    int rn = coop_net_remote_spawned(rem, heard, 64);
    for (int i = 0; i < rn; i++) {
        float p[3] = {0, 0, 0}, rot[4];
        double age = -1;
        char name[COOP_SPAWN_NAME_LEN] = "?";
        bool pos = coop_net_remote_enemy_pos(rem[i].hash, p, rot, &age, NULL, 0);
        coop_net_remote_spawn_name(rem[i].hash, name, sizeof name);
        LOG("[coop] their run-time %08x %s: flags %x heard %.2f s ago, pos %d %.1f %.1f %.1f (age %.2f s)\n",
            rem[i].hash, name, rem[i].flags, heard[i], pos, p[0], p[1], p[2], age);
    }
}
