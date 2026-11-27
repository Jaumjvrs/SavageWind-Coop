// Part of runtime/coop.c (included there after coop/avatar.inc, same translation unit): the other player's horse.
// (2026-11-14: the riding avatar sat low / half in the ground, no horse.)
//
// The real horse is a Horse actor (GameRomHorse01..23, Epona, the giant / donkey / skeleton / Lord of the Mountain
// ones) with AI and physics; its mane, saddle and reins are separate actors (Horse_Link_Mane*, GameRomHorseSaddle_*,
// GameRomHorseReins_*, profile HorseObject / HorseSaddle / HorseReins) that the game binds to the horse's skeleton in
// code (their model lists have no bind bone). The player sits on it: while riding, the player's matrix follows the
// horse's Saddle_Root bone (every horse model has it except the donkey's).
//
// Sender (horse_publish): while the player rides (slot-0 AS with "Horse" in it, or Rideon*: mounting, riding,
// dismounting), the nearest horse body within HORSE_FIND_M is tracked; its accessories are the nearest ones within
// HORSE_ACC_M of it. Each frame the state packet gets a CoopHorse: names, the horse's matrix and velocity, the AS each
// of its first COOP_HORSE_SLOTS slots plays (index into horse_as_names, with a restart counter), its AS input values
// (the horse's trees pick steering / slope variants by them), and the rider's matrix relative to the horse's
// Saddle_Root bone. After a dismount the horse keeps being sent while it stays within HORSE_KEEP_M of the player, up
// to HORSE_KEEP_S seconds (so it does not vanish when the rider hops off; it is a live copy, not a ghost).
// Receiver (horse_frame, right after avatar_frame): puppets Dm_CoopH_<real actor> (Co-op/build_overlay.py: the real
// model, the horse's AS list, AI DemoDummy, no physics) for the body and each accessory, placed on the sent matrix
// (smoothed), playing the sent AS in the same frame on all of them (accessories are phase-locked to the body like the
// armor puppets to the avatar), with the sent inputs (written again after the NPC class's own input update: the
// astree_npc_hook in coop/astree.inc calls horse_npc_inputs). While RIDDEN the avatar is put on the puppet's
// Saddle_Root bone times the sent relative matrix (overriding avatar_frame's placement), so armor / gear follow it.
// The coat: see "the coat" below (protocol 45; a non-default coat, Horse_CollarType 3 / FootType 2, checked
// 2026-11-20). The Master Cycle's wheels / handlebar: coop/cyclefx.inc.
// SAVAGEWIND_COOP_HORSE=0: off (nothing sent, nothing shown).

static const char *const horse_as_names[COOP_HORSE_AS_COUNT] = {
    "", "Appear_Epona", "AttackFront", "Before_Event", "Collar_Shader", "Collar_Shader_Coward", "Collar_Shader_EponaHorse",
    "Collar_Shader_Impatient", "Collar_Shader_ZeldaHorse", "Courbette", "Courbette_For_Rodeo", "Crash", "Crying", "Damage",
    "Dead", "Dislike", "Dislike_For_Charge", "Drown", "Eat", "Eat_High", "Eyeblink", "Fall", "FallDown", "Foot_Visibility",
    "GotOff_From_Wild", "Hit_BottomArea", "Horseshoe", "Jump_Gear3", "Jump_Gear3_S", "Jump_Gear4", "Jump_Gear4_S",
    "KickBack", "KickBackWait", "Move_Back", "Move_Gear1", "Move_Gear2", "Move_Gear3", "Move_Gear4", "Move_Shift",
    "Move_Shift_Brake", "Rodeo", "Shake", "Slip", "SlipPost", "Sprain_Gear1", "Stagger", "Surprised", "Swim", "TurnBack",
    "Wait", "Wait_Curve_L", "Wait_Curve_R", "Wait_Light", "Wait_Search", "Warp_In"};
_Static_assert(sizeof horse_as_names / sizeof horse_as_names[0] == COOP_HORSE_AS_COUNT, "horse AS table");

#define HORSE_PREFIX "Dm_CoopH_"
#define HORSE_FIND_M 4.0f    // a horse body this close to the player when riding starts is the one ridden
#define HORSE_ACC_M 3.0f     // a mane / saddle / reins this close to the horse is its own
#define HORSE_KEEP_M 150.0f  // after a dismount: sent while this close to the player...
#define HORSE_KEEP_S 120.0   // ...for at most this long
#define HORSE_PARTS 4        // body, mane, saddle, reins
#define CYC_BONES 3          // (coop/cyclefx.inc) Wheel_F, Wheel_R, Handle
#define HORSE_LAYERS 3       // layers 0..2 of a slot take turns for the main AS (layer 3: the Horseshoe overlay)
static const char *const horse_part_name[HORSE_PARTS] = {"body", "mane", "saddle", "reins"};

static bool horse_on(void) {
    static int on = -1;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_HORSE");
        LOG("[coop] horse: the other player's horse shown (and ours sent): %s\n", on ? "on" : "off");
    }
    return on;
}
static volatile int horse_report;  // `coop horse`: one report line each side, to the log
static volatile int horse_reins_dev;  // `coop horse reins` (dev): the puppet's reins on the avatar's hands even when not ridden
static volatile int horse_eat_dev;  // `coop horse eat` (dev, 2026-11-21): the sent horse plays Eat (mane test)
static volatile int horse_track;   // `coop horse track` (dev): send the nearest horse (10 m) as if just dismounted

static bool horse_body_name(const char *n) {
    return starts(n, "GameRomHorse") && !starts(n, "GameRomHorseSaddle_") && !starts(n, "GameRomHorseReins_") && strlen(n) < 24;
}
static int horse_acc_kind(const char *n) {  // 1 mane, 2 saddle, 3 reins, 0 none
    if (starts(n, "Horse_Link_Mane") && !strstr(n, "Grabbed") && strlen(n) < 32) return 1;
    if (starts(n, "GameRomHorseSaddle_") && strlen(n) < 24) return 2;
    if (starts(n, "GameRomHorseReins_") && strlen(n) < 24) return 3;
    return 0;
}

// ---- other mounts (2026-11-19, coop-horse2, audit A6 / S13): the same CoopHorse carries them; the receiver tells the
// kind from the body name, and each kind has its own AS table (indices < COOP_HORSE_AS_COUNT, protocol 92):
//   MOUNT_SEAL  a sand seal (Animal_Sunazarashi*, Gerudo desert) while the player surfs behind it (AS WakeBoard*):
//               the puppet runs in front of the avatar; the avatar stays where its own state puts it (on the rope).
//   MOUNT_SIDON Prince Sidon (Npc_ZoraB001) while the player rides on his back (RideonWaitZora / RunZoraRide /
//               CutZoraRide): the avatar is seated on the puppet like on a horse (relative to his root matrix).
//   MOUNT_CYCLE the Master Cycle (GameROMMotorcycle, its model is in the base files) while the AS is Motorcycle*.
//   MOUNT_WILD  a wild deer / doe / bear the player jumped on (the same rider AS as a horse; no GameRomHorse near).
// Wild / unregistered horses need nothing new: they are GameRomHorse00..23 like a registered one (no saddle / bridle,
// mane Horse_Link_Mane_Reduction), the rodeo AS (Horse_Rodeo*) counts as riding, and the coat is read the same way.
// Followers only show a copy (no physics, no ownership change). A non-horse puppet is hidden while the same actor kind
// stands within MOUNT_TWIN_M of it in our own world (the enemy / animal sync may already show the partner's seal or
// deer there). Off switches: SAVAGEWIND_COOP_MOUNTS=0 (all of these), MOUNTWILD / SEAL / SIDON / CYCLE=0 (one kind).
enum { MOUNT_HORSE, MOUNT_SEAL, MOUNT_SIDON, MOUNT_CYCLE, MOUNT_WILD, MOUNT_KINDS };
static const char *const mount_kind_name[MOUNT_KINDS] = {"horse", "sand seal", "Sidon", "Master Cycle", "wild animal"};
#define MOUNT_KEEP_S 3.0     // a non-horse mount: still sent this long after the ride ends (no ghost left behind)
#define MOUNT_TWIN_M 4.0f
static const char *const seal_as_names[] = {
    "", "Damage", "Damage_To_Dizzy", "Damage_Village", "Dash", "Dead", "Dizzy", "Dizzy_To_Wait_Ground", "Dizzy_Wall", "Eat",
    "Electric", "Free_Fall", "Hearing", "Hearing_End", "LocationWait", "Look", "Move_Attack_Gear1", "Move_Attack_Gear2",
    "Move_Attack_Gear3", "Move_Attack_Gear4", "Move_Escape_Gear1", "Move_Escape_Gear2", "Move_Escape_Gear3",
    "Move_Escape_Gear4", "Move_Escape_Ground", "Move_Gear1", "Move_Gear2", "Move_Gear3", "Move_Gear4", "Move_Normal_Gear1",
    "Move_Normal_Gear2", "Move_Normal_Gear3", "Move_Normal_Gear4", "Notice", "Return_End", "Return_St", "Search", "Stagger",
    "Swim", "Swim_End", "Swim_Ground", "Swim_St", "To_Swim", "Turn_Attack", "Turn_Escape", "Turn_Notice", "Wait",
    "Wait_Curve_L", "Wait_Curve_R", "Wait_No_Move"};
