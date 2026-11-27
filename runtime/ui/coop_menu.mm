// Options > Co-op: set up and start a co-op session from inside the game (title menu: Co-op; pause menu: System >
// Options > Co-op), and the lobby once it runs. The rows are drawn by the game's own Options screen
// (game_options.mm); this file gives their texts and acts on A.
//
// (coop-upnp) Setup page: Open router port automatically (settings coop_upnp); lobby page, while hosting: Router port,
// the router's answer (runtime/coop_upnp.h) and A: copy the internet address.
// Setup page: save profiles, Mode (Solo / Host / Join), Your name, Host address (Join), Port, Room code, Start. Text rows open the
// on-screen keyboard (menu.mm menu_keyboard_run_titled). The choices are kept in the settings file (coop_* keys).
// Start runs coop_net_start (runtime/coop_net.c) and shows the lobby.
// Lobby page: the network's state, one row per player (name, connected / waiting, ping, where), Refresh, Leave.
// Leave stops the network and starts the game again at its title screen (rt_request_reboot(1)): every co-op figure
// and shared state goes with the game's own reset (coop.c coop_reset), which is how a language change restarts too.
//
// When the SAVAGEWIND_COOP_* environment started co-op (Co-op Host / Join .bat, test scripts) the page shows the lobby.
// Threads: everything here runs on the game's thread, inside the Options screen's functions.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <string>
#include <vector>

#include "coop_menu.h"
#include "i18n.h"
#include "saves_tab.h"

extern "C" {
#include "../coop_net.h"
#include "../coop_split.h"
#include "../coop_upnp.h"
#include "../coop_dish.h"
#include "../game_place.h"
#include "../items.h"
#include "../services/save_profiles.h"
#include "../settings.h"
#include "../split_audio.h"
void rt_request_reboot(int kind);  // runtime/main.c (1: the game again, from its title screen)
int menu_keyboard_run_titled(const char *title, const char *initial, int max_len, char *out, int cap);  // menu.mm
}

