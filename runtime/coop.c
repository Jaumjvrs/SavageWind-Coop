// Co-op (experimental, game version 1.9.0 only): actors the runtime asks the game to create.
//
// Milestone 1: `coop spawn <actor> [x y z]` (developer command) asks the game's actor creator for an actor by name,
// placed at x y z (default: Link's position). The command only queues the request; the game thread carries it out in
// rt_coop_game_frame (called from rt_vfr_update, the start of every game frame, on the game's main thread), because
// guest functions must run on a guest thread. `coop status` shows what happened.
//
// The game side (found by static analysis of 1.9.0):
//   0x7001535b10 bool request_create(creator, const char *name, heap, link *out, params *init, 0, 1)
//     queues a creation on the actor creator; the actor is built later and `out` (an empty 16-byte link) refers to it.
//   creator = **(0x7001d66550), heap = *(**(0x7001d66558) + 0x28)
//   params: {vtable = *(0x7001d664b0) + 0x10, 0, buffer[0xc4]}; buffer cleared by 0x700112615c(buffer) and filled by
//     0x7001126168(buffer, value *, key SafeString *, size, type): key "@P" (0x7001632442), vec3 (size 12, type 4).
//   SafeString: {vtable = *(0x7001d66260) + 0x10, const char *}.
#include <math.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "control_cells.h"
#include "control_mem.h"
#include "coop_net.h"
#include "coop_split.h"
#include "coop_split_hud.h"
#include "framepace.h"
#include "game_sites.h"
#include "platform.h"
#include "hle.h"
#include "rt.h"
#include "settings.h"
#include "split_audio.h"

#define A_REQUEST_CREATE 0x7001535b10ull  // (turned out to load the actor's resources into a handle, not to create it)
#define A_CREATE_NOW 0x7001535900ull      // Actor *f(creator, name, heap, params *, 1, 0): creates now, NULL if not
#define A_PARAMS_INIT 0x700112615cull
#define A_PARAMS_ADD 0x7001126168ull
#define A_CREATOR_SLOT 0x7001d66550ull
#define A_HEAPS_SLOT 0x7001d66558ull
#define A_PARAMS_VT_SLOT 0x7001d664b0ull
#define A_SAFESTRING_VT_SLOT 0x7001d66260ull
#define A_KEY_POS 0x7001632442ull  // "@P"
// "@M": the creation matrix param (Matrix34f, 0x30 bytes, param type 7) - what the Cryonis manager gives its blocks
// (FUN_7100b2e364, 2026-11-14 RE).
#define A_KEY_MTX 0x700160f9e5ull
// Link's model hides the materials tagged with a HideMaterialGroup until the player code shows them:
// 0x700126ba4c(*(0x7001d66c98), model, armor model or 0, visible) with model = actor+0x4e0 (what the player's
// equipment code does first, with 0 and 1: every group visible, then hides what each armor piece covers).
#define A_MATERIAL_GROUPS 0x700126ba4cull
#define A_MATERIAL_GROUPS_ARG 0x7001d66c98ull
#define ACTOR_MODEL 0x4e0
// Animation: 0x700149f250(as controller = actor+0x560, SafeString *name, slot, 0, 1) with s0 = s1 = -1 (what the AI
// actions' play helper 0x700151e42c does): plays an AS of the actor's AS list by name.
#define A_PLAY_AS 0x700149f250ull
#define ACTOR_AS 0x560
// Delete an actor the way the game does (FUN_7101514bac): remove it from the actor manager's list and run its
// destructor. The create function calls it with reason 0 when creation fails, so it is safe for our actors.
// Returns 1 when it deleted the actor, 0 when it was already gone.
#define A_DELETE_ACTOR 0x7001514bacull
#define ACTOR_DELETE_REASON 0
// Resolve an equipment slot to the equipped item: bool f(long *slot, out[0x18]) (the item pointer is out+8).
#define A_EQUIP_GET 0x700056d8d8ull
// Attach an actor to another (the player's equipment code does this for armor): f(child, parent).
#define A_ATTACH 0x700117363cull
// Give a model another model as its skeleton/parent (the player's armor does this): f(model, parent model).
#define A_MODEL_SET_PARENT 0x7000f7658cull

// The first instructions of the functions, as in 1.9.0: nothing is called when the code differs.
static const struct { uint64_t addr; uint32_t word; } expect[] = {
    {A_REQUEST_CREATE, 0xd10183ff},  // sub sp, sp, #0x60
    {A_CREATE_NOW, 0xa9be7bfd},      // stp x29, x30, [sp, #-0x20]!
    {A_PARAMS_INIT, 0x52801882},     // mov w2, #0xc4
    {A_PARAMS_ADD, 0x79c00408},      // ldrsh w8, [x0, #2]
    {A_MATERIAL_GROUPS, 0xb9403828}, // ldr w8, [x1, #0x38]
    {A_PLAY_AS, 0xf9406c08},         // ldr x8, [x0, #0xd8]
    {A_DELETE_ACTOR, 0xa9bb7bfd},    // stp x29, x30, [sp, #-0x50]!
    {A_EQUIP_GET, 0xa9bd7bfd},       // stp x29, x30, [sp, #-0x30]!
    {A_ATTACH, 0xa9be7bfd},          // stp x29, x30, [sp, #-0x20]!
    {A_MODEL_SET_PARENT, 0xa9be7bfd}, // stp x29, x30, [sp, #-0x20]!
    {0x70014a0040ull, 0xb940b808},   // (A_CURRENT_AS) ldr w8, [x0, #0xb8]
    {0x700056b6d0ull, 0xa9bf7bfd},   // (A_ACTOR_TYPE) stp x29, x30, [sp, #-0x10]!
    {0x700128d5b8ull, 0xd10103ff},   // (A_BYML_GET_U32) sub sp, sp, #0x40
};

enum { COOP_IDLE, COOP_QUEUED, COOP_DONE, COOP_FAILED };

static struct {
    volatile int state;  // written by the presenter (QUEUED) and the game thread (DONE/FAILED)
    char name[96];
    float pos[3];
    bool has_pos;
    bool has_mtx;        // (2026-11-14, runes.inc) also the "@M" matrix param (rotation + translation, 3x4 rows)
    float mtx[12];
    uint64_t link;       // guest address of the link the last request filled
    uint64_t actor;      // what the create returned (spawn), 0 for a preload
    bool preload;        // `coop preload`: only load the resources (0x7001535b10)
    bool sync;           // `coop spawnsync`: create with the background flag (w4) off
    bool result;
    char why[96];
    unsigned count;
} req;