static const char *const sidon_as_names[] = {  // (Npc_ZoraB001's body AS; faces, sleep and most talk left out)
    "", "Act_Call_Hello", "Act_Greetings", "Act_LookAround", "Act_LookDown", "Act_Lookfar", "Act_On_Link", "Act_Repair",
    "Act_Sink", "Act_Sink_Ed", "Act_Squat", "Act_Swim_Ejection", "Act_Swim_Hello", "Act_Victory", "Act_Wait",
    "Act_Wander_Wait", "Attack", "Crouch", "Crouch_Ed", "Crouch_To_Stand", "Detect", "DetectTurn", "Down", "Escape",
    "Escape_Sink", "Escape_Sink_Ed", "GetUp", "Guard", "GuardEquip", "GuardHit", "GuardMove", "GuardMoveLoop", "GuardTurn",
    "GuardUnequip", "Hello", "LookAround", "Meeting", "Move_End", "OpenDoor", "Point", "Run", "Run_Rain", "Stagger",
    "Stagger_Upper", "Stagger_Upper_Run", "Surprised", "Swim_Escape", "Swim_Run", "Swim_Walk", "Turn", "Victory", "Wait",
    "Walk", "Talk", "Talk_Wait"};
static const char *const cycle_as_names[] = {"", "Close", "MatAnim", "Open", "PreCooking", "PreCookingFairy"};
static const char *const wild_as_names[] = {  // (Animal_Bear_A's list; the deer's and doe's are a subset of it)
    "", "Attack", "Attack_Area", "Attack_Howling", "BackStep", "Courbette", "Courbette_For_Rodeo", "Damage", "Dead", "Dislike",
    "Dislike_For_Charge", "Eat", "Electric", "FallOff", "Free_Fall", "Hearing", "Hearing_End", "Hit_BackStep", "Howling",
    "LocationWait", "Look", "Move_AttackEx_Gear1", "Move_Attack_Gear1", "Move_Attack_Gear2", "Move_Attack_Gear3",
    "Move_Attack_Gear4", "Move_Back", "Move_EscapeEx_Gear1", "Move_Escape_Gear1", "Move_Escape_Gear2", "Move_Escape_Gear3",
    "Move_Escape_Gear4", "Move_Gear1", "Move_Gear2", "Move_Gear3", "Move_Gear4", "Move_Normal_Gear1", "Move_Normal_Gear2",
    "Move_Normal_Gear3", "Move_Normal_Gear4", "Notice", "Roar", "RoarLoop", "Rodeo", "Search", "Slip", "SlipPost",
    "Sprain_Gear1", "Stagger", "Turn_Attack", "Turn_Escape", "Turn_Notice", "Wait", "Wait_Curve_L", "Wait_Curve_R"};
#define MOUNT_TAB(t) (int)(sizeof t / sizeof t[0])
_Static_assert(MOUNT_TAB(seal_as_names) <= COOP_HORSE_AS_COUNT && MOUNT_TAB(sidon_as_names) <= COOP_HORSE_AS_COUNT &&
               MOUNT_TAB(cycle_as_names) <= COOP_HORSE_AS_COUNT && MOUNT_TAB(wild_as_names) <= COOP_HORSE_AS_COUNT,
               "mount AS tables fit the horse AS index");
static const char *const *mount_as_table(int kind, int *count) {
    switch (kind) {
    case MOUNT_SEAL: *count = MOUNT_TAB(seal_as_names); return seal_as_names;
    case MOUNT_SIDON: *count = MOUNT_TAB(sidon_as_names); return sidon_as_names;
    case MOUNT_CYCLE: *count = MOUNT_TAB(cycle_as_names); return cycle_as_names;
    case MOUNT_WILD: *count = MOUNT_TAB(wild_as_names); return wild_as_names;
    default: *count = COOP_HORSE_AS_COUNT; return horse_as_names;
    }
}
// The mount kind of an actor name (-1: not a mount). Names must fit CoopHorse.body (23 characters).
static int mount_kind(const char *n) {
    if (horse_body_name(n)) return MOUNT_HORSE;
    if (strlen(n) >= 24) return -1;
    if (starts(n, "Animal_Sunazarashi")) return MOUNT_SEAL;
    if (!strcmp(n, "Npc_ZoraB001")) return MOUNT_SIDON;
    if (!strcmp(n, "GameROMMotorcycle")) return MOUNT_CYCLE;
    if (starts(n, "Animal_Deer_") || starts(n, "Animal_Doe_") || starts(n, "Animal_Bear_")) return MOUNT_WILD;
    return -1;
}
static bool mount_kind_on(int kind) {
    static int on[MOUNT_KINDS] = {-1, -1, -1, -1, -1}, all = -1;
    if (kind < 0 || kind >= MOUNT_KINDS) return false;
    if (kind == MOUNT_HORSE) return true;  // (horse_on() gates every kind)
    if (all < 0) {
        all = look_on("SAVAGEWIND_COOP_MOUNTS");
        LOG("[coop] mounts: the partner's sand seal / Sidon / Master Cycle / wild deer and bears shown: %s\n", all ? "on" : "off");
    }
    if (on[kind] < 0) {
        static const char *const env[MOUNT_KINDS] = {"", "SAVAGEWIND_COOP_SEAL", "SAVAGEWIND_COOP_SIDON", "SAVAGEWIND_COOP_CYCLE",
                                                     "SAVAGEWIND_COOP_MOUNTWILD"};
        on[kind] = all && look_on(env[kind]);
    }
    return on[kind];
}
static bool mount_body_name(const char *n) { return mount_kind(n) >= 0; }
// How far from the player the mount being ridden is looked for (the seal pulls the surfer on a rope).
static float mount_find_m(int kind) { return kind == MOUNT_SEAL ? 16.0f : 4.0f; }
// Which mount the player's slot-0 AS says it rides (-1: none). MOUNT_HORSE also stands for a wild deer / bear.
static bool horse_riding_as(const char *as);
static int mount_riding_kind(const char *as) {
    if (starts(as, "WakeBoard")) return MOUNT_SEAL;
    if (strstr(as, "ZoraRide") || (starts(as, "Rideon") && strstr(as, "Zora"))) return MOUNT_SIDON;
    if (starts(as, "Motorcycle")) return MOUNT_CYCLE;
    if (horse_riding_as(as)) return MOUNT_HORSE;
    return -1;
}
static bool horse_any_name(const char *n) { return mount_body_name(n) || horse_acc_kind(n) > 0; }

