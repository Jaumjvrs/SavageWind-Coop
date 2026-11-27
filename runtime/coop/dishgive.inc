// Included by coop.c. Pouch functions and layout for BOTW 1.9.0.
#include "coop_dish.h"
#include "items.h"

#define A_DISH_RECORD_INIT 0x7000572d80ull
#define A_DISH_ADD         0x7000d92e3cull   // add the cooked actor's complete record, then save the pouch
#define A_DISH_RECYCLE     0x7000d95d0cull   // remove this exact pouch record
#define A_DISH_REBUILD    0x7000d90898ull
#define A_DISH_REINDEX    0x7000d93568ull
#define A_DISH_SAVE       0x7000d92954ull
#define A_DISH_LOCK       0x7000eaff08ull
#define A_DISH_UNLOCK     0x7000eaff18ull
#define DISH_RANGE2       100.0f
#define DISH_RESULT_NS    5000000000ull
#define DISH_RECORD_SIZE  0x228

static CoopDish dish_list[COOP_DISH_MAX];  // (game thread; dish_ui_list is the copy the menu reads)
static int dish_list_n;
static CoopDish dish_ui_list[COOP_DISH_MAX];
static int dish_ui_n;
static int dish_ui_lock;  // spin lock around dish_ui_list
// (DISHINV) With more than two players a dish goes to one player: the nearest one within 10 m in our area (or the
// one the dish window named). dish_out_pid: who our outgoing dish is for; dish_in_pid: who the incoming one is from.
static CoopDishOffer dish_out, dish_in;
static int dish_out_pid = -1, dish_in_pid = -1;
static CoopDishResult dish_last_result[COOP_MAX_PLAYERS];  // per giver: our last answer (sent again for a repeat)
static bool dish_reply[COOP_MAX_PLAYERS];
static bool dish_queued, dish_returning;
static uint64_t dish_id, dish_next_send, dish_next_scan, dish_mem;
// The menu (render thread) only hands a request over (dish_req + dish_req_pending) and reads dish_can_give; the pouch
// and the transfer state stay on the game thread.
static CoopDish dish_req;
static int dish_req_pending, dish_can_give;
static uint64_t dish_notice_until;
static int dish_notice;
static int dish_dev_action;  // control thread: -1 dump, 1..60 give that row
static CoopDish dish_dev_make;  // (dev) a dish to put in our own pouch with the game's cooked-item get
static int dish_dev_make_pending;

static void dish_status_set(int status) {
    __atomic_store_n(&dish_notice_until, plat_now_ns() + DISH_RESULT_NS, __ATOMIC_RELAXED);
    __atomic_store_n(&dish_notice, status, __ATOMIC_RELEASE);
}

int coop_dish_status(bool recent_only) {
    int status = __atomic_load_n(&dish_notice, __ATOMIC_ACQUIRE);
    if (recent_only && status >= DISH_SENT && status != DISH_RETURNING &&
        plat_now_ns() >= __atomic_load_n(&dish_notice_until, __ATOMIC_RELAXED)) return DISH_IDLE;
    return status;
}

bool coop_dish_enabled(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_DISHGIVE");
    return on && code_matches();
}

