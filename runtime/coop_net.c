// Co-op networking (experimental): one UDP link between two game instances, each sending its player's state and
// receiving the other's. Off unless SAVAGEWIND_COOP_PORT or SAVAGEWIND_COOP_PEER is set.
//
//   SAVAGEWIND_COOP_PORT=<port>      UDP port to listen on (default 47000 when only the peer is given: any port)
//   SAVAGEWIND_COOP_PEER=<ip>:<port> where the other instance listens. Without it this side waits and answers whoever
//                                    sends to it first (the host: only the guest needs the host's address, e.g. its
//                                    Tailscale IP).
//   SAVAGEWIND_COOP_SIM=<delay ms>[,<jitter ms>[,<loss %>]]   (tests only) a bad network on the receiving side:
//                                    each packet is lost with that chance, else held for delay +- jitter ms (so
//                                    some arrive out of order), e.g. 150,50,10. Set it on both sides for both ways.
//
// A thread sends the local snapshot (written by the game thread, coop_net_set_local) 20 times a second and keeps the
// newest valid packet from the peer (coop_net_get_remote). Packets carry a sequence number; older ones are dropped.
// Nothing here touches guest memory.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET CoopSocket;
#define COOP_BAD_SOCKET INVALID_SOCKET
#define coop_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int CoopSocket;
#define COOP_BAD_SOCKET (-1)
#define coop_close close
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "coop_net.h"
#include "coop_upnp.h"
#include "rt.h"

#define COOP_MAGIC 0x4f435753u  // "SWCO"
#define COOP_VERSION 100 // 4: session identity and reciprocal acknowledgement; 5: enemy positions; 6: enemy AS; 7: body pos+rot;
                        // 8: sword attack anims; 9: enemy ownership flags; 10: hour of day;
                        // 11: weapon name in the state packet; 12: run-time enemies (names), reliable events;
                        // 13: world events carry their area; 14: shared physics objects (branch coop) / the avatar's
                        // gear, armor and more anim states (branch coop-avatar); 15: both merged;
                        // 16: floor switch events (EV_SWITCH 5: pressed/released in the sender's game); 17: hit switch events
                        // (EV_SWITCH_HIT 6); 18: shared cutscenes (EV_STORY 7);
                        // 19: animation overhaul (action-instance counter anim_seq, expanded combat combos/parry/dodge/death anims);
                        // 20: weapon throw aim/throw and object carry/throw anims, micro tap-walk sliding fix;
                        // 21: visible carried props (remote bombs, barrels, pots, rocks carried overhead in COOP_GEAR_CARRIED);
                        // 22: multi-layer skeletal slot blending (anim_upper in slot 1 concurrently with locomotion);
                        // 23: movement anim states (shield surf, ladder, slide, tumble, wall jump, push, tired, brake);
                        // 24: item and Sheikah Slate anim states, rune/item upper-body states;
                        // 25: face AS index (face) in the state packet's tail padding;
                        // 26: more anim states (talk, doors, mount, sand seal, warp, frozen, knocked down, swim/climb extras);
                        // 27: idle anim states (random fidgets, hot/cold, lock-on stance, hurt);
                        // 28: more anim states (jump / horseback attacks, horse extras, put down, climb extras, selfie, turns);
                        // 29: tree mode: AS input table + AS per slot/layer in a fifth datagram kind ("SWCA", CoopAsTree)
                        // 30: carried shared objects (COOP_OBJ_CARRIED flag) + EV_OBJ 8 (release pose/velocity, broken)
                        // 31: friendly fire / area effects: EV 9 COOP_EV_AREAFX (a remote bomb's blast)
                        // 32: dropped / thrown items shared (EV 10 COOP_EV_DROP, EV 11 COOP_EV_DROP_GONE), thrown
                        //     weapon hits (COOP_EV_AREAFX kind 3)
                        // 33: the sender's lying / rolling remote bombs in the state packet (CoopBomb bombs[2], 712 B)
                        // 34: CoopBomb + vel[3] (the receiver extrapolates; 64 B each, state packet 736 B); CoopDrop +
                        //     flags/pad (80 B): a drop sent before it came to rest (menu drop, in the air) and its update
                        // 35: arrow hits on the other player (COOP_EV_AREAFX kind 1, flags = arrow type); same sizes
                        // 36: Divine Beast moving parts (EV 12 COOP_EV_BEAST_ROT: a rotate tag's angle, 12 B)
                        // 37: waiting for the partner before a shared cutscene (EV 13 EV_STORY_ACK, 100 B)
                        // 38: (castle branch)
                        // 39: the player's effect-set (ELink) properties in the tree packet (CoopAsTree 256 B, SWCA 288 B);
                        //     (coop-polish, same number: unmerged) the rider's horse in the state packet (CoopHorse
                        //     296 B, state packet 1032 B)
                        // 40: (reserved for a parallel branch)
                        // 41: motion-control shrine platforms (EV 15 COOP_EV_GYRO: a DungeonRotateTagGyro's rotation, 48 B)
                        // 42: (skipped: numbered while three branches were built in parallel)
                        // 43: runes: COOP_OBJ_STASIS (a shared object frozen by the sender's Stasis) and Cryonis ice
                        //     blocks (EV 17 COOP_EV_ICE: create with matrix / gone, 60 B)
                        // 44: horse flags COOP_HORSE_HAND_L / HAND_R (which hands hold the reins; same layout)
                        // 45: CoopHorse pad -> coat (coat frame + 1) / socks (socks frame + 1, colour AS kind << 5);
                        //     state packet pad_tail -> aim_pitch / aim_yaw / aim_flags (upper-body aim)
                        // 47: CoopAsTree pad -> charge (spin-attack charge level 0..2, for the ChargeLvUp sparks; same size)
                        // 46: CoopAsTree pad -> horse_aim (the horseback bow body-twist clip's frame * 64 + 1; same size)
                        // 48: ragdoll pose: a sixth datagram kind ("SWCR", CoopPose 200 B, packet 232 B) while the
                        //     sender's Link is knocked down (Havok ragdoll)
                        // 49: gear slot COOP_GEAR_SLATE (the Sheikah Slate) + held[5] (items held in the hands from the
                        //     inventory, relative to the CarryBox): state packet 1032 -> 1344 B
                        // 50: COOP_EV_AREAFX kind 4 = an enemy's arrow (run in the sender's game) hit the other
                        //     player's avatar (flags = arrow type); state flag COOP_FLAG_HURT; same sizes
                        // 51: COOP_EV_AREAFX kind 5 = an enemy's shot (rock / fireball / ice / shock ball) hit the
                        //     other player's avatar (flags = COOP_SHOT_* class | COOP_SHOT_KNOCK); same sizes
                        // 52: enemy shots flying in the sender's game, shown by the other game (CoopShotView shots[3]
                        //     in the state packet: 1344 -> 1424 B)
                        // 53: the join code: every datagram ends with an 8-byte SipHash tag keyed by SAVAGEWIND_COOP_CODE
                        //     (all sizes + 8 on the wire; a wrong tag is dropped before parsing)
                        // 54: trees cut down (event 18 COOP_EV_TREE, 24 B: HashId, area, fall direction, instant)
                        // 55: enemy entry flags COOP_ENEMY_LOOK / _LOOK_PARTNER / _LOOK_BATTLE (whom its head looks at)
                        // 56: enemy entry flag COOP_ENEMY_MTXPOS (animal without a body: matrix position); event 19
                        //     COOP_EV_CRITTER (a placed bird / fish / insect caught or killed)
                        // 57: (coop-multi) more than two players: state packet + origin / your_id / role / players
                        //     (+4 pad: 1424 -> 1432 B); SWCA and SWCR reserved8 -> origin; the host relays guests'
                        //     state / tree / pose
                        // 58: (coop-n2) the world shared with every player: role bit COOP_ROLE_WORLD; the host keeps
                        //     enemies and events per guest and sends each guest a merged enemy packet (the others'
                        //     damage summed, the runner's position relayed); CoopEvent.type's high byte = the origin
                        //     player of a relayed event; run-time ids (enemies, drops, ice) carry the guest's id
                        // 59: (coop-n3) physics objects shared with every world player: CoopObject.pad -> pid (the
                        //     owner); the host sends each guest a merged object view (up to two SWCB datagrams) and
                        //     relays EV_OBJ (release / broken) to every world guest with its origin
                        // 60: (coop-n4) events aimed at one player (a hit on that player's avatar): a guest's event
                        //     whose CoopEvent.type high byte names another player is passed by the host to that player
                        //     only (with the sender as origin), never applied by the host; same datagram sizes
                        // 61: (coop-n6) enemy entry flags COOP_ENEMY_LOOK_PID + player id (bits 12..15): the player an
                        //     enemy looks at, by id, the same for every receiver (the host relays it unchanged); and
                        //     the host's per-guest enemy packet lists what that guest has loaded first; same sizes
                        // 62: (coop-menu) player names + ping: a seventh datagram kind "SWCN" (CoopNamePacket, 272 B)
                        //     twice a second; the co-op menu starts the network on demand (no wire change)
                        // 63: guest state carries a short-lived return-address proof; SWCV sends the challenge
                        // 64: the state carries the sender's Magnesis beam contact point
                        // 65: (coop-looks) the name entry's flags byte is that player's tunic colour (COOP_TINT_*,
                        //     < COOP_TINT_COUNT); same sizes
                        // 66: (coop-stasis) the state carries the sender's Stasis lock and strike arrow (1456 -> 1464)
                        // 67: (coop-sounds; 66 is stasis) reliable event COOP_EV_TRUNK (felled tree trunks lie the same way in both games)
                        // 68: reliable event COOP_EV_TREE_HIT (remaining life of a standing tree)
                        // 69: host weather, climate and blood moon counter (also the enemy damage generation)
                        // 72: (coop-avlook) gear bone COOP_BONE_ARM_R (a held two-handed sword / hammer rides the
                        //     forearm: the player's right wrist is not where its animation puts it); same sizes
                        // 73: host bloody-day state replaces the climate byte (state packet still 1464 bytes)
                        // 74: (coop-grass2) reliable event COOP_EV_ARROW_FLY (partner's arrows visible in flight); same sizes
                        // 75: (coop-objects2) reliable event COOP_EV_CHEST_SHUT 23 (a chest closed again with its item
                        //     refused); a chest's items are shared as soon as they reach the pouch; same sizes
                        // 76: ecosystem catchables, guest catch IDs and host roster in reliable events
                        // 80: (coop-fauna) host fauna snapshot datagram "SWCF" (birds, fish, insects near the guest)
                        //     and reliable event COOP_EV_FAUNA_GONE 26 (one caught / killed)
                        // 81: (coop-eat) the tree carries what the sender eats / drinks from the menu (eat, eat_seq; was fx_pad)
                        // 82: (coop-fire) reliable event COOP_EV_FIRE 27 (a campfire / cooking pot / torch stand
                        //     lit or put out) and state flag COOP_FLAG_BURNING (the weapon in hand burns); same sizes
                        // 83: (coop-grassfire) reliable event COOP_EV_GRASSFIRE 28 (grass set on fire there); same sizes
                        // 84: enemy snapshots carry elemental status flags; same sizes
                        // 85: enemy Stasis remaining time in the Stasis snapshot's spare byte
                        // 86: (coop-water) reliable event COOP_EV_WATERPUZ 29 (shrine water wheels / capstans,
                        //     dynamic water / lava levels, and timed switches: SwitchTimer / SwitchTimeLimited /
                        //     SwitchTimeLag); same datagram sizes
                        // 89: (coop-champ) CoopAsTree eat_pad -> champ (Champion abilities: Daruk's shield shown,
                        //     Urbosa / Mipha / Revali / shield-break use counters); same datagram sizes
                        // 90: (coop-beams) reliable event COOP_EV_BEAM 33 (a Guardian's targeting laser and beam
                        //     shot, drawn / flown in the other games); same datagram sizes
                        // 92: (coop-horse2) CoopHorse may carry a sand seal / Sidon / Master Cycle / wild deer or
                        //     bear (kind from the body name, AS index into that kind's table); same layout and sizes
                        // 93: (coop-storm, was 88) host-only COOP_EV_LIGHTNING 30 (native strike position and impact time)
                        // 94: COOP_OBJ_RAIL 8: the host's shrine rail lifts (their phase) in the object list; same sizes
                        // 95: state flags for Link's friendly elemental arrow reactions; same datagram sizes
                        // 96: (coop-mapmark, was 95) reliable event COOP_EV_MAPMARK 34 (a map stamp / pin placed or
                        //     removed, shared by the placer's "Shared map markers" setting); same datagram sizes
                        // 97: (coop-dishgive, was 95) COOP_EV_DISH 35 / DISH_RESULT 36 (cooked dish transfers, receipts); same datagram sizes
                        //     and (DISHINV): player names are UTF-8 (name packet check); a dish offer /
                        //     result names giver and receiver ids (old pad bytes) and goes to one player only
                        //     (guest -> guest through the host); same datagram and event sizes
                        // 98: COOP_FLAG_BLUE_FIRE qualifies COOP_FLAG_BURNING; same datagram and event sizes
                        // 99: addressed shrine door progress and native elevator rides for partners already aboard
                        // 100: (public 0.1.0, security review 3) the room-code key is stretched (2^20 SipHash rounds): a
                        //      captured datagram no longer lets someone guess the 8-letter code offline in hours;
                        //      same datagram sizes (a 99 peer just looks like a wrong code)
#define SEND_INTERVAL_MS 50

typedef struct {
    uint32_t magic;
    uint8_t version, flags;
    uint16_t anim;
    uint32_t seq;
    float pos[3], yaw, speed;
    uint32_t area;
    uint8_t anim_seq;          // (19) action-instance counter (increments on every action/attack start)
    uint8_t anim_upper_seq;    // (22) upper-body action counter
    uint16_t anim_upper;       // (22) upper-body action state (COOP_ANIM_UPPER_*)
    uint64_t session, echo;
    char weapon[32];  // (11) NUL-terminated actor name of the held weapon, letters/digits/_ only, or ""
    CoopGear gear[COOP_GEAR_COUNT];    // (14) same name rules; bone < COOP_BONE_COUNT; rel finite, |x| < 100
    char armor[COOP_ARMOR_SLOTS][32];  // (14) same name rules
    uint8_t face;                      // (25) face AS index, < COOP_FACE_COUNT
    int8_t aim_pitch, aim_yaw;         // (45) upper-body aim (CoopState): pitch -90..90, yaw from the body
    uint8_t aim_flags;                 // (45) COOP_AIM_*
    CoopBomb bombs[COOP_BOMBS];        // (33) coop_bomb_ok each
    CoopHorse horse;                   // (39, coop-polish) coop_horse_ok
    CoopHeldItem held[COOP_HELD_ITEMS];  // (49) held_ok each
    CoopShotView shots[COOP_SHOT_VIEWS];  // (52) coop_shot_view_ok each
    // (56, in what was the tail padding) origin: the id of the player this state describes (a guest's state relayed
    // by the host carries the guest's id); your_id / role / players: from the host only (origin 0): the receiver's
    // id, COOP_ROLE_PARTNER when the receiver is the host's world partner, bit mask of the ids connected now.
    uint8_t origin, your_id, role, players;
    uint8_t weather;  // (69) 0 unknown, otherwise game index + 1
    uint8_t moon_day;  // (73) COOP_MOON_DAY_*
    uint16_t bloodmoon;  // (69) host's running count of blood moon cutscenes
    uint64_t challenge;  // (63) guest's proof for its current endpoint, never relayed
    float magbeam_tip[3];  // (64) world-space contact on the object when COOP_FLAG_MAGBEAM is set
    CoopStasisFx stasisfx;  // (66) replaces pad64 and keeps the tagged packet within 1472 bytes
} CoopPacket;  // little-endian, same ABI on supported 64-bit hosts
#define COOP_ROLE_PARTNER 1u
#define COOP_ROLE_WORLD 2u  // (58) the host shares the world with every guest (SAVAGEWIND_COOP_MULTIWORLD, default on)
_Static_assert(sizeof(CoopGear) == 84, "co-op gear layout");
_Static_assert(sizeof(CoopBomb) == 64, "co-op bomb layout");
_Static_assert(sizeof(CoopHorse) == 296, "co-op horse layout");
_Static_assert(sizeof(CoopHeldItem) == 44, "co-op held item layout");
_Static_assert(sizeof(CoopShotView) == 28, "co-op shot view layout");
_Static_assert(sizeof(CoopStasisFx) == 12 && sizeof(CoopPacket) == 1464, "co-op state wire layout (66)");

static bool name_ok_n(const char *w, int cap) {
    for (int i = 0; i < cap; i++) {
        char ch = w[i];
        if (!ch) return true;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return false;
}
static bool mtx_ok(const float m[12], float pos_max) {
    for (int i = 0; i < 12; i++) {
        float lim = (i & 3) == 3 ? pos_max : 1.5f;
        if (!isfinite(m[i]) || fabsf(m[i]) > lim) return false;
    }
    return true;
}
bool coop_horse_ok(const CoopHorse *h) {
    if (!h->flags) {  // (no horse: everything zero)
        static const CoopHorse zero;
        return !memcmp(h, &zero, sizeof zero);
    }
    if ((h->flags & ~COOP_HORSE_FLAGS) || !(h->flags & COOP_HORSE_SHOWN) || h->n_in > COOP_HORSE_INPUTS) return false;
    if ((h->socks >> 5) >= COOP_HORSE_COAT_KINDS) return false;
    if (!h->body[0] || !name_ok_n(h->body, sizeof h->body) || !name_ok_n(h->mane, sizeof h->mane) ||
        !name_ok_n(h->saddle, sizeof h->saddle) || !name_ok_n(h->reins, sizeof h->reins))
        return false;
    if (!mtx_ok(h->mtx, 100000) || !mtx_ok(h->rel, 10)) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(h->vel[i]) || fabsf(h->vel[i]) > 20) return false;
    for (int i = 0; i < COOP_HORSE_SLOTS; i++)
        if (h->as[i] >= COOP_HORSE_AS_COUNT) return false;
    for (int i = 0; i < COOP_HORSE_INPUTS; i++) {
        int k = h->in_kind[i];
        if (i >= h->n_in) {
            if (k || h->in_val[i]) return false;
            continue;
        }
        if (k >= 66 || k == 21 || (k >= 44 && k <= 62) || k == 64) return false;  // (float / int kinds only)
        if (k < 30) {
            float f;
            memcpy(&f, &h->in_val[i], 4);
            if (!isfinite(f) || fabsf(f) > 100000) return false;
        }
    }
    return true;
}

bool coop_shot_view_ok(const CoopShotView *v) {
    if (v->kind >= COOP_SHOTV_KINDS || v->pad[0] || v->pad[1] || v->pad[2]) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(v->pos[i]) || fabsf(v->pos[i]) > 100000 || !isfinite(v->vel[i]) || fabsf(v->vel[i]) > 200)
            return false;
    return true;
}

bool coop_bomb_ok(const CoopBomb *b) {
    if (b->kind >= COOP_BOMB_KINDS || b->pad[0] || b->pad[1] || b->pad[2]) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(b->pos[i]) || fabsf(b->pos[i]) > 100000 || !isfinite(b->vel[i]) || fabsf(b->vel[i]) > 100)
            return false;
    for (int i = 0; i < 9; i++)
        if (!isfinite(b->rot[i]) || fabsf(b->rot[i]) > 1.5f) return false;
    return true;
}

// (The receiver builds a file name from it: only an actor-name alphabet, terminated within the field.)
static bool weapon_name_ok(const char *w) {
    for (int i = 0; i < 32; i++) {
        char ch = w[i];
        if (!ch) return true;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return false;
}

static bool gear_ok(const CoopGear *g) {
    if (!weapon_name_ok(g->name) || g->bone >= COOP_BONE_COUNT) return false;
    for (int i = 0; i < 12; i++)
        if (!isfinite(g->rel[i]) || fabsf(g->rel[i]) > 100) return false;
    return true;
}
static bool held_ok(const CoopHeldItem *h) {
    if (!weapon_name_ok(h->name)) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(h->pos[i]) || fabsf(h->pos[i]) > 5) return false;
    return true;
}

// The shared enemies' damage totals and positions: a second datagram kind, same session rules as the state packet.
#define COOP_ENEMY_MAGIC 0x45435753u  // "SWCE"
typedef struct {
    uint32_t magic;
    uint8_t version, count;
    uint16_t generation;
    uint32_t seq;
    float hour;  // (protocol 10) the sender's hour of day 0..24, negative: unknown (was reserved)
    uint64_t session, echo;
    CoopEnemyDamage e[COOP_ENEMY_MAX];
    CoopSpawnName names[COOP_SPAWN_NAMES];  // (12) actor names of run-time enemies (id 0: unused)
} CoopEnemyPacket;
_Static_assert(sizeof(CoopEnemyPacket) == 32 + 72 * COOP_ENEMY_MAX + 48 * COOP_SPAWN_NAMES, "co-op enemy wire layout");
#define REMOTE_ENEMIES 256  // (full: the longest-unheard entry is replaced)

// Reliable events: a third datagram kind. Each packet carries the oldest unacknowledged events (resent every
// packet until acknowledged) and the id of the last event received in order from the peer.
#define COOP_EVENT_MAGIC 0x56435753u  // "SWCV"
#define EVENTS_PER_PACKET 8
typedef struct { uint32_t id; uint16_t type, len; uint8_t data[COOP_EVENT_DATA]; } CoopEvent;
typedef struct {
    uint32_t magic;
    uint8_t version, count;
    uint16_t reserved;
    uint32_t ack;  // the last event id received in order from the peer (0: none)
    uint32_t pad;
    uint64_t session, echo;
    CoopEvent e[EVENTS_PER_PACKET];
} CoopEventPacket;
_Static_assert(sizeof(CoopEvent) == 124 && sizeof(CoopEventPacket) == 32 + 124 * EVENTS_PER_PACKET, "co-op event wire layout");
// Shared physics objects (protocol 14): a fourth datagram kind, same session rules.
#define COOP_OBJ_MAGIC 0x42435753u  // "SWCB"
typedef struct {
    uint32_t magic;
    uint8_t version, count;
    uint16_t reserved;
    uint32_t seq;
    uint32_t pad;
    uint64_t session, echo;
    CoopObject o[COOP_OBJ_MAX];
} CoopObjPacket;
_Static_assert(sizeof(CoopObject) == 48 && sizeof(CoopObjPacket) == 32 + 48 * COOP_OBJ_MAX, "co-op object wire layout");
#define REMOTE_OBJECTS 256
// (protocol 29) Tree mode: a fifth datagram kind, same session rules; newest seq wins.
#define COOP_ASTREE_MAGIC 0x41435753u  // "SWCA"
typedef struct {
    uint32_t magic;
    uint8_t version, origin;  // (56) origin: the player id it describes (was reserved8)
    uint16_t reserved;
    uint32_t seq;
    uint32_t pad;
    uint64_t session, echo;
    CoopAsTree t;
} CoopAsTreePacket;
_Static_assert(sizeof(CoopAsTree) == 256 && sizeof(CoopAsTreePacket) == 32 + 256, "co-op AS tree wire layout");
_Static_assert(sizeof(CoopAsTreePacket) != sizeof(CoopObjPacket) && sizeof(CoopAsTreePacket) != sizeof(CoopEventPacket) &&
               sizeof(CoopAsTreePacket) != sizeof(CoopEnemyPacket) && sizeof(CoopAsTreePacket) != sizeof(CoopPacket), "distinct sizes");
