// Breath of the Wild 1.9.0 world manager. The current-weather getter uses the forecast for the current climate
// and four-hour time slot unless a temporary override is active. +0x7d1 alone is normally 255 (no override).
#define A_WORLD_WEATHER 0x7001431c3cull
#define A_WEATHER_TEMPERATURE 0x7001424af0ull
#define WORLD_SUBMGRS 0x5c8
#define WORLD_CLIMATE 0x780
#define WORLD_WEATHER_TIMER 0x794
#define WORLD_WEATHER_TYPE 0x7d1
#define WORLD_WEATHER_BLEND 0x7d5
#define WORLD_WEATHER_DEMO 0x7d6

static int weather_on_c = -1;
static uint8_t weather_published, weather_followed, weather_source;
static int bloodmoon_on_c = -1;
static bool bloodmoon_dev;
static uint16_t bloodmoon_serial;
static struct {
    uint64_t session, requested_ns, prohibit_tm;
    uint32_t prohibit_handle;
    uint16_t serial, pending;
    bool active;
} bloodmoon;

static uint64_t coop_world_mgr(void) {
    uint64_t wm = g_site.world_mgr ? LD64(g_site.world_mgr) : 0;
    return in_guest(wm + WORLD_WEATHER_DEMO) && LD32(wm + 0x5c0) >= 7 ? wm : 0;
}

static bool weather_overworld(void) {
    char area[96];
    local_area(area, sizeof area);
    return !strcmp(area, "MainField");
}

// 1.9.0 near strikes request these two native elements. Their vtable callbacks create the
// shockwave/bolt and ignition actors. Direct calls inside the recompiled scheduler bypass HLE overrides.
#define A_LIGHTNING_SHOCK_REQUEST 0x700142ca08ull
#define A_LIGHTNING_FIRE_REQUEST 0x700142cf04ull
#define A_LIGHTNING_SHOCK_CREATE 0x700142cc90ull
#define A_LIGHTNING_FIRE_CREATE 0x700142d004ull
#define LIGHTNING_POS 0x50
#define LIGHTNING_RADIUS 140.0f

enum { LIGHTNING_NATIVE, LIGHTNING_HOST, LIGHTNING_FOLLOW };
static int lightning_mode;
static int lightning_on_c = -1;
static __thread bool lightning_replaying;  // the native request calls the creator before returning
static GuestFn lightning_shock_orig, lightning_fire_orig;
static pthread_mutex_t lightning_lock = PTHREAD_MUTEX_INITIALIZER;
static CoopLightning lightning_impacts[16];
static unsigned lightning_count;
static uint64_t lightning_args;
static struct {
    bool pending;
    float pos[3];
} lightning_dev;

static void lightning_element(Cpu *c, GuestFn original, bool shock) {
    int mode = __atomic_load_n(&lightning_mode, __ATOMIC_ACQUIRE);
    bool replay = lightning_replaying;
    if (mode == LIGHTNING_FOLLOW && !replay) {
        c->x[0] = 0;  // same result as an element whose actor creation failed
        return;
    }
    CoopLightning impact = {0};
    if (shock && (mode == LIGHTNING_HOST || replay)) {
        impact.at_ns = plat_now_ns();
        memcpy(impact.pos, (void *)(uintptr_t)(c->x[0] + LIGHTNING_POS), sizeof impact.pos);
    }
    original(c);
    if (!impact.at_ns || !(c->x[0] & 1)) return;
    if (replay) {
        LOG("[coop] lightning: guest native impact %.2f %.2f %.2f\n",
            impact.pos[0], impact.pos[1], impact.pos[2]);
    } else {
        pthread_mutex_lock(&lightning_lock);
        if (lightning_count < sizeof lightning_impacts / sizeof lightning_impacts[0])
            lightning_impacts[lightning_count++] = impact;
        pthread_mutex_unlock(&lightning_lock);
    }
}

