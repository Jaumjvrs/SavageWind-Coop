// Part of runtime/coop.c (included there, same translation unit): Captured Memories (2026-11-14).
// The 12 photo spots + the 13th are map actors "RememberTag" (Attention "Remind": the player stands on the spot and
// presses A). The tag starts the flow RememberTag, entry "Talk_<UniqueName>" (Talk_Demo126_0 .. Talk_Demo138_0):
//   Demo_CallDemo Demo309_2 / Demo<NNN>_309 (the memory: Link remembers, the movie Demo<NNN>_0 plays, the photo shows;
//   for the 13th, Demo309_1 = the repaired photo, flags PictureMemory_Finish + IsRepair_MemoryPhoto_013), then
//   IsPlayed_Demo<NNN>_0 on, and for the 12 photo memories (not 138) PictureMemory_Spot_Int - 1 (12 = none found),
//   PictureMemory_First1 when it reaches 11, PictureMemory_Find11 when it reaches 0. Playing the movie also sets
//   the saved flag Demo<NNN>_0.
// Co-op:
// - The whole RememberTag entry is a shared story event (story.inc EV_STORY + the cutscene wait): when one player
//   starts a memory with the partner within the story range (50 m, same area), the partner's game runs the same flow
//   (started from its own copy of the tag when loaded, else from its Link), so both watch it together and each
//   game's own flow sets its flags and its count. A partner who already has that memory says no (the flow would
//   count it twice), and the first game goes on alone.
// - The cutscenes called inside (Demo309_2 memory entries, Demo309_1, the movies Demo126_0..Demo138_0) are never
//   sent on their own: the outer flow runs in both games.
// - The memory flags are shared progress (world.inc named_story via memories_named): a partner within the share
//   radius of the spot gets IsPlayed_Demo<NNN>_0 / Demo<NNN>_0 even if it could not watch (busy in a talk); a partner
//   far away gets nothing. memories_frame then lowers PictureMemory_Spot_Int to 12 - (photo memories found) and sets
//   First1 / Find11 like the flow does (only lowers, only while no event runs, after two equal readings).
// The Champions' scenes in and after the Divine Beasts (Demo110-125) are covered by beast.inc / the normal cutscene
// sharing; Impa's scenes (Demo309_0) and Purah's photos (Demo032_0) by the normal cutscene sharing + quest flags.
// SAVAGEWIND_COOP_MEMORIES=0 turns this off (the old behaviour: only the called Demo309_2 scene is sent).
static bool memories_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_MEMORIES") && !strcmp(getenv("SAVAGEWIND_COOP_MEMORIES"), "0"));
    return on;
}
// The 13 RememberTag actors in MainField (UniqueName Demo<NNN>_0, map HashId; read from the map units).
static const struct { int demo; uint32_t hashid; } memory_tags[] = {
    {126, 3195630366u}, {127, 1369663485u}, {128, 1771250467u}, {129, 2321729495u}, {130, 2470196119u},
    {131, 1510403310u}, {132, 2330923800u}, {133, 2451285445u}, {134, 1758862945u}, {135, 2349158707u},
    {136, 4247579235u}, {137, 3653782520u}, {138, 3321613302u},
};
// "Talk_Demo<NNN>_0" -> NNN (126..138), else 0.
static int memory_entry_demo(const char *entry) {
    int n = 0;
    char tail = 0;
    if (strncmp(entry, "Talk_Demo", 9) || strlen(entry) != 14 || sscanf(entry + 9, "%3d_%c", &n, &tail) != 2 ||
        tail != '0' || n < 126 || n > 138)
        return 0;
    return n;
}
// story.inc: 0 not a memory event (the usual rules), 1 shared, 3 solo (not sent / not started for the partner).
static int memories_story(const char *flow, const char *entry) {
    if (!memories_on()) return 0;
    if (!strcmp(flow, "RememberTag")) {
        int n = memory_entry_demo(entry);
        if (!n) return 3;
        char f[32];
        snprintf(f, sizeof f, "IsPlayed_Demo%d_0", n);
        return gdt_get_bool(f) == 1 ? 3 : 1;  // (already found here: a second run would count it twice)
    }
    if (!strcmp(flow, "Demo309_2")) {  // its memory entries Demo<NNN>_309 (HerosMemory* stay as before)
        int n = 0;
        if (sscanf(entry, "Demo%d_309", &n) == 1 && n >= 126 && n <= 138) return 3;
        return 0;
    }
    if (!strncmp(flow, "Demo309_1", 9)) return 3;
    int n = 0;
    char tail = 0;
    if (strlen(flow) == 9 && sscanf(flow, "Demo%3d_%c", &n, &tail) == 2 && tail == '0' && n >= 126 && n <= 138)
        return 3;  // (the movies)
    return 0;
}
static bool memory_tag_name(const char *n) { return !strcmp(n, "RememberTag"); }
// story.inc: the actor to start the partner's memory event with: our copy of the same RememberTag (the flow's
// "Starter" is the tag in the original), else def (our Link).
static uint64_t memories_actor(Cpu *c, const char *flow, const char *entry, uint64_t def) {
    // (SAVAGEWIND_COOP_MEMORYTAG=0: always from our Link. Not known yet which works: the tag's actor link has
    // AppendUniqueNameToEntryPointName, unknown whether the event manager appends it again to a named start; a start
    // that fails with the tag is retried from our Link in story.inc)
    static int tag = -1;
    if (tag < 0) tag = !(getenv("SAVAGEWIND_COOP_MEMORYTAG") && !strcmp(getenv("SAVAGEWIND_COOP_MEMORYTAG"), "0"));
    if (!memories_on() || !tag || strcmp(flow, "RememberTag")) return def;
    int n = memory_entry_demo(entry);
    for (size_t i = 0; n && i < sizeof memory_tags / sizeof *memory_tags; i++) {
        if (memory_tags[i].demo != n) continue;
        struct HashHit hit = {memory_tags[i].hashid, 0};
        walk_actors(c, memory_tag_name, find_hashid, &hit);
        static int logged;  // (once per memory: a failed start is retried every frame)
        if (logged != n * 2 + (hit.a != 0))
            LOG("[coop] memories: Talk_Demo%d_0 starts from %s\n", n, hit.a ? "our copy of the tag" : "our Link (tag not loaded)");
        logged = n * 2 + (hit.a != 0);
        return hit.a ? hit.a : def;
    }
    return def;
}
// Shared progress flags (world.inc named_story).
static void memories_named(void) {
    if (!memories_on()) return;
    char s[48];
    for (int n = 126; n <= 138; n++) {
        snprintf(s, sizeof s, "IsPlayed_Demo%d_0", n), named_add(s, 0, UINT64_MAX);
        snprintf(s, sizeof s, "Demo%d_0", n), named_add(s, 0, UINT64_MAX);
    }
    static const char *const more[] = {"IsPlayed_Demo309_0", "IsPlayed_Demo309_1", "IsRepair_MemoryPhoto_013"};
    for (size_t i = 0; i < sizeof more / sizeof *more; i++) named_add(more[i], 0, UINT64_MAX);
}
// Once a second: the remaining-memories count follows the photo memories found (a flag taken from the partner),
// and the memory flags' changes are logged (the play-test log shows what happened).
static void memories_frame(Cpu *c) {
    if (!memories_on()) return;
    static uint64_t next;
    uint64_t now = plat_now_ns();
    if (now < next || !local_in_world()) return;
    next = now + 1000000000ull;
    static signed char was[14];
    static int32_t was_spot = -999;
    static int stable_want = -1;
    int found = 0;
    for (int n = 126; n <= 138; n++) {
        char f[32];
        snprintf(f, sizeof f, "IsPlayed_Demo%d_0", n);
        int v = gdt_get_bool(f);
        if (v < 0) return;  // (no game data yet)
        if (was[n - 126] != 0 && v != was[n - 126] - 1) LOG("[coop] memories: %s %d -> %d\n", f, was[n - 126] - 1, v);
        was[n - 126] = (signed char)(v + 1);
        if (v == 1 && n <= 137) found++;
    }
    int32_t spot;
    if (!gdt_get_s32("PictureMemory_Spot_Int", &spot)) return;
    if (spot != was_spot) {
        if (was_spot != -999) LOG("[coop] memories: PictureMemory_Spot_Int %d -> %d (%d found)\n", was_spot, spot, found);
        was_spot = spot;
    }
    int want = 12 - found;
    if (event_running() || spot <= want || spot > 12) { stable_want = -1; return; }
    if (stable_want != want) { stable_want = want; return; }  // (seen once: act on the next reading)
    stable_want = -1;
    if (gdt_set_s32(c, "PictureMemory_Spot_Int", want)) {
        LOG("[coop] memories: PictureMemory_Spot_Int %d -> %d (a memory taken from the other player)\n", spot, want);
        was_spot = want;
    }
    if (want <= 11 && gdt_get_bool("PictureMemory_First1") == 0) gdt_set_bool(c, "PictureMemory_First1", true);
    if (want == 0 && gdt_get_bool("PictureMemory_Find11") == 0) gdt_set_bool(c, "PictureMemory_Find11", true);
}