// Player pid's distance squared from us (1e30: no fresh state, not in the world); *same: it and we are in `area`.
static float dish_dist2(int pid, uint32_t area, bool *same) {
    CoopState st;
    double age;
    char name[96];
    *same = false;
    if (pid < 0 || pid >= COOP_MAX_PLAYERS || pid == coop_net_my_id() || !local_in_world()) return 1e30f;
    coop_net_view(pid);
    bool ok = coop_net_get_remote(&st, &age);
    coop_net_view(-1);
    if (!ok || age >= 3.0 || !(st.flags & COOP_FLAG_IN_WORLD)) return 1e30f;
    *same = area && st.area == area && local_area(name, sizeof name) == area;
    return coop_magbeam_dist2(last_local.pos, st.pos);
}
// Player pid is within 10 m of us, both in `area` (the offer's), and linked to us for dishes.
static bool dish_player_near(int pid, uint32_t area) {
    bool same;
    uint64_t a, b;
    return coop_net_dish_session(pid, &a, &b) && dish_dist2(pid, area, &same) <= DISH_RANGE2 && same;
}
// The nearest other player a dish can go to now (within 10 m, our area); -1: none. any: the nearest linked player
// at any distance instead (only for a label: who the button would name).
static int dish_nearest(uint32_t area, bool any) {
    int best = -1;
    float bd = 1e30f;
    uint32_t w = coop_net_world_players(3.0);
    for (int pid = 0; pid < COOP_MAX_PLAYERS; pid++) {
        uint64_t a, b;
        bool same;
        if (!(w & (1u << pid)) || !coop_net_dish_session(pid, &a, &b)) continue;
        float d2 = dish_dist2(pid, area, &same);
        if (!any && (!same || d2 > DISH_RANGE2)) continue;
        if (d2 < bd || best < 0) bd = d2, best = pid;
    }
    return best;
}

bool coop_dish_can_give(void) {
    return coop_dish_enabled() && __atomic_load_n(&dish_can_give, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&dish_req_pending, __ATOMIC_ACQUIRE);
}

int coop_dish_list(CoopDish out[COOP_DISH_MAX]) {
    if (!coop_dish_enabled()) return 0;
    while (__atomic_exchange_n(&dish_ui_lock, 1, __ATOMIC_ACQUIRE)) {}
    int n = dish_ui_n;
    memcpy(out, dish_ui_list, (size_t)n * sizeof *out);
    __atomic_store_n(&dish_ui_lock, 0, __ATOMIC_RELEASE);
    return n;
}

bool coop_dish_request(const CoopDish *dish) {
    if (!coop_dish_can_give()) return false;
    dish_req = *dish;
    __atomic_store_n(&dish_req_pending, 1, __ATOMIC_RELEASE);
    dish_status_set(DISH_QUEUED);
    return true;
}

// (game thread) A request from the menu or the control command becomes the outgoing offer (dish_queued: taken out of
// our pouch once the game runs). pid: the player it is for (-1: the nearest one within 10 m).
static bool dish_start(const CoopDish *dish, int pid) {
    char area[96];
    if (dish_out.id || !coop_dish_enabled()) return false;
    CoopDishOffer o = {.area = local_area(area, sizeof area), .dish = *dish};
    int me = coop_net_my_id();
    if (pid < 0) pid = dish_nearest(o.area, false);
    if (me < 0 || pid < 0 || !dish_player_near(pid, o.area) || !coop_net_dish_session(pid, &o.from, &o.to)) return false;
    o.from_pid = (uint8_t)me, o.to_pid = (uint8_t)pid;
    o.id = dish_id + 1;
    if (!coop_dish_offer_ok(&o)) return false;
    dish_id++;
    dish_out = o;
    dish_out_pid = pid;
    dish_queued = true;
    dish_status_set(DISH_QUEUED);
    char who[COOP_NAME_LEN] = "";
    coop_net_player_name(pid, who, sizeof who);
    LOG("[coop] dishgive: %s for player %d \"%s\"\n", dish->name, pid, who);
    return true;
}

static void dish_call(Cpu *cpu, uint64_t fn, uint64_t x0, uint64_t x1) {
    Cpu call = *cpu;
    call.x[0] = x0, call.x[1] = x1;
    guest_call(&call, fn);
}