// ---- the coat (2026-11-14: the horse coat colour was wrong too). The real horse keeps its coat frame at +0xb78
// (CollarType) and socks frame at +0xb7c (FootType) and shows them with AS of Rate 0 held at that frame: the colour AS
// at slot 0 layer 4 (by nature / special horse), "Foot_Visibility" at layer 5; its mane plays "Collar<suffix>" in step
// (frame linked to the body's (0, 4)). The puppets are plain NPC actors: we play the same AS and set their frames.
static const char *const horse_coat_as[COOP_HORSE_COAT_KINDS] = {
    "Collar_Shader", "Collar_Shader_Coward", "Collar_Shader_Impatient", "Collar_Shader_EponaHorse", "Collar_Shader_ZeldaHorse"};
static const char *const horse_mane_as[COOP_HORSE_COAT_KINDS] = {
    "Collar", "Collar_Coward", "Collar_Impatient", "Collar_EponaHorse", "Collar_ZeldaHorse"};
static bool horse_coat_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_HORSECOAT");
        on = !(e && e[0] == '0');
    }
    return on;
}
static int horse_coat_kind(const char *n) {
    for (int k = 0; k < COOP_HORSE_COAT_KINDS; k++)
        if (!strcmp(n, horse_coat_as[k])) return k;
    return -1;
}
// The AS frame of (slot, layer): FUN_71014a2424(as, slot, layer, <the game's default arg>, 0, frame).
#define A_AS_SET_FRAME 0x70014a2424ull
#define A_AS_SET_FRAME_ARG 0x7001d66e68ull
static void as_set_frame(Cpu *c, uint64_t actor, int slot, int layer, float frame) {
    uint64_t as = LD64(actor + ACTOR_AS);
    if (!in_guest(as)) return;
    Cpu saved = *c;
    memset(&c->v[0], 0, 16);
    memcpy(&c->v[0], &frame, 4);
    c->x[0] = as; c->x[1] = (uint64_t)slot; c->x[2] = (uint64_t)layer; c->x[3] = LD64(A_AS_SET_FRAME_ARG); c->x[4] = 0;
    guest_call(c, A_AS_SET_FRAME);
    *c = saved;
}
// Show coat / socks on a puppet part (0 body, 1 mane): plays the AS when it is not playing, then holds the frame.
static void horse_coat_apply(Cpu *c, uint64_t pup, int part, const CoopHorse *h, bool force) {
    int kind = h->socks >> 5;
    const char *want = part == 0 ? horse_coat_as[kind] : horse_mane_as[kind];
    const int lay = 4;
    uint64_t rec = as_layer_rec(pup, 0, lay);
    if (!rec) {  // (the slot has no such layer)
        static unsigned said;
        if (said++ < 4) LOG("[coop] horse puppet: %s has no AS layer (0, %d), coat not shown\n", part ? "mane" : "body", lay);
        return;
    }
    if (force || strcmp(as_layer_name(rec), want)) play_anim_l(c, pup, want, 0, lay);
    as_set_frame(c, pup, 0, lay, (float)(h->coat - 1));
    if (part == 0 && (h->socks & 31) && as_layer_rec(pup, 0, 5)) {
        if (force || strcmp(as_layer_name(as_layer_rec(pup, 0, 5)), "Foot_Visibility")) play_anim_l(c, pup, "Foot_Visibility", 0, 5);
        as_set_frame(c, pup, 0, 5, (float)((h->socks & 31) - 1));
    }
}

// ---- the sender
static struct {
    struct Held part[HORSE_PARTS];  // (Held: actor, unit, id, best distance, name)
    uint64_t ridden_ns, scan_ns, own_scan_ns;
    bool riding, owned;  // owned: it has a saddle / bridle (a registered horse: no time limit, sent before mounting)
    int kind;            // MOUNT_* of part[0]
    bool dev;            // tracked by a dev command (kept like a horse)
    uint64_t find_ns;    // riding with nothing found: the next look
    uint8_t seq[COOP_HORSE_SLOTS], as[COOP_HORSE_SLOTS];
    char as_last[COOP_HORSE_SLOTS][48];
    uint64_t inst_last[COOP_HORSE_SLOTS], time_last[COOP_HORSE_SLOTS];
    float frame_last[COOP_HORSE_SLOTS];
    // (2026-11-14, seen live) the horse cross-fades by playing the next AS on another layer of the slot (Move_Gear3 on
    // 0.1, then Move_Gear2 on 0.2, Wait back on 0.0), leaving the others empty: the newest started layer is sent.
    uint64_t layer_inst[COOP_HORSE_SLOTS][HORSE_LAYERS];
    int8_t cur_layer[COOP_HORSE_SLOTS];
    CoopHorse sent;
} hs;
struct HorseFind { float at[3]; bool body; unsigned kinds; struct Held got[HORSE_PARTS]; };  // kinds: bit per MOUNT_*
static void horse_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c;
    struct HorseFind *f = ctx;
    int mk = mount_kind(name), k = mk >= 0 ? 0 : horse_acc_kind(name);
    if (k == 0 && !f->body) return;
    if (k > 0 && f->body) return;
    if (k == 0 && !(f->kinds >> mk & 1)) return;
    float p[3], d = actor_dist(a, f->at, p);
    if (d >= f->got[k].best) return;
    struct Held *h = &f->got[k];
    h->best = d, h->actor = a, h->unit = LD64(a + PROC_UNIT), h->id = LD32(a + PROC_ID);
    snprintf(h->name, sizeof h->name, "%s", name);
}
// (2026-11-14: horses only appeared in the other game when Link got on top of one) Before any ride,
// the nearest saddled horse (saddle or bridle within HORSE_ACC_M: a registered horse; wild ones have none) within
// HORSE_OWN_M of the player is sent too. SAVAGEWIND_COOP_HORSEOWN=0: only after mounting (old rule).
#define HORSE_OWN_M 120.0f  // (2026-11-14: the horse only appeared really close, so the radius was raised a lot)
#define HORSE_OWN_MAX 16
struct HorseOwn { float at[3]; int nb, na; struct Held body[HORSE_OWN_MAX]; float acc[HORSE_OWN_MAX * 2][3]; };
static void horse_own_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c;
    struct HorseOwn *o = ctx;
    float p[3], d = actor_dist(a, o->at, p);
    int k = horse_body_name(name) ? 0 : horse_acc_kind(name);
    if (k == 0 && !horse_body_name(name)) return;  // (another mount kind: never "our saddled horse")
    if (k == 0) {
        if (d >= HORSE_OWN_M || o->nb >= HORSE_OWN_MAX) return;
        struct Held *h = &o->body[o->nb++];
        h->best = d, h->actor = a, h->unit = LD64(a + PROC_UNIT), h->id = LD32(a + PROC_ID);
        snprintf(h->name, sizeof h->name, "%s", name);
    } else if ((k == 2 || k == 3) && d < HORSE_OWN_M + HORSE_ACC_M && o->na < HORSE_OWN_MAX * 2)
        memcpy(o->acc[o->na++], p, 12);
}
static bool horse_own_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SAVAGEWIND_COOP_HORSEOWN");
        on = !(e && e[0] == '0');
    }
    return on;
}