// Puppet: the last spawned actor, placed by us every game frame (a stand-in for a remote player's avatar).
// Actor world matrix: sead::Matrix34f (3 rows of 4 floats, translation in the 4th column) at actor+0x398; a second
// copy at +0x3d0 (previous frame's?) gets the same values.
#define ACTOR_MTX 0x398
#define ACTOR_MTX2 0x3d0
enum { PUPPET_OFF, PUPPET_FIXED, PUPPET_FOLLOW };
static struct {
    volatile int mode;
    volatile int show_body;  // `coop showbody`: run once on the game thread
    volatile int play;       // `coop anim <name> [slot]`: play once on the game thread
    volatile int despawn;    // `coop despawn`: delete the last spawned actor on the game thread
    volatile int outfit;     // `coop outfit`: apply the baked clothes on the game thread
    volatile int armor;      // `coop armor <name>`: spawn that armor actor and attach it to the avatar
    char armor_name[96];
    uint64_t armor_parent;   // the actor all pieces attach to (the avatar, not the previous piece)
    uint64_t armor_last;     // the last armor actor `coop armor` created
    volatile int armor_tick; // `coop armortick on`: run that armor's per-frame update (vtable +0x238) ourselves
    char anim[64];
    int anim_slot;
    uint64_t anim_actor;     // `coop anim <name> <slot> <actor>`: play on that actor instead of the spawned one
    uint64_t actor;
    float pos[3], yaw;      // FIXED: where; FOLLOW: offset from Link
    unsigned frames;
} puppet;

static void write_mtx(uint64_t m, const float p[3], float yaw_deg) {
    float a = yaw_deg * 3.14159265f / 180, cs = cosf(a), sn = sinf(a);
    float v[12] = {cs, 0, sn, p[0], 0, 1, 0, p[1], -sn, 0, cs, p[2]};
    memcpy((void *)(uintptr_t)m, v, sizeof v);
}

static uint64_t player_pos(float p[3]);

static void puppet_frame(void) {
    int mode = puppet.mode;
    if (mode == PUPPET_OFF || !puppet.actor) return;
    float p[3];
    if (mode == PUPPET_FOLLOW) {
        if (!player_pos(p)) return;
        for (int i = 0; i < 3; i++) p[i] += puppet.pos[i];
    } else memcpy(p, puppet.pos, sizeof p);
    write_mtx(puppet.actor + ACTOR_MTX, p, puppet.yaw);
    write_mtx(puppet.actor + ACTOR_MTX2, p, puppet.yaw);
    puppet.frames++;
}

static bool code_matches(void) {
    if (strcmp(g_game_version, "1.9.0")) return false;
    for (unsigned i = 0; i < sizeof expect / sizeof *expect; i++)
        if (LD32(expect[i].addr) != expect[i].word) return false;
    return true;
}

static void call(Cpu *c, uint64_t fn) { guest_call(c, fn); }

