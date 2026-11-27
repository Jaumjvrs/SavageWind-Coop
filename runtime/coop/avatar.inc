// Part of runtime/coop.c (included there, same translation unit): the other player's avatar: local state sent, animation states, death rule, gear and armor puppets.
// ---- the other player's avatar (networking: runtime/coop_net.c)
#define AVATAR_ACTOR_PLAIN "Dm_CoopLinkE"  // Co-op/build_overlay.py: Link's model, the player's AS list, profile NPC
// (2026-11-14) Dm_CoopLinkF = the same with the player's own effect set (ELink user Player_GameRom): the effects the
// player's AS start (water splashes and ripples while swimming, dust...) show on the avatar too (coop/avatarfx.inc).
// Used when the overlay has it; SAVAGEWIND_COOP_AVATARFX=0: the plain one (no effects).
#define AVATAR_ACTOR_FX "Dm_CoopLinkF"
// (2026-11-14) Dm_CoopLinkS = Dm_CoopLinkF with the player's own sound set too (SLink user GameROMPlayer: the voice and
// action sounds the player's AS start; coop/avatarsnd.inc). SAVAGEWIND_COOP_SOUNDS=0: Dm_CoopLinkF (no avatar sounds).
#define AVATAR_ACTOR_SND "Dm_CoopLinkS"
static bool pack_exists(const char *actor);
static bool look_on(const char *env);
static void fire_held_publish(Cpu *c, CoopState *s);  // (fire.inc)
static bool evcalm_hide(const float pos[3]);  // (coop/evcalm.inc)
#define EVCALM_PARK_M 60.0f
static const char *avatar_actor_name(void) {
    static const char *name;
    if (!name) {
        bool fx = look_on("SAVAGEWIND_COOP_AVATARFX");
        name = fx && look_on("SAVAGEWIND_COOP_SOUNDS") && pack_exists(AVATAR_ACTOR_SND) ? AVATAR_ACTOR_SND
             : fx && pack_exists(AVATAR_ACTOR_FX)                                       ? AVATAR_ACTOR_FX
                                                                                        : AVATAR_ACTOR_PLAIN;
        LOG("[coop] avatar actor: %s (%s)\n", name,
            name[11] == 'S' ? "with the player's effect and sound sets" : name[11] == 'F' ? "with the player's effect set" : "no effects");
    }
    return name;
}
#define AVATAR_ACTOR avatar_actor_name()
static struct {
    uint64_t actor;
    unsigned frames;     // since it was created
    uint64_t retry_ns;   // no new attempt before this
    float pos[3], yaw;   // drawn (smoothed toward the received state)
    int anim;            // COOP_ANIM_* playing, -1: none yet
    uint32_t area;       // the area it was created in (a stage load deletes it)
    uint64_t unit;       // its proc unit and id when created (avatar_alive: the game's handle test)
    uint32_t unit_id;
    char as[32];         // the AS it plays in slot 0
    uint8_t anim_seq;    // (protocol 19) action sequence counter
    int anim_upper;      // (protocol 22) COOP_ANIM_UPPER_* playing, 0: none
    uint8_t anim_upper_seq; // (protocol 22) upper action sequence counter
    char as_upper[32];   // (protocol 22) the AS it plays in slot 1
    char as_face[40];    // (protocol 25) the face AS it plays in slot 3
    float mtx_prev[12];  // its matrix before this frame's move (the one its bones were last computed with)
    bool have_prev;
} avatar;
// world_since_ns: avatar spawning may start 3 s after it (reset by deaths and avatar deletions too).
// placed_since_ns: only the player's own placement (load done), for the shared enemies.
static struct { float pos[3]; uint64_t at_ns; bool have; uint64_t world_since_ns, placed_since_ns; float ground, vertical, lateral; } last_local;
// SAVAGEWIND_COOP_LOOK_LOG=1: log every AS slot change and the gear found (tuning the avatar's states)
static volatile int aim_diag_dev;  // `coop aimdiag` (dev): aim diagnostic + look log on without environment variables
static bool coop_look_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_COOP_LOOK_LOG") && !strcmp(getenv("SAVAGEWIND_COOP_LOOK_LOG"), "1");
    return on || aim_diag_dev;
}

// A world counts as loaded once the player has a position away from the origin (during a load PlayerInfo exists
// with 0,0,0) and a player actor, for 3 seconds without a break: no actor is created during a stage load.
static bool local_in_world(void) {
    return last_local.world_since_ns && plat_now_ns() - last_local.world_since_ns > 3000000000ull;
}

static bool in_guest(uint64_t a) { return a >= GUEST_LO && a < GUEST_HI; }
static uint64_t player_info_ptr(void) {
    uint64_t p = g_site.player_info ? LD64(g_site.player_info) : 0;
    return in_guest(p) ? p : 0;
}

// The AS the player plays in slot 0 ("Wait", "Move", "SwimMove", "ParashawlGlide", ...): 0x70014a0040(as, slot, 0)
// returns the slot's current AS, a SafeString-like object whose +8 is the name (what the AI's play helper compares).
#define A_CURRENT_AS 0x70014a0040ull
static void as_name_slot(Cpu *c, uint64_t actor, int slot, char *out, size_t cap) {
    out[0] = 0;
    uint64_t as = LD64(actor + ACTOR_AS);
    if (!as) return;
    Cpu saved = *c;
    c->x[0] = as; c->x[1] = (uint64_t)slot; c->x[2] = 0;
    guest_call(c, A_CURRENT_AS);
    uint64_t obj = c->x[0];
    *c = saved;
    uint64_t name = obj ? LD64(obj + 8) : 0;
    if (name) snprintf(out, cap, "%s", (const char *)(uintptr_t)name);
}

static void player_as_name(Cpu *c, uint64_t actor, char *out, size_t cap) { as_name_slot(c, actor, 0, out, cap); }

// Every AS the actor plays, "slot.layer=name ..." (diagnostics). The AS controller (actor+0x560) has a slot count at
// +0xb8 and slots of 0x50 bytes at *(+0xc0); a slot's layer count is at +0x20 (what 0x70014a0040 checks).
static void as_list(Cpu *c, uint64_t actor, char *out, size_t cap) {
    size_t n = 0;
    out[0] = 0;
    uint64_t as = LD64(actor + ACTOR_AS);
    if (!as) return;
    int slots = (int)LD32(as + 0xb8);
    uint64_t tab = LD64(as + 0xc0);
    n += (size_t)snprintf(out + n, cap - n, "(slots=%d) ", slots);
    for (int s = 0; s < slots && s < 8 && tab; s++) {
        int layers = (int)LD32(tab + (uint64_t)s * 0x50 + 0x20);
        for (int l = 0; l < layers && l < 4 && n + 48 < cap; l++) {
            Cpu saved = *c;
            c->x[0] = as; c->x[1] = (uint64_t)s; c->x[2] = (uint64_t)l;
            guest_call(c, A_CURRENT_AS);
            uint64_t obj = c->x[0];
            *c = saved;
            uint64_t name = obj ? LD64(obj + 8) : 0;
            n += (size_t)snprintf(out + n, cap - n, "%d.%d=%.40s ", s, l, name ? (const char *)(uintptr_t)name : "-");
        }
    }
}

static bool starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

// ---- death rule (2026-11-08): when our player dies, the game's own Game Over / Continue runs as usual (reload of
// the last save point here). Once we are back in the world, if the other player is alive, standing on the ground
// in the same area and more than REVIVE_MIN_M away, our Link is put next to them: dying sends you back to your
// partner, not to an old save point far away. Waits at most 30 s for the partner to stand on the ground.
// SAVAGEWIND_COOP_REVIVE=0 turns it off.
#define REVIVE_MIN_M 25.0f
static struct { bool pending; uint64_t died_ns, since_ns; } revive;
static bool revive_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_REVIVE") && !strcmp(getenv("SAVAGEWIND_COOP_REVIVE"), "0"));
    return on;
}
static void revive_died(uint64_t now) {
    if (!revive.pending) LOG("[coop] revive: our player died; back next to the other player after the reload\n");
    revive.pending = true, revive.died_ns = now, revive.since_ns = 0;
}

static bool coop_anim_expanded_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_ANIMATIONS") && !strcmp(getenv("SAVAGEWIND_COOP_ANIMATIONS"), "0"));
    return on;
}

#define PROC_STATE 100

// ---- faces (protocol 25). The player's face AS plays in AS slot 3 ("3.0=FaceDefault"). The avatar's AS list holds
// the player's own Face* AS, so it plays the same one by name in its slot 3 (seen 2026-11-10: FaceSmile, FaceDead,
// FaceDamageL, FaceBattle, FaceGet, FaceTalkSurprise, FaceSurfingNG show on the avatar). Not the ones that choose
// hot / cold / tired from the player's own state: on the avatar FaceDefault and FaceMove come out as the open-mouth
// "hot" face, so those play FaceDemoCommon (the plain face with blinking). SAVAGEWIND_COOP_FACE=0 turns it off.
#include "faces.inc"
_Static_assert(sizeof coop_face_as / sizeof coop_face_as[0] == COOP_FACE_COUNT, "face table");
static bool face_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_FACE") && !strcmp(getenv("SAVAGEWIND_COOP_FACE"), "0"));
    return on;
}
static int look_dev_face = -1;  // `coop look face <n|off>` (dev)
static uint8_t local_face(Cpu *c, uint64_t pa) {
    static char last[40];
    static uint8_t idx;
    if (!face_on()) return 0;
    if (look_dev_face >= 0 && look_dev_face < COOP_FACE_COUNT) return (uint8_t)look_dev_face;
    char as[40];
    as_name_slot(c, pa, 3, as, sizeof as);
    if (strcmp(as, last)) {
        snprintf(last, sizeof last, "%s", as);
        idx = 0;
        for (int i = 1; i < COOP_FACE_COUNT; i++)
            if (!strcmp(as, coop_face_as[i])) { idx = (uint8_t)i; break; }
        if (coop_look_log()) LOG("[coop] local face %s -> %d\n", as[0] ? as : "-", idx);
    }
    return idx;
}
// The face AS the avatar plays for a received index ("" = leave it).
static const char *avatar_face_as(int face) {
    if (face <= 0 || face >= COOP_FACE_COUNT) return "FaceDemoCommon";
    const char *n = coop_face_as[face];
    if (!strcmp(n, "FaceDefault") || !strcmp(n, "FaceMove") || !strcmp(n, "FaceHorseWaitAddSearch") ||
        !strcmp(n, "FaceSelfCameraPose"))
        return "FaceDemoCommon";
    return n;
}

// (protocol 22) Classify upper-body action states for slot 1 concurrent blending
static int classify_upper(const char *as, const char *all, bool holding_carried) {
    if (!coop_anim_expanded_on()) return COOP_ANIM_UPPER_NONE;
    if (starts(as, "Dead") || starts(as, "Die") ||
        strstr(as, "Damage") || strstr(as, "Blow") || strstr(as, "Freeze") ||
        strstr(as, "BackJump") || strstr(as, "Step") ||
        starts(as, "Swim") || starts(as, "Climb") ||
        starts(as, "Surfing") || starts(as, "Ladder") || starts(as, "SlippingDown") || starts(as, "WallDash") ||
        starts(as, "Push") || starts(as, "StaminaOut") ||
        starts(as, "Get") || starts(as, "SheikPad") || starts(as, "LookAround") || starts(as, "WaterDive") ||
        starts(as, "Talk") || starts(as, "Door") || starts(as, "Rideon") || starts(as, "Dismount") ||
        starts(as, "WakeBoard") || !strcmp(as, "Warp") || strstr(as, "Unsteady") || !strcmp(as, "GuardBreak") || !strcmp(as, "GuardFailHit") ||
        starts(as, "Motorcycle") || starts(as, "Horse_") ||
        // (protocol 28) full-body states
        !strcmp(as, "GrabPut") || starts(as, "LiftPut") || starts(as, "WallOff") || starts(as, "SelfCamera") ||
        starts(as, "MasterSword") || starts(as, "Demo") || starts(as, "GroundDown") ||
        !strcmp(as, "HorseDead") || strstr(as, "HorseRide") || strstr(as, "ZoraRide") ||
        !strcmp(as, "CutJumpSt") || !strcmp(as, "CutJumpEd") || !strcmp(as, "CutFall") ||
        strstr(as, "Glide") || starts(as, "Para") || starts(as, "Float") ||
        (starts(as, "Cut") && !starts(as, "CutChargeWait") && !starts(as, "CutChargeMove"))) {
        return COOP_ANIM_UPPER_NONE;
    }
    // (protocol 28) one-shot upper-body actions; before the general Bow / WeaponThrow / WeaponEquip rules
    if (strstr(all, "WeaponEquipNG")) return COOP_ANIM_UPPER_EQUIP_NG;
    if (strstr(all, "BowReload")) return COOP_ANIM_UPPER_BOW_RELOAD;
    if (strstr(all, "WeaponThrowCatch")) return COOP_ANIM_UPPER_THROW_CATCH;
    if (strstr(all, "ItemEquipLeftOn")) return COOP_ANIM_UPPER_ITEM_L_ON;
    if (strstr(all, "ItemEquipLeftOff")) return COOP_ANIM_UPPER_ITEM_L_OFF;
    if (strstr(as, "WeaponThrowCharge") || strstr(all, "WeaponThrowCharge")) return COOP_ANIM_UPPER_THROW_AIM;
    if (strstr(as, "WeaponThrow") || strstr(all, "WeaponThrow")) return COOP_ANIM_UPPER_THROW;
    if (starts(as, "GrabThrow") || strstr(all, "GrabThrow")) return COOP_ANIM_UPPER_CARRY_THROW;
    if (holding_carried || starts(as, "Grab") || strstr(all, "GrabWait") || strstr(all, "GrabReady") || strstr(all, "GrabUp") || strstr(all, "GrabStand")) {
        return COOP_ANIM_UPPER_CARRY;
    }
    // (protocol 24) runes and items; AS names from the player's AS list, in any slot
    if (strstr(all, "MagneCatch")) return COOP_ANIM_UPPER_MAGNE;
    if (strstr(all, "ItemBombReady") || strstr(all, "ItemBombStart")) return COOP_ANIM_UPPER_BOMB_READY;
    if (strstr(all, "ItemCommonReady") || strstr(all, "ItemCommonStart")) return COOP_ANIM_UPPER_RUNE_AIM;
    if (strstr(all, "ItemScopeReady")) return COOP_ANIM_UPPER_SCOPE;
    if (strstr(all, "CallComeon") || strstr(all, "CallStay")) return COOP_ANIM_UPPER_WHISTLE;
    if (strstr(all, "WeaponEquipOn")) return COOP_ANIM_UPPER_EQUIP_ON;
    if (strstr(all, "WeaponEquipOff")) return COOP_ANIM_UPPER_EQUIP_OFF;
    if (strstr(as, "BowShoot") || strstr(all, "BowShoot")) return COOP_ANIM_UPPER_BOW_SHOOT;
    if (strstr(as, "Bow") || strstr(all, "Bow")) return COOP_ANIM_UPPER_BOW;
    if (strstr(as, "GuardJust") || strstr(all, "GuardJust")) return COOP_ANIM_UPPER_GUARD_JUST;
    if (strstr(as, "Guard") || strstr(all, "Guard") || strstr(all, "AttentionUpper")) return COOP_ANIM_UPPER_GUARD;
    return COOP_ANIM_UPPER_NONE;
}