_Static_assert(sizeof(CoopPacket) != sizeof(CoopObjPacket) && sizeof(CoopPacket) != sizeof(CoopEventPacket) &&
               sizeof(CoopPacket) != sizeof(CoopEnemyPacket), "distinct sizes (protocol 33 state packet)");
// (protocol 48) Ragdoll pose: a sixth datagram kind, same session rules; newest seq wins.
#define COOP_POSE_MAGIC 0x52435753u  // "SWCR"
typedef struct {
    uint32_t magic;
    uint8_t version, origin;  // (56) origin: the player id it describes (was reserved8)
    uint16_t reserved;
    uint32_t seq;
    uint32_t pad;
    uint64_t session, echo;
    CoopPose pose;
} CoopPosePacket;
_Static_assert(sizeof(CoopPose) == 200 && sizeof(CoopPosePacket) == 32 + 200, "co-op ragdoll pose wire layout");
_Static_assert(sizeof(CoopPosePacket) != sizeof(CoopObjPacket) && sizeof(CoopPosePacket) != sizeof(CoopEventPacket) &&
               sizeof(CoopPosePacket) != sizeof(CoopEnemyPacket) && sizeof(CoopPosePacket) != sizeof(CoopPacket) &&
               sizeof(CoopPosePacket) != sizeof(CoopAsTreePacket), "distinct sizes (protocol 48 pose packet)");
// (protocol 62) Player names and ping: a seventh datagram kind, same session rules (coop_net.h CoopLobbyEntry).
#define COOP_NAME_MAGIC 0x4e435753u  // "SWCN"
typedef struct {
    uint8_t pid;        // the player this entry names (< COOP_MAX_PLAYERS; a guest's own entry: its id, or 0xff unknown)
    uint8_t tint;       // (65) that player's tunic colour, COOP_TINT_* (< COOP_TINT_COUNT)
    uint16_t ping_ms;   // from the host: that player's round trip to it (0xffff unknown)
    char name[COOP_NAME_LEN];  // NUL-terminated UTF-8 (coop_name_ok)
} CoopNameEntry;
typedef struct {
    uint32_t magic;
    uint8_t version, count, pad[2];  // count <= COOP_MAX_PLAYERS entries
    uint64_t session, echo;
    uint64_t t_ns;          // the sender's clock as it sent this
    uint64_t echo_t_ns;     // the receiver's t_ns heard last (0: none)
    uint32_t echo_hold_us;  // how long the sender held that one before this send
    uint32_t pad2;
    CoopNameEntry e[COOP_MAX_PLAYERS];
} CoopNamePacket;
_Static_assert(sizeof(CoopNameEntry) == 28 && sizeof(CoopNamePacket) == 48 + 28 * COOP_MAX_PLAYERS, "co-op name wire layout");
_Static_assert(sizeof(CoopNamePacket) != sizeof(CoopObjPacket) && sizeof(CoopNamePacket) != sizeof(CoopEventPacket) &&
               sizeof(CoopNamePacket) != sizeof(CoopEnemyPacket) && sizeof(CoopNamePacket) != sizeof(CoopPacket) &&
               sizeof(CoopNamePacket) != sizeof(CoopAsTreePacket) && sizeof(CoopNamePacket) != sizeof(CoopPosePacket),
               "distinct sizes (protocol 62 name packet)");
#define COOP_CHALLENGE_MAGIC 0x56435753u  // "SWCV"
typedef struct {
    uint32_t magic;
    uint8_t version, pad[3];
    uint64_t session, cookie;
} CoopChallenge;
_Static_assert(sizeof(CoopChallenge) == 24 && sizeof(CoopChallenge) < sizeof(CoopPacket) &&
               sizeof(CoopChallenge) < sizeof(CoopEnemyPacket) && sizeof(CoopChallenge) < sizeof(CoopObjPacket) &&
               sizeof(CoopChallenge) < sizeof(CoopEventPacket) && sizeof(CoopChallenge) < sizeof(CoopNamePacket) &&
               sizeof(CoopChallenge) < sizeof(CoopPosePacket) && sizeof(CoopChallenge) < sizeof(CoopAsTreePacket),
               "challenge is distinct and smaller than a state datagram");
// (protocol 80, coop-fauna) The host's small animals near the guest: an eighth datagram kind, host -> partner only.
#define COOP_FAUNA_MAGIC 0x46435753u  // "SWCF"
typedef struct {
    uint32_t magic;
    uint8_t version, reserved8;
    uint16_t reserved;
    uint32_t seq;
    uint32_t pad;
    uint64_t session, echo;
    CoopFauna f;
} CoopFaunaPacket;
_Static_assert(sizeof(CoopFaunaAnimal) == 20 && sizeof(CoopFauna) == 24 + 20 * COOP_FAUNA_MAX &&
               sizeof(CoopFaunaPacket) == 32 + sizeof(CoopFauna), "co-op fauna wire layout");
_Static_assert(sizeof(CoopFaunaPacket) != sizeof(CoopObjPacket) && sizeof(CoopFaunaPacket) != sizeof(CoopEventPacket) &&
               sizeof(CoopFaunaPacket) != sizeof(CoopEnemyPacket) && sizeof(CoopFaunaPacket) != sizeof(CoopPacket) &&
               sizeof(CoopFaunaPacket) != sizeof(CoopAsTreePacket) && sizeof(CoopFaunaPacket) != sizeof(CoopPosePacket) &&
               sizeof(CoopFaunaPacket) != sizeof(CoopNamePacket) && sizeof(CoopFaunaPacket) != sizeof(CoopChallenge),
               "distinct sizes (protocol 80 fauna packet)");
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
// (49: the state packet, 1344 B, is now the largest kind)
#define MAX_DATAGRAM MAX2(sizeof(CoopPacket), MAX2(sizeof(CoopEnemyPacket), sizeof(CoopEventPacket)))
_Static_assert(MAX_DATAGRAM >= sizeof(CoopPacket), "the receive buffer holds a state packet");
_Static_assert(MAX_DATAGRAM >= sizeof(CoopFaunaPacket), "the receive buffer holds a fauna packet");
#define EVENT_QUEUE 128

static uint64_t local_session, peer_session;
static uint64_t challenge_key[2], guest_challenge;

// (protocol 53, 2026-11-15, user: playing over the internet with a forwarded port, no VPN) The join code. Every
// datagram ends with an 8-byte tag: SipHash-2-4 of the datagram under a key made from SAVAGEWIND_COOP_CODE (the
// same word typed on both PCs; empty = no code, then both must have none). A datagram whose tag does not match is
// dropped before anything reads it, so a stranger who finds the open port cannot join or send the game anything.
// (It does not hide the traffic and does not stop someone who can see and replay it; the session ids, sequence
// numbers and field checks stay as before.)
#define COOP_TAG 8
static uint64_t code_k0, code_k1;
static inline uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
#define SIPROUND do { v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32); v2 += v3; v3 = rotl64(v3, 16); \
    v3 ^= v2; v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0; v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32); } while (0)
static uint64_t siphash24(uint64_t k0, uint64_t k1, const unsigned char *in, size_t len) {
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    size_t end = len & ~(size_t)7;
    for (size_t i = 0; i < end; i += 8) {
        uint64_t m = 0;
        for (int j = 7; j >= 0; j--) m = (m << 8) | in[i + j];
        v3 ^= m; SIPROUND; SIPROUND; v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t j = 0; j < (len & 7); j++) b |= (uint64_t)in[end + j] << (8 * j);
    v3 ^= b; SIPROUND; SIPROUND; v0 ^= b;
    v2 ^= 0xff; SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
// The key from the code: two SipHashes of it under fixed keys (spaces around it and letter case do not matter).
static void code_key_derive(const char *code, uint64_t *out0, uint64_t *out1) {
    unsigned char c[64];
    size_t n = 0;
    while (code && (*code == ' ' || *code == '\t')) code++;
    for (; code && *code && n < sizeof c; code++) c[n++] = (unsigned char)(*code >= 'A' && *code <= 'Z' ? *code + 32 : *code);
    while (n && (c[n - 1] == ' ' || c[n - 1] == '\t' || c[n - 1] == '\r' || c[n - 1] == '\n')) n--;
    uint64_t k0 = siphash24(0x5361766167655769ull, 0x6e64436f6f704b30ull, c, n);
    uint64_t k1 = siphash24(0x5361766167655769ull, 0x6e64436f6f704b31ull, c, n);
    // (protocol 100) Stretch it: about 2^20 SipHashes (tens of ms once per start) per guess, so the 40-bit generated
    // code can't be brute-forced offline from one captured datagram. Worked out in locals: a running network thread
    // never sees a half-stretched key (the unit test re-keys while it runs; that failed on a slow CI machine).
    for (uint32_t i = 0; i < (1u << 20); i++) {
        uint64_t next = siphash24(k0, k1, c, n);
        k1 = siphash24(k1, k0, c, n) ^ i;
        k0 = next;
    }
    *out0 = k0, *out1 = k1;
}
static void code_key_init(const char *code) { code_key_derive(code, &code_k0, &code_k1); }
static void tag_put(unsigned char *p, uint64_t t) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(t >> (8 * i)); }
// Send one datagram with its tag (the size seen by the receiver: len + COOP_TAG).
static int coop_sendto(CoopSocket s, const void *buf, int len, const struct sockaddr_in *to) {
    unsigned char w[2048];
    if (len < 0 || len + COOP_TAG > (int)sizeof w) return -1;
    memcpy(w, buf, (size_t)len);
    tag_put(w + len, siphash24(code_k0, code_k1, w, (size_t)len));
    int r = (int)sendto(s, (const char *)w, len + COOP_TAG, 0, (const struct sockaddr *)to, sizeof *to);
    return r == len + COOP_TAG ? len : r;
}
// The received datagram's length without its tag, or -1 when the tag is wrong (another join code, or not ours).
static int tag_check(const unsigned char *w, int n) {
    if (n < COOP_TAG + 8) return -1;
    unsigned char t[8];
    tag_put(t, siphash24(code_k0, code_k1, w, (size_t)(n - COOP_TAG)));
    return memcmp(t, w + n - COOP_TAG, 8) ? -1 : n - COOP_TAG;
}
static uint32_t code_rejected;  // (tests, log)
_Static_assert(MAX_DATAGRAM + COOP_TAG <= 2048, "coop_sendto's buffer holds the largest datagram and its tag");
typedef struct Retired { uint64_t session; struct Retired *next; } Retired;
static Retired *retired; // process lifetime: never accept delayed packets from a replaced session

static bool magbeam_ok(const CoopPacket *p) {
    if (!(p->flags & COOP_FLAG_MAGBEAM))
        return !p->magbeam_tip[0] && !p->magbeam_tip[1] && !p->magbeam_tip[2];
    if (!(p->flags & COOP_FLAG_IN_WORLD) || !p->area) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(p->magbeam_tip[i]) || fabsf(p->magbeam_tip[i]) > 100000) return false;
    return coop_magbeam_dist2(p->magbeam_tip, p->pos) <= COOP_MAGBEAM_RANGE * COOP_MAGBEAM_RANGE;
}

static bool stasisfx_ok(const CoopPacket *p) {
    const CoopStasisFx *s = &p->stasisfx;
    if (s->flags & ~(COOP_STASISFX_ARROW | COOP_STASISFX_VISIBLE | COOP_STASISFX_START)) return false;
    if (s->hash) {
        if (!s->serial || !p->area || !(p->flags & COOP_FLAG_IN_WORLD)) return false;
    } else if (s->serial || s->flags || s->foe_ticks) return false;
    int x = s->direction[0], y = s->direction[1], z = s->direction[2];
    if (!(s->flags & COOP_STASISFX_ARROW)) return !(s->flags & COOP_STASISFX_VISIBLE) && !s->charge && !x && !y && !z;
    int length2 = x * x + y * y + z * z;
    return s->charge >= 1 && s->charge <= 100 && x >= -127 && y >= -127 && z >= -127 &&
           length2 >= 126 * 126 && length2 <= 128 * 128;
}

static bool packet_valid(const CoopPacket *p) {
    if (p->magic != COOP_MAGIC || p->version != COOP_VERSION || !p->session ||
        ((p->flags & COOP_FLAG_BLUE_FIRE) &&
         (p->flags & (COOP_FLAG_BURNING | COOP_FLAG_IN_WORLD)) != (COOP_FLAG_BURNING | COOP_FLAG_IN_WORLD)) ||
        ((p->flags & COOP_FLAG_LINK_ELEM) && !(p->flags & COOP_FLAG_IN_WORLD)) || p->anim >= COOP_ANIM_COUNT ||
        p->anim_upper >= COOP_ANIM_UPPER_COUNT || p->face >= COOP_FACE_COUNT || !weapon_name_ok(p->weapon) ||
        (p->aim_flags & ~COOP_AIM_FLAGS) || p->aim_pitch < -90 || p->aim_pitch > 90 || p->aim_yaw < -127 ||
        !weapon_name_ok(p->armor[0]) || !weapon_name_ok(p->armor[1]) || !weapon_name_ok(p->armor[2]) ||
        !isfinite(p->yaw) || fabsf(p->yaw) > 360 || !isfinite(p->speed) || p->speed < 0 || p->speed > 1000 ||
        p->weather > COOP_WEATHER_TYPES || p->moon_day > COOP_MOON_DAY_ON ||
        (p->origin && (p->weather || p->moon_day || p->bloodmoon)) || p->origin >= COOP_MAX_PLAYERS ||
        p->your_id >= COOP_MAX_PLAYERS || (p->role & ~(COOP_ROLE_PARTNER | COOP_ROLE_WORLD)))  // (56, 58)
        return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(p->pos[i]) || fabsf(p->pos[i]) > 100000) return false;
    if (!magbeam_ok(p) || !stasisfx_ok(p)) return false;
    for (int i = 0; i < COOP_GEAR_COUNT; i++)
        if (!gear_ok(&p->gear[i])) return false;
    for (int i = 0; i < COOP_BOMBS; i++)
        if (!coop_bomb_ok(&p->bombs[i])) return false;
    if (!coop_horse_ok(&p->horse)) return false;
    for (int i = 0; i < COOP_HELD_ITEMS; i++)
        if (!held_ok(&p->held[i])) return false;
    for (int i = 0; i < COOP_SHOT_VIEWS; i++)
        if (!coop_shot_view_ok(&p->shots[i])) return false;
    return true;
}

static bool session_retired(uint64_t session) {
    for (Retired *r = retired; r; r = r->next) if (r->session == session) return true;
    return false;
}

static bool adopt_session(uint64_t session) {
    if (peer_session == session) return true;
    if (session_retired(session)) return false;
    if (peer_session) {
        Retired *r = malloc(sizeof *r);
        if (!r) return false;
        *r = (Retired){peer_session, retired};
        retired = r;
    }
    peer_session = session;
    return true;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static CoopState local;
static uint32_t sent, received;
static uint8_t sent_weather, sent_moon_day;
static bool running, have_peer, host_mode;  // host_mode: no SAVAGEWIND_COOP_PEER, the peer is whoever calls
static struct sockaddr_in peer;
static CoopSocket sock = COOP_BAD_SOCKET;

// (protocol 56) What we know of each other player, by player id: their newest state packet and what came with it,
// their AS tree and ragdoll pose. The partner's view is views[partner_pid] (the old single "remote").
typedef struct {
    CoopState st;
    CoopBomb bombs[COOP_BOMBS];
    CoopShotView shots[COOP_SHOT_VIEWS];
    CoopHorse horse;
    uint64_t at_ns;  // when the newest state packet arrived (0: none yet)
    uint32_t seq;
    CoopAsTree astree;
    uint32_t astree_seq;
    uint64_t astree_ns;  // 0: none
    CoopPose pose;
    uint32_t pose_seq;
    uint64_t pose_ns;    // 0: none
} PlayerView;
static PlayerView views[COOP_MAX_PLAYERS];
static int my_id = -1;        // host: 0 (coop_net_init); guest: as the host says (your_id)
static int partner_pid = -1;  // the world partner's id: host: its partner guest; guest: 0 while the host says so
static bool guest_partner = true;  // guest: the host's role bit (an extra guest has no world partner)
static bool guest_world_all;       // (58) guest: the host shares the world with every guest (COOP_ROLE_WORLD)
// (58) SAVAGEWIND_COOP_MULTIWORLD=0: the world is shared with the partner only (the phase-1 extras). The host's
// setting decides for everyone; a guest with it off still keeps its world to itself.
static bool multiworld_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_MULTIWORLD") && !strcmp(getenv("SAVAGEWIND_COOP_MULTIWORLD"), "0"));
    return on;
}
static uint8_t players_mask;       // guest: the ids the host says are connected
// host: each guest's connection, by its id (the partner's too: conn[partner_pid] mirrors peer / peer_session)
static struct {
    bool used;
    struct sockaddr_in addr;
    uint64_t session, last_ns, restarted_ns;
} conn[COOP_MAX_PLAYERS];
static int view_pid = -1;  // (game thread) coop_net_view

// (62) names and ping (lock held): by player id; name_t / name_t_at: the newest t_ns heard from that player (a guest:
// [0] = the host's) and when, echoed back so the sender can time the round trip
static char names[COOP_MAX_PLAYERS][COOP_NAME_LEN];
static int pings[COOP_MAX_PLAYERS] = {-1, -1, -1, -1, -1, -1, -1, -1};
static uint64_t name_t[COOP_MAX_PLAYERS], name_t_at[COOP_MAX_PLAYERS], name_changed_ns[COOP_MAX_PLAYERS];
static char local_name[COOP_NAME_LEN];
static uint8_t tints[COOP_MAX_PLAYERS], local_tint;  // (65) tunic colours by player id, ours
static void view_reset(int pid) {
    if (pid < 0 || pid >= COOP_MAX_PLAYERS) return;
    memset(&views[pid], 0, sizeof views[pid]);
    names[pid][0] = 0, tints[pid] = 0, pings[pid] = -1, name_t[pid] = name_t_at[pid] = name_changed_ns[pid] = 0;
}
// (lock held) The view the partner calls read now: coop_net_view's player, else the partner's (NULL: none).
static PlayerView *view_cur(void) {
    int pid = view_pid >= 0 ? view_pid : partner_pid;
    return pid >= 0 && pid < COOP_MAX_PLAYERS ? &views[pid] : NULL;
}
static double age_of(uint64_t at_ns) { return (plat_now_ns() - at_ns) / 1e9; }

void coop_net_view(int pid) { view_pid = pid >= 0 ? pid : -1; }  // (>= COOP_MAX_PLAYERS: nobody)
int coop_net_my_id(void) {
    pthread_mutex_lock(&lock);
    int id = my_id;
    pthread_mutex_unlock(&lock);
    return id;
}
int coop_net_partner_id(void) {
    pthread_mutex_lock(&lock);
    int id = partner_pid;
    pthread_mutex_unlock(&lock);
    return id;
}
uint32_t coop_net_players(double max_age_s) {
    uint32_t m = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (i != my_id && views[i].at_ns && age_of(views[i].at_ns) <= max_age_s) m |= 1u << i;
    pthread_mutex_unlock(&lock);
    return m;
}

void coop_net_set_local(const CoopState *s) {
    pthread_mutex_lock(&lock);
    local = *s;
    pthread_mutex_unlock(&lock);
}