static uint64_t create_mem;  // guest block reused by every creation (reset on reboot)
static void do_spawn(Cpu *c) {
    // Guest memory for this request (hle_alloc never frees). A preload keeps its link (the game fills it later), so it
    // gets its own block (dev only); creations reuse one block (copies of run-time enemies are made all the time).
    if (!req.preload && !create_mem) create_mem = (uint64_t)(uintptr_t)hle_alloc(0x400);
    uint64_t m = req.preload ? (uint64_t)(uintptr_t)hle_alloc(0x400) : create_mem;
    if (!m) { snprintf(req.why, sizeof req.why, "no guest memory"); req.state = COOP_FAILED; return; }
    uint64_t link = m, params = m + 0x40, key = m + 0x140, pos = m + 0x160, name = m + 0x180;
    memcpy((void *)(uintptr_t)name, req.name, strlen(req.name) + 1);
    uint64_t creator = LD64(LD64(A_CREATOR_SLOT));
    uint64_t heap = LD64(LD64(LD64(A_HEAPS_SLOT)) + 0x28);
    if (!creator || !heap) {
        snprintf(req.why, sizeof req.why, "actor creator not ready (creator %llx heap %llx)",
                 (unsigned long long)creator, (unsigned long long)heap);
        req.state = COOP_FAILED;
        return;
    }
    uint64_t init = 0;
    if (req.has_pos) {
        ST64(params, LD64(A_PARAMS_VT_SLOT) + 0x10);
        ST64(params + 8, 0);
        c->x[0] = params + 0x10;
        call(c, A_PARAMS_INIT);
        ST64(key, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
        ST64(key + 8, A_KEY_POS);
        memcpy((void *)(uintptr_t)pos, req.pos, 12);
        c->x[0] = params + 0x10; c->x[1] = pos; c->x[2] = key; c->x[3] = 12; c->x[4] = 4;
        call(c, A_PARAMS_ADD);
        init = params;
    }
    if (req.has_mtx) {  // (after "@P" when both: the params list keeps both keys)
        uint64_t key2 = m + 0x240, mtx = m + 0x200;
        if (!init) {
            ST64(params, LD64(A_PARAMS_VT_SLOT) + 0x10);
            ST64(params + 8, 0);
            c->x[0] = params + 0x10;
            call(c, A_PARAMS_INIT);
        }
        ST64(key2, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
        ST64(key2 + 8, A_KEY_MTX);
        memcpy((void *)(uintptr_t)mtx, req.mtx, 48);
        c->x[0] = params + 0x10; c->x[1] = mtx; c->x[2] = key2; c->x[3] = 0x30; c->x[4] = 7;
        call(c, A_PARAMS_ADD);
        init = params;
    }
    if (req.preload) {
        c->x[0] = creator; c->x[1] = name; c->x[2] = heap; c->x[3] = link; c->x[4] = init; c->x[5] = 0; c->x[6] = 1;
        call(c, A_REQUEST_CREATE);
        req.result = c->x[0] & 1;
        req.actor = 0;
    } else {
        c->x[0] = creator; c->x[1] = name; c->x[2] = heap; c->x[3] = init; c->x[4] = req.sync ? 0 : 1; c->x[5] = 0;
        call(c, A_CREATE_NOW);
        req.actor = c->x[0];
        req.result = req.actor != 0;
    }
    req.link = link;
    req.count++;
    LOG("[coop] %s %s at %.2f %.2f %.2f -> %d (actor %llx link %llx)\n", req.preload ? "preload" : "spawn", req.name,
        req.pos[0], req.pos[1], req.pos[2], (int)req.result, (unsigned long long)req.actor, (unsigned long long)link);
    req.state = req.result ? COOP_DONE : COOP_FAILED;
    if (!req.result) snprintf(req.why, sizeof req.why, "the game refused the request (unknown actor name?)");
}

// Every hideable material group of a Link model visible (the player's equipment code's first step).
static void show_body(Cpu *c, uint64_t actor) {
    uint64_t model = LD64(actor + ACTOR_MODEL);
    if (!model) return;
    Cpu saved = *c;
    c->x[0] = LD64(A_MATERIAL_GROUPS_ARG); c->x[1] = model; c->x[2] = 0; c->x[3] = 1;
    guest_call(c, A_MATERIAL_GROUPS);
    *c = saved;
}

// The avatar's baked clothes (build_overlay.py: ModelData 1..3 = Armor_001 Head/Upper/Lower). Hide the body groups
// each piece covers, like the player's own equipment code does (FUN_7101175324): the player's equipment component is
// at player actor +0x2500, its slots +0x10/+0x20/+0x30 hold the equipped item, and the item's model is at +0x4e0.
#define PLAYER_EQUIP 0x2500
static uint64_t outfit_out;  // guest memory for A_EQUIP_GET's result (reset on reboot)
static void apply_outfit(Cpu *c, uint64_t actor) {
    uint64_t mc = LD64(actor + ACTOR_MODEL);
    if (!mc) return;
    show_body(c, actor);  // every group visible first
    uint64_t pinfo = g_site.player_info ? LD64(g_site.player_info) : 0;
    uint64_t pa = pinfo ? LD64(pinfo + 0x60) : 0;
    uint64_t equip = pa ? LD64(pa + PLAYER_EQUIP) : 0;
    uint64_t armor[3] = {0, 0, 0}, item[3] = {0, 0, 0};
    if (equip) {
        if (!outfit_out) outfit_out = (uint64_t)(uintptr_t)hle_alloc(0x20);
        Cpu saved = *c;
        for (int i = 0; i < 3; i++) {
            memset((void *)(uintptr_t)outfit_out, 0, 0x20);
            *c = saved;
            c->x[0] = equip + 0x10 + (uint64_t)i * 0x10;
            c->x[1] = outfit_out;
            guest_call(c, A_EQUIP_GET);
            item[i] = LD64(outfit_out + 8);
            if (item[i] >= GUEST_LO && item[i] < GUEST_HI) armor[i] = LD64(item[i] + ACTOR_MODEL);
            if (armor[i] >= GUEST_LO && armor[i] < GUEST_HI) {
                *c = saved;
                c->x[0] = LD64(A_MATERIAL_GROUPS_ARG); c->x[1] = mc; c->x[2] = armor[i]; c->x[3] = 0;
                guest_call(c, A_MATERIAL_GROUPS);
            }
        }
        *c = saved;
    }
    LOG("[coop] outfit %llx mc %llx equip %llx item %llx %llx %llx armor %llx %llx %llx\n",
        (unsigned long long)actor, (unsigned long long)mc, (unsigned long long)equip,
        (unsigned long long)item[0], (unsigned long long)item[1], (unsigned long long)item[2],
        (unsigned long long)armor[0], (unsigned long long)armor[1], (unsigned long long)armor[2]);
}

// Play an AS of the actor's AS list by name.
#define A_STOP_AS 0x700149f1f0ull
static uint64_t anim_mem;  // guest buffer for the name's SafeString (the game copies the name; reset on reboot)
static void play_anim_l(Cpu *c, uint64_t actor, const char *name, int slot, int layer) {
    uint64_t as = LD64(actor + ACTOR_AS), mem;
    if (!as || strlen(name) >= 0xc0) return;
    if (!anim_mem) anim_mem = (uint64_t)(uintptr_t)hle_alloc(0x100);
    mem = anim_mem;
    memcpy((void *)(uintptr_t)(mem + 0x40), name, strlen(name) + 1);
    ST64(mem, LD64(A_SAFESTRING_VT_SLOT) + 0x10);
    ST64(mem + 8, mem + 0x40);
    Cpu saved = *c;
    float m1 = -1.0f;
    memset(&c->v[0], 0, 16), memset(&c->v[1], 0, 16);
    memcpy(&c->v[0], &m1, 4), memcpy(&c->v[1], &m1, 4);
    c->x[0] = as; c->x[1] = mem; c->x[2] = (uint64_t)slot; c->x[3] = (uint64_t)layer; c->x[4] = 1;
    guest_call(c, A_PLAY_AS);
    *c = saved;
}
static void play_anim(Cpu *c, uint64_t actor, const char *name, int slot) { play_anim_l(c, actor, name, slot, 0); }

// (layer: the slot's layer, FUN_710149f1f0(as, slot, layer, 1))
static void stop_anim_l(Cpu *c, uint64_t actor, int slot, int layer) {
    uint64_t as = LD64(actor + ACTOR_AS);
    if (!as) return;
    Cpu saved = *c;
    c->x[0] = as; c->x[1] = (uint64_t)slot; c->x[2] = (uint64_t)layer; c->x[3] = 1;
    guest_call(c, A_STOP_AS);
    *c = saved;
}
static void stop_anim(Cpu *c, uint64_t actor, int slot) { stop_anim_l(c, actor, slot, 0); }

// Delete an actor now (game thread); true when the game deleted it. The pointer is invalid afterwards.
static bool despawn_now(Cpu *c, uint64_t actor, int reason) {
    if (!actor) return false;
    Cpu saved = *c;
    c->x[0] = actor;
    c->x[1] = (uint64_t)reason;
    guest_call(c, A_DELETE_ACTOR);
    bool ok = (c->x[0] & 1) != 0;
    *c = saved;
    return ok;
}

// ---- spawn hold (2026-11-17, play report 21: a game crashed in the model load of an actor we created). The log:
// the player's talk ended in the warm doublet's item fanfare (Event/Demo710_0 being read), the partner's avatar and
// its gear puppets were created in those same frames (one refused), then an access violation inside the actor
// creation (guest frames 7000faeccc ... 700152dc48: a model/material setup reached from do_spawn <- gear_frame).
// So nothing of ours is created while the game starts or plays an event (talk, cutscene, item fanfare) or shows the
// loading screen, nor for SPAWN_HOLD_AFTER_NS after that. An event that has run for SPAWN_HOLD_EVENT_MAX_NS without
// changing its name no longer holds (some events run in the background while the player plays); a new event name
// holds again. SAVAGEWIND_COOP_SPAWNHOLD=0 turns it off. Existing actors are left as they are.
// (2026-11-18, worker cutcrash) The crash decoded: the faulting write is in the new actor's animation setup
// (0x7001520c9c stores actor+0x4e0 from 0x7000f76178; the fault 0x7000faeccc writes through a buffer that came back
// null, x0 = 0x4fc0): the animation memory ran out while the demo loaded its own (same as the 3-player rejoin crash
// and the Rudania boss crash, "could not create" first each time). Two gaps closed:
//  - the 20 s cap: a talk read slowly lasts more than 20 s, and the item fanfare it ends with can run under the same
//    event name. Now the hold also stays on, whatever the time, while an event runs and our Link plays a "Demo*" AS
//    (the event holds the player: DemoWait in that log; background events leave Link playing normally).
//  - a creation the game refuses is the sign that its memory is short: nothing more is created for
//    SPAWN_FAIL_HOLD_NS (doubled on each refusal in a row, up to 60 s; a creation that works resets it). In the crash
//    the refused armor piece was followed by three more creations, the third crashed. SAVAGEWIND_COOP_SPAWNBACKOFF=0
//    turns this part off.
#define SPAWN_HOLD_AFTER_NS 1500000000ull
#define SPAWN_HOLD_EVENT_MAX_NS 20000000000ull
#define SPAWN_FAIL_HOLD_NS 5000000000ull
#define SPAWN_FAIL_HOLD_MAX_NS 60000000000ull
const char *framepace_event_name(void);
static struct {
    uint64_t until_ns, event_since_ns, fail_until_ns, fail_hold_ns;
    bool in_event, held, demo_as, failhold;
    char event[32];
    unsigned refused, fails;
} shold;
static char shold_local_as[32];  // our Link's slot 0 AS (coop/avatar.inc publish_local, the frame before)
static volatile int refuse_dev;  // `coop refuse` (dev, a two-game test): one creation the game refuses
static bool spawn_backoff_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_SPAWNBACKOFF") && !strcmp(getenv("SAVAGEWIND_COOP_SPAWNBACKOFF"), "0"));
    return on;
}
static bool spawn_hold_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_SPAWNHOLD") && !strcmp(getenv("SAVAGEWIND_COOP_SPAWNHOLD"), "0"));
    return on;
}
// Once per frame, before anything creates actors (rt_coop_game_frame).
static void spawn_hold_frame(uint64_t now) {
    if (!spawn_hold_on()) return;
    bool ev = framepace_in_event(), load = framepace_loading_ui();
    const char *nm = ev ? framepace_event_name() : "";
    if (ev && (!shold.in_event || strncmp(nm, shold.event, sizeof shold.event - 1))) {
        shold.event_since_ns = now;
        snprintf(shold.event, sizeof shold.event, "%s", nm);
    }
    shold.in_event = ev;
    bool demo_as = ev && !strncmp(shold_local_as, "Demo", 4);  // (the event holds our Link: no time cap)
    if (demo_as != shold.demo_as) {
        shold.demo_as = demo_as;
        if (demo_as) LOG("[coop] spawn hold: our Link is held by the event%s%s (AS %s)\n", shold.event[0] ? " " : "",
                         shold.event, shold_local_as);
    }
    if ((ev && (now - shold.event_since_ns < SPAWN_HOLD_EVENT_MAX_NS || demo_as)) || load)
        shold.until_ns = now + SPAWN_HOLD_AFTER_NS;
    bool held = now < shold.until_ns;
    if (held != shold.held) {
        if (held) LOG("[coop] spawn hold: nothing is created while %s%s%s\n", load ? "loading" : "an event runs",
                      ev && shold.event[0] ? ": " : "", ev ? shold.event : "");
        else LOG("[coop] spawn hold: over (%u creations put off)\n", shold.refused);
        shold.held = held, shold.refused = 0;
    }
    bool failhold = now < shold.fail_until_ns;
    if (shold.failhold && !failhold) LOG("[coop] spawn hold: refusal wait over\n");
    shold.failhold = failhold;
}
static bool spawn_hold(void) {
    if (!spawn_hold_on()) return false;
    if (shold.held) return true;
    return spawn_backoff_on() && shold.fail_until_ns && plat_now_ns() < shold.fail_until_ns;
}
static void heap_report(Cpu *c, const char *why, uint64_t actor, const char *aname);
// After each creation (spawn_now / spawn_mtx_now): a refusal holds every creation for a while (see above).
static void spawn_result(Cpu *c, const char *name, uint64_t actor) {
    if (actor) { shold.fails = 0, shold.fail_hold_ns = 0; return; }
    if (!spawn_hold_on() || !spawn_backoff_on()) return;
    shold.fails++;
    shold.fail_hold_ns = shold.fail_hold_ns ? shold.fail_hold_ns * 2 : SPAWN_FAIL_HOLD_NS;
    if (shold.fail_hold_ns > SPAWN_FAIL_HOLD_MAX_NS) shold.fail_hold_ns = SPAWN_FAIL_HOLD_MAX_NS;
    shold.fail_until_ns = plat_now_ns() + shold.fail_hold_ns;
    shold.failhold = true;
    LOG("[coop] spawn hold: the game refused %s (%u in a row): nothing is created for %llu s\n", name, shold.fails,
        (unsigned long long)(shold.fail_hold_ns / 1000000000ull));
    heap_report(c, "refused", 0, NULL);
}

