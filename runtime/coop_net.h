// Co-op networking (runtime/coop_net.c): the local and the remote player's state over UDP.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// (protocol 14) One piece of gear the player carries: its actor name, the bone of the player's body it sits on and its
// matrix relative to that bone (3x4, rows like the game's matrices). The avatar shows a puppet of it on its own bone.
// (protocol 49) COOP_GEAR_SLATE: the Sheikah Slate (actor Item_Conductor: on the hip, in the hand while a rune is used).
enum { COOP_GEAR_WEAPON, COOP_GEAR_SHIELD, COOP_GEAR_BOW, COOP_GEAR_GLIDER, COOP_GEAR_CARRIED, COOP_GEAR_SLATE, COOP_GEAR_COUNT };
enum { COOP_BONE_NONE, COOP_BONE_WEAPON_R, COOP_BONE_WEAPON_L, COOP_BONE_POD_A, COOP_BONE_POD_B, COOP_BONE_SPINE_2,
       COOP_BONE_ROOT, COOP_BONE_ARM_R, COOP_BONE_COUNT };  // (protocol 72) ARM_R: Arm_2_R, the right forearm
#define COOP_ARMOR_SLOTS 3
typedef struct {
    char name[32];  // actor name ("Weapon_Shield_001", "PlayerStole2"), letters/digits/_ only; "" = none
    uint8_t bone;   // COOP_BONE_*; NONE = not shown
    uint8_t pad[3];
    float rel[12];
} CoopGear;

// (protocol 49) Items held in the hands from the inventory (apples...): the game puts them in an invisible "CarryBox"
// (sent as gear COOP_GEAR_CARRIED); each item's position is relative to that box (box space, metres).
#define COOP_HELD_ITEMS 5
#define COOP_CARRY_BOX "CarryBox"
typedef struct {
    char name[32];  // actor name ("Item_Fruit_A"), same rules as CoopGear.name; "" = none
    float pos[3];   // finite, |x| < 5
} CoopHeldItem;

// (66) HashId and lock serial name the frozen object in the sender's area.
// Direction is a unit vector quantized to signed 1/127 steps, charge in hundredths.
enum { COOP_STASISFX_ARROW = 1, COOP_STASISFX_VISIBLE = 2, COOP_STASISFX_START = 4 };
enum { COOP_WEATHER_TYPES = 9 };
enum { COOP_MOON_DAY_UNKNOWN, COOP_MOON_DAY_OFF, COOP_MOON_DAY_ON };
typedef struct {
    uint32_t hash;
    uint16_t serial;
    int8_t direction[3];
    uint8_t charge, flags, foe_ticks;  // (85) remaining enemy Stasis time in native 30 Hz units
} CoopStasisFx;

typedef struct {
    float pos[3];
    float yaw;    // degrees, the convention of runtime/coop.c's matrices
    float speed;  // m/s on the ground plane
    uint16_t anim;  // COOP_ANIM_*
    uint8_t anim_seq;  // (protocol 19) action-instance counter (increments on every action/attack start)
    uint8_t flags;  // COOP_FLAG_*
    uint32_t area;  // crc32 of the area (runtime/coop.c local_area): the avatar shows only in the same area
    char weapon[32];  // (protocol 11) the melee weapon in the player's hand at the last swing ("Weapon_Sword_044"), or ""
    // (protocol 14) the avatar's look: what the player carries (where it sits on the body) and wears
    CoopGear gear[COOP_GEAR_COUNT];
    char armor[COOP_ARMOR_SLOTS][32];  // head, upper, lower armor actor names ("Armor_001_Upper"), "" = none
    uint16_t anim_upper;      // (protocol 22) upper-body action state (COOP_ANIM_UPPER_*)
    uint8_t anim_upper_seq;  // (protocol 22) upper-body action-instance counter
    uint8_t face;  // (protocol 25) index of the face AS (runtime/coop/faces.inc), 0 = none; < COOP_FACE_COUNT
    // (protocol 45) where the player's upper body aims (bow...): the game's own look-at target (runtime/coop/aim.inc)
    // as a direction from the chest: pitch -90..90 degrees (up +), yaw -127..127 degrees from the body's yaw;
    // aim_flags COOP_AIM_* (0 = not aiming)
    int8_t aim_pitch, aim_yaw;
    uint8_t aim_flags;
    CoopHeldItem held[COOP_HELD_ITEMS];  // (protocol 49) only while gear[COOP_GEAR_CARRIED] is the CarryBox
    float magbeam_tip[3];  // (64) world-space contact on the held object, with COOP_FLAG_MAGBEAM
    CoopStasisFx stasisfx;
    uint8_t weather;          // 0 unknown, otherwise game index + 1
    uint8_t moon_day;         // COOP_MOON_DAY_*
    uint16_t bloodmoon;        // host's running count of blood moon cutscenes
} CoopState;
enum { COOP_AIM_ON = 1, COOP_AIM_PITCH = 2, COOP_AIM_YAW = 4, COOP_AIM_FLAGS = 7 };

// (protocol 22) Upper-body action states for slot 1 skeletal blending
enum {
    COOP_ANIM_UPPER_NONE,
    COOP_ANIM_UPPER_BOW,
    COOP_ANIM_UPPER_BOW_SHOOT,
    COOP_ANIM_UPPER_GUARD,
    COOP_ANIM_UPPER_GUARD_JUST,
    COOP_ANIM_UPPER_CARRY,
    COOP_ANIM_UPPER_CARRY_THROW,
    COOP_ANIM_UPPER_THROW_AIM,
    COOP_ANIM_UPPER_THROW,
    // (protocol 24) runes and items held or used while moving
    COOP_ANIM_UPPER_RUNE_AIM, COOP_ANIM_UPPER_BOMB_READY, COOP_ANIM_UPPER_MAGNE, COOP_ANIM_UPPER_SCOPE,
    COOP_ANIM_UPPER_EQUIP_ON, COOP_ANIM_UPPER_EQUIP_OFF, COOP_ANIM_UPPER_WHISTLE,
    // (protocol 28) no-weapon shrug, bow reload, catching a thrown weapon, left-hand item on / off (all one-shot)
    COOP_ANIM_UPPER_EQUIP_NG, COOP_ANIM_UPPER_BOW_RELOAD, COOP_ANIM_UPPER_THROW_CATCH, COOP_ANIM_UPPER_ITEM_L_ON,
    COOP_ANIM_UPPER_ITEM_L_OFF,
    COOP_ANIM_UPPER_COUNT
};
static inline bool coop_anim_upper_is_action(int a) {
    return a == COOP_ANIM_UPPER_BOW_SHOOT ||
           a == COOP_ANIM_UPPER_GUARD_JUST ||
           a == COOP_ANIM_UPPER_CARRY_THROW ||
           a == COOP_ANIM_UPPER_THROW ||
           a == COOP_ANIM_UPPER_EQUIP_ON || a == COOP_ANIM_UPPER_EQUIP_OFF || a == COOP_ANIM_UPPER_WHISTLE ||
           (a >= COOP_ANIM_UPPER_EQUIP_NG && a <= COOP_ANIM_UPPER_ITEM_L_OFF);
}

#define COOP_FACE_COUNT 83  // 1 + the player's 82 Face* AS (runtime/coop/faces.inc)

// (protocol 29) Tree mode: the player's own AS trees on the avatar. Each side sends its player's AS input table (the
// values every selector / blender of the player's AS reads, runtime/coop/astree.inc) and the AS it plays in each slot
// and layer; the other side writes the inputs into the avatar's table and plays the same AS by name. A datagram of its
// own ("SWCA"), 20 times a second like the state packet.
#define COOP_AS_NAME_COUNT 400   // 1 + the player's 399 AS (loaded from the local overlay)
#define COOP_AS_STRING_COUNT 126 // 1 + the case strings of the player's string selectors (coop_as_strings)
#define COOP_ASIN_FLOATS 20      // float kinds sent (runtime/coop/astree.inc astree_float_kinds)
#define COOP_ASIN_STRS 11        // string kinds sent (astree_str_kinds)
#define COOP_AS_SLOTS 5
#define COOP_AS_LAYERS 3
#define COOP_ASTREE_VALID 1u     // flags: the sender has a player and a world (else: use the old classified path)
#define COOP_ASTREE_FX 2u        // flags (protocol 39): fx[] holds the player's effect-set (ELink) property values
// (protocol 39) The player's effect-set properties (ELink user Player_GameRom: ground material, water depth, wetness,
// speed...), sent with the tree; the other game writes them into its avatar's effect set (the avatar has the player's
// effect set: runtime/coop/avatarfx.inc), so the effects the AS start pick the same variants (water splash, ripples,
// dust). Order = coop_fx_prop_names in avatarfx.inc (append only); raw 32-bit values (float or int by property type).
#define COOP_FX_PROPS 18
// (protocol 81) Eating / drinking from the inventory: the pause-menu Link's UseItem case (runtime/coop/eat.inc).
enum { COOP_EAT_NONE, COOP_EAT_FOOD, COOP_EAT_DISH, COOP_EAT_SKEWER, COOP_EAT_BIZARRE, COOP_EAT_HARD, COOP_EAT_DRINK,
       COOP_EAT_FAIRY, COOP_EAT_MAX = COOP_EAT_FAIRY };