static CoopBomb local_bombs[COOP_BOMBS];  // (protocol 33)
void coop_net_set_bombs(const CoopBomb *b, int n) {
    pthread_mutex_lock(&lock);
    memset(local_bombs, 0, sizeof local_bombs);
    for (int i = 0; i < n && i < COOP_BOMBS; i++)
        if (coop_bomb_ok(&b[i])) local_bombs[i] = b[i];
    pthread_mutex_unlock(&lock);
}
bool coop_net_remote_bombs(CoopBomb out[COOP_BOMBS], double *age_s) {
    pthread_mutex_lock(&lock);
    PlayerView *v = view_cur();
    bool ok = v && v->at_ns != 0;
    if (ok) {
        memcpy(out, v->bombs, sizeof v->bombs);
        if (age_s) *age_s = age_of(v->at_ns);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

static CoopShotView local_shots[COOP_SHOT_VIEWS];  // (protocol 52)
void coop_net_set_shot_views(const CoopShotView *v, int n) {
    pthread_mutex_lock(&lock);
    memset(local_shots, 0, sizeof local_shots);
    for (int i = 0; i < n && i < COOP_SHOT_VIEWS; i++)
        if (coop_shot_view_ok(&v[i])) local_shots[i] = v[i];
    pthread_mutex_unlock(&lock);
}
bool coop_net_remote_shot_views(CoopShotView out[COOP_SHOT_VIEWS], double *age_s) {
    pthread_mutex_lock(&lock);
    PlayerView *v = view_cur();
    bool ok = v && v->at_ns != 0;
    if (ok) {
        memcpy(out, v->shots, sizeof v->shots);
        if (age_s) *age_s = age_of(v->at_ns);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

static CoopHorse local_horse;  // (39, coop-polish)
void coop_net_set_horse(const CoopHorse *h) {
    pthread_mutex_lock(&lock);
    if (h && coop_horse_ok(h)) local_horse = *h;
    else memset(&local_horse, 0, sizeof local_horse);
    pthread_mutex_unlock(&lock);
}
bool coop_net_remote_horse(CoopHorse *out, double *age_s) {
    pthread_mutex_lock(&lock);
    PlayerView *v = view_cur();
    bool ok = v && v->at_ns != 0;
    if (ok) {
        *out = v->horse;
        if (age_s) *age_s = age_of(v->at_ns);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

bool coop_net_get_remote(CoopState *out, double *age_s) {
    pthread_mutex_lock(&lock);
    PlayerView *v = view_cur();
    bool ok = v && v->at_ns != 0;
    if (ok) {
        *out = v->st;
        if (age_s) *age_s = age_of(v->at_ns);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

bool coop_net_get_host(CoopState *out, double *age_s, uint64_t *session) {
    pthread_mutex_lock(&lock);
    bool ok = !host_mode && views[0].at_ns;
    if (ok) {
        *out = views[0].st;
        *age_s = age_of(views[0].at_ns);
        *session = peer_session;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

bool coop_net_lightning_recent(uint64_t at_ns) {
    pthread_mutex_lock(&lock);
    // Extrapolate from the host's ping/name packet. Its clock has a different epoch.
    uint64_t elapsed = plat_now_ns() - name_t_at[0];
    uint64_t estimate = name_t[0] + elapsed;
    bool ok = !host_mode && name_t[0] && elapsed < 5000000000ull &&
              (at_ns > estimate ? at_ns - estimate < 1000000000ull : estimate - at_ns < 1000000000ull);
    pthread_mutex_unlock(&lock);
    return ok;
}

void coop_net_stats(uint32_t *s, uint32_t *r, bool *peer_known) {
    pthread_mutex_lock(&lock);
    *s = sent, *r = received, *peer_known = have_peer;
    pthread_mutex_unlock(&lock);
}

bool coop_net_on(void) { return running; }

static CoopEnemyDamage local_enemies[COOP_ENEMY_LOCAL_MAX];  // (n6: the host's list may be longer than a packet)
static uint16_t enemy_generation;
static int local_enemy_count;
static float local_hour = -1, remote_hour = -1;  // hour of day (enemy packet), -1: unknown
static uint64_t remote_hour_ns;
static CoopSpawnName local_names[COOP_SPAWN_NAMES];
#define REMOTE_NAMES 128
static CoopSpawnName remote_names[REMOTE_NAMES];
static int remote_name_next;  // (ring: the oldest name is replaced)

// (58) The world data of one link: a guest: wl[0] = the host; the host: wl[g] = guest g (every guest that shares the
// world: the partner, and with SAVAGEWIND_COOP_MULTIWORLD every guest). Its enemy entries and the reliable events
// to / from it. (Before 58 there was one of each: the partner's.)
typedef struct {
    CoopEnemyDamage en[REMOTE_ENEMIES];  // (full: the longest-unheard entry is replaced)
    uint32_t pos_seq[REMOTE_ENEMIES];    // packet seq of each position (newest wins)
    uint64_t pos_at[REMOTE_ENEMIES];     // when it arrived (0: never)
    uint64_t heard[REMOTE_ENEMIES];      // when the link last sent this enemy at all
    uint32_t flags_seq[REMOTE_ENEMIES];  // packet seq of the flags kept in en[].flags
    int n;
    CoopEvent out[EVENT_QUEUE];  // unacknowledged, oldest first
    int out_count;
    uint32_t out_next_id;        // restarts at 1 for every new session of the link
    uint32_t in_last;            // the last event id taken in order from the link
    uint64_t ack_until_ns;       // keep sending event packets (with our ack) until then
} WorldLink;
static WorldLink wl[COOP_MAX_PLAYERS];
// Reliable events received in order, not yet taken by the game thread (from every link; type's high byte = origin).
static CoopEvent in_events[EVENT_QUEUE];
static int in_count;
static uint32_t relay_dropped;  // (58, host) relayed events another guest's full queue could not take

static void ol_reset(int L);
static void link_reset(int L) {  // (lock held) a new session of the link: its world data starts again
    if (L < 0 || L >= COOP_MAX_PLAYERS) return;
    WorldLink *w = &wl[L];
    w->n = 0, w->out_count = 0, w->out_next_id = 1, w->in_last = 0, w->ack_until_ns = 0;
    ol_reset(L);
}
// host (lock held): does guest g share the world with us?
static bool link_world(int g) {
    return g > 0 && g < COOP_MAX_PLAYERS && conn[g].used && conn[g].session && (g == partner_pid || multiworld_on());
}
// (lock held) the links that hold world data now
static bool link_on(int L) {
    if (!host_mode) return L == 0 && guest_partner;
    return link_world(L);
}

// (58) Which events stay between the host and one guest (the partner) instead of going to every player: a hit on the
// other player's avatar (it names the receiver's own Link), and (only without MULTIWORLD, 59) the shared physics
// objects' events. Everything else is a world event: the host relays it to every other guest, whose
// game checks it like one of the host's (distance to its own Link, area...).
#define EV_NUM_OBJ 8  // (runtime/coop/objects.inc EV_OBJ)
static bool ev_pairwise(uint16_t type, const uint8_t *data, int len) {
    if (type == COOP_EV_WILD_POLL || type == COOP_EV_WILD_ROSTER) return true;
    if (type == COOP_EV_DISH || type == COOP_EV_DISH_RESULT) return true;
    if (type == COOP_EV_SHRINE_RIDE || type == COOP_EV_SHRINE_DOOR) return true;
    if ((type & 0xff) == EV_NUM_OBJ) return !multiworld_on();  // (59: objects shared with every world player)
    if ((type & 0xff) == COOP_EV_AREAFX) return !(len >= 1 && data[0] == COOP_AREAFX_BOMB);
    return false;
}
// (lock held) queue an event on one link; origin: who it comes from (0: us / the host)
static bool link_push(int L, uint16_t type, const void *data, int len, int origin) {
    WorldLink *w = &wl[L];
    if (w->out_count >= EVENT_QUEUE) return false;
    if (!w->out_next_id) w->out_next_id = 1;
    CoopEvent *e = &w->out[w->out_count++];
    memset(e, 0, sizeof *e);
    e->id = w->out_next_id++, e->type = (uint16_t)((type & 0xff) | (origin << 8)), e->len = (uint16_t)len;
    memcpy(e->data, data, (size_t)len);
    return true;
}

static bool lightning_payload_ok(const void *data, int len) {
    CoopLightning e;
    if (len != sizeof e) return false;
    memcpy(&e, data, sizeof e);
    return coop_lightning_ok(&e);
}

_Static_assert(sizeof(CoopShrineRide) == 16 && sizeof(CoopShrineDoor) == 16 && 16 <= COOP_EVENT_DATA,
               "co-op shrine event layout");
static bool shrine_pos_ok(const float pos[3]) {
    for (int i = 0; i < 3; i++)
        if (!isfinite(pos[i]) || fabsf(pos[i]) > 10000.0f) return false;
    return true;
}
bool coop_shrine_ride_ok(const CoopShrineRide *ride) {
    return ride->hash && shrine_pos_ok(ride->pos);
}
bool coop_shrine_door_ok(const CoopShrineDoor *door) {
    return (door->dungeon <= COOP_SHRINE_MAX || door->dungeon == COOP_SHRINE_DOOR_SCENE) && shrine_pos_ok(door->pos);
}
static bool shrine_type(uint16_t type) {
    return type == COOP_EV_SHRINE_RIDE || type == COOP_EV_SHRINE_DOOR;
}
static bool shrine_payload_ok(uint16_t type, const void *data, int len) {
    if (type == COOP_EV_SHRINE_RIDE) {
        CoopShrineRide ride;
        if (len != sizeof ride) return false;
        memcpy(&ride, data, sizeof ride);
        return coop_shrine_ride_ok(&ride);
    }
    CoopShrineDoor door;
    if (len != sizeof door) return false;
    memcpy(&door, data, sizeof door);
    return coop_shrine_door_ok(&door);
}

_Static_assert(sizeof(CoopDishOffer) == 96 && sizeof(CoopDishResult) == 32 &&
               sizeof(CoopDishOffer) <= COOP_EVENT_DATA, "co-op dish wire layout");

bool coop_dish_offer_ok(const CoopDishOffer *offer) {
    const CoopDish *dish = &offer->dish;
    const char *end = memchr(dish->name, 0, sizeof dish->name);
    if (!offer->from || offer->from == offer->to || !offer->id || !offer->area || offer->pad[0] || offer->pad[1] ||
        offer->from_pid >= COOP_MAX_PLAYERS || offer->to_pid >= COOP_MAX_PLAYERS || offer->from_pid == offer->to_pid ||
        !end || end == dish->name + 10 || strncmp(dish->name, "Item_Cook_", 10)) return false;
    for (const char *p = dish->name + 10; p < end; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return dish->hearts >= 0 && dish->hearts <= 120 &&
           dish->seconds >= 0 && dish->seconds <= 1800 &&
           dish->price >= 0 && dish->price <= 99999 &&
           dish->effect >= -1 && dish->effect <= 32 &&
           isfinite(dish->level) && dish->level >= 0 && dish->level <= 120 && dish->level == truncf(dish->level);
}

bool coop_dish_result_ok(const CoopDishResult *result) {
    return result->from && result->to && result->from != result->to && result->id &&
           result->accepted <= 1 && !result->pad[0] && !result->pad[1] && result->from_pid < COOP_MAX_PLAYERS &&
           result->to_pid < COOP_MAX_PLAYERS && result->from_pid != result->to_pid;
}

static bool dish_type(uint16_t type) {
    return type == COOP_EV_DISH || type == COOP_EV_DISH_RESULT;
}

// (DISHINV, lock held) Player pid's session as we know it: the host knows every guest's; a guest only the host's
// (0 for another guest). 0 too while there is no world link to pid.
static bool dish_link(int pid) {
    if (pid < 0 || pid >= COOP_MAX_PLAYERS || pid == my_id || my_id < 0 || !local_session) return false;
    if (host_mode) return pid > 0 && link_world(pid) && (pid != partner_pid || (have_peer && peer_session));
    return have_peer && peer_session && guest_partner && (pid == 0 || guest_world_all);
}
static uint64_t dish_session_of(int pid) {
    if (!dish_link(pid)) return 0;
    return host_mode ? conn[pid].session : pid == 0 ? peer_session : 0;
}
bool coop_net_dish_session(int pid, uint64_t *ours, uint64_t *theirs) {
    pthread_mutex_lock(&lock);
    bool ok = dish_link(pid);
    *ours = local_session, *theirs = dish_session_of(pid);
    pthread_mutex_unlock(&lock);
    return ok;
}
static bool dish_parse(uint16_t type, const void *data, int len, CoopDishOffer *o, CoopDishResult *r) {
    if (type == COOP_EV_DISH) {
        if (len != sizeof *o) return false;
        memcpy(o, data, sizeof *o);
        return coop_dish_offer_ok(o);
    }
    if (len != sizeof *r) return false;
    memcpy(r, data, sizeof *r);
    return coop_dish_result_ok(r);
}
// (lock held) Ours to send to player pid: from us (session and id), to pid (its session where we know it; a guest's
// offer to another guest has to = 0).
static bool dish_send_ok(int pid, uint16_t type, const void *data, int len) {
    CoopDishOffer o;
    CoopDishResult r;
    if (!dish_link(pid) || !dish_parse(type, data, len, &o, &r)) return false;
    uint64_t them = dish_session_of(pid);
    if (type == COOP_EV_DISH)
        return o.from == local_session && o.from_pid == my_id && o.to_pid == pid && (them ? o.to == them : !o.to);
    return r.from == local_session && r.from_pid == my_id && r.to_pid == pid && (!them || r.to == them);
}
// (host, lock held) Guest L's dish event for guest target: from L (its session) to target; an offer gets target's
// session filled in (data is changed), a result must already name it.
static bool dish_relay_ok(int L, int target, uint16_t type, uint8_t *data, int len) {
    CoopDishOffer o;
    CoopDishResult r;
    if (target <= 0 || target >= COOP_MAX_PLAYERS || target == L || !dish_link(target) || !link_world(L) ||
        !dish_parse(type, data, len, &o, &r)) return false;
    uint64_t them = conn[target].session;
    if (type == COOP_EV_DISH) {
        if (o.from != conn[L].session || o.from_pid != L || o.to_pid != target || (o.to && o.to != them)) return false;
        o.to = them;
        memcpy(data, &o, sizeof o);
        return true;
    }
    return r.from == conn[L].session && r.from_pid == L && r.to_pid == target && r.to == them;
}
// (lock held) A dish event for us from player origin (host: guest origin's session; a guest: the host's session when
// the host sent it, else the host checked the sender when it passed it on).
static bool dish_recv_ok(int origin, uint16_t type, const void *data, int len) {
    CoopDishOffer o;
    CoopDishResult r;
    if (origin < 0 || origin >= COOP_MAX_PLAYERS || origin == my_id || !dish_link(origin) ||
        !dish_parse(type, data, len, &o, &r)) return false;
    uint64_t from_s = host_mode ? conn[origin].session : origin == 0 ? peer_session : 0;
    uint64_t from = type == COOP_EV_DISH ? o.from : r.from, to = type == COOP_EV_DISH ? o.to : r.to;
    int from_pid = type == COOP_EV_DISH ? o.from_pid : r.from_pid, to_pid = type == COOP_EV_DISH ? o.to_pid : r.to_pid;
    return to == local_session && to_pid == my_id && from_pid == origin && (!from_s || from == from_s);
}

bool coop_net_send_event(uint16_t type, const void *data, int len) {
    if (len < 0 || len > COOP_EVENT_DATA || type > 0xff) return false;
    if (dish_type(type) || shrine_type(type)) return false;  // addressed to one player
    pthread_mutex_lock(&lock);
    bool ok = false;
    if (type == COOP_EV_LIGHTNING && (!host_mode || !lightning_payload_ok(data, len))) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    if (!host_mode) {
        ok = have_peer && peer_session && guest_partner && link_push(0, type, data, len, 0);
    } else {
        bool pair = ev_pairwise(type, data, len);
        for (int g = 1; g < COOP_MAX_PLAYERS; g++) {
            if (!link_world(g) || (pair && g != partner_pid)) continue;
            if (g == partner_pid && !(have_peer && peer_session)) continue;
            if (link_push(g, type, data, len, 0)) ok = true;
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

// (60) An event for one player only: pid 0 = the host, else that guest. The host queues it on that guest's link; a
// guest sends it to the host with the target in the type's high byte (0: the host itself), and the host passes it on
// to that guest with our id as origin (take_events). Only to a player whose world is shared with ours.
bool coop_net_send_event_to(int pid, uint16_t type, const void *data, int len) {
    if (len < 0 || len > COOP_EVENT_DATA || type > 0xff || pid < 0 || pid >= COOP_MAX_PLAYERS) return false;
    pthread_mutex_lock(&lock);
    if ((type == COOP_EV_LIGHTNING && (!host_mode || !lightning_payload_ok(data, len))) ||
        (shrine_type(type) && !shrine_payload_ok(type, data, len)) ||
        (dish_type(type) && !dish_send_ok(pid, type, data, len))) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    bool ok = false;
    if (!host_mode) {
        ok = pid != my_id && have_peer && peer_session && guest_partner && link_push(0, type, data, len, pid);
    } else if (pid > 0 && link_world(pid) && (pid != partner_pid || (have_peer && peer_session))) {
        ok = link_push(pid, type, data, len, 0);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

bool coop_net_next_event_from(uint16_t *type, void *data, int *len, int *origin) {
    pthread_mutex_lock(&lock);
    bool ok = in_count > 0;
    if (ok) {
        *type = in_events[0].type & 0xff, *len = in_events[0].len;
        if (origin) *origin = in_events[0].type >> 8;
        memcpy(data, in_events[0].data, COOP_EVENT_DATA);
        memmove(in_events, in_events + 1, sizeof *in_events * (size_t)--in_count);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
bool coop_net_next_event(uint16_t *type, void *data, int *len) { return coop_net_next_event_from(type, data, len, NULL); }

// (lock held) Link L's ack drops our delivered events; its events are taken in order (a gap waits for the resend).
// Host: an event from guest L is ours to apply (origin L) and, unless pairwise, goes on to every other world guest.
static void take_events(int L, const CoopEventPacket *p) {
    WorldLink *w = &wl[L];
    int drop = 0;
    while (drop < w->out_count && (int32_t)(p->ack - w->out[drop].id) >= 0) drop++;
    if (drop) memmove(w->out, w->out + drop, sizeof *w->out * (size_t)(w->out_count -= drop));
    for (int i = 0; i < p->count && i < EVENTS_PER_PACKET; i++) {
        const CoopEvent *e = &p->e[i];
        if (e->id != w->in_last + 1 || e->len > COOP_EVENT_DATA) continue;  // (old resend, or a gap)
        if (in_count == EVENT_QUEUE) break;                                 // (full: not acknowledged, comes again)
        w->in_last = e->id;
        uint16_t type = e->type & 0xff;
        int target = host_mode ? e->type >> 8 : 0;  // (60) a guest's event for another player only: passed on
        const uint8_t *payload = e->data;
        uint8_t stamped[COOP_EVENT_DATA];  // (DISHINV) a dish offer passed on, the receiver's session filled in
        if (dish_type(type)) {
            if (target) {
                memcpy(stamped, e->data, COOP_EVENT_DATA);
                if (!dish_relay_ok(L, target, type, stamped, e->len)) continue;
                payload = stamped;
            } else if (!dish_recv_ok(host_mode ? L : e->type >> 8, type, e->data, e->len)) continue;
        }
        if (type == COOP_EV_LIGHTNING && (host_mode || e->type >> 8 || !lightning_payload_ok(e->data, e->len)))
            continue;
        if (shrine_type(type) && !shrine_payload_ok(type, e->data, e->len)) continue;
        if (target) {
            if (in_count == EVENT_QUEUE) break;  // (keeps the order with the events below: comes again)
            w->in_last = e->id;
            if (target < COOP_MAX_PLAYERS && target != L && link_world(target) && !link_push(target, type, payload, e->len, L) &&
                !(relay_dropped++ % 64))
                LOG("[coop] player %d's event queue is full: an event of player %d for it not passed on (%u so far)\n",
                    target, L, relay_dropped);
            continue;
        }
        int origin = host_mode ? L : e->type >> 8;  // (a guest: the host names the player it relays)
        if (origin >= COOP_MAX_PLAYERS || (!host_mode && origin == my_id && my_id > 0)) continue;
        if (host_mode && L != partner_pid && type == EV_NUM_OBJ && !multiworld_on()) continue;  // (objects: the partner's only)
        in_events[in_count] = *e;
        in_events[in_count++].type = (uint16_t)(type | (origin << 8));
        if (host_mode && !ev_pairwise(type, e->data, e->len))
            for (int g = 1; g < COOP_MAX_PLAYERS; g++)
                if (g != L && link_world(g) && !link_push(g, type, e->data, e->len, L) && !(relay_dropped++ % 64))
                    LOG("[coop] player %d's event queue is full: an event of player %d not passed on (%u so far)\n", g, L,
                        relay_dropped);
    }
    if (p->count) w->ack_until_ns = plat_now_ns() + 2000000000ull;  // (answer with our ack for a while)
}

void coop_net_set_spawn_names(const CoopSpawnName *n, int count) {
    pthread_mutex_lock(&lock);
    memset(local_names, 0, sizeof local_names);
    for (int i = 0; i < count && i < COOP_SPAWN_NAMES; i++) local_names[i] = n[i];
    pthread_mutex_unlock(&lock);
}

// Actor names: "Enemy_" (or, protocol 55, "Animal_": SPAWNANIMALS) and the actor-name alphabet, terminated within
// the field. (Probe 2026-11-16: animal names were dropped here, so no run-time animal copy was ever made.)
static bool spawn_name_ok(const char *s) {
    if (strncmp(s, "Enemy_", 6) && strncmp(s, "Animal_", 7)) return false;
    for (int i = 0; i < COOP_SPAWN_NAME_LEN; i++) {
        char ch = s[i];
        if (!ch) return i > 6;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return false;
}

bool coop_net_remote_spawn_name(uint32_t id, char *out, size_t cap) {
    bool ok = false;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < REMOTE_NAMES && !ok; i++)
        if (remote_names[i].id == id) snprintf(out, cap, "%s", remote_names[i].name), ok = true;
    pthread_mutex_unlock(&lock);
    return ok;
}

// (lock held)
static void take_remote_names(const CoopEnemyPacket *p) {
    for (int i = 0; i < COOP_SPAWN_NAMES; i++) {
        const CoopSpawnName *n = &p->names[i];
        if (!(n->id & 0x80000000u) || !spawn_name_ok(n->name)) continue;
        int k = 0;
        while (k < REMOTE_NAMES && remote_names[k].id != n->id) k++;
        if (k < REMOTE_NAMES) continue;
        remote_names[remote_name_next] = *n;
        remote_name_next = (remote_name_next + 1) % REMOTE_NAMES;
    }
}

void coop_net_set_hour(float hour) {
    pthread_mutex_lock(&lock);
    local_hour = hour;
    pthread_mutex_unlock(&lock);
}

bool coop_net_remote_hour(float *hour, double *age_s) {
    pthread_mutex_lock(&lock);
    bool ok = remote_hour >= 0 && remote_hour_ns;
    if (ok) *hour = remote_hour, *age_s = (plat_now_ns() - remote_hour_ns) / 1e9;
    pthread_mutex_unlock(&lock);
    return ok;
}

void coop_net_set_enemies(const CoopEnemyDamage *e, int n) {
    if (n < 0) n = 0;
    if (n > COOP_ENEMY_LOCAL_MAX) n = COOP_ENEMY_LOCAL_MAX;
    pthread_mutex_lock(&lock);
    if (!host_mode && n > COOP_ENEMY_MAX) n = COOP_ENEMY_MAX;  // (a guest's list is its packet)
    memcpy(local_enemies, e, sizeof *e * (size_t)n);
    local_enemy_count = n;
    pthread_mutex_unlock(&lock);
}

bool coop_net_is_host(void) { return running && host_mode; }

// ---- shared physics objects
// (59) One table per link: a guest: [0] = what the host sends (its own objects and every other player's, merged: one
// entry per object, the winning claim, `pid` = its owner); the host: [g] = guest g's own objects (pid = g). Before 59
// there was one table, the partner's.
static CoopObject local_objects[COOP_OBJ_MAX];
static int local_object_count;
typedef struct {
    CoopObject o[REMOTE_OBJECTS];
    uint32_t seq[REMOTE_OBJECTS];
    uint64_t at[REMOTE_OBJECTS];  // when it arrived (0: never)
    int n;
} ObjLink;
static ObjLink ol[COOP_MAX_PLAYERS];
static void ol_reset(int L) {
    if (L >= 0 && L < COOP_MAX_PLAYERS) ol[L].n = 0;
}
#define OBJ_FRESH_NS 1500000000ull  // an entry this old still counts as the owner's (runtime/coop objects OBJ_STALE_S)
#define OBJ_RELAY_NS 1000000000ull  // (host) another guest's entry is passed on while it is this fresh
void coop_net_set_objects(const CoopObject *o, int n) {
    if (n < 0) n = 0;
    if (n > COOP_OBJ_MAX) n = COOP_OBJ_MAX;
    pthread_mutex_lock(&lock);
    memcpy(local_objects, o, sizeof *o * (size_t)n);
    for (int i = 0; i < n; i++) local_objects[i].pid = (uint8_t)(my_id > 0 ? my_id : 0);
    local_object_count = n;
    pthread_mutex_unlock(&lock);
}
bool coop_object_ok(const CoopObject *o) {
    for (int k = 0; k < 3; k++)
        if (!isfinite(o->pos[k]) || fabsf(o->pos[k]) > 100000 || !isfinite(o->vel[k]) || fabsf(o->vel[k]) > 1000) return false;
    if (o->flags & COOP_OBJ_RAIL) {  // (94) a rail lift: rot = {start, dest, dir 0/1, wait or -1}, not a quaternion
        for (int k = 0; k < 4; k++)
            if (!isfinite(o->rot[k]) || fabsf(o->rot[k]) > 100000) return false;
        return o->rot[2] >= 0 && o->rot[2] <= 1 && o->hash != 0 && o->pid < COOP_MAX_PLAYERS;
    }
    float norm2 = 0;
    for (int k = 0; k < 4; k++) {
        if (!isfinite(o->rot[k]) || fabsf(o->rot[k]) > 1.01f) return false;
        norm2 += o->rot[k] * o->rot[k];
    }
    return norm2 >= 0.5f && norm2 <= 1.5f && o->hash != 0 && o->pid < COOP_MAX_PLAYERS;
}
// (lock held) Link L's object datagram. Host: the entries are guest L's own (pid = L, whatever it says). Guest: the
// host's merged view; an entry naming us as its owner is dropped (ours is ours).
static void take_remote_objects(int L, const CoopObjPacket *p) {
    ObjLink *t = &ol[L];
    for (int i = 0; i < p->count && i < COOP_OBJ_MAX; i++) {
        CoopObject o = p->o[i];
        if (host_mode) o.pid = (uint8_t)L;
        if (!coop_object_ok(&o) || (!host_mode && my_id > 0 && o.pid == my_id)) continue;
        if (!(o.flags & COOP_OBJ_RAIL)) {
            float norm2 = 0;
            for (int k = 0; k < 4; k++) norm2 += o.rot[k] * o.rot[k];
            float inv_norm = 1 / sqrtf(norm2);
            for (int k = 0; k < 4; k++) o.rot[k] *= inv_norm;
        }
        int k = 0;
        while (k < t->n && t->o[k].hash != o.hash) k++;
        if (k == t->n) {
            if (t->n == REMOTE_OBJECTS) {  // full: replace the one not heard of for the longest
                k = 0;
                for (int j = 1; j < t->n; j++)
                    if (t->at[j] < t->at[k]) k = j;
            } else {
                t->n++;
            }
            t->at[k] = 0;
        }
        if (t->at[k] && (int32_t)(p->seq - t->seq[k]) <= 0) continue;  // (an older packet)
        t->o[k] = o;
        t->o[k].flags &= COOP_OBJ_OWNED | COOP_OBJ_CARRIED | COOP_OBJ_STASIS | COOP_OBJ_RAIL;
        t->seq[k] = p->seq, t->at[k] = plat_now_ns();
    }
}
// Does claim a beat claim b? A newer generation wins; a tie: the lower player id (the host, 0, first).
static bool obj_claim_beats(const CoopObject *a, const CoopObject *b) {
    int16_t d = (int16_t)(a->gen - b->gen);
    return d > 0 || (d == 0 && a->pid < b->pid);
}
// (lock held) The winning entry for an object among the links that hold world data (a guest: the host's; the host:
// every world guest's), with our own objects too when `with_local`. Entries younger than `fresh_ns` compete; with
// none, `stale_ok` gives the newest older one. at: when it arrived (local: now). False: nobody sent it.
static bool obj_pick(uint32_t hash, bool with_local, uint64_t fresh_ns, bool stale_ok, uint64_t now, CoopObject *out,
                     uint64_t *at) {
    bool have = false, have_stale = false;
    CoopObject best, stale;
    uint64_t best_at = 0, stale_at = 0;
    if (with_local)
        for (int i = 0; i < local_object_count; i++)
            if (local_objects[i].hash == hash) best = local_objects[i], best.pid = (uint8_t)(my_id > 0 ? my_id : 0), best_at = now, have = true;
    for (int L = 0; L < COOP_MAX_PLAYERS; L++) {
        if (!link_on(L)) continue;
        const ObjLink *t = &ol[L];
        for (int k = 0; k < t->n; k++) {
            if (t->o[k].hash != hash || !t->at[k]) continue;
            if (now - t->at[k] <= fresh_ns) {
                if (!have || obj_claim_beats(&t->o[k], &best)) best = t->o[k], best_at = t->at[k], have = true;
            } else if (!have_stale || t->at[k] > stale_at) {
                stale = t->o[k], stale_at = t->at[k], have_stale = true;
            }
            break;
        }
    }
    if (have) { *out = best, *at = best_at; return true; }
    if (stale_ok && have_stale) { *out = stale, *at = stale_at; return true; }
    return false;
}
bool coop_net_remote_object(uint32_t hash, CoopObject *out, double *age_s) {
    pthread_mutex_lock(&lock);
    uint64_t now = plat_now_ns(), at = 0;
    bool ok = obj_pick(hash, false, OBJ_FRESH_NS, true, now, out, &at);
    if (ok) *age_s = (double)(now - at) / 1e9;
    pthread_mutex_unlock(&lock);
    return ok;
}
int coop_net_remote_objects(CoopObject *out, int max, double max_age_s) {
    int n = 0;
    pthread_mutex_lock(&lock);
    uint64_t now = plat_now_ns(), win = (uint64_t)(max_age_s * 1e9), at;
    for (int L = 0; L < COOP_MAX_PLAYERS; L++) {
        if (!link_on(L)) continue;
        for (int k = 0; k < ol[L].n && n < max; k++) {
            if (!ol[L].at[k] || now - ol[L].at[k] > win) continue;
            int j = 0;
            while (j < n && out[j].hash != ol[L].o[k].hash) j++;
            if (j < n) continue;
            if (obj_pick(ol[L].o[k].hash, false, win, false, now, &out[n], &at)) n++;
        }
    }
    pthread_mutex_unlock(&lock);
    return n;
}
// (59, host, lock held) The objects guest G gets: ours and every other world guest's fresh entries, one per object:
// the winning claim (obj_claim_beats). An object whose winning claim is G's own is left out (G owns it). Returns the
// count (at most max; ours first, then the others' from a turning start, so more than fit all come in turns).
static int build_objects_for(int G, CoopObject *out, int max, uint64_t now) {
    int n = 0;
    uint64_t at;
    for (int i = 0; i < local_object_count && n < max; i++) {
        CoopObject w;
        if (!obj_pick(local_objects[i].hash, true, OBJ_RELAY_NS, false, now, &w, &at) || w.pid == G) continue;
        out[n++] = w;
    }
    static unsigned turn;
    turn++;
    for (int L = 1; L < COOP_MAX_PLAYERS && n < max; L++) {
        if (L == G || !link_world(L) || !ol[L].n) continue;
        const ObjLink *t = &ol[L];
        for (int c = 0; c < t->n && n < max; c++) {
            int k = (int)((c + turn) % (unsigned)t->n);
            if (!t->at[k] || now - t->at[k] > OBJ_RELAY_NS || !(t->o[k].flags & COOP_OBJ_OWNED)) continue;
            int j = 0;
            while (j < n && out[j].hash != t->o[k].hash) j++;
            if (j < n) continue;
            CoopObject w;
            if (!obj_pick(t->o[k].hash, true, OBJ_RELAY_NS, false, now, &w, &at) || w.pid == G) continue;
            out[n++] = w;
        }
    }
    return n;
}

// ---- (protocol 29) tree mode
static CoopAsTree local_astree;  // (the other players': PlayerView.astree)
static bool local_astree_set;
void coop_net_set_astree(const CoopAsTree *t) {
    pthread_mutex_lock(&lock);
    local_astree = *t;
    local_astree_set = true;
    pthread_mutex_unlock(&lock);
}
bool coop_net_remote_astree(CoopAsTree *out, double *age_s) {
    pthread_mutex_lock(&lock);
    PlayerView *v = view_cur();
    bool ok = v && v->astree_ns != 0;
    if (ok) *out = v->astree, *age_s = age_of(v->astree_ns);
    pthread_mutex_unlock(&lock);
    return ok;
}
// ---- (protocol 80) fauna snapshot: host -> each world guest (its own set: the animals near it), each set sent once
static CoopFauna local_fauna[COOP_MAX_PLAYERS], remote_fauna;
static bool local_fauna_pending[COOP_MAX_PLAYERS];
static uint32_t local_fauna_seq[COOP_MAX_PLAYERS], remote_fauna_seq;
static uint64_t remote_fauna_ns, remote_fauna_session;
void coop_net_set_fauna_for(int pid, const CoopFauna *f) {
    if (pid < 1 || pid >= COOP_MAX_PLAYERS) return;
    pthread_mutex_lock(&lock);
    local_fauna[pid] = *f;
    local_fauna_pending[pid] = true;
    pthread_mutex_unlock(&lock);
}
void coop_net_set_fauna(const CoopFauna *f) {
    pthread_mutex_lock(&lock);
    int pid = partner_pid;
    pthread_mutex_unlock(&lock);
    coop_net_set_fauna_for(pid, f);
}
bool coop_net_remote_fauna(CoopFauna *out, double *age_s, uint32_t *seq) {
    pthread_mutex_lock(&lock);
    bool ok = !host_mode && remote_fauna_ns && remote_fauna_session == peer_session && peer_session;
    if (ok) *out = remote_fauna, *age_s = age_of(remote_fauna_ns), *seq = remote_fauna_seq;
    pthread_mutex_unlock(&lock);
    return ok;
}
// ---- (protocol 48) ragdoll pose
static CoopPose local_pose;  // (the other players': PlayerView.pose)
static bool local_pose_set;
void coop_net_set_pose(const CoopPose *p) {
    pthread_mutex_lock(&lock);
    if (p) local_pose = *p;
    local_pose_set = p != NULL;
    pthread_mutex_unlock(&lock);
}
bool coop_net_remote_pose(CoopPose *out, double *age_s) {
    pthread_mutex_lock(&lock);
    PlayerView *v = view_cur();
    bool ok = v && v->pose_ns != 0;
    if (ok) *out = v->pose, *age_s = age_of(v->pose_ns);
    pthread_mutex_unlock(&lock);
    return ok;
}
bool coop_pose_ok(const CoopPose *p) {
    if ((p->flags & ~COOP_POSE_ACTIVE) || p->count != COOP_POSE_BONES || p->pad || p->pad2 || (p->mask >> COOP_POSE_BONES)) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(p->root[i]) || fabsf(p->root[i]) > 20) return false;
    for (int b = 0; b < COOP_POSE_BONES; b++) {
        float l = 0;
        for (int k = 0; k < 4; k++) l += ((float)p->q[b][k] / 32767.0f) * ((float)p->q[b][k] / 32767.0f);
        if (p->mask >> b & 1 ? l < 0.8f * 0.8f || l > 1.2f * 1.2f : l != 0) return false;
    }
    return true;
}
bool coop_beast_rot_ok(const CoopBeastRot *e) {
    return e->hash && e->area && isfinite(e->angle) && fabsf(e->angle) <= 6.3f;
}
bool coop_gyro_ok(const CoopGyro *e) {
    if (!e->hash || !e->area || (e->flags & ~COOP_GYRO_DRIVING) || e->pad) return false;
    for (int r = 0; r < 3; r++) {
        float l = 0;
        for (int k = 0; k < 3; k++) {
            float v = e->rot[3 * r + k];
            if (!isfinite(v) || fabsf(v) > 1.01f) return false;
            l += v * v;
        }
        if (l < 0.9f || l > 1.1f) return false;
    }
    return true;
}
bool coop_tree_ok(const CoopTree *e) {
    if (!e->hash || !e->area || e->instant > 1 || e->pad[0] || e->pad[1] || e->pad[2]) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->dir[k]) || fabsf(e->dir[k]) > 1.01f) return false;
    return true;
}
bool coop_tree_hit_ok(const CoopTreeHit *e) {
    return e->hash && e->area && e->life > 0 && e->life <= COOP_TREE_LIFE_MAX &&
           isfinite(e->from[0]) && fabsf(e->from[0]) <= 100000.0f &&
           isfinite(e->from[1]) && fabsf(e->from[1]) <= 100000.0f;
}
_Static_assert(sizeof(CoopArrowFly) == 40 && sizeof(CoopArrowFly) <= COOP_EVENT_DATA, "co-op arrow fly layout (74)");
bool coop_arrow_fly_ok(const CoopArrowFly *e) {
    if (!e->id || !e->area || e->kind >= COOP_ARROW_TYPES || e->pad[0] || e->pad[1]) return false;
    if (e->flags != COOP_ARROW_FLY_SHOT && e->flags != COOP_ARROW_FLY_STOP) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f) return false;
    if (e->flags == COOP_ARROW_FLY_STOP) {
        for (int k = 0; k < 3; k++)
            if (e->vel[k] != 0.0f) return false;
        return e->gravity == 0.0f;
    }
    float v2 = 0;
    for (int k = 0; k < 3; k++) {
        if (!isfinite(e->vel[k]) || fabsf(e->vel[k]) > 300.0f) return false;
        v2 += e->vel[k] * e->vel[k];
    }
    return v2 > 1.0f && isfinite(e->gravity) && e->gravity >= -50.0f && e->gravity <= 0.0f;
}
bool coop_trunk_ok(const CoopTrunk *e) {
    if (!e->hash || !e->area || !e->gen || (e->flags & ~(COOP_TRUNK_REST | COOP_TRUNK_GONE)) || e->pad) return false;
    if (e->flags & COOP_TRUNK_GONE) return true;  // (gone: no pose needed)
    float qn = 0;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f || !isfinite(e->vel[k]) || fabsf(e->vel[k]) > 50.0f)
            return false;
    for (int k = 0; k < 4; k++) {
        if (!isfinite(e->rot[k])) return false;
        qn += e->rot[k] * e->rot[k];
    }
    return qn > 0.9f && qn < 1.1f;
}
_Static_assert(sizeof(CoopWildRoster) == 116 && sizeof(CoopWildRoster) <= COOP_EVENT_DATA, "wildlife roster wire size");
_Static_assert(sizeof(CoopWildPoll) == 28, "wildlife poll wire size");
_Static_assert(sizeof(CoopWildReady) == offsetof(CoopWildRoster, center), "wildlife acknowledgement prefix");
const char *coop_wild_name(uint32_t species) {
    static const char *const names[] = {NULL,
        "Animal_Insect_A", "Animal_Insect_AA", "Animal_Insect_AB", "Animal_Insect_B",
        "Animal_Insect_C", "Animal_Insect_E", "Animal_Insect_EP", "Animal_Insect_F",
        "Animal_Insect_G", "Animal_Insect_H", "Animal_Insect_I", "Animal_Insect_K",
        "Animal_Insect_KimoP", "Animal_Insect_M", "Animal_Insect_N", "Animal_Insect_O",
        "Animal_Insect_P", "Animal_Insect_Q", "Animal_Insect_R", "Animal_Insect_S",
        "Animal_Insect_T", "Animal_Insect_X", "Animal_Insect_Z"};
    return species < sizeof names / sizeof *names ? names[species] : NULL;
}
bool coop_wild_poll_ok(const CoopWildPoll *p) {
    if (!p->area || !p->token) return false;
    for (int i = 0; i < COOP_WILD_MAX; i++) {
        if (!p->caught[i]) continue;
        if (!p->previous) return false;
        for (int j = 0; j < i; j++) if (p->caught[i] == p->caught[j]) return false;
    }
    return true;
}
bool coop_wild_roster_ok(const CoopWildRoster *r) {
    if (!r->area || !r->token) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(r->center[k]) || fabsf(r->center[k]) > 100000) return false;
    float last = 0;
    for (int i = 0; i < COOP_WILD_MAX; i++) {
        const CoopWildAnimal *a = &r->animals[i];
        if (!a->id) {
            static const CoopWildAnimal zero;
            if (memcmp(a, &zero, sizeof zero)) return false;
            continue;
        }
        if (!coop_wild_name(a->species) || !isfinite(a->yaw) || fabsf(a->yaw) > 3.142f ||
            (i && !r->animals[i-1].id)) return false;
        for (int j = 0; j < i; j++) if (r->animals[j].id == a->id) return false;
        float d = 0;
        for (int k = 0; k < 3; k++) {
            float delta = a->pos[k] - r->center[k];
            if (!isfinite(delta) || fabsf(a->pos[k]) > 100000) return false;
            d += delta * delta;
        }
        if (d > 24.0f * 24.0f || d < last) return false;
        last = d;
    }
    return true;
}
// (80) Species shared by the fauna snapshot: the 1.9.0 AreaData Bird, Fish and Insect lists (Ecosystem/AreaData.sbyml)
// plus Animal_Insect_Z. Wire indices: append only.
static const char *const fauna_names[] = {NULL,
    "Animal_Crow_A", "Animal_Gull_A", "Animal_Hawk_A", "Animal_Heron_A", "Animal_Heron_B",
    "Animal_LittleBird_A", "Animal_LittleBird_B", "Animal_LittleBird_C", "Animal_LittleBird_D",
    "Animal_LittleBird_E", "Animal_LittleBird_F", "Animal_Pigeon_A", "Animal_Pigeon_B",
    "Animal_Ptarmigan_A", "Animal_Ptarmigan_B", "Animal_WildDuck_A",
    "Animal_Fish_A", "Animal_Fish_B", "Animal_Fish_C", "Animal_Fish_D", "Animal_Fish_E", "Animal_Fish_F",
    "Animal_Fish_G", "Animal_Fish_H", "Animal_Fish_I", "Animal_Fish_J", "Animal_Fish_K", "Animal_Fish_L",
    "Animal_Fish_M", "Animal_Fish_X",
    "Animal_Insect_A", "Animal_Insect_AA", "Animal_Insect_AB", "Animal_Insect_B", "Animal_Insect_C",
    "Animal_Insect_E", "Animal_Insect_EP", "Animal_Insect_F", "Animal_Insect_G", "Animal_Insect_H",
    "Animal_Insect_I", "Animal_Insect_K", "Animal_Insect_KimoP", "Animal_Insect_M", "Animal_Insect_N",
    "Animal_Insect_O", "Animal_Insect_P", "Animal_Insect_Q", "Animal_Insect_R", "Animal_Insect_S",
    "Animal_Insect_T", "Animal_Insect_X", "Animal_Insect_Z"};
#define FAUNA_SPECIES (int)(sizeof fauna_names / sizeof *fauna_names)
const char *coop_fauna_name(uint32_t species) {
    return species < (uint32_t)FAUNA_SPECIES ? fauna_names[species] : NULL;
}
int coop_fauna_species(const char *name) {
    if (!name || strncmp(name, "Animal_", 7)) return 0;
    for (int i = 1; i < FAUNA_SPECIES; i++)
        if (!strcmp(name, fauna_names[i])) return i;
    return 0;
}
bool coop_fauna_ok(const CoopFauna *f) {
    if (!f->area || f->count > COOP_FAUNA_MAX) return false;
    for (int k = 0; k < (int)sizeof f->pad; k++)
        if (f->pad[k]) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(f->center[k]) || fabsf(f->center[k]) > 100000) return false;
    for (int i = 0; i < COOP_FAUNA_MAX; i++) {
        const CoopFaunaAnimal *a = &f->a[i];
        if (i >= f->count) {
            static const CoopFaunaAnimal zero;
            if (memcmp(a, &zero, sizeof zero)) return false;
            continue;
        }
        if (!a->id || !coop_fauna_name(a->species) || (a->flags & ~(COOP_FAUNA_PLACED | COOP_FAUNA_FLED))) return false;
        float d = 0;
        for (int k = 0; k < 3; k++) {
            float delta = a->pos[k] - f->center[k];
            if (!isfinite(a->pos[k]) || fabsf(a->pos[k]) > 100000) return false;
            d += delta * delta;
        }
        if (d > COOP_FAUNA_R * COOP_FAUNA_R) return false;
        for (int j = 0; j < i; j++)
            if (f->a[j].id == a->id && f->a[j].flags == a->flags) return false;
    }
    return true;
}
bool coop_fauna_gone_ok(const CoopFaunaGone *e) {
    if (!e->area || !e->id || !coop_fauna_name(e->species) || (e->flags & ~COOP_FAUNA_PLACED) || e->pad ||
        (e->why != COOP_FAUNA_CAUGHT && e->why != COOP_FAUNA_KILLED)) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000) return false;
    return true;
}
bool coop_fire_ok(const CoopFire *e) {
    if (!e->area || e->on > 1 || e->flags || e->pad[0] || e->pad[1]) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f) return false;
    return true;
}
bool coop_grassfire_ok(const CoopGrassFire *e) {
    if (!e->area) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f) return false;
    return true;
}
_Static_assert(sizeof(CoopLightning) == 24 && sizeof(CoopLightning) <= COOP_EVENT_DATA, "co-op lightning layout (88)");
bool coop_lightning_ok(const CoopLightning *e) {
    if (!e->area || !e->at_ns) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f) return false;
    return true;
}
_Static_assert(sizeof(CoopWaterPuz) == 52 && sizeof(CoopWaterPuz) <= COOP_EVENT_DATA, "co-op waterpuz layout (86)");
bool coop_waterpuz_ok(const CoopWaterPuz *e) {
    if (!e->hash || !e->area || e->kind < COOP_WP_WHEEL || e->kind > COOP_WP_KIND_MAX || (e->flags & ~COOP_WP_FLAGS))
        return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f || !isfinite(e->val[k])) return false;
    for (int k = 0; k < 4; k++)
        if (!isfinite(e->rot[k]) || fabsf(e->rot[k]) > 1.01f) return false;
    if (e->kind == COOP_WP_WHEEL) {
        float qn = 0;
        for (int k = 0; k < 4; k++) qn += e->rot[k] * e->rot[k];
        if (qn < 0.9f || qn > 1.1f) return false;
        for (int k = 0; k < 3; k++)
            if (fabsf(e->val[k]) > 50.0f) return false;
        return true;
    }
    if (e->rot[0] != 0.0f || e->rot[1] != 0.0f || e->rot[2] != 0.0f || e->rot[3] != 1.0f) return false;
    if (e->kind == COOP_WP_WATER) {
        return e->pos[0] == 0.0f && e->pos[2] == 0.0f && fabsf(e->val[0]) <= 100000.0f &&
               e->val[1] >= 0.0f && e->val[1] <= 100.0f && e->val[2] == 0.0f;
    }
    // Timers: COOP_WP_TIMER, COOP_WP_LIMITED, COOP_WP_TIMELAG
    return e->pos[0] == 0.0f && e->pos[1] == 0.0f && e->pos[2] == 0.0f &&
           e->val[0] >= 0.0f && e->val[0] <= 10000.0f &&
           e->val[1] > 0.0f && e->val[1] <= 10000.0f && e->val[2] == 0.0f;
}
_Static_assert(sizeof(CoopBeam) == 40 && sizeof(CoopBeam) <= COOP_EVENT_DATA, "co-op beam layout (90)");
bool coop_beam_ok(const CoopBeam *e) {
    if (!e->area || e->what < COOP_BEAM_SHOT || e->what > COOP_BEAM_AIM_END || e->kind >= COOP_BEAM_KINDS || e->pad ||
        e->target < -1 || e->target >= COOP_MAX_PLAYERS) return false;
    for (int k = 0; k < 3; k++) {
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f || !isfinite(e->aim[k])) return false;
        if (fabsf(e->aim[k]) > (e->target >= 0 ? 20.0f : 100000.0f)) return false;
    }
    if (!isfinite(e->speed)) return false;
    return e->what == COOP_BEAM_SHOT ? e->speed >= 1.0f && e->speed <= 1000.0f : e->speed == 0.0f;
}
_Static_assert(sizeof(CoopMapMark) == 16 && sizeof(CoopMapMark) <= COOP_EVENT_DATA, "co-op map marker layout (95)");
bool coop_mapmark_ok(const CoopMapMark *e) {
    if ((e->what != COOP_MAPMARK_ADD && e->what != COOP_MAPMARK_DEL) || e->pad[0] || e->pad[1]) return false;
    if (!((e->no >= 27 && e->no <= 35) || (e->no >= 37 && e->no <= 41))) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 10000.0f) return false;
    return true;
}
bool coop_critter_ok(const CoopCritter *e) {
    if (!e->hash || !e->area) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f) return false;
    return true;
}
bool coop_chest_shut_ok(const CoopChestShut *e) {
    if (!e->hash || !e->area || e->flags) return false;
    for (int k = 0; k < 3; k++)
        if (!isfinite(e->pos[k]) || fabsf(e->pos[k]) > 100000.0f) return false;
    return true;
}
bool coop_ice_ok(const CoopIce *e) {
    if (!(e->id & COOP_ICE_ID) || !e->area || e->pad[0] || e->pad[1] || e->pad[2]) return false;
    if (e->kind == COOP_ICE_GONE) return true;  // (the matrix is not used)
    if (e->kind != COOP_ICE_CREATE) return false;
    for (int r = 0; r < 3; r++) {
        float l = 0;
        for (int k = 0; k < 3; k++) {
            float v = e->m[4 * r + k];
            if (!isfinite(v) || fabsf(v) > 1.01f) return false;
            l += v * v;
        }
        if (l < 0.9f || l > 1.1f) return false;
        if (!isfinite(e->m[4 * r + 3]) || fabsf(e->m[4 * r + 3]) > 100000) return false;
    }
    return true;
}
bool coop_areafx_ok(const CoopAreaFx *e) {
    if (e->kind != COOP_AREAFX_ARROW && e->kind != COOP_AREAFX_BOMB && e->kind != COOP_AREAFX_THROW &&
        e->kind != COOP_AREAFX_FOE_ARROW && e->kind != COOP_AREAFX_FOE_SHOT)
        return false;
    bool arrow = e->kind == COOP_AREAFX_ARROW || e->kind == COOP_AREAFX_FOE_ARROW;
    bool shot_bad = (e->flags & COOP_SHOT_CLASS) >= COOP_SHOT_TYPES || (e->flags & ~(COOP_SHOT_CLASS | COOP_SHOT_KNOCK));
    if ((arrow ? e->flags >= COOP_ARROW_TYPES : e->kind == COOP_AREAFX_FOE_SHOT ? shot_bad : e->flags != 0) ||
        e->damage < 0 || e->damage > 400)
        return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(e->pos[i]) || fabsf(e->pos[i]) > 100000 || !isfinite(e->dir[i]) || fabsf(e->dir[i]) > 1.5f) return false;
    return isfinite(e->radius) && e->radius >= 0 && e->radius <= 20;
}
bool coop_drop_ok(const CoopDrop *d) {
    if (!(d->id & COOP_DROP_ID) || d->life < -1 || d->life > 10000000 || !isfinite(d->yaw) || fabsf(d->yaw) > 720)
        return false;
    if ((d->flags & ~(COOP_DROP_F_MOVING | COOP_DROP_F_UPDATE)) || d->pad[0] || d->pad[1] || d->pad[2]) return false;  // (34)
    for (int i = 0; i < 3; i++)
        if (!isfinite(d->pos[i]) || fabsf(d->pos[i]) > 100000) return false;
    // ((33) also insects / frogs / lizards and Courser Bee Honey: materials without the Item_ prefix)
    if (strncmp(d->name, "Weapon_", 7) && strncmp(d->name, "Item_", 5) && strncmp(d->name, "Obj_", 4) &&
        strncmp(d->name, "Animal_Insect_", 14) && strcmp(d->name, "BeeHome"))
        return false;
    for (int i = 0; i < COOP_DROP_NAME; i++) {
        char ch = d->name[i];
        if (!ch) return i > 0;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return false;  // (not terminated)
}
bool coop_drop_gone_ok(const CoopDropGone *g) {
    return (g->id & COOP_DROP_ID) && (g->reason == COOP_DROP_PICKED || g->reason == COOP_DROP_VANISHED) && !g->pad[0] &&
           !g->pad[1] && !g->pad[2];
}
// (89) the Champion bits: the shield count only with COOP_CHAMP_COUNT (1..3), nothing without a valid tree.
bool coop_champ_ok(uint16_t c, uint8_t tree_flags) {
    unsigned n = (c & COOP_CHAMP_COUNT_MASK) >> COOP_CHAMP_COUNT_SHIFT;
    if (!(tree_flags & COOP_ASTREE_VALID)) return !c;
    return (c & COOP_CHAMP_COUNT) ? n >= 1 : !n;
}
bool coop_astree_ok(const CoopAsTree *t) {
    if (t->flags & ~(COOP_ASTREE_VALID | COOP_ASTREE_FX) || t->charge_lv > 2) return false;
    if (!coop_champ_ok(t->champ, t->flags) || t->eat > COOP_EAT_MAX || (t->fx_mask >> COOP_FX_PROPS) || (!(t->flags & COOP_ASTREE_FX) && t->fx_mask)) return false;
    for (int i = 0; i < COOP_FX_PROPS; i++) {
        float f;
        memcpy(&f, &t->fx[i], 4);
        int32_t v = (int32_t)t->fx[i];
        if (!((isfinite(f) && fabsf(f) <= 1000000.0f) || (v >= -100000 && v <= 100000))) return false;
        if (!(t->fx_mask >> i & 1) && t->fx[i]) return false;
    }
    for (int i = 0; i < COOP_ASIN_FLOATS; i++)
        if (!isfinite(t->f[i]) || fabsf(t->f[i]) > 100000) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(t->normal[i]) || fabsf(t->normal[i]) > 2 || !isfinite(t->vel[i]) || fabsf(t->vel[i]) > 20) return false;
    if (t->charge < -1000 || t->charge > 1000 || t->combo < -1000 || t->combo > 1000) return false;
    for (int i = 0; i < COOP_ASIN_STRS; i++)
        if (t->str[i] >= COOP_AS_STRING_COUNT) return false;
    for (int i = 0; i < COOP_AS_SLOTS * COOP_AS_LAYERS; i++)
        if (t->as[i] >= COOP_AS_NAME_COUNT) return false;
    return true;
}

// ---- (58) enemies of every link. The game's calls (coop_net_remote_*) see one merged "other side": damage summed over
// the links, the position / flags of the link that runs the enemy (the owner the host's game set in its own entry,
// else the freshest). The _of calls read one player's link.
static int link_find(const WorldLink *w, uint32_t hash) {
    for (int i = 0; i < w->n; i++)
        if (w->en[i].hash == hash) return i;
    return -1;
}
static int local_owner(uint32_t hash) {  // (lock held) the owner the host's game wrote in its entry (-1: none)
    for (int i = 0; i < local_enemy_count; i++)
        if (local_enemies[i].hash == hash) {
            uint32_t f = local_enemies[i].flags;
            int own = (int)((f & COOP_ENEMY_OWNER_MASK) >> COOP_ENEMY_OWNER_SHIFT);
            return own ? own : (f & COOP_ENEMY_GUEST_RUNS) && !(f & COOP_ENEMY_COPY) ? partner_pid : 0;
        }
    return -1;
}
static int pid_link(int pid) {  // (lock held) the link holding player pid's world data (-1: none)
    if (host_mode) return pid > 0 && link_on(pid) ? pid : -1;
    return pid == 0 && link_on(0) ? 0 : -1;
}
// (lock held) which link answers for this enemy: pos: by its newest position, else by its newest entry
static int link_for(uint32_t hash, bool pos) {
    int own = host_mode ? local_owner(hash) : -1;
    if (own > 0 && link_on(own)) {
        int k = link_find(&wl[own], hash);
        if (k >= 0 && (pos ? wl[own].pos_at[k] : wl[own].heard[k])) return own;
    }
    int best = -1;
    uint64_t t = 0;
    for (int L = 0; L < COOP_MAX_PLAYERS; L++) {
        if (!link_on(L)) continue;
        int k = link_find(&wl[L], hash);
        uint64_t at = k < 0 ? 0 : pos ? wl[L].pos_at[k] : wl[L].heard[k];
        // (flags: a link that has it loaded comes first, as the partner's did)
        if (!pos && k >= 0 && at && (wl[L].en[k].flags & COOP_ENEMY_LOADED)) at += 1ull << 62;
        if (at > t) t = at, best = L;
    }
    return best;
}
static bool enemy_pos_in(int L, uint32_t hash, float pos[3], float rot[4], double *age_s, char *as, size_t cap) {
    int k = L >= 0 ? link_find(&wl[L], hash) : -1;
    if (k < 0 || !wl[L].pos_at[k]) return false;
    const CoopEnemyDamage *e = &wl[L].en[k];
    memcpy(pos, e->pos, sizeof e->pos);
    memcpy(rot, e->rot, sizeof e->rot);
    *age_s = (plat_now_ns() - wl[L].pos_at[k]) / 1e9;
    if (as && cap) snprintf(as, cap, "%s", e->as);
    return true;
}
static bool enemy_flags_in(int L, uint32_t hash, uint32_t *flags, double *age_s) {
    int k = L >= 0 ? link_find(&wl[L], hash) : -1;
    if (k < 0 || !wl[L].heard[k]) return false;
    *flags = wl[L].en[k].flags;
    *age_s = (plat_now_ns() - wl[L].heard[k]) / 1e9;
    return true;
}
bool coop_net_remote_enemy_pos(uint32_t hash, float pos[3], float rot[4], double *age_s, char *as, size_t cap) {
    pthread_mutex_lock(&lock);
    bool ok = enemy_pos_in(link_for(hash, true), hash, pos, rot, age_s, as, cap);
    pthread_mutex_unlock(&lock);
    return ok;
}
bool coop_net_remote_enemy_flags(uint32_t hash, uint32_t *flags, double *age_s) {
    pthread_mutex_lock(&lock);
    bool ok = enemy_flags_in(link_for(hash, false), hash, flags, age_s);
    pthread_mutex_unlock(&lock);
    return ok;
}
// (lock held) Fresh native statuses from every other world link.
static uint32_t enemy_elem(uint32_t hash, int except, uint64_t now) {
    uint32_t elem = 0;
    for (int L = 0; L < COOP_MAX_PLAYERS; L++) {
        if (L == except || !link_on(L)) continue;
        int k = link_find(&wl[L], hash);
        if (k >= 0 && wl[L].heard[k] && now - wl[L].heard[k] < COOP_ENEMY_ELEM_FRESH_NS)
            elem |= wl[L].en[k].flags & COOP_ENEMY_ELEM_MASK;
    }
    return elem;
}
uint32_t coop_net_remote_enemy_elem(uint32_t hash) {
    pthread_mutex_lock(&lock);
    uint32_t elem = enemy_elem(hash, -1, plat_now_ns());
    pthread_mutex_unlock(&lock);
    return elem;
}
bool coop_net_enemy_pos_of(int pid, uint32_t hash, float pos[3], float rot[4], double *age_s) {
    pthread_mutex_lock(&lock);
    bool ok = enemy_pos_in(pid_link(pid), hash, pos, rot, age_s, NULL, 0);
    pthread_mutex_unlock(&lock);
    return ok;
}
bool coop_net_enemy_flags_of(int pid, uint32_t hash, uint32_t *flags, double *age_s) {
    pthread_mutex_lock(&lock);
    bool ok = enemy_flags_in(pid_link(pid), hash, flags, age_s);
    pthread_mutex_unlock(&lock);
    return ok;
}
uint32_t coop_net_enemy_damage_of(int pid, uint32_t hash) {
    pthread_mutex_lock(&lock);
    int L = pid_link(pid), k = L >= 0 ? link_find(&wl[L], hash) : -1;
    uint32_t d = k >= 0 ? wl[L].en[k].damage : 0;
    pthread_mutex_unlock(&lock);
    return d;
}

int coop_net_remote_spawned(CoopEnemyDamage *out, double *heard_age_s, int max) {
    int n = 0;
    pthread_mutex_lock(&lock);
    uint64_t now = plat_now_ns();
    for (int L = 0; L < COOP_MAX_PLAYERS; L++) {
        if (!link_on(L)) continue;
        const WorldLink *w = &wl[L];
        for (int i = 0; i < w->n && n < max; i++)
            if ((w->en[i].flags & COOP_ENEMY_SPAWNED) && w->heard[i]) {
                out[n] = w->en[i];
                heard_age_s[n++] = (now - w->heard[i]) / 1e9;
            }
    }
    pthread_mutex_unlock(&lock);
    return n;
}

// (lock held) the sum of the links' totals, but not link `except`
static uint32_t damage_sum(uint32_t hash, int except) {
    uint64_t d = 0;
    for (int L = 0; L < COOP_MAX_PLAYERS; L++) {
        if (L == except || !link_on(L)) continue;
        int k = link_find(&wl[L], hash);
        if (k >= 0) d += wl[L].en[k].damage;
    }
    return d > 1000000 ? 1000000 : (uint32_t)d;
}
uint32_t coop_net_remote_damage(uint32_t hash) {
    pthread_mutex_lock(&lock);
    uint32_t d = damage_sum(hash, -1);
    pthread_mutex_unlock(&lock);
    return d;
}

static bool finite_pos(const CoopEnemyDamage *e) {
    for (int i = 0; i < 3; i++)
        if (!(e->pos[i] > -100000.0f && e->pos[i] < 100000.0f)) return false;
    for (int i = 0; i < 4; i++)
        if (!(e->rot[i] >= -1.01f && e->rot[i] <= 1.01f)) return false;
    return true;
}

// (lock held) Link L's totals: keep the largest seen per enemy (totals only grow within a session). Positions: keep
// the newest by packet seq (a reordered older packet must not move the enemy back).
static void take_remote_enemies(int L, const CoopEnemyPacket *p) {
    WorldLink *w = &wl[L];
    for (int i = 0; i < p->count && i < COOP_ENEMY_MAX; i++) {
        uint32_t h = p->e[i].hash, d = p->e[i].damage;
        if (!h || d > 1000000) continue;
        int k = link_find(w, h);
        if (k < 0) {
            if (w->n == REMOTE_ENEMIES) {  // full: replace the one not heard of for the longest
                k = 0;
                for (int j = 1; j < w->n; j++)
                    if (w->heard[j] < w->heard[k]) k = j;
            } else {
                k = w->n++;
            }
            w->en[k] = (CoopEnemyDamage){.hash = h};
            w->pos_seq[k] = 0, w->pos_at[k] = 0, w->heard[k] = 0;
        }
        if (!w->heard[k] || (int32_t)(p->seq - w->flags_seq[k]) >= 0) {  // (newest flags win)
            w->en[k].flags = p->e[i].flags & (COOP_ENEMY_POS | COOP_ENEMY_LOADED | COOP_ENEMY_GUEST_RUNS | COOP_ENEMY_SPAWNED |
                                              COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PARTNER | COOP_ENEMY_LOOK_BATTLE |
                                              COOP_ENEMY_MTXPOS | COOP_ENEMY_LOOK_PID | COOP_ENEMY_LOOK_PID_MASK);
            if (!(w->en[k].flags & COOP_ENEMY_LOOK)) w->en[k].flags &= ~(COOP_ENEMY_LOOK_PID | COOP_ENEMY_LOOK_PID_MASK);
            // The host can relay another guest's status even when the enemy is unloaded on the host.
            if (!host_mode || (w->en[k].flags & COOP_ENEMY_LOADED)) w->en[k].flags |= p->e[i].flags & COOP_ENEMY_ELEM_MASK;
            w->flags_seq[k] = p->seq;
            w->heard[k] = plat_now_ns();
        }
        if (d > w->en[k].damage) w->en[k].damage = d;
        if ((p->e[i].flags & COOP_ENEMY_POS) && finite_pos(&p->e[i]) &&
            (!w->pos_at[k] || (int32_t)(p->seq - w->pos_seq[k]) > 0)) {
            memcpy(w->en[k].pos, p->e[i].pos, sizeof p->e[i].pos);
            memcpy(w->en[k].rot, p->e[i].rot, sizeof p->e[i].rot);
            memcpy(w->en[k].as, p->e[i].as, sizeof w->en[k].as);
            w->en[k].as[sizeof w->en[k].as - 1] = 0;  // (always terminated)
            w->pos_seq[k] = p->seq, w->pos_at[k] = plat_now_ns();
        }
    }
}

// ---- (58, host) the enemy packet for guest g: our entries, the other guests' damage added, and where another guest
// runs an enemy, that guest's position (g's copy follows it). Then enemies we do not have but other guests do (two
// guests fighting far from us): their owner is the nearest of those guests (RELAY_MARGIN m nearer to take it over,
// at most every RELAY_HOLD_NS), and g's copy follows the owner.
#define RELAY_MARGIN 6.0f
#define RELAY_HOLD_NS 3000000000ull
#define RELAY_FRESH_NS 3000000000ull  // an entry heard this recently counts (the guest has it loaded now)
#define RELAY_POS_NS 1000000000ull    // a position this recent is passed on
static struct { uint32_t hash; int8_t pid; uint64_t since_ns, used_ns; } relay_own[256];
bool coop_net_set_enemy_generation(uint16_t generation) {
    pthread_mutex_lock(&lock);
    bool changed = enemy_generation != generation;
    if (changed) {
        enemy_generation = generation;
        local_enemy_count = 0;
        for (int i = 0; i < COOP_MAX_PLAYERS; i++) wl[i].n = 0;
        memset(relay_own, 0, sizeof relay_own);
    }
    pthread_mutex_unlock(&lock);
    return changed;
}
static float dist2_pos(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}
static bool link_loaded(int L, uint32_t hash, uint64_t now) {
    int k = link_find(&wl[L], hash);
    return k >= 0 && wl[L].heard[k] && now - wl[L].heard[k] < RELAY_FRESH_NS && (wl[L].en[k].flags & COOP_ENEMY_LOADED);
}
// (lock held) give entry o the position of the enemy as link L runs it (false: none fresh)
static bool relay_pos(CoopEnemyDamage *o, int L, uint64_t now) {
    int k = L >= 0 ? link_find(&wl[L], o->hash) : -1;
    o->flags &= ~(COOP_ENEMY_POS | COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PARTNER | COOP_ENEMY_LOOK_BATTLE | COOP_ENEMY_MTXPOS |
                  COOP_ENEMY_LOOK_PID | COOP_ENEMY_LOOK_PID_MASK);
    if (k < 0 || !wl[L].pos_at[k] || now - wl[L].pos_at[k] > RELAY_POS_NS) return false;
    const CoopEnemyDamage *s = &wl[L].en[k];
    memcpy(o->pos, s->pos, sizeof o->pos);
    memcpy(o->rot, s->rot, sizeof o->rot);
    memcpy(o->as, s->as, sizeof o->as);
    o->flags |= COOP_ENEMY_POS | (s->flags & COOP_ENEMY_MTXPOS);
    // (whom it looks at: only by player id (61), the same for every receiver; the relative LOOK_PARTNER is not passed)
    if ((s->flags & COOP_ENEMY_LOOK) && (s->flags & COOP_ENEMY_LOOK_PID))
        o->flags |= s->flags & (COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_BATTLE | COOP_ENEMY_LOOK_PID | COOP_ENEMY_LOOK_PID_MASK);
    return true;
}
// (lock held) the guest that runs an enemy we do not have (-1: no guest has it loaded)
static int relay_owner(uint32_t hash, uint64_t now) {
    int slot = -1, old = 0;
    for (int i = 0; i < 256; i++) {
        if (relay_own[i].hash == hash && relay_own[i].used_ns) { slot = i; break; }
        if (relay_own[i].used_ns < relay_own[old].used_ns) old = i;
    }
    if (slot < 0) slot = old, relay_own[slot].hash = hash, relay_own[slot].pid = -1, relay_own[slot].since_ns = now;
    relay_own[slot].used_ns = now;
    int cur = relay_own[slot].pid;
    // where it is: the current owner's position, else any guest's
    float at[3];
    bool have_at = false;
    for (int t = -1; t < COOP_MAX_PLAYERS && !have_at; t++) {
        int p = t < 0 ? cur : t;  // (the current owner first)
        int k = p > 0 && link_on(p) ? link_find(&wl[p], hash) : -1;
        if (k >= 0 && wl[p].pos_at[k] && now - wl[p].pos_at[k] < RELAY_FRESH_NS) memcpy(at, wl[p].en[k].pos, 12), have_at = true;
    }
    int best = -1;
    float bd = 1e30f, cd = 1e30f;
    for (int L = 1; L < COOP_MAX_PLAYERS; L++) {
        if (!link_on(L) || !link_loaded(L, hash, now)) continue;
        float d = have_at && views[L].at_ns ? dist2_pos(at, views[L].st.pos) : 1e20f;
        if (d < bd) bd = d, best = L;
        if (L == cur) cd = d;
    }
    bool cur_ok = cur > 0 && link_on(cur) && link_loaded(cur, hash, now);
    int want = cur;
    if (!cur_ok) want = best;
    else if (best != cur && best > 0 && now - relay_own[slot].since_ns > RELAY_HOLD_NS && sqrtf(bd) + RELAY_MARGIN < sqrtf(cd))
        want = best;
    if (want != cur) relay_own[slot].pid = (int8_t)want, relay_own[slot].since_ns = now;
    return want;
}
static int spawn_origin(uint32_t id);
static bool in_packet(const CoopEnemyPacket *q, uint32_t hash) {
    for (int i = 0; i < q->count; i++)
        if (q->e[i].hash == hash) return true;
    return false;
}
// (lock held) host: the enemy entries for guest g (q->count, q->e)
// (n6, ENEMYPICK) Two players: our list's first COOP_ENEMY_MAX entries, as before. With more world guests, the entries
// guest g has loaded (or runs) come first, then the rest: seen in a 3-player test (host with ~25 enemies loaded), an
// Octorok player 1 ran fell out of the host's 16 entries; player 2 then heard nothing of it, took it as "the host does
// not have it" and ran it too (two runners). SAVAGEWIND_COOP_ENEMYPICK=0: the old order.
static bool enemypick_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_ENEMYPICK") && !strcmp(getenv("SAVAGEWIND_COOP_ENEMYPICK"), "0"));
    return on;
}
static bool other_world_guest(int g) {  // (lock held) another world guest than g (and the partner when g is not it)
    for (int L = 1; L < COOP_MAX_PLAYERS; L++)
        if (L != g && link_world(L)) return true;
    return false;
}
static void build_enemies_for(int g, CoopEnemyPacket *q, uint64_t now) {
    q->count = 0;
    bool pick = enemypick_on() && other_world_guest(g);
    int n_local = pick ? local_enemy_count : (local_enemy_count < COOP_ENEMY_MAX ? local_enemy_count : COOP_ENEMY_MAX);
    for (int pass = pick ? 0 : 1; pass < 2; pass++)
    for (int i = 0; i < n_local && q->count < COOP_ENEMY_MAX; i++) {
        CoopEnemyDamage o = local_enemies[i];
        if (pick) {  // (pass 0: what g has loaded or runs; pass 1: the rest)
            int ow = (int)((o.flags & COOP_ENEMY_OWNER_MASK) >> COOP_ENEMY_OWNER_SHIFT);
            bool g_has = ow == g || link_loaded(g, o.hash, now);
            if ((pass == 0) != g_has) continue;
        }
        int owner = (int)((o.flags & COOP_ENEMY_OWNER_MASK) >> COOP_ENEMY_OWNER_SHIFT);
        bool copy = (o.flags & COOP_ENEMY_COPY) != 0;
        if (!owner && !copy && (o.flags & COOP_ENEMY_GUEST_RUNS)) owner = partner_pid;  // (no owner named: the partner)
        o.flags &= ~(COOP_ENEMY_OWNER_MASK | COOP_ENEMY_COPY | COOP_ENEMY_GUEST_RUNS);
        uint64_t sum = (uint64_t)o.damage + damage_sum(o.hash, g);
        o.damage = sum > 1000000 ? 1000000 : (uint32_t)sum;
        if (copy) {
            // our copy of a guest's run-time enemy: to that guest as before; to the others as that guest's spawn
            if (owner != g && owner > 0) {
                int k = link_find(&wl[owner], o.hash);
                o.flags |= COOP_ENEMY_SPAWNED;
                if (k < 0 || !(wl[owner].en[k].flags & COOP_ENEMY_LOADED)) o.flags &= ~COOP_ENEMY_LOADED;
                relay_pos(&o, owner, now);
            }
        } else if (owner == g) {
            o.flags |= COOP_ENEMY_GUEST_RUNS;
        } else if (owner > 0) {
            relay_pos(&o, owner, now);  // (another guest runs it: g's copy follows that one)
        } else if (g != partner_pid && (o.flags & COOP_ENEMY_LOOK_PARTNER)) {
            if (o.flags & COOP_ENEMY_LOOK_PID) o.flags &= ~COOP_ENEMY_LOOK_PARTNER;  // (61: the id says who)
            else o.flags &= ~(COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PARTNER | COOP_ENEMY_LOOK_BATTLE);  // (it looks at our partner)
        }
        o.flags |= enemy_elem(o.hash, g, now);
        q->e[q->count++] = o;
    }
    // Enemies only other guests report: those g has loaded first (whom to follow), then run-time enemies of another
    // guest (g makes a copy), then damage totals.
    for (int pass = 0; pass < 3; pass++)
        for (int L = 1; L < COOP_MAX_PLAYERS && q->count < COOP_ENEMY_MAX; L++) {
            if (L == g || !link_on(L)) continue;
            const WorldLink *w = &wl[L];
            for (int i = 0; i < w->n && q->count < COOP_ENEMY_MAX; i++) {
                const CoopEnemyDamage *s = &w->en[i];
                if (!w->heard[i] || now - w->heard[i] > RELAY_FRESH_NS || in_packet(q, s->hash)) continue;
                bool spawned = (s->flags & COOP_ENEMY_SPAWNED) != 0;
                bool here = link_loaded(g, s->hash, now) && (s->flags & COOP_ENEMY_LOADED);
                if (pass == 0 ? !(here && !spawned) : pass == 1 ? !spawned : !s->damage) continue;
                bool ours = false;  // (in our own list: sent above)
                for (int j = 0; j < local_enemy_count && !ours; j++) ours = local_enemies[j].hash == s->hash;
                if (ours) continue;
                CoopEnemyDamage o = {.hash = s->hash, .damage = damage_sum(s->hash, g)};
                if (spawned) {
                    o.flags = COOP_ENEMY_SPAWNED | (s->flags & COOP_ENEMY_LOADED);
                    relay_pos(&o, L, now);
                } else {
                    int own = relay_owner(s->hash, now);
                    if (own == g) o.flags = COOP_ENEMY_LOADED | COOP_ENEMY_GUEST_RUNS;
                    else if (own > 0) o.flags = COOP_ENEMY_LOADED, relay_pos(&o, own, now);
                }
                o.flags |= enemy_elem(o.hash, g, now);
                q->e[q->count++] = o;
            }
        }
    // run-time enemies' names: every other packet, names of other guests' ones (from the ring) when there are any
    static int name_turn[COOP_MAX_PLAYERS];
    if (q->seq & 1) {
        CoopSpawnName pick[COOP_SPAWN_NAMES];
        int np = 0;
        for (int t = 0; t < REMOTE_NAMES && np < COOP_SPAWN_NAMES; t++) {
            const CoopSpawnName *s = &remote_names[(name_turn[g] + t) % REMOTE_NAMES];
            if (!s->id || spawn_origin(s->id) == g || spawn_origin(s->id) <= 0) continue;
            pick[np++] = *s;
            if (np == COOP_SPAWN_NAMES) name_turn[g] = (name_turn[g] + t + 1) % REMOTE_NAMES;
        }
        if (np) {
            memset(q->names, 0, sizeof q->names);
            memcpy(q->names, pick, sizeof *pick * (size_t)np);
        }
    }
}

// (58) Ids of things made at run time (enemies, drops, ice blocks): bit 31 set by the caller, bit 30 = the host's,
// else bits 26..28 = the guest's player id (two guests' ids never collide); the counter below bit 26.
static int spawn_origin(uint32_t id) {
    if (id & 0x40000000u) return 0;
    int p = (int)((id >> 26) & 7);
    return p ? p : -1;
}
uint32_t coop_net_id_tag(void) {
    pthread_mutex_lock(&lock);
    uint32_t t = host_mode ? 0x40000000u : my_id > 0 ? (uint32_t)my_id << 26 : 0;
    pthread_mutex_unlock(&lock);
    return t;
}
int coop_net_id_origin(uint32_t id) { return spawn_origin(id); }

// (58) The other players whose world is shared with ours, heard of within max_age_s (bit per id).
uint32_t coop_net_world_players(double max_age_s) {
    uint32_t m = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        if (i == my_id || !views[i].at_ns || age_of(views[i].at_ns) > max_age_s) continue;
        bool world = host_mode ? link_world(i) : guest_partner && (i == 0 || guest_world_all);
        if (world) m |= 1u << i;
    }
    pthread_mutex_unlock(&lock);
    return m;
}