// (2026-11-21, 11-20 evening play: 3 crashes with the same frames - a new avatar / armor / horse puppet's AS setup got a
// null block from its heap - each right after a shrine load, when the avatar and 2-3 puppets were created in the same
// few frames.) Our puppets (Dm_Coop*) are created at most one per SPAWN_GAP_NS; a creation put off this way returns 0
// like the spawn hold (callers try again later). SAVAGEWIND_COOP_SPAWNGAP=0: off.
#define SPAWN_GAP_NS 150000000ull
static uint64_t spawn_gap_last;
static bool spawn_gap_hold(const char *name) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_SPAWNGAP") && !strcmp(getenv("SAVAGEWIND_COOP_SPAWNGAP"), "0"));
    if (!on || strncmp(name, "Dm_Coop", 7)) return false;
    uint64_t now = plat_now_ns();
    if (spawn_gap_last && now - spawn_gap_last < SPAWN_GAP_NS) return true;
    spawn_gap_last = now;
    return false;
}

// Create an actor now, at pos (game thread); 0 if the game refuses (or while the spawn hold is on).
static uint64_t spawn_now(Cpu *c, const char *name, const float pos[3]) {
    if (spawn_hold() || spawn_gap_hold(name)) { shold.refused++; req.actor = 0; return 0; }
    snprintf(req.name, sizeof req.name, "%s", name);
    memcpy(req.pos, pos, sizeof req.pos);
    req.has_pos = true, req.preload = false, req.sync = true, req.has_mtx = false;
    req.actor = 0;  // (do_spawn's early refusals leave it as it was)
    Cpu saved = *c;
    do_spawn(c);
    *c = saved;
    spawn_result(c, name, req.actor);
    return req.actor;
}
// Create an actor now with a full creation matrix (rows of a 3x4: rotation and translation; game thread).
static uint64_t spawn_mtx_now(Cpu *c, const char *name, const float m[12]) {
    if (spawn_hold() || spawn_gap_hold(name)) { shold.refused++; req.actor = 0; return 0; }
    snprintf(req.name, sizeof req.name, "%s", name);
    float p[3] = {m[3], m[7], m[11]};
    memcpy(req.pos, p, sizeof req.pos);
    memcpy(req.mtx, m, sizeof req.mtx);
    req.has_pos = true, req.has_mtx = true, req.preload = false, req.sync = true;
    req.actor = 0;
    Cpu saved = *c;
    do_spawn(c);
    *c = saved;
    req.has_mtx = false;
    spawn_result(c, name, req.actor);
    return req.actor;
}

// The heap our actors are created in (do_spawn) and sead::Heap virtuals on it (game thread). sead::Heap: name
// SafeString text at +0x28, parent at +0x30; vtable slots 14 getSize, 15 getFreeSize, 16 getMaxAllocatableSize(align).
static uint64_t spawn_heap(void) {
    uint64_t h = LD64(A_HEAPS_SLOT);
    h = h >= GUEST_LO && h < GUEST_HI ? LD64(h) : 0;
    h = h >= GUEST_LO && h < GUEST_HI ? LD64(h + 0x28) : 0;
    return h >= GUEST_LO && h < GUEST_HI ? h : 0;
}
static uint64_t heap_call(Cpu *c, uint64_t heap, int slot, uint64_t arg) {
    uint64_t vt = LD64(heap);
    if (vt < GUEST_LO || vt >= GUEST_HI) return 0;
    Cpu saved = *c;
    c->x[0] = heap; c->x[1] = arg;
    guest_call(c, LD64(vt + (uint64_t)slot * 8));
    uint64_t r = c->x[0];
    *c = saved;
    return r;
}
static volatile int heap_dev;  // coop heap: one report to the log
static bool heap_name(uint64_t h, char *out, size_t n) {
    uint64_t nm = LD64(h + 0x28);
    if (nm < GUEST_LO || nm >= GUEST_HI) return false;
    snprintf(out, n, "%.40s", (const char *)(uintptr_t)nm);
    return true;
}
static void heap_line(Cpu *c, const char *why, const char *what, uint64_t h) {
    char name[48] = "?";
    heap_name(h, name, sizeof name);
    LOG("[coop] heap %s: %s %llx \"%s\" size %llu KB free %llu KB biggest block %llu KB\n", why, what,
        (unsigned long long)h, name, (unsigned long long)heap_call(c, h, 14, 0) >> 10,
        (unsigned long long)heap_call(c, h, 15, 0) >> 10, (unsigned long long)heap_call(c, h, 16, 8) >> 10);
}
// The spawn heap, and (actor != 0) the heaps the actor points to in its first 0x800 bytes: pointers to an object with
// the spawn heap's vtable or whose name is the actor's name (to find the actor's own heap).
static void heap_report(Cpu *c, const char *why, uint64_t actor, const char *aname) {
    uint64_t h = spawn_heap();
    if (!h) return;
    heap_line(c, why, "spawn heap", h);
    if (actor < GUEST_LO || actor >= GUEST_HI) return;
    uint64_t vt = LD64(h);
    for (uint64_t off = 0; off < 0x800; off += 8) {
        uint64_t v = LD64(actor + off);
        if (v < GUEST_LO || v >= GUEST_HI || (v & 7)) continue;
        uint64_t vv = LD64(v);
        char name[48];
        bool same = vv == vt;
        bool named = aname && heap_name(v, name, sizeof name) && !strncmp(name, aname, strlen(aname));
        if (!same && !named) continue;
        char what[40];
        snprintf(what, sizeof what, "actor+0x%llx%s", (unsigned long long)off, same ? "" : " (other vtable)");
        if (same) heap_line(c, why, what, v);
        else LOG("[coop] heap %s: %s %llx \"%s\" vtable %llx\n", why, what, (unsigned long long)v, name, (unsigned long long)vv);
    }
}