static void lightning_shock_create(Cpu *c) { lightning_element(c, lightning_shock_orig, true); }
static void lightning_fire_create(Cpu *c) { lightning_element(c, lightning_fire_orig, false); }

static void lightning_hooks_init(void) {
    if (!env_on("SAVAGEWIND_COOP_LIGHTNING", &lightning_on_c)) return;
    lightning_shock_orig = dispatch_lookup(A_LIGHTNING_SHOCK_CREATE);
    lightning_fire_orig = dispatch_lookup(A_LIGHTNING_FIRE_CREATE);
    if (!lightning_shock_orig || !lightning_fire_orig || !dispatch_lookup(A_LIGHTNING_SHOCK_REQUEST) ||
        !dispatch_lookup(A_LIGHTNING_FIRE_REQUEST)) {
        lightning_shock_orig = NULL;
        LOG("[coop] lightning: native functions unavailable\n");
        return;
    }
    hle_override(A_LIGHTNING_SHOCK_CREATE, lightning_shock_create, "coop lightning shockwave");
    hle_override(A_LIGHTNING_FIRE_CREATE, lightning_fire_create, "coop lightning fire");
}

static bool lightning_near(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz <= LIGHTNING_RADIUS * LIGHTNING_RADIUS;
}

static void lightning_request(Cpu *c, const float pos[3], bool replay) {
    if (!lightning_args) lightning_args = (uint64_t)(uintptr_t)hle_alloc(16);
    memcpy((void *)(uintptr_t)lightning_args, pos, 12);
    ST32(lightning_args + 12, 1);
    Cpu saved = *c;
    lightning_replaying = replay;
    c->x[0] = LD64(lightning_args);
    c->x[1] = LD64(lightning_args + 8);
    c->x[2] = 1;  // native water test, using the original impact coordinates
    guest_call(c, A_LIGHTNING_SHOCK_REQUEST);
    c->x[0] = lightning_args;
    guest_call(c, A_LIGHTNING_FIRE_REQUEST);
    lightning_replaying = false;
    *c = saved;
}

static void lightning_ev(Cpu *c, const uint8_t *data, int len) {
    if (len != sizeof(CoopLightning) ||
        __atomic_load_n(&lightning_mode, __ATOMIC_ACQUIRE) != LIGHTNING_FOLLOW ||
        framepace_loading_ui() || spawn_hold()) return;
    CoopLightning impact;
    memcpy(&impact, data, sizeof impact);
    char area[96];
    if (!coop_lightning_ok(&impact) || impact.area != local_area(area, sizeof area) ||
        !coop_net_lightning_recent(impact.at_ns) || !lightning_near(impact.pos, last_local.pos)) return;
    // Both games run their normal collision with their own Link. There is no network damage event.
    lightning_request(c, impact.pos, true);
    LOG("[coop] lightning: replay host strike %.2f %.2f %.2f at %llu\n",
        impact.pos[0], impact.pos[1], impact.pos[2], (unsigned long long)impact.at_ns);
}