// The avatar's state from the player's AS and speed (m/s; ground speed, and up/down speed).
// all: every slot's AS ("0.0=Wait 1.0=BowCharge ...", as_list). lateral: sideways speed, + = to the right.
static int classify(const char *as, const char *all, float ground, float vertical, float lateral) {
    float speed3 = sqrtf(ground * ground + vertical * vertical);
    if (coop_anim_expanded_on()) {
        if (starts(as, "Dead") || starts(as, "Die")) return COOP_ANIM_DIE;
        // (protocol 28) jump / fall / horseback sword attacks, horse extras, putting down, climb extras, selfie, turns
        if (starts(as, "Cut")) {
            if (!strcmp(as, "CutJumpSt") || !strcmp(as, "CutJumpHorseRide")) return COOP_ANIM_CUT_JUMP_ST;
            if (!strcmp(as, "CutJumpEd")) return COOP_ANIM_CUT_JUMP_LOOP;
            if (starts(as, "CutJumpLand")) return COOP_ANIM_CUT_JUMP_LAND;
            if (!strcmp(as, "CutFall")) return COOP_ANIM_CUT_FALL;
            if (!strcmp(as, "CutFallLand")) return COOP_ANIM_CUT_FALL_LAND;
            if (!strcmp(as, "CutChargeWaitHorseRide")) return COOP_ANIM_CUT_HORSE_CHARGE;
            if (!strcmp(as, "CutHorseRide") || !strcmp(as, "CutChargeHorseRide") || !strcmp(as, "CutZoraRide"))
                return COOP_ANIM_CUT_HORSE;
        }
        if (!strcmp(as, "Horse_Crash")) return COOP_ANIM_HORSE_CRASH;
        if (!strcmp(as, "Horse_Move_Slip")) return COOP_ANIM_HORSE_SLIP;
        if (starts(as, "Horse_Courbette")) return COOP_ANIM_HORSE_COURBETTE;
        if (!strcmp(as, "HorseDead")) return COOP_ANIM_HORSE_DEAD;
        if (starts(as, "Horse_Move_Gear_") && strstr(as, "_Go")) return COOP_ANIM_HORSE_SPUR;
        if (!strcmp(as, "GrabPut")) return COOP_ANIM_GRAB_PUT;
        if (!strcmp(as, "ParaEquipOff")) return COOP_ANIM_PARA_OFF;
        if (!strcmp(as, "LiftPutPouchL")) return COOP_ANIM_LIFT_PUT_POUCH;
        if (!strcmp(as, "ClimbTreeTopSharp")) return COOP_ANIM_CLIMB_TREE_TOP;
        if (!strcmp(as, "ClimbTreeEdSharp")) return COOP_ANIM_CLIMB_TREE_ED;
        if (!strcmp(as, "ClimbTreeTurnSharp")) return COOP_ANIM_CLIMB_TREE_TURN;
        if (!strcmp(as, "GroundDownToWall")) return COOP_ANIM_GROUND_DOWN_WALL;
        if (!strcmp(as, "LadderJump")) return COOP_ANIM_LADDER_JUMP;
        if (!strcmp(as, "LadderToClimb")) return COOP_ANIM_LADDER_TO_CLIMB;
        if (!strcmp(as, "WallOff")) return COOP_ANIM_WALL_OFF;
        if (!strcmp(as, "WallOffSquat")) return COOP_ANIM_WALL_OFF_SQUAT;
        if (starts(as, "SelfCameraPause")) return COOP_ANIM_SELFIE;
        if (!strcmp(as, "MasterSwordDead")) return COOP_ANIM_MASTER_DEAD;
        if (!strcmp(as, "Turn") && ground < 1.0f) return COOP_ANIM_TURN;
        if (!strcmp(as, "TurnAttention")) return COOP_ANIM_TURN_LOCKON;
        if (starts(as, "DemoTurn")) return COOP_ANIM_DEMO_TURN;
        if (!strcmp(as, "Demo318_0")) return COOP_ANIM_DEMO_LOOKAROUND;
        // (protocol 26) the rest of the player's AS list; before the general Damage / Horse / Swim / Climb rules
        if (starts(as, "Talk")) {
            if (starts(as, "Talking")) return COOP_ANIM_TALKING;
            if (!strcmp(as, "TalkHello")) return COOP_ANIM_TALK_HELLO;
            if (!strcmp(as, "TalkYes")) return COOP_ANIM_TALK_YES;
            if (!strcmp(as, "TalkSurprise")) return COOP_ANIM_TALK_SURPRISE;
            if (!strcmp(as, "TalkEmbarrass")) return COOP_ANIM_TALK_EMBARRASS;
            if (!strcmp(as, "TalkHandOver")) return COOP_ANIM_TALK_HANDOVER;
            if (starts(as, "TalkWeaponShow")) return COOP_ANIM_TALK_SHOW;
            if (starts(as, "TalkSheikPadShow")) return COOP_ANIM_TALK_SLATE;
            return COOP_ANIM_TALK;
        }
        if (starts(as, "DoorPush") || starts(as, "DoorPull")) return COOP_ANIM_DOOR_PUSH;
        if (starts(as, "DoorSlide")) return COOP_ANIM_DOOR_SLIDE;
        if (starts(as, "Rideon")) return COOP_ANIM_MOUNT;
        if (!strcmp(as, "DismountHorse")) return COOP_ANIM_DISMOUNT;
        if (!strcmp(as, "Horse_Move_Soothe") || !strcmp(as, "Horse_Rodeo_Soothe")) return COOP_ANIM_HORSE_SOOTHE;
        if (!strcmp(as, "Horse_Rodeo")) return COOP_ANIM_HORSE_RODEO;
        if (starts(as, "Horse_Jump")) return COOP_ANIM_HORSE_JUMP;
        if (starts(as, "WakeBoard")) {  // (sand-seal surfing)
            if (!strcmp(as, "WakeBoardCrash")) return COOP_ANIM_DAMAGE;
            return !strcmp(as, "WakeBoardAir") ? COOP_ANIM_WAKEBOARD_AIR : COOP_ANIM_WAKEBOARD;
        }
        if (!strcmp(as, "Warp")) return COOP_ANIM_WARP;
        if (!strcmp(as, "Freeze")) return COOP_ANIM_FREEZE;
        if (!strcmp(as, "FreezeBreak")) return COOP_ANIM_FREEZE_BREAK;
        if (!strcmp(as, "WaitUnsteady") || !strcmp(as, "DemoUnsteady")) return COOP_ANIM_UNSTEADY;
        if (!strcmp(as, "LandDamage")) return COOP_ANIM_LAND_DAMAGE;
        if (!strcmp(as, "LandDamageStand") || !strcmp(as, "DamageLStand")) return COOP_ANIM_STANDUP;
        if (!strcmp(as, "GuardBreak")) return COOP_ANIM_GUARD_BREAK;
        if (!strcmp(as, "GuardFailHit")) return COOP_ANIM_GUARD_HIT;
        if (!strcmp(as, "SwimDead")) return COOP_ANIM_SWIM_DIE;
        if (starts(as, "SwimWaterfall")) return COOP_ANIM_WATERFALL;
        if (starts(as, "SwimJump")) return COOP_ANIM_SWIM_JUMP;
        if (!strcmp(as, "SwimAttack")) return COOP_ANIM_SWIM_ATTACK;
        if (!strcmp(as, "SwimHangClimb") || !strcmp(as, "ClimbEd")) return COOP_ANIM_CLIMB_EDGE;
        if (starts(as, "ClimbJumpOff")) return COOP_ANIM_JUMP;
        if (!strcmp(as, "ClimbJump")) return COOP_ANIM_CLIMB_JUMP;
        if (!strcmp(as, "ClimbSlip")) return COOP_ANIM_CLIMB_SLIP;
        if (!strcmp(as, "ParaEquipOn")) return COOP_ANIM_PARA_ON;
        if (!strcmp(as, "JumpStagger")) return COOP_ANIM_STAGGER;
        // (the Master Cycle's own clips are not in Player_Animation: the riding pose stands in)
        if (starts(as, "Motorcycle")) return ground > 2.0f ? COOP_ANIM_HORSE_RUN : COOP_ANIM_HORSE_WAIT;
        if (!strcmp(as, "WaitDamage")) return COOP_ANIM_HURT_WAIT;  // (protocol 27: low on hearts, standing)
        // (2026-11-18, a two-game test: a hurt Link - hit by a Bokoblin just before - opened the field chest with
        // GetTreasurebox_Kick_Damage, which counted as damage: nobody locked the chest early. The chest AS go to the
        // chest kinds below. SAVAGEWIND_COOP_CHESTHURT=0: a hurt opening counts as damage, as before.)
        static int chesthurt = -1;
        if (chesthurt < 0) chesthurt = look_on("SAVAGEWIND_COOP_CHESTHURT");
        bool chest_as = chesthurt && starts(as, "GetTreasurebox");
        if (!chest_as && (strstr(as, "Damage") || strstr(as, "Blow") || strstr(as, "Freeze"))) return COOP_ANIM_DAMAGE;
        if (strstr(as, "BackJumpLand") || strstr(as, "StepLand") || starts(as, "Land")) return COOP_ANIM_LAND;
        if (strstr(as, "BackJump")) return COOP_ANIM_BACKJUMP;
        if (strstr(as, "Step")) return COOP_ANIM_SIDEHOP;
    }
    if (coop_anim_expanded_on()) {  // (protocol 23) movement family; AS names from the player's AS list
        if (starts(as, "Surfing")) {
            if (starts(as, "SurfingOn")) return COOP_ANIM_SURF_ON;  // (On, OnFall, OnPara: getting on the shield)
            if (!strcmp(as, "SurfingOff")) return COOP_ANIM_SURF_OFF;
            if (!strcmp(as, "SurfingJump")) return COOP_ANIM_SURF_JUMP;
            if (!strcmp(as, "SurfingAir")) return COOP_ANIM_SURF_AIR;
            if (!strcmp(as, "SurfingLand")) return COOP_ANIM_SURF_LAND;
            if (!strcmp(as, "SurfingStagger")) return COOP_ANIM_SURF_STAGGER;
            return COOP_ANIM_SURF;  // (Slide, NG)
        }
        if (starts(as, "Ladder") && strcmp(as, "LadderJump") && strcmp(as, "LadderToClimb")) {
            if (starts(as, "LadderUp") && strcmp(as, "LadderUpReadySt")) return COOP_ANIM_LADDER_UP;
            if (starts(as, "LadderDown")) return COOP_ANIM_LADDER_DOWN;
            return COOP_ANIM_LADDER_WAIT;  // (Wait, UpReadySt, Land)
        }
        if (starts(as, "SlippingDown")) return COOP_ANIM_ROLLDOWN;
        if (!strcmp(as, "Slide") || !strcmp(as, "SlideBrake")) return COOP_ANIM_SLIDE;
        if (starts(as, "WallDash")) return COOP_ANIM_CLIMB_DASH;
        if (starts(as, "Push")) return COOP_ANIM_PUSH;
        if (starts(as, "StaminaOut")) return COOP_ANIM_TIRED;
        if (!strcmp(as, "Brake")) return COOP_ANIM_BRAKE;
        // (protocol 24) items and the Sheikah Slate
        if (!strcmp(as, "Get")) return COOP_ANIM_GET;
        if (starts(as, "GetTreasurebox_Kick")) return COOP_ANIM_CHEST_KICK;
        if (starts(as, "GetTreasurebox_Unlock")) return COOP_ANIM_CHEST_OPEN;
        if (starts(as, "GetCooking")) return !strcmp(as, "GetCookingBad") ? COOP_ANIM_COOK_BAD : COOP_ANIM_COOK_GOOD;
        if (starts(as, "SheikPadSet")) return COOP_ANIM_SLATE_SET;
        if (starts(as, "SheikPadDownLoad")) return COOP_ANIM_SLATE_DOWNLOAD;
        if (starts(as, "SheikPadTouch")) return COOP_ANIM_SLATE_TOUCH;
        if (!strcmp(as, "SheikPadLookWaist")) return COOP_ANIM_SLATE_WAIST;
        if (starts(as, "SheikPadLook")) return COOP_ANIM_SLATE_LOOK;
        if (starts(as, "LookAround")) return COOP_ANIM_LOOK_AROUND;
        if (!strcmp(as, "WaterDiveJump")) return COOP_ANIM_DIVE;
    }
    if (starts(as, "Swim")) return ground > 4.0f ? COOP_ANIM_SWIM_FAST : ground > 0.3f ? COOP_ANIM_SWIM : COOP_ANIM_SWIM_WAIT;
    if (starts(as, "Climb")) {
        if (speed3 < 0.3f) return COOP_ANIM_CLIMB_WAIT;
        if (fabsf(vertical) >= fabsf(lateral)) return vertical >= 0 ? COOP_ANIM_CLIMB : COOP_ANIM_CLIMB_DOWN;
        return lateral > 0 ? COOP_ANIM_CLIMB_RIGHT : COOP_ANIM_CLIMB_LEFT;
    }
    if (strstr(as, "Glide") || starts(as, "Para") || starts(as, "Float")) return COOP_ANIM_GLIDE;
    // riding (HorseWait, HorseRun*, Rideon*, JumpHorseRide...): no horse under the avatar, but the pose
    if (strstr(as, "Horse") && !strstr(as, "Call") && !strstr(as, "Dead"))
        return ground > 2.0f ? COOP_ANIM_HORSE_RUN : COOP_ANIM_HORSE_WAIT;
    if (!coop_anim_expanded_on()) {
        if (strstr(all, "Bow") && !strstr(as, "Jump") && !strstr(as, "Fall")) return COOP_ANIM_BOW;
        if (strstr(all, "Guard") && !strstr(as, "Jump") && !strstr(as, "Fall")) return COOP_ANIM_GUARD;
    }
    if (starts(as, "Squat")) return ground > 0.3f ? COOP_ANIM_SQUAT_MOVE : COOP_ANIM_SQUAT_WAIT;
    // Sword attacks (the player's Cut* AS; jumping ones stay jump/fall below). One-handed sword clips stand in for
    // every weapon type.
    // (CutChargeWait / CutChargeMove*: Y held before the spin, also the first frames of every swing: stay on the
    // walk/wait states.)
    if (starts(as, "Cut") && !strstr(as, "Jump") && !strstr(as, "Fall") && !strstr(as, "Horse") &&
        !starts(as, "CutChargeWait") && !starts(as, "CutChargeMove")) {
        if (!strcmp(as, "CutS2")) return COOP_ANIM_CUT2;
        if (!strcmp(as, "CutS3") || !strcmp(as, "CutS4")) return COOP_ANIM_CUT3;
        if (!strcmp(as, "CutSF") || strstr(as, "Finish")) return COOP_ANIM_CUT_FINISH;
        if (!strcmp(as, "CutDash")) return COOP_ANIM_CUT_DASH;
        if (starts(as, "CutCharge")) return COOP_ANIM_CUT_SPIN;
        return COOP_ANIM_CUT1;
    }
    if (strstr(as, "Jump")) return vertical > 0 ? COOP_ANIM_JUMP : COOP_ANIM_FALL;
    if (strstr(as, "Fall") || strstr(as, "SkyDive") || vertical < -4.0f) return COOP_ANIM_FALL;
    // ground speeds measured on 1.9.0: walk 0.5-3, run ~6.0-6.2, dash (B held) ~7.2 m/s.
    // When the game's AS is Move or Turn, the player is actively moving their feet even during
    // initial acceleration or micro-stick taps. Never clamp to Wait just because speed is low.
    if (starts(as, "Move") || starts(as, "Turn") || strstr(as, "Move")) {
        if (ground > 6.6f) return COOP_ANIM_DASH;
        if (ground > 3.0f) return COOP_ANIM_RUN;
        return COOP_ANIM_WALK;
    }
    // Deceleration: if the stick was released and AS became Wait, but Link is still sliding/gliding
    // to a stop (> 1.0 m/s), keep walking/running so the legs continue moving instead of ice-skating.
    if (ground > 1.0f) return ground > 3.0f ? COOP_ANIM_RUN : COOP_ANIM_WALK;
    // Settled at rest (or decelerated below 0.4 m/s in Wait)
    if (ground < 0.4f || !strcmp(as, "Wait") || !strcmp(as, "WaitAttention")) return COOP_ANIM_WAIT;
    if (ground < 3.0f) return COOP_ANIM_WALK;
    if (ground < 6.6f) return COOP_ANIM_RUN;
    return COOP_ANIM_DASH;
}

// (protocol 27) Idles. The player's random idle fidgets, and the hot / cold ones, show in the face slot's AS name
// ("3.0=FaceRandomNemui"): that is how the standing state is told apart. Plain hot / cold standing (WaitHotCold) has
// no name of its own for hot or cold: the kind last seen in a fidget is kept (plain standing until one was seen).
static int idle_variant(int anim, const char *as, const char *all) {
    static int hotcold;  // 0 unknown, 1 cold, 2 hot
    if (!coop_anim_expanded_on()) return anim;
    if (strstr(all, "FaceColdWaitRandom")) hotcold = 1;
    if (strstr(all, "FaceHotWaitRandom")) hotcold = 2;
    if (anim == COOP_ANIM_SQUAT_WAIT && strstr(all, "FaceRandomWipeHand")) return COOP_ANIM_IDLE_WIPE;
    if (anim != COOP_ANIM_WAIT) return anim;
    if (strstr(all, "FaceRandomNemui")) return COOP_ANIM_IDLE_SLEEPY;
    if (strstr(all, "FaceRandomRelaxUpper")) return COOP_ANIM_IDLE_RELAX;
    if (strstr(all, "FaceRandomKinnikujiman")) return COOP_ANIM_IDLE_FLEX;
    if (strstr(all, "FaceColdWaitRandom")) return COOP_ANIM_COLD_FIDGET;
    if (strstr(all, "FaceHotWaitRandom")) return COOP_ANIM_HOT_FIDGET;
    if (!strcmp(as, "WaitHotCold")) return hotcold == 1 ? COOP_ANIM_COLD_WAIT : hotcold == 2 ? COOP_ANIM_HOT_WAIT : anim;
    hotcold = 0;  // (a plain Wait again: the weather passed)
    if (!strcmp(as, "WaitAttention")) return COOP_ANIM_LOCKON_WAIT;
    return anim;
}

// The avatar's AS for each state (Co-op/build_overlay.py COOP_CLIPS)
static const char *const coop_anim_as[COOP_ANIM_COUNT] = {
    "CoopWait", "CoopWalk", "CoopRun", "CoopDash", "CoopJump", "CoopFall", "CoopGlide",
    "CoopSwimWait", "CoopSwim", "CoopSwimFast", "CoopClimbWait", "CoopClimb",
    "CoopCut1", "CoopCut2", "CoopCut3", "CoopCutFinish", "CoopCutDash", "CoopCutSpin",
    "CoopSquatWait", "CoopSquatMove", "CoopGuard", "CoopBow", "CoopHorseWait", "CoopHorseRun",
    "CoopClimbDown", "CoopClimbLeft", "CoopClimbRight",
    "CoopGuardJust", "CoopBackJump", "CoopSideHop", "CoopLand", "CoopDamage", "CoopBowShoot",
    "CoopDie",
    "CoopThrowAim", "CoopThrow", "CoopCarry", "CoopCarryThrow",
    "CoopSurf", "CoopSurfAir", "CoopSurfJump", "CoopSurfLand", "CoopSurfOn", "CoopSurfOff", "CoopSurfStagger",
    "CoopLadderWait", "CoopLadderUp", "CoopLadderDown",
    "CoopSlide", "CoopRolldown", "CoopClimbDash", "CoopPush", "CoopTired", "CoopBrake",
    "CoopGet", "CoopChestKick", "CoopChestOpen",
    "CoopSlateSet", "CoopSlateLook", "CoopSlateTouch", "CoopSlateDownload", "CoopSlateWaist",
    "CoopCookGood", "CoopCookBad", "CoopLookAround", "CoopDive",
    "CoopTalk", "CoopTalking", "CoopTalkShow", "CoopTalkSlate", "CoopHorseSoothe", "CoopHorseRodeo",
    "CoopWakeboard", "CoopWakeboardAir", "CoopFreeze", "CoopUnsteady", "CoopWaterfall", "CoopClimbSlip",
    "CoopTalkHello", "CoopTalkYes", "CoopTalkSurprise", "CoopTalkEmbarrass", "CoopTalkHandover", "CoopDoorPush",
    "CoopDoorSlide", "CoopMount", "CoopDismount", "CoopHorseJump", "CoopWarp", "CoopFreezeBreak", "CoopLandDamage",
    "CoopStandup", "CoopGuardBreak", "CoopGuardHit", "CoopSwimDie", "CoopSwimJump", "CoopSwimAttack",
    "CoopClimbJump", "CoopClimbEdge", "CoopParaOn", "CoopStagger",
    "CoopIdleSleepy", "CoopIdleRelax", "CoopIdleFlex", "CoopIdleWipe", "CoopColdFidget", "CoopHotFidget",
    "CoopColdWait", "CoopHotWait", "CoopLockonWait", "CoopHurtWait",
    "CoopCutJumpLoop", "CoopCutFall", "CoopCutHorseCharge", "CoopSelfie", "CoopDemoLookaround", "CoopCutJumpSt",
    "CoopCutJumpLand", "CoopCutFallLand", "CoopCutHorse", "CoopHorseCrash", "CoopHorseSlip", "CoopHorseCourbette",
    "CoopHorseDead", "CoopHorseSpur", "CoopGrabPut", "CoopParaOff", "CoopLiftPutPouch", "CoopClimbTreeTop",
    "CoopClimbTreeEd", "CoopClimbTreeTurn", "CoopGroundDownWall", "CoopLadderJump", "CoopLadderToClimb",
    "CoopWallOff", "CoopWallOffSquat", "CoopMasterDead", "CoopTurn", "CoopTurnLockon", "CoopDemoTurn",
};
// The avatar's AS for a state with the other player's weapon: two-handed swords and spears swing their own way.
static const char *avatar_as(int anim, const char *weapon) {
    if (anim < 0 || anim >= COOP_ANIM_COUNT) anim = COOP_ANIM_WAIT;
    if (coop_anim_is_cut(anim)) {
        if (!strncmp(weapon, "Weapon_Lsword_", 14)) {
            if (anim == COOP_ANIM_CUT_SPIN) return "CoopLswordSpin";
            return anim == COOP_ANIM_CUT_FINISH ? "CoopLswordFinish" : "CoopLsword1";
        }
        if (!strncmp(weapon, "Weapon_Spear_", 13)) {
            if (anim == COOP_ANIM_CUT_SPIN) return "CoopSpearRush";
            return anim == COOP_ANIM_CUT_FINISH ? "CoopSpearFinish" : "CoopSpear1";
        }
    }
    {  // (protocol 28) jump / horse attacks and the selfie pose: a variant per weapon type
        const char *v = NULL;
        switch (anim) {
        case COOP_ANIM_CUT_JUMP_LOOP: v = "CoopCutJumpLoop"; break;
        case COOP_ANIM_CUT_FALL: v = "CoopCutFall"; break;
        case COOP_ANIM_CUT_HORSE_CHARGE: v = "CoopCutHorseCharge"; break;
        case COOP_ANIM_SELFIE: v = "CoopSelfie"; break;
        case COOP_ANIM_CUT_JUMP_ST: v = "CoopCutJumpSt"; break;
        case COOP_ANIM_CUT_JUMP_LAND: v = "CoopCutJumpLand"; break;
        case COOP_ANIM_CUT_FALL_LAND: v = "CoopCutFallLand"; break;
        case COOP_ANIM_CUT_HORSE: v = "CoopCutHorse"; break;
        }
        if (v) {
            static char buf[48];
            if (!strncmp(weapon, "Weapon_Lsword_", 14)) { snprintf(buf, sizeof buf, "%sLsword", v); return buf; }
            if (!strncmp(weapon, "Weapon_Spear_", 13)) { snprintf(buf, sizeof buf, "%sSpear", v); return buf; }
            return v;
        }
    }
    if (anim == COOP_ANIM_THROW_AIM) {
        if (!strncmp(weapon, "Weapon_Lsword_", 14)) return "CoopLswordThrowAim";
        if (!strncmp(weapon, "Weapon_Spear_", 13)) return "CoopSpearThrowAim";
        return "CoopThrowAim";
    }
    if (anim == COOP_ANIM_THROW) {
        if (!strncmp(weapon, "Weapon_Lsword_", 14)) return "CoopLswordThrow";
        if (!strncmp(weapon, "Weapon_Spear_", 13)) return "CoopSpearThrow";
        return "CoopThrow";
    }
    if (anim == COOP_ANIM_LOCKON_WAIT) {  // (the lock-on stance differs with the weapon)
        if (!strncmp(weapon, "Weapon_Lsword_", 14)) return "CoopLockonLsword";
        if (!strncmp(weapon, "Weapon_Spear_", 13)) return "CoopLockonSpear";
        if (!strncmp(weapon, "Weapon_Sword_", 13)) return "CoopLockonSword";
    }
    return coop_anim_as[anim];
}

// (protocol 27) The upper-body AS of an avatar standing with a drawn weapon (gear on the right hand) or a bow in
// the left hand; "" = nothing in the hands.
static const char *armed_idle_as(const CoopState *rs) {
    const CoopGear *w = &rs->gear[COOP_GEAR_WEAPON], *b = &rs->gear[COOP_GEAR_BOW];
    if (w->name[0] && w->bone == COOP_BONE_WEAPON_R) {
        if (!strncmp(w->name, "Weapon_Lsword_", 14)) return "CoopArmLsword";
        if (!strncmp(w->name, "Weapon_Spear_", 13)) return "CoopArmSpear";
        return "CoopArmSword";
    }
    if (b->name[0] && b->bone == COOP_BONE_WEAPON_L) return "CoopArmBow";
    return "";
}

// (protocol 22) The avatar's upper-body AS for slot 1
static const char *const coop_anim_upper_as[COOP_ANIM_UPPER_COUNT] = {
    "",
    "CoopBow",
    "CoopBowShoot",
    "CoopGuard",
    "CoopGuardJust",
    "CoopCarry",
    "CoopCarryThrow",
    "CoopThrowAim",
    "CoopThrow",
    "CoopRuneAim", "CoopBombReady", "CoopMagne", "CoopScope", "CoopEquipOn", "CoopEquipOff", "CoopWhistle",
    "CoopEquipNg", "CoopBowReload", "CoopThrowCatch", "CoopItemLOn", "CoopItemLOff",
};

static const char *avatar_as_upper(int anim_upper, const char *weapon) {
    if (anim_upper <= COOP_ANIM_UPPER_NONE || anim_upper >= COOP_ANIM_UPPER_COUNT) return "";
    if (anim_upper == COOP_ANIM_UPPER_THROW_AIM) {
        if (!strncmp(weapon, "Weapon_Lsword_", 14)) return "CoopLswordThrowAim";
        if (!strncmp(weapon, "Weapon_Spear_", 13)) return "CoopSpearThrowAim";
        return "CoopThrowAim";
    }
    if (anim_upper == COOP_ANIM_UPPER_THROW_CATCH && !strncmp(weapon, "Weapon_Lsword_", 14)) return "CoopLswordThrowCatch";
    if (anim_upper == COOP_ANIM_UPPER_THROW) {
        if (!strncmp(weapon, "Weapon_Lsword_", 14)) return "CoopLswordThrow";
        if (!strncmp(weapon, "Weapon_Spear_", 13)) return "CoopSpearThrow";
        return "CoopThrow";
    }
    return coop_anim_upper_as[anim_upper];
}

// The area the player is in: the game's current map type and name (static SafeStrings: 0x7101208304 / 0x71012082f8
// in Ghidra return them, the text at +8): "MainField" + its 1000 m section ("I-4"), "CDungeon" + "Dungeon126" for a
// shrine, "MainFieldDungeon" + "RemainsWater"... The overworld counts as one area (its section changes as you walk).
#define A_MAP_TYPE 0x7001dae3a0ull
#define A_MAP_NAME 0x7001dae3d8ull
static uint32_t local_area(char *text, size_t cap) {
    uint64_t t = LD64(A_MAP_TYPE + 8), n = LD64(A_MAP_NAME + 8);
    const char *type = t ? (const char *)(uintptr_t)t : "", *name = n ? (const char *)(uintptr_t)n : "";
    if (!strcmp(type, "MainField")) snprintf(text, cap, "%s", type);
    else snprintf(text, cap, "%.40s/%.40s", type, name);
    return (uint32_t)crc32(0, (const Bytef *)text, (uInt)strlen(text));
}

// The local player's gear and armor for the avatar's look (protocol 14; local_look below the avatar code).
static void local_look(Cpu *c, uint64_t player, CoopState *s);
static CoopState look_sent;        // the last state we sent (`coop look`)
static volatile int look_report;   // `coop look`: log it on the game thread

static struct { char armor[COOP_ARMOR_SLOTS][32]; CoopGear gear[COOP_GEAR_COUNT]; int anim, anim_upper; } look_dev = {.anim = -1, .anim_upper = -1};
static void astree_publish(Cpu *c, uint64_t pa);  // (protocol 29, coop/astree.inc) tree mode: what we send