// (2026-11-14) Hyrule Castle / final boss hooks used by earlier parts (coop/castle.inc)
static bool castle_on(void);
static bool castle_whole(const float a[3], const float b[3]);
static float castle_enemy_radius(const char *name, float def);
static bool castle_flag_private(const char *actor);
static bool castle_item_own(const char *item);
static void castle_named(void);
static int castle_story(const char *flow, const char *entry);
static float castle_story_range(const char *flow, const char *entry, float def);
static void castle_missed(const char *flow, const char *entry, const char *why);
#define CASTLE_OWN_WAIT 20000000000ull  // (story.inc: how long a castle cutscene waits for our own flow to call it)
// (2026-11-14) Captured Memories hooks used by earlier parts (coop/memories.inc)
static void memories_named(void);
static int memories_story(const char *flow, const char *entry);
static uint64_t memories_actor(Cpu *c, const char *flow, const char *entry, uint64_t def);

static void local_aim(Cpu *c, uint64_t pa, CoopState *s);  // (coop/aim.inc, called from avatar.inc)
// (n4) Every other player's avatar drawn here now (the partner's first) and that player's newest state (fresh: under
// 1 s old): coop/multi.inc rav_update, once a frame after the avatars; used by arrows.inc and enemies.inc.
struct RemoteAv { uint64_t actor; int pid; bool partner, fresh; CoopState rs; };
static struct RemoteAv rav[COOP_MAX_PLAYERS];
static int rav_n;
// (coop-multi) Function-local statics of the avatar's receive path that hold per-avatar state, moved here so that
// coop/multi.inc can swap them with the rest of an avatar's state (one avatar per extra player).
static struct {
    uint64_t avatar_last_ns;                                        // avatar_frame
    float lead_y, lead_vy; uint64_t lead_ns; bool lead_have;        // avatar_frame (vertical lead, 2026-11-21)
    bool uw_zeroed[1 + COOP_ARMOR_SLOTS]; uint64_t uw_was[1 + COOP_ARMOR_SLOTS];  // astree_upper_weight
    bool ha_set[1 + COOP_ARMOR_SLOTS]; uint64_t ha_was[1 + COOP_ARMOR_SLOTS];     // astree_horse_aim
    float ha_w01_was[1 + COOP_ARMOR_SLOTS];
    float rh_last[3]; bool rh_have_last;                            // horse_reins_hands
    uint64_t hf_twin_ns; bool hf_twin; uint64_t hf_last_ns;         // horse_frame
    uint8_t hf_shown_coat, hf_shown_socks; unsigned hf_tick;
    unsigned cf_last; bool cf_armed; uint8_t cf_seq_was; uint64_t cf_h;  // charge_fx
    uint64_t rp_refire_ns;                                          // ripple_frame
    bool fx_swim_fix;                                               // avatarfx.inc (per avatar, swapped)
    struct { uint64_t actor; bool swim; unsigned splashes; } af_s;  // avatarfx_frame
    uint16_t st_was_as; uint8_t st_was_seq;                         // steps_frame
    uint16_t sw_was[2]; uint8_t sw_was_seq[2]; int sw_was_anim; uint8_t sw_was_anim_seq;  // swings_frame
    int ss_was; uint64_t ice_h, fire_h;                                           // status_frame
    uint32_t status_unit; bool fire_shown, ice_shown;
} avl = {.sw_was_anim = -1};
static void environment_publish(Cpu *c, CoopState *s);
static bool elemarrow_on(void);
static uint8_t elemarrow_publish(uint64_t pa);
#include "coop/avatar.inc"  // the other player's avatar: local state sent, animation states, death rule, gear and armor puppets
#include "coop/magbeam.inc"  // the native Magnesis beam on each remote avatar

#include "coop/enemies.inc"  // shared enemies: actor list, one health bar, run-time enemies, thaw, hit reaction

#include "coop/horse.inc"  // (2026-11-14) the other player's horse under the riding avatar; ours sent
#include "coop/aim.inc"  // (2026-11-14) the avatar's upper body follows the other player's aim
#include "coop/cyclefx.inc"  // (2026-11-20) the partner's Master Cycle puppet: wheels roll, handlebar steers
#include "coop/ragdoll.inc"  // (2026-11-15, protocol 48) the avatar takes the other player's ragdoll pose
#include "coop/grip.inc"  // (2026-11-21) the avatar's wrist turned so a held two-handed weapon sits in the hand

#include "coop/world.inc"  // time of day; shared chests, items, progress and quest flags

#include "coop/gate.inc"  // shrine entrance, tower proximity gate and leaving a shrine together

#include "coop/objects.inc"  // physics wrapper and shared physics objects

#include "coop/switches.inc"  // floor switches, crystals, torches, pinwheels, electric switches

#include "coop/story.inc"  // starting events by name and shared cutscenes

#include "coop/pvp.inc"  // friendly damage between the players, wind and blasts reaching the other player
#include "coop/elemarrow.inc"  // elemental arrow reactions and friendly damage budget
#include "coop/grass.inc"  // (2026-11-14) grass the other player's blade cuts, cut in our world too

#include "coop/drops.inc"  // dropped / thrown items shared between the players, thrown weapons hitting the partner
#include "coop/dishgive.inc"  // cooked dishes given to the nearby world partner
#include "language.h"
#include "coop/dishinv.inc"  // (DISHINV) "Dar a <partner>" in the pouch's own dish window

#include "coop/arrows.inc"  // (protocol 35) our arrows hitting the other player
#include "coop/shots.inc"  // (protocol 51) enemy rocks / fireballs / ice and shock balls hitting the other player

#include "coop/dev_probes.inc"  // dev probes run on the game thread (rbscan, rbmove, setflag, flag)

#include "coop/beast.inc"  // (2026-11-12) Divine Beasts: terminal count, s32 game flags
#include "coop/weather.inc"  // host weather and midnight events