static bool dish_read(uint64_t item, CoopDish *out) {
    if (!in_guest(item) || !in_guest(item + 0xa8) || LD32(item + 0x18) != 8 ||
        !LD8(item + 0x25) || LD32(item + 0x20) != 1) return false;
    uint64_t name = LD64(item + 0x30);
    const char *text = (const char *)(uintptr_t)name;
    if (!in_guest(name) || !in_guest(name + sizeof out->name) ||
        strncmp(text, "Item_Cook_", 10) || strnlen(text, sizeof out->name) == sizeof out->name) return false;

    *out = (CoopDish){0};
    snprintf(out->name, sizeof out->name, "%s", text);
    out->hearts = (int32_t)LD32(item + 0x80), out->seconds = (int32_t)LD32(item + 0x84);
    out->price = (int32_t)LD32(item + 0x88), out->level = LD32F(item + 0x90);
    float effect = LD32F(item + 0x8c);
    if (!isfinite(effect) || effect < -1 || effect > 32 || effect != truncf(effect)) return false;
    out->effect = (int32_t)effect;
    if (LD32(item + 0x98) != COOP_DISH_INGREDIENTS) return false;
    uint64_t ingredients = LD64(item + 0xa0);
    if (!in_guest(ingredients) || !in_guest(ingredients + 8 * COOP_DISH_INGREDIENTS)) return false;
    for (int i = 0; i < COOP_DISH_INGREDIENTS; i++) {
        uint64_t slot = LD64(ingredients + 8 * i);
        if (!in_guest(slot) || !in_guest(slot + 0x10)) return false;
        uint64_t str = LD64(slot + 8);
        if (!in_guest(str) || !in_guest(str + 64)) return false;
        const char *actor = (const char *)(uintptr_t)str;
        if (*actor && (strnlen(actor, 64) == 64 || !(out->ingredient[i] = items_ingredient_id(actor)))) return false;
    }
    return true;
}

// Pouch list, including multiple distinct meals made from the same recipe.
static int dish_scan(uint64_t mgr, const CoopDish *wanted, uint64_t *found) {
    uint64_t head = mgr + 0x68;
    int count = 0;
    uint64_t node = LD64(head + 8);
    for (int step = 0; step < 420 && in_guest(node) && node != head && count < COOP_DISH_MAX; step++, node = LD64(node + 8)) {
        CoopDish dish;
        uint64_t item = node - (int32_t)LD32(head + 0x14);
        if (!dish_read(item, &dish)) continue;
        if (wanted) {
            if (!memcmp(&dish, wanted, sizeof dish)) {
                *found = item;
                break;
            }
        } else if (items_dish_name(dish.name)) {
            CoopDishOffer check = {.from = 1, .to = 2, .id = 1, .area = 1, .to_pid = 1, .dish = dish};
            if (coop_dish_offer_ok(&check)) dish_list[count] = dish;
            else continue;
        } else continue;
        count++;
    }
    return count;
}

static void dish_recycle(Cpu *c, uint64_t mgr, uint64_t item) {
    if (LD64(mgr + 0x444f0) == item) ST64(mgr + 0x444f0, 0);
    if (LD64(mgr + 0x44488) == item) ST64(mgr + 0x44488, 0);
    dish_call(c, A_DISH_RECYCLE, mgr + 0x68, item);  // (the game's own removal order, FUN_7100d97f68)
    dish_call(c, A_DISH_REBUILD, mgr, mgr + 0x68);
    dish_call(c, A_DISH_REINDEX, mgr, 0);
    dish_call(c, A_DISH_SAVE, mgr, mgr + 0x68);
}

static bool dish_remove(Cpu *c, uint64_t mgr, const CoopDish *dish) {
    uint64_t item = 0;
    dish_call(c, A_DISH_LOCK, mgr + 0x28, 0);
    dish_scan(mgr, dish, &item);
    if (item) dish_recycle(c, mgr, item);
    dish_call(c, A_DISH_UNLOCK, mgr + 0x28, 0);
    return item != 0;
}