// The local player's state, for the peer: position (PlayerInfo +0x88), facing (the player actor's matrix: PlayerInfo
// +0x60 is GameROMPlayer), speed from the last frames (smoothed), and what the avatar should play.
// ---- the red hurt flash (2026-11-15, protocol 50: the red flash that plays when Link gets hurt did not show
// on the other Link). Ghidra: the player's damage-colour timer is a float at player+0x1da4 (frames left;
// FUN_7100ce927c counts it down each frame) and its colour (1, 0.15, 0.1, a = timer / 24 * 0.5; 0 0 0 0 when done)
// goes to the shader uniform uk_damage_color of every unit of the actor's model (actor+0x4e0) through FUN_710124d110
// (unused, model, float[4]; called by FUN_7100ccbf14). The sender sets COOP_FLAG_HURT while its timer runs; from the
// flag's rising edge the avatar runs the same fade (HURT_FLASH_FRAMES) on its model and its armor / gear puppets.
// SAVAGEWIND_COOP_HURT_FLASH=0 turns it off.
#define PLAYER_DMG_COLOR_T 0x1da4
#define A_MODEL_DAMAGE_COLOR 0x700124d110ull
#define HURT_FLASH_FRAMES 24.0f
static bool hurt_flash_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_HURT_FLASH") && !strcmp(getenv("SAVAGEWIND_COOP_HURT_FLASH"), "0"));
    return on;
}
static float LDF32(uint64_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }
static uint64_t rune_aim_ns;  // (n5, runes.inc) when our Link last aimed a rune (ItemCommonReady / Start)
static uint64_t arrows_bow_ns;  // (arrows.inc) when our Link last had the bow out (drawn / shooting / reloading)
static uint64_t arrow_on_string(void);  // (arrows.inc) our arrow on the bow string, or 0
static uint64_t astree_pad_until_ns;    // (astree.inc astree_settle) the paused-idle Slate pose is being sent
static uint64_t local_chest_open_ns;    // when our Link last played the chest-opening AS (GetTreasurebox_Unlock)
static int local_anim_now = -1;         // our Link's anim as last sent (gate.inc: the get-item window waits for calm)
static void magbeam_publish(CoopState *s);
static void stasisfx_publish(Cpu *c, CoopState *s);
static void publish_local(Cpu *c) {
    CoopState s;
    memset(&s, 0, sizeof s);
    uint64_t pinfo = player_info_ptr(), now = plat_now_ns();
    if (pinfo) memcpy(s.pos, (void *)(uintptr_t)(pinfo + 0x88), 12);
    bool placed = pinfo && LD64(pinfo + 0x60) && fabsf(s.pos[0]) + fabsf(s.pos[1]) + fabsf(s.pos[2]) > 1;
    if (!placed) last_local.world_since_ns = last_local.placed_since_ns = 0;
    else {
        if (!last_local.world_since_ns) last_local.world_since_ns = now;
        if (!last_local.placed_since_ns) last_local.placed_since_ns = now;
    }
    if (placed) {
        uint64_t pa = LD64(pinfo + 0x60);
        if (pa) {
            float m[12];
            memcpy(m, (void *)(uintptr_t)(pa + ACTOR_MTX), sizeof m);
            s.yaw = atan2f(m[2], m[0]) * 180 / 3.14159265f;
        }
        float ground = 0, vertical = 0, lateral = 0;
        if (last_local.have && now > last_local.at_ns) {
            float dx = s.pos[0] - last_local.pos[0], dy = s.pos[1] - last_local.pos[1], dz = s.pos[2] - last_local.pos[2];
            double dt = (now - last_local.at_ns) / 1e9;
            ground = (float)(sqrt(dx * dx + dz * dz) / dt), vertical = (float)(dy / dt);
            // sideways speed (+: toward the player's right; facing = the matrix's z column, right = -x column)
            float m[12];
            memcpy(m, (void *)(uintptr_t)(pa + ACTOR_MTX), sizeof m);
            lateral = (float)((-m[0] * dx - m[8] * dz) / dt);
            if (ground > 40 || fabsf(vertical) > 60) ground = vertical = lateral = 0;  // (a teleport)
        }
        // smoothed over ~0.15 s: positions move in steps that don't match every frame
        last_local.ground += (ground - last_local.ground) * 0.25f;
        last_local.vertical += (vertical - last_local.vertical) * 0.25f;
        last_local.lateral += (lateral - last_local.lateral) * 0.25f;
        s.speed = last_local.ground;
        memcpy(last_local.pos, s.pos, 12);
        last_local.at_ns = now, last_local.have = true;
        s.flags = local_in_world() ? COOP_FLAG_IN_WORLD : 0;
        // (protocol 50) our Link's red damage colour is on (its timer, frames left: see hurt_flash_frame)
        if (hurt_flash_on() && in_guest(pa) && LDF32(pa + PLAYER_DMG_COLOR_T) > 1e-3f) s.flags |= COOP_FLAG_HURT;
        char area[96];
        s.area = local_area(area, sizeof area);
        static uint32_t last_area;
        if (s.area != last_area) {
            LOG("[coop] local area %s (%08x)\n", area, s.area);
            // A new area: 3 s more before anything is created here. The area's name changes when its load starts,
            // while PlayerInfo still holds the old position (seen 2026-11-08: the guest following into a shrine
            // spawned the host's avatar mid-load and stayed on the loading screen for good).
            if (last_area && last_local.world_since_ns) last_local.world_since_ns = now;
            last_area = s.area;
            s.flags &= ~COOP_FLAG_IN_WORLD;
        }
        char as[64];
        player_as_name(c, pa, as, sizeof as);
        snprintf(shold_local_as, sizeof shold_local_as, "%s", as);  // (coop.c spawn hold: Link held by an event)
        if (!as[0]) {  // (no AS on the player: a load is running - seen right before that hang)
            last_local.world_since_ns = 0;
            s.flags &= ~COOP_FLAG_IN_WORLD;
        }
        // Dead (DeadRagdoll, game over, then a reload that deletes actors while PlayerInfo still looks placed):
        // keep IN_WORLD for 3.5 s so the peer sees the death animation, then hide until 3 s after reload.
        // Any "...Dead" AS counts (2026-11-12: drowning plays SwimDead; missed, the avatar was re-created during the
        // game-over reload and the game crashed in the Ruta boss probe). Also HorseDead, MasterSwordDead.
        static uint64_t died_at_ns = 0;
        if (strstr(as, "Dead") || starts(as, "Die")) {
            if (!died_at_ns) {
                died_at_ns = now;
                revive_died(now);
            }
            if (now - died_at_ns > 3500000000ull) {
                last_local.world_since_ns = 0;
                s.flags &= ~COOP_FLAG_IN_WORLD;
            }
        } else {
            died_at_ns = 0;
        }
        if (s.flags & COOP_FLAG_IN_WORLD) s.flags |= elemarrow_publish(pa);
        char all[256];  // (every slot: the bow and the shield guard play in the upper-body slots)
        as_list(c, pa, all, sizeof all);
        bool holding_carried = (in_guest(pa) && LD64(pa + 0x88) && LD8(LD64(pa + 0x88) + PROC_STATE) != 3) ||
                               (look_dev.gear[COOP_GEAR_CARRIED].bone != 0);
        s.anim = (uint16_t)classify(as, all, last_local.ground, last_local.vertical, last_local.lateral);
        s.anim = (uint16_t)idle_variant(s.anim, as, all);
        s.anim_upper = (uint16_t)classify_upper(as, all, holding_carried);
        if (s.anim == COOP_ANIM_BOW || s.anim == COOP_ANIM_BOW_SHOOT || s.anim_upper == COOP_ANIM_UPPER_BOW ||
            s.anim_upper == COOP_ANIM_UPPER_BOW_SHOOT || s.anim_upper == COOP_ANIM_UPPER_BOW_RELOAD)
            arrows_bow_ns = now;
        if (s.anim_upper == COOP_ANIM_UPPER_RUNE_AIM) rune_aim_ns = now;  // (n5, runes.inc: our Cryonis may break a copy)
        // Action-instance counter: increments on every distinct action/attack so peer restarts playback
        static uint8_t local_anim_seq = 0;
        static char last_action_as[64] = "";
        static int last_action_anim = -1;
        if (coop_anim_is_action(s.anim)) {
            if (s.anim != last_action_anim || strcmp(as, last_action_as) != 0) {
                local_anim_seq++;
                if (!local_anim_seq) local_anim_seq = 1;
                snprintf(last_action_as, sizeof last_action_as, "%s", as);
                last_action_anim = s.anim;
            }
        } else {
            last_action_as[0] = '\0';
            last_action_anim = -1;
        }
        s.anim_seq = local_anim_seq;

        // Upper-body action-instance counter (protocol 22)
        static uint8_t local_anim_upper_seq = 0;
        static int last_action_upper_anim = -1;
        if (coop_anim_upper_is_action(s.anim_upper)) {
            if (s.anim_upper != last_action_upper_anim) {
                local_anim_upper_seq++;
                if (!local_anim_upper_seq) local_anim_upper_seq = 1;
                last_action_upper_anim = s.anim_upper;
            }
        } else {
            last_action_upper_anim = -1;
        }
        s.anim_upper_seq = local_anim_upper_seq;
        s.face = local_face(c, pa);
        local_aim(c, pa, &s);  // (coop/aim.inc)

        local_look(c, pa, &s);
        fire_held_publish(c, &s);
        astree_publish(c, pa);
        static char last_as[64], last_all[256];
        static int last_anim = -1, last_anim_upper = -1;
        if (strcmp(as, last_as) || s.anim != last_anim || s.anim_upper != last_anim_upper) {  // (for tuning: what the player plays, and what we send)
            LOG("[coop] local AS %s, ground %.2f m/s, vertical %.2f m/s, side %.2f m/s -> slot0 %s, slot1 %s\n", as, last_local.ground,
                last_local.vertical, last_local.lateral, coop_anim_as[s.anim], coop_anim_upper_as[s.anim_upper]);
            snprintf(last_as, sizeof last_as, "%s", as);
            last_anim = s.anim;
            last_anim_upper = s.anim_upper;
        }
        if (strcmp(all, last_all)) {
            if (coop_look_log()) LOG("[coop] local AS slots %s\n", all);
            snprintf(last_all, sizeof last_all, "%s", all);
        }
    } else last_local.have = false, shold_local_as[0] = 0, astree_publish(c, 0);
    look_sent = s;
    // (gate.inc: a chest opened by both. 2026-11-17: big iron chests are only kicked - GetTreasurebox_Kick, no Unlock
    // before it - so the kick counts as opening a chest too; SAVAGEWIND_COOP_CHESTKICK=0: Unlock only, as before.)
    if (s.anim == COOP_ANIM_CHEST_OPEN || (s.anim == COOP_ANIM_CHEST_KICK && look_on("SAVAGEWIND_COOP_CHESTKICK")))
        local_chest_open_ns = plat_now_ns();
    local_anim_now = s.anim;
    magbeam_publish(&s);
    stasisfx_publish(c, &s);
    environment_publish(c, &s);
    coop_net_set_local(&s);
}

// Is the avatar pointer still our actor? The lifecycle below clears it on every transition we know about (area
// change, load, explicit despawn); this is only a last-resort check for an actor the game deleted on its own.
// It uses the game's own weak-handle test (FUN_7101517e60 makes a handle, FUN_7101517e0c checks it): an actor's
// proc unit (+0x68, a pooled object that outlives the actor) holds the live actor at +0x48 and an id at +0x40 that
// must match the actor's id (+0x60); state byte +100 == 3 means it is being deleted. A death "Continue" deletes
// the avatar without a stage-load sign we can see (the player actor and position stay), and the freed memory can
// still read "Dm_CoopLinkE", so a name check alone is not enough (seen 2026-11-07).
#define PROC_UNIT 0x68
#define PROC_ID 0x60
#define UNIT_ID 0x40
#define UNIT_ACTOR 0x48
static void avatar_take_handle(void) {
    avatar.unit = in_guest(avatar.actor) ? LD64(avatar.actor + PROC_UNIT) : 0;
    avatar.unit_id = avatar.unit ? LD32(avatar.actor + PROC_ID) : 0;
}
static bool av_ptr_alive(uint64_t a, uint64_t u, uint32_t id) {
    if (!in_guest(a) || !in_guest(u)) return false;
    if (LD32(u + UNIT_ID) != id || LD64(u + UNIT_ACTOR) != a) return false;
    if (LD8(a + PROC_STATE) == 3) return false;
    return !strncmp((const char *)(uintptr_t)(a + 0x1c), AVATAR_ACTOR, strlen(AVATAR_ACTOR) + 1);
}
static bool avatar_alive(void) {
    return av_ptr_alive(avatar.actor, avatar.unit, avatar.unit_id);
}

static void avatar_inputs_reset(void);
static void astree_forget_target(uint64_t actor);
// Unpublish actor pointers before deleting an actor, or when a game deletion is detected.
static void forget_actor(uint64_t actor) {
    if (!actor) return;
    if (avatar.actor == actor) {
        avatar_inputs_reset();
        avatar.actor = 0, avatar.unit = 0, avatar.unit_id = 0;
        avatar.frames = 0, avatar.have_prev = false;
    } else {
        astree_forget_target(actor);
    }
    if (req.actor == actor) req.actor = 0;
    if (puppet.actor == actor) puppet.actor = 0, puppet.mode = PUPPET_OFF;
}

// ---- the avatar's look (2026-11-08, protocol 14): gear and clothes.
// The avatar has no equipment of its own. Every piece the other player carries (melee weapon, shield, bow, the
// paraglider while gliding) is a physics-free puppet with that item's model (Dm_CoopW_<Sword_044>, Dm_CoopGlider;
// Co-op/build_overlay.py) put every frame on one of the avatar's bones: the bone the real item sits on in the other
// game (hand, or a back holder), times the item's matrix relative to that bone (sent in CoopGear). Bone world matrices
// come from the game's own helper 0x700152f0d4(actor, Matrix34f *out, SafeString *bone) (asks the actor's model at
// +0x4e0; the game's weapon code uses it with "Weapon_R").
// Armor: each piece is a puppet with the armor's model and the Coop* AS (Dm_CoopA_<Armor_001_Upper>), put on the
// avatar's matrix and playing the same AS at the same moment, so its skeleton moves like the avatar's. The body parts
// it covers are hidden on the avatar like the player's own equipment code does (A_MATERIAL_GROUPS with its model).
// SAVAGEWIND_COOP_SWORD=0: no gear puppets; SAVAGEWIND_COOP_ARMOR=0: no armor puppets.
#define A_BONE_MTX 0x700152f0d4ull
#define SWORD_ACTOR "Dm_CoopSword"
#define GLIDER_ACTOR "Dm_CoopGlider"
static const char *const coop_bone_name[COOP_BONE_COUNT] = {"", "Weapon_R", "Weapon_L", "Pod_A", "Pod_B", "Spine_2",
                                                             "Skl_Root", "Arm_2_R"};

static bool pack_exists(const char *actor) {
    const char *dir = getenv("SAVAGEWIND_ROMFS_OVERLAY");
    if (!dir || !*dir) return false;
    char path[600];
    snprintf(path, sizeof path, "%s/Actor/Pack/%.60s.sbactorpack", dir, actor);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// The puppet for a gear name: Dm_CoopW_<Sword_044> when the overlay has one (build_overlay.py makes them for every
// Weapon_Sword/Lsword/Spear/Shield/Bow with a plain model list); melee weapons fall back to the Traveler's Sword.
static void gear_puppet(int kind, const char *name, char out[48]) {
    out[0] = 0;
    if (kind == COOP_GEAR_GLIDER) { snprintf(out, 48, "%s", GLIDER_ACTOR); return; }
    if (kind == COOP_GEAR_SLATE) {  // (49) the Sheikah Slate (model Item_Conductor)
        if (pack_exists("Dm_CoopW_Slate")) snprintf(out, 48, "Dm_CoopW_Slate");
        return;
    }
    if (kind == COOP_GEAR_WEAPON) snprintf(out, 48, "%s", SWORD_ACTOR);
    if (kind == COOP_GEAR_CARRIED) {
        char p[48];
        // (2026-11-19, play report: a half-broken - rotten, Obj_BarrelOld_A_01 - barrel showed as a whole barrel: every
        // *Barrel* got the wooden barrel's prop.) The object's own model first (build_overlay.py makes Dm_CoopW_<name>
        // for carriable barrels, pots, crates); the family prop only when there is none (an older overlay).
        // SAVAGEWIND_COOP_HELDSTATE=0: the family props only, as before.
        static int exact = -1;
        if (exact < 0) exact = look_on("SAVAGEWIND_COOP_HELDSTATE");
        static char last_name[32];
        static bool last_have;
        snprintf(p, sizeof p, "Dm_CoopW_%.31s", name);
        if (exact && strncmp(name, "RemoteBomb", 10)) {
            if (strncmp(last_name, name, sizeof last_name - 1))  // (one file check per new name, not per frame)
                snprintf(last_name, sizeof last_name, "%.31s", name), last_have = pack_exists(p);
            if (last_have) { memcpy(out, p, 48); return; }
        }
        if (strstr(name, "RemoteBombCube")) snprintf(p, sizeof p, "Dm_CoopW_RemoteBombCube");
        else if (strstr(name, "RemoteBomb")) snprintf(p, sizeof p, "Dm_CoopW_RemoteBomb");
        else if (strstr(name, "BarrelBomb")) snprintf(p, sizeof p, "Dm_CoopW_BarrelBomb");
        else if (strstr(name, "Barrel")) snprintf(p, sizeof p, "Dm_CoopW_Barrel");
        else if (strstr(name, "Pot")) snprintf(p, sizeof p, "Dm_CoopW_Pot");
        else if (strstr(name, "KorokStone")) snprintf(p, sizeof p, "Dm_CoopW_KorokStone");
        else snprintf(p, sizeof p, "Dm_CoopW_%.31s", name);
        if (pack_exists(p)) memcpy(out, p, 48);
        return;
    }
    if (strncmp(name, "Weapon_", 7) || !name[7]) return;
    char p[48];
    snprintf(p, sizeof p, "Dm_CoopW_%.31s", name + 7);
    if (pack_exists(p)) memcpy(out, p, 48);
}

static uint64_t bone_mem;  // guest buffer: SafeString at +0, its text at +0x40, the matrix at +0x80

static bool bone_matrix(Cpu *c, uint64_t actor, const char *bone, float out[12]) {
    if (!bone_mem) bone_mem = (uint64_t)(uintptr_t)hle_alloc(0x100);
    if (!bone_mem || !in_guest(actor) || !LD64(actor + 0x4e0) || strlen(bone) >= 0x30) return false;
    memcpy((void *)(uintptr_t)(bone_mem + 0x40), bone, strlen(bone) + 1);
    ST64(bone_mem, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
    ST64(bone_mem + 8, bone_mem + 0x40);
    Cpu saved = *c;
    c->x[0] = actor; c->x[1] = bone_mem + 0x80; c->x[2] = bone_mem;
    guest_call(c, A_BONE_MTX);
    bool ok = c->x[0] & 1;
    *c = saved;
    if (ok) memcpy(out, (void *)(uintptr_t)(bone_mem + 0x80), 48);
    for (int k = 0; ok && k < 12; k++) ok = isfinite(out[k]) && fabsf(out[k]) < 100000.0f;
    return ok;
}

// 3x4 affine matrices (rows of 4, translation in the 4th column): o = a * b, and the inverse.
static void mtx_mul(const float a[12], const float b[12], float o[12]) {
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 4; k++)
            o[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k];
        o[r * 4 + 3] += a[r * 4 + 3];
    }
}
static bool mtx_inv(const float m[12], float o[12]) {
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g, det = a * A + b * B + c * C;
    if (!isfinite(det) || fabsf(det) < 1e-6f) return false;
    float r = 1 / det;
    o[0] = A * r, o[1] = -(b * i - c * h) * r, o[2] = (b * f - c * e) * r;
    o[4] = B * r, o[5] = (a * i - c * g) * r, o[6] = -(a * f - c * d) * r;
    o[8] = C * r, o[9] = -(a * h - b * g) * r, o[10] = (a * e - b * d) * r;
    for (int k = 0; k < 3; k++)
        o[k * 4 + 3] = -(o[k * 4] * m[3] + o[k * 4 + 1] * m[7] + o[k * 4 + 2] * m[11]);
    return true;
}

// -- the local side: find the player's gear (every 0.5 s: the Weapon_*/PlayerStole2 actors sitting on one of the
// player's bones) and, every frame, where each sits relative to its bone.
static bool melee_name(const char *n) {
    return !strncmp(n, "Weapon_Sword_", 13) || !strncmp(n, "Weapon_Lsword_", 14) || !strncmp(n, "Weapon_Spear_", 13);
}
static int gear_kind(const char *n) {
    if (melee_name(n)) return COOP_GEAR_WEAPON;
    if (!strncmp(n, "Weapon_Shield_", 14)) return COOP_GEAR_SHIELD;
    if (!strncmp(n, "Weapon_Bow_", 11)) return COOP_GEAR_BOW;
    if (!strcmp(n, "PlayerStole2")) return COOP_GEAR_GLIDER;
    if (!strcmp(n, "Item_Conductor")) return COOP_GEAR_SLATE;  // (49: the Sheikah Slate; a probe 2026-11-15)
    return -1;
}
static bool gear_name(const char *n) { return gear_kind(n) >= 0; }
#define GEAR_NEAR_M 0.6f     // an item this close to one of the player's bones is carried
#define GLIDER_NEAR_M 2.5f   // (the paraglider's origin is above the head)
#define SURF_NEAR_M 1.6f     // (the shield while shield surfing: under the feet, ~1 m below the root bone)
#define SLATE_NEAR_M 0.9f    // (the Slate on the hip: 0.7 m from the root, its origin away from the pouch bones)
static bool local_surfing;
static float gear_near_m(int k) {
    return k == COOP_GEAR_GLIDER ? GLIDER_NEAR_M : k == COOP_GEAR_SHIELD && local_surfing ? SURF_NEAR_M :
           k == COOP_GEAR_SLATE ? SLATE_NEAR_M : GEAR_NEAR_M;
}
struct Held { uint64_t actor, unit; uint32_t id; int bone; float best; char name[32]; };
static struct Held held[COOP_GEAR_COUNT], held_find[COOP_GEAR_COUNT];
static float bone_now[COOP_BONE_COUNT][12];
static bool bone_have[COOP_BONE_COUNT];

