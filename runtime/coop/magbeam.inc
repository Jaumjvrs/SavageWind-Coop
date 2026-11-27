// The native MagneConnect ELink effect belongs to the world effects actor. The
// held-object callback aims its matrix from the Slate to the object.
static struct {
    uint64_t handle;  // reusable guest return buffer, two words
    uint64_t retry_ns;
    bool showing;
} magbeam;
static bool magbeam_local_active;

// Game 1.9.0 addresses (found by the coop-runefx worker's game_sites patterns, generated layout 2026-11-18). Kept
// here instead of in tools/game_sites.py: that file is part of the game library's id, and a new id makes every
// player's installed library refuse the co-op zip. The key string is checked once before the beam is used.
static const struct { uint64_t mgr, world, key, emit, fade, shader_mgr; } mb_site = {
    .mgr = 0x7001e15f18ull, .world = 0x7001e3f2b0ull, .key = 0x7001600222ull,
    .emit = 0x7001100bd0ull, .fade = 0x700056d010ull, .shader_mgr = 0x7001e58720ull,
};

static bool magbeam_on(void) {
    static int on = -1;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_MAGBEAM");
        if (on && (!in_guest(mb_site.key) || memcmp((const void *)(uintptr_t)mb_site.key, "MagneConnect", 13))) {
            LOG("[coop] magbeam: the game's MagneConnect name is not where expected (other game version?): beam off\n");
            on = 0;
        }
    }
    return on;
}

static bool magbeam_log(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_MAGBEAM_LOG");
        on = e && !strcmp(e, "1");
    }
    return on;
}

// (elink_live_effect is defined in coop/avatarfx.inc)

static void magbeam_publish(CoopState *s) {
    if (!magbeam_on()) return;
    bool active = false;
    if ((s->flags & COOP_FLAG_IN_WORLD) && s->area) {
        uint64_t mgr = LD64(mb_site.mgr);
        // +0x331: rune connection requested, cleared on release. +0x318: held
        // body, set after the acquisition burst reaches the object.
        if (in_guest(mgr) && in_guest(mgr + 0x331) && LD8(mgr + 0x331) && LD64(mgr + 0x318)) {
            uint64_t fx = elink_live_effect(mgr + 0x48);
            if (fx) {
                float length = LDF32(fx + 0x110);
                float tip[] = {
                    LDF32(fx + 0xcc) + LDF32(fx + 0xc8) * length,
                    LDF32(fx + 0xdc) + LDF32(fx + 0xd8) * length,
                    LDF32(fx + 0xec) + LDF32(fx + 0xe8) * length,
                };
                if (coop_magbeam_dist2(tip, s->pos) <= COOP_MAGBEAM_RANGE * COOP_MAGBEAM_RANGE) {
                    memcpy(s->magbeam_tip, tip, sizeof tip);
                    s->flags |= COOP_FLAG_MAGBEAM;
                    active = true;
                }
            }
        }
    }
    if (active != magbeam_local_active && magbeam_log()) {
        if (active) LOG("[coop] magbeam: local grab at %.2f %.2f %.2f\n",
                        s->magbeam_tip[0], s->magbeam_tip[1], s->magbeam_tip[2]);
        else LOG("[coop] magbeam: local release\n");
    }
    magbeam_local_active = active;
}

static void magbeam_stop(Cpu *c) {
    if (!magbeam.showing) return;
    if (elink_live_effect(magbeam.handle)) {
        Cpu saved = *c;
        c->x[0] = magbeam.handle;
        guest_call(c, mb_site.fade);
        *c = saved;
    }
    memset((void *)(uintptr_t)magbeam.handle, 0, 16);
    magbeam.showing = false;
    if (magbeam_log()) LOG("[coop] magbeam: stopped\n");
}

static uint64_t magbeam_start(Cpu *c) {
    uint64_t world = LD64(mb_site.world);
    uint64_t emitter = in_guest(world) ? LD64(world + 0x30) : 0;
    if (!in_guest(emitter)) return 0;
    if (!magbeam.handle) magbeam.handle = (uint64_t)(uintptr_t)hle_alloc(16);
    if (!magbeam.handle) return 0;
    memset((void *)(uintptr_t)magbeam.handle, 0, 16);
    Cpu saved = *c;
    c->x[0] = emitter;
    c->x[1] = mb_site.key;
    c->x[8] = magbeam.handle;  // AArch64 indirect return
    guest_call(c, mb_site.emit);
    *c = saved;
    return elink_live_effect(magbeam.handle);
}