static void horse_forget(void) {
    if (hs.part[0].actor) LOG("[coop] horse: no longer sent (%s)\n", hs.part[0].name);
    memset(hs.part, 0, sizeof hs.part);
    memset(hs.as_last, 0, sizeof hs.as_last);
    memset(hs.as, 0, sizeof hs.as);
    hs.riding = false, hs.owned = false, hs.dev = false, hs.kind = MOUNT_HORSE;
}
// Riding by the player's slot-0 AS: HorseWait, HorseRun, JumpHorseRide, CutHorseRide, RideonWaitHorse, DismountHorse...
static bool horse_riding_as(const char *as) {
    if (starts(as, "Rideon")) return !strstr(as, "Zora");
    return strstr(as, "Horse") && strcmp(as, "HorseCall") && !starts(as, "Face") && !starts(as, "Voice");
}
static void horse_publish(Cpu *c) {
    if (!horse_on()) return;
    uint64_t now = plat_now_ns(), pinfo = player_info_ptr(), pa = pinfo ? LD64(pinfo + 0x60) : 0;
    if (!in_guest(pa) || !local_in_world()) {
        if (hs.part[0].actor) horse_forget();
        coop_net_set_horse(NULL);
        return;
    }
    char as[64];
    as_name_slot(c, pa, 0, as, sizeof as);
    int rkind = mount_riding_kind(as);
    if (rkind > MOUNT_HORSE && !mount_kind_on(rkind)) rkind = -1;  // (that kind switched off: not sent)
    bool riding = rkind >= 0;
    float me[3];
    memcpy(me, (void *)(uintptr_t)(pinfo + 0x88), 12);
    if (hs.part[0].actor && !held_alive(&hs.part[0])) horse_forget();
    // (mounting another kind than the one sent, e.g. surfing a seal next to our saddled horse: find that one)
    if (riding && !hs.riding && hs.part[0].actor && rkind != hs.kind && !(rkind == MOUNT_HORSE && hs.kind == MOUNT_WILD))
        horse_forget();
    if (horse_eat_dev && hs.part[0].actor) horse_eat_dev = 0, play_anim(c, hs.part[0].actor, "Eat", 0);
    int track = horse_track;  // (1: `coop horse track`, the nearest horse; 2: `coop mount track`, the nearest other mount)
    if (track && hs.part[0].actor) horse_forget();  // (dev: the one asked for replaces the one sent)
    if ((track || (riding && now >= hs.find_ns)) && !hs.part[0].actor) {  // (find the mount being mounted / ridden)
        struct HorseFind f;
        memset(&f, 0, sizeof f);
        memcpy(f.at, me, 12);
        f.body = true;
        unsigned others = 0;
        for (int k = MOUNT_SEAL; k < MOUNT_KINDS; k++)
            if (mount_kind_on(k)) others |= 1u << k;
        f.kinds = track == 1 ? 1u << MOUNT_HORSE : track == 2 ? others : 1u << rkind;
        f.got[0].best = track == 1 ? 10.0f : track == 2 ? 16.0f : mount_find_m(rkind);
        walk_actors(c, horse_any_name, horse_seen, &f);
        if (!f.got[0].actor && !track && rkind == MOUNT_HORSE && mount_kind_on(MOUNT_WILD)) {  // (a wild deer / bear?)
            f.kinds = 1u << MOUNT_WILD;
            f.got[0].best = HORSE_FIND_M;
            walk_actors(c, horse_any_name, horse_seen, &f);
        }
        if (f.got[0].actor) {
            hs.part[0] = f.got[0];
            hs.kind = mount_kind(hs.part[0].name);
            hs.dev = track != 0;
            hs.scan_ns = 0;
            hs.ridden_ns = now;
            LOG("[coop] horse: %s (%s, %.1f m away) %s\n", hs.part[0].name, mount_kind_name[hs.kind], f.got[0].best,
                riding ? "ridden" : "tracked (dev)");
        } else if (riding)
            hs.find_ns = now + 250000000ull;  // (nothing near: look again in a moment, not every frame)
        if (track) horse_track = 0;
    }
    if (!riding && !hs.part[0].actor && horse_own_on() && now >= hs.own_scan_ns) {  // (our saddled horse nearby)
        hs.own_scan_ns = now + 1000000000ull;
        static struct HorseOwn o;
        memset(&o, 0, sizeof o);
        memcpy(o.at, me, 12);
        walk_actors(c, horse_any_name, horse_own_seen, &o);
        int best = -1;
        for (int i = 0; i < o.nb; i++) {
            float bp[3], dummy = actor_dist(o.body[i].actor, me, bp);
            (void)dummy;
            bool saddled = false;
            for (int j = 0; j < o.na && !saddled; j++) {
                float dx = o.acc[j][0] - bp[0], dy = o.acc[j][1] - bp[1], dz = o.acc[j][2] - bp[2];
                saddled = dx * dx + dy * dy + dz * dz < HORSE_ACC_M * HORSE_ACC_M;
            }
            if (saddled && (best < 0 || o.body[i].best < o.body[best].best)) best = i;
        }
        if (best >= 0) {
            hs.part[0] = o.body[best];
            hs.scan_ns = 0, hs.ridden_ns = now, hs.owned = true, hs.kind = MOUNT_HORSE;
            LOG("[coop] horse: %s (%.1f m away, saddled) sent before mounting\n", hs.part[0].name, o.body[best].best);
        }
    }
    if (!hs.part[0].actor) {
        coop_net_set_horse(NULL);
        return;
    }
    uint64_t body = hs.part[0].actor;
    float hm[12];
    memcpy(hm, (void *)(uintptr_t)(body + ACTOR_MTX), sizeof hm);
    float dx = hm[3] - me[0], dy = hm[7] - me[1], dz = hm[11] - me[2], dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (riding && !hs.riding && dist > mount_find_m(hs.kind) + 2.0f) {  // (mounting another horse than the one sent)
        horse_forget();
        coop_net_set_horse(NULL);
        return;
    }
    if (riding) hs.ridden_ns = now;
    else if ((!hs.owned && now - hs.ridden_ns > (uint64_t)((hs.kind == MOUNT_HORSE || hs.dev ? HORSE_KEEP_S : MOUNT_KEEP_S) * 1e9)) ||
             dist > HORSE_KEEP_M) {
        horse_forget();
        coop_net_set_horse(NULL);
        return;
    }
    if (riding != hs.riding) LOG("[coop] horse: %s %s (AS %s)\n", riding ? "riding" : "got off", hs.part[0].name, as);
    hs.riding = riding;
    if (hs.kind == MOUNT_HORSE && now >= hs.scan_ns) {  // (its accessories: every 0.5 s)
        hs.scan_ns = now + 500000000ull;
        struct HorseFind f;
        memset(&f, 0, sizeof f);
        f.at[0] = hm[3], f.at[1] = hm[7], f.at[2] = hm[11];
        for (int k = 1; k < HORSE_PARTS; k++) f.got[k].best = HORSE_ACC_M;
        walk_actors(c, horse_any_name, horse_seen, &f);
        for (int k = 1; k < HORSE_PARTS; k++) {
            if (f.got[k].actor != hs.part[k].actor || strcmp(f.got[k].name, hs.part[k].name))
                LOG("[coop] horse: %s %s (%.2f m from the horse)\n", horse_part_name[k], f.got[k].actor ? f.got[k].name : "none",
                    f.got[k].actor ? f.got[k].best : 0.0f);
            hs.part[k] = f.got[k];
        }
        if (hs.part[2].actor || hs.part[3].actor) hs.owned = true;  // (saddled: kept while near, no time limit)
    }
    CoopHorse h;
    memset(&h, 0, sizeof h);
    h.flags = COOP_HORSE_SHOWN;
    snprintf(h.body, sizeof h.body, "%s", hs.part[0].name);
    if (hs.part[1].actor && held_alive(&hs.part[1])) snprintf(h.mane, sizeof h.mane, "%s", hs.part[1].name);
    if (hs.part[2].actor && held_alive(&hs.part[2])) snprintf(h.saddle, sizeof h.saddle, "%s", hs.part[2].name);
    if (hs.part[3].actor && held_alive(&hs.part[3])) snprintf(h.reins, sizeof h.reins, "%s", hs.part[3].name);
    memcpy(h.mtx, hm, sizeof hm);
    float v[3];
    memcpy(v, (void *)(uintptr_t)(body + ACTOR_VEL), 12);
    for (int i = 0; i < 3; i++) h.vel[i] = isfinite(v[i]) && fabsf(v[i]) <= 20 ? v[i] : 0;
    // its AS (layer 0 of each slot) and restarts (same rule as the player's tree, coop/astree.inc astree_build)
    for (int s = 0; s < COOP_HORSE_SLOTS; s++) {
        // the layer that started last (a new instance there), else the current one while it plays, else any playing
        int cl = hs.cur_layer[s];
        for (int l = 0; l < HORSE_LAYERS; l++) {
            uint64_t r = as_layer_rec(body, s, l), li = r && as_layer_name(r)[0] ? LD64(r + 0x18) : 0;
            if (li && li != hs.layer_inst[s][l]) cl = l;
            hs.layer_inst[s][l] = li;
        }
        if (!hs.layer_inst[s][cl])
            for (int l = 0; l < HORSE_LAYERS; l++)
                if (hs.layer_inst[s][l]) { cl = l; break; }
        hs.cur_layer[s] = (int8_t)cl;
        uint64_t rec = as_layer_rec(body, s, cl);
        const char *n = as_layer_name(rec);
        uint64_t inst = rec ? LD64(rec + 0x18) : 0, tm = 0;
        float fr = -1;
        if (inst && hs.as[s] && (tm = as_layer_time(c, rec)) != 0) {
            memcpy(&fr, (void *)(uintptr_t)(tm + 4), 4);
            if (!isfinite(fr)) fr = -1;
        }
        if (strncmp(n, hs.as_last[s], sizeof hs.as_last[s])) {
            snprintf(hs.as_last[s], sizeof hs.as_last[s], "%s", n);
            int tn;
            const char *const *tab = mount_as_table(hs.kind, &tn);
            hs.as[s] = (uint8_t)table_index(tab, tn, n);
            if (n[0] && !hs.as[s])
                LOG("[coop] horse: slot %d plays %s, not in the %s AS table (not sent)\n", s, n, mount_kind_name[hs.kind]);
        } else if (hs.as[s] && inst && ((inst != hs.inst_last[s] && hs.inst_last[s]) ||
                   (tm && hs.time_last[s] && tm != hs.time_last[s] && fr >= 0 && fr <= ASRESTART_START_F && hs.frame_last[s] > fr + 0.5f)))
            hs.seq[s]++;
        hs.inst_last[s] = inst, hs.time_last[s] = tm, hs.frame_last[s] = fr;
        h.as[s] = hs.as[s], h.seq[s] = hs.seq[s];
    }
    // its coat (2026-11-14): the coat frame +0xb78, the socks frame +0xb7c, and which colour AS plays at (0, 4)
    if (horse_coat_on() && hs.kind == MOUNT_HORSE) {
        int32_t col = (int32_t)LD32(body + 0xb78), foot = (int32_t)LD32(body + 0xb7c);
        int kind = horse_coat_kind(as_layer_name(as_layer_rec(body, 0, 4)));
        if (kind >= 0 && col >= 0 && col < 255) {
            h.coat = (uint8_t)(col + 1);
            h.socks = (uint8_t)((foot >= 0 && foot < 31 ? foot + 1 : 0) | kind << 5);
        }
    }
    // its AS inputs: every registered float / int kind (the first COOP_HORSE_INPUTS), and the bools
    uint64_t hold = asin_holder(body);
    for (int k = 0; hold && k < 66 && h.n_in < COOP_HORSE_INPUTS; k++) {
        int ty = asin_type(k);
        if (ty != ASIN_FLOAT && ty != ASIN_INT) continue;
        uint64_t sl = asin_slot(hold, k);
        if (!sl) continue;
        uint32_t raw = LD32(sl);
        if (ty == ASIN_FLOAT) {
            float f;
            memcpy(&f, &raw, 4);
            if (!isfinite(f) || fabsf(f) > 100000) raw = 0;
        }
        h.in_kind[h.n_in] = (uint8_t)k, h.in_val[h.n_in] = raw, h.n_in++;
    }
    uint64_t sb = hold ? asin_slot(hold, 66) : 0, bv = sb ? LD64(sb) : 0;
    if (in_guest(bv)) h.bools = LD64(bv);
    // the rider, relative to the saddle bone (else to the horse's matrix; a seal's surfer is not seated: on the rope)
    if (riding && hs.kind != MOUNT_SEAL) {
        float pm[12], ref[12], inv[12], rel[12];
        memcpy(pm, (void *)(uintptr_t)(pa + ACTOR_MTX), sizeof pm);
        bool saddle = hs.kind == MOUNT_HORSE && bone_matrix(c, body, "Saddle_Root", ref);
        if (!saddle) memcpy(ref, hm, sizeof ref);
        if (mtx_inv(ref, inv)) {
            mtx_mul(inv, pm, rel);
            bool ok = true;
            for (int i = 0; i < 12 && ok; i++) ok = isfinite(rel[i]) && fabsf(rel[i]) <= ((i & 3) == 3 ? 10.0f : 1.5f);
            if (ok) {
                memcpy(h.rel, rel, sizeof rel);
                h.flags |= COOP_HORSE_RIDDEN | (saddle ? COOP_HORSE_REL_SADDLE : 0);
                // which hands hold the reins: the HorseReins actor's byte +0x868 (bit 0 left, bit 1 right; read by
                // its bone links 0x7101195f44, which put Reins_5_L/R on the rider's Weapon_L/R)
                if (hs.part[3].actor && held_alive(&hs.part[3])) {
                    uint8_t hb = LD8(hs.part[3].actor + 0x868);
                    h.flags |= (hb & 1 ? COOP_HORSE_HAND_L : 0) | (hb & 2 ? COOP_HORSE_HAND_R : 0);
                }
            }
        }
    }
    if (!coop_horse_ok(&h)) {
        static unsigned said;
        if (said++ < 5) LOG("[coop] horse: state of %s failed the check, not sent\n", h.body);
        coop_net_set_horse(NULL);
        return;
    }
    if ((h.flags ^ hs.sent.flags) & COOP_HORSE_RIDDEN)
        LOG("[coop] horse: sent %s %s, rider at %.2f %.2f %.2f from %s\n", h.body, h.flags & COOP_HORSE_RIDDEN ? "ridden" : "not ridden",
            h.rel[3], h.rel[7], h.rel[11], h.flags & COOP_HORSE_REL_SADDLE ? "Saddle_Root" : "the horse's root");
    if (h.coat != hs.sent.coat || h.socks != hs.sent.socks)
        LOG("[coop] horse: coat %d socks %d (%s) sent\n", h.coat - 1, (h.socks & 31) - 1,
            h.coat ? horse_coat_as[h.socks >> 5] : "unknown");
    if (memcmp(h.as, hs.sent.as, sizeof h.as) && (coop_look_log() || hs.kind != MOUNT_HORSE)) {
        char line[300] = "";
        int tn;
        const char *const *tab = mount_as_table(hs.kind, &tn);
        for (int s = 0; s < COOP_HORSE_SLOTS; s++)
            if (h.as[s] && h.as[s] < tn) snprintf(line + strlen(line), sizeof line - strlen(line), " %d=%s", s, tab[h.as[s]]);
        LOG("[coop] horse sent AS:%s\n", line);
    }
    hs.sent = h;
    coop_net_set_horse(&h);
}