// Dev diagnostic (2026-11-16, play report: "weapon desynced when walking while aiming"), SAVAGEWIND_COOP_AIMDIAG=1.
// Cause not known yet; two hypotheses, one log line each, twice a second while the player aims with gear in a hand:
// - sender ("aimdiag send"): the weapon actor's matrix may be a frame older (or newer) than the hand bone read at the
//   start of the frame; then the offset we send (gear origin - hand bone) grows with the walking speed, mostly along
//   the move ("along" near +-moved). Standing still it is the item's real grip offset.
// - receiver ("aimdiag avatar"): where gear_world put the gear's hand last frame vs where the hand was really drawn
//   (the bone read now); large while walking = our one-frame carry is wrong. Also the slot 1 layer weights.
static uint64_t as_layer_rec(uint64_t actor, int s, int l);  // (coop/astree.inc)
static bool env_on(const char *name, int *cache);  // (coop/enemies.inc)
static const char *as_layer_name(uint64_t rec);
static bool aim_diag_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_COOP_AIMDIAG") && !strcmp(getenv("SAVAGEWIND_COOP_AIMDIAG"), "1");
    return on || aim_diag_dev;
}
static void gear_diag_send(uint64_t pa, const CoopState *s, int k, uint64_t actor, int b, const float m[12]) {
    static float prev[COOP_GEAR_COUNT][3];
    static uint64_t next_ns[COOP_GEAR_COUNT], prev_ns[COOP_GEAR_COUNT];
    if (!aim_diag_on() || k < 0 || k >= COOP_GEAR_COUNT) return;
    const float *pm = (const float *)(uintptr_t)(pa + ACTOR_MTX);
    uint64_t now = plat_now_ns();
    float mv[3] = {pm[3] - prev[k][0], pm[7] - prev[k][1], pm[11] - prev[k][2]};
    bool fresh = prev_ns[k] && now - prev_ns[k] < 100000000ull;  // (moved since the last frame)
    double dt = fresh ? (now - prev_ns[k]) / 1e9 : 0;
    prev[k][0] = pm[3], prev[k][1] = pm[7], prev[k][2] = pm[11], prev_ns[k] = now;
    if (!(s->aim_flags & COOP_AIM_ON) || (b != COOP_BONE_WEAPON_L && b != COOP_BONE_WEAPON_R) || !fresh || now < next_ns[k])
        return;
    next_ns[k] = now + 500000000ull;
    float ml = sqrtf(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]);
    float off[3] = {m[3] - bone_now[b][3], m[7] - bone_now[b][7], m[11] - bone_now[b][11]};
    float ol = sqrtf(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    float along = ml > 1e-4f ? (off[0] * mv[0] + off[1] * mv[1] + off[2] * mv[2]) / ml : 0;
    float across = sqrtf(fmaxf(ol * ol - along * along, 0));
    const CoopGear *g = &s->gear[k];
    LOG("[coop] aimdiag send: %s on %s (actor %llx) speed %.2f m/s, moved %.3f m this frame; gear - hand %.3f m (along the "
        "move %+.3f, across %.3f); rel t %.3f %.3f %.3f; aim %d %d; AS 0.0 %s 1.1 %s\n",
        g->name, coop_bone_name[b], (unsigned long long)actor, dt > 0 ? ml / dt : 0.0, ml, ol, along, across,
        g->rel[3], g->rel[7], g->rel[11], s->aim_pitch, s->aim_yaw, as_layer_name(as_layer_rec(pa, 0, 0)),
        as_layer_name(as_layer_rec(pa, 1, 1)));
}

// (2026-11-18: the weapon on the partner's back was wrong and went through him; play screenshots
// 20:45/20:46: on the avatar a hammer's head hangs in front of the hips and the shield sits inside the back, on the
// real Link both hang behind.) Sheathed weapons, the shield and the bow sit on the back holder bones Pod_A / Pod_B,
// which the player's code poses itself (they are not where the avatar's clip leaves them), so an offset measured from
// the real Pod bone lands elsewhere on the avatar. Sent relative to the chest (Spine_2, posed by the clip on both
// sides) the item keeps its real place on the body. SAVAGEWIND_COOP_BACKSPINE=0: Pod bones as before.
// SAVAGEWIND_COOP_PODDIAG=1 (dev): every 3 s, Pod_A / Pod_B in Spine_2 space on our Link and on the avatar.
static bool backspine_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_BACKSPINE") && !strcmp(getenv("SAVAGEWIND_COOP_BACKSPINE"), "0"));
    return on;
}
static int back_bone(int k, int b) {
    if (!backspine_on() || !bone_have[COOP_BONE_SPINE_2]) return b;
    if (k != COOP_GEAR_WEAPON && k != COOP_GEAR_SHIELD && k != COOP_GEAR_BOW) return b;
    return b == COOP_BONE_POD_A || b == COOP_BONE_POD_B ? COOP_BONE_SPINE_2 : b;
}
// (2026-11-18: the hammer grip was wrong) With a two-handed sword / hammer drawn, the real player's right
// wrist and Weapon_R are turned ~50 deg away from where the animation puts them (PODDIAG, Iron Sledgehammer held:
// Weapon_R x axis 0.17 -0.83 0.54 on the player, 0.67 -0.70 -0.24 on the avatar playing the same AS), while the
// forearm Arm_2_R matches within ~10 deg. The weapon sent relative to Weapon_R therefore stuck out sideways from the
// avatar's hand; sent relative to Arm_2_R it should hang like the real one. Only Weapon_Lsword_* (one-handed swords, bows
// and shields looked right). NOT tried in a game yet: off by default, SAVAGEWIND_COOP_HOLDARM=1 turns it on (both
// players need protocol 72 for the new bone).
static int held_arm_bone(int k, const char *name, int b) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_HOLDARM") && !strcmp(getenv("SAVAGEWIND_COOP_HOLDARM"), "0"));  // (on by default since 2026-11-18 23:20, by design; =0 off)
    if (!on || k != COOP_GEAR_WEAPON || b != COOP_BONE_WEAPON_R || !bone_have[COOP_BONE_ARM_R]) return b;
    return !strncmp(name, "Weapon_Lsword_", 14) ? COOP_BONE_ARM_R : b;
}
static bool bone_matrix(Cpu *c, uint64_t actor, const char *bone, float out[12]);
static void pod_diag(Cpu *c, const char *who, uint64_t actor) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_COOP_PODDIAG") && !strcmp(getenv("SAVAGEWIND_COOP_PODDIAG"), "1");
    static uint64_t next_ns[2];
    int w = who[0] == 'a';
    if (!on || !actor || plat_now_ns() < next_ns[w]) return;
    next_ns[w] = plat_now_ns() + 3000000000ull;
    float sp[12], inv[12], pod[12], r[12];
    if (!bone_matrix(c, actor, "Spine_2", sp) || !mtx_inv(sp, inv)) return;
    static const char *const bones[] = {"Pod_A", "Pod_B", "Weapon_R", "Wrist_R", "Hand_R", "Arm_2_R"};
    for (int p = 0; p < 6; p++) {
        if (!bone_matrix(c, actor, bones[p], pod)) continue;
        mtx_mul(inv, pod, r);
        LOG("[coop] poddiag %s %s in Spine_2: t %.3f %.3f %.3f | x %.2f %.2f %.2f y %.2f %.2f %.2f\n", who,
            bones[p], r[3], r[7], r[11], r[0], r[4], r[8], r[1], r[5], r[9]);
    }
}

static bool held_alive(const struct Held *h) {
    if (!in_guest(h->actor) || !in_guest(h->unit)) return false;
    return LD32(h->unit + UNIT_ID) == h->id && LD64(h->unit + UNIT_ACTOR) == h->actor && LD8(h->actor + PROC_STATE) != 3;
}
// The nearest bone to the item's origin (not the root: an item lying at the player's feet is not carried; except
// while shield surfing, when the shield is under the feet).
// (2026-11-16, play report: "the weapon desyncs when the partner walks while aiming") A held item sits exactly on its
// hand bone (probe_anim2: bow 0.000 m from Weapon_L while aiming, also walking). An item on the back or the hip (sword
// Pod_A, Slate Pod_B / hip) was attached to a hand whenever the hand passed near it (probe: the Slate flipped Pod_B ->
// Spine_2 -> Weapon_L every few frames while aiming, 0.20 m from the hand), and was then drawn riding on the avatar's
// hand: a hand bone counts only within HAND_HOLD_M (probe 2026-11-16: every held item 0.000 m; Slate on the
// hip 0.06+). SAVAGEWIND_COOP_HANDHOLD=0: off (the nearest bone, as before).
#define HAND_HOLD_M 0.02f
static bool hand_hold_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_HANDHOLD") && !strcmp(getenv("SAVAGEWIND_COOP_HANDHOLD"), "0"));
    return on;
}
static bool hand_bone(int b) { return b == COOP_BONE_WEAPON_R || b == COOP_BONE_WEAPON_L; }
static float bone_dist(const float m[12], int b) {
    float dx = m[3] - bone_now[b][3], dy = m[7] - bone_now[b][7], dz = m[11] - bone_now[b][11];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
static int nearest_bone(const float m[12], float *dist) {
    int best = COOP_BONE_NONE;
    *dist = 1e9f;
    for (int b = COOP_BONE_WEAPON_R; b < (local_surfing ? COOP_BONE_COUNT : COOP_BONE_ROOT); b++) {
        if (!bone_have[b] || b == COOP_BONE_ARM_R) continue;  // (ARM_R only by held_arm_bone)
        float d = bone_dist(m, b);
        if (hand_bone(b) && d > HAND_HOLD_M && hand_hold_on()) continue;
        if (d < *dist) *dist = d, best = b;
    }
    return best;
}
static void gear_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c; (void)ctx;
    int k = gear_kind(name);
    if (k < 0) return;
    float m[12], d;
    memcpy(m, (void *)(uintptr_t)(a + ACTOR_MTX), sizeof m);
    nearest_bone(m, &d);
    if (d >= held_find[k].best) return;
    struct Held *h = &held_find[k];
    h->best = d, h->actor = a, h->unit = LD64(a + PROC_UNIT), h->id = LD32(a + PROC_ID);
    snprintf(h->name, sizeof h->name, "%s", name);
}

static int walk_actors(Cpu *c, bool (*want)(const char *name), void (*fn)(Cpu *, uint64_t, const char *, void *),
                       void *ctx);
static uint64_t look_scan_ns;
static bool look_on(const char *env);
// (protocol 49, 2026-11-15: held items, like apples, did not show on the other player's screen, only when
// dropped). probe 2026-11-15: items held from the inventory are their own actors
// ("Item_Fruit_A" x2 at the hands) and the player's carried pointer (+0x88) is an invisible "CarryBox" actor that
// holds them; it was sent as the carried gear, but no puppet has that name, so nothing showed. Now each item near the
// box is sent with its position in box space; the other game shows a model of each (Dm_CoopW_<item>, build_overlay.py).
// SAVAGEWIND_COOP_HELDITEMS=0: off.
static struct Held held_items[COOP_HELD_ITEMS];
static uint64_t held_items_box, held_items_scan_ns;
static int held_items_n;
static bool held_item_name(const char *n) {
    return (!strncmp(n, "Item_", 5) && strcmp(n, "Item_Conductor")) || !strncmp(n, "Animal_", 7) ||
           !strncmp(n, "BeeHome", 7) || !strncmp(n, "Obj_", 4);
}
static void held_item_seen(Cpu *c, uint64_t a, const char *name, void *ctx) {
    (void)c;
    uint64_t box = (uint64_t)(uintptr_t)ctx;
    if (held_items_n >= COOP_HELD_ITEMS || a == box) return;
    const float *m = (const float *)(uintptr_t)(a + ACTOR_MTX), *b = (const float *)(uintptr_t)(box + ACTOR_MTX);
    float dx = m[3] - b[3], dy = m[7] - b[7], dz = m[11] - b[11];
    if (dx * dx + dy * dy + dz * dz > 0.8f * 0.8f || LD8(a + PROC_STATE) == 3) return;
    struct Held *h = &held_items[held_items_n++];
    h->actor = a, h->unit = LD64(a + PROC_UNIT), h->id = LD32(a + PROC_ID), h->best = sqrtf(dx * dx + dy * dy + dz * dz);
    snprintf(h->name, sizeof h->name, "%s", name);
}
// Every frame while the CarryBox is the carried actor: the items in it (actor scan every 0.3 s), in box space.
static void held_items_fill(Cpu *c, uint64_t box, CoopState *s) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_HELDITEMS");
    if (!on) return;
    uint64_t now = plat_now_ns();
    if (box != held_items_box || now >= held_items_scan_ns) {
        int was = held_items_n;
        held_items_box = box, held_items_scan_ns = now + 300000000ull, held_items_n = 0;
        memset(held_items, 0, sizeof held_items);
        walk_actors(c, held_item_name, held_item_seen, (void *)(uintptr_t)box);
        if (held_items_n != was && coop_look_log())
            LOG("[coop] look: holding %d item(s) in the hands%s%s\n", held_items_n, held_items_n ? ", first " : "",
                held_items_n ? held_items[0].name : "");
    }
    float inv[12];
    if (!mtx_inv((const float *)(uintptr_t)(box + ACTOR_MTX), inv)) return;
    for (int i = 0; i < held_items_n; i++) {
        struct Held *h = &held_items[i];
        if (!held_alive(h)) continue;
        const float *m = (const float *)(uintptr_t)(h->actor + ACTOR_MTX);
        CoopHeldItem *o = &s->held[i];
        snprintf(o->name, sizeof o->name, "%s", h->name);
        for (int r = 0; r < 3; r++)
            o->pos[r] = inv[r * 4] * m[3] + inv[r * 4 + 1] * m[7] + inv[r * 4 + 2] * m[11] + inv[r * 4 + 3];
    }
}
// `coop look armor <0-2> <name|-|off>`, `coop look gear <0-3> <name|-|off> [bone 1-6] [dx dy dz]` (dev): bone 0 = not
// forced, COOP_BONE_COUNT = forced to none.
static bool carry_last_shared;  // (objects.inc: what our Link holds / held last is a shared object)
static void local_look(Cpu *c, uint64_t pa, CoopState *s) {
    static char armor[COOP_ARMOR_SLOTS][32];
    uint64_t now = plat_now_ns();
    local_surfing = coop_anim_is_surf(s->anim);
    for (int b = 1; b < COOP_BONE_COUNT; b++) bone_have[b] = bone_matrix(c, pa, coop_bone_name[b], bone_now[b]);
    pod_diag(c, "own", pa);
    // (2026-11-12, user: the paraglider shows late on the avatar) the glider is sent from the opening on
    // (ParaEquipOn, and while closing); opening or gliding without a known paraglider actor: scan again at once
    // (at most every 100 ms) instead of waiting up to 0.5 s. SAVAGEWIND_COOP_GLIDER_EARLY=0: only while gliding.
    static int glider_early = -1;
    if (glider_early < 0) glider_early = look_on("SAVAGEWIND_COOP_GLIDER_EARLY");
    bool para = s->anim == COOP_ANIM_GLIDE || (glider_early && (s->anim == COOP_ANIM_PARA_ON || s->anim == COOP_ANIM_PARA_OFF));
    static uint64_t glider_rescan_ns;
    if (glider_early && para && (!held[COOP_GEAR_GLIDER].name[0] || !held_alive(&held[COOP_GEAR_GLIDER])) &&
        now >= glider_rescan_ns && look_scan_ns > now) {
        glider_rescan_ns = now + 100000000ull;
        look_scan_ns = now;
        if (coop_look_log()) LOG("[coop] look: gliding without a known paraglider: gear scan now\n");
    }
    if (now >= look_scan_ns) {
        look_scan_ns = now + 500000000ull;
        for (int k = 0; k < COOP_GEAR_COUNT; k++) {
            memset(&held_find[k], 0, sizeof held_find[k]);
            held_find[k].best = gear_near_m(k);
        }
        walk_actors(c, gear_name, gear_seen, NULL);
        for (int k = 0; k < COOP_GEAR_COUNT; k++) {
            if (strcmp(held_find[k].name, held[k].name) && coop_look_log())
                LOG("[coop] look: gear %d %s -> %s (%.2f m from a bone)\n", k, held[k].name[0] ? held[k].name : "-",
                    held_find[k].name[0] ? held_find[k].name : "-", held_find[k].name[0] ? held_find[k].best : 0.0f);
            int bone = held[k].actor == held_find[k].actor ? held[k].bone : COOP_BONE_NONE;
            held[k] = held_find[k];
            held[k].bone = bone;
        }
        // Armor: the player's equipment component (player actor + 0x2500), slots +0x10/+0x20/+0x30 = head/upper/lower.
        uint64_t equip = LD64(pa + PLAYER_EQUIP);
        if (!outfit_out) outfit_out = (uint64_t)(uintptr_t)hle_alloc(0x20);
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
            char nm[32] = "";
            if (in_guest(equip) && outfit_out) {
                memset((void *)(uintptr_t)outfit_out, 0, 0x20);
                Cpu saved = *c;
                c->x[0] = equip + 0x10 + (uint64_t)i * 0x10; c->x[1] = outfit_out;
                guest_call(c, A_EQUIP_GET);
                *c = saved;
                uint64_t it = LD64(outfit_out + 8);
                const char *in = in_guest(it) ? (const char *)(uintptr_t)(it + 0x1c) : "";
                if (!strncmp(in, "Armor_", 6) && strncmp(in, "Armor_Default", 13))  // (Default: no armor, bare Link)
                    snprintf(nm, sizeof nm, "%.31s", in);
            }
            if (strcmp(nm, armor[i])) {
                LOG("[coop] look: armor %d %s -> %s\n", i, armor[i][0] ? armor[i] : "-", nm[0] ? nm : "-");
                memcpy(armor[i], nm, sizeof nm);
            }
        }
    }
    memcpy(s->armor, armor, sizeof armor);
    for (int k = 0; k < COOP_GEAR_COUNT; k++) {
        CoopGear *g = &s->gear[k];
        struct Held *h = &held[k];
        if (!h->name[0] || !held_alive(h)) continue;
        if (k == COOP_GEAR_GLIDER && !para) continue;  // (it hangs around hidden otherwise)
        float m[12], d, inv[12];
        memcpy(m, (void *)(uintptr_t)(h->actor + ACTOR_MTX), sizeof m);
        int b = nearest_bone(m, &d);
        // keep the bone it had unless another is clearly nearer (no flicker between two close bones)
        if (h->bone != COOP_BONE_NONE && h->bone != b && bone_have[h->bone]) {
            float dh = bone_dist(m, h->bone);
            if (dh < d + 0.05f && !(hand_bone(h->bone) && dh > HAND_HOLD_M && hand_hold_on())) b = h->bone, d = dh;
        }
        if (b == COOP_BONE_NONE || d > gear_near_m(k) + 0.3f) {
            static uint64_t said_ns;
            if (coop_look_log() && plat_now_ns() > said_ns)
                said_ns = plat_now_ns() + 500000000ull, LOG("[coop] look: %s %.2f m from the nearest bone: not sent\n", h->name, d);
            continue;
        }
        if (b != h->bone && coop_look_log()) LOG("[coop] look: %s now on %s (%.3f m)\n", h->name, coop_bone_name[b], d);
        h->bone = b;
        b = back_bone(k, b);
        b = held_arm_bone(k, h->name, b);
        if (!mtx_inv(bone_now[b], inv)) continue;
        snprintf(g->name, sizeof g->name, "%s", h->name);
        g->bone = (uint8_t)b;
        mtx_mul(inv, m, g->rel);
        if (b == COOP_BONE_ARM_R && coop_look_log() && bone_have[COOP_BONE_WEAPON_R]) {  // (HOLDWRIST assumes ~identity)
            static uint64_t said_ns;
            float wi[12], gw[12];
            if (plat_now_ns() > said_ns && mtx_inv(bone_now[COOP_BONE_WEAPON_R], wi)) {
                said_ns = plat_now_ns() + 5000000000ull;
                mtx_mul(wi, m, gw);
                float tr = (gw[0] + gw[5] + gw[10] - 1) * 0.5f;
                LOG("[coop] look: held %s vs our Weapon_R: %.3f m, %.0f deg\n", h->name,
                    sqrtf(gw[3] * gw[3] + gw[7] * gw[7] + gw[11] * gw[11]), acosf(fmaxf(-1, fminf(1, tr))) * 57.29578f);
            }
        }
        gear_diag_send(pa, s, k, h->actor, b, m);
    }
    // (2026-11-15, user: in the map / inventory the avatar takes the Slate pose with empty hands - the real Slate stays
    // on the hip while the game is paused). Remember the Slate's place the last time our Link held it in a hand (rune,
    // scope) and send that while the paused Slate pose is sent. Before the first rune use: a default place in the left
    // hand. Off with the pose itself: SAVAGEWIND_COOP_PAUSE_PAD=0.
    {
        static CoopGear slate_hand;
        CoopGear *g = &s->gear[COOP_GEAR_SLATE];
        bool paused_pad = plat_now_ns() < astree_pad_until_ns;
        if (!paused_pad && g->name[0] && (g->bone == COOP_BONE_WEAPON_L || g->bone == COOP_BONE_WEAPON_R)) {
            if (!slate_hand.name[0])
                LOG("[coop] look: Slate seen in the hand (%s): kept for the menu pose, rel %.3f %.3f %.3f %.3f | %.3f %.3f "
                    "%.3f %.3f | %.3f %.3f %.3f %.3f\n", coop_bone_name[g->bone], g->rel[0], g->rel[1], g->rel[2],
                    g->rel[3], g->rel[4], g->rel[5], g->rel[6], g->rel[7], g->rel[8], g->rel[9], g->rel[10], g->rel[11]);
            slate_hand = *g;
        }
        // (2026-11-16) before any rune use: a default place in the left hand. polish2: the Slate held ready for a rune
        // is sent exactly on the Weapon_L bone (rel = identity, logged in a probe), so that is the default
        // now that the menu pose is the rune-ready pose; off with SAVAGEWIND_COOP_PAUSE_PAD=0
        static const float slate_rel_default[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (paused_pad && slate_hand.name[0]) *g = slate_hand;
        else if (paused_pad && g->name[0]) g->bone = COOP_BONE_WEAPON_L, memcpy(g->rel, slate_rel_default, sizeof g->rel);
    }
    // Carried prop (protocol 21: remote bombs, barrels, pots, rocks carried overhead)
    static char last_carried_name[32] = "";
    static float last_carried_rel[12];
    static uint64_t carried_until_ns = 0;
    // (polish2, 2026-11-20) Put down (GrabPut): the player's carried pointer clears at the start of the put-down, so
    // the prop vanished from the avatar's hands while the arms were still lowering, and a shared object (barrel, bomb)
    // appeared on the floor only ~0.3-0.5 s later. Keep sending the prop at the REAL object's current place (relative
    // to the spine) while our Link still plays GrabPut, at most 1.2 s after the release. Off: SAVAGEWIND_COOP_PUTHOLD=0.
    static uint64_t put_actor = 0, put_until_ns = 0;
    static int put_hold = -1;
    static unsigned put_said;
    // (default ON since 2026-11-20: seen in a two-instance test run - the guest put a lifted rock down,
    // the host's burst shows the rock still in the avatar's lowering hands, then on the floor; =0 turns it off)
    if (put_hold < 0) put_hold = !(getenv("SAVAGEWIND_COOP_PUTHOLD") && getenv("SAVAGEWIND_COOP_PUTHOLD")[0] == '0');
    uint64_t carried = in_guest(pa) ? LD64(pa + 0x88) : 0;
    // (protocol 30: a shared object - objects.inc - flies as the real object in the other game from its release on;
    // keeping its prop on the avatar for the throw would show it twice)
    if (carry_last_shared) carried_until_ns = 0;
    // (49: the CarryBox of items held from the inventory has no model at +0x4e0: let it through for its items)
    if (in_guest(carried) && (LD64(carried + 0x4e0) || !strcmp((const char *)(uintptr_t)(carried + 0x1c), COOP_CARRY_BOX)) &&
        LD8(carried + PROC_STATE) != 3) {
        const char *cname = (const char *)(uintptr_t)(carried + 0x1c);
        if (cname && cname[0] && bone_have[COOP_BONE_SPINE_2]) {
            float m[12], inv[12];
            memcpy(m, (void *)(uintptr_t)(carried + ACTOR_MTX), sizeof m);
            if (mtx_inv(bone_now[COOP_BONE_SPINE_2], inv)) {
                CoopGear *g = &s->gear[COOP_GEAR_CARRIED];
                snprintf(g->name, sizeof g->name, "%s", cname);
                g->bone = COOP_BONE_SPINE_2;
                mtx_mul(inv, m, g->rel);
                snprintf(last_carried_name, sizeof last_carried_name, "%s", cname);
                memcpy(last_carried_rel, g->rel, sizeof last_carried_rel);
                carried_until_ns = now + 400000000ull;
                put_actor = carried, put_until_ns = now + 1200000000ull;
                if (!strcmp(cname, COOP_CARRY_BOX)) held_items_fill(c, carried, s);
            }
        }
    } else if ((s->anim == COOP_ANIM_CARRY_THROW || s->anim_upper == COOP_ANIM_UPPER_CARRY_THROW) && now < carried_until_ns && last_carried_name[0]) {
        // Wind-up and release of throw: hold prop for ~0.4s so the throw doesn't show empty hands
        CoopGear *g = &s->gear[COOP_GEAR_CARRIED];
        snprintf(g->name, sizeof g->name, "%s", last_carried_name);
        g->bone = COOP_BONE_SPINE_2;
        memcpy(g->rel, last_carried_rel, sizeof g->rel);
    } else if (put_hold && s->anim == COOP_ANIM_GRAB_PUT && now < put_until_ns && last_carried_name[0] &&
               in_guest(put_actor) && LD8(put_actor + PROC_STATE) != 3 &&
               !strncmp((const char *)(uintptr_t)(put_actor + 0x1c), last_carried_name, sizeof last_carried_name - 1) &&
               strcmp(last_carried_name, COOP_CARRY_BOX) && bone_have[COOP_BONE_SPINE_2]) {
        float inv[12];
        if (mtx_inv(bone_now[COOP_BONE_SPINE_2], inv)) {
            CoopGear *g = &s->gear[COOP_GEAR_CARRIED];
            snprintf(g->name, sizeof g->name, "%s", last_carried_name);
            g->bone = COOP_BONE_SPINE_2;
            mtx_mul(inv, (const float *)(uintptr_t)(put_actor + ACTOR_MTX), g->rel);
            if (put_said++ < 20 || coop_look_log())
                LOG("[coop] look: put down %s: prop kept at the real object (rel pos %.2f %.2f %.2f)\n", last_carried_name,
                    g->rel[3], g->rel[7], g->rel[11]);
        }
    } else {
        last_carried_name[0] = '\0';
        // (2026-11-14) nothing carried: the arrow on our bow string, on the right hand (SAVAGEWIND_COOP_ARROWSHOW=0: off)
        static int arrow_show = -1;
        if (arrow_show < 0) arrow_show = look_on("SAVAGEWIND_COOP_ARROWSHOW");
        uint64_t ar = arrow_show && now - arrows_bow_ns < 1000000000ull ? arrow_on_string() : 0;
        float inv[12];
        if (ar && bone_have[COOP_BONE_WEAPON_R] && mtx_inv(bone_now[COOP_BONE_WEAPON_R], inv)) {
            CoopGear *g = &s->gear[COOP_GEAR_CARRIED];
            snprintf(g->name, sizeof g->name, "%.31s", (const char *)(uintptr_t)(ar + 0x1c));
            g->bone = COOP_BONE_WEAPON_R;
            mtx_mul(inv, (const float *)(uintptr_t)(ar + ACTOR_MTX), g->rel);
        }
    }
    // `coop look ...` (dev, tests): send this armor / gear instead of what the player has
    if (look_dev.anim >= 0 && look_dev.anim < COOP_ANIM_COUNT) s->anim = (uint16_t)look_dev.anim;
    if (look_dev.anim_upper >= 0 && look_dev.anim_upper < COOP_ANIM_UPPER_COUNT) s->anim_upper = (uint16_t)look_dev.anim_upper;
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++)
        if (look_dev.armor[i][0]) snprintf(s->armor[i], sizeof s->armor[i], "%s", strcmp(look_dev.armor[i], "-") ? look_dev.armor[i] : "");
    for (int k = 0; k < COOP_GEAR_COUNT; k++)
        if (look_dev.gear[k].bone) {
            s->gear[k] = look_dev.gear[k];
            if (look_dev.gear[k].bone >= COOP_BONE_COUNT) memset(&s->gear[k], 0, sizeof s->gear[k]);  // ("-": none)
        }
    memcpy(s->weapon, s->gear[COOP_GEAR_WEAPON].name, sizeof s->weapon);  // (protocol 11 field, kept filled)
}