static void lightning_frame(Cpu *c) {
    if (!lightning_shock_orig) return;
    bool host = coop_net_is_host();
    bool ready = local_in_world() && weather_overworld() && !framepace_loading_ui();
    uint64_t wm = ready ? coop_world_mgr() : 0;
    ready = wm && !LD8(wm + WORLD_WEATHER_DEMO);
    int mode = ready && host ? LIGHTNING_HOST :
               ready && weather_followed && coop_net_world_players(1.0) & 1u ? LIGHTNING_FOLLOW : LIGHTNING_NATIVE;
    __atomic_store_n(&lightning_mode, mode, __ATOMIC_RELEASE);
    if (!host) return;

    CoopLightning impacts[sizeof lightning_impacts / sizeof lightning_impacts[0]];
    pthread_mutex_lock(&lightning_lock);
    unsigned n = lightning_count;
    memcpy(impacts, lightning_impacts, n * sizeof impacts[0]);
    lightning_count = 0;
    bool request = lightning_dev.pending && ready && !spawn_hold();
    float pos[3];
    if (request) {
        memcpy(pos, lightning_dev.pos, sizeof pos);
        lightning_dev.pending = false;
    }
    pthread_mutex_unlock(&lightning_lock);
    if (request && lightning_near(pos, last_local.pos)) lightning_request(c, pos, false);
    if (!ready || !n) return;

    char area[96];
    uint32_t here = local_area(area, sizeof area);
    uint32_t peers = coop_net_world_players(1.0);
    for (unsigned i = 0; i < n; i++) {
        CoopLightning *impact = &impacts[i];
        impact->area = here;
        if (!coop_lightning_ok(impact) || plat_now_ns() - impact->at_ns >= 1000000000ull) continue;
        unsigned sent = 0;
        for (int pid = 1; pid < COOP_MAX_PLAYERS; pid++) {
            if (!(peers & (1u << pid))) continue;
            CoopState remote;
            double age;
            coop_net_view(pid);
            bool present = coop_net_get_remote(&remote, &age);
            coop_net_view(-1);
            if (present && age < 1.0 && (remote.flags & COOP_FLAG_IN_WORLD) && remote.area == here &&
                lightning_near(impact->pos, remote.pos) &&
                coop_net_send_event_to(pid, COOP_EV_LIGHTNING, impact, sizeof *impact)) sent++;
        }
        LOG("[coop] lightning: host native impact %.2f %.2f %.2f at %llu sent to %u\n",
            impact->pos[0], impact->pos[1], impact->pos[2], (unsigned long long)impact->at_ns, sent);
    }
}

static uint8_t weather_for_local_temperature(Cpu *c, uint64_t wm, uint8_t type) {
    // Match WeatherMgr's rain/snow boundary, including altitude and the temperature transition.
    // Thunder and thunder rain keep their lightning. A sun shower in freezing weather becomes light snow.
    if (type < 2 || type == 6 || type == 7) return type;
    uint64_t mgrs = LD64(wm + WORLD_SUBMGRS);
    uint64_t temp = in_guest(mgrs + 0x20) ? LD64(mgrs + 0x20) : 0;
    if (!in_guest(temp + 0x60)) return type;
    Cpu saved = *c;
    c->x[0] = temp;
    guest_call(c, A_WEATHER_TEMPERATURE);
    float next;
    memcpy(&next, &c->v[0], sizeof next);
    *c = saved;
    bool freezing = next <= -2.0f || LDF32(temp + 0x60) <= -2.0f;
    if (freezing && (type == 2 || type == 8)) return 4;
    if (freezing && type == 3) return 5;
    if (!freezing && (type == 4 || type == 5)) return type - 2;
    return type;
}

static uint8_t bloodmoon_host_day(Cpu *c);

// ---- the host's weather kept over its death (2026-11-21: when the host dies and respawns the weather can
// change, which looks weird for the other player).
// The game over (Demo006_*) reloads the last save and the weather forecast is rolled again, so the guest (who follows
// the host's weather) saw the weather jump. Now the host, back in the overworld after that reload, keeps the weather it
// had before dying (= what the guest still shows) with the same native override the guest uses, until the forecast's
// 4-hour slot changes (when the weather would change anyway), at most WHOLD_MAX_NS, or until it leaves the overworld.
// The reload also turns the clock back to the save's time (seen: 10:42 -> 5:15 for both players); the host's clock is
// set back to the time of the death in the same frame it is back (the guest follows the host's clock).
// Sleeping is left alone (no reload). SAVAGEWIND_COOP_WEATHERHOLD=0: off (weather and clock); SAVAGEWIND_COOP_TIMEHOLD=0:
// the clock only.
#define WHOLD_MAX_NS (6ull * 60 * 1000000000ull)
float test_warp_hour(bool *held);
void test_warp_set_hour(float hour);
static int whold_on_c = -1, thold_on_c = -1;
static struct {
    int state;  // 0 idle, 1 game over seen, 2 reload seen (out of the world), 3 holding
    bool have_type;
    uint8_t type;
    float hour;  // at the death (-1: unknown)
    int slot;
    uint64_t since_ns;
} whold;
static uint8_t weather_for_local_temperature(Cpu *c, uint64_t wm, uint8_t type);