// ---- the receiver
static struct {
    struct Pup p[HORSE_PARTS];
    uint8_t as[COOP_HORSE_SLOTS], seq[COOP_HORSE_SLOTS];
    bool ready[HORSE_PARTS];            // (playing the horse's AS: replay all when one becomes ready)
    float pos[3], rot[9], rel[12];
    bool placed, rel_have, riding;
    float mtx_prev[12];
    bool have_prev;
    CoopHorse rx;
    volatile uint64_t targets[HORSE_PARTS];
    volatile int have;
    char want[HORSE_PARTS][48];
    bool want_ok[HORSE_PARTS];
    int kind;                           // MOUNT_* of the shown body (from its name)
    struct {                            // (coop/cyclefx.inc) the Master Cycle puppet's wheels / handlebar
        uint64_t actor, node[CYC_BONES];
        bool on[CYC_BONES], failed[CYC_BONES], axis_ok[CYC_BONES], have_prev, fake_have;
        float axis[CYC_BONES][3], spin_f, spin_r, steer, prev[12], fake[12];
        float done[CYC_BONES], h0[2], steer_seen;  // (angle already applied per bone; handle rest direction)
    } cyc;
} hr;
static void cycle_fx(Cpu *c, uint64_t body, float m[12], double dt);  // (coop/cyclefx.inc)
static void cycle_fx_drop(Cpu *c);