// ---- simulated bad network (SAVAGEWIND_COOP_SIM, tests only): received datagrams wait in a small pool until
// their release time; the pool is unordered, so jitter reorders them like a real path does.
static struct { int delay_ms, jitter_ms, loss_pct; bool on; } sim;
#define SIM_SLOTS 256
static struct SimSlot {
    uint64_t release_ns;  // 0: free
    int n;
    struct sockaddr_in from;
    unsigned char wire[MAX_DATAGRAM + 1];  // (the largest datagram kind)
} sim_slot[SIM_SLOTS];
static uint64_t sim_rng = 0x9e3779b97f4a7c15ull;
static uint32_t sim_rand(void) {  // xorshift64*: test traffic only
    sim_rng ^= sim_rng >> 12, sim_rng ^= sim_rng << 25, sim_rng ^= sim_rng >> 27;
    return (uint32_t)((sim_rng * 0x2545f4914f6cdd1dull) >> 32);
}
static uint32_t sim_lost, sim_held;

static void sim_init(void) {
    const char *e = getenv("SAVAGEWIND_COOP_SIM");
    if (!e || !*e) return;
    int v[3] = {0, 0, 0};
    sscanf(e, "%d,%d,%d", &v[0], &v[1], &v[2]);
    sim.delay_ms = v[0] < 0 ? 0 : v[0] > 5000 ? 5000 : v[0];
    sim.jitter_ms = v[1] < 0 ? 0 : v[1] > sim.delay_ms ? sim.delay_ms : v[1];
    sim.loss_pct = v[2] < 0 ? 0 : v[2] > 100 ? 100 : v[2];
    sim.on = sim.delay_ms || sim.loss_pct;
    sim_rng ^= local_session;
    if (sim.on)
        LOG("[coop] simulated network: delay %d +- %d ms, loss %d%% (SAVAGEWIND_COOP_SIM)\n", sim.delay_ms,
            sim.jitter_ms, sim.loss_pct);
}