static void weather_hold_frame(Cpu *c) {
    if (!env_on("SAVAGEWIND_COOP_WEATHERHOLD", &whold_on_c)) return;
    uint64_t now = plat_now_ns();
    if (!strncmp(framepace_event_name(), "Demo006_", 8)) {
        if (whold.state != 1) {
            whold.state = 1, whold.since_ns = now;
            whold.have_type = weather_published != 0, whold.type = weather_published ? weather_published - 1 : 0;
            whold.hour = local_in_world() ? test_warp_hour(NULL) : -1.0f;
            LOG("[coop] weather: host game over: keeping weather type %d and clock %.2f h for after the reload (WEATHERHOLD)\n",
                whold.have_type ? whold.type : -1, whold.hour);
        }
        return;
    }
    if (!whold.state) return;
    uint64_t wm = coop_world_mgr();
    float hour = test_warp_hour(NULL);
    bool here = wm && local_in_world() && weather_overworld() && hour >= 0;
    if (whold.state == 1) {  // (the scene is over: wait for the reload)
        if (!local_in_world()) whold.state = 2;
        else if (now - whold.since_ns > 60000000000ull) whold.state = 0;  // (no reload came: a revival)
        return;
    }
    if (whold.state == 2) {
        if (!local_in_world() || hour < 0) return;
        if (whold.hour >= 0 && env_on("SAVAGEWIND_COOP_TIMEHOLD", &thold_on_c)) {
            LOG("[coop] time: host back after the game over: clock %.2f -> %.2f h (the time of the death, TIMEHOLD)\n", hour, whold.hour);
            test_warp_set_hour(whold.hour);
            hour = whold.hour;
        }
        if (!here || !whold.have_type || g_settings.weather) { whold.state = 0; return; }  // (a shrine, or a weather setting)
        whold.state = 3, whold.slot = (int)(hour / 4), whold.since_ns = now;
        LOG("[coop] weather: host back after the game over: weather type %u kept (%.2f h)\n", whold.type, hour);
    }
    if (!here || (int)(hour / 4) != whold.slot || now - whold.since_ns > WHOLD_MAX_NS) {
        whold.state = 0;
        LOG("[coop] weather: host's kept weather released (%s)\n", here ? "the forecast moved on" : "left the overworld");
        return;
    }
    if (LD8(wm + WORLD_WEATHER_DEMO)) return;
    *(volatile uint8_t *)(uintptr_t)(wm + WORLD_WEATHER_TYPE) = weather_for_local_temperature(c, wm, whold.type);
    *(volatile uint8_t *)(uintptr_t)(wm + WORLD_WEATHER_BLEND) = 0;
    ST32(wm + WORLD_WEATHER_TIMER, 4);
}

static void environment_publish(Cpu *c, CoopState *s) {
    if (!coop_net_is_host()) return;
    s->bloodmoon = bloodmoon_serial;
    s->moon_day = bloodmoon_host_day(c);
    if (!env_on("SAVAGEWIND_COOP_WEATHER", &weather_on_c)) return;
    // Keep the last overworld weather while the host is indoors or a demo owns its weather.
    s->weather = weather_published;
    if (!(s->flags & COOP_FLAG_IN_WORLD) || !weather_overworld()) return;
    if (whold.state == 1 || whold.state == 2) return;  // (game over / reload: the new forecast is not sent)
    uint64_t wm = coop_world_mgr();
    if (!wm || LD8(wm + WORLD_WEATHER_DEMO)) return;
    Cpu saved = *c;
    c->x[0] = wm;
    guest_call(c, A_WORLD_WEATHER);
    uint8_t type = (uint8_t)c->x[0];
    *c = saved;
    if (type >= COOP_WEATHER_TYPES) return;
    s->weather = weather_published = type + 1;
}

