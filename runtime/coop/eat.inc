// Part of runtime/coop.c (included from coop/astree.inc): eating / drinking from the inventory shown on the avatar
// (protocol 81, 2026-11-19, audit A1: the partner only saw the Slate menu pose while a player ate in a fight).
// In the pause menu the game shows its own Link, the actor "PauseMenuPlayer" (Actor/Pack/PauseMenuPlayer.sbactorpack);
// using food / an elixir / a fairy plays its AS "UseItem", a UseItemSelector (AS input kind 62, a string) with the cases
// EatFood, EatCookingDish, EatCookingSkewer, EatBizarre, EatTooHard, Drink, UseFairy (clips Eat_OneHand_R, Eat_Normal,
// Eat_BBQ, Eat_LookBad, Eat_Hard, Eat_Drink, Eat_Fairy of Player_Animation; FaceUseItem the same cases for the face,
// Face_Eat_*). Those AS are not in the player's 399 world AS (the tree name table), so:
// - sender: while paused, find the menu Link (actor scan, cached), and while one of its layers plays UseItem send the
//   case (kind 62) and a use counter in the tree (CoopAsTree eat / eat_seq);
// - receiver: on a new use the avatar and its armor puppets play CoopEat<case> in 0.0 + 1.0 (overlay single-clip AS,
//   build_overlay.py COOP_CLIPS) and CoopXFaceEat<case> in the face slot 3.0 (overlay: the FaceUseItem branch of that
//   case), until the clip ends or the sender's menu Link stops eating; then the normal tree (menu pose) again.
// Healing and the inventory stay local (nothing else is sent). SAVAGEWIND_COOP_EAT=0: off (both sides).

static const char *const eat_case[COOP_EAT_MAX + 1] = {"", "EatFood", "EatCookingDish", "EatCookingSkewer", "EatBizarre",
                                                       "EatTooHard", "Drink", "UseFairy"};
static const char *const eat_body[COOP_EAT_MAX + 1] = {"", "CoopEatFood", "CoopEatDish", "CoopEatSkewer", "CoopEatBizarre",
                                                       "CoopEatHard", "CoopEatDrink", "CoopEatFairy"};
static const char *const eat_face[COOP_EAT_MAX + 1] = {"", "CoopXFaceEatFood", "CoopXFaceEatDish", "CoopXFaceEatSkewer",
                                                       "CoopXFaceEatBizarre", "CoopXFaceEatHard", "CoopXFaceEatDrink",
                                                       "CoopXFaceEatFairy"};
#define EAT_KIND 62  // (UseItemSelector's input kind)

static bool eat_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_EAT"), LOG("[coop] eat: eating / drinking from the menu shown on the avatar: %s\n", on ? "on" : "off");
    return on;
}

// ---- sender
static struct {
    uint64_t actor, unit, scan_ns;
    uint32_t id;
    uint8_t seq, kind;
    bool eating, said_found, said_none;
    float frame;
    char layers[160];
    unsigned diag;
} eat_tx;
static bool eat_menu_name(const char *n) { return !strcmp(n, "PauseMenuPlayer"); }
static void eat_menu_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)name, (void)ctx;
    eat_tx.actor = a, eat_tx.unit = LD64(a + PROC_UNIT), eat_tx.id = LD32(a + PROC_ID);
}
static bool eat_menu_alive(void) {
    uint64_t a = eat_tx.actor;
    return in_guest(a) && in_guest(eat_tx.unit) && LD32(eat_tx.unit + UNIT_ID) == eat_tx.id &&
           LD64(eat_tx.unit + UNIT_ACTOR) == a && LD8(a + PROC_STATE) != 3;
}

// `coop look eat <case> [ms]` (dev, tests): send that case as a new use for a while, as if the menu Link ate.
static struct { volatile int go; volatile uint8_t kind; volatile uint64_t until_ns; } eat_dev;