// Hold a received datagram (or lose it). False when it is to be handled now (simulation off).
static bool sim_hold(const unsigned char *wire, int n, const struct sockaddr_in *from) {
    if (!sim.on) return false;
    if ((int)(sim_rand() % 100) < sim.loss_pct) { sim_lost++; return true; }
    int ms = sim.delay_ms + (sim.jitter_ms ? (int)(sim_rand() % (2u * sim.jitter_ms + 1)) - sim.jitter_ms : 0);
    for (int i = 0; i < SIM_SLOTS; i++) {
        struct SimSlot *s = &sim_slot[i];
        if (s->release_ns) continue;
        s->release_ns = plat_now_ns() + (uint64_t)(ms > 0 ? ms : 0) * 1000000ull + 1;
        s->n = n > (int)sizeof s->wire ? (int)sizeof s->wire : n;
        memcpy(s->wire, wire, (size_t)s->n);
        s->from = *from;
        sim_held++;
        return true;
    }
    sim_lost++;  // pool full: a congested path drops it
    return true;
}

// The earliest release time of a held datagram (0: none).
static uint64_t sim_next(void) {
    uint64_t t = 0;
    for (int i = 0; i < SIM_SLOTS; i++)
        if (sim_slot[i].release_ns && (!t || sim_slot[i].release_ns < t)) t = sim_slot[i].release_ns;
    return t;
}

