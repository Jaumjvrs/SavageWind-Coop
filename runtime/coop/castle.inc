// Part of runtime/coop.c (included there, same translation unit): Hyrule Castle, the final boss and the ending
// (2026-11-14).
// Everything happens in MainField. The game's flow "HyruleCastle" drives it from three map event tags:
//   BossRoom0      entering the Sanctum (-254 294.8 -1033): Blight re-fights (one per Divine Beast not freed), then
//                  Calamity Ganon (Enemy_Ganon, arena floor y~191 under the Sanctum);
//   GanonDead      Ganon's death: Demo145_0, then Zelda gives the Light Bow (Demo146_x) and the horse fight with Dark
//                  Beast Ganon (Enemy_GanonBeast, field south of the castle around -254 125 560);
//   BeastGanonDead the Dark Beast's death: Demo147_0, the ending Demo148_0, the save star (flag GameClear) and the
//                  return to the title screen.
// Every cutscene in between is called by that flow itself (Demo_CallDemo), so:
// - the three entries are shared like other cutscenes (story.inc, EV_STORY): when one starts in one game, the
//   partner's game starts the same entry when its player is near enough (castle_story_range), and the first game
//   waits for it, so both flows run side by side (the flow's own warps pull the partner into the room);
// - the cutscenes inside are "own-only": both games' flows call them, the story wait keeps them in step, but a game
//   never starts one on its own (it waits up to CASTLE_OWN_WAIT for its own flow, then answers "not started");
// - Demo146_x (which one depends on each player's horse) and Demo139_0 (each player's memories) stay solo.
// Shared progress flags: the four Blight re-fight "dead" flags and their intro flags, the cocoon scene flag. Not the
// boss generate flags, IsGanonLiving or GameClear: each game's own flow sets those.
// Items stay each player's own: the Light Bow (Weapon_Bow_071, its map flag is not shared; each gets one from Zelda)
// and the Master Sword (Weapon_Sword_070) in the castle zones are never shared as pick-ups.
// The two bosses are shared enemies like any other (enemies.inc: one owner, the nearer player; the follower copies
// its life), only the table reaches farther for them (castle_enemy_radius).
// SAVAGEWIND_COOP_CASTLE=0 turns all of this off.
static bool castle_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_CASTLE") && !strcmp(getenv("SAVAGEWIND_COOP_CASTLE"), "0"));
    return on;
}
// 1: the Sanctum and Ganon's arena below it; 2: the Dark Beast's field; 0: elsewhere (horizontal distance only).
static int castle_zone(const float p[3]) {
    float sx = p[0] + 254.0f, sz = p[2] + 1061.0f, bx = p[0] + 254.0f, bz = p[2] - 560.0f;
    if (sx * sx + sz * sz < 90.0f * 90.0f) return 1;
    if (bx * bx + bz * bz < 650.0f * 650.0f) return 2;
    return 0;
}
// Both players in the same castle zone: any story flag from the partner is taken there (gate.inc whole_castle).
static bool castle_whole(const float a[3], const float b[3]) {
    if (!castle_on()) return false;
    int za = castle_zone(a);
    return za && za == castle_zone(b);
}
static float castle_enemy_radius(const char *name, float def) {
    if (!castle_on()) return def;
    if (!strcmp(name, "Enemy_GanonBeast")) return 700.0f;  // (it roams the whole field: TraverseRadius 505 m)
    if (!strcmp(name, "Enemy_Ganon")) return 250.0f;
    size_t n = strlen(name);
    if (!strncmp(name, "Enemy_SiteBoss_", 15) && n > 7 && !strcmp(name + n - 7, "_Castle")) return 250.0f;
    return def;
}
// A placed actor's map flag that stays each player's own (gate.inc named_seen).
static bool castle_flag_private(const char *actor) { return castle_on() && !strcmp(actor, "Weapon_Bow_071"); }
// A pouch gain that is not shared even while an acquisition is open (gate.inc world_frame).
static bool castle_item_own(const char *item) {
    if (!castle_on()) return false;
    if (!strcmp(item, "Weapon_Bow_071")) return true;
    return !strcmp(item, "Weapon_Sword_070") && castle_zone((const float *)(uintptr_t)(player_info_ptr() + 0x88)) != 0;
}
// Shared progress flags (world.inc named_story).
static void castle_named(void) {
    if (!castle_on()) return;
    static const char *const names[] = {
        "IsHyruleCastlePGanonWindDead", "IsHyruleCastlePGanonWaterDead",   "IsHyruleCastlePGanonFireDead",
        "IsHyruleCastlePGanonElectricDead", "IsPlayed_Demo141_0", "IsPlayed_Demo141_1", "IsPlayed_Demo141_2",
        "IsPlayed_Demo141_3", "IsPlayed_Demo142_0",
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) named_add(names[i], 0, UINT64_MAX);
}
// story.inc: 0 not a castle event (the usual rules), 1 shared (a castle flow entry started for the partner too),
// 2 own-only (shared and waited for, never started on its own), 3 solo (not shared).