static void coop_weather_frame(Cpu *c) {
    if (!env_on("SAVAGEWIND_COOP_WEATHER", &weather_on_c)) return;
    if (coop_net_is_host()) { weather_hold_frame(c); return; }
    uint64_t wm = coop_world_mgr();
    CoopState host;
    double age;
    uint64_t session;
    uint8_t want = 0;
    if (wm && !g_settings.weather && !LD8(wm + WORLD_WEATHER_DEMO) &&
        local_in_world() && weather_overworld() && coop_net_get_host(&host, &age, &session) && age < 1.0 && host.weather)
        want = weather_for_local_temperature(c, wm, host.weather - 1) + 1;
    if (want) {
        // Use the native weather request for the sky/light and WeatherMgr's current condition.
        // Its short lifetime lets native weather resume when we stop refreshing it.
        *(volatile uint8_t *)(uintptr_t)(wm + WORLD_WEATHER_TYPE) = want - 1;
        *(volatile uint8_t *)(uintptr_t)(wm + WORLD_WEATHER_BLEND) = 0;
        ST32(wm + WORLD_WEATHER_TIMER, 4);
        if (want != weather_followed || host.weather != weather_source)
            LOG("[coop] weather: guest applied type %u from host type %u (local climate %u)\n",
                want - 1, host.weather - 1, LD32(wm + WORLD_CLIMATE));
    } else if (weather_followed) {
        LOG("[coop] weather: released host weather\n");
    }
    weather_followed = want;
    weather_source = want ? host.weather : 0;
}

// TimeMgr's midnight rollover requests Demo011_0 when WM_BloodyDay or its cached request is set.
// The demo itself calls Demo_OffWaitRevival, which lets the native respawn work proceed.
#define A_BLOODMOON_REQUEST 0x7001425c5cull
#define A_BLOODMOON_DAY_GET 0x7001426c34ull
#define A_BLOODMOON_DAY_SET 0x700064af38ull
#define MOON_TIMER 0xc8
#define MOON_DAY_HANDLE 0x11c
#define MOON_GATE_HANDLE 0x128  // (a midnight-only gate flag handle; see bloodmoon_follow_day)
#define MOON_REQUEST 0x14a
#define MOON_DAY_CACHED 0x152
#define MOON_SCHEDULE_THRESHOLD 2520.0f

static uint64_t bloodmoon_time_mgr(void) {
    uint64_t wm = coop_world_mgr();
    uint64_t mgrs = wm ? LD64(wm + WORLD_SUBMGRS) : 0;
    uint64_t tm = in_guest(mgrs) ? LD64(mgrs) : 0;
    return in_guest(tm + MOON_DAY_CACHED) ? tm : 0;
}

static void bloodmoon_enemy_generation(uint16_t serial) {
    if (!coop_net_set_enemy_generation(serial)) return;
    // Placed enemies may return with the same HashId. Carrying the old damage total into that actor kills it again.
    for (int i = 0; i < enemy_count; i++) {
        struct Enemy *e = &enemies[i];
        if (e->spawned) continue;
        e->own = e->applied_remote = 0;
        e->remote_known = false;
        e->inject_ns = e->hurt_ns = e->peer_hit_ns = 0;
        int32_t life = enemy_alive(e) ? (int32_t)LD32(e->actor + ENEMY_LIFE) : 0;
        e->base = e->last = life > 0 ? life : 0;
    }
}

static bool bloodmoon_day_get(Cpu *c, uint64_t tm) {
    Cpu saved = *c;
    c->x[0] = tm;
    guest_call(c, A_BLOODMOON_DAY_GET);
    bool day = (c->x[0] & 1) != 0;
    *c = saved;
    return day;
}