static void handle_datagram(const unsigned char *wire, int n, struct sockaddr_in from);
static void send_local(uint32_t *seq);
static void send_names(uint64_t now);
static volatile bool stop_req;  // coop_net_stop: the thread ends at its next turn
static pthread_t net_tid;

static void sim_release(void) {
    if (!sim.on) return;
    uint64_t now = plat_now_ns();
    static uint64_t report_ns;
    if (now > report_ns) {
        if (report_ns) LOG("[coop] simulated network: %u delivered late, %u lost so far\n", sim_held, sim_lost);
        report_ns = now + 10000000000ull;
    }
    for (int i = 0; i < SIM_SLOTS; i++) {
        struct SimSlot *s = &sim_slot[i];
        if (!s->release_ns || s->release_ns > now) continue;
        s->release_ns = 0;
        handle_datagram(s->wire, s->n, s->from);
    }
}

static void *net_thread(void *arg) {
    (void)arg;
    uint32_t seq = 0;
    uint64_t next_send = 0;
    while (!stop_req) {
        // wait for a packet, at most until the next send (or the next held packet's release)
        uint64_t now = plat_now_ns(), until = next_send, held = sim_next();
        if (held && held < until) until = held;
        long wait_ms = until > now ? (long)((until - now + 999999) / 1000000) : 0;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = {wait_ms / 1000, (wait_ms % 1000) * 1000};
        if (select((int)sock + 1, &fds, NULL, NULL, &tv) > 0) {
            // An extra byte distinguishes an oversized UDP datagram on POSIX too.
            unsigned char wire[MAX_DATAGRAM + COOP_TAG + 1];  // (the largest kind + its tag; +1 spots an oversized one)
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(sock, (char *)wire, sizeof wire, 0, (struct sockaddr *)&from, &fl);
            int m = n > 0 ? tag_check(wire, n) : -1;
            if (n > 0 && m < 0) {
                // (protocol 53) not our join code: dropped. An older build (no tag) is still told apart by its
                // protocol number (handle_datagram only logs that and returns); anything else gets one line per 10 s.
                code_rejected++;
                uint32_t magic;
                if (n >= 8 && (memcpy(&magic, wire, 4), magic == COOP_MAGIC || magic == COOP_ENEMY_MAGIC ||
                               magic == COOP_EVENT_MAGIC || magic == COOP_OBJ_MAGIC || magic == COOP_ASTREE_MAGIC ||
                               magic == COOP_POSE_MAGIC) && wire[4] != COOP_VERSION && wire[4] < 53)
                    handle_datagram(wire, n, from);
                else {
                    static uint64_t said_ns;
                    uint64_t t = plat_now_ns();
                    if (!said_ns || t - said_ns > 10000000000ull) {
                        said_ns = t;
                        char ip[INET_ADDRSTRLEN] = "?";
                        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                        LOG("[coop] ignored messages from %s:%u: not the same join code (both players must type the "
                            "same code, or both none) (%u so far)\n", ip, (unsigned)ntohs(from.sin_port), code_rejected);
                    }
                }
            } else if (n > 0 && !sim_hold(wire, m, &from)) handle_datagram(wire, m, from);
        }
        sim_release();
        now = plat_now_ns();
        if (now < next_send) continue;
        next_send = now + SEND_INTERVAL_MS * 1000000ull;
        send_local(&seq);
        send_names(now);
    }
    return NULL;
}

// ---- (protocol 56) more than two players (lock held for all of these)
static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
#define CONN_LIVE_NS 5000000000ull    // a guest heard of within this is connected (the partner: not replaced)
#define CONN_FORGET_NS 30000000000ull // stop sends after 30 s; a silent non-partner's id can then be reused
#define CHALLENGE_BUCKET_NS 2000000000ull

static bool conn_recent(int g, uint64_t now) {
    return g > 0 && conn[g].used && conn[g].session && now - conn[g].last_ns <= CONN_FORGET_NS;
}

// Stateless challenge, keyed separately from the room code. The address, UDP port and client session
// must match the original request. Accept two time buckets so a reply can cross a bucket boundary.
static uint64_t challenge_cookie(const struct sockaddr_in *from, uint64_t session, uint64_t bucket) {
    uint64_t data[] = {session, bucket, (uint64_t)from->sin_addr.s_addr << 16 | from->sin_port};
    return siphash24(challenge_key[0], challenge_key[1], (const unsigned char *)data, sizeof data);
}
static bool host_confirm(const struct sockaddr_in *from, const CoopPacket *p, uint64_t now) {
    for (int g = 1; g < COOP_MAX_PLAYERS; g++)
        if (conn_recent(g, now) && conn[g].session == p->session && same_addr(&conn[g].addr, from))
            return p->echo == local_session;

    uint64_t bucket = now / CHALLENGE_BUCKET_NS;
    uint64_t cookie = challenge_cookie(from, p->session, bucket);
    if (p->challenge && (p->challenge == cookie ||
        (bucket && p->challenge == challenge_cookie(from, p->session, bucket - 1)))) return true;
    if (cookie) {
        CoopChallenge reply = {.magic = COOP_CHALLENGE_MAGIC, .version = COOP_VERSION,
                               .session = p->session, .cookie = cookie};
        coop_sendto(sock, &reply, sizeof reply, from);
    }
    return false;
}
// (58) The run-time enemy names a player made (-1: all).
static void names_forget(int pid) {
    for (int i = 0; i < REMOTE_NAMES; i++)
        if (remote_names[i].id && (pid < 0 || spawn_origin(remote_names[i].id) == pid)) memset(&remote_names[i], 0, sizeof remote_names[i]);
}
// All world data (enemies, objects, run-time names, events): a guest's new host session or role; the host's new
// partner without MULTIWORLD (as with two players).
static void world_reset(void) {
    for (int L = 0; L < COOP_MAX_PLAYERS; L++) link_reset(L);  // (a restarted peer starts its totals from 0)
    for (int L = 0; L < COOP_MAX_PLAYERS; L++) ol[L].n = 0;
    names_forget(-1);
    in_count = 0;  // (event ids start again at 1 for each pair of sessions)
}
// host: guest pid is new or restarted: its world data starts again (two players: everything, as before)
static void link_new_session(int pid) {
    bool others = false;
    for (int g = 1; g < COOP_MAX_PLAYERS; g++) others |= g != pid && link_world(g);
    if (!others) { world_reset(); return; }
    link_reset(pid);
    names_forget(pid);
    ol[pid].n = 0;  // (59: every world guest's objects, its own table)
}
static void retire(uint64_t session) {
    if (!session || session_retired(session)) return;
    Retired *r = malloc(sizeof *r);
    if (r) *r = (Retired){session, retired}, retired = r;
}
// host: the ids connected now (bit 0: the host)
static uint8_t players_now(uint64_t now) {
    uint8_t m = 1;
    for (int g = 1; g < COOP_MAX_PLAYERS; g++)
        if (conn[g].used && conn[g].session && now - conn[g].last_ns <= CONN_LIVE_NS) m |= (uint8_t)(1u << g);
    return m;
}
// host: send relayed datagrams of player `origin` to guest g?
static bool relay_target(int g, int origin) {
    return g != origin && conn_recent(g, plat_now_ns());
}
// (60, host, lock held) Relay rate by distance: is this datagram of `origin` (kind 0 state, 1 AS tree, 2 pose) to
// guest g due? A guest far from that player (another area, or farther than COOP_RELAY_FAR_M; both positions known)
// gets the state at most every RELAY_FAR_STATE_NS and the tree / pose every RELAY_FAR_ANIM_NS: it does not draw
// that player's figure up close. SAVAGEWIND_COOP_RELAYRATE=0: everything relayed (as before 60).
#define RELAY_FAR_STATE_NS 95000000ull
#define RELAY_FAR_ANIM_NS 240000000ull
static uint64_t relay_last_ns[3][COOP_MAX_PLAYERS][COOP_MAX_PLAYERS];
static unsigned relay_skipped;  // (tests / log)
static int relayrate_c = -1;
static bool relay_far(int g, int origin) {
    const PlayerView *a = &views[origin], *b = &views[g];
    if (!a->at_ns || !b->at_ns) return false;
    if (a->st.area != b->st.area) return true;
    float d2 = 0;
    for (int k = 0; k < 3; k++) d2 += (a->st.pos[k] - b->st.pos[k]) * (a->st.pos[k] - b->st.pos[k]);
    return d2 > COOP_RELAY_FAR_M * COOP_RELAY_FAR_M;
}
static bool relay_due(int kind, int g, int origin, uint64_t now) {
    if (relayrate_c < 0) relayrate_c = !(getenv("SAVAGEWIND_COOP_RELAYRATE") && !strcmp(getenv("SAVAGEWIND_COOP_RELAYRATE"), "0"));
    if (!relayrate_c || kind < 0 || kind > 2 || g < 0 || g >= COOP_MAX_PLAYERS || origin < 0 || origin >= COOP_MAX_PLAYERS)
        return true;
    uint64_t *last = &relay_last_ns[kind][origin][g];
    if (relay_far(g, origin) && *last && now - *last < (kind ? RELAY_FAR_ANIM_NS : RELAY_FAR_STATE_NS)) {
        relay_skipped++;
        return false;
    }
    *last = now;
    return true;
}
// The player a tree / pose datagram describes (-1: not taken). Host: the guest it came from (its connection and
// session). Guest: from the host, its session, echoing ours: the origin it names (0 = the host itself).
static int sender_of(const struct sockaddr_in *from, uint64_t session, uint64_t echo, int origin) {
    if (echo != local_session) return -1;
    if (host_mode) {
        for (int g = 1; g < COOP_MAX_PLAYERS; g++)
            if (conn_recent(g, plat_now_ns()) && conn[g].session == session && same_addr(&conn[g].addr, from)) return g;
        return -1;
    }
    if (!have_peer || !same_addr(from, &peer) || session != peer_session) return -1;
    return origin >= 0 && origin < COOP_MAX_PLAYERS && origin != my_id ? origin : -1;
}
// (58) The link a world datagram (enemies, events) belongs to (-1: not taken). Host: the guest it came from, if that
// guest shares the world. Guest: the host, its session, echoing ours.
static int world_link_from(const struct sockaddr_in *from, uint64_t session, uint64_t echo) {
    if (echo != local_session) return -1;
    if (host_mode) {
        for (int g = 1; g < COOP_MAX_PLAYERS; g++)
            if (conn_recent(g, plat_now_ns()) && link_world(g) && conn[g].session == session &&
                same_addr(&conn[g].addr, from)) return g;
        return -1;
    }
    return have_peer && same_addr(from, &peer) && session == peer_session ? 0 : -1;
}
// host: a valid state packet from `from`: the guest's id (a new guest gets the lowest free one; -1: no room or a
// retired session). The first guest is the partner; another becomes the partner only after the partner was silent
// for 5 s (the old partner keeps its id: back, it is an extra guest).
static int host_admit(const struct sockaddr_in *from, const CoopPacket *p, uint64_t now) {
    uint64_t session = p->session;
    for (int g = 1; g < COOP_MAX_PLAYERS; g++)  // (an extra guest silent for long: its id is free again)
        if (conn[g].used && g != partner_pid && now - conn[g].last_ns > CONN_FORGET_NS) {
            LOG("[coop] player %d left (silent for 30 s)\n", g);
            retire(conn[g].session);
            conn[g].used = false;
            view_reset(g);
            link_reset(g), names_forget(g);  // (58)
        }
    int pid = -1;
    for (int g = 1; g < COOP_MAX_PLAYERS && pid < 0; g++)
        if (conn[g].used && same_addr(&conn[g].addr, from)) pid = g;
    for (int g = 1; g < COOP_MAX_PLAYERS; g++) {
        if (!conn[g].used || conn[g].session != session || g == pid) continue;
        if (pid >= 0) return -1;  // never let two slots represent the same session
        pid = g, conn[g].addr = *from, conn[g].last_ns = now;  // the guest's router changed its mapping
        break;
    }
    if (pid < 0) {
        for (int g = 1; g < COOP_MAX_PLAYERS && pid < 0; g++)
            if (!conn[g].used) pid = g;
        if (pid < 0) {
            static uint64_t said_ns;
            if (!said_ns || now - said_ns > 10000000000ull)
                said_ns = now, LOG("[coop] %s:%d wants to join: already %d players, no room\n", inet_ntoa(from->sin_addr),
                                   ntohs(from->sin_port), COOP_MAX_PLAYERS);
            return -1;
        }
        conn[pid].used = true, conn[pid].addr = *from, conn[pid].session = session, conn[pid].last_ns = now;
        conn[pid].restarted_ns = 0;
        view_reset(pid);
        link_reset(pid), names_forget(pid);  // (58: a new guest's world data starts empty)
        LOG("[coop] player %d joined from %s:%d\n", pid, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    } else if (conn[pid].session != session) {  // (the guest restarted: same address, new session)
        if (conn[pid].restarted_ns && now - conn[pid].restarted_ns < 2000000000ull) return -1;
        retire(conn[pid].session);
        conn[pid].session = session, conn[pid].last_ns = conn[pid].restarted_ns = now;
        view_reset(pid);
        if (pid == partner_pid) peer_session = session;
        if (link_world(pid)) link_new_session(pid);  // (58: two players: world_reset, as before)
        LOG("[coop] peer %s:%d new session (player %d)\n", inet_ntoa(from->sin_addr), ntohs(from->sin_port), pid);
    }
    if (!conn_recent(pid, now) || !views[pid].at_ns || (int32_t)(p->seq - views[pid].seq) > 0)
        conn[pid].last_ns = now;
    bool partner_live = partner_pid > 0 && conn[partner_pid].used && now - conn[partner_pid].last_ns <= CONN_LIVE_NS;
    if (pid != partner_pid && !partner_live) {
        if (partner_pid > 0) LOG("[coop] player %d is silent: player %d is the world partner now\n", partner_pid, pid);
        else LOG("[coop] player %d is the world partner\n", pid);
        bool was_world = link_world(pid);
        partner_pid = pid;
        peer_session = session;
        // (58) with MULTIWORLD the new partner already shared the world: its data stays (59: its objects too, they
        // are kept per guest now); without it, everything starts again as before
        if (!(multiworld_on() && was_world)) world_reset();
    }
    if (pid == partner_pid) peer = *from, have_peer = true;
    return pid;
}
// guest: what the host says in its own state packet: our id, whether we are its world partner, who is connected.
static void guest_take_host_info(const CoopPacket *p) {
    if (p->your_id && p->your_id != my_id) {
        LOG("[coop] we are player %d\n", p->your_id);
        my_id = p->your_id;
        view_reset(my_id);
    }
    // (58) COOP_ROLE_WORLD: every guest shares the host's world (unless our own SAVAGEWIND_COOP_MULTIWORLD=0); the
    // host is then our world partner, as for its first guest
    bool all = (p->role & COOP_ROLE_WORLD) && multiworld_on();
    bool part = (p->role & COOP_ROLE_PARTNER) || all;
    guest_world_all = all;
    if (part != guest_partner) {
        guest_partner = part;
        partner_pid = part ? 0 : -1;
        world_reset();
        LOG("[coop] %s\n", !part ? "joined as an extra player: everyone's figure is shown, the world is our own"
                           : (p->role & COOP_ROLE_PARTNER) ? "the host's world partner now: enemies, items and the rest are shared with the host"
                                                           : "joined as another player: enemies, items and the rest are shared with everyone");
    }
    players_mask = p->players;
}

// ---- (protocol 62) names and ping
// (DISHINV) UTF-8 names (coop_net.h coop_name_ok)
int coop_utf8_char(const char *str, int avail, uint32_t *cp) {
    const unsigned char *s = (const unsigned char *)str;
    if (avail < 1) return 0;
    uint32_t c = s[0];
    int n;
    if (c < 0x80) n = 1;
    else if (c >= 0xc2 && c <= 0xdf) n = 2, c &= 0x1f;  // (0xc0 / 0xc1: always overlong)
    else if (c >= 0xe0 && c <= 0xef) n = 3, c &= 0x0f;
    else if (c >= 0xf0 && c <= 0xf4) n = 4, c &= 0x07;
    else return 0;
    if (n > avail) return 0;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        c = c << 6 | (s[i] & 0x3fu);
    }
    if ((n == 3 && c < 0x800) || (n == 4 && (c < 0x10000 || c > 0x10ffff)) || (c >= 0xd800 && c <= 0xdfff)) return 0;
    if (c < 0x20 || (c >= 0x7f && c < 0xa0)) return 0;  // (C0, DEL, C1)
    if (cp) *cp = c;
    return n;
}
bool coop_name_ok(const char *s) {
    int i = 0;
    while (i < COOP_NAME_LEN && s[i]) {
        int n = coop_utf8_char(s + i, COOP_NAME_LEN - 1 - i, NULL);  // (the NUL still fits after it)
        if (!n) return false;
        i += n;
    }
    return i < COOP_NAME_LEN;  // (terminated inside the field)
}
void coop_name_clean(const char *name, char out[COOP_NAME_LEN]) {
    memset(out, 0, COOP_NAME_LEN);
    int k = 0;
    while (name && (*name == ' ' || *name == '\t')) name++;
    while (name && *name) {
        int avail = (int)strnlen(name, 4);
        int n = coop_utf8_char(name, avail, NULL);
        const char *ch = name;
        if (n) name += n;
        else {  // (one '?' for a control character or a broken sequence and its stray continuation bytes)
            ch = "?", n = 1, name++;
            while ((*name & 0xc0) == 0x80) name++;
        }
        if (k + n > COOP_NAME_LEN - 1) break;  // (cut at a character boundary)
        memcpy(out + k, ch, (size_t)n), k += n;
    }
    while (k && out[k - 1] == ' ') out[--k] = 0;
}
static bool name_text_ok(const char *s) { return coop_name_ok(s); }
static bool coop_name_packet_ok(const CoopNamePacket *p) {
    if (p->magic != COOP_NAME_MAGIC || p->version != COOP_VERSION || p->count > COOP_MAX_PLAYERS || !p->session ||
        p->pad[0] || p->pad[1] || p->pad2)
        return false;
    for (int i = 0; i < p->count; i++) {
        const CoopNameEntry *e = &p->e[i];
        if ((e->pid >= COOP_MAX_PLAYERS && e->pid != 0xff) || e->tint >= COOP_TINT_COUNT || !name_text_ok(e->name)) return false;
    }
    return true;
}
static int rtt_ms(const CoopNamePacket *p, uint64_t now) {
    if (!p->echo_t_ns || p->echo_t_ns > now) return -1;
    uint64_t d = now - p->echo_t_ns, hold = (uint64_t)p->echo_hold_us * 1000ull;
    if (hold > d || d - hold > 10000000000ull) return -1;
    return (int)((d - hold) / 1000000ull);
}
// (lock held) Host: a guest's own name (its first entry; the id is the one we gave its connection, never what it
// claims). Guest: from the host, every player's name and the pings the host measured.
static void update_peer_name(int pid, const char *name, uint64_t now) {
    if (!strcmp(names[pid], name) || (name_changed_ns[pid] && now - name_changed_ns[pid] < 500000000ull)) return;
    memcpy(names[pid], name, COOP_NAME_LEN);
    name_changed_ns[pid] = now;
    LOG("[coop] player %d is called \"%s\"\n", pid, names[pid]);
}