static bool magbeam_shader_ready(void) {
    uint64_t mgr = LD64(mb_site.shader_mgr);
    if (!in_guest(mgr) || !in_guest(mgr + 0x192e0)) return false;
    // MagneConnect's shader callback also reads a shared up vector. It starts
    // at zero in a game that has never used Magnesis. Supply world up only
    // while it is unset so the native rune remains free to set its own.
    uint64_t up = mgr + 0x192d8;
    if (LDF32(up) == 0.0f && LDF32(up + 4) == 0.0f && LDF32(up + 8) == 0.0f)
        *(float *)(uintptr_t)(up + 4) = 1.0f;
    return true;
}

static void magbeam_frame(Cpu *c, const CoopState *rs, bool fresh) {
    if (!magbeam_on()) return;
    if (!local_in_world()) {
        // Scene teardown retires the ELink effects.
        if (magbeam.handle) memset((void *)(uintptr_t)magbeam.handle, 0, 16);
        magbeam.showing = false;
        magbeam.retry_ns = 0;
        return;
    }
    if (!fresh || !(rs->flags & COOP_FLAG_MAGBEAM) || !avatar.actor || !avatar_alive() ||
        rs->area != avatar.area || spawn_hold()) {
        magbeam_stop(c);
        return;
    }

    // Use the same transform as the remote Slate puppet. The bone fallback
    // covers the few frames before gear placement is available.
    const CoopGear *slate = &rs->gear[COOP_GEAR_SLATE];
    float m[12];
    if (!(slate->name[0] && slate->bone == COOP_BONE_WEAPON_L && gear_world(c, slate, m)) &&
        !bone_matrix(c, avatar.actor, "Weapon_L", m)) {
        magbeam_stop(c);
        return;
    }
    const float *tip = rs->magbeam_tip;
    float x = tip[0] - m[3], y = tip[1] - m[7], z = tip[2] - m[11];
    float length = sqrtf(x * x + y * y + z * z);
    if (!(length > 0.05f && length <= COOP_MAGBEAM_RANGE) || !magbeam_shader_ready()) {
        magbeam_stop(c);
        return;
    }
    x /= length; y /= length; z /= length;
    // Build a stable right axis even when the object is directly overhead.
    float rx = z, ry = 0, rz = -x;
    if (fabsf(y) > 0.99f) { rx = 0; ry = -z; rz = y; }
    float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
    rx /= rlen; ry /= rlen; rz /= rlen;
    float matrix[12] = {
        rx, y * rz - z * ry, x, m[3],
        ry, z * rx - x * rz, y, m[7],
        rz, x * ry - y * rx, z, m[11],
    };

    uint64_t fx = magbeam.showing ? elink_live_effect(magbeam.handle) : 0;
    if (!fx) {
        uint64_t now = plat_now_ns();
        if (now < magbeam.retry_ns) return;
        fx = magbeam_start(c);
        if (!fx) {
            magbeam.retry_ns = now + 250000000ull;
            return;
        }
        magbeam.showing = true;
        if (magbeam_log()) LOG("[coop] magbeam: showing (%.2f m)\n", length);
    }
    // MagneConnect uses a world matrix and the effect's X length channel.
    static const float scale[] = {1.0f, 1.0f, 1.0f};
    const float size[] = {1.0f, length};
    memcpy((void *)(uintptr_t)(fx + 0xb4), scale, sizeof scale);
    memcpy((void *)(uintptr_t)(fx + 0xc0), matrix, sizeof matrix);
    memcpy((void *)(uintptr_t)(fx + 0x10c), size, sizeof size);
    *(uint32_t *)(uintptr_t)(fx + 0x180) |= 0x408;
    // The effect starts hidden (fade = 1). The native hold callback writes
    // the manager's fade value, which is 0 for a steady grab.
    uint64_t custom = LD64(fx + 0x190);
    if (in_guest(custom) && in_guest(custom + 0x18))
        *(float *)(uintptr_t)(custom + 0x18) = 0.0f;
}