// -- the avatar side
struct Pup { uint64_t actor, unit, retry_ns; uint32_t id, area; unsigned frames; bool said; uint8_t bow_pose; char name[48]; };
static struct Pup gear_pup[COOP_GEAR_COUNT], armor_pup[COOP_ARMOR_SLOTS], held_pup[COOP_HELD_ITEMS];
static bool pup_alive(const struct Pup *p) {
    if (!in_guest(p->actor) || !in_guest(p->unit)) return false;
    return LD32(p->unit + UNIT_ID) == p->id && LD64(p->unit + UNIT_ACTOR) == p->actor && LD8(p->actor + PROC_STATE) != 3;
}
static void pup_drop(Cpu *c, struct Pup *p) {
    uint64_t a = p->actor;
    // (split-screen playtest 2026-11-26, after a shrine door scene: "a piece of link was left flying". The scene's end
    // makes us "not in the world" for 3 s with no stage load; the avatar itself was deleted, its puppets were only
    // forgotten and stayed where they were. Now a live puppet is deleted unless the area changed (a load deletes it).)
    // SAVAGEWIND_COOP_PUPDEL=0: off (forgotten only when not in the world, as before).
    char area[96];
    static int pupdel_c = -1;
    bool del = a && pup_alive(p) && (local_in_world() || (env_on("SAVAGEWIND_COOP_PUPDEL", &pupdel_c) &&
                                                          p->area == local_area(area, sizeof area)));
    forget_actor(a);
    p->actor = 0, p->unit = 0, p->id = 0, p->frames = 0, p->name[0] = 0;
    if (del) despawn_now(c, a, ACTOR_DELETE_REASON);
}
// Make sure the puppet is `want` (deleted when it is something else); true while it exists. Created at the avatar,
// pup_spawn_drop metres lower (the collision body: never created inside our Link).
static float pup_spawn_drop;
static bool pup_need(Cpu *c, struct Pup *p, const char *want) {
    uint64_t now = plat_now_ns();
    if (p->actor && (!local_in_world() || !pup_alive(p))) pup_drop(c, p);  // (a load deleted it)
    if (p->actor && strcmp(p->name, want)) pup_drop(c, p);
    if (!want[0]) return false;
    if (p->actor) return true;
    if (now < p->retry_ns || spawn_hold()) return false;  // (spawn hold: no creation while an event / load runs)
    p->retry_ns = now + 2000000000ull;
    float am[12], at[3];  // (created at the avatar, then put in place)
    memcpy(am, (void *)(uintptr_t)(avatar.actor + ACTOR_MTX), sizeof am);
    at[0] = am[3], at[1] = am[7] - pup_spawn_drop, at[2] = am[11];
    p->actor = spawn_now(c, want, at);
    if (!p->actor) {
        if (!p->said) p->said = true, LOG("[coop] look: could not create %s\n", want);
        return false;
    }
    if (coop_look_log()) heap_report(c, want, p->actor, want);  // (its own heap: actor+0x5b8, size = ActorInfo instSize)
    p->unit = LD64(p->actor + PROC_UNIT), p->id = LD32(p->actor + PROC_ID), p->frames = 0, p->bow_pose = 0;
    char area[96];
    p->area = local_area(area, sizeof area);
    snprintf(p->name, sizeof p->name, "%s", want);
    LOG("[coop] look: %s on the avatar (%llx)\n", want, (unsigned long long)p->actor);
    return true;
}

static bool look_on(const char *env) {
    return !(getenv(env) && !strcmp(getenv(env), "0"));
}

// The paraglider puppet (2026-11-12, user: it appears late on the avatar, and the little wind trails at the wing tips
// are missing).
// - It waits: created as soon as the avatar is ready and kept, parked GLIDER_PARK_M under the avatar, while the other
//   player does not glide; opening the glider then only moves it into place (no creation and model load first).
//   SAVAGEWIND_COOP_GLIDERWAIT=0: created only while gliding (as before).
// - The trails: the puppet keeps the paraglider's effect set (ActorInfo elink PlayerStole2: asset "Tail" = Tail_00 /
//   Tail_01, and a local property "horizontal speed"). HYPOTHESIS (not verified in Ghidra): that speed comes from the
//   actor's velocity (+0x400), which nobody wrote on the puppet (in tree mode the avatar got the sender's velocity and
//   then streaks showed on it). So while gliding it gets the avatar's motion of this frame (m / frame), 0 when parked.
//   NOT WORKING (probe 2026-11-12): read back during a glide, the puppet's +0x400 held ~0.0003 instead of the ~0.02
//   m/frame written (the game's own update of the puppet seems to overwrite it), and no trails were seen. Writing it
//   after the puppet's own update (like astree_npc_hook does for the AS inputs) is the next thing to try.
//   SAVAGEWIND_COOP_GLIDERTRAIL=0: no velocity written.
#define GLIDER_PARK_M 60.0f
static void glider_trail(uint64_t actor, bool moving) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_GLIDERTRAIL");
    if (!on || !in_guest(actor)) return;
    float v[3] = {0, 0, 0};
    if (moving && avatar.have_prev) {
        const float *now = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
        v[0] = now[3] - avatar.mtx_prev[3], v[1] = now[7] - avatar.mtx_prev[7], v[2] = now[11] - avatar.mtx_prev[11];
        for (int i = 0; i < 3; i++)
            if (!isfinite(v[i]) || fabsf(v[i]) > 5.0f) v[i] = 0;  // (a warp, not a glide)
    }
    memcpy((void *)(uintptr_t)(actor + 0x400), v, sizeof v);
    static bool said;
    if (moving && !said && (v[0] != 0 || v[2] != 0)) {
        said = true;
        LOG("[coop] look: paraglider puppet gets the avatar's speed (%.3f %.3f %.3f m/frame) for its trails\n", v[0], v[1], v[2]);
    }
}
// The wing-tip trails themselves (2026-11-12). Found: the paraglider's effect set PlayerStole2 has one action slot
// "Act" with one action "開く" (open: the real paraglider's AI action PlayerStoleOpen), whose action trigger starts
// the trails; the puppet's AI (DemoDummy) never has that action, so its speed property alone (ELink property 9
// "水平速度", read live: ~0.2 on the puppet, 0.35 on a real glide) starts nothing. So the puppet fires the effect key
// "Tail" itself, through the game's XLink event call (FUN_710159f00c(actor, key, 0 = ELink, no handle), what the game
// uses for "Break" etc.), once it has been in place for a few frames of a glide (not while it moves up from its parked
// spot: that would draw a 60 m streak). The key keeps playing (no handle to stop it), so when the glide ends the puppet
// is deleted (its effects go with it) and glider_wait_frame parks a fresh one. SAVAGEWIND_COOP_GLIDERTAILKEY=0: off.
#define A_XLINK_EVENT 0x700159f00cull
#define S_XLINK_TAIL 0x70016550c2ull  // ("Tail" in the game's own strings; checked before use)
static struct { uint64_t actor; uint32_t id; int frames; bool fired; } gtail;
static void glider_tail_key(Cpu *c, struct Pup *p, bool gliding) {
    static int on = -1;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_GLIDERTAILKEY") && !memcmp((const void *)(uintptr_t)S_XLINK_TAIL, "Tail", 5);
        LOG("[coop] look: paraglider wing-tip trails (effect key Tail on the glider puppet): %s\n", on ? "on" : "off");
    }
    if (!on || !p->actor) return;
    if (gtail.actor != p->actor || gtail.id != p->id) gtail.actor = p->actor, gtail.id = p->id, gtail.frames = 0, gtail.fired = false;
    if (!gliding) {
        gtail.frames = 0;
        if (gtail.fired) {
            LOG("[coop] look: glide over: paraglider puppet replaced (its trails end with it)\n");
            pup_drop(c, p);
            gtail.actor = 0, gtail.fired = false;
        }
        return;
    }
    if (gtail.fired || ++gtail.frames < 4) return;
    Cpu saved = *c;
    c->x[0] = p->actor, c->x[1] = S_XLINK_TAIL, c->x[2] = 0, c->x[3] = 0;
    guest_call(c, A_XLINK_EVENT);
    *c = saved;
    gtail.fired = true;
    LOG("[coop] look: paraglider puppet trails on (effect key Tail)\n");
}
// DEV probe (SAVAGEWIND_COOP_XPROBE=1): the effect-set (ELink) properties of an actor: user instance =
// *(*(actor+0x568)+0x48), property table *(ui+0x30) (+0x44 count u16, +0x48 defs, def+0x10 name, def+0x60 type),
// values ui+0x70 (FUN_7100f515ec = setPropertyValue float).
static void xprobe_dump(const char *tag, uint64_t a) {
    if (!in_guest(a)) { LOG("[coop] xprobe %s: no actor\n", tag); return; }
    uint64_t lh = LD64(a + 0x568), ui = in_guest(lh) ? LD64(lh + 0x48) : 0;
    if (!in_guest(ui)) { LOG("[coop] xprobe %s: no elink (lh %llx)\n", tag, (unsigned long long)lh); return; }
    uint64_t tb = LD64(ui + 0x30), vals = LD64(ui + 0x70);
    if (!in_guest(tb) || !in_guest(vals)) { LOG("[coop] xprobe %s: tb %llx vals %llx\n", tag, (unsigned long long)tb, (unsigned long long)vals); return; }
    unsigned n = LD32(tb + 0x44) & 0xffff;
    uint64_t defs = LD64(tb + 0x48);
    const float *v = (const float *)(uintptr_t)(a + 0x400);
    char line[1024];
    int o = snprintf(line, sizeof line, "vel %.4f %.4f %.4f n %u:", v[0], v[1], v[2], n);
    for (unsigned i = 0; i < n && i < 64 && in_guest(defs) && o < (int)sizeof line - 64; i++) {
        uint64_t d = LD64(defs + i * 8);
        if (!in_guest(d)) continue;
        uint64_t nm = LD64(d + 0x10);
        int ty = (int)LD32(d + 0x60);
        float f = *(const float *)(uintptr_t)(vals + i * 4);
        int iv = (int)LD32(vals + i * 4);
        if (f == 0 && iv == 0) continue;  // (only non-zero ones)
        o += snprintf(line + o, sizeof line - o, " %u:%.24s(t%d)=%g/%d", i, in_guest(nm) ? (const char *)(uintptr_t)nm : "?", ty, f, iv);
    }
    LOG("[coop] xprobe %s %llx: %s\n", tag, (unsigned long long)a, line);
}
static void xprobe_frame(uint64_t glider_pup) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_COOP_XPROBE") && !strcmp(getenv("SAVAGEWIND_COOP_XPROBE"), "1");
    if (!on) return;
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (now - last < 1000000000ull) return;
    last = now;
    xprobe_dump("gliderpup", glider_pup);
    xprobe_dump("avatar", avatar.actor);
    if (held[COOP_GEAR_GLIDER].name[0] && held_alive(&held[COOP_GEAR_GLIDER])) xprobe_dump("myglider", held[COOP_GEAR_GLIDER].actor);
}
// true when it handled the glider puppet this frame (parked); false: gear_frame's usual path (want "" drops it).
static bool glider_wait_frame(Cpu *c, struct Pup *p, const char *want) {
    static int on = -1;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_GLIDERWAIT") && look_on("SAVAGEWIND_COOP_SWORD");
        LOG("[coop] look: paraglider puppet waits under the avatar: %s\n", on ? "on" : "off");
    }
    if (want[0] || !on) return false;
    if (!(avatar.actor && avatar_alive() && avatar.frames >= 6)) return false;
    glider_tail_key(c, p, false);  // (a puppet that showed trails is replaced)
    bool had = p->actor != 0;
    if (!pup_need(c, p, GLIDER_ACTOR)) return true;
    if (!had) LOG("[coop] look: paraglider puppet ready (parked %.0f m under the avatar)\n", GLIDER_PARK_M);
    float m[12];
    memcpy(m, (void *)(uintptr_t)(avatar.actor + ACTOR_MTX), sizeof m);
    m[7] -= GLIDER_PARK_M;
    memcpy((void *)(uintptr_t)(p->actor + ACTOR_MTX), m, sizeof m);
    memcpy((void *)(uintptr_t)(p->actor + ACTOR_MTX2), m, sizeof m);
    glider_trail(p->actor, false);
    xprobe_frame(p->actor);
    return true;
}

// A gear piece's matrix on the avatar this frame: its bone (carried along the avatar's move of this frame) times rel.
static bool avatar_bone_now(Cpu *c, const char *bone, float b[12]) {
    if (!bone_matrix(c, avatar.actor, bone, b)) return false;
    // The bone is where the last model update put it, with the avatar's matrix of before this frame's move: carry
    // it along that move (else the gear trails the avatar by a frame of motion when it runs or jumps).
    float inv[12], delta[12], bn[12];
    if (avatar.have_prev && mtx_inv(avatar.mtx_prev, inv)) {
        mtx_mul((const float *)(uintptr_t)(avatar.actor + ACTOR_MTX), inv, delta);
        mtx_mul(delta, b, bn);
        memcpy(b, bn, sizeof bn);
    }
    return true;
}
// (2026-11-19, user screenshot user_hammer_grip_1118.webp: with HOLDARM the hammer's angle is right but "not connected
// to his hand" - the handle end sticks out past the avatar's fist.) On the real Link a held weapon's origin (its grip
// point) is exactly on Weapon_R (PODDIAG 0.000 m). The forearm-relative place keeps the real Link's wrist offset,
// and the real wrist is turned ~50 deg away from the avatar's, so the grip point landed a few cm off the avatar's
// hand along the handle. Now the forearm only gives the angle: the grip point goes on the avatar's own Weapon_R.
// Receiver side only (no protocol change). SAVAGEWIND_COOP_HOLDGRIP=0: the whole place from the forearm as before.
static bool hold_grip_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_HOLDGRIP") && !strcmp(getenv("SAVAGEWIND_COOP_HOLDGRIP"), "0"));
    return on;
}
static bool gear_world(Cpu *c, const CoopGear *g, float m[12]) {
    float b[12], h[12];
    if (!avatar_bone_now(c, coop_bone_name[g->bone], b)) return false;
    mtx_mul(b, g->rel, m);
    if (g->bone == COOP_BONE_ARM_R && hold_grip_on() && avatar_bone_now(c, "Weapon_R", h)) {
        static uint64_t said_ns;
        if (coop_look_log() && plat_now_ns() > said_ns) {
            float dx = m[3] - h[3], dy = m[7] - h[7], dz = m[11] - h[11];
            said_ns = plat_now_ns() + 3000000000ull;
            LOG("[coop] look: %s grip moved %.3f m onto the avatar's Weapon_R (HOLDGRIP)\n", g->name,
                sqrtf(dx * dx + dy * dy + dz * dz));
        }
        m[3] = h[3], m[7] = h[7], m[11] = h[11];
    }
    return true;
}