static int castle_story(const char *flow, const char *entry) {
    if (!castle_on()) return 0;
    if (!strcmp(flow, "HyruleCastle"))
        return !strcmp(entry, "BossRoom0") || !strcmp(entry, "GanonDead") || !strcmp(entry, "BeastGanonDead") ? 1 : 0;
    if (!strncmp(flow, "Demo146_", 8) || !strncmp(flow, "Demo139_0", 9)) return 3;
    static const char *const own[] = {"Demo141_", "Demo142_", "Demo143_", "Demo145_", "Demo147_", "Demo148_", "Demo720_"};
    for (size_t i = 0; i < sizeof own / sizeof *own; i++)
        if (!strncmp(flow, own[i], strlen(own[i]))) return 2;
    return 0;
}
static float castle_story_range(const char *flow, const char *entry, float def) {
    if (!castle_on()) return def;
    if (!strcmp(flow, "HyruleCastle"))
        return !strcmp(entry, "BossRoom0") ? 400.0f : !strcmp(entry, "GanonDead") ? 300.0f
               : !strcmp(entry, "BeastGanonDead") ? 1500.0f : def;
    if (castle_story(flow, entry) == 2) return 1500.0f;
    // a Blight's death in the Sanctum (its scene calls the next re-fight)
    if (!strncmp(flow, "Demo712_", 8) && !strcmp(entry, "Die_Castle")) return 300.0f;
    return def;
}
// (2026-11-14, found by test_castle: the guest died to Ganon, reloaded before the fight, the host killed Ganon; the
// guest's game declined GanonDead "our Link is dead" and stayed before the fight for good.) A boss-death entry the
// partner ran while we could not start it (dead, loading, busy) is remembered and started here later, once we are back
// in the world with no event running, if our own game has not done that step itself. Off with SAVAGEWIND_COOP_CASTLE=0.
static struct { char entry[24]; uint64_t at; int tries; } castle_owed;
static void castle_missed(const char *flow, const char *entry, const char *why) {
    if (!castle_on() || strcmp(flow, "HyruleCastle")) return;
    if (strcmp(entry, "GanonDead") && strcmp(entry, "BeastGanonDead")) return;
    // (a player far away when it happened did not take part: no catch-up, the shared-story rule)
    if (!strcmp(why, "too far") || !strcmp(why, "it ran here already")) return;
    snprintf(castle_owed.entry, sizeof castle_owed.entry, "%s", entry);
    castle_owed.at = plat_now_ns(), castle_owed.tries = 0;
    LOG("[coop] castle: the other player ran %s while we could not (%s): we start it when we are back\n", entry, why);
}
static void castle_catch_up(Cpu *c, uint64_t now) {
    if (!castle_owed.entry[0] || now - castle_owed.at < 3000000000ull) return;
    // done by our own game meanwhile? (GanonDead sets up the Dark Beast; BeastGanonDead ends Ganon)
    bool ganon = !strcmp(castle_owed.entry, "GanonDead");
    if ((ganon && (gdt_get_bool("IsPlayed_Demo145_0") > 0 || gdt_get_bool("LastBossGanonBeastGenerateFlag") > 0)) ||
        (!ganon && gdt_get_bool("IsGanonLiving") == 0)) {
        LOG("[coop] castle: %s happened here by itself\n", castle_owed.entry);
        castle_owed.entry[0] = 0;
        return;
    }
    uint64_t mgr = g_site.event_mgr ? LD64(g_site.event_mgr) : 0, ev = in_guest(mgr) ? LD64(mgr + 0x1d2b8) : 0;
    if (in_guest(ev)) return;  // (an event runs: the death scene, the reload's arrival scene...)
    castle_owed.at = now;      // (next try in 3 s)
    uint64_t pa = LD64(player_info_ptr() + 0x60);
    bool ok = event_start(c, pa, "HyruleCastle", castle_owed.entry);
    LOG("[coop] castle: catching up on the other player's %s: %s\n", castle_owed.entry, ok ? "started" : "refused");
    if (ok || ++castle_owed.tries >= 5) castle_owed.entry[0] = 0;
}
// Logs the castle's progress flags when they change (the test reads these lines).
static void castle_frame(Cpu *c) {
    if (!castle_on()) return;
    if (local_in_world()) castle_catch_up(c, plat_now_ns());
    static uint64_t next;
    uint64_t now = plat_now_ns();
    if (now < next || !local_in_world()) return;
    next = now + 1000000000ull;
    static const char *const watch[] = {
        "FirstInHyruleCastleBossRoom", "IsHyruleCastlePGanonWindDead", "IsHyruleCastlePGanonWaterDead",
        "IsHyruleCastlePGanonFireDead", "IsHyruleCastlePGanonElectricDead", "LastBossIncompleteGanonGenerateFlag",
        "IsPlayed_Demo145_0", "LastBossGanonBeastGenerateFlag", "IsPlayed_Demo147_0", "IsGanonLiving", "GameClear",
    };
    static signed char was[sizeof watch / sizeof *watch];
    static bool init;
    for (size_t i = 0; i < sizeof watch / sizeof *watch; i++) {
        int v = gdt_get_bool(watch[i]);
        if (init && v != was[i]) LOG("[coop] castle: %s %d -> %d\n", watch[i], was[i], v);
        was[i] = (signed char)v;
    }
    if (!init) {
        init = true;
        LOG("[coop] castle: start FirstIn %d Ganon %d BeastGen %d Living %d GameClear %d\n", was[0], was[5], was[7],
            was[9], was[10]);
    }
}