static void horse_write_inputs(uint64_t a, const CoopHorse *h) {
    uint64_t hold = asin_holder(a);
    if (!hold) return;
    for (int i = 0; i < h->n_in; i++) {
        uint64_t sl = asin_slot(hold, h->in_kind[i]);
        if (sl) ST32(sl, h->in_val[i]);
    }
    uint64_t sb = asin_slot(hold, 66), bv = sb ? LD64(sb) : 0;
    if (in_guest(bv)) ST64(bv, h->bools);
    memcpy((void *)(uintptr_t)(a + ACTOR_VEL), h->vel, 12);
}
static bool horse_target_alive(const struct Pup *p, uint64_t a) {
    return a && p->actor == a && pup_alive(p);
}
// (called from coop/astree.inc astree_npc_hook, after the NPC class's own per-frame input update)
static void horse_npc_inputs(uint64_t a) {
    if (!hr.have || !a) return;
    for (int k = 0; k < HORSE_PARTS; k++)
        if (hr.targets[k] == a && horse_target_alive(&hr.p[k], a)) { horse_write_inputs(a, &hr.rx); return; }
}

static void horse_drop_all(Cpu *c) {
    hr.have = 0;
    cycle_fx_drop(c);  // (unlinks its bone nodes while the cycle puppet still lives)
    for (int k = 0; k < HORSE_PARTS; k++) {
        hr.targets[k] = 0;
        if (hr.p[k].actor) LOG("[coop] horse: %s puppet %s deleted\n", horse_part_name[k], hr.p[k].name);
        pup_drop(c, &hr.p[k]);
        hr.ready[k] = false;
    }
    memset(hr.as, 0, sizeof hr.as);
    hr.placed = hr.rel_have = hr.have_prev = hr.riding = false;
}
// The puppet name for a sent actor name, "" when the overlay has none (pack_exists is a file test: cached per name).
static const char *horse_puppet(int k, const char *real) {
    char want[48] = "";
    if (real[0]) snprintf(want, sizeof want, HORSE_PREFIX "%.38s", real);
    if (strcmp(want, hr.want[k])) {
        memcpy(hr.want[k], want, sizeof want);
        hr.want_ok[k] = want[0] && pack_exists(want);
        if (want[0] && !hr.want_ok[k]) LOG("[coop] horse: no puppet %s in the overlay (rebuild it: build_overlay.py)\n", want);
    }
    return hr.want_ok[k] ? hr.want[k] : "";
}
// Rotation smoothing: the 3x3 part blended toward the target, then made orthonormal again (columns = axes).
static void horse_orthonormal(float r[9]) {
    float x[3] = {r[0], r[3], r[6]}, y[3] = {r[1], r[4], r[7]}, z[3] = {r[2], r[5], r[8]};
    float n = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(n > 1e-4f)) return;
    for (int i = 0; i < 3; i++) z[i] /= n;
    float d = y[0] * z[0] + y[1] * z[1] + y[2] * z[2];
    for (int i = 0; i < 3; i++) y[i] -= d * z[i];
    n = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!(n > 1e-4f)) return;
    for (int i = 0; i < 3; i++) y[i] /= n;
    x[0] = y[1] * z[2] - y[2] * z[1], x[1] = y[2] * z[0] - y[0] * z[2], x[2] = y[0] * z[1] - y[1] * z[0];
    for (int i = 0; i < 3; i++) r[i * 3] = x[i], r[i * 3 + 1] = y[i], r[i * 3 + 2] = z[i];
}

// (2026-11-14: the other Link's hand was not synced with what he grabs on the horse) The reins' grips
// follow the rider's hands: the HorseReins actor's bone links (0x7101195f44 / 0x71011968c4) write the world matrix of
// Reins_5_L / Reins_5_R to the rider's Weapon_L / Weapon_R every frame, through the model unit's bone setter (vtable
// +0x48: unit, Matrix34f world, Vec3f scale, bone; 0x7100f772dc). The puppet does the same with the avatar's hands
// while the sender's flags say that hand holds the reins. Bone index: 0x7000f77260(model, SafeString) = bone << 16 | unit.
// SAVAGEWIND_COOP_REINS=0: off.
#define A_BONE_INDEX 0x7000f77260ull
static bool bone_set(Cpu *c, uint64_t actor, const char *bone, const float m[12]) {
    if (!bone_mem) bone_mem = (uint64_t)(uintptr_t)hle_alloc(0x100);
    uint64_t model = in_guest(actor) ? LD64(actor + 0x4e0) : 0;
    if (!bone_mem || !in_guest(model) || strlen(bone) >= 0x30) return false;
    for (int k = 0; k < 12; k++)
        if (!isfinite(m[k])) return false;
    memcpy((void *)(uintptr_t)(bone_mem + 0x40), bone, strlen(bone) + 1);
    ST64(bone_mem, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
    ST64(bone_mem + 8, bone_mem + 0x40);
    Cpu saved = *c;
    c->x[0] = model; c->x[1] = bone_mem;
    guest_call(c, A_BONE_INDEX);
    uint32_t idx = (uint32_t)c->x[0];
    *c = saved;
    if (idx >= 0xffff0000u || (idx & 0xffff) == 0xffff) return false;
    uint64_t units = LD64(model + 0x40), up = in_guest(units) ? LD64(units + (uint64_t)(int16_t)(idx & 0xffff) * 8) : 0;
    uint64_t unit = in_guest(up) ? LD64(up) : 0, vt = in_guest(unit) ? LD64(unit) : 0, fn = in_guest(vt) ? LD64(vt + 0x48) : 0;
    if (!fn) return false;
    memcpy((void *)(uintptr_t)(bone_mem + 0x80), m, 48);
    float one[4] = {1, 1, 1, 0};
    memcpy((void *)(uintptr_t)(bone_mem + 0xc0), one, 16);
    c->x[0] = unit; c->x[1] = bone_mem + 0x80; c->x[2] = bone_mem + 0xc0; c->x[3] = (uint64_t)(int64_t)((int32_t)idx >> 16);
    guest_call(c, fn);
    *c = saved;
    return true;
}
static float reins_drift = -1;  // (report) how far Reins_5_L moved from where we put it last frame (0: it stays)
static void horse_reins_hands(Cpu *c, const CoopHorse *h, const float am_prev[12], const float am[12]) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_REINS");
    // (coop-multi: static float last[3]; -> per avatar, avl)
    // (coop-multi: static bool have_last; -> per avatar, avl)
    uint64_t reins = hr.targets[3];
    if (!on || !reins || !hr.targets[0]) { avl.rh_have_last = false; return; }
    float cur[12];
    if (avl.rh_have_last && bone_matrix(c, reins, "Reins_5_L", cur))
        reins_drift = sqrtf((cur[3] - avl.rh_last[0]) * (cur[3] - avl.rh_last[0]) + (cur[7] - avl.rh_last[1]) * (cur[7] - avl.rh_last[1]) +
                            (cur[11] - avl.rh_last[2]) * (cur[11] - avl.rh_last[2]));
    avl.rh_have_last = false;
    // the avatar's hand bones are from its last pose (at am_prev): carried along to where it was just put (am)
    // The setter takes the bone's matrix relative to the reins skeleton's root bone Reins_Root (the bit, in the horse's
    // mouth), applied from the next frame on: measured 2026-11-14 (probe: identity -> exactly on
    // Reins_Root, +1 x -> 1 m along its x axis; a world matrix put the grips ~2000 m away). Reins_Root is read from
    // its last pose and carried along with this frame's move of the puppets (hr.mtx_prev -> the new matrix).
    float inv[12], delta[12], rinv[12], root[12], root_now[12], pinv[12], pdelta[12];
    if (!bone_matrix(c, reins, "Reins_Root", root)) return;
    const float *rm = (const float *)(uintptr_t)(reins + ACTOR_MTX);
    if (hr.have_prev && mtx_inv(hr.mtx_prev, pinv)) {
        mtx_mul(rm, pinv, pdelta);
        mtx_mul(pdelta, root, root_now);
    } else memcpy(root_now, root, sizeof root);
    if (!mtx_inv(root_now, rinv)) return;
    bool av = avatar.actor && mtx_inv(am_prev, inv);
    if (av) mtx_mul(am, inv, delta);
    // A grip no hand holds goes where the game puts it (0x7101195f44): on the horse's Spine_2 times a fixed offset
    // (left: identity, t 0.44 0.15 -0.25; right: rotation diag(1, -1, -1), t 0.44 0.15 0.25). The setter's value
    // stays until set again, so a grip is always set (else one let go would hang where the hand last was).
    static const float spine_off[2][12] = {{1, 0, 0, 0.44f, 0, 1, 0, 0.15f, 0, 0, 1, -0.25f},
                                           {1, 0, 0, 0.44f, 0, -1, 0, 0.15f, 0, 0, -1, 0.25f}};
    float spine[12], spine_now[12];
    bool sp = bone_matrix(c, hr.targets[0], "Spine_2", spine);
    if (sp) {
        if (hr.have_prev && mtx_inv(hr.mtx_prev, pinv)) mtx_mul(pdelta, spine, spine_now);
        else memcpy(spine_now, spine, sizeof spine);
    }
    static const char *const hand[2] = {"Weapon_L", "Weapon_R"}, *const grip[2] = {"Reins_5_L", "Reins_5_R"};
    for (int s = 0; s < 2; s++) {
        float hb[12], w[12], lm[12];
        bool held = av && (h->flags & (s ? COOP_HORSE_HAND_R : COOP_HORSE_HAND_L)) && bone_matrix(c, avatar.actor, hand[s], hb);
        if (held) mtx_mul(delta, hb, w);
        else if (sp) mtx_mul(spine_now, spine_off[s], w);
        else continue;
        mtx_mul(rinv, w, lm);
        if (bone_set(c, reins, grip[s], lm) && s == 0) avl.rh_last[0] = w[3], avl.rh_last[1] = w[7], avl.rh_last[2] = w[11], avl.rh_have_last = true;
    }
}