typedef struct {
    float f[COOP_ASIN_FLOATS];   // finite, |x| <= 100000
    float normal[3];             // GroundNormal (kind 21), finite, |x| <= 2
    float vel[3];                // the player's velocity (actor+0x400), m per frame, |x| <= 20
    uint64_t bools;              // Bool (kind 66) bit set
    int16_t charge, combo;       // kinds 42, 43; -1000..1000
    uint8_t str[COOP_ASIN_STRS]; // index into coop_as_strings (0 = ""), < COOP_AS_STRING_COUNT
    uint8_t flags;               // COOP_ASTREE_*
    uint16_t as[COOP_AS_SLOTS * COOP_AS_LAYERS];  // slot * COOP_AS_LAYERS + layer: index into coop_as_names, 0 = none
    uint8_t seq[COOP_AS_SLOTS * COOP_AS_LAYERS];  // per slot / layer: counts restarts of the same AS
    uint8_t horse_aim[2];        // (46) little-endian u16: 0 none, else 1 + frame * 64 of layer 0.1 AimHorseLower*
    uint8_t charge_lv;             // (47) spin-attack charge level of the sender (player+0x1f94 while CutCharge*): 0..2
    uint32_t fx[COOP_FX_PROPS];  // (protocol 39) with COOP_ASTREE_FX: each a finite float or an int in +-100000
    uint32_t fx_mask;            // bit i: fx[i] was read from the sender's player (else 0, not to be written)
    uint8_t eat;                 // (81) 0, or what the sender eats / drinks from the menu now: COOP_EAT_* (1..COOP_EAT_MAX)
    uint8_t eat_seq;             // (81) counts the sender's uses (a new one restarts the animation)
    uint16_t champ;              // (89) Champion abilities of the sender (COOP_CHAMP_*: runtime/coop/champ.inc)
} CoopAsTree;
// (protocol 89) CoopAsTree.champ: the sender's Champion ability effects. Bits 0-3: persistent effects the player shows
// now (the player's own ELink keys Kago_Shield, Kago_ShieldCount_<n>, Kago_Range, Kago_Ready); bits 4-5: n, the
// shield count shown (1..3, with COOP_CHAMP_COUNT only); bits 6-7: the count the last broken shield had (1..3);
// bits 8-15: four 2-bit use counters (a change = one new use): Urbosa's Fury, Mipha's Grace, Revali's Gale launch,
// Daruk's shield broken by a hit.
#define COOP_CHAMP_SHIELD 0x1u
#define COOP_CHAMP_COUNT 0x2u
#define COOP_CHAMP_RANGE 0x4u
#define COOP_CHAMP_READY 0x8u
#define COOP_CHAMP_HELD 0xfu
#define COOP_CHAMP_COUNT_SHIFT 4
#define COOP_CHAMP_COUNT_MASK 0x30u
#define COOP_CHAMP_BREAK_SHIFT 6
#define COOP_CHAMP_BREAK_MASK 0xc0u
enum { COOP_CHAMP_URBOSA, COOP_CHAMP_MIPHA, COOP_CHAMP_REVALI, COOP_CHAMP_BREAK, COOP_CHAMP_USES };
#define COOP_CHAMP_USE_SHIFT(k) (8 + 2 * (k))
bool coop_champ_ok(uint16_t champ, uint8_t tree_flags);  // (the receiver's validation; tests)
void coop_net_set_astree(const CoopAsTree *t);
bool coop_net_remote_astree(CoopAsTree *out, double *age_s);  // false until one arrived (this peer session)
bool coop_astree_ok(const CoopAsTree *t);  // (the receiver's validation; tests)
// What the avatar shows (runtime/coop.c maps the player's own AS to these, and these to the avatar's Coop* AS).
enum {
    COOP_ANIM_WAIT, COOP_ANIM_WALK, COOP_ANIM_RUN, COOP_ANIM_DASH, COOP_ANIM_JUMP, COOP_ANIM_FALL, COOP_ANIM_GLIDE,
    COOP_ANIM_SWIM_WAIT, COOP_ANIM_SWIM, COOP_ANIM_SWIM_FAST, COOP_ANIM_CLIMB_WAIT, COOP_ANIM_CLIMB,
    // sword attacks (protocol 8): the player's Cut* AS
    COOP_ANIM_CUT1, COOP_ANIM_CUT2, COOP_ANIM_CUT3, COOP_ANIM_CUT_FINISH, COOP_ANIM_CUT_DASH, COOP_ANIM_CUT_SPIN,
    // (protocol 14) more of the player's states
    COOP_ANIM_SQUAT_WAIT, COOP_ANIM_SQUAT_MOVE, COOP_ANIM_GUARD, COOP_ANIM_BOW, COOP_ANIM_HORSE_WAIT, COOP_ANIM_HORSE_RUN,
    COOP_ANIM_CLIMB_DOWN, COOP_ANIM_CLIMB_LEFT, COOP_ANIM_CLIMB_RIGHT,
    // (protocol 19) expanded combat, dodges, parry, damage, and death
    COOP_ANIM_GUARD_JUST, COOP_ANIM_BACKJUMP, COOP_ANIM_SIDEHOP, COOP_ANIM_LAND, COOP_ANIM_DAMAGE, COOP_ANIM_BOW_SHOOT,
    COOP_ANIM_DIE,
    // (protocol 20) weapon throw aim & throw, object carry & throw
    COOP_ANIM_THROW_AIM, COOP_ANIM_THROW, COOP_ANIM_CARRY, COOP_ANIM_CARRY_THROW,
    // (protocol 23) movement family: shield surfing, ladders, slope slide, tumbling down, wall jump, pushing,
    // out of stamina, braking
    COOP_ANIM_SURF, COOP_ANIM_SURF_AIR, COOP_ANIM_SURF_JUMP, COOP_ANIM_SURF_LAND, COOP_ANIM_SURF_ON,
    COOP_ANIM_SURF_OFF, COOP_ANIM_SURF_STAGGER,
    COOP_ANIM_LADDER_WAIT, COOP_ANIM_LADDER_UP, COOP_ANIM_LADDER_DOWN,
    COOP_ANIM_SLIDE, COOP_ANIM_ROLLDOWN, COOP_ANIM_CLIMB_DASH, COOP_ANIM_PUSH, COOP_ANIM_TIRED, COOP_ANIM_BRAKE,
    // (protocol 24) items and the Sheikah Slate: pick up, chests, terminals, cooking result, look around, dive
    COOP_ANIM_GET, COOP_ANIM_CHEST_KICK, COOP_ANIM_CHEST_OPEN,
    COOP_ANIM_SLATE_SET, COOP_ANIM_SLATE_LOOK, COOP_ANIM_SLATE_TOUCH, COOP_ANIM_SLATE_DOWNLOAD, COOP_ANIM_SLATE_WAIST,
    COOP_ANIM_COOK_GOOD, COOP_ANIM_COOK_BAD, COOP_ANIM_LOOK_AROUND, COOP_ANIM_DIVE,
    // (protocol 26) the rest: talking, doors, mounting, sand-seal surfing, warp, frozen, knocked down, swim and
    // climb extras. Loops first, then the one-shot actions (COOP_ANIM_TALK_HELLO..COOP_ANIM_STAGGER).
    COOP_ANIM_TALK, COOP_ANIM_TALKING, COOP_ANIM_TALK_SHOW, COOP_ANIM_TALK_SLATE, COOP_ANIM_HORSE_SOOTHE,
    COOP_ANIM_HORSE_RODEO, COOP_ANIM_WAKEBOARD, COOP_ANIM_WAKEBOARD_AIR, COOP_ANIM_FREEZE, COOP_ANIM_UNSTEADY,
    COOP_ANIM_WATERFALL, COOP_ANIM_CLIMB_SLIP, COOP_ANIM_TALK_HELLO, COOP_ANIM_TALK_YES, COOP_ANIM_TALK_SURPRISE,
    COOP_ANIM_TALK_EMBARRASS, COOP_ANIM_TALK_HANDOVER, COOP_ANIM_DOOR_PUSH, COOP_ANIM_DOOR_SLIDE, COOP_ANIM_MOUNT,
    COOP_ANIM_DISMOUNT, COOP_ANIM_HORSE_JUMP, COOP_ANIM_WARP, COOP_ANIM_FREEZE_BREAK, COOP_ANIM_LAND_DAMAGE,
    COOP_ANIM_STANDUP, COOP_ANIM_GUARD_BREAK, COOP_ANIM_GUARD_HIT, COOP_ANIM_SWIM_DIE, COOP_ANIM_SWIM_JUMP,
    COOP_ANIM_SWIM_ATTACK, COOP_ANIM_CLIMB_JUMP, COOP_ANIM_CLIMB_EDGE, COOP_ANIM_PARA_ON, COOP_ANIM_STAGGER,
    // (protocol 27) idles: random fidgets (one-shot), hot / cold, lock-on stance, hurt
    COOP_ANIM_IDLE_SLEEPY, COOP_ANIM_IDLE_RELAX, COOP_ANIM_IDLE_FLEX, COOP_ANIM_IDLE_WIPE, COOP_ANIM_COLD_FIDGET,
    COOP_ANIM_HOT_FIDGET, COOP_ANIM_COLD_WAIT, COOP_ANIM_HOT_WAIT, COOP_ANIM_LOCKON_WAIT, COOP_ANIM_HURT_WAIT,
    // (protocol 28) jump / fall / horseback sword attacks, horse extras, putting down, climb extras, selfie, turns.
    // Loops first, then the one-shot actions (COOP_ANIM_CUT_JUMP_ST..COOP_ANIM_DEMO_TURN).
    COOP_ANIM_CUT_JUMP_LOOP, COOP_ANIM_CUT_FALL, COOP_ANIM_CUT_HORSE_CHARGE, COOP_ANIM_SELFIE,
    COOP_ANIM_DEMO_LOOKAROUND, COOP_ANIM_CUT_JUMP_ST, COOP_ANIM_CUT_JUMP_LAND, COOP_ANIM_CUT_FALL_LAND,
    COOP_ANIM_CUT_HORSE, COOP_ANIM_HORSE_CRASH, COOP_ANIM_HORSE_SLIP, COOP_ANIM_HORSE_COURBETTE,
    COOP_ANIM_HORSE_DEAD, COOP_ANIM_HORSE_SPUR, COOP_ANIM_GRAB_PUT, COOP_ANIM_PARA_OFF, COOP_ANIM_LIFT_PUT_POUCH,
    COOP_ANIM_CLIMB_TREE_TOP, COOP_ANIM_CLIMB_TREE_ED, COOP_ANIM_CLIMB_TREE_TURN, COOP_ANIM_GROUND_DOWN_WALL,
    COOP_ANIM_LADDER_JUMP, COOP_ANIM_LADDER_TO_CLIMB, COOP_ANIM_WALL_OFF, COOP_ANIM_WALL_OFF_SQUAT,
    COOP_ANIM_MASTER_DEAD, COOP_ANIM_TURN, COOP_ANIM_TURN_LOCKON, COOP_ANIM_DEMO_TURN,
    COOP_ANIM_COUNT
};
static inline int coop_anim_is_surf(int a) { return a >= COOP_ANIM_SURF && a <= COOP_ANIM_SURF_STAGGER; }
static inline int coop_anim_is_cut(int a) { return a >= COOP_ANIM_CUT1 && a <= COOP_ANIM_CUT_SPIN; }
static inline bool coop_anim_is_action(int a) {
    return coop_anim_is_cut(a) ||
           a == COOP_ANIM_GUARD_JUST ||
           a == COOP_ANIM_BACKJUMP ||
           a == COOP_ANIM_SIDEHOP ||
           a == COOP_ANIM_LAND ||
           a == COOP_ANIM_DAMAGE ||
           a == COOP_ANIM_BOW_SHOOT ||
           a == COOP_ANIM_THROW ||
           a == COOP_ANIM_CARRY_THROW ||
           a == COOP_ANIM_SURF_JUMP || a == COOP_ANIM_SURF_LAND || a == COOP_ANIM_SURF_ON ||
           a == COOP_ANIM_SURF_OFF || a == COOP_ANIM_SURF_STAGGER ||
           a == COOP_ANIM_CLIMB_DASH || a == COOP_ANIM_BRAKE ||
           a == COOP_ANIM_GET || a == COOP_ANIM_CHEST_KICK || a == COOP_ANIM_CHEST_OPEN ||
           a == COOP_ANIM_SLATE_SET || a == COOP_ANIM_SLATE_TOUCH || a == COOP_ANIM_DIVE ||
           (a >= COOP_ANIM_TALK_HELLO && a <= COOP_ANIM_STAGGER) ||
           (a >= COOP_ANIM_IDLE_SLEEPY && a <= COOP_ANIM_HOT_FIDGET) ||
           (a >= COOP_ANIM_CUT_JUMP_ST && a <= COOP_ANIM_DEMO_TURN) ||
           a == COOP_ANIM_DIE;
}
#define COOP_FLAG_IN_WORLD 1u  // a world is loaded (else the state is stale and the avatar hidden)
#define COOP_FLAG_HURT 2u      // (protocol 50) the sender's Link shows the red damage colour now (the avatar flashes)
#define COOP_FLAG_MAGBEAM 4u   // (protocol 64) the sender's Magnesis is holding an object
#define COOP_FLAG_BURNING 8u   // (protocol 82) the weapon in the sender's hand is burning (a lit torch...)
#define COOP_FLAG_LINK_FIRE 16u  // (95) the sender's Link is burning after friendly fire
#define COOP_FLAG_LINK_ICE 32u   // (95) the sender's Link is frozen after friendly fire
#define COOP_FLAG_LINK_SHOCK 64u // (95) the sender's Link is shocked after friendly fire
#define COOP_FLAG_BLUE_FIRE 128u // (98) the burning weapon carries the game's blue flame
#define COOP_FLAG_LINK_ELEM (COOP_FLAG_LINK_FIRE | COOP_FLAG_LINK_ICE | COOP_FLAG_LINK_SHOCK)
#define COOP_MAGBEAM_RANGE 60.0f

