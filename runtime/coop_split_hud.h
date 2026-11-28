// (split map) What the empty quarter of a three-player split screen shows: a map with every player's dot and their
// hearts and stamina (runtime/ui/coop_banner.mm). Each split-screen game writes its own player's line into a block of
// memory shared by the games of one window (Windows: a named file mapping "Local\SavageWindSplitHud-<player 1's PID>",
// runtime/coop_split.cpp); the banner process reads it. Nothing goes over the network.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPLIT_HUD_MAGIC 0x44554853u  // "SHUD"
#define SPLIT_HUD_VERSION 3  // (2: the map markers; 3: each player's activated Sheikah Towers)
#define SPLIT_HUD_MARKS 128
#define SPLIT_HUD_PLAYERS 4

enum { SPLIT_WHERE_NONE, SPLIT_WHERE_FIELD, SPLIT_WHERE_SHRINE, SPLIT_WHERE_BEAST, SPLIT_WHERE_OTHER };
enum { SPLIT_ZOOM_NEAR, SPLIT_ZOOM_NORMAL, SPLIT_ZOOM_FAR, SPLIT_ZOOM_COUNT };

typedef struct {
    volatile uint32_t seq;   // odd while the line is being written (readers copy it and check seq again)
    uint32_t tick_ms;        // the writer's GetTickCount at the write (a line older than ~3 s: that game is gone)
    char name[32];           // UTF-8, NUL-terminated
    float pos[3];            // world position (metres; the map is x -6000..6000, z -5000..5000, north is -z)
    float fwd[2];            // facing on the map: x and z of the forward direction
    float life, life_max;    // quarter hearts
    float stamina, stamina_max;  // 1000 per stamina wheel
    int32_t where;           // SPLIT_WHERE_*: the overworld, a shrine, a Divine Beast, another area; NONE: loading
    uint32_t towers;         // bit n: MapTower_<n+1> on (that tower's region is revealed); SPLIT_TOWERS_KNOWN: read
} SplitHudPlayer;
#define SPLIT_TOWERS_KNOWN 0x80000000u
#define SPLIT_TOWERS_ALL 0x7fffu  // (the 15 towers)

typedef struct { float x, z; int32_t no; } SplitHudMark;  // a stamp (No 27..35) or a pin (No 37..41) on the map

typedef struct {
    uint32_t magic, version;
    volatile int32_t zoom;   // SPLIT_ZOOM_*: player 1's Options > Co-op > Map zoom
    uint32_t pad;
    SplitHudPlayer p[SPLIT_HUD_PLAYERS];  // [n]: player n + 1
    volatile uint32_t mark_seq;  // the markers on player 1's map (odd while written; shared markers: everyone's)
    int32_t mark_n;
    SplitHudMark marks[SPLIT_HUD_MARKS];
} SplitHud;

// The shared block (NULL: no split screen, or not made yet), and this game's line in it (-1: none: the banner).
SplitHud *coop_split_hud(void);
int coop_split_hud_slot(void);

#ifdef __cplusplus
}
#endif