// (dev, SAVAGEWIND_COOP_AIMDIAG=1: see gear_diag_send) the receiver's side, right after gear_world placed gear k.
static void gear_diag_recv(Cpu *c, const CoopState *rs, int k, const CoopGear *g) {
    static float pred[COOP_GEAR_COUNT][3];
    static bool have_pred[COOP_GEAR_COUNT];
    static uint64_t next_ns[COOP_GEAR_COUNT];
    if (!aim_diag_on() || k < 0 || k >= COOP_GEAR_COUNT) return;
    float b[12];
    if (!(rs->aim_flags & COOP_AIM_ON) || (g->bone != COOP_BONE_WEAPON_L && g->bone != COOP_BONE_WEAPON_R) ||
        !bone_matrix(c, avatar.actor, coop_bone_name[g->bone], b)) {
        have_pred[k] = false;
        return;
    }
    float err = -1;  // (last frame's hand as we predicted it vs as it was drawn = the bone read now)
    if (have_pred[k]) {
        float dx = b[3] - pred[k][0], dy = b[7] - pred[k][1], dz = b[11] - pred[k][2];
        err = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    const float *am = (const float *)(uintptr_t)(avatar.actor + ACTOR_MTX);
    float p[3] = {b[3], b[7], b[11]}, inv[12], delta[12], mv = 0;
    if (avatar.have_prev && mtx_inv(avatar.mtx_prev, inv)) {  // (this frame's prediction, as gear_world)
        mtx_mul(am, inv, delta);
        for (int r = 0; r < 3; r++) p[r] = delta[r * 4] * b[3] + delta[r * 4 + 1] * b[7] + delta[r * 4 + 2] * b[11] + delta[r * 4 + 3];
        float dx = am[3] - avatar.mtx_prev[3], dy = am[7] - avatar.mtx_prev[7], dz = am[11] - avatar.mtx_prev[11];
        mv = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    memcpy(pred[k], p, sizeof p);
    have_pred[k] = true;
    uint64_t now = plat_now_ns();
    if (now < next_ns[k] || err < 0) return;
    next_ns[k] = now + 500000000ull;
    uint64_t r10 = as_layer_rec(avatar.actor, 1, 0), r11 = as_layer_rec(avatar.actor, 1, 1);
    float w10 = -1, w11 = -1;
    if (r10) memcpy(&w10, (void *)(uintptr_t)(r10 + 0x10), 4);
    if (r11) memcpy(&w11, (void *)(uintptr_t)(r11 + 0x10), 4);
    LOG("[coop] aimdiag avatar: %s on %s: hand predicted vs drawn %.3f m, avatar moved %.3f m this frame; slot 1 weights "
        "1.0 %.2f 1.1 %.2f; AS 0.0 %s 1.0 %s 1.1 %s; aim %d %d\n",
        g->name, coop_bone_name[g->bone], err, mv, w10, w11, as_layer_name(as_layer_rec(avatar.actor, 0, 0)),
        as_layer_name(r10), as_layer_name(r11), rs->aim_pitch, rs->aim_yaw);
}

// (protocol 49) The items the other player holds in the hands (apples...): a model of each, placed in the CarryBox's
// space (the box itself has no model).
static void held_items_frame(Cpu *c, const CoopState *rs, bool show) {
    const CoopGear *box = show ? &rs->gear[COOP_GEAR_CARRIED] : NULL;
    bool ok = box && !strcmp(box->name, COOP_CARRY_BOX) && box->bone > COOP_BONE_NONE && box->bone < COOP_BONE_COUNT;
    float bm[12];
    bool have_bm = false;
    for (int i = 0; i < COOP_HELD_ITEMS; i++) {
        struct Pup *p = &held_pup[i];
        char want[48] = "";
        if (ok && rs->held[i].name[0]) {
            snprintf(want, sizeof want, "Dm_CoopW_%.31s", rs->held[i].name);
            if (!pack_exists(want)) {
                static char said[32];
                if (strcmp(said, rs->held[i].name) && coop_look_log())
                    snprintf(said, sizeof said, "%s", rs->held[i].name),
                    LOG("[coop] look: no model for the held item %s\n", rs->held[i].name);
                want[0] = 0;
            }
        }
        if (!pup_need(c, p, want)) continue;
        if (!have_bm && !(have_bm = gear_world(c, box, bm))) continue;
        float m[12];
        memcpy(m, bm, sizeof m);
        const float *q = rs->held[i].pos;
        for (int r = 0; r < 3; r++) m[r * 4 + 3] = bm[r * 4] * q[0] + bm[r * 4 + 1] * q[1] + bm[r * 4 + 2] * q[2] + bm[r * 4 + 3];
        memcpy((void *)(uintptr_t)(p->actor + ACTOR_MTX), m, sizeof m);
        memcpy((void *)(uintptr_t)(p->actor + ACTOR_MTX2), m, sizeof m);
    }
}

// (protocol 50) The avatar's red hurt flash, every frame (game thread). Fades over HURT_FLASH_NS from the rising edge
// of the partner's COOP_FLAG_HURT, as the player's own (alpha 0.5 -> 0); the colour is cleared once at the end.
#define HURT_FLASH_NS 800000000ull  // (24 frames at the game's 30 fps)
static struct {
    uint64_t start_ns;
    bool was, lit;
    unsigned shown;
    int tint, tint_frames;
    uint64_t tint_actor, tint_armors[COOP_ARMOR_SLOTS];
} hurt_flash;
static uint64_t hurt_color_mem;
static bool looks_mat_is_skin(const char *name, int len);  // (coop/looks.inc)
static int looks_stain_color(int tint);                    // (coop/looks.inc)
static uint64_t looks_mats_until;                          // (coop/looks.inc, dev: `coop looks mats` logs the materials until then)
// Native armor dye (1.9.0 FUN_7101172ec0 / FUN_7101529b60): each dyeable armor carries Head/Upper/Lower_ftp
// (tex-pattern) and optional *_fsp (shader-param) FMAA animations inside its .sbfres, switching only the dyeable cloth
// albedo textures across the 16 StainColor frames (leaving straps, leather, metal and shading untouched).
#define A_MODEL_ANIM_FIND  0x7000f7d4b8ull  // (mgr, type 0=ftp/3=fsp, &SafeString) -> u32 id
#define A_MODEL_ANIM_PLAY  0x7000f7c6e4ull  // (s0=float frame, mgr, slot, u32 id)
#define A_MODEL_ANIM_APPLY 0x7000f78244ull  // (model, model, 2) -> evaluate + push to materials
#define A_MODEL_ANIM_CLEAR 0x7000f7c4a8ull  // (mgr, slot) -> clear transient slot binding
#define A_MODEL_FSP_RES    0x7000579914ull  // (mgr, slot) -> shader parameter animation resource
#define A_DYE_TEX_LOOKUP   0x700155c59cull  // (texture cache, name CRC32) -> already loaded texture
#define A_DYE_TEX_BIND     0x7000ec3ef8ull  // (fsp instance, texture index, texture)
#define A_DYE_NAME_HASH    0x7000eb7a84ull  // (name) -> CRC32
#define A_DYE_FRAME_NAMES  0x7000f7d6bcull  // (s0=frame, mgr, model, id, names, capacity)
#define A_DYE_MODEL_FILES  0x700150237cull  // (model list, out descriptor)
#define A_DYE_ARCHIVE_CTOR 0x7001322d84ull
#define A_DYE_ARCHIVE_SET  0x7001322ed0ull
#define A_DYE_ARCHIVE_DTOR 0x7001322e10ull
#define A_DYE_HOLDER_INIT  0x70013198e8ull
#define A_DYE_HOLDER_FREE  0x70013199b4ull  // (holder + 0x10), frees bookkeeping without releasing textures
#define A_DYE_TEX_LOAD     0x7001319a30ull  // (holder, name, archive) -> retained texture
#define A_DYE_FALLBACK_TEX 0x7001e46948ull
#define A_DYE_PENDING_TEX  0x7001e46bd0ull
#define A_SAFESTRING_VT_PTR 0x7001d66260ull  // (Ghidra PTR_DAT_7101d66260: sead::SafeString vtable - 0x10)
static uint64_t dye_str_mem;  // 3 * 0x20: SafeString + "Head" / "Upper" / "Lower"
static uint64_t dye_load_mem;
static uint32_t model_anim_find(Cpu *c, uint64_t mgr, int type, uint64_t ss) {
    Cpu saved = *c;
    c->x[0] = mgr; c->x[1] = (uint64_t)(uint32_t)type; c->x[2] = ss;
    guest_call(c, A_MODEL_ANIM_FIND);
    uint32_t id = (uint32_t)c->x[0];
    *c = saved;
    return id;
}
static void model_anim_play(Cpu *c, uint64_t mgr, int slot, uint32_t id, float frame) {
    Cpu saved = *c;
    memset(&c->v[0], 0, 16);
    memcpy(&c->v[0], &frame, 4);
    c->x[0] = mgr; c->x[1] = (uint64_t)(uint32_t)slot; c->x[2] = id;
    guest_call(c, A_MODEL_ANIM_PLAY);
    *c = saved;
}
static void model_anim_apply(Cpu *c, uint64_t model, uint64_t mgr, int clear_slot0, int clear_slot1) {
    Cpu saved = *c;
    c->x[0] = model; c->x[1] = model; c->x[2] = 2;
    guest_call(c, A_MODEL_ANIM_APPLY);
    *c = saved;
    if (clear_slot0 >= 0) {
        c->x[0] = mgr; c->x[1] = (uint64_t)(uint32_t)clear_slot0;
        guest_call(c, A_MODEL_ANIM_CLEAR);
        *c = saved;
    }
    if (clear_slot1 >= 0) {
        c->x[0] = mgr; c->x[1] = (uint64_t)(uint32_t)clear_slot1;
        guest_call(c, A_MODEL_ANIM_CLEAR);
        *c = saved;
    }
}
static uint64_t dye_call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2) {
    Cpu saved = *c;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2;
    memset(c->x + 3, 0, 5 * sizeof c->x[0]);
    guest_call(c, fn);
    uint64_t result = c->x[0];
    *c = saved;
    return result;
}

static bool dye_texture_ready(uint64_t tex) {
    return in_guest(tex) && tex != LD64(A_DYE_FALLBACK_TEX) && tex != LD64(A_DYE_PENDING_TEX);
}

static bool dye_reserve_textures(Cpu *c, uint64_t holder, uint64_t heap, uint64_t scratch, unsigned more) {
    unsigned used = LD32(holder + 0x10), capacity = LD32(holder + 0x14);
    if (used > capacity || capacity > 256) return false;
    if (more <= capacity - used) return true;
    unsigned size = used + more;
    if (size > 256 || !in_guest(heap)) return false;
    uint64_t index = LD64(holder + 0x18);
    for (unsigned i = 0; i < used; i++)
        if (!in_guest(index) || !in_guest(LD64(index + i * 8))) return false;

    // Keep the original references and make room for the new ones in the game-owned
    // holder. Its destructor releases every recorded texture when the armor unloads.
    uint64_t replacement = scratch;
    uint64_t args = scratch + 0x30;
    memset((void *)(uintptr_t)replacement, 0, 0x30);
    ST64(args, size);
    ST64(args + 8, heap);
    ST64(args + 0x10, LD64(holder + 8));
    if (!dye_call(c, A_DYE_HOLDER_INIT, replacement, args, 0)) return false;
    uint64_t node = LD64(replacement + 0x20), new_index = LD64(replacement + 0x18);
    for (unsigned i = 0; i < used; i++) {
        uint64_t next = LD64(node);
        ST32(node, LD32(LD64(index + i * 8)));
        ST64(new_index + i * 8, node);
        node = next;
    }
    ST32(replacement + 0x10, used);
    ST64(replacement + 0x20, node);
    memcpy((void *)(uintptr_t)args, (void *)(uintptr_t)(holder + 0x10), 0x20);
    memcpy((void *)(uintptr_t)(holder + 0x10), (void *)(uintptr_t)(replacement + 0x10), 0x20);
    dye_call(c, A_DYE_HOLDER_FREE, args, 0, 0);
    return true;
}

static bool model_dye_prepare(Cpu *c, uint64_t actor, uint64_t model, uint64_t mgr, uint32_t fsp, float frame) {
    uint64_t holder = LD64(actor + 0x658);
    uint64_t cache = in_guest(holder) ? LD64(holder + 8) : 0;
    uint64_t svt = LD64(A_SAFESTRING_VT_PTR);
    if (!in_guest(svt)) return false;
    if (!dye_load_mem) dye_load_mem = (uint64_t)(uintptr_t)hle_alloc(0x700);
    if (!dye_load_mem) return false;

    uint64_t names = dye_load_mem, info = names + 0x400, archive = info + 0xa0;
    uint64_t options = archive + 0x140, path = options + 0x20, scratch = path + 0x80;
    Cpu saved = *c;
    memcpy(&c->v[0], &frame, sizeof frame);
    c->x[0] = mgr; c->x[1] = model; c->x[2] = fsp; c->x[3] = names; c->x[4] = 128;
    guest_call(c, A_DYE_FRAME_NAMES);
    unsigned count = (unsigned)c->x[0];
    *c = saved;
    if (count > 128) return false;
    if (!count) return true;
    if (!in_guest(cache)) return false;

    uint64_t missing[128];
    unsigned needed = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t name = LD64(names + i * 8);
        if (!in_guest(name)) return false;
        uint32_t hash = (uint32_t)dye_call(c, A_DYE_NAME_HASH, name, 0, 0);
        // A matching entry held by another actor needs an extra reference here.
        uint64_t index = LD64(holder + 0x18);
        unsigned used = LD32(holder + 0x10);
        if (used > 256 || (used && !in_guest(index))) return false;
        bool retained = false;
        for (unsigned j = 0; j < used; j++) {
            uint64_t node = LD64(index + j * 8);
            if (!in_guest(node)) return false;
            if (LD32(node) == hash) { retained = true; break; }
        }
        if (retained && !dye_texture_ready(dye_call(c, A_DYE_TEX_LOOKUP, cache, hash, 0))) return false;
        if (!retained) missing[needed++] = name;
    }
    if (!needed) return true;

    uint64_t profile = LD64(actor + 0x570);
    uint64_t model_list = in_guest(profile) ? LD64(profile + 0x90) : 0;
    if (!in_guest(model_list)) return false;
    dye_call(c, A_DYE_MODEL_FILES, model_list, info, 0);
    uint64_t folder = LD64(info + 0x88);
    if (!in_guest(folder) || !LD8(folder)) return false;
    int len = snprintf((char *)(uintptr_t)path, 0x80, "Model/%.100s.Tex.bfres", (const char *)(uintptr_t)folder);
    if (len <= 0 || len >= 0x80 ||
        !dye_reserve_textures(c, holder, LD64(actor + 0x5b8), scratch, needed)) return false;

    // The actor loader uses this short-lived archive object to open the model's
    // texture file. Each texture loaded through the holder keeps its own reference.
    dye_call(c, A_DYE_ARCHIVE_CTOR, archive, 0, 0);
    ST64(options, 1);
    ST64(options + 8, svt + 0x10);
    ST64(options + 0x10, path);
    ST64(options + 0x18, 0);
    dye_call(c, A_DYE_ARCHIVE_SET, archive, options, 0);
    bool ready = true;
    for (unsigned i = 0; i < needed; i++) {
        ST64(options, svt + 0x10);
        ST64(options + 8, missing[i]);
        if (!dye_texture_ready(dye_call(c, A_DYE_TEX_LOAD, holder, options, archive))) {
            ready = false;
            break;
        }
    }
    dye_call(c, A_DYE_ARCHIVE_DTOR, archive, 0, 0);
    if (coop_look_log())
        LOG("[coop] looks: own dye textures %s (stain %.0f, %u requested, %s)\n",
            (const char *)(uintptr_t)path, frame, needed, ready ? "ready" : "unavailable");
    return ready;
}