static uint8_t bloodmoon_host_day(Cpu *c) {
    if (!env_on("SAVAGEWIND_COOP_BLOODMOON", &bloodmoon_on_c)) return COOP_MOON_DAY_UNKNOWN;
    uint64_t tm = bloodmoon_time_mgr();
    if (!tm || !in_guest(LD64(A_GDT_MGR)) || LD32(tm + MOON_DAY_HANDLE) == UINT32_MAX)
        return COOP_MOON_DAY_UNKNOWN;
    return !bloodmoon.active && bloodmoon_day_get(c, tm) ? COOP_MOON_DAY_ON : COOP_MOON_DAY_OFF;
}

static void bloodmoon_follow_day(Cpu *c, uint64_t tm, bool want) {
    *(volatile uint8_t *)(uintptr_t)(tm + MOON_REQUEST) = 0;
    if (LDF32(tm + MOON_TIMER) >= MOON_SCHEDULE_THRESHOLD - 1.0f)
        *(volatile float *)(uintptr_t)(tm + MOON_TIMER) = MOON_SCHEDULE_THRESHOLD - 1.0f;
    uint64_t gdt = LD64(A_GDT_MGR);
    uint32_t handle = LD32(tm + MOON_DAY_HANDLE);
    if (!in_guest(gdt) || handle == UINT32_MAX) return;
    bool controlled = bloodmoon.prohibit_tm == tm && LD32(tm + MOON_GATE_HANDLE) == handle;
    bool day = bloodmoon_day_get(c, tm);
    if (day != want) {
        Cpu saved = *c;
        c->x[0] = gdt, c->x[1] = want, c->x[2] = handle;
        guest_call(c, A_BLOODMOON_DAY_SET);
        *c = saved;
        if (want && !bloodmoon_day_get(c, tm)) return;
    }
    *(volatile uint8_t *)(uintptr_t)(tm + MOON_DAY_CACHED) = want;
    if (want && !controlled) {
        // Block the guest's own midnight request: point one of the midnight gate's flag handles (TimeMgr+0x128,
        // read only by 0x7001425cc4, whose one caller is the midnight rollover 0x7001425e98) at the mirrored day
        // flag while it is true, then restore it before the serial-driven demo. GameData flags stay untouched.
        // (Until 2026-11-21 this used the BloodyMoonProhibition handle +0x140, but the red evening renderer
        // 0x700141ac4c also reads that one (via 0x7001425d8c) and fades the red to nothing while it is set:
        // the guest never saw the red sky, embers and moon, only the midnight cutscene.)
        bloodmoon.prohibit_tm = tm;
        bloodmoon.prohibit_handle = LD32(tm + MOON_GATE_HANDLE);
        ST32(tm + MOON_GATE_HANDLE, handle);
        LOG("[coop] blood moon: guest applied host bloody day\n");
    } else if (!want && controlled) {
        ST32(tm + MOON_GATE_HANDLE, bloodmoon.prohibit_handle);
        bloodmoon.prohibit_tm = 0;
        LOG("[coop] blood moon: guest cleared shared bloody day\n");
    } else if (!want && day) {
        LOG("[coop] blood moon: suppressed local schedule on guest\n");
    }
}

static void bloodmoon_release_day(Cpu *c) {
    if (!bloodmoon.prohibit_tm) return;
    uint64_t tm = bloodmoon_time_mgr();
    if (tm == bloodmoon.prohibit_tm) bloodmoon_follow_day(c, tm, false);
    else if (tm) bloodmoon.prohibit_tm = 0;
}