static inline float coop_magbeam_dist2(const float a[3], const float b[3]) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

void coop_net_init(void);  // reads SAVAGEWIND_COOP_PORT / SAVAGEWIND_COOP_PEER; does nothing without them
bool coop_net_on(void);

// (2026-11-17, co-op menu) Starting the network on demand. coop_net_init() = coop_net_env_config() + coop_net_start():
// the environment (the launchers, every test script) decides exactly as before. Without it the co-op stays off until
// the game's Options > Co-op page (runtime/ui/coop_menu.mm) fills a config and starts it.
#define COOP_NAME_LEN 24  // a player's name in bytes, NUL included (UTF-8, coop_name_ok)
enum { COOP_MODE_SOLO, COOP_MODE_HOST, COOP_MODE_JOIN };
typedef struct {
    int mode;            // COOP_MODE_HOST / _JOIN (SOLO: nothing starts)
    int port;            // UDP port to listen on (0: any; a guest's)
    char peer[128];      // join: "<address>[:<port>]" of the host (IPv4 or a host name); host: ""
    char code[64];       // the join code ("" none)
    char name[COOP_NAME_LEN];  // this player's name shown to the others ("" none)
    bool loopback;       // host a room reachable only from this PC
} CoopNetConfig;
bool coop_net_env_config(CoopNetConfig *out);  // false: no SAVAGEWIND_COOP_PORT / _PEER (menu decides)
bool coop_net_start(const CoopNetConfig *cfg);  // false: not started (coop_net_error says why); already on: false
void coop_net_stop(void);  // ends the network thread and forgets every player (the game side: reboot after it)
bool coop_net_env_driven(void);  // started from the environment (a launcher / test), not the menu
const char *coop_net_room_code(void);  // current host code for display, empty for an open room
int coop_net_local_join_port(void);  // host port reachable at 127.0.0.1, or 0
bool coop_net_split_room(void);  // room created on demand for split screen
const char *coop_net_error(void);  // the last start failure ("" none)
// The co-op menu (title button, Options > Co-op): on unless SAVAGEWIND_COOP_MENU=0
bool coop_menu_on(void);

// (protocol 62) Player names and ping: a seventh datagram kind ("SWCN", CoopNamePacket), twice a second. A guest
// sends its own name; the host sends every player's name and each guest's ping to it.
// (DISHINV) Names are UTF-8 ("Zoë"): well-formed (no overlong forms, surrogates or code points above U+10FFFF), no
// control characters (C0, DEL, C1), NUL-terminated inside COOP_NAME_LEN bytes.
void coop_net_set_name(const char *name);  // ours, through coop_name_clean
bool coop_name_ok(const char *s);  // s (COOP_NAME_LEN bytes readable) is a valid name
// out: name cleaned: leading / trailing spaces off, a control character or a broken UTF-8 sequence -> one '?', cut at
// a character boundary to fit COOP_NAME_LEN - 1 bytes
void coop_name_clean(const char *name, char out[COOP_NAME_LEN]);
// The UTF-8 character at s (avail bytes may be read): its length (1..4) and code point; 0: broken, overlong, a
// surrogate, above U+10FFFF, or a control character
int coop_utf8_char(const char *s, int avail, uint32_t *cp);
// (protocol 65, coop-looks) Each player's tunic colour (Options > Co-op), sent with the names; the others show that
// player's Link in it (coop/looks.inc). 0 = the game's own colours.
enum { COOP_TINT_NONE, COOP_TINT_RED, COOP_TINT_BLUE, COOP_TINT_GREEN, COOP_TINT_YELLOW, COOP_TINT_PURPLE,
       COOP_TINT_BLACK, COOP_TINT_WHITE, COOP_TINT_COUNT };
