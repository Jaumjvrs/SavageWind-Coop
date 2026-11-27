// Part of runtime/coop.c (included there, same translation unit): house doors the other player opens open here too.
// (2026-11-21, 11-20 evening play: "the other Link goes right through the doors, the door they open stays shut in my
// game".) Facts (map data + Ghidra, 2026-11-21): village doors are TwnObj_*Door*
// actors (Kakariko TwnObj_Village_SheikerDoor_A_01: AI SlideDoor, AS "Slide" / "Close_Slide"; Hateno push doors: AI
// Door, AS "Push_L" / "Push_R" / "Close_L" / "Close_R", auto close after 135 frames; double doors: AI GatefoldDoor, AS
// "In" / "InClose" / "Out" / "OutClose"). The player opens one with a Door* AS (DoorSlideOpenL/R, DoorPushOpenL/R,
// DoorPushDouble, DoorPullOpen) while the door's AI plays its own open AS.
// Here: when an avatar starts a Door* AS (it plays the other player's own AS: tree mode), the nearest TwnObj_*Door*
// within DOOR_NEAR_M plays its open AS in our game, and its close AS DOOR_OPEN_NS later. Only the look: the door's
// AI and collision stay as they are (our own Link still opens it normally). SAVAGEWIND_COOP_DOORS=0: off.
#define DOOR_NEAR_M 3.0f
#define DOOR_OPEN_NS 3500000000ull
#define DOOR_MAX 4
static struct { uint64_t actor; unsigned id; uint64_t close_ns; char close_as[16]; } doors[DOOR_MAX];
static bool doors_on(void) {
    static int c_ = -1;
    return env_on("SAVAGEWIND_COOP_DOORS", &c_);
}
static bool door_name(const char *n) { return !strncmp(n, "TwnObj_", 7) && strstr(n, "Door"); }
struct DoorFind { float p[3], best; uint64_t a; char name[48]; };
static void door_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c;
    struct DoorFind *f = ctx;
    float dx = LDF32(a + ACTOR_MTX + 12) - f->p[0], dy = LDF32(a + ACTOR_MTX + 28) - f->p[1],
          dz = LDF32(a + ACTOR_MTX + 44) - f->p[2];
    float d = dx * dx + dy * dy + dz * dz;
    if (d < f->best) f->best = d, f->a = a, snprintf(f->name, sizeof f->name, "%s", name);
}
static void door_alive(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c, (void)name;
    struct DoorFind *f = ctx;
    if (a == f->a) f->best = 1;
}
// Every frame for each avatar (multi_run): open the door it opens; close the doors whose time is up.
static void doors_open(Cpu *c, const float at[3], const char *as, uint64_t now);
static void doors_frame(Cpu *c, uint64_t avatar_actor) {
    if (!doors_on()) return;
    uint64_t now = plat_now_ns();
    for (int i = 0; i < DOOR_MAX; i++) {
        if (!doors[i].actor || now < doors[i].close_ns) continue;
        struct DoorFind live = {{0, 0, 0}, 0, doors[i].actor, ""};  // (still a live door actor? walk_actors checks)
        walk_actors(c, door_name, door_alive, &live);
        if (live.best > 0 && LD32(doors[i].actor + PROC_ID) == doors[i].id) play_anim(c, doors[i].actor, doors[i].close_as, 0);
        LOG("[coop] doors: closed again (%s)\n", doors[i].close_as);
        doors[i].actor = 0;
    }
    if (!avatar_actor || !in_guest(avatar_actor)) return;
    const char *as = as_layer_name(as_layer_rec(avatar_actor, 0, 0));
    for (int l = 1; l < 3 && strncmp(as, "Door", 4); l++) {  // (the tree may cross-fade into another layer)
        const char *n = as_layer_name(as_layer_rec(avatar_actor, 0, l));
        if (!strncmp(n, "Door", 4)) as = n;
    }
    static struct { uint64_t avatar; char as[32]; } seen[8];  // (per avatar: multi.inc runs this for each player)
    int si = 0;
    while (si < 7 && seen[si].avatar && seen[si].avatar != avatar_actor) si++;
    bool fresh = seen[si].avatar != avatar_actor || strncmp(as, seen[si].as, sizeof seen[si].as - 1);
    seen[si].avatar = avatar_actor;
    snprintf(seen[si].as, sizeof seen[si].as, "%s", as);
    if (!fresh || strncmp(as, "Door", 4)) return;
    float at[3] = {LDF32(avatar_actor + ACTOR_MTX + 12), LDF32(avatar_actor + ACTOR_MTX + 28), LDF32(avatar_actor + ACTOR_MTX + 44)};
    doors_open(c, at, as, now);
}
// Open the nearest door within DOOR_NEAR_M of `at` the way a Link playing `as` opens it (also `coop door <AS>`, dev).
static void doors_open(Cpu *c, const float at[3], const char *as, uint64_t now) {
    struct DoorFind f = {{at[0], at[1], at[2]}, DOOR_NEAR_M * DOOR_NEAR_M, 0, ""};
    walk_actors(c, door_name, door_seen, &f);
    if (!f.a) {
        LOG("[coop] doors: the other player plays %s, no door within %.0f m\n", as, DOOR_NEAR_M);
        return;
    }
    for (int i = 0; i < DOOR_MAX; i++)
        if (doors[i].actor == f.a) return;  // (open already)
    const char *open, *close;
    bool right = as[strlen(as) - 1] == 'R';
    if (strstr(f.name, "Double") || strstr(as, "Double")) open = "In", close = "InClose";
    else if (strstr(as, "Slide") || strstr(f.name, "Sheiker")) open = "Slide", close = "Close_Slide";
    else if (strstr(as, "Pull")) open = right ? "Pull_R" : "Pull_L", close = right ? "Close_R" : "Close_L";
    else open = right ? "Push_R" : "Push_L", close = right ? "Close_R" : "Close_L";
    int k = 0;
    while (k < DOOR_MAX && doors[k].actor) k++;
    if (k == DOOR_MAX) k = 0;
    play_anim(c, f.a, open, 0);
    doors[k].actor = f.a, doors[k].id = LD32(f.a + PROC_ID), doors[k].close_ns = now + DOOR_OPEN_NS;
    snprintf(doors[k].close_as, sizeof doors[k].close_as, "%s", close);
    LOG("[coop] doors: the other player plays %s: %s opens here (%s, %.1f m)\n", as, f.name, open, sqrtf(f.best));
}
// (dev) `coop door <AS>`: on the game thread (doors_dev_frame from rt_coop_game_frame via the pending flag)
static struct { volatile int pending; char as[32]; } door_dev;
static void doors_dev_frame(Cpu *c) {
    if (!door_dev.pending) return;
    door_dev.pending = 0;
    float p[3];
    if (!player_pos(p)) return;
    doors_open(c, p, door_dev.as, plat_now_ns());
}