// Each frame on the sender, after the tree was built (paused or not): fills t->eat / t->eat_seq.
static void eat_publish(Cpu *c, CoopAsTree *t, bool paused) {
    t->eat = 0, t->eat_seq = eat_tx.seq;
    if (!eat_on()) return;
    if (eat_dev.go) eat_dev.go = 0, eat_tx.seq++, LOG("[coop] eat: dev use %s (use %u)\n", eat_case[eat_dev.kind], eat_tx.seq);
    if (eat_dev.kind && plat_now_ns() < eat_dev.until_ns) {
        t->eat = eat_dev.kind, t->eat_seq = eat_tx.seq;
        return;
    }
    if (!paused) {
        if (eat_tx.eating) LOG("[coop] eat: menu closed\n");
        eat_tx.eating = false;
        return;
    }
    uint64_t now = plat_now_ns();
    if (!eat_menu_alive()) {
        eat_tx.actor = 0;
        if (now < eat_tx.scan_ns) return;
        eat_tx.scan_ns = now + 500000000ull;
        walk_actors(c, eat_menu_name, eat_menu_seen, NULL);
        if (!eat_menu_alive()) {
            eat_tx.actor = 0;
            if (!eat_tx.said_none) eat_tx.said_none = true, LOG("[coop] eat: no PauseMenuPlayer actor found (paused)\n");
            return;
        }
        if (!eat_tx.said_found) eat_tx.said_found = true, LOG("[coop] eat: menu Link found (%llx)\n", (unsigned long long)eat_tx.actor);
    }
    uint64_t a = eat_tx.actor, use = 0;
    char layers[160];
    size_t n = 0;
    layers[0] = 0;
    for (int s = 0; s < COOP_AS_SLOTS; s++)
        for (int l = 0; l < COOP_AS_LAYERS; l++) {
            uint64_t rec = as_layer_rec(a, s, l);
            const char *nm = as_layer_name(rec);
            if (!nm[0]) continue;
            if (n + 40 < sizeof layers) n += (size_t)snprintf(layers + n, sizeof layers - n, " %d.%d=%.28s", s, l, nm);
            if (!use && !strcmp(nm, "UseItem")) use = rec;
        }
    const char *cs = asin_str(asin_slot(asin_holder(a), EAT_KIND));
    if (strcmp(layers, eat_tx.layers) && (eat_tx.diag++ < 80 || coop_look_log())) {
        LOG("[coop] eat: menu Link plays%s (kind %d '%s')\n", layers[0] ? layers : " nothing", EAT_KIND, cs);
        snprintf(eat_tx.layers, sizeof eat_tx.layers, "%s", layers);
    }
    if (!use) {
        if (eat_tx.eating) LOG("[coop] eat: menu Link stopped eating\n");
        eat_tx.eating = false;
        return;
    }
    int kind = 0;
    for (int k = 1; k <= COOP_EAT_MAX; k++)
        if (!strcmp(cs, eat_case[k])) kind = k;
    if (!kind) kind = COOP_EAT_FOOD;  // (the selector's "default" case is EatFood's branch)
    float fr = -1;
    uint64_t tm = as_layer_time(c, use);
    if (tm) memcpy(&fr, (void *)(uintptr_t)(tm + 4), 4);
    if (!isfinite(fr)) fr = -1;
    // a new use: UseItem started, another case, or the same AS started again (its frame back near the start)
    if (!eat_tx.eating || kind != eat_tx.kind || (fr >= 0 && eat_tx.frame > fr + 2)) {
        eat_tx.seq++;
        LOG("[coop] eat: menu Link %s (%s, use %u)\n", eat_case[kind], cs[0] ? "case read" : "case not readable", eat_tx.seq);
    }
    eat_tx.eating = true, eat_tx.kind = (uint8_t)kind, eat_tx.frame = fr;
    t->eat = (uint8_t)kind, t->eat_seq = eat_tx.seq;
    astree_pad_until_ns = 0;  // (no Slate in the hand while eating: avatar.inc gear)
}

// ---- receiver (state in astree.eat: per avatar, coop/multi.inc swaps astree)
// Each frame while the avatar plays the trees, before the normal play loop: true while the eat animation runs (the
// loop is skipped); *resync is set on the frame it ends (every layer played again: the menu pose comes back).
static bool eat_frame(Cpu *c, const CoopAsTree *t, bool *resync) {
    if (!eat_on()) return false;
    uint64_t now = plat_now_ns();
    if (t->eat && t->eat <= COOP_EAT_MAX && (!astree.eat.had || t->eat_seq != astree.eat.seq)) {
        astree.eat.had = true, astree.eat.seq = t->eat_seq, astree.eat.kind = t->eat;
        astree.eat.active = true, astree.eat.start_ns = now;
        astree.rx = *t, astree.have = 1;
        for (int k = 0; k < ASTREE_TARGETS; k++)
            if (astree.targets[k]) astree_write(astree.targets[k], t);
        const char *b = eat_body[t->eat];
        astree_play_all(c, b, 0, 0);
        astree_play_all(c, b, 1, 0);
        astree_play_all(c, NULL, 1, 1);
        astree_play_all(c, eat_face[t->eat], 3, 0);
        const char *now0 = astree.targets[0] ? as_layer_name(as_layer_rec(astree.targets[0], 0, 0)) : "";
        LOG("[coop] eat: the other player %s: avatar plays %s + %s (use %u; 0.0 now '%s')\n", eat_case[t->eat], b,
            eat_face[t->eat], t->eat_seq, now0);
        return true;
    }
    if (!astree.eat.active) return false;
    astree.rx = *t, astree.have = 1;
    for (int k = 0; k < ASTREE_TARGETS; k++)
        if (astree.targets[k]) astree_write(astree.targets[k], t);
    uint64_t el = now - astree.eat.start_ns;
    const char *n0 = astree.targets[0] ? as_layer_name(as_layer_rec(astree.targets[0], 0, 0)) : "";
    const char *why = NULL;
    if (el > 200000000ull && strcmp(n0, eat_body[astree.eat.kind])) why = n0[0] ? "replaced" : "clip over";
    else if (!t->eat && el > 300000000ull) why = "the other player's menu Link stopped";
    else if (el > 8000000000ull) why = "8 s";
    if (!why) {
        astree_phase_lock(c);  // (armor puppets in step with the avatar's clip)
        return true;
    }
    astree.eat.active = false;
    *resync = true;
    LOG("[coop] eat: avatar's %s over after %.1f s (%s): back to the tree\n", eat_body[astree.eat.kind], el / 1e9, why);
    return false;
}