void coop_net_set_tint(int tint);         // ours
int coop_net_player_tint(int pid);        // that player's (0 unknown / none)
bool coop_net_player_name(int pid, char *out, size_t cap);  // that player's name; false: none known
// (coop-looks) Names above the other players' heads: the game thread places them (coop/looks.inc), the overlay
// (ui/menu.mm) draws them. x, y: in the window, 0..1; alpha 0..1; size: a scale (1 near).
typedef struct { float x, y, alpha, size, aspect; int tint; char name[COOP_NAME_LEN]; } CoopNameTag;  // (aspect: the game image's)
int coop_name_tags(CoopNameTag *out, int max);  // the tags to draw now (0: none)
// (coop-mapmark) The other players as arrows on our map (coop/mapmark.inc places them, ui/menu.mm draws them). x, y:
// the game's UI layout (1280x720, origin at the centre, y up); angle: radians, counter-clockwise from "up"; size:
// layout pixels; alpha 0..1; tint: COOP_TINT_*.
typedef struct { float x, y, angle, size, alpha; int tint; } CoopMapArrow;
int coop_map_arrows(CoopMapArrow *out, int max);  // the arrows to draw now (0: none)
typedef struct {
    int pid;                    // player id (host 0)
    char name[COOP_NAME_LEN];   // "" while unknown
    int ping_ms;                // round trip to the host (the host: to that guest); -1 unknown
    double age_s;               // since its last state packet (-1: none yet); us: 0
    bool me, host, partner;
    bool in_world;              // its newest state says it is in the world (pos: where)
    float pos[3];
} CoopLobbyEntry;
int coop_net_lobby(CoopLobbyEntry *out, int max);  // us first, then the others the host knows of; count
void coop_net_set_local(const CoopState *s);
bool coop_net_get_remote(CoopState *out, double *age_s);  // false until a packet arrived
bool coop_net_get_host(CoopState *out, double *age_s, uint64_t *session);
bool coop_net_set_enemy_generation(uint16_t generation);  // clear old damage when the host's blood moon rolls over
void coop_net_stats(uint32_t *sent, uint32_t *received, bool *peer_known);

// (protocol 56, 2026-11-16) More than two players: a star around the host. The host accepts up to
// COOP_MAX_PLAYERS - 1 guests and relays each one's state / AS tree / ragdoll pose to the others. Player ids: host 0,
// guests 1..7 (given by the host). One guest is the host's "partner" (the first to join): all world sync (enemies,
// objects, events, gates...) runs between the host and the partner as in the two-player game; the other guests
// ("extra") see everyone's figure and the host's time, their world is their own. On an extra guest the partner calls
// (coop_net_get_remote and the rest) return nothing. coop/multi.inc draws the other players' figures.
#define COOP_MAX_PLAYERS 8
int coop_net_my_id(void);       // 0 on the host; a guest: its id once the host told it (-1 before)
int coop_net_partner_id(void);  // the world partner's id (-1: none: alone, or an extra guest)
uint32_t coop_net_players(double max_age_s);  // bit per other player id heard of within max_age_s (not ours)
// (game thread only) While pid >= 0, coop_net_get_remote / _remote_astree / _remote_pose / _remote_horse /
// _remote_bombs / _remote_shot_views return player pid's data instead of the partner's. -1: back to the partner.
void coop_net_view(int pid);  // -1: the partner again; COOP_VIEW_NONE: nobody (no data)
#define COOP_VIEW_NONE 0xff
// (58) The other players whose world is shared with ours (bit per id), heard of within max_age_s. With
// SAVAGEWIND_COOP_MULTIWORLD (on unless =0 on the host) every guest shares the world: the host keeps each guest's
// enemies and events, merges them for its own game and relays each guest's world events to the others.
uint32_t coop_net_world_players(double max_age_s);

// Shared enemies (runtime/coop.c): per enemy (its map HashId), the damage this side's player dealt, as a running
// total. Totals only grow, so the newest one received is all that matters: packet loss and reordering are harmless.
// The host also sends where each enemy is (the host runs the real enemy; the guest's copy follows it with its AI off).
#define COOP_ENEMY_MAX 16
// (n6, host only, not on the wire) The host's own list may hold more entries than one packet: each guest's packet
// (build_enemies_for) picks from it, the enemies that guest has loaded first, so with 3+ players an enemy another guest
// runs is never left out of a guest's packet (left out = "the host does not have it" = that guest ran it too).
#define COOP_ENEMY_LOCAL_MAX 48
#define COOP_ENEMY_POS 1u  // flags: pos/rot are valid: the sender runs this enemy's AI (the other side follows it)
#define COOP_ENEMY_LOADED 2u      // (protocol 9) the sender has this enemy loaded now
#define COOP_ENEMY_GUEST_RUNS 4u  // (protocol 9, from the host) the guest runs this enemy: its player is nearer
typedef struct {
    uint32_t hash, damage, flags;
    float pos[3];       // its physics body's position (body + 0x1a0; about 1 m above its feet)
    float rot[4];       // and rotation (quaternion, body + 0x1d0)
    char as[32];        // the AS it plays in slot 0 (with COOP_ENEMY_POS; NUL-terminated, "" = unknown)
} CoopEnemyDamage;
void coop_net_set_enemies(const CoopEnemyDamage *e, int n);  // this side's totals (sent with every state packet;
                                                             // n <= COOP_ENEMY_MAX, the host up to COOP_ENEMY_LOCAL_MAX)
uint32_t coop_net_remote_damage(uint32_t hash);               // the peer's total for that enemy (0: none known)
// The peer's newest position for that enemy and its age; false when the peer never sent one.
// as (may be NULL): its AS name, cap bytes.
bool coop_net_remote_enemy_pos(uint32_t hash, float pos[3], float rot[4], double *age_s, char *as, size_t cap);
// The flags of the peer's newest entry for that enemy and its age; false when the peer never sent it.
bool coop_net_remote_enemy_flags(uint32_t hash, uint32_t *flags, double *age_s);
// Hour of day (0..24; negative: unknown), sent in the enemy packet: the host's clock is the shared one.
void coop_net_set_hour(float hour);
bool coop_net_remote_hour(float *hour, double *age_s);
bool coop_net_is_host(void);  // started without SAVAGEWIND_COOP_PEER: this side runs the shared enemies

// (protocol 12) Enemies the game created at run time (no map HashId): the side whose game created one ("native")
// gives it an id (bit 31 set; bit 30: the host's) and runs it; the other side creates a copy that follows it.
// Entries of such enemies carry COOP_ENEMY_SPAWNED (only from the native side). Their actor names travel separately,
// a few per packet.
#define COOP_ENEMY_SPAWNED 8u
// (protocol 55) With COOP_ENEMY_POS: the enemy's head looks at a player (its look-at request is on); _PARTNER: at the
// sender's partner (= the receiver's own Link), else at the sender's own Link; _BATTLE: the look-at's battle mode.
#define COOP_ENEMY_LOOK 16u
#define COOP_ENEMY_LOOK_PARTNER 32u
#define COOP_ENEMY_LOOK_BATTLE 64u
// (protocol 56) With COOP_ENEMY_POS: pos/rot are the actor's own matrix (its feet), not a physics body (small animals
// whose body the sender cannot find, e.g. squirrels: position-only sync, runtime/coop/enemies.inc).
#define COOP_ENEMY_MTXPOS 128u
// (protocol 61, n6) With COOP_ENEMY_LOOK: _LOOK_PID set = the player it looks at, by player id (bits 13..15; 0 = the
// host), the same for every receiver: the host relays it unchanged, also for enemies another guest runs. LOOK_PARTNER
// (relative to the sender) is still set for two players. (Bits 8..11 are host-only: never on the wire.)
#define COOP_ENEMY_LOOK_PID 0x1000u
#define COOP_ENEMY_LOOK_PID_SHIFT 13
#define COOP_ENEMY_LOOK_PID_MASK (7u << COOP_ENEMY_LOOK_PID_SHIFT)
// (84) Native status observed on a loaded enemy. Mirrored effects are never reported back.
#define COOP_ENEMY_BURNING (1u << 16)
#define COOP_ENEMY_FROZEN (1u << 17)
#define COOP_ENEMY_SHOCKED (1u << 18)
#define COOP_ENEMY_ELEM_MASK (COOP_ENEMY_BURNING | COOP_ENEMY_FROZEN | COOP_ENEMY_SHOCKED)
#define COOP_ENEMY_ELEM_FRESH_NS 750000000ull
uint32_t coop_net_remote_enemy_elem(uint32_t hash);
// (58, host only: never on the wire) In the host's own entries (coop_net_set_enemies): which player runs the enemy
// (0: the host; a guest's id), and COOP_ENEMY_COPY: the host's copy of a guest's run-time enemy (the owner = that
// guest). coop_net.c turns them into each guest's flags (COOP_ENEMY_GUEST_RUNS, relayed positions).
#define COOP_ENEMY_OWNER_SHIFT 8
#define COOP_ENEMY_OWNER_MASK (7u << COOP_ENEMY_OWNER_SHIFT)
#define COOP_ENEMY_COPY 0x800u
// (58) One player's own data (the merged calls above: damage summed over every player, position / flags from the
// player that runs it). pid: a guest: 0 = the host; the host: a guest's id.
bool coop_net_enemy_pos_of(int pid, uint32_t hash, float pos[3], float rot[4], double *age_s);
bool coop_net_enemy_flags_of(int pid, uint32_t hash, uint32_t *flags, double *age_s);
uint32_t coop_net_enemy_damage_of(int pid, uint32_t hash);
// (58) Run-time ids (enemies, drops, ice): OR this in (bit 30: the host's; else the guest's id in bits 26..28) and
// keep the counter below bit 26. coop_net_id_origin: the player who made the id (-1: unknown).
uint32_t coop_net_id_tag(void);
int coop_net_id_origin(uint32_t id);
#define COOP_SPAWN_NAMES 2
#define COOP_SPAWN_NAME_LEN 44
typedef struct { uint32_t id; char name[COOP_SPAWN_NAME_LEN]; } CoopSpawnName;
void coop_net_set_spawn_names(const CoopSpawnName *n, int count);  // sent with the next enemy packets
bool coop_net_remote_spawn_name(uint32_t id, char *out, size_t cap);
// The peer's spawned enemies it reports (COOP_ENEMY_SPAWNED), with how long ago each was last heard of. Returns count.
int coop_net_remote_spawned(CoopEnemyDamage *out, double *heard_age_s, int max);