static void bloodmoon_frame(Cpu *c) {
    bool on = env_on("SAVAGEWIND_COOP_BLOODMOON", &bloodmoon_on_c);
    bool host = coop_net_is_host();
    bool active = !strcmp(framepace_event_name(), "Demo011_0");
    if (active && !bloodmoon.active) {
        if (host && on) {
            ++bloodmoon_serial;
            bloodmoon_enemy_generation(bloodmoon_serial);
            LOG("[coop] blood moon: host cutscene started (%u)\n", bloodmoon_serial);
        } else if (bloodmoon.requested_ns && bloodmoon.pending) {
            --bloodmoon.pending;
            bloodmoon_enemy_generation(bloodmoon.serial);
            LOG("[coop] blood moon: guest cutscene started (%u waiting)\n", bloodmoon.pending);
        }
        bloodmoon.requested_ns = 0;
    }
    if (bloodmoon.active && !active) LOG("[coop] blood moon: cutscene ended\n");
    bloodmoon.active = active;

    if (!host) {
        CoopState state;
        uint64_t session;
        double age;
        if (coop_net_get_host(&state, &age, &session)) {
            if (session != bloodmoon.session) {
                bloodmoon.session = session;
                bloodmoon.serial = state.bloodmoon;
                bloodmoon.pending = 0;
                bloodmoon.requested_ns = 0;
                bloodmoon_enemy_generation(state.bloodmoon);
            }
            uint16_t delta = state.bloodmoon - bloodmoon.serial;
            if (delta && delta < 0x8000) {
                bloodmoon.serial = state.bloodmoon;
                // A single revival covers missed moons while away. Keep an extra request if one is already loading.
                if (on) {
                    bloodmoon.pending = bloodmoon.requested_ns ? 2 : 1;
                    LOG("[coop] blood moon: received host cutscene (%u)\n", state.bloodmoon);
                } else {
                    bloodmoon_enemy_generation(state.bloodmoon);
                }
            }
            uint64_t tm = bloodmoon_time_mgr();
            if (on && tm && !active) {
                bool want = age < 1.0 && state.moon_day == COOP_MOON_DAY_ON && !bloodmoon.pending &&
                            local_in_world() && weather_overworld();
                bloodmoon_follow_day(c, tm, want);
            } else bloodmoon_release_day(c);
        } else bloodmoon_release_day(c);
    } else {
        bloodmoon_release_day(c);
    }

    bool dev = host && __atomic_load_n(&bloodmoon_dev, __ATOMIC_ACQUIRE);
    if (!(dev || (!host && on && bloodmoon.pending))) return;
    if (!local_in_world() || !weather_overworld() || framepace_loading_ui() || event_running()) {
        static uint64_t diag_ns;
        uint64_t t = plat_now_ns();
        if (t - diag_ns > 3000000000ull) {
            diag_ns = t;
            LOG("[coop] blood moon: waiting (world %d overworld %d loading %d event %d)\n", local_in_world(),
                weather_overworld(), framepace_loading_ui(), event_running());
        }
        return;
    }
    uint64_t now = plat_now_ns();
    if (bloodmoon.requested_ns && now - bloodmoon.requested_ns < 15000000000ull) return;
    uint64_t tm = bloodmoon_time_mgr();
    if (!tm) return;
    if (dev) __atomic_store_n(&bloodmoon_dev, false, __ATOMIC_RELEASE);
    bloodmoon.requested_ns = now;
    *(volatile float *)(uintptr_t)(tm + MOON_TIMER) = 0.0f;
    Cpu saved = *c;
    guest_call(c, A_BLOODMOON_REQUEST);
    *c = saved;
    LOG("[coop] blood moon: %s requested native cutscene\n", host ? "host dev" : "guest");
}

RT_ON_RESET(coop_weather_reset) {
    weather_published = weather_followed = weather_source = 0;
    memset(&whold, 0, sizeof whold);
    __atomic_store_n(&lightning_mode, LIGHTNING_NATIVE, __ATOMIC_RELEASE);
    pthread_mutex_lock(&lightning_lock);
    lightning_dev.pending = false;
    lightning_count = 0;
    pthread_mutex_unlock(&lightning_lock);
    lightning_args = 0;
    __atomic_store_n(&bloodmoon_dev, false, __ATOMIC_RELEASE);
    memset(&bloodmoon, 0, sizeof bloodmoon);
    // The outgoing serial belongs to the network session, which can outlive a guest boot.
}