static void update_peer_tint(int pid, uint8_t tint) {
    if (tints[pid] == tint) return;
    tints[pid] = tint;
    LOG("[coop] player %d's tunic colour: %d\n", pid, (int)tint);
}

static void take_names(const CoopNamePacket *p, const struct sockaddr_in *from) {
    uint64_t now = plat_now_ns();
    if (p->echo != local_session) return;
    if (host_mode) {
        int g = -1;
        for (int i = 1; i < COOP_MAX_PLAYERS && g < 0; i++)
            if (conn_recent(i, now) && conn[i].session == p->session && same_addr(&conn[i].addr, from)) g = i;
        if (g < 0) return;
        if (p->count >= 1) update_peer_name(g, p->e[0].name, now), update_peer_tint(g, p->e[0].tint);
        int r = rtt_ms(p, now);
        if (r >= 0) pings[g] = r;
        name_t[g] = p->t_ns, name_t_at[g] = now;
        return;
    }
    if (!have_peer || !same_addr(from, &peer) || p->session != peer_session) return;
    int r = rtt_ms(p, now);
    if (r >= 0) pings[0] = r;
    name_t[0] = p->t_ns, name_t_at[0] = now;
    for (int i = 0; i < p->count; i++) {
        const CoopNameEntry *e = &p->e[i];
        if (e->pid >= COOP_MAX_PLAYERS || e->pid == my_id) continue;
        update_peer_name(e->pid, e->name, now);
        update_peer_tint(e->pid, e->tint);
        if (e->pid) pings[e->pid] = e->ping_ms == 0xffff ? -1 : e->ping_ms;
    }
}
// (net thread) Twice a second: a guest sends its name to the host; the host sends each guest the list.
static void send_names(uint64_t now) {
    static uint64_t next_ns;
    if (now < next_ns) return;
    next_ns = now + 500000000ull;
    CoopNamePacket q[COOP_MAX_PLAYERS];
    struct sockaddr_in to[COOP_MAX_PLAYERS];
    int n = 0;
    pthread_mutex_lock(&lock);
    CoopNamePacket b = {0};
    b.magic = COOP_NAME_MAGIC, b.version = COOP_VERSION, b.session = local_session, b.t_ns = now;
    if (host_mode) {
        b.e[b.count].pid = 0, b.e[b.count].ping_ms = 0, b.e[b.count].tint = local_tint;
        memcpy(b.e[b.count].name, local_name, COOP_NAME_LEN), b.count++;
        for (int g = 1; g < COOP_MAX_PLAYERS; g++)
            if (conn_recent(g, now)) {
                CoopNameEntry *e = &b.e[b.count++];
                e->pid = (uint8_t)g, e->ping_ms = pings[g] < 0 ? 0xffff : (uint16_t)(pings[g] > 0xfffe ? 0xfffe : pings[g]);
                memcpy(e->name, names[g], COOP_NAME_LEN);
                e->tint = tints[g];
            }
        for (int g = 1; g < COOP_MAX_PLAYERS; g++)
            if (relay_target(g, -1)) {
                q[n] = b, q[n].echo = conn[g].session;
                q[n].echo_t_ns = name_t[g];
                q[n].echo_hold_us = name_t[g] ? (uint32_t)((now - name_t_at[g]) / 1000ull) : 0;
                to[n++] = conn[g].addr;
            }
    } else if (have_peer && peer_session) {
        b.e[0].pid = (uint8_t)(my_id >= 0 && my_id < COOP_MAX_PLAYERS ? my_id : 0xff);
        b.e[0].ping_ms = 0xffff, b.e[0].tint = local_tint;
        memcpy(b.e[0].name, local_name, COOP_NAME_LEN);
        b.count = 1;
        q[0] = b, q[0].echo = peer_session;
        q[0].echo_t_ns = name_t[0];
        q[0].echo_hold_us = name_t[0] ? (uint32_t)((now - name_t_at[0]) / 1000ull) : 0;
        to[0] = peer, n = 1;
    }
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) coop_sendto(sock, &q[i], (int)sizeof q[i], &to[i]);
}

void coop_net_set_name(const char *name) {
    char c[COOP_NAME_LEN];
    coop_name_clean(name, c);
    pthread_mutex_lock(&lock);
    memcpy(local_name, c, sizeof c);
    pthread_mutex_unlock(&lock);
}

void coop_net_set_tint(int tint) {
    pthread_mutex_lock(&lock);
    local_tint = (uint8_t)(tint > 0 && tint < COOP_TINT_COUNT ? tint : 0);
    pthread_mutex_unlock(&lock);
}
int coop_net_player_tint(int pid) {
    if (pid < 0 || pid >= COOP_MAX_PLAYERS) return 0;
    pthread_mutex_lock(&lock);
    int t = pid == my_id || (host_mode && !pid) ? local_tint : tints[pid];
    pthread_mutex_unlock(&lock);
    return t;
}
bool coop_net_player_name(int pid, char *out, size_t cap) {
    if (!cap) return false;
    out[0] = 0;
    if (pid < 0 || pid >= COOP_MAX_PLAYERS) return false;
    pthread_mutex_lock(&lock);
    snprintf(out, cap, "%s", pid == my_id || (host_mode && !pid) ? local_name : names[pid]);
    pthread_mutex_unlock(&lock);
    return out[0] != 0;
}

int coop_net_lobby(CoopLobbyEntry *out, int max) {
    int n = 0;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&lock);
    if (n < max) {
        CoopLobbyEntry *e = &out[n++];
        memset(e, 0, sizeof *e);
        e->pid = running ? (my_id >= 0 ? my_id : -1) : 0;
        memcpy(e->name, local_name, COOP_NAME_LEN);
        e->ping_ms = host_mode ? 0 : -1, e->age_s = 0, e->me = true, e->host = running && host_mode;
        e->in_world = (local.flags & COOP_FLAG_IN_WORLD) != 0, memcpy(e->pos, local.pos, sizeof e->pos);
    }
    for (int g = 0; running && g < COOP_MAX_PLAYERS && n < max; g++) {
        if (g == my_id) continue;
        bool known = host_mode ? (g > 0 && conn[g].used) : (g == 0 ? have_peer : ((players_mask >> g) & 1) || views[g].at_ns);
        if (!known) continue;
        CoopLobbyEntry *e = &out[n++];
        memset(e, 0, sizeof *e);
        e->pid = g;
        memcpy(e->name, names[g], COOP_NAME_LEN);
        e->ping_ms = pings[g];
        e->age_s = views[g].at_ns ? (now - views[g].at_ns) / 1e9 : -1;
        e->host = g == 0, e->partner = g == partner_pid;
        if (views[g].at_ns) e->in_world = (views[g].st.flags & COOP_FLAG_IN_WORLD) != 0, memcpy(e->pos, views[g].st.pos, sizeof e->pos);
    }
    pthread_mutex_unlock(&lock);
    return n;
}