// (protocol 14) Shared physics objects (shrine puzzles: boxes, blocks, plates, balls, doors), by map HashId. Each
// object has at most one owner, the side whose game moved it last; the owner sends its pose and velocity, the other
// side holds its copy there (runtime/coop.c objects_frame). gen: the claim's generation (a newer claim wins; on a
// tie the host's). Unreliable, newest packet wins, like the enemies.
#define COOP_OBJ_MAX 16
#define COOP_OBJ_OWNED 1u  // the sender owns it now (else: the sender gave it up)
#define COOP_OBJ_CARRIED 2u  // (protocol 30) the sender's player holds it (lifted overhead): the receiver hides its copy
#define COOP_OBJ_STASIS 4u   // (protocol 43) the sender's Stasis holds it: the receiver freezes its copy at that pose
#define COOP_OBJ_RAIL 8u     // (protocol 94) a rail lift's phase from the host (rails.inc): pos = cursor, rot = {start, dest, dir, wait or -1}
// (59) More than two players: every world player's claims go through the host. A guest sends its own objects; the
// host keeps each guest's, and sends every guest one merged entry per object: the winning claim of the host's and
// the other guests' (a newer gen wins, a tie: the lower player id), `pid` naming its owner; an object whose winning
// claim is that guest's own is left out. coop_net_remote_object on the host picks the same way among its guests.
typedef struct {
    uint32_t hash;
    uint16_t gen;
    uint8_t flags;
    uint8_t pid;   // (59, was padding) the owner's player id (< COOP_MAX_PLAYERS); the host overwrites a guest's own
    float pos[3];  // its main rigid body: havok transform translation (body + 0x1a0)
    float rot[4];  // quaternion x y z w (body + 0x1d0)
    float vel[3];  // linear velocity, m/s (from the sender's last two samples)
} CoopObject;
void coop_net_set_objects(const CoopObject *o, int n);  // sent with the next packets (20 Hz)
// The peer's newest entry for that object and its age; false when the peer never sent it.
bool coop_net_remote_object(uint32_t hash, CoopObject *out, double *age_s);
// The peer's objects heard of in the last max_age_s seconds (to learn new claims). Returns count.
int coop_net_remote_objects(CoopObject *out, int max, double max_age_s);
bool coop_object_ok(const CoopObject *o);  // (the receiver's validation; tests)

// (protocol 31) A blast from the sender's game that reaches the other player (runtime/coop/pvp.inc), sent as the
// reliable event COOP_EV_AREAFX: kind 2 = the sender's remote bomb blew up at pos (radius m, damage at the centre).
// (protocol 35) kind 1 = the sender's arrow hit the other player's avatar in the sender's game (runtime/coop/arrows.inc):
// damage = the bow's power, flags = the arrow type (COOP_ARROW_*), pos = where the arrow was, dir = the way it flew
// (radius 0).
// (protocol 32) kind 3 = the sender's thrown weapon hit the other player's avatar in the sender's game: damage = the
// weapon's power, pos = where the weapon was, dir = the way it flew (radius 0).
// (protocol 50) kind 4 = an enemy's arrow hit the other player's avatar in the sender's game (the sender runs that
// enemy): damage = the enemy bow's power, flags = the arrow type, pos / dir as kind 1.
// (protocol 51) kind 5 = an enemy's shot (rock, fireball, ice / shock ball: runtime/coop/shots.inc) hit the other
// player's avatar in the sender's game: damage = the shooter's power, flags = COOP_SHOT_* class (| COOP_SHOT_KNOCK: a
// big rock / bomb that knocks down), pos / dir as kind 1.
#define COOP_EV_AREAFX 9
enum { COOP_AREAFX_ARROW = 1, COOP_AREAFX_BOMB = 2, COOP_AREAFX_THROW = 3, COOP_AREAFX_FOE_ARROW = 4,
       COOP_AREAFX_FOE_SHOT = 5 };
enum { COOP_SHOT_ROCK, COOP_SHOT_FIRE, COOP_SHOT_ICE, COOP_SHOT_SHOCK, COOP_SHOT_BOMB, COOP_SHOT_TYPES };
#define COOP_SHOT_CLASS 0x0f
#define COOP_SHOT_KNOCK 0x10
// (the game's own arrow type order, FUN_710120c970: NormalArrow, BombArrow_A, AncientArrow, FireArrow, IceArrow,
// ElectricArrow)
enum { COOP_ARROW_NORMAL, COOP_ARROW_BOMB, COOP_ARROW_ANCIENT, COOP_ARROW_FIRE, COOP_ARROW_ICE, COOP_ARROW_SHOCK,
       COOP_ARROW_TYPES };
typedef struct {
    uint8_t kind, flags;  // flags: 0 (kind 1: the arrow type, < COOP_ARROW_TYPES)
    int16_t damage;       // quarter hearts, 0..400
    float pos[3];         // finite, |x| <= 100000
    float dir[3];         // finite, |x| <= 1.5 (0 0 0: none)
    float radius;         // 0..20 m
} CoopAreaFx;
bool coop_areafx_ok(const CoopAreaFx *e);  // (the receiver's validation; tests)

