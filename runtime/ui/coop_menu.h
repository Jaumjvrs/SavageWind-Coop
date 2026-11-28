// Options > Co-op (runtime/ui/coop_menu.mm): the co-op setup page (mode, name, host address, port, room code, Start)
// and the lobby page (who is in the session, connected or waiting, ping, where). game_options.mm turns these rows into
// rows of the game's own Options screen and asks this file for their texts and what A does.
// SAVAGEWIND_COOP_MENU=0 leaves the page (and the title menu's Co-op button) out.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// (coop-menu) SETUP / LOBBY are the two top levels; the others are their sub-menus (opened from a row, B comes back).
// Split and Options hold both the setup's rows and the lobby's own (coop_menu_shown keeps the ones that fit the state).
enum { COOP_PAGE_SETUP, COOP_PAGE_LOBBY, COOP_PAGE_ONLINE, COOP_PAGE_SPLIT, COOP_PAGE_OPTS, COOP_PAGE_DISH };
enum { COOP_ROW_TOGGLE = 0, COOP_ROW_CHOICE = 1, COOP_ROW_BUTTON = 2 };  // (game_options.mm KIND_*)

typedef struct {
    int id;      // COOP_R_* (coop_menu.mm)
    int page;    // COOP_PAGE_*
    int kind;    // COOP_ROW_*
    const char *title, *desc;  // English (tr() is applied when shown)
} CoopMenuRow;

// What A on a row asks of the Options screen
enum { COOP_PRESS_NONE, COOP_PRESS_REDRAW, COOP_PRESS_REOPEN, COOP_PRESS_LOBBY, COOP_PRESS_SETUP, COOP_PRESS_SUBMENU };

int coop_menu_rows(const CoopMenuRow **rows);     // every row of both pages, in page order
void coop_menu_setup(int max_choices);           // refresh the save list before building the setup page
bool coop_menu_shown(int id);                     // at the page's opening: this row is on it
int coop_menu_count(int id);                      // COOP_ROW_CHOICE: number of values
const char *coop_menu_label(int id, int v);       // a choice's value v; v = -1: a button's text
int coop_menu_get(int id);
void coop_menu_set(int id, int v);                // (the caller saves the settings)
const char *coop_menu_title(int id);              // a title made when shown (NULL: the row's own)
const char *coop_menu_desc(int id);               // a description made when shown (NULL: the row's own)
bool coop_menu_off(int id, int v);                // greyed: can't be used (button) / picked (choice value v)
int coop_menu_target(int id);                     // a COOP_PRESS_SUBMENU row: the COOP_PAGE_* it opens
int coop_menu_press(int id);                     // A on a button row: COOP_PRESS_*
bool coop_menu_lobby(void);                       // the network runs: Co-op shows the lobby
bool coop_menu_tick(void);                        // (lobby open, each frame) true: time to redraw its rows
const char *coop_menu_dish_notice(bool recent_only);

#ifdef __cplusplus
}
#endif