namespace {

enum {
    R_SAVE, R_USE_SAVE, R_NEW_SAVE,
    R_MODE, R_NAME, R_NAMES, R_TINT, R_MARKS, R_HOST, R_PORT, R_UPNP, R_CODE, R_START,
    R_STATUS, R_ROUTER, R_LAN, R_PLAYER0, R_PLAYER_LAST = R_PLAYER0 + COOP_MAX_PLAYERS - 1,
    R_DISH_PICK, R_DISH_GIVE, R_NAMES_L, R_TINT_L, R_MARKS_L, R_REFRESH, R_LEAVE,
    R_SPLIT_LAYOUT, R_SPLIT_PAD, R_SPLIT_AUDIO, R_SPLIT_DEVICE, R_SPLIT,
    R_SPLIT_LAYOUT_L, R_SPLIT_PAD_L, R_SPLIT_AUDIO_L, R_SPLIT_DEVICE_L, R_SPLIT_L,
};

const CoopMenuRow k_rows[] = {
    {R_SAVE, COOP_PAGE_SETUP, COOP_ROW_CHOICE, "Save", ""},
    {R_USE_SAVE, COOP_PAGE_SETUP, COOP_ROW_BUTTON, "Use this save",
     "Restarts at the title screen. Unsaved progress is lost."},
    {R_NEW_SAVE, COOP_PAGE_SETUP, COOP_ROW_BUTTON, "New save",
     "Creates an empty save. Select it, then choose New Game at the title screen."},
    {R_MODE, COOP_PAGE_SETUP, COOP_ROW_CHOICE, "Mode",
     "Solo: play alone. Host: others join you. Join: play in a host's game."},
    {R_NAME, COOP_PAGE_SETUP, COOP_ROW_BUTTON, "Your name", "The name the other players see. A: type it."},
    {R_NAMES, COOP_PAGE_SETUP, COOP_ROW_TOGGLE, "Names above players", "Shows the other players' names above their heads."},
    {R_TINT, COOP_PAGE_SETUP, COOP_ROW_CHOICE, "Your tunic colour", "The colour the other players see your Link in."},
    {R_MARKS, COOP_PAGE_SETUP, COOP_ROW_CHOICE, "Shared map markers",
     "Which markers you place on the map the others get too. Pins only: pins are shared, stamps stay yours."},
    {R_HOST, COOP_PAGE_SETUP, COOP_ROW_BUTTON, "Host address",
     "Join: the host's IP address or network name, no port. A: type it."},
    {R_PORT, COOP_PAGE_SETUP, COOP_ROW_BUTTON, "Port",
     "UDP port. Host: the one others use. Join: the host's. A: type it."},
    {R_UPNP, COOP_PAGE_SETUP, COOP_ROW_TOGGLE, "Open router port automatically",
     "Host: your router (UPnP) forwards this port to this PC while you host."},
    {R_CODE, COOP_PAGE_SETUP, COOP_ROW_BUTTON, "Room code",
     "Same word for every player; other codes can't join. A: type it."},
    {R_START, COOP_PAGE_SETUP, COOP_ROW_BUTTON, "Start", "Starts co-op with these settings."},
    {R_STATUS, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Co-op", ""},
    {R_ROUTER, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Router port", ""},
    {R_LAN, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Same Wi-Fi address",
     "Friends on your Wi-Fi join with this, not the internet address. A: copy."},
    {R_PLAYER0 + 0, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Player", ""},
    {R_PLAYER0 + 1, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Player", ""},
    {R_PLAYER0 + 2, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Player", ""},
    {R_PLAYER0 + 3, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Player", ""},
    {R_PLAYER0 + 4, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Player", ""},
    {R_PLAYER0 + 5, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Player", ""},
    {R_PLAYER0 + 6, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Player", ""},
    {R_PLAYER0 + 7, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Player", ""},
    {R_DISH_PICK, COOP_PAGE_LOBBY, COOP_ROW_CHOICE, "Cooked dish", ""},
    {R_DISH_GIVE, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Give a dish", "Close the pause menu to hand the dish over."},
    {R_NAMES_L, COOP_PAGE_LOBBY, COOP_ROW_TOGGLE, "Names above players", "Shows the other players' names above their heads."},
    {R_TINT_L, COOP_PAGE_LOBBY, COOP_ROW_CHOICE, "Your tunic colour", "The colour the other players see your Link in."},
    {R_MARKS_L, COOP_PAGE_LOBBY, COOP_ROW_CHOICE, "Shared map markers",
     "Which markers you place on the map the others get too. Pins only: pins are shared, stamps stay yours."},
    {R_REFRESH, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Refresh", "Updates the names here (they do not change by themselves)."},
    {R_LEAVE, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Leave co-op",
     "Leaves and restarts at the title screen (unsaved progress is lost)."},
    {R_SPLIT_LAYOUT, COOP_PAGE_SETUP, COOP_ROW_CHOICE, "Split-screen layout", ""},
    {R_SPLIT_PAD, COOP_PAGE_SETUP, COOP_ROW_CHOICE, "Player 2 controller", ""},
    {R_SPLIT_AUDIO, COOP_PAGE_SETUP, COOP_ROW_CHOICE, "Player 2 sound", ""},
    {R_SPLIT_DEVICE, COOP_PAGE_SETUP, COOP_ROW_CHOICE, "Player 2 sound device", ""},
    {R_SPLIT, COOP_PAGE_SETUP, COOP_ROW_BUTTON, "Split screen", ""},
    {R_SPLIT_LAYOUT_L, COOP_PAGE_LOBBY, COOP_ROW_CHOICE, "Split-screen layout", ""},
    {R_SPLIT_PAD_L, COOP_PAGE_LOBBY, COOP_ROW_CHOICE, "Player 2 controller", ""},
    {R_SPLIT_AUDIO_L, COOP_PAGE_LOBBY, COOP_ROW_CHOICE, "Player 2 sound", ""},
    {R_SPLIT_DEVICE_L, COOP_PAGE_LOBBY, COOP_ROW_CHOICE, "Player 2 sound device", ""},
    {R_SPLIT_L, COOP_PAGE_LOBBY, COOP_ROW_BUTTON, "Split screen", ""},
};
static_assert(sizeof k_rows / sizeof k_rows[0] == R_SPLIT_L + 1, "one row per id, in id order");

bool split_action(int id) { return id == R_SPLIT || id == R_SPLIT_L; }
bool split_layout(int id) { return id == R_SPLIT_LAYOUT || id == R_SPLIT_LAYOUT_L; }
bool split_pad(int id) { return id == R_SPLIT_PAD || id == R_SPLIT_PAD_L; }
bool split_audio_row(int id) { return id == R_SPLIT_AUDIO || id == R_SPLIT_AUDIO_L; }
bool split_device_row(int id) { return id == R_SPLIT_DEVICE || id == R_SPLIT_DEVICE_L; }
// (split layout and player 2's controller: g_settings.split_layout / split_pad, saved since 0.1.1)
// (coop-splitaudio) Player 2 sound (runtime/split_audio.h) and its device: the playback devices as the page opened
const char *const k_split_audio[SPLIT_AUDIO_COUNT] = {"Off", "Own device", "Shared speakers"};
char g_devices[24][128];
int g_ndevices;
char g_default_device[128];
bool split_audio_allowed(void) {
    const char *v = getenv("SAVAGEWIND_COOP_SPLITAUDIO");
    return !(v && !strcmp(v, "0"));
}
void devices_update(void) {
    g_ndevices = split_audio_devices(g_devices, 24);
    if (!split_audio_default_name(g_default_device, sizeof g_default_device)) g_default_device[0] = 0;
}
// (2026-11-21 review) Windows names are "<kind> (<product>)": "Fones de ouvido (USB Headset)", "Alto-falantes
// (Realtek(R) Audio)", "LG ULTRAGEAR (HD Audio Driver for Display Audio)". The full name did not fit the row; show the
// product in the parentheses, or the part before them when that is a driver description, capped at 20 characters.
const char *device_short(const char *full, std::string &o) {
    std::string s = full, inner, outer = s;
    size_t open = s.find(" ("), close = s.rfind(')');
    if (open != std::string::npos && close != std::string::npos && close > open + 2) {
        inner = s.substr(open + 2, close - open - 2);
        outer = s.substr(0, open);
    }
    o = inner.empty() || inner.find("Driver") != std::string::npos || inner.find("driver") != std::string::npos ? outer : inner;
    if (o.empty()) o = s;
    size_t chars = 0, cut = std::string::npos;
    for (size_t i = 0; i < o.size(); i++) {
        if (((unsigned char)o[i] & 0xC0) == 0x80) continue;  // (UTF-8 continuation byte)
        if (++chars == 20) { cut = i; break; }
    }
    if (cut != std::string::npos) o = o.substr(0, cut) + "\xE2\x80\xA6";  // (…)
    return o.c_str();
}
int device_index(void) {  // the chosen device in g_devices (-1: none of them)
    for (int i = 0; i < g_ndevices; i++)
        if (!strcmp(g_devices[i], g_settings.split_audio_device)) return i;
    return -1;
}
int device_count(void) {  // the devices, and the saved one when it is not connected now (shown last)
    int n = g_ndevices + (g_settings.split_audio_device[0] && device_index() < 0 ? 1 : 0);
    return n > 0 ? n : 1;
}

const char *const k_modes[] = {"Solo", "Host", "Join"};
// (coop-looks) COOP_TINT_* order (coop_net.h)
// (coop-mapmark) settings coop_sharemarks
const char *const k_marks[3] = {"Off", "Pins only", "All"};
const char *const k_tints[COOP_TINT_COUNT] = {"Default", "Red", "Blue", "Green", "Yellow", "Purple", "Black", "White"};
const int DEFAULT_PORT = 47000;

char g_error[160];  // why the last Start / entry failed ("": none)
std::chrono::steady_clock::time_point g_leave_armed;  // Leave was pressed once: a second press within 5 s leaves
std::chrono::steady_clock::time_point g_next_redraw;
std::chrono::steady_clock::time_point g_copied_until;  // (coop-upnp) "Copy address" was pressed: shown for a while
std::chrono::steady_clock::time_point g_lan_copied_until;  // (0.1.1) the same for the Same Wi-Fi address
CoopLobbyEntry g_lobby[COOP_MAX_PLAYERS];
int g_nlobby;
CoopDish g_dishes[COOP_DISH_MAX];
int g_ndishes, g_dish;
std::vector<SpProfile> g_saves;
int g_selected_save;
char g_active_save[SP_NAME_MAX + 1];
char g_new_save[SP_NAME_MAX + 1];  // select this name when the page reopens after creation
std::string g_save_error;
int g_save_error_row;

bool saves_on(void) {
    const char *v = getenv("SAVAGEWIND_COOP_SAVES");
    return !sp_override_active() && (!v || strcmp(v, "0"));
}

const char *save_label(const char *name) {
    if (strcmp(name, SP_DEFAULT_NAME)) return name;
    return tr("Main");
}

int save_failure(int id, SpCode code) {
    std::string hint;
    profile_failure_text(code, &g_save_error, &hint);
    if (!hint.empty()) g_save_error += ". " + hint;
    g_save_error_row = id;
    return COOP_PRESS_REDRAW;
}

int port(void) { return g_settings.coop_port > 0 && g_settings.coop_port < 65536 ? g_settings.coop_port : DEFAULT_PORT; }

void lobby_update(void) { g_nlobby = coop_net_lobby(g_lobby, COOP_MAX_PLAYERS); }

std::string &buf(void) {  // (a few texts are asked for in one go: a ring of them)
    static std::string ring[8];
    static int k;
    return ring[k++ & 7];
}

bool leave_armed(void) { return std::chrono::steady_clock::now() < g_leave_armed; }

// (0.1.1) Nobody has connected yet (no player's data ever arrived): Leave only stops the network and goes back to the
// setup page (another address, Host <-> Join), with no restart: nothing was shared, so there is nothing to undo.
// (The split-screen room does the same when player 2 never started: coop_split.cpp.) SAVAGEWIND_COOP_MENUSTOP=0: off.
bool stop_only(void) {
    const char *v = getenv("SAVAGEWIND_COOP_MENUSTOP");
    if ((v && !strcmp(v, "0")) || !coop_net_on() || coop_net_split_room()) return false;
    uint32_t s = 0, r = 0;
    bool known = false;
    coop_net_stats(&s, &r, &known);
    return r == 0;
}

// A text row: the keyboard over the screen; true: a new text in out
bool ask(const char *title, const char *initial, int max_len, char *out, int cap) {
    char t[96] = "";
    if (!menu_keyboard_run_titled(tr(title), initial, max_len, t, sizeof t)) return false;
    // (leading / trailing blanks: a stray space would make an address or a code differ)
    char *s = t;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    snprintf(out, cap, "%s", s);
    return true;
}

}  // namespace

extern "C" int coop_menu_rows(const CoopMenuRow **rows) {
    *rows = k_rows;
    return (int)(sizeof k_rows / sizeof k_rows[0]);
}

extern "C" bool coop_menu_lobby(void) { return coop_net_on(); }

extern "C" void coop_menu_setup(int max_choices) {
    g_saves.clear();
    g_selected_save = 0;
    g_save_error.clear();
    if (saves_on()) {
        SpProfile *profiles = nullptr;
        int n = sp_list(NULL, &profiles);
        sp_active(NULL, g_active_save, sizeof g_active_save);
        const char *selected = g_new_save[0] ? g_new_save : g_active_save;
        for (int i = 0; i < n; i++) {
            bool pick = !strcmp(profiles[i].name, selected);
            if ((int)g_saves.size() == max_choices) {
                if (pick) g_saves.back() = profiles[i], g_selected_save = max_choices - 1;
                continue;
            }
            if (pick) g_selected_save = (int)g_saves.size();
            g_saves.push_back(profiles[i]);
        }
        free(profiles);
    }
    g_new_save[0] = 0;
}

extern "C" bool coop_menu_shown(int id) {
    if (split_audio_row(id)) devices_update();
    if ((split_audio_row(id) || split_device_row(id)) && !split_audio_allowed()) return false;
    if (id >= R_SPLIT_LAYOUT) {
        const char *enabled = getenv("SAVAGEWIND_COOP_SPLITMENU");
        return (!enabled || strcmp(enabled, "0")) && coop_split_available() && (!coop_net_on() || coop_net_is_host());
    }
    if (id >= R_SAVE && id <= R_NEW_SAVE) return saves_on() && !g_saves.empty();
    if (id == R_ROUTER) {  // (coop-upnp) a host reachable from the internet: the router's answer
        CoopUpnpStatus st;
        coop_upnp_status(&st);
        return coop_net_on() && coop_net_is_host() && !coop_net_split_room() && st.state != COOP_UPNP_IDLE;
    }
    if (id == R_LAN) {  // (0.1.1) any host, router port or not: friends on the same Wi-Fi need this one
        char a[48];
        return coop_net_on() && coop_net_is_host() && !coop_net_split_room() && coop_lan_address(a, sizeof a);
    }
    if (id == R_STATUS) {
        lobby_update();  // the list as the page opens
        g_ndishes = coop_dish_list(g_dishes);
        g_dish = 0;
    }
    if (id == R_DISH_PICK || id == R_DISH_GIVE) return coop_dish_enabled() && g_ndishes > 0;
    if (id >= R_PLAYER0 && id <= R_PLAYER_LAST) return id - R_PLAYER0 < g_nlobby;
    if (id == R_STATUS) g_leave_armed = {}, g_next_redraw = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    return true;
}

extern "C" int coop_menu_count(int id) {
    if (split_layout(id)) return 2;
    if (split_pad(id)) return 4;
    if (split_audio_row(id)) return SPLIT_AUDIO_COUNT;
    if (split_device_row(id)) return device_count();
    if (id == R_DISH_PICK) return g_ndishes > 0 ? g_ndishes : 1;
    if (id == R_SAVE && !g_saves.empty()) return (int)g_saves.size();
    if (id == R_TINT || id == R_TINT_L) return COOP_TINT_COUNT;
    if (id == R_NAMES || id == R_NAMES_L || id == R_UPNP) return 2;
    if (id == R_MARKS || id == R_MARKS_L) return 3;
    return id == R_MODE ? 3 : 1;
}

extern "C" int coop_menu_get(int id) {
    if (split_layout(id)) return g_settings.split_layout == 1;
    if (split_pad(id)) return g_settings.split_pad >= 0 && g_settings.split_pad < 4 ? g_settings.split_pad : 1;
    if (split_audio_row(id)) return g_settings.split_audio >= 0 && g_settings.split_audio < SPLIT_AUDIO_COUNT ? g_settings.split_audio : 0;
    if (split_device_row(id)) return device_index() >= 0 ? device_index() : device_count() - 1;
    if (id == R_DISH_PICK) return g_dish;
    if (id == R_SAVE) return g_selected_save;
    if (id == R_MODE) return g_settings.coop_mode >= 0 && g_settings.coop_mode <= 2 ? g_settings.coop_mode : 0;
    if (id == R_NAMES || id == R_NAMES_L) return g_settings.coop_names ? 1 : 0;
    if (id == R_UPNP) return g_settings.coop_upnp ? 1 : 0;
    if (id == R_MARKS || id == R_MARKS_L) return g_settings.coop_sharemarks >= 0 && g_settings.coop_sharemarks <= 2 ? g_settings.coop_sharemarks : 1;
    if (id == R_TINT || id == R_TINT_L) return g_settings.coop_tint >= 0 && g_settings.coop_tint < COOP_TINT_COUNT ? g_settings.coop_tint : 0;
    return 0;
}

extern "C" void coop_menu_set(int id, int v) {
    if (split_layout(id) && v >= 0 && v < 2 && v != g_settings.split_layout) g_settings.split_layout = v, settings_save();
    if (split_pad(id) && v >= 0 && v < 4 && v != g_settings.split_pad) g_settings.split_pad = v, settings_save();
    if (split_audio_row(id) && v >= 0 && v < SPLIT_AUDIO_COUNT) {
        g_settings.split_audio = v;
        // (Own device with nothing chosen yet: the first device that is not the default one, as headphones would be)
        if (v == SPLIT_AUDIO_DEVICE && !g_settings.split_audio_device[0])
            for (int i = 0; i < g_ndevices && !g_settings.split_audio_device[0]; i++)
                if (strcmp(g_devices[i], g_default_device) || g_ndevices == 1)
                    snprintf(g_settings.split_audio_device, sizeof g_settings.split_audio_device, "%s", g_devices[i]);
    }
    if (split_device_row(id) && v >= 0 && v < g_ndevices)
        snprintf(g_settings.split_audio_device, sizeof g_settings.split_audio_device, "%s", g_devices[v]);
    if (id == R_DISH_PICK && v >= 0 && v < g_ndishes) g_dish = v;
    if (id == R_SAVE && v >= 0 && v < (int)g_saves.size()) {
        g_selected_save = v;
        g_save_error.clear();
    }
    if (id == R_MODE && v >= 0 && v <= 2) g_settings.coop_mode = v, g_error[0] = 0;
    if (id == R_NAMES || id == R_NAMES_L) g_settings.coop_names = v ? 1 : 0;
    if (id == R_UPNP) g_settings.coop_upnp = v ? 1 : 0;
    if ((id == R_MARKS || id == R_MARKS_L) && v >= 0 && v <= 2) g_settings.coop_sharemarks = v;
    if ((id == R_TINT || id == R_TINT_L) && v >= 0 && v < COOP_TINT_COUNT) g_settings.coop_tint = v, coop_net_set_tint(v);
}

extern "C" bool coop_menu_off(int id, int) {
    if (split_action(id))
        return coop_split_pending() || (coop_net_on() && !coop_net_local_join_port());
    if (split_layout(id) || split_pad(id)) return coop_split_pending() || coop_split_pid();
    if (split_audio_row(id)) return coop_split_pending();
    if (split_device_row(id)) return coop_split_pending() || g_settings.split_audio != SPLIT_AUDIO_DEVICE || !g_ndevices;
    if (id == R_DISH_GIVE) return !g_ndishes || !coop_dish_can_give();
    if (id == R_USE_SAVE)
        return g_saves.empty() || !strcmp(g_saves[g_selected_save].name, g_active_save);
    if (id == R_HOST) return g_settings.coop_mode != COOP_MODE_JOIN;
    if (id == R_ROUTER) {  // (Copy address: only once the router told it)
        CoopUpnpStatus st;
        coop_upnp_status(&st);
        return st.state != COOP_UPNP_OPEN || !st.external[0];
    }
    if (id == R_START) return g_settings.coop_mode == COOP_MODE_SOLO || coop_net_on() || coop_split_pending();
    if (id == R_LEAVE) return coop_split_pending();
    return false;
}

extern "C" const char *coop_menu_label(int id, int v) {
    std::string &o = buf();
    if (split_layout(id)) return v == COOP_SPLIT_LEFT ? tr("Left / right") : tr("Top / bottom");
    if (split_pad(id)) { o = std::to_string(v + 1); return o.c_str(); }
    if (split_audio_row(id)) return tr(k_split_audio[v >= 0 && v < SPLIT_AUDIO_COUNT ? v : 0]);
    if (split_device_row(id)) {
        if (v >= 0 && v < g_ndevices) return device_short(g_devices[v], o);
        if (g_settings.split_audio_device[0]) return device_short(g_settings.split_audio_device, o);  // (saved, not connected now)
        return tr("(no devices found)");
    }
    if (split_action(id)) return coop_split_pid() ? tr("Remove player 2") : tr("Add player 2");
    switch (id) {
    case R_DISH_PICK: return v >= 0 && v < g_ndishes ? items_dish_name(g_dishes[v].name) : "";
    case R_DISH_GIVE: return tr("Give");
    case R_SAVE: return v >= 0 && v < (int)g_saves.size() ? save_label(g_saves[v].name) : "";
    case R_USE_SAVE: return tr("Use");
    case R_NEW_SAVE: return tr("Create");
    case R_MODE: return tr(k_modes[v >= 0 && v < 3 ? v : 0]);
    case R_MARKS: case R_MARKS_L: return tr(k_marks[v >= 0 && v <= 2 ? v : 1]);
    case R_TINT: case R_TINT_L: return tr(k_tints[v >= 0 && v < COOP_TINT_COUNT ? v : 0]);
    case R_NAME: return g_settings.coop_name[0] ? g_settings.coop_name : tr("(not set)");
    case R_HOST: return g_settings.coop_host[0] ? g_settings.coop_host : tr("(not set)");
    case R_PORT: o = std::to_string(port()); return o.c_str();
    case R_CODE: return g_settings.coop_code[0] ? g_settings.coop_code : tr("(none)");
    case R_START: return tr(g_settings.coop_mode == COOP_MODE_JOIN ? "Join" : g_settings.coop_mode == COOP_MODE_HOST ? "Host" : "Start");
    case R_STATUS: {
        if (!coop_net_on()) return tr("Off");
        int others = g_nlobby > 0 ? g_nlobby - 1 : 0;
        char t[96];
        if (coop_net_is_host()) snprintf(t, sizeof t, tr("Hosting, %d joined"), others);
        else snprintf(t, sizeof t, "%s", coop_net_my_id() >= 0 ? tr("Joined") : tr("Connecting..."));
        o = t;  // (0.2.0: the room code moved to the row's description line: with it the text ran out of the box)
        return o.c_str();
    }
    case R_ROUTER: {
        CoopUpnpStatus st;
        coop_upnp_status(&st);
        if (st.external[0] && std::chrono::steady_clock::now() < g_copied_until) return tr("Address copied");
        return tr(coop_upnp_status_short(&st));
    }
    case R_LAN: {
        char a[48];
        if (!coop_lan_address(a, sizeof a)) return "-";
        if (std::chrono::steady_clock::now() < g_lan_copied_until) return tr("Address copied");
        o = a;
        return o.c_str();
    }
    case R_REFRESH: return tr("Refresh");
    case R_LEAVE: return tr(stop_only() ? "Stop" : leave_armed() ? "Press A again" : "Leave");
    }
    if (id >= R_PLAYER0 && id <= R_PLAYER_LAST) {
        int k = id - R_PLAYER0;
        if (k >= g_nlobby) return "-";
        const CoopLobbyEntry &e = g_lobby[k];
        char t[96];
        if (e.me) snprintf(t, sizeof t, "%s", tr("You"));
        else if (e.age_s < 0 || e.age_s > 5) snprintf(t, sizeof t, "%s", tr("Waiting"));
        else if (e.ping_ms >= 0) snprintf(t, sizeof t, tr("Connected, %d ms"), e.ping_ms);
        else snprintf(t, sizeof t, "%s", tr("Connected"));
        o = t;
        return o.c_str();
    }
    return "";
}

extern "C" const char *coop_menu_title(int id) {
    if (id == R_LEAVE && stop_only()) return tr(coop_net_is_host() ? "Stop hosting" : "Stop joining");
    if (id == R_SAVE) {
        const char *title = tr("Save profile");
        return strcmp(title, "Save profile") ? title : "Save";
    }
    if (id < R_PLAYER0 || id > R_PLAYER_LAST) return nullptr;
    int k = id - R_PLAYER0;
    std::string &o = buf();
    if (k >= g_nlobby) return "-";
    const CoopLobbyEntry &e = g_lobby[k];
    char t[96];
    if (e.name[0]) snprintf(t, sizeof t, "%s", e.name);
    else if (e.host) snprintf(t, sizeof t, "%s", tr("The host"));
    else snprintf(t, sizeof t, tr("Player %d"), e.pid >= 0 ? e.pid + 1 : 0);
    o = t;
    return o.c_str();
}

extern "C" const char *coop_menu_desc(int id) {
    if (split_action(id) && coop_split_failed()) return tr("Could not start split screen. See the game log.");
    if (id == R_LEAVE && stop_only())
        return tr("Nobody has connected yet: back to the setup page to change the address or the mode. No restart.");
    std::string &o = buf();
    char t[256];
    if (split_audio_row(id)) {
        o = tr(g_settings.split_audio == SPLIT_AUDIO_DEVICE ? "Player 2's full sound, music too, on another output such as headphones."
               : g_settings.split_audio == SPLIT_AUDIO_SHARED ? "Player 2's game on the same speakers: quieter, no music, silent while you are close together."
               : "Only player 1's game makes sound.");
        if (coop_split_pid()) o = o + " " + tr("Applies the next time player 2 is added.");
        return o.c_str();
    }
    if (split_device_row(id)) {
        if (g_settings.split_audio != SPLIT_AUDIO_DEVICE) return tr("Pick Own device under Player 2 sound first.");
        if (!g_ndevices) return tr("No sound output devices were found.");
        int k = device_index();
        if (k < 0) o = tr("This device is not connected now: player 2 uses the default device.");
        else if (g_ndevices == 1) o = tr("This is the only device: the same as player 1's.");
        else if (!strcmp(g_devices[k], g_default_device)) o = tr("The same device as player 1's (the default device).");
        else o = tr("The output device player 2's sound plays on.");
        if (coop_split_pid()) o = o + " " + tr("Applies the next time player 2 is added.");
        return o.c_str();
    }
    if (id == R_DISH_PICK && g_ndishes) {
        const CoopDish &dish = g_dishes[g_dish];
        snprintf(t, sizeof t, tr("%g hearts · %d:%02d · Effect strength %g"),
                 dish.hearts / 4.0, dish.seconds / 60, dish.seconds % 60, (double)dish.level);
        o = t;
        return o.c_str();
    }
    if (id == R_DISH_GIVE) {
        const char *notice = coop_menu_dish_notice(true);
        if (notice) return notice;
        if (!coop_dish_can_give()) return tr("Move within 10 m of your co-op partner.");
    }
    if (!g_save_error.empty() && id == g_save_error_row) return g_save_error.c_str();
    if (id == R_SAVE) {
        snprintf(t, sizeof t, tr("Active save: %s"), save_label(g_active_save));
        o = t;
        return o.c_str();
    }
    if (id == R_START) {
        if (g_error[0]) {
            snprintf(t, sizeof t, tr("Could not start: %s"), g_error);
            o = t;
            return o.c_str();
        }
        if (g_settings.coop_mode == COOP_MODE_SOLO) return tr("Pick Host or Join under Mode first.");
        if (coop_net_on()) return tr("Co-op is already running.");
        return nullptr;
    }
    if ((id == R_NAME || id == R_HOST || id == R_PORT || id == R_CODE) && g_error[0]) {
        o = g_error;
        return o.c_str();
    }
    if (id == R_STATUS) {
        if (!coop_net_on()) return "";
        if (coop_net_split_room()) return tr("Local split-screen room.");
        if (!coop_net_is_host()) snprintf(t, sizeof t, "%s", tr("In a host's game. Leave ends it for you."));
        else if (coop_net_env_driven())  // (started by the launcher: its port, not this page's)
            snprintf(t, sizeof t, "%s", tr("Hosting. The others join with your IP address and port."));
        else snprintf(t, sizeof t, tr("Hosting on UDP port %d. Others join with your IP and this port."), port());
        const char *code = coop_net_room_code();
        if (*code && coop_net_is_host()) {  // (the friends need the code too)
            if (coop_net_env_driven()) snprintf(t, sizeof t, tr("Room code %s. Others join with your IP, port and this code."), code);
            else snprintf(t, sizeof t, tr("Room code %s. Others join with your IP, port %d and this code."), code, port());
        }
        o = t;
        return o.c_str();
    }
    if (id == R_ROUTER) {
        CoopUpnpStatus st;
        coop_upnp_status(&st);
        char line[320];
        o = coop_upnp_status_text(&st, tr, line, sizeof line);
        return o.c_str();
    }
    if (id >= R_PLAYER0 && id <= R_PLAYER_LAST) {
        int k = id - R_PLAYER0;
        if (k >= g_nlobby) return "";
        const CoopLobbyEntry &e = g_lobby[k];
        const char *role = e.host ? tr("The host") : e.partner ? tr("World partner") : tr("Guest");
        const char *place = e.in_world ? game_place_name(e.pos[0], e.pos[2]) : nullptr;
        if (place && *place) snprintf(t, sizeof t, "%s. %s", role, place);
        else if (e.in_world) snprintf(t, sizeof t, tr("%s. At %.0f, %.0f."), role, e.pos[0], e.pos[2]);
        else snprintf(t, sizeof t, tr("%s. Not in the world yet."), role);
        o = t;
        return o.c_str();
    }
    return nullptr;
}

extern "C" int coop_menu_press(int id) {
    if (split_action(id)) {
        if (!coop_menu_off(id, 0)) {
            bool add = !coop_split_pid();
            if (coop_split_request(add, g_settings.split_layout == 1, coop_menu_get(R_SPLIT_PAD) + 1))
                fprintf(stderr, "[split] menu requested %s\n", add ? "add" : "remove");
        }
        return COOP_PRESS_REDRAW;
    }
    char t[96];
    switch (id) {
    case R_DISH_GIVE:
        if (g_ndishes) coop_dish_request(&g_dishes[g_dish]);
        return COOP_PRESS_REDRAW;
    case R_USE_SAVE: {
        if (!saves_on() || coop_net_on() || coop_menu_off(id, 0)) return COOP_PRESS_NONE;
        SpCode code = sp_set_active(NULL, g_saves[g_selected_save].name);
        if (code != SP_OK) return save_failure(id, code);
        settings_save();
        rt_request_reboot(1);
        return COOP_PRESS_NONE;
    }
    case R_NEW_SAVE: {
        if (!saves_on() || coop_net_on()) return COOP_PRESS_NONE;
        if (!ask("New save", "", SP_NAME_MAX, t, sizeof t)) return COOP_PRESS_NONE;
        SpCode code = sp_validate_name(t);
        if (code == SP_OK) code = sp_create(NULL, t);
        if (code != SP_OK) return save_failure(id, code);
        snprintf(g_new_save, sizeof g_new_save, "%s", t);
        return COOP_PRESS_REOPEN;
    }
    case R_NAME:
        if (!ask("Your name", g_settings.coop_name, COOP_NAME_LEN - 1, t, sizeof t)) return COOP_PRESS_NONE;
        snprintf(g_settings.coop_name, sizeof g_settings.coop_name, "%s", t);
        settings_save();
        if (coop_net_on()) coop_net_set_name(g_settings.coop_name);
        g_error[0] = 0;
        return COOP_PRESS_REDRAW;
    case R_HOST:
        if (g_settings.coop_mode != COOP_MODE_JOIN) return COOP_PRESS_NONE;
        if (!ask("Host address", g_settings.coop_host, 23, t, sizeof t)) return COOP_PRESS_NONE;
        if (strchr(t, ':')) *strchr(t, ':') = 0;  // (the port has its own row)
        snprintf(g_settings.coop_host, sizeof g_settings.coop_host, "%s", t);
        settings_save();
        g_error[0] = 0;
        return COOP_PRESS_REDRAW;
    case R_PORT: {
        char cur[16];
        snprintf(cur, sizeof cur, "%d", port());
        if (!ask("Port", cur, 5, t, sizeof t)) return COOP_PRESS_NONE;
        char *end = nullptr;
        long p = strtol(t, &end, 10);
        if (!*t || (end && *end) || p < 1 || p > 65535) {
            snprintf(g_error, sizeof g_error, tr("\"%s\" is not a port (a number from 1 to 65535)."), t);
            return COOP_PRESS_REDRAW;
        }
        g_settings.coop_port = (int)p;
        settings_save();
        g_error[0] = 0;
        return COOP_PRESS_REDRAW;
    }
    case R_CODE:
        if (!ask("Room code", g_settings.coop_code, 23, t, sizeof t)) return COOP_PRESS_NONE;
        snprintf(g_settings.coop_code, sizeof g_settings.coop_code, "%s", t);
        settings_save();
        g_error[0] = 0;
        return COOP_PRESS_REDRAW;
    case R_START: {
        if (coop_menu_off(id, 0)) return COOP_PRESS_NONE;
        CoopNetConfig c = {};
        c.mode = g_settings.coop_mode;
        if (c.mode == COOP_MODE_JOIN) {
            if (!g_settings.coop_host[0]) {
                snprintf(g_error, sizeof g_error, "%s", tr("type the host's address first."));
                return COOP_PRESS_REDRAW;
            }
            c.port = 0;  // (any free port here; the host's is in the address)
            snprintf(c.peer, sizeof c.peer, "%s:%d", g_settings.coop_host, port());
        } else {
            c.port = port();
        }
        snprintf(c.code, sizeof c.code, "%s", g_settings.coop_code);
        snprintf(c.name, sizeof c.name, "%s", g_settings.coop_name[0] ? g_settings.coop_name : (c.mode == COOP_MODE_HOST ? "Host" : "Player"));
        settings_save();
        if (!coop_net_start(&c)) {
            snprintf(g_error, sizeof g_error, "%s", coop_net_error());
            fprintf(stderr, "[coop] menu: start failed: %s\n", g_error);
            return COOP_PRESS_REDRAW;
        }
        g_error[0] = 0;
        fprintf(stderr, "[coop] menu: started as %s\n", c.mode == COOP_MODE_HOST ? "host" : "guest");
        return COOP_PRESS_LOBBY;
    }
    case R_ROUTER: {  // (coop-upnp) Copy address
        CoopUpnpStatus st;
        coop_upnp_status(&st);
        if (st.state != COOP_UPNP_OPEN || !st.external[0]) return COOP_PRESS_NONE;
        if (SDL_SetClipboardText(st.external)) {
            g_copied_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            fprintf(stderr, "[coop] upnp: the internet address was copied to the clipboard\n");
        }
        return COOP_PRESS_REDRAW;
    }
    case R_LAN: {  // (0.1.1) Copy the Same Wi-Fi address
        char a[48];
        if (!coop_lan_address(a, sizeof a)) return COOP_PRESS_NONE;
        if (SDL_SetClipboardText(a)) {
            g_lan_copied_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            fprintf(stderr, "[coop] menu: the same Wi-Fi address was copied to the clipboard\n");
        }
        return COOP_PRESS_REDRAW;
    }
    case R_REFRESH:
        return COOP_PRESS_REOPEN;
    case R_LEAVE:
        if (coop_menu_off(id, 0)) return COOP_PRESS_NONE;
        if (stop_only()) {  // (0.1.1) nobody connected yet: back to the setup page, no restart
            g_leave_armed = {};
            fprintf(stderr, "[coop] menu: stopped before anyone connected: back to the setup page\n");
            coop_net_stop();
            settings_save();
            return COOP_PRESS_SETUP;
        }
        if (!leave_armed()) {
            g_leave_armed = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            return COOP_PRESS_REDRAW;
        }
        g_leave_armed = {};
        fprintf(stderr, "[coop] menu: leaving co-op, the game starts again\n");
        coop_net_stop();
        settings_save();
        rt_request_reboot(1);
        return COOP_PRESS_NONE;
    }
    return COOP_PRESS_NONE;
}

extern "C" bool coop_menu_tick(void) {
    auto now = std::chrono::steady_clock::now();
    if (now < g_next_redraw) return false;
    g_next_redraw = now + std::chrono::seconds(1);
    lobby_update();
    return true;
}

extern "C" const char *coop_menu_dish_notice(bool recent_only) {
    switch (coop_dish_status(recent_only)) {
    case DISH_QUEUED: return tr("Close the pause menu to hand the dish over.");
    case DISH_SENDING: return tr("Waiting for your partner to receive the dish.");
    case DISH_SENT: return tr("Dish given to your partner.");
    case DISH_RECEIVED: return tr("Your partner gave you a dish.");
    case DISH_RETURNING: return tr("Waiting for room to return your dish.");
    case DISH_RETURNED: return tr("Your partner could not receive the dish. It was returned.");
    case DISH_TOO_FAR: return tr("Move within 10 m of your co-op partner.");
    case DISH_CHANGED: return tr("That dish is no longer in your pouch.");
    default: return nullptr;
    }
}