// (protocol 32) Dropped items (runtime/coop/drops.inc), reliable events. An item the sender's player dropped from the
// pouch or threw lies at pos in the sender's game (COOP_EV_DROP); the other game makes a copy there. Whoever picks
// up their copy tells the other game (COOP_EV_DROP_GONE), which deletes its own: the item exists once.
#define COOP_EV_DROP 10
#define COOP_EV_DROP_GONE 11
// (protocol 36) A Divine Beast's moving part (Vah Ruta's trunk; runtime/coop/beast.inc): its rotate tag's angle in
// the sender's game, sent while it moves (the sender's player moved it) and again every few seconds after.
#define COOP_EV_BEAST_ROT 12
typedef struct {
    uint32_t hash;  // the rotate tag actor's HashId
    uint32_t area;  // crc of the sender's area (the beast)
    float angle;    // radians (Vah Ruta's trunk: 0 down .. -0.35 up)
} CoopBeastRot;
bool coop_beast_rot_ok(const CoopBeastRot *e);  // (the receiver's validation; tests)
// (protocol 41) A motion-control shrine platform (runtime/coop/gyro.inc): the rotation the sender's game gives a
// DungeonRotateTagGyro* mechanism (the scenery transform its action applies: rotation about the tag's position). Sent by
// the platform's owner - the player at its pedestal (DRIVING, up to 20 times a second while it moves, every 0.5 s while
// still) and, after they let go, once more without DRIVING and every few seconds. gen: the claim's generation (newer
// wins; a tie: the host's).
#define COOP_EV_GYRO 15
#define COOP_GYRO_DRIVING 1u
typedef struct {
    uint32_t hash;   // the DungeonRotateTagGyro* actor's HashId
    uint32_t area;   // crc of the sender's area (the shrine)
    uint16_t gen;
    uint8_t flags, pad;  // COOP_GYRO_* only; pad 0
    float rot[9];    // rows of the 3x3 rotation; finite, each row of length ~1
} CoopGyro;
bool coop_gyro_ok(const CoopGyro *e);  // (the receiver's validation; tests)
// (protocol 43) A Cryonis ice block (Obj_IceMakerBlock; runtime/coop/runes.inc). The game that made it sends CREATE
// with its creation matrix once (and again when the players are together again); the other game makes a copy there.
// When the block or a copy breaks (Cryonis on it, a fourth block, ...) that game sends GONE and the other game breaks
// its own. ids: bit 31 always set, bit 30 given by the host.
#define COOP_EV_ICE 17
#define COOP_ICE_CREATE 1
#define COOP_ICE_GONE 2
#define COOP_ICE_ID 0x80000000u
typedef struct {
    uint32_t id;     // COOP_ICE_ID set
    uint32_t area;   // crc of the sender's area
    uint8_t kind, pad[3];  // COOP_ICE_*; pad 0
    float m[12];     // rows of the 3x4 creation matrix (rotation | translation); finite, rows of length ~1, |t| <= 100000
} CoopIce;
bool coop_ice_ok(const CoopIce *e);  // (the receiver's validation; tests)
// (protocol 54) A tree (or signboard / scaffold: the LumberjackTree AI) cut down in the sender's game
// (runtime/coop/trees.inc): the other game fells the same placed tree (HashId) the game's way.
#define COOP_EV_TREE 18
typedef struct {
    uint32_t hash;   // the tree's map HashId, not 0
    uint32_t area;   // crc of the sender's area, not 0
    float dir[3];    // fall direction (horizontal unit vector, or 0); finite, |x| <= 1.01
    uint8_t instant, pad[3];  // 1: felled at once (bomb); 0/1 only; pad 0
} CoopTree;
bool coop_tree_ok(const CoopTree *e);  // (the receiver's validation; tests)
// Protocol 68: remaining life of a standing tree. A separate TREE event handles the final cut.
#define COOP_EV_TREE_HIT 21
#define COOP_TREE_LIFE_MAX 100000
typedef struct {
    uint32_t hash, area;  // map HashId and area CRC, both nonzero
    int32_t life;        // 1..COOP_TREE_LIFE_MAX
    float from[2];       // sender's X/Z for the direction of the hit effect
} CoopTreeHit;
bool coop_tree_hit_ok(const CoopTreeHit *e);
// (protocol 74, coop-grass2) Partner's arrow in flight (runtime/coop/arrows.inc, SAVAGEWIND_COOP_ARROWFLY): sent when
// our arrow leaves the bow (SHOT: start position, velocity, gravity and arrow kind) and when it stops / hits / vanishes
// (STOP: matching id). Receivers fly a physics-free visual copy (Dm_CoopW_*Arrow) that never deals damage.
#define COOP_EV_ARROW_FLY 22
enum { COOP_ARROW_FLY_SHOT = 1, COOP_ARROW_FLY_STOP = 2 };
typedef struct {
    uint32_t id;       // sender's arrow process id (nonzero)
    uint32_t area;     // sender's area CRC (nonzero)
    uint8_t kind;      // COOP_ARROW_* (< COOP_ARROW_TYPES)
    uint8_t flags;     // COOP_ARROW_FLY_SHOT or COOP_ARROW_FLY_STOP
    uint8_t pad[2];    // 0
    float pos[3];      // start position (SHOT) or stop position (STOP); finite, |x| <= 100000
    float vel[3];      // m/s (SHOT: finite, |v| <= 300, speed > 1; STOP: 0)
    float gravity;     // m/s^2 (SHOT: finite, -50..0; STOP: 0)
} CoopArrowFly;
bool coop_arrow_fly_ok(const CoopArrowFly *e);  // (the receiver's validation; tests)
// (protocol 67) A felled tree's trunk (runtime/coop/trees.inc, SAVAGEWIND_COOP_TRUNKS): the game that moves it last
// ("owner", first the one whose player cut the tree) sends its pose while it moves and once more when it rests; the
// other game holds its own trunk of the same tree there. GONE: it was cut into pieces there.
#define COOP_EV_TRUNK 20
enum { COOP_TRUNK_REST = 1, COOP_TRUNK_GONE = 2 };
typedef struct {
    uint32_t hash;   // the tree's map HashId, not 0
    uint32_t area;   // crc of the sender's area, not 0
    uint16_t gen;    // claim generation, not 0 (a newer one takes the trunk over)
    uint8_t flags, pad;  // COOP_TRUNK_* only; pad 0
    float pos[3];    // the trunk body's position; finite, |x| <= 100000 (not checked when GONE)
    float rot[4];    // its rotation quaternion x y z w; length ~1
    float vel[3];    // m/s, |x| <= 50
} CoopTrunk;
bool coop_trunk_ok(const CoopTrunk *e);  // (the receiver's validation; tests)
// (protocol 56) A placed critter (bird / fish / insect, by map HashId) caught or killed in the sender's game
// (runtime/coop/critters.inc): the other game deletes its own.
#define COOP_EV_CRITTER 19
typedef struct {
    uint32_t hash;   // the critter's map HashId, not 0
    uint32_t area;   // crc of the sender's area, not 0
    float pos[3];    // where it was; finite, |x| <= 100000
} CoopCritter;
bool coop_critter_ok(const CoopCritter *e);  // (the receiver's validation; tests)
// (protocol 75) The sender opened a chest but its game put the item back and closed it again (a full pouch): the
// other games re-close their copy (runtime/coop/gate.inc chest_shut_ev), which CHESTEARLY had locked open.
#define COOP_EV_CHEST_SHUT 23
typedef struct {
    uint32_t hash;   // the chest's map HashId, not 0
    uint32_t area;   // crc of the sender's area, not 0
    float pos[3];    // the chest; finite, |x| <= 100000
    uint32_t flags;  // 0 (reserved)
} CoopChestShut;
bool coop_chest_shut_ok(const CoopChestShut *e);  // (the receiver's validation; tests)
#define COOP_EV_WILD_POLL 24
#define COOP_EV_WILD_ROSTER 25
#define COOP_WILD_MAX 4
typedef struct { uint32_t area, token; } CoopWildReady;  // mammals only, beyond critter range
typedef struct {
    uint32_t area, token, previous, caught[COOP_WILD_MAX];
} CoopWildPoll;
typedef struct {
    uint32_t id, species;
    float pos[3], yaw;  // radians
} CoopWildAnimal;
typedef struct {
    uint32_t area, token;
    float center[3];
    CoopWildAnimal animals[COOP_WILD_MAX];  // nearest first, trailing empty slots
} CoopWildRoster;
const char *coop_wild_name(uint32_t species);
bool coop_wild_poll_ok(const CoopWildPoll *p);
bool coop_wild_roster_ok(const CoopWildRoster *r);
// (protocol 80, coop-fauna) The host's small animals (birds, fish, insects / frogs / lizards) near the guest: an
// unreliable datagram "SWCF", host -> its world partner, about 5 per second while the players are close. Ecosystem
// animals carry the host's process id; placed ones (COOP_FAUNA_PLACED) their map HashId.
#define COOP_FAUNA_MAX 32
#define COOP_FAUNA_R 45.0f       // every animal within this of center (the guest); the game deletes copies made 50+ m away
#define COOP_FAUNA_PLACED 0x01   // id is the map HashId (the guest has its own actor with it)
#define COOP_FAUNA_FLED 0x02     // the host's animal is fleeing (moving fast): the copy keeps up instead of lagging
typedef struct {
    uint32_t id;      // not 0
    uint8_t species;  // coop_fauna_name(species) != NULL
    uint8_t flags;    // COOP_FAUNA_PLACED, COOP_FAUNA_FLED only
    int16_t yaw;      // radians * 32767 / pi
    float pos[3];     // finite, within COOP_FAUNA_R of center
} CoopFaunaAnimal;
typedef struct {
    uint32_t area;    // the host's area crc, not 0
    float center[3];  // the guest's position as the host had it
    uint8_t count, pad[7];  // count <= COOP_FAUNA_MAX animals, the rest zero; pad 0
    CoopFaunaAnimal a[COOP_FAUNA_MAX];
} CoopFauna;
const char *coop_fauna_name(uint32_t species);  // NULL: no such species
int coop_fauna_species(const char *name);        // 0: not shared
bool coop_fauna_ok(const CoopFauna *f);          // (the receiver's validation; tests)
void coop_net_set_fauna(const CoopFauna *f);     // host (game thread): sent once to the world partner
void coop_net_set_fauna_for(int pid, const CoopFauna *f);  // host: guest pid's set (3+ players: one per world guest)
bool coop_net_remote_fauna(CoopFauna *out, double *age_s, uint32_t *seq);  // guest: the host's newest (this session)
#define COOP_EV_FAUNA_GONE 26
#define COOP_FAUNA_CAUGHT 1
#define COOP_FAUNA_KILLED 2
typedef struct {
    uint32_t area, id;  // not 0
    uint8_t species, flags, why, pad;  // species valid, flags COOP_FAUNA_PLACED only, why CAUGHT or KILLED, pad 0
    float pos[3];       // finite, |x| <= 100000
} CoopFaunaGone;
bool coop_fauna_gone_ok(const CoopFaunaGone *e);
// (protocol 82, coop-fire) A fire object (campfire FireWood*, cooking pot Item_CookSet*, firewood bundle, torch stand)
// lit or put out in the sender's game by something other than this event (runtime/coop/fire.inc): the other games set
// their own copy the same way (the game's chemical ignite / extinguish). Placed ones by map HashId, run-time ones
// (hash 0: a lit firewood bundle) by position.
#define COOP_EV_FIRE 27
typedef struct {
    uint32_t hash;   // map HashId, 0: match by position
    uint32_t area;   // crc of the sender's area, not 0
    float pos[3];    // the object; finite, |x| <= 100000
    uint8_t on;      // 1 lit, 0 put out
    uint8_t flags;   // 0 (reserved)
    uint8_t pad[2];  // 0
} CoopFire;
bool coop_fire_ok(const CoopFire *e);  // (the receiver's validation; tests)
// (protocol 83, coop-grassfire) Grass caught fire in the sender's game away from any fire it already had (a new fire,
// not its spread; runtime/coop/grassfire.inc): the other games set their grass there on fire with the game's own grass
// fire cell calls, and their fire spreads by itself.
#define COOP_EV_GRASSFIRE 28
typedef struct {
    uint32_t area;   // crc of the sender's area, not 0
    float pos[3];    // the burning cell's centre on the ground; finite, |x| <= 100000
} CoopGrassFire;
bool coop_grassfire_ok(const CoopGrassFire *e);  // (the receiver's validation; tests)
// (protocol 86, coop-water) Water wheels / capstans (SwitchWheel), dynamic water / lava levels (WaterSurface),
// and timed switches (SwitchTimer, SwitchTimeLimited, SwitchTimeLag) in a shrine (runtime/coop/water.inc).
#define COOP_EV_WATERPUZ 29
enum {
    COOP_WP_WHEEL = 1,    // SwitchWheel: pos[3], rot[4] (unit quat), val[3]=angvel (rad/s)
    COOP_WP_WATER = 2,    // WaterSurface: pos={0,y,0}, rot={0,0,0,1}, val={target_y,speed,0}
    COOP_WP_TIMER = 3,    // SwitchTimer: pos={0,0,0}, rot={0,0,0,1}, val={elapsed,wait_time,0}
    COOP_WP_LIMITED = 4,  // SwitchTimeLimited: pos={0,0,0}, rot={0,0,0,1}, val={elapsed,wait_time,0}
    COOP_WP_TIMELAG = 5,  // SwitchTimeLag: pos={0,0,0}, rot={0,0,0,1}, val={elapsed,wait_time,0}
    COOP_WP_KIND_MAX = COOP_WP_TIMELAG
};
#define COOP_WP_F_ON 1u      // WHEEL: spinning; WATER: raised/signal-on; timers: output on
#define COOP_WP_F_ACTIVE 2u  // WHEEL: actively owned/spinning; WATER: currently moving
#define COOP_WP_FLAGS 3u
typedef struct {
    uint32_t hash;   // map HashId, not 0
    uint32_t area;   // crc of the sender's area, not 0
    uint16_t gen;    // claim generation
    uint8_t kind;    // COOP_WP_WHEEL .. COOP_WP_TIMELAG
    uint8_t flags;   // COOP_WP_F_* only
    float pos[3];    // finite, |x| <= 100000
    float rot[4];    // unit quaternion (x, y, z, w)
    float val[3];    // kind-dependent parameters (angvel / target+speed / elapsed+wait)
} CoopWaterPuz;
bool coop_waterpuz_ok(const CoopWaterPuz *e);  // (the receiver's validation; tests)
// (protocol 90, coop-beams) A Guardian's targeting laser and beam shot in the game that runs the Guardian
// (runtime/coop/beams.inc): the other games draw the laser from their copy's beam cannon to the same player and fly
// the game's own beam actor at that player (the hit is decided in the game of the player it reaches).
#define COOP_EV_BEAM 33
enum { COOP_BEAM_SHOT = 1, COOP_BEAM_AIM = 2, COOP_BEAM_AIM_END = 3 };
#define COOP_BEAM_KINDS 3  // SHOT: 0 GuardianBeam, 1 GuardianMiniBeam, 2 GuardianMiniBeam_Junior; AIM: 0 from the
                           // Guardian's GuardianBeamCannon part, 1 from its body