// The same dish, ingredients in any order (the game's cook data setter FUN_7100d92f34 sorts them; a dish read from a
// pouch is sorted already, so a given dish comes out identical).
static bool dish_same(const CoopDish *a, const CoopDish *b) {
    CoopDish x = *a, y = *b;
    memset(x.ingredient, 0, sizeof x.ingredient), memset(y.ingredient, 0, sizeof y.ingredient);
    if (memcmp(&x, &y, sizeof x)) return false;
    bool used[COOP_DISH_INGREDIENTS] = {0};
    for (int i = 0; i < COOP_DISH_INGREDIENTS; i++) {
        int j = 0;
        while (j < COOP_DISH_INGREDIENTS && (used[j] || b->ingredient[j] != a->ingredient[i])) j++;
        if (j == COOP_DISH_INGREDIENTS) return false;
        used[j] = true;
    }
    return true;
}

static bool dish_add(Cpu *c, uint64_t mgr, const CoopDish *dish) {
    if (!items_dish_name(dish->name)) return false;
    const char *names[COOP_DISH_INGREDIENTS];
    for (int i = 0; i < COOP_DISH_INGREDIENTS; i++) {
        names[i] = items_ingredient_name(dish->ingredient[i]);
        if (dish->ingredient[i] && !names[i]) return false;
    }
    if (!dish_mem) dish_mem = (uint64_t)(uintptr_t)hle_alloc(DISH_RECORD_SIZE);
    if (!dish_mem) return false;

    memset((void *)(uintptr_t)dish_mem, 0, DISH_RECORD_SIZE);
    dish_call(c, A_DISH_RECORD_INIT, dish_mem, 0);
    memcpy((void *)(uintptr_t)LD64(dish_mem + 8), dish->name, strlen(dish->name) + 1);
    for (int i = 0; i < COOP_DISH_INGREDIENTS; i++) {
        uint64_t str = LD64(dish_mem + 8 + 0x58 * (i + 1));
        if (names[i]) memcpy((void *)(uintptr_t)str, names[i], strlen(names[i]) + 1);
    }
    float hearts = (float)dish->hearts;
    memcpy((void *)(uintptr_t)(dish_mem + 0x210), &hearts, sizeof hearts);
    ST32(dish_mem + 0x214, dish->seconds);
    ST32(dish_mem + 0x218, dish->price);
    ST32(dish_mem + 0x21c, dish->effect);
    memcpy((void *)(uintptr_t)(dish_mem + 0x220), &dish->level, sizeof dish->level);

    dish_call(c, A_DISH_LOCK, mgr + 0x28, 0);
    // The native cook setter uses the last-added pointer. Clear it first so a
    // full pouch can never overwrite an older dish. The native mutex is recursive.
    uint64_t previous = LD64(mgr + 0x44488);
    ST64(mgr + 0x44488, 0);
    dish_call(c, A_DISH_ADD, mgr, dish_mem);
    uint64_t added = LD64(mgr + 0x44488);
    CoopDish readback;
    bool ok = dish_read(added, &readback) && dish_same(dish, &readback);
    if (!ok) {
        if (added && in_guest(added)) dish_recycle(c, mgr, added);
        ST64(mgr + 0x44488, previous);
        dish_call(c, A_DISH_REBUILD, mgr, mgr + 0x68);
    }
    dish_call(c, A_DISH_UNLOCK, mgr + 0x28, 0);
    return ok;
}