// The same horse standing in our world too (both saves have it there, e.g. copies of one save): no puppet on top.
// (coop-horse2) The same for another mount kind within MOUNT_TWIN_M: the animal / NPC sync may show it already.
struct HorseTwin { float at[3]; const char *name; float r; bool found; };
static void horse_twin_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c;
    struct HorseTwin *t = ctx;
    float p[3];
    if (!t->found && !strcmp(name, t->name) && actor_dist(a, t->at, p) < t->r) {
        for (int k = 0; k < HORSE_PARTS; k++)
            if (hr.p[k].actor == a) return;  // (our own puppet)
        t->found = true;
    }
}

// (dev, `coop horse coat`) every horse body / mane within 60 m: coat fields and the AS on slot 0 layers 0-5, to the log
static volatile int horse_coat_dev;
static bool horse_coat_dev_name(const char *n) { return horse_body_name(n) || horse_acc_kind(n) == 1 || starts(n, HORSE_PREFIX); }
static void horse_coat_dev_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    float p[3], d = actor_dist(a, (const float *)ctx, p);
    if (d > 60) return;
    char line[700];
    size_t n = 0;
    for (int l = 0; l < 6; l++) {
        uint64_t rec = as_layer_rec(a, 0, l), tm = rec && as_layer_name(rec)[0] ? as_layer_time(c, rec) : 0;
        float fr = -1;
        if (tm) memcpy(&fr, (void *)(uintptr_t)(tm + 4), 4);
        n += (size_t)snprintf(line + n, sizeof line - n, " 0.%d=%s@%.1f", l, as_layer_name(rec)[0] ? as_layer_name(rec) : "-", fr);
    }
    bool body = horse_body_name(name);
    LOG("[coop] horse coat dev: %s %llx at %.1f %.1f %.1f (%.1f m)%s%s\n", name, (unsigned long long)a, p[0], p[1], p[2], d,
        line, "");
    if (body) LOG("[coop] horse coat dev: %s +b78 %d +b7c %d\n", name, (int)LD32(a + 0xb78), (int)LD32(a + 0xb7c));
}