typedef struct {
    uint32_t hash;    // the Guardian's map HashId (or co-op id of a run-time one); 0: unknown
    uint32_t area;    // crc of the sender's area, not 0
    uint8_t what;     // COOP_BEAM_SHOT / AIM / AIM_END
    uint8_t kind;     // < COOP_BEAM_KINDS (see above)
    int8_t target;    // player id aimed at (< COOP_MAX_PLAYERS), -1: none (aim is an absolute point)
    uint8_t pad;      // 0
    float pos[3];     // SHOT: where the beam was first seen; AIM: the laser origin minus the Guardian body position; finite, |x| <= 100000
    float aim[3];     // target >= 0: offset from that player's body centre (|x| <= 20); else the aimed point
    float speed;      // SHOT: m/s, 1..1000; else 0
} CoopBeam;
bool coop_beam_ok(const CoopBeam *e);  // (the receiver's validation; tests)
// (protocol 95, coop-mapmark) A map marker a player placed or removed (runtime/coop/mapmark.inc): a stamp (No 27..35:
// sword, bow, shield, pot, star, chest, skull, leaf, gem) or a coloured pin (No 37..41, one per colour). The other
// games place / remove the same marker on their map (a pin of that colour moves there). Which ones are sent is the
// sender's Options > Co-op "Shared map markers" (off / pins only / all).
#define COOP_EV_MAPMARK 34
enum { COOP_MAPMARK_ADD = 1, COOP_MAPMARK_DEL = 2 };
typedef struct {
    uint8_t what;    // COOP_MAPMARK_ADD / DEL
    uint8_t no;      // 27..35 (stamp) or 37..41 (pin)
    uint8_t pad[2];  // 0
    float pos[3];    // world position, finite, |x|, |z| <= 10000, |y| <= 10000
} CoopMapMark;
bool coop_mapmark_ok(const CoopMapMark *e);  // (the receiver's validation; tests)
#define COOP_EV_LIGHTNING 30
typedef struct {
    uint64_t at_ns;  // host monotonic clock at impact
    uint32_t area;
    float pos[3];
} CoopLightning;
bool coop_lightning_ok(const CoopLightning *e);
bool coop_net_lightning_recent(uint64_t at_ns);
#define COOP_DROP_ID 0x80000000u  // ids: bit 31 always set; bit 30: given by the host, else bits 26..28 the guest id (58)
#define COOP_DROP_NAME 48
typedef struct {
    uint32_t id;     // COOP_DROP_ID set
    uint32_t area;   // the sender's area (crc, as CoopState.area)
    float pos[3];    // finite, |x| <= 100000
    float yaw;       // degrees, finite, |x| <= 720
    int32_t life;    // weapons: durability (-1: the item's default), -1..10000000
    uint8_t flags, pad[3];  // (34) COOP_DROP_F_* only; pad 0
    char name[COOP_DROP_NAME];  // actor name: Weapon_* / Item_* / Obj_*, letters/digits/_ only, NUL-terminated
} CoopDrop;
// (protocol 34) A drop is sent as soon as it leaves the dropper's Link (dropped from the menu while the menu is still
// open, put down from the hands, thrown): MOVING = it was not at rest yet (the copy falls with its own physics). When
// the original then comes to rest somewhere else, the same id is sent again with UPDATE: the copy is moved there.
enum { COOP_DROP_F_MOVING = 1, COOP_DROP_F_UPDATE = 2 };
enum { COOP_DROP_PICKED = 1, COOP_DROP_VANISHED = 2 };
typedef struct { uint32_t id; uint8_t reason, pad[3]; } CoopDropGone;  // reason COOP_DROP_PICKED / _VANISHED, pad 0
bool coop_drop_ok(const CoopDrop *d);           // (the receiver's validation; tests)
bool coop_drop_gone_ok(const CoopDropGone *g);

// (protocol 33) The sender's remote bombs lying / rolling in its world (runtime/coop/pvp.inc), sent in the state
// packet: the other game shows a model of each where it is (the bomb held overhead is gear COOP_GEAR_CARRIED instead).
#define COOP_BOMBS 2
enum { COOP_BOMB_NONE, COOP_BOMB_ROUND, COOP_BOMB_CUBE, COOP_BOMB_KINDS };
typedef struct {
    uint8_t kind, pad[3];  // COOP_BOMB_*; pad 0
    float pos[3];          // the bomb actor's matrix translation; finite, |x| <= 100000
    float rot[9];          // its rotation (rows of the 3x3 part); finite, |x| <= 1.5
    float vel[3];          // (34) its velocity, m/s (from the sender's last frames); finite, |x| <= 100
} CoopBomb;
void coop_net_set_bombs(const CoopBomb *b, int n);           // ours, sent with the next state packets (n <= COOP_BOMBS)
bool coop_net_remote_bombs(CoopBomb out[COOP_BOMBS], double *age_s);  // the peer's, from its newest state packet
bool coop_bomb_ok(const CoopBomb *b);                        // (the receiver's validation; tests)

// (protocol 52) Enemy shots flying in the sender's game (runtime/coop/shots.inc: shots of enemies it runs): the other
// game, whose copy of the enemy shoots nothing, shows a model of each where it is (moved on by its velocity).
#define COOP_SHOT_VIEWS 3
enum { COOP_SHOTV_NONE, COOP_SHOTV_OCT_ROCK, COOP_SHOTV_OCT_SNOW, COOP_SHOTV_ROCK, COOP_SHOTV_GIANT_ROCK, COOP_SHOTV_KINDS };
typedef struct {
    uint8_t kind, pad[3];  // COOP_SHOTV_*; pad 0 (an empty slot: all 0)
    float pos[3];          // the shot actor's matrix translation; finite, |x| <= 100000
    float vel[3];          // m/s; finite, |x| <= 200
} CoopShotView;
void coop_net_set_shot_views(const CoopShotView *v, int n);  // ours, sent with the next state packets (n <= COOP_SHOT_VIEWS)
bool coop_net_remote_shot_views(CoopShotView out[COOP_SHOT_VIEWS], double *age_s);  // the peer's, newest state packet
bool coop_shot_view_ok(const CoopShotView *v);               // (the receiver's validation; tests)