// origin: the player it comes from (the network checked its session and that it is meant for us).
static void dish_event(uint16_t type, const uint8_t *data, int len, int origin) {
    uint64_t us, them;
    int me = coop_net_my_id();
    if (origin < 0 || origin >= COOP_MAX_PLAYERS || !coop_net_dish_session(origin, &us, &them)) return;
    if (type == COOP_EV_DISH && len == sizeof(CoopDishOffer)) {
        CoopDishOffer offer;
        memcpy(&offer, data, sizeof offer);
        if (offer.to != us || offer.to_pid != me || offer.from_pid != origin || (them && offer.from != them) ||
            !coop_dish_offer_ok(&offer)) return;
        CoopDishResult *last = &dish_last_result[origin];
        if (last->id && last->to == offer.from) {
            if (offer.id == last->id) dish_reply[origin] = true;
            if (offer.id <= last->id) return;
        }
        // (one at a time: another giver's offer comes again in 3 s)
        if (!dish_in.id || (dish_in_pid == origin && dish_in.from != offer.from)) dish_in = offer, dish_in_pid = origin;
    } else if (type == COOP_EV_DISH_RESULT && len == sizeof(CoopDishResult)) {
        CoopDishResult result;
        memcpy(&result, data, sizeof result);
        if (!dish_out.id || dish_queued || origin != dish_out_pid || result.id != dish_out.id ||
            result.to != dish_out.from || result.to != us || result.to_pid != me || result.from_pid != origin ||
            (dish_out.to && result.from != dish_out.to) || (them && result.from != them) || !coop_dish_result_ok(&result))
            return;
        LOG("[coop] dishgive: partner %s %s (player %d)\n", result.accepted ? "took" : "refused", dish_out.dish.name, origin);
        if (result.accepted) {
            dish_out.id = 0;
            dish_status_set(DISH_SENT);
        } else {
            dish_returning = true;
            dish_status_set(DISH_RETURNING);
        }
    }
}