static void handle_datagram(const unsigned char *wire, int n, struct sockaddr_in from) {
    // A co-op packet of another protocol version (the other PC runs an older or newer build): say so in the log,
    // every 10 s at most - otherwise nothing at all happens and the players cannot tell why.
    uint32_t magic;
    if (n >= 8 && (memcpy(&magic, wire, 4), magic == COOP_MAGIC || magic == COOP_ENEMY_MAGIC || magic == COOP_EVENT_MAGIC || magic == COOP_OBJ_MAGIC || magic == COOP_ASTREE_MAGIC || magic == COOP_POSE_MAGIC || magic == COOP_NAME_MAGIC || magic == COOP_FAUNA_MAGIC) && wire[4] != COOP_VERSION) {
        static uint64_t said_ns;
        uint64_t now = plat_now_ns();
        if (!said_ns || now - said_ns > 10000000000ull) {
            said_ns = now;
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            LOG("[coop] %s:%u runs co-op protocol %u, this build has %u: both PCs need the same co-op build "
                "(send the new SavageWind-Coop.zip and run Co-op Setup.bat again)\n",
                ip, (unsigned)ntohs(from.sin_port), (unsigned)wire[4], (unsigned)COOP_VERSION);
        }
        return;
    }
    if (n == (int)sizeof(CoopChallenge)) {
        CoopChallenge c;
        memcpy(&c, wire, sizeof c);
        if (!host_mode && have_peer && same_addr(&from, &peer) && c.magic == COOP_CHALLENGE_MAGIC &&
            c.version == COOP_VERSION && !c.pad[0] && !c.pad[1] && !c.pad[2] && c.session == local_session)
            guest_challenge = c.cookie;
        return;
    }
    if (n == (int)sizeof(CoopEnemyPacket)) {  // shared enemies: only from the current peer and session
        CoopEnemyPacket ep;
        memcpy(&ep, wire, sizeof ep);
        if (ep.magic != COOP_ENEMY_MAGIC || ep.version != COOP_VERSION || ep.count > COOP_ENEMY_MAX) return;
        pthread_mutex_lock(&lock);
        int L = world_link_from(&from, ep.session, ep.echo);  // (58: the host: from every guest sharing the world)
        if (L >= 0 && link_on(L) && ep.generation == enemy_generation) {
            take_remote_enemies(L, &ep);
            take_remote_names(&ep);
        }
        // (the hour: a guest's comes from the host, also as an extra player; the host keeps its partner's, as before)
        if (L >= 0 && L == (host_mode ? partner_pid : 0) && isfinite(ep.hour) && ep.hour >= 0 && ep.hour <= 24)
            remote_hour = ep.hour, remote_hour_ns = plat_now_ns();
        pthread_mutex_unlock(&lock);
        return;
    }
    if (n == (int)sizeof(CoopObjPacket)) {  // shared objects: (59) from every world link, its current session
        CoopObjPacket op;
        memcpy(&op, wire, sizeof op);
        if (op.magic != COOP_OBJ_MAGIC || op.version != COOP_VERSION || op.count > COOP_OBJ_MAX) return;
        pthread_mutex_lock(&lock);
        int L = world_link_from(&from, op.session, op.echo);
        if (L >= 0 && link_on(L)) take_remote_objects(L, &op);
        pthread_mutex_unlock(&lock);
        return;
    }
    if (n == (int)sizeof(CoopAsTreePacket)) {  // (protocol 29) tree mode: only from the current peer and session
        CoopAsTreePacket ap;
        memcpy(&ap, wire, sizeof ap);
        if (ap.magic != COOP_ASTREE_MAGIC || ap.version != COOP_VERSION || !coop_astree_ok(&ap.t)) return;
        pthread_mutex_lock(&lock);
        int pid = sender_of(&from, ap.session, ap.echo, ap.origin);  // (56: a host relays other guests' trees)
        PlayerView *v = pid >= 0 ? &views[pid] : NULL;
        uint64_t now = plat_now_ns();
        if (v && (!v->astree_ns || (int32_t)(ap.seq - v->astree_seq) > 0 || now - v->astree_ns > 1000000000ull)) {
            v->astree = ap.t;
            v->astree_seq = ap.seq, v->astree_ns = now;
            if (host_mode) {  // (56) on to the other guests
                for (int g = 1; g < COOP_MAX_PLAYERS; g++) {
                    if (!relay_target(g, pid) || !relay_due(1, g, pid, now)) continue;
                    CoopAsTreePacket q = ap;
                    q.origin = (uint8_t)pid, q.session = local_session, q.echo = conn[g].session;
                    coop_sendto(sock, &q, (int)sizeof q, &conn[g].addr);
                }
            }
        }
        pthread_mutex_unlock(&lock);
        return;
    }
    if (n == (int)sizeof(CoopPosePacket)) {  // (protocol 48) ragdoll pose: only from the current peer and session
        CoopPosePacket rp;
        memcpy(&rp, wire, sizeof rp);
        if (rp.magic != COOP_POSE_MAGIC || rp.version != COOP_VERSION || !coop_pose_ok(&rp.pose)) return;
        pthread_mutex_lock(&lock);
        int pid = sender_of(&from, rp.session, rp.echo, rp.origin);
        PlayerView *v = pid >= 0 ? &views[pid] : NULL;
        uint64_t now = plat_now_ns();
        if (v && (!v->pose_ns || (int32_t)(rp.seq - v->pose_seq) > 0 || now - v->pose_ns > 1000000000ull)) {
            v->pose = rp.pose;
            v->pose_seq = rp.seq, v->pose_ns = now;
            if (host_mode) {
                for (int g = 1; g < COOP_MAX_PLAYERS; g++) {
                    if (!relay_target(g, pid) || !relay_due(2, g, pid, now)) continue;
                    CoopPosePacket q = rp;
                    q.origin = (uint8_t)pid, q.session = local_session, q.echo = conn[g].session;
                    coop_sendto(sock, &q, (int)sizeof q, &conn[g].addr);
                }
            }
        }
        pthread_mutex_unlock(&lock);
        return;
    }
    if (n == (int)sizeof(CoopFaunaPacket)) {  // (protocol 80) the host's small animals: guest only, its host's session
        CoopFaunaPacket fp;
        memcpy(&fp, wire, sizeof fp);
        if (fp.magic != COOP_FAUNA_MAGIC || fp.version != COOP_VERSION || !coop_fauna_ok(&fp.f)) return;
        pthread_mutex_lock(&lock);
        uint64_t now = plat_now_ns();
        if (!host_mode && guest_partner && have_peer && same_addr(&from, &peer) && peer_session &&
            fp.session == peer_session && fp.echo == local_session &&
            (!remote_fauna_ns || remote_fauna_session != peer_session || (int32_t)(fp.seq - remote_fauna_seq) > 0 ||
             now - remote_fauna_ns > 1000000000ull)) {
            remote_fauna = fp.f;
            remote_fauna_seq = fp.seq, remote_fauna_ns = now, remote_fauna_session = peer_session;
        }
        pthread_mutex_unlock(&lock);
        return;
    }
    if (n == (int)sizeof(CoopEventPacket)) {  // reliable events: only from the current peer and session
        CoopEventPacket vp;
        memcpy(&vp, wire, sizeof vp);
        if (vp.magic != COOP_EVENT_MAGIC || vp.version != COOP_VERSION || vp.count > EVENTS_PER_PACKET) return;
        pthread_mutex_lock(&lock);
        int L = world_link_from(&from, vp.session, vp.echo);  // (58: per link)
        if (L >= 0 && link_on(L)) take_events(L, &vp);
        pthread_mutex_unlock(&lock);
        return;
    }
    if (n == (int)sizeof(CoopNamePacket)) {  // (protocol 62) names and ping
        CoopNamePacket np;
        memcpy(&np, wire, sizeof np);
        if (coop_name_packet_ok(&np)) {
            pthread_mutex_lock(&lock);
            take_names(&np, &from);
            pthread_mutex_unlock(&lock);
        }
        return;
    }
    CoopPacket p;
    if (n == (int)sizeof p) memcpy(&p, wire, sizeof p);
    if (n == (int)sizeof p && packet_valid(&p) &&
        (!host_mode || !(p.weather || p.moon_day || p.bloodmoon))) {
        pthread_mutex_lock(&lock);
        uint64_t now = plat_now_ns();
        int pid = -1;
        if (host_mode) {
            if (!session_retired(p.session) && host_confirm(&from, &p, now))
                pid = host_admit(&from, &p, now);
        } else if (have_peer && same_addr(&from, &peer) && p.echo == local_session) {
            // Guest: only the host. A new host session (the host restarted) starts everything again.
            uint64_t old_session = peer_session;
            if (!session_retired(p.session) && adopt_session(p.session) && old_session != peer_session) {
                for (int i = 0; i < COOP_MAX_PLAYERS; i++) view_reset(i);
                world_reset();
                my_id = -1, players_mask = 0;
                LOG("[coop] peer %s:%d new session\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            }
            if (p.session == peer_session) {
                if (p.origin == 0) {
                    guest_challenge = 0;
                    guest_take_host_info(&p);
                }
                pid = p.origin != my_id ? p.origin : -1;
            }
        }
        PlayerView *v = pid >= 0 && pid < COOP_MAX_PLAYERS ? &views[pid] : NULL;
        if (v && p.session == (host_mode ? conn[pid].session : peer_session) && p.echo == local_session &&
            (v->at_ns == 0 || (int32_t)(p.seq - v->seq) > 0 || (!host_mode && now - v->at_ns > 1000000000ull))) {
            CoopState *st = &v->st;
            v->seq = p.seq;
            memcpy(st->pos, p.pos, sizeof st->pos);
            st->yaw = p.yaw, st->speed = p.speed, st->anim = p.anim, st->flags = p.flags;
            st->anim_seq = p.anim_seq;
            st->anim_upper = p.anim_upper;
            st->anim_upper_seq = p.anim_upper_seq;
            st->face = p.face;
            st->aim_pitch = p.aim_pitch, st->aim_yaw = p.aim_yaw, st->aim_flags = p.aim_flags;
            st->area = p.area;
            memcpy(st->weapon, p.weapon, sizeof st->weapon);
            memcpy(st->gear, p.gear, sizeof st->gear);
            memcpy(st->held, p.held, sizeof st->held);
            memcpy(st->magbeam_tip, p.magbeam_tip, sizeof st->magbeam_tip);
            st->stasisfx = p.stasisfx;
            st->weather = p.weather, st->moon_day = p.moon_day;
            st->bloodmoon = p.bloodmoon;
            memcpy(st->armor, p.armor, sizeof st->armor);
            memcpy(v->bombs, p.bombs, sizeof v->bombs);
            memcpy(v->shots, p.shots, sizeof v->shots);
            v->horse = p.horse;
            v->at_ns = now;
            received++;
            if (host_mode) {  // (56) on to the other guests, as from us, with the sender's id
                for (int g = 1; g < COOP_MAX_PLAYERS; g++) {
                    if (!relay_target(g, pid) || !relay_due(0, g, pid, now)) continue;
                    CoopPacket q = p;
                    q.origin = (uint8_t)pid, q.your_id = (uint8_t)g, q.role = 0, q.players = players_now(now);
                    q.session = local_session, q.echo = conn[g].session, q.challenge = 0;
                    q.weather = q.moon_day = 0;
                    q.bloodmoon = 0;
                    coop_sendto(sock, &q, (int)sizeof q, &conn[g].addr);
                }
            }
        }
        pthread_mutex_unlock(&lock);
    }
}

static void send_local(uint32_t *seq) {
    pthread_mutex_lock(&lock);
    CoopPacket p = {0};
    p.magic = COOP_MAGIC; p.version = COOP_VERSION; p.flags = local.flags; p.anim = local.anim; p.anim_seq = local.anim_seq;
    p.anim_upper = local.anim_upper; p.anim_upper_seq = local.anim_upper_seq; p.face = local.face; p.seq = ++*seq;
    p.aim_pitch = local.aim_pitch; p.aim_yaw = local.aim_yaw; p.aim_flags = local.aim_flags;
    memcpy(p.pos, local.pos, sizeof p.pos);
    p.yaw = local.yaw; p.speed = local.speed; p.area = local.area;
    memcpy(p.magbeam_tip, local.magbeam_tip, sizeof p.magbeam_tip);
    if (!magbeam_ok(&p)) {
        p.flags &= ~COOP_FLAG_MAGBEAM;
        memset(p.magbeam_tip, 0, sizeof p.magbeam_tip);
    }
    p.stasisfx = local.stasisfx;
    if (host_mode) p.weather = local.weather, p.moon_day = local.moon_day, p.bloodmoon = local.bloodmoon;
    if (!stasisfx_ok(&p)) p.stasisfx = (CoopStasisFx){0};
    memcpy(p.weapon, local.weapon, sizeof p.weapon);
    p.weapon[sizeof p.weapon - 1] = 0;
    memcpy(p.gear, local.gear, sizeof p.gear);
    memcpy(p.armor, local.armor, sizeof p.armor);
    for (int i = 0; i < COOP_GEAR_COUNT; i++) {  // (a bad piece is dropped, not the whole state)
        p.gear[i].name[sizeof p.gear[i].name - 1] = 0;
        if (!gear_ok(&p.gear[i])) memset(&p.gear[i], 0, sizeof p.gear[i]);
    }
    for (int i = 0; i < COOP_ARMOR_SLOTS; i++) {
        p.armor[i][31] = 0;
        if (!weapon_name_ok(p.armor[i])) p.armor[i][0] = 0;
    }
    memcpy(p.held, local.held, sizeof p.held);
    for (int i = 0; i < COOP_HELD_ITEMS; i++) {
        p.held[i].name[sizeof p.held[i].name - 1] = 0;
        if (!held_ok(&p.held[i])) memset(&p.held[i], 0, sizeof p.held[i]);
    }
    memcpy(p.bombs, local_bombs, sizeof p.bombs);
    memcpy(p.shots, local_shots, sizeof p.shots);
    p.horse = local_horse;
    p.session = local_session; p.echo = peer_session;
    if (!host_mode) p.challenge = guest_challenge;
    // (56) who we are: a guest names its id (0 until the host told it); the host tells its partner its id and role
    p.origin = (uint8_t)(!host_mode && my_id > 0 ? my_id : 0);
    if (host_mode && partner_pid > 0) {
        p.your_id = (uint8_t)partner_pid, p.role = COOP_ROLE_PARTNER | (multiworld_on() ? COOP_ROLE_WORLD : 0), p.players = players_now(plat_now_ns());
    }
    if (!packet_valid(&p)) { // publish an unavailable state while the local game is transitioning
        memset(p.bombs, 0, sizeof p.bombs);
        memset(p.shots, 0, sizeof p.shots);
        memset(&p.horse, 0, sizeof p.horse);
        p.flags = 0; p.anim = 0; memset(p.pos, 0, sizeof p.pos); p.yaw = p.speed = 0; p.area = 0;
        memset(p.weapon, 0, sizeof p.weapon);
        memset(p.gear, 0, sizeof p.gear);
        memset(p.armor, 0, sizeof p.armor);
        memset(p.held, 0, sizeof p.held);
        memset(p.magbeam_tip, 0, sizeof p.magbeam_tip);
        p.stasisfx = (CoopStasisFx){0};
        p.weather = p.moon_day = 0;
    }
    uint64_t now_ns = plat_now_ns();
    bool send = have_peer && (!host_mode || conn_recent(partner_pid, now_ns));
    bool world = host_mode || guest_partner;  // (56) an extra guest sends no world data (the host ignores it)
    struct sockaddr_in to = peer;
    int self_link = host_mode ? partner_pid : 0;  // (58) the world link of `to` (host: the partner's)
    CoopEnemyPacket ep = {0};
    ep.magic = COOP_ENEMY_MAGIC; ep.version = COOP_VERSION; ep.seq = *seq;
    ep.generation = enemy_generation;
    ep.session = local_session; ep.echo = peer_session;
    ep.count = (uint8_t)(local_enemy_count < COOP_ENEMY_MAX ? local_enemy_count : COOP_ENEMY_MAX);
    ep.hour = local_hour;
    memcpy(ep.e, local_enemies, sizeof ep.e);
    memcpy(ep.names, local_names, sizeof ep.names);
    if (host_mode && partner_pid > 0) build_enemies_for(partner_pid, &ep, now_ns);  // (58: the others' damage added...)
    else for (int i = 0; i < ep.count; i++) ep.e[i].flags &= ~(COOP_ENEMY_OWNER_MASK | COOP_ENEMY_COPY);
    CoopEventPacket vp = {0};
    bool send_events = false;
    if (self_link >= 0) {
        WorldLink *w = &wl[self_link];
        vp.magic = COOP_EVENT_MAGIC; vp.version = COOP_VERSION; vp.ack = w->in_last;
        vp.session = local_session; vp.echo = peer_session;
        vp.count = (uint8_t)(w->out_count < EVENTS_PER_PACKET ? w->out_count : EVENTS_PER_PACKET);
        memcpy(vp.e, w->out, sizeof *vp.e * vp.count);
        send_events = peer_session && (vp.count || now_ns < w->ack_until_ns);
    }
    // (59) objects: a guest sends its own; the host sends each world guest its merged view (build_objects_for), in
    // up to two datagrams
    CoopObjPacket op[2] = {{0}};
    for (int k = 0; k < 2; k++) {
        op[k].magic = COOP_OBJ_MAGIC; op[k].version = COOP_VERSION; op[k].seq = *seq;
        op[k].session = local_session; op[k].echo = peer_session;
    }
    if (host_mode && partner_pid > 0) {
        CoopObject all[2 * COOP_OBJ_MAX];
        int n = build_objects_for(partner_pid, all, 2 * COOP_OBJ_MAX, now_ns);
        op[0].count = (uint8_t)(n < COOP_OBJ_MAX ? n : COOP_OBJ_MAX), op[1].count = (uint8_t)(n - op[0].count);
        memcpy(op[0].o, all, sizeof *all * op[0].count);
        memcpy(op[1].o, all + op[0].count, sizeof *all * op[1].count);
    } else {
        op[0].count = (uint8_t)local_object_count;
        memcpy(op[0].o, local_objects, sizeof *op[0].o * op[0].count);
    }
    CoopAsTreePacket ap = {0};
    ap.magic = COOP_ASTREE_MAGIC; ap.version = COOP_VERSION; ap.seq = *seq;
    ap.session = local_session; ap.echo = peer_session;
    ap.t = local_astree;
    bool send_astree = local_astree_set && peer_session && coop_astree_ok(&ap.t);
    CoopPosePacket rp = {0};
    rp.magic = COOP_POSE_MAGIC; rp.version = COOP_VERSION; rp.seq = *seq;
    rp.session = local_session; rp.echo = peer_session;
    rp.pose = local_pose;
    bool send_pose = local_pose_set && peer_session && coop_pose_ok(&rp.pose);
    ap.origin = rp.origin = p.origin;
    bool tree_ok = local_astree_set && coop_astree_ok(&ap.t), pose_ok = local_pose_set && coop_pose_ok(&rp.pose);
    CoopFaunaPacket fp = {0};  // (80) the host's small animals: to the partner, once per new set
    int fpid = host_mode ? partner_pid : -1;
    bool send_fauna = fpid > 0 && local_fauna_pending[fpid] && peer_session && coop_fauna_ok(&local_fauna[fpid]);
    if (send_fauna) {
        fp.magic = COOP_FAUNA_MAGIC; fp.version = COOP_VERSION; fp.seq = ++local_fauna_seq[fpid];
        fp.session = local_session; fp.echo = peer_session;
        fp.f = local_fauna[fpid];
        local_fauna_pending[fpid] = false;
    }
    // (56) host: the other guests (not the partner) get our state, tree, pose and the hour (an empty enemy packet);
    // (58) with MULTIWORLD each also gets its own enemy packet and its events
    static struct { struct sockaddr_in to; uint64_t echo; uint8_t pid; bool world, events, fauna; CoopEnemyPacket e; CoopEventPacket v; CoopObjPacket o[2]; CoopFaunaPacket f; } extra[COOP_MAX_PLAYERS];
    int extras = 0;
    uint8_t players = 0;
    if (host_mode) {
        players = players_now(now_ns);
        for (int g = 1; g < COOP_MAX_PLAYERS; g++) {
            if (g == partner_pid || !conn_recent(g, now_ns)) continue;
            typeof(extra[0]) *x = &extra[extras++];
            x->to = conn[g].addr, x->echo = conn[g].session, x->pid = (uint8_t)g, x->world = link_world(g);
            memset(&x->e, 0, sizeof x->e);
            x->e.magic = COOP_ENEMY_MAGIC; x->e.version = COOP_VERSION; x->e.seq = *seq;
            x->e.session = local_session; x->e.echo = x->echo; x->e.hour = ep.hour;
            x->e.generation = ep.generation;
            x->events = x->fauna = false;
            if (!x->world) continue;
            if (local_fauna_pending[g] && coop_fauna_ok(&local_fauna[g])) {  // (80) its own small-animal set
                memset(&x->f, 0, sizeof x->f);
                x->f.magic = COOP_FAUNA_MAGIC; x->f.version = COOP_VERSION; x->f.seq = ++local_fauna_seq[g];
                x->f.session = local_session; x->f.echo = x->echo;
                x->f.f = local_fauna[g];
                local_fauna_pending[g] = false, x->fauna = true;
            }
            memcpy(x->e.names, local_names, sizeof x->e.names);
            build_enemies_for(g, &x->e, now_ns);
            WorldLink *w = &wl[g];
            memset(&x->v, 0, sizeof x->v);
            x->v.magic = COOP_EVENT_MAGIC; x->v.version = COOP_VERSION; x->v.ack = w->in_last;
            x->v.session = local_session; x->v.echo = x->echo;
            x->v.count = (uint8_t)(w->out_count < EVENTS_PER_PACKET ? w->out_count : EVENTS_PER_PACKET);
            memcpy(x->v.e, w->out, sizeof *x->v.e * x->v.count);
            x->events = x->v.count || now_ns < w->ack_until_ns;
            CoopObject all[2 * COOP_OBJ_MAX];  // (59) its objects: ours and the other guests', merged
            int n = build_objects_for(g, all, 2 * COOP_OBJ_MAX, now_ns);
            for (int k = 0; k < 2; k++) {
                x->o[k] = op[k];
                x->o[k].echo = x->echo;
                x->o[k].count = (uint8_t)(k == 0 ? (n < COOP_OBJ_MAX ? n : COOP_OBJ_MAX) : n - x->o[0].count);
                memset(x->o[k].o, 0, sizeof x->o[k].o);
                memcpy(x->o[k].o, all + (k ? x->o[0].count : 0), sizeof *all * x->o[k].count);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    bool state_sent = false;
    for (int i = 0; i < extras; i++) {
        CoopPacket q = p;
        q.echo = extra[i].echo, q.your_id = extra[i].pid, q.role = extra[i].world ? COOP_ROLE_WORLD : 0, q.players = players;
        if (coop_sendto(sock, &q, (int)sizeof q, &extra[i].to) == (int)sizeof q) state_sent = true;
        if (extra[i].e.count || ep.hour >= 0) coop_sendto(sock, &extra[i].e, (int)sizeof extra[i].e, &extra[i].to);
        if (extra[i].events) coop_sendto(sock, &extra[i].v, (int)sizeof extra[i].v, &extra[i].to);
        if (extra[i].fauna) coop_sendto(sock, &extra[i].f, (int)sizeof extra[i].f, &extra[i].to);
        for (int k = 0; k < 2 && extra[i].world; k++)
            if (extra[i].o[k].count) coop_sendto(sock, &extra[i].o[k], (int)sizeof extra[i].o[k], &extra[i].to);
        if (tree_ok) {
            CoopAsTreePacket qa = ap;
            qa.echo = extra[i].echo;
            coop_sendto(sock, &qa, (int)sizeof qa, &extra[i].to);
        }
        if (pose_ok) {
            CoopPosePacket qr = rp;
            qr.echo = extra[i].echo;
            coop_sendto(sock, &qr, (int)sizeof qr, &extra[i].to);
        }
    }
    if (send && coop_sendto(sock, &p, (int)sizeof p, &to) == (int)sizeof p) {
        state_sent = true;
        pthread_mutex_lock(&lock);
        sent++;
        pthread_mutex_unlock(&lock);
    }
    if (host_mode && state_sent) {
        if (p.weather && p.weather != sent_weather)
            LOG("[coop] weather: host sent type %u\n", p.weather - 1);
        if (p.moon_day && p.moon_day != sent_moon_day)
            LOG("[coop] blood moon: host sent bloody day %s\n", p.moon_day == COOP_MOON_DAY_ON ? "on" : "off");
        sent_weather = p.weather, sent_moon_day = p.moon_day;
    }
    if (send && world && (ep.count || ep.hour >= 0)) coop_sendto(sock, &ep, (int)sizeof ep, &to);
    if (send && world && send_events) coop_sendto(sock, &vp, (int)sizeof vp, &to);
    for (int k = 0; k < 2; k++)
        if (send && world && op[k].count) coop_sendto(sock, &op[k], (int)sizeof op[k], &to);
    if (send && send_astree) coop_sendto(sock, &ap, (int)sizeof ap, &to);
    if (send && send_pose) coop_sendto(sock, &rp, (int)sizeof rp, &to);
    if (send && world && send_fauna) coop_sendto(sock, &fp, (int)sizeof fp, &to);
}

// The environment's config (the launchers and test scripts): SAVAGEWIND_COOP_PORT / _PEER / _CODE / _NAME.
bool coop_net_env_config(CoopNetConfig *c) {
    const char *port_s = getenv("SAVAGEWIND_COOP_PORT"), *peer_s = getenv("SAVAGEWIND_COOP_PEER");
    memset(c, 0, sizeof *c);
    if ((!port_s || !*port_s) && (!peer_s || !*peer_s)) return false;
    bool join = peer_s && *peer_s;
    c->mode = join ? COOP_MODE_JOIN : COOP_MODE_HOST;
    c->port = port_s && *port_s ? atoi(port_s) : (join ? 0 : 47000);
    if (join) snprintf(c->peer, sizeof c->peer, "%s", peer_s);
    const char *code = getenv("SAVAGEWIND_COOP_CODE"), *name = getenv("SAVAGEWIND_COOP_NAME");
    if (code) snprintf(c->code, sizeof c->code, "%s", code);
    if (name) snprintf(c->name, sizeof c->name, "%s", name);
    return true;
}

static bool env_driven;
static char room_code[sizeof ((CoopNetConfig *)0)->code];
static int local_join_port;
static bool split_room;
const char *coop_net_room_code(void) { return running && host_mode ? room_code : ""; }
int coop_net_local_join_port(void) { return running && host_mode ? local_join_port : 0; }
bool coop_net_split_room(void) { return running && split_room; }

static bool random_bytes(void *out, size_t n) {
#ifdef _WIN32
    unsigned char *p = out;
    while (n) {
        unsigned int value;
        if (rand_s(&value)) return false;
        size_t take = n < sizeof value ? n : sizeof value;
        memcpy(p, &value, take);
        p += take;
        n -= take;
    }
    return true;
#else
    FILE *rng = fopen("/dev/urandom", "rb");
    if (!rng) return false;
    bool ok = fread(out, 1, n, rng) == n;
    fclose(rng);
    return ok;
#endif
}
static char start_error[160];
bool coop_net_env_driven(void) { return running && env_driven; }
const char *coop_net_error(void) { return start_error; }
bool coop_menu_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_MENU") && !strcmp(getenv("SAVAGEWIND_COOP_MENU"), "0"));
    return on;
}

void coop_net_init(void) {
    CoopNetConfig c;
    if (!coop_net_env_config(&c)) return;
    if (coop_net_start(&c)) env_driven = true;
}

// An IPv4 address, or a host name that resolves to one (a guest's peer)
static bool resolve_v4(const char *host, struct in_addr *out) {
    if (inet_pton(AF_INET, host, out) == 1) return true;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return false;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

bool coop_net_start(const CoopNetConfig *cfg) {
    start_error[0] = 0;
    if (running) { snprintf(start_error, sizeof start_error, "already started"); return false; }
    if (!cfg || (cfg->mode != COOP_MODE_HOST && cfg->mode != COOP_MODE_JOIN)) {
        snprintf(start_error, sizeof start_error, "solo: nothing to start");
        return false;
    }
    const char *peer_s = cfg->mode == COOP_MODE_JOIN ? cfg->peer : "";
    if (cfg->mode == COOP_MODE_JOIN && !*peer_s) { snprintf(start_error, sizeof start_error, "no host address"); return false; }
    // Unique boot identity, not an authentication secret. Use only a trusted LAN/VPN.
#ifdef _WIN32
    local_session = plat_now_ns() ^ ((uint64_t)GetCurrentProcessId() << 32);
#else
    local_session = plat_now_ns() ^ ((uint64_t)getpid() << 32);
#endif
    if (!local_session) local_session = 1;
#ifdef _WIN32
    static bool wsa_up;
    WSADATA wsa;
    if (!wsa_up && WSAStartup(MAKEWORD(2, 2), &wsa)) {
        LOG("[coop] Winsock initialization failed\n");
        snprintf(start_error, sizeof start_error, "network unavailable");
        return false;
    }
    wsa_up = true;
#endif
    int port = cfg->port;
    have_peer = false;
    if (*peer_s) {
        char host[128];
        snprintf(host, sizeof host, "%s", peer_s);
        char *colon = strrchr(host, ':');
        int pport = colon ? atoi(colon + 1) : 47000;
        if (colon) *colon = 0;
        memset(&peer, 0, sizeof peer);
        peer.sin_family = AF_INET;
        peer.sin_port = htons((uint16_t)pport);
        if (!resolve_v4(host, &peer.sin_addr)) {
            LOG("[coop] SAVAGEWIND_COOP_PEER: \"%s\" is not an IPv4 address\n", host);
            snprintf(start_error, sizeof start_error, "\"%.100s\" is not an address", host);
            return false;
        }
        have_peer = true;
    }
    host_mode = !have_peer;
    enemy_generation = host_mode ? local.bloodmoon : 0;
    guest_challenge = 0;
    if (host_mode && !random_bytes(challenge_key, sizeof challenge_key)) {
        snprintf(start_error, sizeof start_error, "network challenge generation failed");
        return false;
    }
    my_id = -1, partner_pid = -1;
    if (host_mode) my_id = 0;     // (56) the host is player 0; its world partner is its first guest
    else partner_pid = 0;         // a guest's partner is the host (until the host says we are an extra guest)
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    const char *bind_ip = cfg->loopback ? "127.0.0.1" : getenv("SAVAGEWIND_COOP_BIND");
    if (host_mode && bind_ip && *bind_ip && inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) {
        snprintf(start_error, sizeof start_error, "invalid IPv4 bind address");
        return false;
    }
    const char *open_room = getenv("SAVAGEWIND_COOP_OPEN");
    const char *code = cfg->code + strspn(cfg->code, " \t");
    size_t code_len = strlen(code);
    while (code_len && strchr(" \t\r\n", code[code_len - 1])) code_len--;
    snprintf(room_code, sizeof room_code, "%.*s", (int)code_len, code);
    if (host_mode && !room_code[0] && (ntohl(a.sin_addr.s_addr) >> 24) != 127 && (!open_room || strcmp(open_room, "1"))) {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        unsigned char bytes[8];
        if (!random_bytes(bytes, sizeof bytes)) {
            snprintf(start_error, sizeof start_error, "room code generation failed");
            return false;
        }
        for (size_t i = 0; i < sizeof bytes; i++) room_code[i] = alphabet[bytes[i] & 31];
        room_code[sizeof bytes] = 0;
    }
    code_key_init(room_code);
    sim_init();
    coop_net_set_name(cfg->name);
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == COOP_BAD_SOCKET) { LOG("[coop] no UDP socket\n"); snprintf(start_error, sizeof start_error, "no UDP socket"); return false; }
    if (bind(sock, (struct sockaddr *)&a, sizeof a)) {
        LOG("[coop] cannot listen on UDP port %d\n", port);
        snprintf(start_error, sizeof start_error, "UDP port %d is in use", port);
        coop_close(sock);
        sock = COOP_BAD_SOCKET;
        return false;
    }
    socklen_t addr_len = sizeof a;
    if (!getsockname(sock, (struct sockaddr *)&a, &addr_len)) port = ntohs(a.sin_port);
    local_join_port = host_mode && (a.sin_addr.s_addr == htonl(INADDR_ANY) ||
                      a.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) ? port : 0;
    split_room = cfg->loopback;
    stop_req = false;
    sent_weather = sent_moon_day = 0;
    if (pthread_create(&net_tid, NULL, net_thread, NULL)) {
        LOG("[coop] no network thread\n");
        snprintf(start_error, sizeof start_error, "no network thread");
        coop_close(sock);
        sock = COOP_BAD_SOCKET;
        return false;
    }
    running = true;
    // (coop-upnp) a host others reach from the internet: the router is asked to forward the port (background)
    if (host_mode && !split_room && (ntohl(a.sin_addr.s_addr) >> 24) != 127) coop_upnp_start(port);
    LOG("[coop] UDP port %d, peer %s, %s\n", port, have_peer ? peer_s : "(waiting for one)",
        room_code[0] ? "join code set" : "no join code");
    if (local_name[0]) LOG("[coop] our name: \"%s\"\n", local_name);
    return true;
}

// The thread ends, the socket closes, and every player is forgotten; a later coop_net_start begins a new session
// (the others see a restarted player). The game side keeps its avatars until the reboot that follows (co-op menu).
void coop_net_stop(void) {
    if (!running) return;
    coop_upnp_stop();
    stop_req = true;
    pthread_join(net_tid, NULL);
    pthread_mutex_lock(&lock);
    running = false, env_driven = false;
    coop_close(sock);
    sock = COOP_BAD_SOCKET;
    retire(peer_session);
    for (int g = 0; g < COOP_MAX_PLAYERS; g++) retire(conn[g].session), view_reset(g);
    memset(conn, 0, sizeof conn);
    world_reset();
    have_peer = false, host_mode = false, peer_session = 0;
    enemy_generation = 0;
    my_id = -1, partner_pid = -1, guest_partner = true, guest_world_all = false, players_mask = 0;
    sent = received = 0;
    remote_hour = -1, remote_hour_ns = 0;
    memset(relay_last_ns, 0, sizeof relay_last_ns);
    room_code[0] = 0;
    pthread_mutex_unlock(&lock);
    LOG("[coop] network stopped (left the co-op session)\n");
}