#include "coop/gyro.inc"  // (2026-11-14) motion-control shrine platforms

#include "coop/runes.inc"  // (2026-11-14, protocol 43) Stasis target (objects.inc does the sync) and Cryonis blocks
#include "coop/stasisfx.inc"  // Stasis chains and strike arrow for each remote player
#include "coop/champ.inc"  // (protocol 89) Champion ability effects on each remote avatar

#include "coop/castle.inc"  // (2026-11-14) Hyrule Castle, the final boss and the ending

#include "coop/memories.inc"  // (2026-11-14) Captured Memories: memory spots watched together, found flags shared

#include "coop/mapicon.inc"  // (2026-11-15) the other player as a star stamp on our map
#include "coop/trees.inc"  // (2026-11-15, protocol 54) trees cut down in both games
#include "coop/critters.inc"  // (2026-11-16, protocol 56) placed birds / fish / insects caught in both games
#include "coop/fire.inc"  // (2026-11-19, protocol 82) campfires, cooking pots, torch stands lit in both games
#include "coop/grassfire.inc"  // (2026-11-19, protocol 83) grass set on fire in both games
#include "coop/elemfoe.inc"
#include "coop/water.inc"  // (2026-11-19, protocol 86) shrine water wheels, water levels, timed switches
#include "coop/beams.inc"  // (2026-11-19, protocol 90) Guardian targeting lasers and beam shots in every game
#include "coop/rails.inc"  // (2026-11-21, protocol 94) shrine rail lifts in step: the guest's follow the host's

#include "coop/doors.inc"  // (2026-11-21) house doors the other player opens open here too
#include "coop/multi.inc"  // (coop-multi) one avatar per extra player (n4: after objects.inc, its bodies are swapped too)
#include "coop/looks.inc"  // (coop-looks, protocol 65) tunic colour per player, names above the other players' heads
#include "coop/mapmark.inc"  // (coop-mapmark, protocol 95) partner arrows on the map, shared map markers
#include "coop/evcalm.inc"  // (2026-11-18) the avatars during our events: animated, no pushing, hidden in shared cutscenes

#include "coop/npc.inc"  // (2026-11-20) townspeople in the same places for both players (rides in the enemy packet)
#include "coop/enemies_frame.inc"  // per-frame work of the shared enemies (uses the world and gate code above)
#include "coop/fauna.inc"  // (2026-11-19, protocol 80) birds, fish, insects: the host's are the guest's while close
#include "coop/wildlife.inc"
#include "coop/splitaudio.inc"
#include "coop/splithud.inc"  // (2026-11-27) split map: this game's player line for the empty quarter's map  // (2026-11-21) split screen player 2 on shared speakers: quiet near player 1