static void model_dye_rebind(Cpu *c, uint64_t actor, uint64_t mgr, int slot, int slots) {
    uint64_t holder = LD64(actor + 0x658);
    uint64_t cache = in_guest(holder) ? LD64(holder + 8) : 0;
    uint64_t table = LD64(mgr + 0x58);
    if (!in_guest(cache) || !in_guest(table)) return;
    uint64_t primary = table + (uint64_t)slot * 0x30;
    unsigned parts = LD32(primary + 0x20);
    uint64_t links = LD64(primary + 0x28);
    if (!in_guest(links) || !parts || parts > (unsigned)slots) return;
    Cpu saved = *c;
    c->x[0] = mgr; c->x[1] = (uint64_t)slot;
    guest_call(c, A_MODEL_FSP_RES);
    uint64_t res = c->x[0];
    *c = saved;
    if (!in_guest(res)) return;
    unsigned textures = LD16(res + 0x76);
    uint64_t names = LD64(res + 0x40);
    if (textures > 128 || !in_guest(names)) return;
    for (unsigned i = 0; i < textures; i++) {
        uint64_t name = LD64(names + i * 8);
        if (!in_guest(name)) continue;
        c->x[0] = name + 2;
        guest_call(c, A_DYE_NAME_HASH);
        uint32_t hash = (uint32_t)c->x[0];
        *c = saved;
        c->x[0] = cache; c->x[1] = hash;
        guest_call(c, A_DYE_TEX_LOOKUP);
        uint64_t tex = c->x[0];
        *c = saved;
        if (!dye_texture_ready(tex)) continue;
        for (unsigned p = 0; p < parts; p++) {
            uint64_t part = LD64(links + p * 8);
            if (part < table || part >= table + (uint64_t)slots * 0x30 || LD16(part) != 3) continue;
            uint64_t instance = LD64(part + 0x10);
            if (!in_guest(instance)) continue;
            c->x[0] = instance; c->x[1] = i; c->x[2] = tex;
            guest_call(c, A_DYE_TEX_BIND);
            *c = saved;
        }
    }
}
// SAVAGEWIND_COOP_PUPPETDYE=0: armor puppets dye the old way (no own textures; wrong colour with the same armor)
static bool puppet_dye_tex_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_PUPPETDYE");
    return on;
}
// A puppet actor (profile NPC) gets no texture holder from the loader (FUN_710152d980 makes one only for "Armor"
// profiles / tag ArmorDye). Make one the way the loader does: 0x30 bytes on the actor's own heap, ctor, then
// init with 16 entries over the texture cache of the actor's model resource (+0x678; index 1 when it has several).
#define A_DYE_HEAP_ALLOC   0x7000e8c250ull  // (size, heap, align)
#define A_DYE_HOLDER_CTOR  0x7001319828ull
#define A_DYE_RES_CACHE    0x70015584c8ull  // (resource handle) -> texture cache or 0
static uint64_t puppet_dye_holder(Cpu *c, uint64_t actor) {
    uint64_t holder = LD64(actor + 0x658);
    if (holder) return holder;
    uint64_t heap = LD64(actor + 0x5b8), res = LD64(actor + 0x678);
    if (!in_guest(heap) || !in_guest(res)) return 0;
    uint64_t cache = dye_call(c, A_DYE_RES_CACHE, res + (LD32(actor + 0x670) > 1 ? 0x50 : 0), 0, 0);
    if (!in_guest(cache)) cache = dye_call(c, A_DYE_RES_CACHE, res, 0, 0);
    if (!in_guest(cache)) return 0;
    holder = dye_call(c, A_DYE_HEAP_ALLOC, 0x30, heap, 8);
    if (!in_guest(holder)) return 0;
    dye_call(c, A_DYE_HOLDER_CTOR, holder, 0, 0);
    static uint64_t args;
    if (!args) args = (uint64_t)(uintptr_t)hle_alloc(0x20);
    if (!args) return 0;
    ST64(args, 16); ST64(args + 8, heap); ST64(args + 0x10, cache);
    bool ok = dye_call(c, A_DYE_HOLDER_INIT, holder, args, 0) != 0;
    if (coop_look_log())
        LOG("[coop] looks: puppet dye holder %llx cache %llx %s\n", (unsigned long long)holder,
            (unsigned long long)cache, ok ? "ready" : "failed");
    if (!ok) return 0;
    ST64(actor + 0x658, holder);
    return holder;
}
static bool armor_native_dye(Cpu *c, uint64_t actor, int part_hint, int stain, const char *what, int original) {
    uint64_t model = in_guest(actor) ? LD64(actor + ACTOR_MODEL) : 0;
    uint64_t mgr = in_guest(model) ? LD64(model + 0xd0) : 0;
    int slots = in_guest(mgr) ? (int)LD32(mgr + 0x50) : 0;
    if (slots <= 0 || slots > 16) return false;
    if (!dye_str_mem) {
        uint64_t svt = LD64(A_SAFESTRING_VT_PTR);
        if (!in_guest(svt)) return false;
        dye_str_mem = (uint64_t)(uintptr_t)hle_alloc(0x60);
        if (!dye_str_mem) return false;
        static const char *const parts[3] = {"Head", "Upper", "Lower"};
        for (int i = 0; i < 3; i++) {
            uint64_t base = dye_str_mem + (uint64_t)i * 0x20;
            strcpy((char *)(uintptr_t)(base + 0x10), parts[i]);
            uint64_t s[2] = {svt + 0x10, base + 0x10};
            memcpy((void *)(uintptr_t)base, s, sizeof s);
        }
    }
    uint32_t ftp = 0xffffffffu, fsp = 0xffffffffu;
    bool has_ftp = false, has_fsp = false;
    int matched = -1;
    for (int k = 0; k < 3; k++) {
        int p = (part_hint + k) % 3;
        uint64_t ss = dye_str_mem + (uint64_t)p * 0x20;
        ftp = model_anim_find(c, mgr, 0, ss);
        fsp = model_anim_find(c, mgr, 3, ss);
        has_ftp = ftp < 0xffff0000u && (ftp & 0xffffu) != 0xffffu;
        has_fsp = fsp < 0xffff0000u && (fsp & 0xffffu) != 0xffffu;
        if (has_ftp || has_fsp) { matched = p; break; }
    }
    if (!has_ftp && !has_fsp) return false;
    float frame = (float)stain;
    bool equipped = original >= 0;
    // (2026-11-18 dyefix) A puppet had no texture holder, so its frame's textures were never loaded: its dyed cloth
    // had no texture (slot view 0, descriptor 0xffffffff) and showed the local Link's armor colour instead. Now it
    // gets a holder (puppet_dye_holder) and loads/binds its colour's textures like the equipped armor. If that fails
    // it keeps its original frame and gets the cloth tint instead (return false).
    uint64_t holder = LD64(actor + 0x658);
    if (!equipped && !holder && has_fsp && puppet_dye_tex_on()) holder = puppet_dye_holder(c, actor);
    bool own_tex = equipped || (puppet_dye_tex_on() && in_guest(holder) && in_guest(LD64(holder + 8)));
    bool fallback = !own_tex && puppet_dye_tex_on() && has_fsp && stain;
    if (fallback) frame = 0;  // (back to the original frame if it was dyed before)
    if (own_tex && has_fsp) {
        // Verify that Default can restore the native frame before changing the model.
        if (equipped && original != stain && !model_dye_prepare(c, actor, model, mgr, fsp, (float)original)) return false;
        if (!model_dye_prepare(c, actor, model, mgr, fsp, frame)) {
            if (equipped) return false;
            own_tex = false, fallback = stain != 0, frame = 0;  // (puppet: its textures did not load)
        }
    }
    if (has_fsp && slots >= 2 && (has_ftp || own_tex)) {
        if (has_ftp) model_anim_play(c, mgr, slots - 1, ftp, frame);
        model_anim_play(c, mgr, slots - 2, fsp, frame);
        if (own_tex) model_dye_rebind(c, actor, mgr, slots - 2, slots);
        model_anim_apply(c, model, mgr, has_ftp ? slots - 1 : -1, slots - 2);
    } else {
        if (has_ftp) {
            model_anim_play(c, mgr, slots - 1, ftp, frame);
            model_anim_apply(c, model, mgr, slots - 1, -1);
        }
        if (has_fsp) {
            model_anim_play(c, mgr, slots - 1, fsp, frame);
            if (own_tex) model_dye_rebind(c, actor, mgr, slots - 1, slots);
            model_anim_apply(c, model, mgr, slots - 1, -1);
        }
    }
    if (plat_now_ns() < looks_mats_until) {
        static const char *const parts[3] = {"Head", "Upper", "Lower"};
        LOG("[coop] looks dye: %s native %s StainColor %d (slots %d, ftp %08x, fsp %08x)\n",
            what, parts[matched], stain, slots, has_ftp ? ftp : 0xffffffffu, has_fsp ? fsp : 0xffffffffu);
    }
    return !fallback;
}
// (coop-looks2) The same uniform per material (the loop of FUN_710124d16c, unit vtable +0xb8 material count, +0xd0
// uniform index by name, +0x158 set it): `col` on the cloth, nothing on skin/hair/face/eyes/belts/metal. A material's
// name: unit +0x128 = its material objects (0x80 bytes each), the first field = the nn::g3d ResMaterial, +0x10 =
// its name (u16 length, then the characters). Unreadable names count as skin (no colour).
static uint64_t mat_uniform_mem;  // SafeString {vtable, "uk_damage_color"} + the name
static void unit_clothes_color(Cpu *c, uint64_t unit, const float col[4], const char *what) {
    static const float none[4] = {0, 0, 0, 0};
    uint64_t vt = in_guest(unit) ? LD64(unit) : 0;
    if (!in_guest(vt) || !in_guest(LD64(vt + 0xb8)) || !in_guest(LD64(vt + 0xd0)) || !in_guest(LD64(vt + 0x158))) return;
    if (!mat_uniform_mem) {
        uint64_t svt = LD64(A_SAFESTRING_VT_PTR);
        if (!in_guest(svt)) return;
        mat_uniform_mem = (uint64_t)(uintptr_t)hle_alloc(0x40);
        if (!mat_uniform_mem) return;
        strcpy((char *)(uintptr_t)(mat_uniform_mem + 0x10), "uk_damage_color");
        uint64_t s[2] = {svt + 0x10, mat_uniform_mem + 0x10};
        memcpy((void *)(uintptr_t)mat_uniform_mem, s, sizeof s);
    }
    Cpu saved = *c;
    c->x[0] = unit;
    guest_call(c, LD64(vt + 0xb8));
    int n = (int)(uint32_t)c->x[0];
    *c = saved;
    uint64_t mats = LD64(unit + 0x128);
    if (n <= 0 || n > 64 || !in_guest(mats)) return;
    for (int i = 0; i < n; i++) {
        uint64_t res = LD64(mats + (uint64_t)i * 0x80), nm = in_guest(res) ? LD64(res + 0x10) : 0;
        int len = in_guest(nm) ? LD16(nm) : 0;
        const char *name = len > 0 && len < 64 ? (const char *)(uintptr_t)(nm + 2) : "";
        bool skin = !*name || looks_mat_is_skin(name, len);
        c->x[0] = unit, c->x[1] = (uint64_t)i, c->x[2] = mat_uniform_mem;
        guest_call(c, LD64(vt + 0xd0));
        int u = (int)(uint32_t)c->x[0];
        *c = saved;
        if (plat_now_ns() < looks_mats_until) LOG("[coop] looks mats: %s material %d %.*s: %s (uniform %d)\n", what, i, *name ? len : 1, *name ? name : "?",
                                skin ? "skin, no colour" : "clothes, coloured", u);
        if (u == -1) continue;
        memcpy((void *)(uintptr_t)hurt_color_mem, skin ? none : col, 16);
        c->x[0] = unit, c->x[1] = (uint64_t)i, c->x[2] = (uint64_t)(uint32_t)u, c->x[3] = hurt_color_mem;
        guest_call(c, LD64(vt + 0x158));
        *c = saved;
    }
}
// `clothes`: per material, skin left alone (the tunic colour); otherwise the whole model (the red hurt flash).
static void model_damage_color(Cpu *c, uint64_t actor, const float col[4], bool clothes, const char *what) {
    uint64_t model = in_guest(actor) ? LD64(actor + 0x4e0) : 0;
    if (!in_guest(model) || !in_guest(LD64(model + 0x40)) || (int)LD32(model + 0x38) <= 0) return;
    if (!hurt_color_mem) hurt_color_mem = (uint64_t)(uintptr_t)hle_alloc(0x10);
    if (!hurt_color_mem) return;
    if (clothes) {
        int nu = (int)LD32(model + 0x38);
        uint64_t arr = LD64(model + 0x40);
        for (int u = 0; u < nu && u < 8; u++) {
            uint64_t p = LD64(arr + (uint64_t)u * 8);
            if (in_guest(p)) unit_clothes_color(c, LD64(p), col, what);
        }
        return;
    }
    memcpy((void *)(uintptr_t)hurt_color_mem, col, 16);
    Cpu saved = *c;
    c->x[0] = 0, c->x[1] = model, c->x[2] = hurt_color_mem;
    guest_call(c, A_MODEL_DAMAGE_COLOR);
    *c = saved;
}
// (coop-looks, coop/looks.inc) The player's tunic colour rides the same uniform: set between flashes, on the armor
// puppets (the clothes) and on the body when no upper-body armor is worn; set again every TINT_REFRESH frames (new
// puppets after an outfit change, a respawned avatar).
static int looks_cur_tint(void);
static int multi_in_pid(void);  // (coop/looks.inc: the player whose avatar is being updated)
static bool looks_tint_color(int tint, float col[4]);
#define TINT_REFRESH 15
static void hurt_flash_frame(Cpu *c, const CoopState *rs, bool have) {
    uint64_t now = plat_now_ns();
    bool hurt = hurt_flash_on() && have && (rs->flags & COOP_FLAG_HURT);
    if (hurt && !hurt_flash.was) {
        hurt_flash.start_ns = now;
        if (hurt_flash.shown++ < 40) LOG("[coop] avatar: the other player was hurt: red flash\n");
    }
    hurt_flash.was = hurt;
    bool av = avatar.actor && avatar_alive() && avatar.frames >= 6;
    float t = hurt_flash.start_ns ? (float)((double)(now - hurt_flash.start_ns) / HURT_FLASH_NS) : 1.0f;
    bool on = hurt_flash_on() && av && t < 1.0f;
    uint64_t ready_armors[COOP_ARMOR_SLOTS] = {0};
    for (int i = 0; av && i < COOP_ARMOR_SLOTS; i++)
        if (armor_pup[i].actor && armor_pup[i].frames >= 6 && pup_alive(&armor_pup[i])) ready_armors[i] = armor_pup[i].actor;
    bool armor_changed = av && memcmp(ready_armors, hurt_flash.tint_armors, sizeof ready_armors) != 0;
    float tcol[4] = {0, 0, 0, 0};
    int tint = av && looks_tint_color(looks_cur_tint(), tcol) ? looks_cur_tint() : 0;
    bool retint = !on && av && (tint != hurt_flash.tint || armor_changed ||
                                (tint && (avatar.actor != hurt_flash.tint_actor || ++hurt_flash.tint_frames >= TINT_REFRESH)));
    if (!on && !hurt_flash.lit && !retint) return;
    if (!hurt_color_mem) hurt_color_mem = (uint64_t)(uintptr_t)hle_alloc(0x10);
    if (!hurt_color_mem) return;
    if (!av) {
        hurt_flash.lit = false, hurt_flash.tint = 0;
        memset(hurt_flash.tint_armors, 0, sizeof hurt_flash.tint_armors);
        return;
    }
    float col[4] = {0, 0, 0, 0};
    if (on) col[0] = 1.0f, col[1] = 0.15f, col[2] = 0.1f, col[3] = (1.0f - t) * 0.5f;
    else memcpy(col, tcol, sizeof col);
    if (on) {
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++)
            if (ready_armors[i]) model_damage_color(c, ready_armors[i], col, false, armor_pup[i].name);
        model_damage_color(c, avatar.actor, col, false, "body");
    } else {
        static const float zero[4] = {0, 0, 0, 0};
        int stain = looks_stain_color(tint), native_n = 0, worn = 0;
        for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
            if (!ready_armors[i]) continue;
            worn++;
            if (armor_native_dye(c, ready_armors[i], i, stain, armor_pup[i].name, -1)) {
                native_n++;
                model_damage_color(c, ready_armors[i], zero, false, armor_pup[i].name);
            } else {
                model_damage_color(c, ready_armors[i], col, tint != 0, armor_pup[i].name);
            }
        }
        model_damage_color(c, avatar.actor, col, tint != 0, "body");
        if (tint != hurt_flash.tint || avatar.actor != hurt_flash.tint_actor || armor_changed)
            LOG("[coop] looks: avatar of player %d in colour %d (stain %d: %d native-dyed + %d cloth-tinted, body)\n",
                multi_in_pid(), tint, stain, native_n, worn - native_n);
    }
    hurt_flash.lit = on;
    hurt_flash.tint = on ? -1 : tint;  // (after a flash: the colour again at once)
    hurt_flash.tint_actor = avatar.actor, hurt_flash.tint_frames = 0;
    if (!on) memcpy(hurt_flash.tint_armors, ready_armors, sizeof ready_armors);
}
// The bow's grip follows Weapon_L, but the limbs and string have their own animation. Replay that animation on
// the bow puppet from the same action that poses Link. Older overlays only have the static model.
static bool bow_pose_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *dir = getenv("SAVAGEWIND_ROMFS_OVERLAY");
        on = 0;
        if (dir && *dir && look_on("SAVAGEWIND_COOP_BOWPOSE")) {
            char path[600];
            snprintf(path, sizeof path, "%s/coop_bow_anim", dir);
            FILE *f = fopen(path, "rb");
            if (f) fclose(f), on = 1;
        }
    }
    return on;
}

static void gear_bow_pose(Cpu *c, const CoopState *rs, const CoopGear *g, struct Pup *p) {
    enum { BOW_STOWED = 1, BOW_HOLD, BOW_DRAW, BOW_SHOOT };
    static const char *const names[] = {
        [BOW_STOWED] = "Equiped", [BOW_HOLD] = "Hold", [BOW_DRAW] = "Draw", [BOW_SHOOT] = "Shoot",
    };
    if (!bow_pose_on()) return;
    int pose = BOW_STOWED;
    if (g->bone == COOP_BONE_WEAPON_L) {
        if (rs->anim_upper == COOP_ANIM_UPPER_BOW_SHOOT || rs->anim == COOP_ANIM_BOW_SHOOT) pose = BOW_SHOOT;
        else if (rs->anim_upper == COOP_ANIM_UPPER_BOW || rs->anim == COOP_ANIM_BOW) pose = BOW_DRAW;
        else pose = BOW_HOLD;
    }
    if (p->bow_pose == pose) return;
    uint64_t rec = as_layer_rec(p->actor, 0, 0);
    if (!rec) return;
    play_anim(c, p->actor, names[pose], 0);
    p->bow_pose = (uint8_t)pose;
    if (aim_diag_on()) LOG("[coop] aimdiag bow pose: %s -> %s (playing %s)\n", g->name, names[pose], as_layer_name(rec));
}

static bool gear_frame(Cpu *c, const CoopState *rs, bool have) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_SWORD");
    bool av = avatar.actor && avatar_alive();
    bool weapon_placed = false;
    held_items_frame(c, rs, on && av && have);
    if (av) pod_diag(c, "avatar", avatar.actor);
    for (int k = 0; k < COOP_GEAR_COUNT; k++) {
        struct Pup *p = &gear_pup[k];
        char want[48] = "";
        const CoopGear *g = have ? &rs->gear[k] : NULL;
        if (on && av && g && g->name[0] && g->bone > COOP_BONE_NONE && g->bone < COOP_BONE_COUNT)
            gear_puppet(k, g->name, want);
        if (k == COOP_GEAR_GLIDER && glider_wait_frame(c, p, want)) continue;
        if (!pup_need(c, p, want)) continue;
        float m[12];
        if (!gear_world(c, g, m)) continue;
        memcpy((void *)(uintptr_t)(p->actor + ACTOR_MTX), m, sizeof m);
        memcpy((void *)(uintptr_t)(p->actor + ACTOR_MTX2), m, sizeof m);
        if (k == COOP_GEAR_WEAPON) weapon_placed = true;
        gear_diag_recv(c, rs, k, g);  // (dev, SAVAGEWIND_COOP_AIMDIAG=1)
        if (k == COOP_GEAR_BOW) gear_bow_pose(c, rs, g, p);
        if (k == COOP_GEAR_GLIDER) glider_trail(p->actor, true), glider_tail_key(c, p, true), xprobe_frame(p->actor);
    }
    return weapon_placed;
}

// The armor puppet for an armor actor name (Dm_CoopA_Armor_001_Upper), "" without one in the overlay.
static void armor_puppet(const char *armor, char out[48]) {
    out[0] = 0;
    if (strncmp(armor, "Armor_", 6)) return;
    char p[48];
    snprintf(p, sizeof p, "Dm_CoopA_%.31s", armor);
    if (pack_exists(p)) memcpy(out, p, 48);
}

// Body parts under the clothes hidden (every group shown first, then each piece hides what it covers).
static char outfit_done[COOP_ARMOR_SLOTS * 48];
static void outfit_apply(Cpu *c) {
    uint64_t mc = LD64(avatar.actor + ACTOR_MODEL);
    if (!in_guest(mc)) return;
    show_body(c, avatar.actor);
    char key[sizeof outfit_done] = "";
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
        struct Pup *p = &armor_pup[i];
        if (!p->actor || !pup_alive(p)) continue;
        uint64_t am = LD64(p->actor + ACTOR_MODEL);
        if (!in_guest(am)) continue;
        Cpu saved = *c;
        c->x[0] = LD64(A_MATERIAL_GROUPS_ARG); c->x[1] = mc; c->x[2] = am; c->x[3] = 0;
        guest_call(c, A_MATERIAL_GROUPS);
        *c = saved;
        strncat(key, p->name, sizeof key - strlen(key) - 1);
    }
    if (strcmp(key, outfit_done)) LOG("[coop] look: avatar body hidden under \"%s\"\n", key);
    memcpy(outfit_done, key, sizeof key);
}

// No armor puppets during a Divine Beast boss fight (2026-11-13). Seen 4 times in the Rudania fight (both players in
// the Flamebreaker set): after the boss intro reloads the area, the avatar comes back, its first armor puppet is
// created, the next one fails ("could not create": the game is out of actor memory with the boss loaded), and the
// game then crashes in its own model setup (guest FUN_7100f7913c / FUN_7100f77144, a failed allocation falls back to
// a null buffer). Waiting 1 s or 10 s, or a bigger instSize, did not help; without armor puppets the whole fight
// passed. So from Appear_PGanon<X> until Clear_Remains<X> the avatar shows Link's plain body (puppets deleted). (Until
// Die_PGanon<X> was not enough: the guest crashed the same way at the next reload after the boss died.)
// SAVAGEWIND_COOP_ARMORBOSS=0: armor kept anyway. Other memory-heavy places may need the same: not known.
static int beast_here(void);
static int gdt_get_bool(const char *name);
static bool armor_boss_fight(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_ARMORBOSS");
    static const char *const nm[4] = {"Water", "Fire", "Wind", "Electric"};
    int b = on ? beast_here() : -1;
    if (b < 0) return false;
    char f[40];
    snprintf(f, sizeof f, "Appear_PGanon%s", nm[b]);
    bool fight = gdt_get_bool(f) == 1;
    snprintf(f, sizeof f, "Clear_Remains%s", nm[b]);
    if (fight && gdt_get_bool(f) == 1) fight = false;
    static int said = -1;
    if (said != (int)fight) said = fight, LOG("[coop] look: %s: armor puppets %s\n", fight ? "boss fight in this Divine Beast" : "no boss fight", fight ? "off" : "on");
    return fight;
}

// (2026-11-18: the other Links were always bald) Link's hair is not part of his body model (Link.sbfres
// has only the scalp and a Hair_Root bone): with no head armor the real player wears the actor Armor_Default_Head
// (model Armor_Default/Armor_Default_Head: fringe, sides, pony tail, hair band; its 11 hair strands are Havok cloth,
// Armor_Default_Head.bphysics). The sender sends "" for "no head armor" (Default is bare Link), so the avatar showed
// the scalp. Now an empty head slot gets the hair puppet Dm_CoopA_Armor_Default_Head (build_overlay.py, with a
// cloth-only copy of the real hair physics) like any head armor. SAVAGEWIND_COOP_HAIR=0: no hair puppet.
#define HAIR_ARMOR "Armor_Default_Head"
static bool hair_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_HAIR");
    return on;
}

static bool armor_resync;  // a new armor puppet is ready: replay the avatar's AS on everyone (same phase)
static void armor_frame(Cpu *c, const CoopState *rs, bool have) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_ARMOR");
    bool av = avatar.actor && avatar_alive() && !armor_boss_fight(), changed = false;
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
        struct Pup *p = &armor_pup[i];
        char want[48] = "";
        if (on && av && have) armor_puppet(rs->armor[i], want);
        if (i == 0 && on && av && have && !rs->armor[0][0] && hair_on()) armor_puppet(HAIR_ARMOR, want);
        bool had = p->actor != 0;
        if (!pup_need(c, p, want)) {
            if (had) changed = true;
            continue;
        }
        p->frames++;
        if (p->frames == 6) armor_resync = true, changed = true;  // (its AS and model are ready by then)
        memcpy((void *)(uintptr_t)(p->actor + ACTOR_MTX), (void *)(uintptr_t)(avatar.actor + ACTOR_MTX), 48);
        memcpy((void *)(uintptr_t)(p->actor + ACTOR_MTX2), (void *)(uintptr_t)(avatar.actor + ACTOR_MTX2), 48);
    }
    if (av && changed) outfit_apply(c);
}

static void look_log(const CoopState *rs, bool have) {
    const CoopState *st[2] = {&look_sent, have ? rs : NULL};
    for (int w = 0; w < 2; w++) {
        const CoopState *s = st[w];
        if (!s) { LOG("[coop] look: nothing received\n"); continue; }
        LOG("[coop] look %s: anim %s armor %s|%s|%s\n", w ? "received" : "sent", coop_anim_as[s->anim < COOP_ANIM_COUNT ? s->anim : 0],
            s->armor[0], s->armor[1], s->armor[2]);
        for (int k = 0; k < COOP_GEAR_COUNT; k++)
            if (s->gear[k].name[0])
                LOG("[coop] look %s: gear %d %s on %s, offset %.2f %.2f %.2f\n", w ? "received" : "sent", k,
                    s->gear[k].name, coop_bone_name[s->gear[k].bone < COOP_BONE_COUNT ? s->gear[k].bone : 0],
                    s->gear[k].rel[3], s->gear[k].rel[7], s->gear[k].rel[11]);
        for (int i = 0; i < COOP_HELD_ITEMS; i++)
            if (s->held[i].name[0])
                LOG("[coop] look %s: held item %d %s at %.2f %.2f %.2f in the box\n", w ? "received" : "sent", i,
                    s->held[i].name, s->held[i].pos[0], s->held[i].pos[1], s->held[i].pos[2]);
    }
    for (int i = 0; i < COOP_HELD_ITEMS; i++)
        if (held_pup[i].actor) LOG("[coop] look: held item puppet %d %s\n", i, held_pup[i].name);
    char pups[400] = "";
    for (int k = 0; k < COOP_GEAR_COUNT; k++)
        if (gear_pup[k].actor) strncat(pups, " ", sizeof pups - strlen(pups) - 1), strncat(pups, gear_pup[k].name, sizeof pups - strlen(pups) - 1);
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++)
        if (armor_pup[i].actor) strncat(pups, " ", sizeof pups - strlen(pups) - 1), strncat(pups, armor_pup[i].name, sizeof pups - strlen(pups) - 1);
    LOG("[coop] look: avatar %llx plays %s; puppets%s\n", (unsigned long long)avatar.actor, avatar.as, pups);
}

// Play an AS on the avatar and on its armor puppets in a specific slot (0 = lower/base, 1 = upper body).
static void avatar_play_slot(Cpu *c, const char *name, int slot) {
    if (!name || !*name) return;
    play_anim(c, avatar.actor, name, slot);
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++)
        if (armor_pup[i].actor && armor_pup[i].frames >= 6 && pup_alive(&armor_pup[i]))
            play_anim(c, armor_pup[i].actor, name, slot);
}

static void avatar_stop_slot(Cpu *c, int slot) {
    stop_anim(c, avatar.actor, slot);
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++)
        if (armor_pup[i].actor && armor_pup[i].frames >= 6 && pup_alive(&armor_pup[i]))
            stop_anim(c, armor_pup[i].actor, slot);
}

static void avatar_play(Cpu *c, const char *name) {
    avatar_play_slot(c, name, 0);
}