static void horse_frame(Cpu *c) {
    uint64_t now = plat_now_ns();
    if (horse_coat_dev) {
        horse_coat_dev = 0;
        uint64_t pinfo = player_info_ptr();
        float me[3] = {0, 0, 0};
        if (pinfo) memcpy(me, (void *)(uintptr_t)(pinfo + 0x88), 12);
        walk_actors(c, horse_coat_dev_name, horse_coat_dev_seen, me);
    }
    CoopHorse h;
    double age = 1e9;
    bool av = avatar.actor && avatar_alive() && avatar.frames >= 6;
    bool show = horse_on() && av && local_in_world() && coop_net_remote_horse(&h, &age) && age < 1.0 &&
                (h.flags & COOP_HORSE_SHOWN);
    if (horse_report) {
        horse_report = 0;
        LOG("[coop] horse report: sending %s%s%s at %.1f %.1f %.1f | received %s age %.2f flags %d | puppets %s %s %s %s at %.1f %.1f %.1f | riding %d, avatar at %.1f %.1f %.1f, reins drift %.2f\n",
            hs.part[0].actor ? hs.part[0].name : "none", hs.riding ? " (ridden)" : "", hs.part[1].actor ? " +mane" : "",
            hs.sent.mtx[3], hs.sent.mtx[7], hs.sent.mtx[11],
            show ? h.body : "none", age < 1e8 ? age : -1.0, show ? h.flags : 0, hr.p[0].actor ? hr.p[0].name : "-",
            hr.p[1].actor ? hr.p[1].name : "-", hr.p[2].actor ? hr.p[2].name : "-", hr.p[3].actor ? hr.p[3].name : "-",
            hr.pos[0], hr.pos[1], hr.pos[2], (int)hr.riding, avatar.pos[0], avatar.pos[1], avatar.pos[2], reins_drift);
    }
    // (coop-multi: static uint64_t twin_ns; -> per avatar, avl)
    // (coop-multi: static bool twin; -> per avatar, avl)
    int kind = show ? mount_kind(h.body) : -1;
    if (show && !mount_kind_on(kind)) show = false;  // (not a mount we know, or that kind switched off here)
    if (show && kind != hr.kind && (hr.p[0].actor || hr.p[1].actor || hr.p[2].actor || hr.p[3].actor)) horse_drop_all(c);
    if (show && kind != hr.kind) LOG("[coop] horse: the partner's mount is a %s (%s)\n", mount_kind_name[kind], h.body);
    if (show) hr.kind = kind;
    if (show && (kind != MOUNT_HORSE || !(h.flags & COOP_HORSE_RIDDEN))) {
        if (now >= avl.hf_twin_ns) {
            avl.hf_twin_ns = now + (kind == MOUNT_HORSE ? 1000000000ull : 300000000ull);
            struct HorseTwin t = {{h.mtx[3], h.mtx[7], h.mtx[11]}, h.body, kind == MOUNT_HORSE ? 1.5f : MOUNT_TWIN_M, false};
            walk_actors(c, mount_body_name, horse_twin_seen, &t);
            if (t.found != avl.hf_twin) LOG("[coop] horse puppet: %s %s\n", h.body, t.found ? "stands here in our world too: hidden" : "shown");
            avl.hf_twin = t.found;
        }
        if (avl.hf_twin) show = false;
    } else
        avl.hf_twin = false, avl.hf_twin_ns = 0;
    if (!show) {
        if (hr.p[0].actor || hr.p[1].actor || hr.p[2].actor || hr.p[3].actor) horse_drop_all(c);
        return;
    }
    const char *real[HORSE_PARTS] = {h.body, h.mane, h.saddle, h.reins};
    bool body_ok = pup_need(c, &hr.p[0], horse_puppet(0, h.body));
    for (int k = 1; k < HORSE_PARTS; k++) pup_need(c, &hr.p[k], body_ok ? horse_puppet(k, real[k]) : "");
    if (!body_ok) { hr.have = 0; return; }
    // place: smoothed toward the sent matrix (packets come 20 times a second)
    // (coop-multi: static uint64_t last_ns; -> per avatar, avl)
    double dt = avl.hf_last_ns && now > avl.hf_last_ns ? (now - avl.hf_last_ns) / 1e9 : 1 / 60.0;
    avl.hf_last_ns = now;
    float k = (float)(dt * 12 > 1 ? 1 : dt * 12), d2 = 0;
    float tp[3] = {h.mtx[3], h.mtx[7], h.mtx[11]}, tr[9] = {h.mtx[0], h.mtx[1], h.mtx[2], h.mtx[4], h.mtx[5], h.mtx[6],
                                                            h.mtx[8], h.mtx[9], h.mtx[10]};
    for (int i = 0; i < 3; i++) d2 += (tp[i] - hr.pos[i]) * (tp[i] - hr.pos[i]);
    if (!hr.placed || d2 > 20 * 20) k = 1;  // (new, or far: jump there)
    for (int i = 0; i < 3; i++) hr.pos[i] += (tp[i] - hr.pos[i]) * k;
    for (int i = 0; i < 9; i++) hr.rot[i] += (tr[i] - hr.rot[i]) * k;
    horse_orthonormal(hr.rot);
    hr.placed = true;
    float m[12] = {hr.rot[0], hr.rot[1], hr.rot[2], hr.pos[0], hr.rot[3], hr.rot[4], hr.rot[5], hr.pos[1],
                   hr.rot[6], hr.rot[7], hr.rot[8], hr.pos[2]};
    uint64_t body = hr.p[0].actor;
    memcpy(hr.mtx_prev, (void *)(uintptr_t)(body + ACTOR_MTX), sizeof hr.mtx_prev);
    hr.have_prev = hr.p[0].frames > 0;
    bool resync = false;
    for (int p = 0; p < HORSE_PARTS; p++) {
        if (!hr.p[p].actor) { hr.ready[p] = false, hr.targets[p] = 0; continue; }
        memcpy((void *)(uintptr_t)(hr.p[p].actor + ACTOR_MTX), m, sizeof m);
        memcpy((void *)(uintptr_t)(hr.p[p].actor + ACTOR_MTX2), m, sizeof m);
        hr.p[p].frames++;
        bool ready = hr.p[p].frames >= 6;  // (its AS and model are ready by then)
        if (ready && !hr.ready[p]) resync = true;
        hr.ready[p] = ready;
        hr.targets[p] = ready ? hr.p[p].actor : 0;
    }
    if (kind == MOUNT_CYCLE) cycle_fx(c, hr.targets[0], m, dt);  // (the Master Cycle: wheels roll, handlebar steers)
    // inputs (again after the NPC class's own update: horse_npc_inputs), then the AS, all parts in the same frame
    hr.rx = h;
    hr.have = 1;
    for (int p = 0; p < HORSE_PARTS; p++)
        if (hr.targets[p]) horse_write_inputs(hr.targets[p], &h);
    int tn;
    const char *const *tab = mount_as_table(kind, &tn);
    for (int s = 0; s < COOP_HORSE_SLOTS; s++) {
        if (h.as[s] >= tn) h.as[s] = 0;  // (an index past this kind's table: nothing)
        if (!resync && h.as[s] == hr.as[s] && h.seq[s] == hr.seq[s]) continue;
        for (int p = 0; p < HORSE_PARTS; p++) {
            if (!hr.targets[p]) continue;
            if (h.as[s]) play_anim_l(c, hr.targets[p], tab[h.as[s]], s, 0);
            else if (hr.as[s]) stop_anim_l(c, hr.targets[p], s, 0);
        }
        if (h.as[s] != hr.as[s] && (coop_look_log() || s == 0))
            LOG("[coop] horse puppet: slot %d %s\n", s, h.as[s] ? tab[h.as[s]] : "(stopped)");
        hr.as[s] = h.as[s], hr.seq[s] = h.seq[s];
    }
    // the accessories in step with the body (separate AS instances: same phase each frame)
    if (hr.targets[0])
        for (int p = 1; p < HORSE_PARTS; p++) {
            if (!hr.targets[p]) continue;
            for (int s = 0; s < COOP_HORSE_SLOTS; s++) {
                const char *nb = as_layer_name(as_layer_rec(hr.targets[0], s, 0));
                if (nb[0] && !strcmp(nb, as_layer_name(as_layer_rec(hr.targets[p], s, 0))))
                    as_sync_layer(c, hr.targets[p], hr.targets[0], s, s, 0, 0);
            }
        }
    // the coat: on a change or a newly ready part, else checked every 15 frames (held frame, AS still playing)
    if (h.coat && horse_coat_on() && kind == MOUNT_HORSE) {
        // (coop-multi: static uint8_t shown_coat, shown_socks; -> per avatar, avl)
        // (coop-multi: static unsigned tick; -> per avatar, avl)
        bool chg = h.coat != avl.hf_shown_coat || h.socks != avl.hf_shown_socks;
        if (chg || resync || ++avl.hf_tick % 15 == 0)
            for (int p = 0; p < 2; p++)
                if (hr.targets[p]) horse_coat_apply(c, hr.targets[p], p, &h, chg || resync);
        if (chg && hr.targets[0]) {
            LOG("[coop] horse puppet: coat %d socks %d (%s)%s\n", h.coat - 1, (h.socks & 31) - 1, horse_coat_as[h.socks >> 5],
                hr.targets[1] ? ", mane too" : "");
            avl.hf_shown_coat = h.coat, avl.hf_shown_socks = h.socks;
        }
    }
    // the rider: the avatar on the puppet's saddle bone (bones are one frame old: carried along this frame's move)
    bool ride = (h.flags & COOP_HORSE_RIDDEN) && hr.targets[0];
    if (ride != hr.riding) LOG("[coop] horse: avatar %s the horse puppet\n", ride ? "seated on" : "no longer on");
    hr.riding = ride;
    if (!ride) {
        hr.rel_have = false;
        // not ridden: the grips on the horse (dev `coop horse reins`: on the standing avatar's hands)
        CoopHorse f = h;
        f.flags &= ~(COOP_HORSE_HAND_L | COOP_HORSE_HAND_R);
        if (horse_reins_dev) f.flags |= COOP_HORSE_HAND_L | COOP_HORSE_HAND_R;
        float am[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (avatar.actor) memcpy(am, (void *)(uintptr_t)(avatar.actor + ACTOR_MTX), sizeof am);
        horse_reins_hands(c, &f, am, am);
        return;
    }
    float ref[12];
    bool bone = (h.flags & COOP_HORSE_REL_SADDLE) && bone_matrix(c, body, "Saddle_Root", ref);
    if (bone && hr.have_prev) {
        float inv[12], delta[12], moved[12];
        if (mtx_inv(hr.mtx_prev, inv)) {
            mtx_mul(m, inv, delta);
            mtx_mul(delta, ref, moved);
            memcpy(ref, moved, sizeof ref);
        }
    }
    if (!bone) {
        if (h.flags & COOP_HORSE_REL_SADDLE) return;  // (the puppet's bones are not there yet: avatar_frame's place)
        memcpy(ref, m, sizeof ref);
    }
    if (!hr.rel_have) memcpy(hr.rel, h.rel, sizeof hr.rel), hr.rel_have = true;
    for (int i = 0; i < 12; i++) hr.rel[i] += (h.rel[i] - hr.rel[i]) * k;
    float am[12];
    mtx_mul(ref, hr.rel, am);
    for (int i = 0; i < 12; i++)
        if (!isfinite(am[i])) return;
    float am_prev[12];
    memcpy(am_prev, (void *)(uintptr_t)(avatar.actor + ACTOR_MTX), sizeof am_prev);
    memcpy((void *)(uintptr_t)(avatar.actor + ACTOR_MTX), am, sizeof am);
    memcpy((void *)(uintptr_t)(avatar.actor + ACTOR_MTX2), am, sizeof am);
    horse_reins_hands(c, &h, am_prev, am);
    avatar.pos[0] = am[3], avatar.pos[1] = am[7], avatar.pos[2] = am[11];  // (avatar_frame smooths on from here)
    avatar.yaw = atan2f(am[2], am[0]) * 180 / 3.14159265f;
}