static void dish_frame(Cpu *c) {
    if (!local_in_world()) {
        dish_list_n = 0;
        __atomic_store_n(&dish_can_give, 0, __ATOMIC_RELEASE);
        return;
    }
    uint64_t now = plat_now_ns(), mgr = LD64(A_POUCH_MGR);
    if (!in_guest(mgr) || !in_guest(mgr + 0x44800)) return;
    bool can_change_pouch = !local_paused() && !spawn_hold();
    if (__atomic_load_n(&dish_dev_make_pending, __ATOMIC_ACQUIRE) && can_change_pouch) {
        bool ok = dish_add(c, mgr, &dish_dev_make);
        LOG("[coop] dishgive: dev make %s %s\n", dish_dev_make.name, ok ? "ok" : "FAILED");
        dish_next_scan = 0;
        __atomic_store_n(&dish_dev_make_pending, 0, __ATOMIC_RELEASE);
    }
    int dev = __atomic_exchange_n(&dish_dev_action, 0, __ATOMIC_ACQ_REL);
    if (coop_dish_enabled() && (dev || now >= dish_next_scan)) {
        dish_call(c, A_DISH_LOCK, mgr + 0x28, 0);
        dish_list_n = dish_scan(mgr, NULL, NULL);
        dish_call(c, A_DISH_UNLOCK, mgr + 0x28, 0);
        dish_next_scan = now + 500000000ull;
        while (__atomic_exchange_n(&dish_ui_lock, 1, __ATOMIC_ACQUIRE)) {}
        memcpy(dish_ui_list, dish_list, (size_t)dish_list_n * sizeof *dish_list);
        dish_ui_n = dish_list_n;
        __atomic_store_n(&dish_ui_lock, 0, __ATOMIC_RELEASE);
        if (dev == -1) {
            for (int i = 0; i < dish_list_n; i++) {
                const CoopDish *d = &dish_list[i];
                LOG("[coop] dishgive: pouch %d %s hearts=%d seconds=%d effect=%d level=%g price=%d ingredients=%08x,%08x,%08x,%08x,%08x\n",
                    i + 1, d->name, d->hearts, d->seconds, d->effect, d->level, d->price,
                    d->ingredient[0], d->ingredient[1], d->ingredient[2], d->ingredient[3], d->ingredient[4]);
            }
            LOG("[coop] dishgive: pouch total=%d\n", dish_list_n);
        } else if (dev > 0) {
            bool ok = dev <= dish_list_n && dish_start(&dish_list[dev - 1], -1);
            LOG("[coop] dishgive: dev give %d %s\n", dev, ok ? "queued" : "refused (no such dish, busy or partner not close)");
        }
    }
    if (__atomic_load_n(&dish_req_pending, __ATOMIC_ACQUIRE)) {
        CoopDish want = dish_req;
        __atomic_store_n(&dish_req_pending, 0, __ATOMIC_RELEASE);
        if (!dish_start(&want, -1)) dish_status_set(dish_out.id ? DISH_SENDING : DISH_TOO_FAR);
    }
    {
        char area[96];
        bool can = coop_dish_enabled() && !dish_out.id && dish_nearest(local_area(area, sizeof area), false) >= 0;
        __atomic_store_n(&dish_can_give, can, __ATOMIC_RELEASE);
    }

    int me = coop_net_my_id();
    uint64_t us, them;
    if (dish_in.id) {
        int g = dish_in_pid;
        if (g < 0 || !coop_net_dish_session(g, &us, &them) || dish_in.to != us || (them && dish_in.from != them)) {
            dish_in.id = 0;  // (that player left, or a new session: the giver offers again)
        } else {
            bool allowed = coop_dish_enabled() && dish_player_near(g, dish_in.area);
            if (!allowed || can_change_pouch) {
                bool accepted = allowed && dish_add(c, mgr, &dish_in.dish);
                dish_last_result[g] = (CoopDishResult){.from = us, .to = dish_in.from, .id = dish_in.id,
                                                       .accepted = accepted, .from_pid = (uint8_t)me, .to_pid = (uint8_t)g};
                dish_reply[g] = true;
                dish_in.id = 0;
                if (accepted) {
                    dish_next_scan = 0;
                    dish_status_set(DISH_RECEIVED);
                }
                LOG("[coop] dishgive: %s %s (from player %d)\n", accepted ? "received" : "declined", dish_in.dish.name, g);
            }
        }
    }
    for (int g = 0; g < COOP_MAX_PLAYERS; g++)
        if (dish_reply[g] && coop_net_send_event_to(g, COOP_EV_DISH_RESULT, &dish_last_result[g], sizeof dish_last_result[g]))
            dish_reply[g] = false;
    if (!dish_out.id) return;
    int g = dish_out_pid;
    bool same_peer = g >= 0 && coop_net_dish_session(g, &us, &them) && dish_out.from == us &&
                     (!dish_out.to || !them || dish_out.to == them);
    if (dish_queued && can_change_pouch) {
        dish_queued = false;
        if (!same_peer || !dish_player_near(g, dish_out.area)) {
            dish_out.id = 0;
            dish_status_set(DISH_TOO_FAR);
            return;
        }
        if (!dish_remove(c, mgr, &dish_out.dish)) {
            dish_out.id = 0;
            dish_status_set(DISH_CHANGED);
            return;
        }
        LOG("[coop] dishgive: removed %s to give\n", dish_out.dish.name);
        dish_next_scan = dish_next_send = 0;
        dish_status_set(DISH_SENDING);
    }
    if (dish_returning) {
        if (can_change_pouch && dish_add(c, mgr, &dish_out.dish)) {
            LOG("[coop] dishgive: %s back in our pouch\n", dish_out.dish.name);
            dish_out.id = 0, dish_returning = false, dish_next_scan = 0;
            dish_status_set(DISH_RETURNED);
        }
    } else if (!dish_queued && same_peer && now >= dish_next_send &&
               coop_net_send_event_to(g, COOP_EV_DISH, &dish_out, sizeof dish_out)) {
        // The reliable link retries packets. Re-offer the same id as well so a
        // link queue reset can recover while the two session identities survive.
        dish_next_send = now + 3000000000ull;
    }
}

RT_ON_RESET(coop_dish_reset) {
    dish_list_n = 0, dish_next_scan = 0, dish_mem = 0;
    dish_out.id = dish_in.id = 0;
    dish_out_pid = dish_in_pid = -1;
    memset(dish_last_result, 0, sizeof dish_last_result);
    memset(dish_reply, 0, sizeof dish_reply);
    dish_queued = dish_returning = false;
    __atomic_store_n(&dish_dev_action, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&dish_req_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&dish_dev_make_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&dish_can_give, 0, __ATOMIC_RELEASE);
    dish_ui_n = 0;
    dish_status_set(DISH_IDLE);
}