// ---- ears under a hood (2026-11-11, user: the avatar's ears go through the Hylian hood). A head armor's
// GeneralParamList has ArmorHead.EarRotate (Armor_001_Head: 0 50 0 degrees). The armor actor's model component
// (FUN_710115f064, Ghidra 1.9.0) makes two bone accessors (FUN_71010966cc, 0xa8 bytes) for "Ear_L" / "Ear_R"
// (FUN_7101096be4) and writes the rotation, R = Rz * Ry * Rx, into each accessor's Matrix34f at +0x68 (FUN_710115f208).
// An accessor registered on an actor (FUN_7101533770(actor, acc, 0): linked into actor+0x4d8, bone found on the
// actor's model) multiplies that bone's local transform by the matrix every model update (FUN_7101096748, mode 0); an
// identity matrix with scale 1 is skipped. Our puppets are NPCs without that component, so the runtime registers two
// accessors on the avatar and two on its head-armor puppet (a model without those bones simply does not bind) and sets
// the rotation of the head armor the other player wears (Co-op/build_overlay.py writes <overlay>/coop_ear_rotate.txt
// from the player's own files), identity otherwise. The accessors live in our own guest memory for good (never freed;
// rebuilt only for a new actor). UNTESTED in game. SAVAGEWIND_COOP_EARS=0: off.
#define A_BONEACC_CTOR 0x70010966ccull
#define A_BONEACC_NAME 0x7001096be4ull
#define A_BONEACC_ADD 0x7001533770ull
#define BONEACC_STRIDE 0xb0  // (0xa8 bytes each)
#define BONEACC_MTX 0x68
#define EAR_TARGETS 2        // the avatar, its head armor puppet
#define EAR_ROWS 160
static struct {
    uint64_t mem;                 // 2 targets x 2 accessors, then the bone names (SafeStrings + text)
    uint64_t acc[EAR_TARGETS];    // each target's accessor pair now (a fresh block per actor, see ears_attach)
    uint64_t actor[EAR_TARGETS];  // what each pair is registered on
    unsigned id[EAR_TARGETS];     // (its actor id: a new actor at the same address gets new accessors)
    int rows;                     // coop_ear_rotate.txt: armor name -> degrees
    struct { char name[40]; float deg[3]; } row[EAR_ROWS];
    char shown[40];
} ears = {.rows = -1};

static bool ears_on(void) {
    static int on = -1;
    if (on < 0) {
        on = look_on("SAVAGEWIND_COOP_EARS");
        if (LD32(A_BONEACC_CTOR) != 0xa9be7bfdu || LD32(A_BONEACC_NAME) != 0x39402008u || LD32(A_BONEACC_ADD) != 0x2a0203e8u) {
            LOG("[coop] ears: game code differs, off\n");
            on = 0;
        }
    }
    return on;
}

static void ears_load(void) {
    ears.rows = 0;
    const char *dir = getenv("SAVAGEWIND_ROMFS_OVERLAY");
    char path[600], line[160];
    snprintf(path, sizeof path, "%s/coop_ear_rotate.txt", dir ? dir : "");
    FILE *f = dir && *dir ? fopen(path, "r") : NULL;
    while (f && ears.rows < EAR_ROWS && fgets(line, sizeof line, f)) {
        char n[64];
        float x, y, z;
        if (sscanf(line, "%63s %f %f %f", n, &x, &y, &z) == 4 && strlen(n) < sizeof ears.row[0].name &&
            isfinite(x) && isfinite(y) && isfinite(z) && fabsf(x) <= 180 && fabsf(y) <= 180 && fabsf(z) <= 180) {
            snprintf(ears.row[ears.rows].name, sizeof ears.row[0].name, "%s", n);
            ears.row[ears.rows].deg[0] = x, ears.row[ears.rows].deg[1] = y, ears.row[ears.rows].deg[2] = z;
            ears.rows++;
        }
    }
    if (f) fclose(f);
    LOG("[coop] ears: %d head armors fold the ears%s\n", ears.rows, f ? "" : " (no coop_ear_rotate.txt: re-run the overlay setup)");
}

// Register the pair of target t on actor a (a fresh actor: the accessors are built anew).
static void ears_attach(Cpu *c, int t, uint64_t a) {
    if (!ears.mem) {
        ears.mem = (uint64_t)(uintptr_t)hle_alloc(0x400);
        if (!ears.mem) return;
        memset((void *)(uintptr_t)ears.mem, 0, 0x400);
        uint64_t s = ears.mem + EAR_TARGETS * 2 * BONEACC_STRIDE;  // two SafeStrings, then their text
        memcpy((void *)(uintptr_t)(s + 0x20), "Ear_L", 6);
        memcpy((void *)(uintptr_t)(s + 0x30), "Ear_R", 6);
        for (int k = 0; k < 2; k++) {
            ST64(s + (uint64_t)k * 0x10, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
            ST64(s + (uint64_t)k * 0x10 + 8, s + 0x20 + (uint64_t)k * 0x10);
        }
    }
    uint64_t names = ears.mem + EAR_TARGETS * 2 * BONEACC_STRIDE;
    // (2026-11-20, a tester's crashes 1 and 3 at shrine entry: the old actor is finalized later than we see it go, and it
    // still had our accessors in its list; zeroing that same memory for the new actor broke the old one's list ->
    // read at 0x8 in actor finalize, x26 = old head armor + 0x524. Every attach gets fresh memory now (never freed).)
    uint64_t pair = (uint64_t)(uintptr_t)hle_alloc(2 * BONEACC_STRIDE);
    if (!pair) return;
    memset((void *)(uintptr_t)pair, 0, 2 * BONEACC_STRIDE);
    ears.acc[t] = pair;
    for (int k = 0; k < 2; k++) {
        uint64_t acc = pair + (uint64_t)k * BONEACC_STRIDE;
        Cpu saved = *c;
        c->x[0] = acc;
        guest_call(c, A_BONEACC_CTOR);
        c->x[0] = acc; c->x[1] = names + (uint64_t)k * 0x10;
        guest_call(c, A_BONEACC_NAME);
        c->x[0] = a; c->x[1] = acc; c->x[2] = 0;
        guest_call(c, A_BONEACC_ADD);
        *c = saved;
    }
    ears.actor[t] = a, ears.id[t] = LD32(a + PROC_ID);
    LOG("[coop] ears: accessors on %s %llx (bound: %d %d)\n", t ? "head armor" : "avatar", (unsigned long long)a,
        (int)LD8(pair + 8), (int)LD8(pair + BONEACC_STRIDE + 8));
}

static void ears_frame(Cpu *c, const CoopState *rs, bool have) {
    if (!ears_on()) return;
    if (ears.rows < 0) ears_load();
    bool av = avatar.actor && avatar_alive() && avatar.frames >= 6;
    uint64_t tgt[EAR_TARGETS] = {av ? avatar.actor : 0,
                                 0};  // (2026-11-20: the head armor puppet never bound them (bound: 0 0) and
                                       // it is the one that crashed at shrine entry: not used any more)
    // the rotation of the head armor worn now (identity: none, or one that leaves the ears alone)
    const float *deg = NULL;
    const char *head = have ? rs->armor[0] : "";
    for (int i = 0; i < ears.rows && head[0]; i++)
        if (!strcmp(ears.row[i].name, head)) deg = ears.row[i].deg;
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (deg) {
        const float r = 3.14159265f / 180;
        float sx = sinf(deg[0] * r), sy = sinf(deg[1] * r), sz = sinf(deg[2] * r);
        float cx = cosf(deg[0] * r), cy = cosf(deg[1] * r), cz = cosf(deg[2] * r);
        float rot[12] = {cy * cz, sx * sy * cz - sz * cx, sx * sz + sy * cx * cz, 0,
                         sz * cy, cx * cz + sz * sx * sy, -(sx * cz) + sy * sz * cx, 0,
                         -sy, sx * cy, cx * cy, 0};
        memcpy(m, rot, sizeof m);
    }
    if (strcmp(ears.shown, deg ? head : "")) {
        snprintf(ears.shown, sizeof ears.shown, "%s", deg ? head : "");
        LOG("[coop] ears: %s\n", deg ? head : "not folded");
    }
    for (int t = 0; t < EAR_TARGETS; t++) {
        uint64_t a = tgt[t];
        if (!a) continue;
        if (ears.actor[t] != a || ears.id[t] != LD32(a + PROC_ID)) ears_attach(c, t, a);
        if (!ears.mem || !ears.acc[t]) return;
        for (int k = 0; k < 2; k++)
            memcpy((void *)(uintptr_t)(ears.acc[t] + (uint64_t)k * BONEACC_STRIDE + BONEACC_MTX), m, sizeof m);
    }
}

// ---- the paraglider's wind (2026-11-11, user: the little wind effect is missing while the avatar glides). The
// streaks came from the effect set "PlayerStole2" (ActorInfo elink), which every puppet once had (copied from the
// paraglider puppet): on the avatar's Link model they also trailed a walking avatar, so it was removed from all but the
// glider puppet, which (paraglider model) shows nothing. Now a model-less effect puppet Dm_CoopGlideFx (overlay,
// elink PlayerStole2, like the game's own model-less effect locators) exists only while the other player glides and
// rides on the avatar's chest. UNTESTED in game (whether its emitters fire without Link's bones).
// 2026-11-12: OFF by default (SAVAGEWIND_COOP_GLIDEFX=1: on): an effect set bound to the paraglider's bones cannot
// show on a model-less actor; the paraglider puppet itself now gets the speed its trails need (glider_trail).
#define GLIDEFX_ACTOR "Dm_CoopGlideFx"
static struct Pup glidefx_pup;
static void glidefx_frame(Cpu *c, const CoopState *rs, bool have) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_COOP_GLIDEFX") && !strcmp(getenv("SAVAGEWIND_COOP_GLIDEFX"), "1") && pack_exists(GLIDEFX_ACTOR);
    bool av = avatar.actor && avatar_alive() && avatar.frames >= 6;
    bool gliding = on && av && have && rs->anim == COOP_ANIM_GLIDE && rs->gear[COOP_GEAR_GLIDER].name[0];
    if (!pup_need(c, &glidefx_pup, gliding ? GLIDEFX_ACTOR : "")) return;
    float m[12];
    memcpy(m, (void *)(uintptr_t)(avatar.actor + ACTOR_MTX), sizeof m);
    m[7] += 1.0f;  // (the chest, not the feet)
    memcpy((void *)(uintptr_t)(glidefx_pup.actor + ACTOR_MTX), m, sizeof m);
    memcpy((void *)(uintptr_t)(glidefx_pup.actor + ACTOR_MTX2), m, sizeof m);
}

#include "astree.inc"  // (protocol 29) the player's own AS trees on the avatar (tree mode)
#include "avatarfx.inc"  // (2026-11-14, protocol 39) the player's effects (splashes, ripples, dust) on the avatar
#include "avatarsnd.inc"  // (2026-11-14) the player's sounds (voice, footsteps, swings) and status effects on the avatar

static void avatar_inputs_reset(void) {
    astree.have = 0;
    memset(&astree, 0, sizeof astree);
    memset(&astree_end, 0, sizeof astree_end);
    astree_charge_lv = 0;
    nodepos_avatar = nodepos_avatar_initial;
    uint64_t charge_handle = avl.cf_h, ice_handle = avl.ice_h, fire_handle = avl.fire_h;
    memset(&avl, 0, sizeof avl);
    avl.cf_h = charge_handle;
    avl.ice_h = ice_handle;
    avl.fire_h = fire_handle;
    if (ice_handle) memset((void *)(uintptr_t)ice_handle, 0, 0x20);
    if (fire_handle) memset((void *)(uintptr_t)fire_handle, 0, 0x20);
    avl.sw_was_anim = -1;
    outfit_done[0] = 0;
    armor_resync = false;
    rip.actor = 0, rip.on = false, rip_water_set = 0;
    wfx.actor = 0, wfx.active = 0;
    if (!local_in_world()) {
        if (charge_handle) memset((void *)(uintptr_t)charge_handle, 0, 0x20);
        if (rip.h) memset((void *)(uintptr_t)rip.h, 0, 0x20);
        for (int i = 0; i < 2; i++)
            if (wfx.h[i]) memset((void *)(uintptr_t)wfx.h[i], 0, 0x20);
    }
    memset(&fx_avatar_map, 0, sizeof fx_avatar_map);
    memset(&snd_avatar_map, 0, sizeof snd_avatar_map);
    LOG("[coop] avatar: cleared animation targets and look state for %llx\n", (unsigned long long)avatar.actor);
}

static void avatar_frame(Cpu *c) {
    uint64_t now = plat_now_ns();
    char area[96];
    uint32_t here = local_area(area, sizeof area);
    // A stage load deletes every actor, so the avatar pointer is stale afterwards. Drop it without calling delete
    // (the game already did). Same while the player is loading, dead or back at the title screen.
    if (avatar.actor && avatar.area != here) {
        LOG("[coop] avatar dropped on area change (%08x -> %08x)\n", avatar.area, here);
        forget_actor(avatar.actor);
    }
    if (avatar.actor && !local_in_world()) {
        // Loading, dead or at the title screen. Delete it if the game has not (the player can lie dead in a
        // normal world); otherwise only drop the pointer.
        uint64_t a = avatar.actor;
        char area[96];
        bool loading = avatar.area != local_area(area, sizeof area);  // (a stage load deletes actors itself)
        bool alive = avatar_alive();
        if (alive && !loading) weapon_fx_kill(c);
        forget_actor(a);
        bool ok = alive && !loading && despawn_now(c, a, ACTOR_DELETE_REASON);
        LOG("[coop] avatar dropped while loading/dead (%s)\n",
            loading ? "area changed: the load deletes it" : alive ? (ok ? "deleted" : "delete refused") : "already gone");
    }
    if (avatar.actor && !avatar_alive()) {
        LOG("[coop] avatar gone (the game deleted it)\n");
        forget_actor(avatar.actor);
        // The game deletes actors in waves during a reload: count the world as settled again only 3 s from now.
        if (last_local.world_since_ns) last_local.world_since_ns = now;
    }
    CoopState rs;
    double age = 0;
    bool show = local_in_world() && coop_net_get_remote(&rs, &age) && age < 3 && (rs.flags & COOP_FLAG_IN_WORLD) &&
                fabsf(rs.pos[0]) + fabsf(rs.pos[1]) + fabsf(rs.pos[2]) > 1 && rs.area == here;
    if (!show) {
        if (avatar.actor) {  // peer left, or is in another area: delete our copy of their avatar
            uint64_t a = avatar.actor;
            weapon_fx_kill(c);
            forget_actor(a);
            bool ok = despawn_now(c, a, ACTOR_DELETE_REASON);
            LOG("[coop] avatar despawn %llx -> %d\n", (unsigned long long)a, (int)ok);
            avatar.retry_ns = now + 1000000000ull;  // don't recreate in the same frame
        }
        return;
    }
    if (!avatar.actor) {
        if (now < avatar.retry_ns || spawn_hold()) return;  // (spawn hold: crash report 21)
        avatar.retry_ns = now + 2000000000ull;
        avatar.actor = spawn_now(c, AVATAR_ACTOR, rs.pos);
        avatar_take_handle();
        avatar.frames = 0, avatar.anim = -1, avatar.anim_seq = 0;
        avatar.anim_upper = 0, avatar.anim_upper_seq = 0, avatar.as_face[0] = 0;
        avatar.as[0] = 0, avatar.as_upper[0] = 0;
        avatar.area = here;
        memcpy(avatar.pos, rs.pos, 12);
        avatar.yaw = rs.yaw;
        LOG("[coop] avatar %s at %.1f %.1f %.1f -> %llx\n", AVATAR_ACTOR, rs.pos[0], rs.pos[1], rs.pos[2],
            (unsigned long long)avatar.actor);
        if (!avatar.actor) return;
    }
    avatar.frames++;
    // The armor models are not attached to the avatar's skeleton yet, so hiding the body would leave it invisible.
    // Keep the whole body visible until `coop outfit` can also render the clothes.
    if (avatar.frames == 5 || avatar.frames == 120) outfit_apply(c);  // (again once its model settled)
    const char *as = avatar_as(rs.anim, rs.gear[COOP_GEAR_WEAPON].name);
    bool restart = coop_anim_is_action(rs.anim) && rs.anim_seq != avatar.anim_seq;
    // Two body layers, like the player (seen live 2026-11-10: the player always plays an AS in BOTH: "0.0=Wait
    // 1.0=Wait"). Slot 0 only moves the legs and root, slot 1 the body from Spine_1 up: a slot 1 left empty leaves
    // the upper body in its bind pose. So without an upper-body action slot 1 plays the same AS as slot 0, started in
    // the same frame (same phase).
    const char *as_u = avatar_as_upper(rs.anim_upper, rs.gear[COOP_GEAR_WEAPON].name);
    bool upper_on = rs.anim_upper != COOP_ANIM_UPPER_NONE && as_u[0];
    // (protocol 27) standing with a weapon or bow in the hand: the upper body holds it ready
    const char *armed = !upper_on && rs.anim == COOP_ANIM_WAIT ? armed_idle_as(&rs) : "";
    const char *want_u = upper_on ? as_u : armed[0] ? armed : as;
    bool u_action = upper_on && coop_anim_upper_is_action(rs.anim_upper);
    bool restart_u = u_action && rs.anim_upper_seq != avatar.anim_upper_seq;
    bool play0 = rs.anim != avatar.anim || strcmp(as, avatar.as) || restart || armor_resync;
    bool play1 = strcmp(want_u, avatar.as_upper) || restart_u || armor_resync || (!upper_on && play0);
    if (!upper_on && play1) play0 = true;  // (back from an upper-body action: both layers restart together)
    // (protocol 29) tree mode: the other player's own AS with their inputs (coop/astree.inc); else the classified states
    bool tree = avatar.frames >= 6 && astree_frame(c, armor_resync);
    avatarfx_frame(c, &rs);
    if (tree) {
        play0 = play1 = false;
        avatar.anim = -1, avatar.as[0] = avatar.as_upper[0] = avatar.as_face[0] = 0;  // (all replayed when it ends)
    }
    if (avatar.frames >= 6 && play0) {
        avatar_play_slot(c, as, 0);
        if (coop_anim_is_action(rs.anim) || coop_look_log())  // (only actions: the others change all the time)
            LOG("[coop] avatar plays slot 0 %s (seq %u)\n", as, (unsigned)rs.anim_seq);
        avatar.anim = rs.anim;
        avatar.anim_seq = rs.anim_seq;
        snprintf(avatar.as, sizeof avatar.as, "%s", as);
    }
    if (avatar.frames >= 6 && play1) {
        avatar_play_slot(c, want_u, 1);
        if (u_action || coop_look_log())
            LOG("[coop] avatar plays slot 1 %s (seq %u)\n", want_u, (unsigned)rs.anim_upper_seq);
        snprintf(avatar.as_upper, sizeof avatar.as_upper, "%s", want_u);
        avatar.anim_upper = rs.anim_upper;
        avatar.anim_upper_seq = rs.anim_upper_seq;
    }
    if (avatar.frames >= 6) armor_resync = false;
    if (avatar.frames >= 6 && face_on() && !tree) {  // (protocol 25) the face, in AS slot 3, on the avatar only
        const char *fa = avatar_face_as(rs.face);
        if (strcmp(fa, avatar.as_face)) {
            play_anim(c, avatar.actor, avatar_as_alias(fa), 3);  // (CoopX<name>: coop/astree.inc)
            if (coop_look_log()) LOG("[coop] avatar face %s\n", fa);
            snprintf(avatar.as_face, sizeof avatar.as_face, "%s", fa);
        }
    }
    // Smooth toward the received state: packets come 20 times a second, frames 30-120.
    // (coop-multi: static uint64_t last_ns; -> per avatar, avl)
    double dt = avl.avatar_last_ns && now > avl.avatar_last_ns ? (now - avl.avatar_last_ns) / 1e9 : 1 / 60.0;
    avl.avatar_last_ns = now;
    float k = (float)(dt * 12 > 1 ? 1 : dt * 12), d2 = 0;
    // (2026-11-21, elevators: the partner sank into a rising shrine lift by ~v * 0.15 s) Vertical lead: the sent height
    // plus its vertical speed (from the last two packets) times the smoothing lag + the packet's age, at most 0.2 s,
    // only for steady climbs / descents under 8 m/s. SAVAGEWIND_COOP_AVLEAD=0: off.
    static int lead_c = -1;
    if (env_on("SAVAGEWIND_COOP_AVLEAD", &lead_c)) {
        if (!avl.lead_have || rs.pos[1] != avl.lead_y) {
            double pdt = avl.lead_have && now > avl.lead_ns ? (now - avl.lead_ns) / 1e9 : 0;
            float vy = pdt > 0.02 && pdt < 0.3 ? (float)((rs.pos[1] - avl.lead_y) / pdt) : 0;
            avl.lead_vy = avl.lead_have ? avl.lead_vy * 0.5f + vy * 0.5f : 0;
            avl.lead_y = rs.pos[1], avl.lead_ns = now, avl.lead_have = true;
        } else if (now - avl.lead_ns > 120000000ull) {
            avl.lead_vy = 0;  // (the same height for a while: standing)
        }
        float vy = avl.lead_vy;
        if (fabsf(vy) > 0.3f && fabsf(vy) < 8.0f && age < 0.3) {
            float lead = (float)(1.0 / 12 + age);
            if (lead > 0.2f) lead = 0.2f;
            rs.pos[1] += vy * lead;
        }
    }
    for (int i = 0; i < 3; i++) d2 += (rs.pos[i] - avatar.pos[i]) * (rs.pos[i] - avatar.pos[i]);
    if (d2 > 20 * 20) k = 1;  // (far: jump there)
    for (int i = 0; i < 3; i++) avatar.pos[i] += (rs.pos[i] - avatar.pos[i]) * k;
    float dy = fmodf(rs.yaw - avatar.yaw + 540, 360) - 180;
    avatar.yaw += dy * k;
    memcpy(avatar.mtx_prev, (void *)(uintptr_t)(avatar.actor + ACTOR_MTX), sizeof avatar.mtx_prev);
    avatar.have_prev = avatar.frames > 1;
    float wpos[3] = {avatar.pos[0], avatar.pos[1], avatar.pos[2]};
    if (evcalm_hide(avatar.pos)) wpos[1] -= EVCALM_PARK_M;  // (coop/evcalm.inc: out of sight in a shared cutscene)
    write_mtx(avatar.actor + ACTOR_MTX, wpos, avatar.yaw);
    write_mtx(avatar.actor + ACTOR_MTX2, wpos, avatar.yaw);
}