// (2026-11-14, protocol 39 on branch coop-polish) The sender's horse, in the state packet (runtime/coop/horse.inc):
// the horse it rides (or rode a moment ago and still stands near it). The other game shows a puppet of it
// (Dm_CoopH_<body>, and one each for mane / saddle / reins) playing the same horse AS, and seats the avatar on it.
#define COOP_HORSE_SLOTS 6    // AS slots sent (layer 0 of each; the horse model's AnmTarget has 6 slots)
#define COOP_HORSE_INPUTS 12  // AS input table values sent (float / int kinds, raw 32 bits)
#define COOP_HORSE_AS_COUNT 55  // runtime/coop/horse.inc horse_as_names[] (0 = none)
enum { COOP_HORSE_SHOWN = 1, COOP_HORSE_RIDDEN = 2, COOP_HORSE_REL_SADDLE = 4, COOP_HORSE_HAND_L = 8, COOP_HORSE_HAND_R = 16,
       COOP_HORSE_FLAGS = 31 };  // (44) HAND_L/R: the rider's left / right hand holds the reins
typedef struct {
    uint8_t flags, n_in;                // COOP_HORSE_* (0: no horse, all else 0); n_in <= COOP_HORSE_INPUTS
    uint8_t coat, socks;                // (45) coat: the coat frame + 1 (0 = unknown); socks: bits 0-4 the socks frame + 1
                                        // (0 = unknown), bits 5-7 the colour AS kind (COOP_HORSE_COAT_*)
    char body[24];                      // the real horse actor (GameRomHorse01...), actor-name alphabet, terminated
    char mane[32], saddle[24], reins[24];  // its accessories ("" = none), same rules
    float mtx[12];                      // its matrix (3x4 rows); finite, rotation |x| <= 1.5, position |x| <= 100000
    float vel[3];                       // its AS velocity (actor +0x400); finite, |x| <= 20
    float rel[12];                      // the rider relative to the horse's Saddle_Root bone (REL_SADDLE) or to the
                                        // horse matrix; finite, rotation |x| <= 1.5, position |x| <= 10
    uint8_t as[COOP_HORSE_SLOTS];       // AS per slot (layer 0), < COOP_HORSE_AS_COUNT
    uint8_t seq[COOP_HORSE_SLOTS];      // restart counters
    uint8_t in_kind[COOP_HORSE_INPUTS]; // AS input kinds (< 66, float or int kinds only)
    uint32_t in_val[COOP_HORSE_INPUTS]; // their raw values (a float kind: finite, |x| <= 100000)
    uint64_t bools;                     // AS input kind 66 (bit set)
} CoopHorse;
// (45) The colour AS the real horse plays (its nature): Collar_Shader, _Coward, _Impatient, _EponaHorse, _ZeldaHorse.
enum { COOP_HORSE_COAT_KINDS = 5 };
void coop_net_set_horse(const CoopHorse *h);                   // ours, sent with the next state packets (NULL: none)
bool coop_net_remote_horse(CoopHorse *out, double *age_s);     // the peer's, from its newest state packet
bool coop_horse_ok(const CoopHorse *h);                        // (the receiver's validation; tests)

// (protocol 48) Ragdoll pose: while the sender's Link tumbles (knocked down: Havok ragdoll), its skeleton pose in a
// datagram of its own ("SWCR", runtime/coop/ragdoll.inc), 20 times a second, plus a short tail with flags 0 (the
// receiver blends out). Bone i = ragdoll.inc rag_bones[i]. Frame F = the sender's sent position + yaw (the state
// packet's pos / yaw, as write_mtx builds it): each bone's world rotation is F's rotation times q[i], the root bone
// (index 0, Skl_Root) sits at F * root.
#define COOP_POSE_BONES 22
#define COOP_POSE_ACTIVE 1u   // flags: the sender's player is in ragdoll now (0: ended, blend out)
typedef struct {
    uint8_t flags;                       // COOP_POSE_*
    uint8_t count;                       // = COOP_POSE_BONES
    uint16_t pad;                        // 0
    uint32_t mask;                       // bit i: bone i was read (others: q all 0); bits >= COOP_POSE_BONES 0
    float root[3];                       // Skl_Root in frame F, m; finite, |x| <= 20
    int16_t q[COOP_POSE_BONES][4];       // x, y, z, w * 32767 relative to F; a masked one has length 0.8..1.2
    uint32_t pad2;                       // 0 (keeps the packet a multiple of 8)
} CoopPose;
void coop_net_set_pose(const CoopPose *p);                     // ours, sent from now on (NULL: stop sending)
bool coop_net_remote_pose(CoopPose *out, double *age_s);       // the peer's newest (false: none this peer session)
bool coop_pose_ok(const CoopPose *p);                          // (the receiver's validation; tests)

// (protocol 12) Reliable events (items, chests, flags): delivered once, in order, while both sessions live.
// Resent every packet until the peer acknowledges them.
#define COOP_EVENT_DATA 116
// Protocol 99: addressed shrine events.
#define COOP_EV_SHRINE_RIDE 37
#define COOP_EV_SHRINE_DOOR 38
#define COOP_SHRINE_MAX 136
// (0.1.1) A door with this number: "my pedestal scene started here" (no number known yet). Older builds reject it.
#define COOP_SHRINE_DOOR_SCENE 0xffffu
typedef struct {
    uint32_t hash;
    float pos[3];
} CoopShrineRide;
typedef struct {
    uint32_t dungeon;
    float pos[3];
} CoopShrineDoor;
bool coop_shrine_ride_ok(const CoopShrineRide *ride);
bool coop_shrine_door_ok(const CoopShrineDoor *door);

// Protocol 97 (was 95; events renumbered at merge: MAPMARK is 34). A cooked meal and the result of giving it to the world partner.
#define COOP_EV_DISH 35
#define COOP_EV_DISH_RESULT 36
#define COOP_DISH_MAX 60
#define COOP_DISH_INGREDIENTS 5
typedef struct {
    char name[24];
    int32_t hearts, seconds, price, effect;
    float level;
    uint32_t ingredient[COOP_DISH_INGREDIENTS];  // ActorInfo name hashes, 0 for empty slots
} CoopDish;
// (DISHINV, more than two players) A dish goes to one player only (the nearest one within 10 m): sent with
// coop_net_send_event_to(to_pid) (guest -> guest through the host); never broadcast (coop_net_send_event refuses it).
// Only player to_pid takes it: every receiver checks to_pid / to against itself, from_pid against the sender.
typedef struct {
    uint64_t from, to, id;   // giver's and receiver's session (to 0: a guest giving to another guest, the host fills
                             // it in when it passes the offer on); id: the giver's transfer id, grows
    uint32_t area;
    uint8_t from_pid, to_pid, pad[2];  // giver's and receiver's player ids (< COOP_MAX_PLAYERS, different); pad 0
    CoopDish dish;
} CoopDishOffer;
typedef struct {
    uint64_t from, to, id;   // the receiver's (answering) and the giver's session, the offer's id
    uint32_t accepted;
    uint8_t from_pid, to_pid, pad[2];  // the answering player's id and the giver's
} CoopDishResult;
bool coop_dish_offer_ok(const CoopDishOffer *offer);
bool coop_dish_result_ok(const CoopDishResult *result);
// Our session and player pid's as we know it (*theirs 0: a guest does not know another guest's: the host fills it in).
// false: no world link to that player (not connected, or an extra guest whose world is its own).
bool coop_net_dish_session(int pid, uint64_t *ours, uint64_t *theirs);
bool coop_net_send_event(uint16_t type, const void *data, int len);  // false: queue full or no peer yet
bool coop_net_next_event(uint16_t *type, void *data, int *len);      // the next received event (data: COOP_EVENT_DATA)
// (58) The same, and the player it comes from (a guest: 0 = the host, else the guest the host relays it from).
// Events the host relays: all but the pairwise ones (a hit on the other player's avatar; objects only without
// SAVAGEWIND_COOP_MULTIWORLD, 59).
bool coop_net_next_event_from(uint16_t *type, void *data, int *len, int *origin);
// (60) The same as coop_net_send_event, for one player only (pid: 0 = the host, else a guest): a hit on that player's
// avatar. A guest's goes through the host, which passes it on (origin = the sender) and does not apply it itself.
bool coop_net_send_event_to(int pid, uint16_t type, const void *data, int len);
// (60) Host: player state / AS tree / ragdoll pose relayed to a guest far from their sender (another area, or farther
// than COOP_RELAY_FAR_M) at a lower rate (state 10 a second, tree and pose 4). SAVAGEWIND_COOP_RELAYRATE=0: always all.
#define COOP_RELAY_FAR_M 200.0f