// Start of every game frame, on the game's main thread (runtime/hle/framepace.c rt_vfr_update).
void rt_coop_game_frame(void) {
    static bool net_started;
    if (!net_started) {
        net_started = true;
        coop_net_init();  // (the SAVAGEWIND_COOP_* environment; without it the co-op menu starts the network later)
        if (coop_net_on() && !getenv("SAVAGEWIND_COOP_NAME") && g_settings.coop_name[0]) coop_net_set_name(g_settings.coop_name);
        multi_snapshot();  // (multi: the avatar state as at start)
    }
    puppet_frame();
    Cpu *gc = kernel_current_cpu();
    bool active = gc && coop_net_on() && code_matches();
    if (!active) __atomic_store_n(&lightning_mode, LIGHTNING_NATIVE, __ATOMIC_RELEASE);
    if (active) {
        spawn_hold_frame(plat_now_ns());  // (first: no creations while an event / load runs, crash report 21)
        bloodmoon_frame(gc);
        publish_local(gc);
        splithud_frame();  // (split map: after publish_local, which keeps local_in_world current)
        coop_weather_frame(gc);
        lightning_frame(gc);
        evcalm_frame();  // (after publish_local: our Link's AS of this frame; before the avatars)
        evanim_frame();  // (coop/evcalm.inc: avatars keep animating while an event runs here)
        evanim_dev_frame(gc);
        horse_publish(gc);  // (the horse we ride, sent with the state packet)
        avatar_frame(gc);
        horse_frame(gc);    // (the other player's horse; seats the avatar on it: before armor / gear follow the avatar)
        aim_frame(gc);      // (2026-11-14: the avatar's spine follows the other player's aim)
        ragdoll_frame(gc);  // (2026-11-15: our ragdoll pose sent; the other player's on the avatar)
        multi_frame(gc);    // (coop-multi: the avatars of the players other than the partner)
        doors_dev_frame(gc);  // (dev `coop door <AS>`)
        rav_update();       // (n4: every remote avatar, for arrows and enemy targets)
        looks_frame(gc);    // (coop-looks: our tunic colour out + on our own Link; the name tags placed + occluded)
        (void)enemy_hp_pct();  // (ENEMYHP: our enemy life setting out when hosting, also with no enemy near)
        asdev_frame(gc, avatar.actor && avatar_alive() && avatar.frames >= 6 ? avatar.actor : 0);
        {
            CoopState rs;
            double rage;
            bool have = coop_net_get_remote(&rs, &rage);
            armor_frame(gc, &rs, have);
            bool weapon_placed = gear_frame(gc, &rs, have);
            fire_torch_frame(gc, &rs, have && rage < 1.0, weapon_placed);
            grip_frame(gc, &rs, have);  // (2026-11-21, HOLDWRIST: after the gear, the wrist follows the weapon)
            magbeam_frame(gc, &rs, have && rage < 0.5);
            stasisfx_frame(gc, &rs, have && rage < 0.5);
            champ_frame(gc);  // (89) the partner's Champion ability effects on the avatar
            hurt_flash_frame(gc, &rs, have);  // (2026-11-15, protocol 50) the red flash when the other player is hurt
            ears_frame(gc, &rs, have);     // (2026-11-11) a hood folds the avatar's ears
            doors_frame(gc, have ? avatar.actor : 0);  // (2026-11-21) doors the other player opens open here too
            glidefx_frame(gc, &rs, have);  // (2026-11-11) the paraglider's wind while the avatar glides
            avatarsnd_frame(gc, &rs, have);  // (2026-11-14) footsteps, swings, status effects (after the gear is placed)
            if (look_report) look_report = 0, look_log(&rs, have);
        }
        enemies_frame(gc);
        stasisfoe_frame(gc);
        drops_frame(gc);  // (before world_frame: its pouch diff asks drops_owns_gain)
        arrows_frame(gc);
        shots_frame(gc);
        shots_view(gc);  // (protocol 52) the other game's enemy shots shown here
        beams_frame(gc);  // (protocol 90) Guardian lasers / beams sent and shown
        shotprobe_frame();  // (dev)
        if (player_info_ptr()) hitwho_frame(LD64(player_info_ptr() + 0x60));  // (dev, with shotprobe)
        dishinv_frame(gc);  // (before dish_frame: a give chosen in the dish window starts there)
        dish_frame(gc);
        world_frame(gc);
        arrow_fly_frame(gc);  // (protocol 74) partner's flying arrows (after world_frame: same-frame spawn)
        revive_frame(plat_now_ns());
        leave_watch();
        leave_follow(gc);
        setflag_frame(gc);
        beast_frame(gc);
        castle_frame(gc);
        memories_frame(gc);
        rbscan_frame(gc);
        bodyfind_frame();
        gyro_frame(gc);  // (before objects_frame: it asks gyro_partner_tilting)
        objects_frame(gc);
        rails_frame(gc);
        // (after objects_frame: rb_set_transform only updates body+0x1a0 on the next physics step, so running rbmove
        // before objects_frame let the active follower branch overwrite a test move on the same frame — e.g. when
        // Dungeon034's barrel rode the moving platform at 3 m/s while the guest pushed it)
        rbmove_frame(gc);
        ice_frame(gc);  // (Cryonis blocks: runes.inc)
        stasis_cd_frame(gc, plat_now_ns());  // (n6: our rune's recharge after showing the other player's Stasis)
        collide_frame(gc);
        horse_body_frame(gc);  // (2026-11-14: the partner's horse is solid)
        pvp_frame(gc);
        grass_frame(gc);  // (2026-11-14: the other player's sword cuts our grass)
        switch_dev_frame();
        switches_frame(gc);
        waterpuz_frame(gc);  // (2026-11-19: shrine water wheels, water levels, timed switches)
        event_dev_frame(gc);
        story_frame(gc);
        mapicon_frame(gc);  // (2026-11-15: the partner as a star on the map)
        mapmark_frame(gc);  // (2026-11-21: the partner as an arrow on the map; shared map markers)
        trees_frame(gc);  // (2026-11-15: trees cut down in both games)
        critters_frame(gc);  // (2026-11-16: placed critters caught / killed in both games)
        fire_frame(gc);  // (2026-11-19: campfires / cooking pots lit or put out in both games)
        grassfire_frame(gc);  // (2026-11-19: grass set on fire in both games)
        wildlife_frame(gc);
        fauna_frame(gc);  // (2026-11-19) small animals shared (after wildlife_frame: its old roster stands down)
        time_frame();
        splitaudio_frame();  // (coop-splitaudio: player 2 on shared speakers fades out near player 1)
    } else if (gc && bloodmoon.prohibit_tm && code_matches()) {
        bloodmoon_release_day(gc);
    }
    if (enemy_report) {
        enemy_report = 0;
        report_enemies(code_matches() ? gc : NULL);
    }
    if (devcall.pending) {
        devcall.pending = 0;
        Cpu *c = kernel_current_cpu();
        if (c && code_matches()) {
            Cpu saved = *c;
            for (int i = 0; i < 4; i++) c->x[i] = devcall.x[i];
            guest_call(c, devcall.fn);
            uint64_t ret = c->x[0];
            *c = saved;
            LOG("[coop] call %llx -> x0 %llx\n", (unsigned long long)devcall.fn, (unsigned long long)ret);
        }
    }
    if (watch.len) {  // `coop watch <addr> <len>`: log the 4-byte words that changed since the last frame (dev)
        static uint64_t frame;
        frame++;
        if (watch.fresh) { memcpy(watch.prev, (void *)(uintptr_t)watch.addr, watch.len); watch.fresh = 0; }
        int shown = 0;
        for (uint32_t o = 0; o + 4 <= watch.len; o += 4) {
            uint32_t now = LD32(watch.addr + o), was;
            memcpy(&was, watch.prev + o, 4);
            if (now != was && shown++ < 24 && watch.lines < 4000) {
                float f;
                memcpy(&f, &now, 4);
                LOG("[coop] watch f%llu +0x%x: %08x -> %08x (%g)\n", (unsigned long long)frame, o, was, now, f);
                watch.lines++;
            }
        }
        memcpy(watch.prev, (void *)(uintptr_t)watch.addr, watch.len);
    }
    if (as_dev_actor) {
        uint64_t a = as_dev_actor;
        as_dev_actor = 0;
        Cpu *c = kernel_current_cpu();
        if (c && code_matches() && in_guest(a) && in_guest(a + 0x800)) {
            char all[300];
            as_list(c, a, all, sizeof all);
            uint64_t tm = as_layer_time(c, as_layer_rec(a, 0, 0));  // (0.0's frame and rate, dev)
            float fr[2] = {-1, -1};
            if (tm) memcpy(fr, (void *)(uintptr_t)(tm + 4), 8);
            LOG("[coop] as %llx: %s| signals %08x | 0.0 time+4 %.2f +8 %.2f\n", (unsigned long long)a, all, LD32(a + 0x71c),
                fr[0], fr[1]);
            float b[12];  // (chests, dev: the lid bone "Cover" - its axes against the chest's up, and its height)
            if (bone_matrix(c, a, "Cover", b)) {
                const float *am = (const float *)(uintptr_t)(a + ACTOR_MTX);
                LOG("[coop] as %llx: lid bone Cover axes.up %.2f %.2f %.2f height %.2f\n", (unsigned long long)a,
                    b[0] * am[1] + b[4] * am[5] + b[8] * am[9], b[1] * am[1] + b[5] * am[5] + b[9] * am[9],
                    b[2] * am[1] + b[6] * am[5] + b[10] * am[9], b[7] - am[7]);
            }
        }
    }
    if (equip_dev) {  // `coop equip` (dev): the local player's six equipment handles, resolved to actor names
        equip_dev = 0;
        Cpu *c = kernel_current_cpu();
        uint64_t pinfo = g_site.player_info ? LD64(g_site.player_info) : 0;
        uint64_t pa = pinfo ? LD64(pinfo + 0x60) : 0, eq = in_guest(pa) ? LD64(pa + PLAYER_EQUIP) : 0;
        if (c && code_matches() && in_guest(eq)) {
            if (!outfit_out) outfit_out = (uint64_t)(uintptr_t)hle_alloc(0x20);
            for (int i = 1; i <= 6; i++) {
                memset((void *)(uintptr_t)outfit_out, 0, 0x20);
                Cpu saved = *c;
                c->x[0] = eq + (uint64_t)i * 0x10; c->x[1] = outfit_out;
                guest_call(c, A_EQUIP_GET);
                *c = saved;
                uint64_t it = LD64(outfit_out + 8);
                LOG("[coop] equip +0x%x: id %d -> %llx %.40s\n", i * 0x10, (int)LD32(eq + (uint64_t)i * 0x10 + 8),
                    (unsigned long long)it, in_guest(it) ? (const char *)(uintptr_t)(it + 0x1c) : "-");
            }
        }
    }
    if (target_dev.actor) {  // `coop target <enemy> avatar|player|show` (dev, combat2): the enemy's target link
        uint64_t a = target_dev.actor;
        target_dev.actor = 0;
        Cpu *c = kernel_current_cpu();
        if (c && code_matches() && in_guest(a) && in_guest(a + 0x1000)) {
            uint64_t before = enemy_target_actor(a);
            bool av = avatar.actor && avatar_alive();
            if (target_dev.mode == 1 && av) enemy_set_target(c, a, avatar.actor);
            else if (target_dev.mode == 2) enemy_set_target(c, a, 0);
            uint64_t after = enemy_target_actor(a);
            LOG("[coop] target %llx (%.40s): was %.40s, now %.40s (link %llx id %x)%s\n", (unsigned long long)a,
                (const char *)(uintptr_t)(a + 0x1c), before ? (const char *)(uintptr_t)(before + 0x1c) : "-",
                after ? (const char *)(uintptr_t)(after + 0x1c) : "-", (unsigned long long)LD64(a + ENEMY_TARGET_LINK),
                LD32(a + ENEMY_TARGET_LINK + 8), target_dev.mode == 1 && !av ? " (no avatar)" : "");
        }
    }
    if (inject_dev.actor) {  // `coop inject <actor> <damage> [kind]` (dev): fake a received hit
        uint64_t a = inject_dev.actor;
        inject_dev.actor = 0;
        if (in_guest(a)) {
            enemy_inject(a, inject_dev.damage, inject_dev.kind);
            LOG("[coop] inject %llx damage %d kind %d\n", (unsigned long long)a, inject_dev.damage, inject_dev.kind);
        }
    }
    if (flinch_dev_actor) {
        uint64_t a = flinch_dev_actor;
        flinch_dev_actor = 0;
        Cpu *c = kernel_current_cpu();
        if (c && code_matches()) { enemy_flinch(c, a, 1); LOG("[coop] flinch %llx done\n", (unsigned long long)a); }
    }
    if (code_matches()) arrowtrace_frame();
    if (heap_dev) {
        heap_dev = 0;
        Cpu *c = kernel_current_cpu();
        if (c && code_matches()) { heap_report(c, "now", 0, NULL); for (int k = 0; k < COOP_ARMOR_SLOTS; k++) if (armor_pup[k].actor && pup_alive(&armor_pup[k])) heap_report(c, "now", armor_pup[k].actor, armor_pup[k].name); }
    }
    if (actlist.pending) {
        actlist.pending = 0;
        Cpu *c = kernel_current_cpu();
        if (c && code_matches()) list_actors(c);
    }
    if (puppet.show_body && req.actor) {
        puppet.show_body = 0;
        Cpu *c = kernel_current_cpu();
        if (c && code_matches()) show_body(c, req.actor);
        LOG("[coop] showbody actor %llx\n", (unsigned long long)req.actor);
    }
    if (puppet.play && (req.actor || puppet.anim_actor)) {
        puppet.play = 0;
        uint64_t target = puppet.anim_actor ? puppet.anim_actor : req.actor;
        Cpu *c = kernel_current_cpu();
        if (c && code_matches()) play_anim(c, target, puppet.anim, puppet.anim_slot);
        LOG("[coop] anim %s slot %d on actor %llx\n", puppet.anim, puppet.anim_slot, (unsigned long long)target);
    }
    if (refuse_dev) {  // `coop refuse` (dev): a creation the game refuses, as when its memory is short
        refuse_dev = 0;
        Cpu *c = kernel_current_cpu();
        float p[3];
        if (c && code_matches() && player_pos(p)) {
            p[1] -= 50;
            uint64_t a = spawn_now(c, "Dm_CoopNoSuchActor", p);
            LOG("[coop] refuse (dev): creation -> %llx (held now: %d)\n", (unsigned long long)a, (int)spawn_hold());
        }
    }
    if (puppet.despawn) {
        puppet.despawn = 0;
        Cpu *c = kernel_current_cpu();
        uint64_t a = avatar.actor ? avatar.actor : req.actor;
        if (c && code_matches() && a) {
            forget_actor(a);
            bool ok = despawn_now(c, a, ACTOR_DELETE_REASON);
            LOG("[coop] despawn %llx -> %d\n", (unsigned long long)a, (int)ok);
        }
    }
    if (puppet.outfit) {
        puppet.outfit = 0;
        Cpu *c = kernel_current_cpu();
        uint64_t a = avatar.actor ? avatar.actor : req.actor;
        if (c && code_matches() && a) apply_outfit(c, a);
    }
    if (puppet.armor) {
        puppet.armor = 0;
        Cpu *c = kernel_current_cpu();
        uint64_t parent = puppet.armor_parent ? puppet.armor_parent : (avatar.actor ? avatar.actor : req.actor);
        if (c && code_matches() && parent) {
            float p[3] = { *(float *)(uintptr_t)(parent + ACTOR_MTX + 0x0c),
                           *(float *)(uintptr_t)(parent + ACTOR_MTX + 0x1c),
                           *(float *)(uintptr_t)(parent + ACTOR_MTX + 0x2c) };
            uint64_t child = spawn_now(c, puppet.armor_name, p);
            if (child) {
                puppet.armor_last = child;
                Cpu saved = *c;
                c->x[0] = child;
                c->x[1] = parent;
                guest_call(c, A_ATTACH);
                *c = saved;
                write_mtx(child + ACTOR_MTX, p, 0);
                write_mtx(child + ACTOR_MTX2, p, 0);
                // +0x4e0 is the model controller; its first child (+0x18) is the Model the skeleton link takes.
                uint64_t amc = LD64(child + ACTOR_MODEL), pmc = LD64(parent + ACTOR_MODEL);
                uint64_t am = amc ? LD64(amc + 0x18) : 0, pm = pmc ? LD64(pmc + 0x18) : 0;
                LOG("[coop] armor models: armor %llx (ctrl %llx) parent %llx (ctrl %llx)\n",
                    (unsigned long long)am, (unsigned long long)amc, (unsigned long long)pm, (unsigned long long)pmc);
                // Do NOT call FUN_7100f7658c (A_MODEL_SET_PARENT) or write model+0xf0: that field is the model's
                // render scene (it locks scene+0x5d98), not a parent model; passing a model hung the game. The
                // armor only renders when FUN_7101173368 accepts its wearer (the player or a PauseMenuPlayer),
                // `coop armor <name> player` shows it working on the real player.
                (void)am;
                (void)pm;
                LOG("[coop] armor %s -> %llx attached to %llx (model %llx parent model %llx)\n",
                    puppet.armor_name, (unsigned long long)child, (unsigned long long)parent,
                    (unsigned long long)am, (unsigned long long)pm);
            } else {
                LOG("[coop] armor %s refused\n", puppet.armor_name);
            }
        }
    }
    if (puppet.armor_tick && puppet.armor_last) {
        Cpu *c = kernel_current_cpu();
        uint64_t vt = LD64(puppet.armor_last);
        if (c && code_matches() && vt >= 0x7000000000ull && vt < 0x7100000000ull) {
            Cpu saved = *c;
            c->x[0] = puppet.armor_last;
            guest_call(c, LD64(vt + 0x238));  // the armor class's per-frame update (FUN_710117274c)
            *c = saved;
            static unsigned n;
            if (n++ % 120 == 0)
                LOG("[coop] armortick %llx fn %llx alpha %.2f\n", (unsigned long long)puppet.armor_last,
                    (unsigned long long)LD64(vt + 0x238), *(float *)(uintptr_t)(puppet.armor_last + 0x4ec));
        } else {
            static unsigned m;
            if (m++ % 120 == 0) LOG("[coop] armortick skipped (vt %llx)\n", (unsigned long long)vt);
        }
    }
    if (req.state != COOP_QUEUED) return;
    Cpu *c = kernel_current_cpu();
    if (!c) return;
    if (!code_matches()) {
        snprintf(req.why, sizeof req.why, "game code differs from 1.9.0 (version %s)", g_game_version);
        req.state = COOP_FAILED;
        return;
    }
    Cpu saved = *c;
    do_spawn(c);
    *c = saved;
}

#include "coop/commands.inc"  // the `coop` dev command and reset
