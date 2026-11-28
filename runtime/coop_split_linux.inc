// Part of runtime/coop_split.cpp (included there on Linux): split screen under X11 (also XWayland on a Wayland
// desktop or the Steam Deck: coop-dev/play_coop.sh starts the game with SDL_VIDEO_DRIVER=x11; a pure Wayland window
// cannot hold another program's window).
//
// The same design as the Windows code above: each player is a process of this program with its own SDL window,
// Vulkan surface, controller and data folder (split-playerN). Player 1 makes a plain X11 window (the container),
// moves its own SDL window into it (XReparentWindow) and starts the others; each of them makes an X11 child window
// of the container (SAVAGEWIND_COOP_SPLIT_PARENT) and gives it to SDL (SDL_PROP_WINDOW_CREATE_X11_WINDOW_NUMBER), as
// the Windows guests make a Win32 child. Player 1 places every pane (halves for 2 players, quarters for 3-4; a
// child's pane is its _SAVAGEWIND_SPLIT_SLOT property). Guests ask player 1 for Ctrl+Tab, fullscreen and close with
// ClientMessages to the container. A guest is killed (SIGKILL) on Remove a player and dies with player 1
// (PR_SET_PDEATHSIG). libX11 is loaded at run time: without it (or with SDL on Wayland) split screen is unavailable.
// SAVAGEWIND_COOP_LINUXSPLIT=0: unavailable too.
#include <dlfcn.h>
#include <errno.h>
#include <strings.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "coop_net.h"
#include "paths.h"
#include "services/save_profiles.h"
#include "settings.h"
#include "split_audio.h"
#include "window_size.h"
bool input_user_blocked(const SDL_Event *e);
}

// (last: Xlib's macros such as None, Bool and Status must not reach the headers above)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

extern char **environ;

namespace {
namespace fs = std::filesystem;

// ---- libX11, loaded at run time (SDL loads it the same way)
#define SW_X11_FUNCS(F) \
    F(XOpenDisplay) F(XCreateSimpleWindow) F(XCreateWindow) F(XDestroyWindow) F(XInternAtom) F(XChangeProperty) \
    F(XGetWindowProperty) F(XFree) F(XSelectInput) F(XMapWindow) F(XMapRaised) F(XUnmapWindow) F(XWithdrawWindow) \
    F(XReparentWindow) F(XMoveResizeWindow) F(XGetWindowAttributes) F(XQueryTree) F(XSetInputFocus) \
    F(XGetInputFocus) F(XSendEvent) F(XPending) F(XNextEvent) F(XSync) F(XFlush) F(XSetErrorHandler) \
    F(XSetWMProtocols) F(XStoreName) F(XSetClassHint) F(XSetWMNormalHints) F(XSetWMHints) F(XTranslateCoordinates)
struct XLib {
#define SW_X11_PTR(name) decltype(&::name) name;
    SW_X11_FUNCS(SW_X11_PTR)
#undef SW_X11_PTR
} X;

Display *g_dpy;  // this process's own connection (main thread only); SDL has its own
Window g_root;
Atom A_PANES, A_SLOT, A_CMD, A_PROTOCOLS, A_DELETE, A_PID, A_STATE, A_FULLSCREEN, A_ICON, A_NAME, A_UTF8, A_WMCHECK;

bool x_open() {
    static int state;  // 0: not tried, 1: open, -1: unavailable
    if (state) return state > 0;
    state = -1;
    void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "[split] libX11 not found: no split screen\n");
        return false;
    }
#define SW_X11_LOAD(name) \
    if (!(X.name = (decltype(X.name))dlsym(lib, #name))) { fprintf(stderr, "[split] libX11 lacks " #name "\n"); return false; }
    SW_X11_FUNCS(SW_X11_LOAD)
#undef SW_X11_LOAD
    g_dpy = X.XOpenDisplay(nullptr);
    if (!g_dpy) {
        fprintf(stderr, "[split] could not open the X display: no split screen\n");
        return false;
    }
    g_root = DefaultRootWindow(g_dpy);
    auto atom = [](const char *n) { return X.XInternAtom(g_dpy, n, False); };
    A_PANES = atom("_SAVAGEWIND_SPLIT_PANES"), A_SLOT = atom("_SAVAGEWIND_SPLIT_SLOT");
    A_CMD = atom("_SAVAGEWIND_SPLIT_CMD"), A_PROTOCOLS = atom("WM_PROTOCOLS"), A_DELETE = atom("WM_DELETE_WINDOW");
    A_PID = atom("_NET_WM_PID"), A_STATE = atom("_NET_WM_STATE"), A_FULLSCREEN = atom("_NET_WM_STATE_FULLSCREEN");
    A_ICON = atom("_NET_WM_ICON"), A_NAME = atom("_NET_WM_NAME"), A_UTF8 = atom("UTF8_STRING");
    A_WMCHECK = atom("_NET_SUPPORTING_WM_CHECK");
    state = 1;
    return true;
}

int x_ignore(Display *, XErrorEvent *) { return 0; }
// Our requests on windows that can vanish at any time (another player's pane): their errors are ignored, never the
// default handler's exit. Synced at the end so every error of the block arrives inside it.
struct XGuard {
    XErrorHandler old;
    XGuard() : old(X.XSetErrorHandler(x_ignore)) {}
    ~XGuard() {
        X.XSync(g_dpy, False);
        X.XSetErrorHandler(old);
    }
};

bool x_cardinal(Window w, Atom prop, long *out) {
    Atom type;
    int format;
    unsigned long n = 0, rest;
    unsigned char *data = nullptr;
    bool ok = X.XGetWindowProperty(g_dpy, w, prop, 0, 1, False, XA_CARDINAL, &type, &format, &n, &rest, &data) == Success &&
              type == XA_CARDINAL && format == 32 && n == 1 && data;
    if (ok) *out = *(long *)data;
    if (data) X.XFree(data);
    return ok;
}

void x_set_cardinal(Window w, Atom prop, long v) {
    X.XChangeProperty(g_dpy, w, prop, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
}

Window x_parent(Window w) {
    Window root = 0, parent = 0, *kids = nullptr;
    unsigned n = 0;
    if (!X.XQueryTree(g_dpy, w, &root, &parent, &kids, &n)) return 0;
    if (kids) X.XFree(kids);
    return parent;
}

std::vector<Window> x_children(Window w) {
    Window root = 0, parent = 0, *kids = nullptr;
    unsigned n = 0;
    std::vector<Window> out;
    if (X.XQueryTree(g_dpy, w, &root, &parent, &kids, &n) && kids) out.assign(kids, kids + n);
    if (kids) X.XFree(kids);
    return out;
}

constexpr int k_slots = 4;
constexpr int k_remove = -1, k_busy = -2;
constexpr long k_banner_flag = 0x100;  // in a pane's _SAVAGEWIND_SPLIT_SLOT: the split banner (Ctrl+Tab skips it)
enum { CMD_FOCUS_NEXT = 1, CMD_FULLSCREEN, CMD_CLOSE };

struct Guest {
    pid_t pid;
    bool reaped;
    int status;
    int data_owner = -1;  // the flock'd split-owner.lock of its data folder
    uint64_t stop_deadline;
    int pad;
};
Guest g_guests[k_slots];  // ([0]: player 1 itself, unused)
std::atomic<int> g_request, g_pid, g_pad{-2}, g_count;
std::atomic<int> g_panes;  // player 1: bit n while player n+1's game runs (bit 0 too then); the container's
                           // _SAVAGEWIND_SPLIT_PANES
std::atomic<bool> g_failed, g_minimized, g_has_focus, g_foreground;
volatile sig_atomic_t g_stop;  // a guest: SIGTERM from player 1 (Remove)
int g_slot = 1;                // a guest: its pane
Guest g_banner;
int g_banner_slot;
bool g_banner_broken;
bool banner_mode() { return getenv("SAVAGEWIND_COOP_SPLIT_BANNER") != nullptr; }

Window g_container, g_parent, g_child, g_host, g_focus;  // g_focus: player 1, the pane that last had the keyboard
SDL_Window *g_pane;
int g_layout;
const char *g_fallback;
bool g_fullscreen, g_container_mapped;
int g_fs_saved[4];  // (no window manager) the container's place before fullscreen
int g_host_saved[4];  // (two-window fallback) player 1's window before it took its half
std::string g_game;

uint64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

bool env_off(const char *key) {
    const char *v = getenv(key);
    return v && !strcmp(v, "0");
}

bool is_guest() { return getenv("SAVAGEWIND_COOP_SPLIT_CHILD") != nullptr; }

// libX11 present, the X display open, and SDL on its x11 driver (on Wayland it cannot embed windows)
bool x11_split() {
    if (env_off("SAVAGEWIND_COOP_LINUXSPLIT")) return false;
    const char *driver = SDL_GetCurrentVideoDriver();
    return driver && !strcmp(driver, "x11") && x_open();
}

int max_guests() {
    return env_off("SAVAGEWIND_COOP_SPLIT4") || env_off("SAVAGEWIND_COOP_SPLITEMBED") ? 1 : k_slots - 1;
}

int free_slot() {
    for (int i = 1; i < k_slots; i++)
        if (!(g_panes & (1 << i))) return i;
    return k_slots;
}

void push_quit() {
    SDL_Event e = {};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

Window xwin(SDL_Window *window) {
    return (Window)SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}

// A pane's rectangle in a w x h container: the same layouts as Windows' place_pane
void pane_rect(int panes, int slot, int w, int h, int r[4]) {
    panes |= 1 | 1 << slot;
    int players = 0;
    for (int i = 0; i < k_slots; i++) players += panes >> i & 1;
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (players > 2) {
        if (slot & 1) x0 = w / 2; else x1 = w / 2;
        if (slot & 2) y0 = h / 2; else y1 = h / 2;
    } else if (g_layout == COOP_SPLIT_LEFT) {
        if (slot) x0 = w / 2; else x1 = w / 2;
    } else {
        if (slot) y0 = h / 2; else y1 = h / 2;
    }
    r[0] = x0, r[1] = y0, r[2] = std::max(1, x1 - x0), r[3] = std::max(1, y1 - y0);
}

// Player 1: every pane to its place (after a resize, or a player came or went)
void layout_panes() {
    if (!g_container) return;
    XGuard guard;
    XWindowAttributes a;
    if (!X.XGetWindowAttributes(g_dpy, g_container, &a) || a.width < 2 || a.height < 2) return;
    int panes = g_panes.load() | (g_banner_slot ? 1 << g_banner_slot : 0), r[4];
    for (Window w : x_children(g_container)) {
        long slot = 0;
        if (w != g_host && !x_cardinal(w, A_SLOT, &slot)) continue;
        slot &= 0xff;
        if (slot < 0 || slot >= k_slots) continue;
        if (w != g_host && !(panes >> slot & 1)) {  // a game that is leaving (or a stale window): out of sight
            X.XMoveResizeWindow(g_dpy, w, a.width, a.height, 1, 1);
            continue;
        }
        pane_rect(panes, (int)slot, a.width, a.height, r);
        X.XMoveResizeWindow(g_dpy, w, r[0], r[1], (unsigned)r[2], (unsigned)r[3]);
    }
}

void publish_panes() {
    if (!g_container) return;
    {
        XGuard guard;
        x_set_cardinal(g_container, A_PANES, g_panes.load() | (g_banner_slot ? 1 << g_banner_slot : 0));
    }
    layout_panes();
}

bool wm_running() {
    long check = 0;
    Atom type;
    int format;
    unsigned long n = 0, rest;
    unsigned char *data = nullptr;
    if (X.XGetWindowProperty(g_dpy, g_root, A_WMCHECK, 0, 1, False, XA_WINDOW, &type, &format, &n, &rest, &data) ==
            Success && data && n == 1)
        check = *(long *)data;
    if (data) X.XFree(data);
    return check != 0;
}

void apply_fullscreen() {
    bool fullscreen = g_settings.fullscreen && !getenv("SAVAGEWIND_WINDOW");
    if (g_fullscreen == fullscreen || !g_container) return;
    g_fullscreen = fullscreen;
    XGuard guard;
    if (wm_running()) {  // (EWMH: the window manager makes it cover the monitor)
        XEvent e = {};
        e.xclient.type = ClientMessage;
        e.xclient.window = g_container;
        e.xclient.message_type = A_STATE;
        e.xclient.format = 32;
        e.xclient.data.l[0] = fullscreen ? 1 : 0;  // _NET_WM_STATE_ADD / _REMOVE
        e.xclient.data.l[1] = (long)A_FULLSCREEN;
        e.xclient.data.l[3] = 1;
        X.XSendEvent(g_dpy, g_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
        return;
    }
    if (fullscreen) {  // (no window manager: Xvfb, a bare X server) the whole screen by hand
        XWindowAttributes a;
        if (X.XGetWindowAttributes(g_dpy, g_container, &a)) g_fs_saved[0] = a.x, g_fs_saved[1] = a.y, g_fs_saved[2] = a.width, g_fs_saved[3] = a.height;
        X.XMoveResizeWindow(g_dpy, g_container, 0, 0, (unsigned)DisplayWidth(g_dpy, DefaultScreen(g_dpy)),
                            (unsigned)DisplayHeight(g_dpy, DefaultScreen(g_dpy)));
    } else if (g_fs_saved[2]) {
        X.XMoveResizeWindow(g_dpy, g_container, g_fs_saved[0], g_fs_saved[1], (unsigned)g_fs_saved[2], (unsigned)g_fs_saved[3]);
    }
}

void copy_icon(Window from, Window to) {
    Atom type;
    int format;
    unsigned long n = 0, rest;
    unsigned char *data = nullptr;
    if (X.XGetWindowProperty(g_dpy, from, A_ICON, 0, 1 << 22, False, XA_CARDINAL, &type, &format, &n, &rest, &data) ==
            Success && data && n && format == 32)
        X.XChangeProperty(g_dpy, to, A_ICON, XA_CARDINAL, 32, PropModeReplace, data, (int)n);
    if (data) X.XFree(data);
}

// Player 1: the container, and its own SDL window moved into it
bool make_container(SDL_Window *window) {
    Window game = xwin(window);
    if (!game) return false;
    SDL_SetWindowFullscreen(window, false);
    SDL_SyncWindow(window);
    SDL_Rect work = {0, 0, DisplayWidth(g_dpy, DefaultScreen(g_dpy)), DisplayHeight(g_dpy, DefaultScreen(g_dpy))};
    SDL_DisplayID d = SDL_GetDisplayForWindow(window);
    if (d) SDL_GetDisplayUsableBounds(d, &work);
    int w = work.w * 4 / 5, h = work.h * 4 / 5, x = work.x + (work.w - w) / 2, y = work.y + (work.h - h) / 2;
    XGuard guard;
    unsigned long black = BlackPixel(g_dpy, DefaultScreen(g_dpy));
    g_container = X.XCreateSimpleWindow(g_dpy, g_root, x, y, (unsigned)w, (unsigned)h, 0, black, black);
    if (!g_container) return false;
    X.XStoreName(g_dpy, g_container, "Savage Wind");
    X.XChangeProperty(g_dpy, g_container, A_NAME, A_UTF8, 8, PropModeReplace, (const unsigned char *)"Savage Wind", 11);
    XClassHint cls = {(char *)"savagewind", (char *)"Savage Wind"};
    X.XSetClassHint(g_dpy, g_container, &cls);
    XSizeHints size = {};
    size.flags = PPosition | PSize;
    size.x = x, size.y = y, size.width = w, size.height = h;
    X.XSetWMNormalHints(g_dpy, g_container, &size);
    XWMHints hints = {};
    hints.flags = InputHint | StateHint;
    hints.input = True;
    hints.initial_state = NormalState;
    X.XSetWMHints(g_dpy, g_container, &hints);
    X.XSetWMProtocols(g_dpy, g_container, &A_DELETE, 1);
    x_set_cardinal(g_container, A_PID, (long)getpid());  // (guests check the container is their player 1's)
    copy_icon(game, g_container);
    X.XSelectInput(g_dpy, g_container, StructureNotifyMask | SubstructureNotifyMask | FocusChangeMask);
    // out of the window manager's hands first (ICCCM withdraw: it gives the window back to the root)
    X.XWithdrawWindow(g_dpy, game, DefaultScreen(g_dpy));
    X.XSync(g_dpy, False);
    for (int i = 0; i < 100; i++) {
        XWindowAttributes a;
        if (x_parent(game) == g_root && X.XGetWindowAttributes(g_dpy, game, &a) && a.map_state == IsUnmapped) break;
        SDL_Delay(5);
    }
    X.XReparentWindow(g_dpy, game, g_container, 0, 0);
    X.XSync(g_dpy, False);
    if (x_parent(game) != g_container) {
        X.XDestroyWindow(g_dpy, g_container);
        g_container = 0;
        X.XMapWindow(g_dpy, game);
        winsize_apply(window);
        return false;
    }
    g_host = g_focus = game;
    g_pane = window;
    g_container_mapped = false;
    x_set_cardinal(g_container, A_PANES, g_panes.load());
    X.XMapWindow(g_dpy, game);
    X.XMapRaised(g_dpy, g_container);
    return true;
}

void restore_window(SDL_Window *window) {
    if (g_container) {
        int size[2] = {0, 0};
        {
            XGuard guard;
            int x = 0, y = 0;
            Window child;
            X.XTranslateCoordinates(g_dpy, g_container, g_root, 0, 0, &x, &y, &child);
            X.XUnmapWindow(g_dpy, g_host);
            X.XReparentWindow(g_dpy, g_host, g_root, x, y);
            XWindowAttributes a;  // (back to the whole container's size, not the last pane's)
            if (X.XGetWindowAttributes(g_dpy, g_container, &a) && a.width > 1 && a.height > 1)
                size[0] = a.width, size[1] = a.height;
            X.XDestroyWindow(g_dpy, g_container);
            X.XMapWindow(g_dpy, g_host);  // (managed again by the window manager, as a new window)
        }
        g_container = g_host = g_focus = 0;
        g_pane = nullptr;
        g_fullscreen = false;
        g_minimized = false;
        g_container_mapped = false;
        if (size[0]) SDL_SetWindowSize(window, size[0], size[1]);  // (SDL keeps its own idea of the size: tell it)
        SDL_RaiseWindow(window);
    }
    if (g_fallback) {
        g_fallback = nullptr;
        SDL_SetWindowBordered(window, true);
        if (g_host_saved[2]) {
            SDL_SetWindowSize(window, g_host_saved[2], g_host_saved[3]);
            SDL_SetWindowPosition(window, g_host_saved[0], g_host_saved[1]);
        }
    }
    if (g_settings.fullscreen && !getenv("SAVAGEWIND_WINDOW")) winsize_apply(window);
}

// Player 1: the keyboard to the pane after `from` (Ctrl+Tab), after the last player back to player 1's
void focus_next(int from) {
    if (!g_container) return;
    XGuard guard;
    std::map<int, Window> panes = {{0, g_host}};
    for (Window w : x_children(g_container)) {
        long slot = 0;
        if (w != g_host && x_cardinal(w, A_SLOT, &slot) && !(slot & k_banner_flag) && slot > 0 && slot < k_slots)
            panes[(int)slot] = w;
    }
    auto next = panes.upper_bound(from);
    g_focus = next == panes.end() ? g_host : next->second;
    X.XSetInputFocus(g_dpy, g_focus, RevertToParent, CurrentTime);
}

// A guest: a request to player 1's container (Ctrl+Tab, fullscreen, close)
void send_cmd(long cmd) {
    if (!g_parent) return;
    XGuard guard;
    XEvent e = {};
    e.xclient.type = ClientMessage;
    e.xclient.window = g_parent;
    e.xclient.message_type = A_CMD;
    e.xclient.format = 32;
    e.xclient.data.l[0] = cmd;
    e.xclient.data.l[1] = g_slot;
    X.XSendEvent(g_dpy, g_parent, False, NoEventMask, &e);  // (no mask: to the container's maker, player 1)
}

// Player 1: the container's events (resize, minimise, focus, close, the guests' requests)
void container_events() {
    if (!g_container) return;
    bool relayout = false;
    XGuard guard;
    while (X.XPending(g_dpy)) {
        XEvent e;
        X.XNextEvent(g_dpy, &e);
        switch (e.type) {
        case ConfigureNotify:
            if (e.xconfigure.window == g_container) relayout = true;
            break;
        case MapNotify:
            if (e.xmap.window == g_container) {
                g_minimized = false;
                if (!g_container_mapped) X.XSetInputFocus(g_dpy, g_focus, RevertToParent, CurrentTime);
                g_container_mapped = true;
                relayout = true;
            } else if (e.xmap.event == g_container) {
                relayout = true;  // (a player's pane came)
            }
            break;
        case UnmapNotify:
            if (e.xunmap.window == g_container) g_minimized = true;
            break;
        case CreateNotify: case DestroyNotify: case ReparentNotify:
            relayout = true;
            break;
        case FocusIn:  // the window manager gave the container the keyboard: on to the pane that had it
            if (e.xfocus.window == g_container && e.xfocus.detail != NotifyInferior && e.xfocus.detail != NotifyPointer &&
                g_focus)
                X.XSetInputFocus(g_dpy, g_focus, RevertToParent, CurrentTime);
            break;
        case ClientMessage:
            if (e.xclient.message_type == A_PROTOCOLS && (Atom)e.xclient.data.l[0] == A_DELETE) {
                push_quit();  // (the presenter stops first; the guests end with this process)
            } else if (e.xclient.message_type == A_CMD) {
                long cmd = e.xclient.data.l[0], from = e.xclient.data.l[1];
                if (cmd == CMD_FOCUS_NEXT && from > 0 && from < k_slots) focus_next((int)from);
                else if (cmd == CMD_FULLSCREEN) {
                    g_settings.fullscreen = !g_settings.fullscreen;
                    apply_fullscreen();
                    settings_save();
                } else if (cmd == CMD_CLOSE) {
                    push_quit();
                }
            }
            break;
        }
    }
    if (relayout) layout_panes();
}

// Each process: does its pane have the keyboard, is the split window the active one, is it shown
void focus_state() {
    Window own = g_child ? g_child : g_host, top = g_child ? g_parent : g_container;
    if (!own) return;
    XGuard guard;
    Window focus = 0;
    int revert;
    X.XGetInputFocus(g_dpy, &focus, &revert);
    g_has_focus = focus == own;
    static Window last_focus, last_top;
    if (focus != last_focus) {  // (the pane that has it, or the container itself)
        last_focus = focus;
        last_top = focus == top || (focus > PointerRoot && x_parent(focus) == top) ? top : 0;
        if (g_container && last_top && focus != g_container) g_focus = focus;
    }
    g_foreground = last_top != 0;
    if (g_child) {
        XWindowAttributes a;
        g_minimized = X.XGetWindowAttributes(g_dpy, g_child, &a) && a.map_state != IsViewable;
    }
}

// ---- data folders (as the Windows prepare_data: a one-time copy of player 1's save, settings and fonts)
void check_tree(const fs::path &path) {  // (no links inside a player's folder: a copy must not write elsewhere)
    if (fs::is_symlink(path)) throw std::runtime_error("linked data path");
    if (!fs::is_directory(path)) return;
    for (const auto &entry : fs::recursive_directory_iterator(path))
        if (entry.is_symlink()) throw std::runtime_error("linked data path");
}

void copy_once(const fs::path &src, const fs::path &dst, bool directory) {
    if (fs::exists(fs::symlink_status(dst))) return;
    check_tree(src);
    if (!fs::exists(src) && !directory) return;
    fs::path stage = dst;
    stage += ".copy-" + std::to_string(getpid()) + "-" + std::to_string(now_ms());
    if (fs::exists(fs::symlink_status(stage))) throw std::runtime_error("split save staging path exists");
    try {
        if (fs::exists(src)) fs::copy(src, stage, fs::copy_options::recursive);
        else fs::create_directory(stage);
        if (rename(stage.c_str(), dst.c_str())) throw std::runtime_error("could not publish split data copy");
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(stage, ignored);
        throw;
    }
}

void copy_fonts(const fs::path &root, const fs::path &data) {
    for (const auto &entry : fs::directory_iterator(root))
        if (!strcasecmp(entry.path().extension().c_str(), ".ttf"))
            copy_once(entry.path(), data / entry.path().filename(), false);
}

fs::path prepare_data(int slot) {
    fs::path root(rt_data_root());
    fs::path data = root / ("split-player" + std::to_string(slot + 1));
    check_tree(data);
    fs::create_directory(data);
    // Two hosts sharing a data root must not launch guests that write the same save.
    int &owner = g_guests[slot].data_owner;
    owner = open((data / "split-owner.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (owner >= 0 && flock(owner, LOCK_EX | LOCK_NB)) close(owner), owner = -1;
    if (owner < 0)
        throw std::runtime_error("Player " + std::to_string(slot + 1) + "'s data folder is already in use or cannot be opened");
    copy_once(fs::absolute(fs::path(sp_session_save_dir())), data / "save", true);
    copy_once(fs::absolute(fs::path(settings_file())), data / "savagewind.ini", false);
    copy_once(root / "colour-luts", data / "colour-luts", false);
    copy_fonts(root, data);
    return data;
}

fs::path prepare_banner_data() {
    fs::path root(rt_data_root());
    fs::path data = root / "split-banner";
    check_tree(data);
    fs::create_directories(data / "save");
    copy_once(fs::absolute(fs::path(settings_file())), data / "savagewind.ini", false);
    copy_fonts(root, data);
    return data;
}

int split_audio_mode() {
    if (env_off("SAVAGEWIND_COOP_SPLITAUDIO")) return SPLIT_AUDIO_OFF;
    int mode = g_settings.split_audio;
    return mode > SPLIT_AUDIO_OFF && mode < SPLIT_AUDIO_COUNT ? mode : SPLIT_AUDIO_OFF;
}

using Env = std::vector<std::pair<std::string, std::string>>;  // (an empty value: removed)

void env_set(Env &env, const char *key, const std::string &value) {
    for (auto &item : env)
        if (item.first == key) { item.second = value; return; }
    env.emplace_back(key, value);
}

// banner: the split banner in pane slot (no game, no co-op, no sound, no controller)
bool spawn_guest(const fs::path &data, int port, int pad, bool embed, int slot, bool banner = false) {
    const char *control = getenv("SAVAGEWIND_COOP_SPLIT_CONTROL");
    bool scratch = getenv("SAVAGEWIND_CONTROL") && control && *control;
    int audio = slot == 1 ? split_audio_mode() : SPLIT_AUDIO_OFF;
    Env env = {
        {"SAVAGEWIND_NO_AUDIO", audio == SPLIT_AUDIO_OFF ? "1" : ""},
        {"SAVAGEWIND_AUDIO_DEVICE", audio == SPLIT_AUDIO_DEVICE ? g_settings.split_audio_device : ""},
        {"SAVAGEWIND_SPLIT_AUDIO", audio == SPLIT_AUDIO_SHARED ? "shared" : audio == SPLIT_AUDIO_DEVICE ? "device" : ""},
        {"SAVAGEWIND_NO_MUSIC", audio == SPLIT_AUDIO_SHARED ? "1" : ""},
        {"SAVAGEWIND_COOP_SPLIT_CHILD", std::to_string(getpid())},
        {"SAVAGEWIND_COOP_SPLIT_PARENT", embed ? std::to_string((unsigned long)g_container) : ""},
        {"SDL_VIDEO_DRIVER", "x11"},  // (its window goes into player 1's X11 container: also under XWayland)
        {"SAVAGEWIND_DATA_ROOT", data.string()},
        {"SAVAGEWIND_SAVE_DIR", (data / "save").string()},
        {"SAVAGEWIND_DEFAULT_SAVE_DIR", ""},
        {"SAVAGEWIND_SETTINGS", (data / "savagewind.ini").string()},
        {"SAVAGEWIND_NO_LAUNCHER", "1"},
        {"SAVAGEWIND_HIDDEN", ""}, {"SAVAGEWIND_WINDOW", ""},
        {"SAVAGEWIND_SPLIT", g_layout == COOP_SPLIT_LEFT ? "right" : "bottom"},
        {"SAVAGEWIND_PAD", std::to_string(pad)},
        {"SAVAGEWIND_COOP_PORT", ""}, {"SAVAGEWIND_COOP_BIND", ""},
        {"SAVAGEWIND_COOP_PEER", "127.0.0.1:" + std::to_string(port)},
        {"SAVAGEWIND_COOP_CODE", coop_net_room_code()},
        {"SAVAGEWIND_COOP_NAME", "Player " + std::to_string(slot + 1)},
        {"SAVAGEWIND_LAUNCH_FLAGS_APPLIED", ""}, {"SAVAGEWIND_REEXEC", ""},
        {"SAVAGEWIND_CONTROL", scratch ? std::to_string(atoi(control) + slot - 1) : ""},
        {"SAVAGEWIND_COOP_SPLIT_CONTROL", ""},
        {"SAVAGEWIND_COOP_SPLIT_SLOT", std::to_string(slot)},
        {"SAVAGEWIND_COOP_SPLIT_BANNER", banner ? "1" : ""},
    };
    if (banner) {
        audio = SPLIT_AUDIO_OFF;
        for (const char *key : {"SAVAGEWIND_AUDIO_DEVICE", "SAVAGEWIND_SPLIT_AUDIO", "SAVAGEWIND_COOP_PEER",
                                "SAVAGEWIND_COOP_CODE", "SAVAGEWIND_COOP_NAME", "SAVAGEWIND_CONTROL",
                                "SAVAGEWIND_NO_BOOT_SPLASH"})
            env_set(env, key, "");
        env_set(env, "SAVAGEWIND_NO_AUDIO", "1");
        env_set(env, "SAVAGEWIND_PAD", "none");
    }
    if (audio == SPLIT_AUDIO_SHARED) {
        char gain[32];
        const char *base = getenv("SAVAGEWIND_AUDIO_GAIN");
        snprintf(gain, sizeof gain, "%.3f", (base && *base ? atof(base) : 1.0) * SPLIT_SHARED_VOLUME);
        env_set(env, "SAVAGEWIND_AUDIO_GAIN", gain);
    }
    if (!banner) fprintf(stderr, "[split] Player %d sound: %s%s%s%s\n", slot + 1,
            audio == SPLIT_AUDIO_SHARED ? "shared speakers" : audio == SPLIT_AUDIO_DEVICE ? "own device " : "off",
            audio == SPLIT_AUDIO_DEVICE ? "\"" : "", audio == SPLIT_AUDIO_DEVICE ? g_settings.split_audio_device : "",
            audio == SPLIT_AUDIO_DEVICE ? "\"" : "");
    for (const char *key : {"SAVAGEWIND_GAME_LIB", "SAVAGEWIND_ROMFS_OVERLAY"}) {
        const char *v = getenv(key);
        if (v && *v) env_set(env, key, fs::absolute(fs::path(v)).string());
    }
    for (const char *key : {"SAVAGEWIND_HOST_INPUT", "SAVAGEWIND_BIND_TEST", "SAVAGEWIND_REBOOT_AT",
                            "SAVAGEWIND_SHOT_AT", "SAVAGEWIND_LAUNCHER_MODE", "SAVAGEWIND_LAUNCHER_SHOT",
                            "SAVAGEWIND_LAUNCHER_EXTRACT", "SAVAGEWIND_LAUNCHER_SETUP", "SAVAGEWIND_LAUNCHER_DELETE"})
        env_set(env, key, "");
    if (!scratch)
        for (const char *key : {"SAVAGEWIND_INPUT", "SAVAGEWIND_TEST_WARP", "SAVAGEWIND_TEST_ITEMS",
                                "SAVAGEWIND_TEST_FLAGS", "SAVAGEWIND_NO_USER_INPUT"})
            env_set(env, key, "");

    std::vector<std::string> vars;
    for (char **p = environ; *p; p++) {
        const char *eq = strchr(*p, '=');
        if (!eq) continue;
        std::string key(*p, (size_t)(eq - *p));
        bool overridden = false;
        for (const auto &item : env)
            if (item.first == key) { overridden = true; break; }
        if (!overridden) vars.emplace_back(*p);
    }
    for (const auto &item : env)
        if (!item.second.empty()) vars.push_back(item.first + "=" + item.second);
    std::vector<char *> envp;
    for (auto &v : vars) envp.push_back(v.data());
    envp.push_back(nullptr);

    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    std::string game = g_game;
    char play[] = "--play", no_flags[] = "--no-launch-flags";
    char *argv[] = {exe, game.data(), play, no_flags, nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, data.c_str());  // (its log: savagewind.log in its folder)
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);  // (not player 1's sockets, save lock or X connection)
#endif
    pid_t pid = 0;
    int err = posix_spawn(&pid, exe, &actions, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (err) {
        fprintf(stderr, "[split] posix_spawn failed (%s)\n", strerror(err));
        return false;
    }
    Guest &g = banner ? g_banner : g_guests[slot];
    g.pid = pid, g.reaped = false, g.status = 0;
    if (banner) {
        fprintf(stderr, "[split] banner PID %d in pane %d\n", (int)pid, slot + 1);
        return true;
    }
    fprintf(stderr, "[split] Player %d PID %d, %s, controller %d\n", slot + 1, (int)pid,
            embed ? "embedded" : "two windows", pad);
    g_pid = (int)pid;
    return true;
}

bool reap(Guest &g) {  // true once it has ended (its status in g.status)
    if (!g.pid) return true;
    if (g.reaped) return true;
    int st = 0;
    pid_t r = waitpid(g.pid, &st, WNOHANG);
    if (r == g.pid || (r < 0 && errno == ECHILD)) {
        g.reaped = true;
        g.status = r == g.pid ? (WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0)) : 0;
    }
    return g.reaped;
}

void close_guest(Guest &g) {
    if (!g.pid || g.reaped) return;
    kill(g.pid, SIGKILL);
    for (int i = 0; i < 200 && !reap(g); i++) SDL_Delay(5);
}

void drop_banner() {
    if (!g_banner_slot) return;
    close_guest(g_banner);
    g_banner.pid = 0;
    g_banner_slot = 0;
    publish_panes();
}

void update_banner() {
    if (g_banner_slot && reap(g_banner)) {
        fprintf(stderr, "[split] the banner ended by itself; no banner this session\n");
        g_banner_broken = true;
    }
    int want = g_container && g_count == 2 && !g_banner_broken && !env_off("SAVAGEWIND_COOP_SPLITBANNER")
                   ? free_slot() : 0;
    if (want >= k_slots) want = 0;
    if (g_banner_slot && want != g_banner_slot) drop_banner();
    if (!want || g_banner_slot) return;
    g_banner_slot = want;
    publish_panes();
    bool ok = false;
    try {
        ok = spawn_guest(prepare_banner_data(), 0, -1, true, want, true);
    } catch (const std::exception &e) {
        fprintf(stderr, "[split] banner: %s\n", e.what());
    }
    if (!ok) {
        g_banner_broken = true;
        g_banner_slot = 0;
        publish_panes();
    }
}

void release_slot(int slot) {
    Guest &g = g_guests[slot];
    if (g.data_owner >= 0) close(g.data_owner);
    g.data_owner = -1;
    g.pid = 0;
    g.reaped = false;
    g.stop_deadline = 0;
    g.pad = 0;
    g_panes &= ~(1 << slot);
    if (g_panes == 1) g_panes = 0;
}

bool start(SDL_Window *window, int layout, int pad) {
    if (g_game.empty() || !x11_split()) return false;
    int slot = free_slot();
    if (slot > max_guests()) return false;
    bool first = !g_count;
    int host_pad = g_pad;
    if (first) {
        const char *host_choice = getenv("SAVAGEWIND_PAD");
        host_pad = host_choice && !strcmp(host_choice, "none") ? -1 : host_choice ? atoi(host_choice) : 0;
        if (host_pad < -1 || !host_pad || host_pad > 4) host_pad = pad == 1 ? -1 : 1;
        if (host_pad == pad) {
            fprintf(stderr, "[split] controller %d is already assigned to player 1\n", pad);
            return false;
        }
    } else {
        auto used = [&](int p) {
            if (p == host_pad) return true;
            for (int i = 1; i < k_slots; i++)
                if (g_guests[i].pid && g_guests[i].pad == p) return true;
            return false;
        };
        int want = pad;
        for (pad = want; pad <= 4 && used(pad); pad++) {}
        if (pad > 4) for (pad = 1; pad < want && used(pad); pad++) {}
        if (used(pad)) {
            fprintf(stderr, "[split] no free controller for player %d\n", slot + 1);
            return false;
        }
        if (pad != want) fprintf(stderr, "[split] controller %d is in use: player %d gets controller %d\n", want, slot + 1, pad);
    }
    bool started_room = false, launched = false;
    try {
        if (!coop_net_on()) {
            CoopNetConfig config = {};
            config.mode = COOP_MODE_HOST;
            config.loopback = true;
            const char *port = getenv("SAVAGEWIND_COOP_SPLIT_PORT");
            if (port && *port) config.port = atoi(port);
            if (config.port < 0 || config.port > 65535) return false;
            snprintf(config.name, sizeof config.name, "%s", g_settings.coop_name[0] ? g_settings.coop_name : "Player 1");
            if (!coop_net_start(&config)) {
                fprintf(stderr, "[split] could not start the local room: %s\n", coop_net_error());
                return false;
            }
            started_room = true;
        }
        int port = coop_net_local_join_port();
        if (port) {
            fs::path data = prepare_data(slot);
            if (first) g_layout = layout;
            bool embed = first ? !env_off("SAVAGEWIND_COOP_SPLITEMBED") : g_container != 0;
            if (embed && first && !make_container(window)) {
                fprintf(stderr, "[split] could not create the split-screen container\n");
            } else {
                g_panes |= 1 | 1 << slot;
                publish_panes();
                if (first && !embed) {  // (two-window fallback: player 1 keeps its own window, in its half)
                    SDL_SetWindowFullscreen(window, false);
                    SDL_SyncWindow(window);
                    SDL_GetWindowPosition(window, &g_host_saved[0], &g_host_saved[1]);
                    SDL_GetWindowSize(window, &g_host_saved[2], &g_host_saved[3]);
                }
                launched = spawn_guest(data, port, pad, embed, slot);
                if (launched) {
                    g_pad = host_pad;
                    g_guests[slot].pad = pad;
                    g_count++;
                    if (first && embed) winsize_apply(window);
                    else if (first) {
                        g_fallback = layout == COOP_SPLIT_LEFT ? "left" : "top";
                        winsize_apply(window);
                    }
                }
            }
        } else {
            fprintf(stderr, "[split] the current co-op room is not listening on 127.0.0.1\n");
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "[split] %s\n", e.what());
    }
    if (!launched) {
        release_slot(slot);
        if (first) {
            if (g_container) restore_window(window);
            if (started_room) coop_net_stop();
        } else {
            publish_panes();
        }
    }
    return launched;
}

void finish(SDL_Window *window, int slot) {
    close_guest(g_guests[slot]);
    release_slot(slot);
    g_count--;
    g_pid = 0;
    for (int i = 1; i < k_slots; i++)
        if (g_guests[i].pid) g_pid = (int)g_guests[i].pid;
    if (g_count > 0) {
        publish_panes();
        return;
    }
    g_count = 0;
    g_pad = -2;
    drop_banner();
    restore_window(window);
}

void on_stop_signal(int) { g_stop = 1; }

}  // namespace

bool coop_split_available() {
    const char *legacy = getenv("SAVAGEWIND_SPLIT");
    return !is_guest() && !(legacy && *legacy) && x11_split();
}

bool coop_split_request(bool add, int layout, int pad) {
    if (!coop_split_available()) return false;
    if (add && (layout < COOP_SPLIT_TOP || layout > COOP_SPLIT_LEFT || pad < 1 || pad > 4 ||
                g_count >= max_guests() || (coop_net_on() && !coop_net_local_join_port()))) return false;
    if (!add && !g_count) return false;
    int idle = 0;
    if (!g_request.compare_exchange_strong(idle, add ? pad + 4 * layout : k_remove)) return false;
    g_failed = false;
    return true;
}

int coop_split_pid() { return g_pid; }
int coop_split_count() { return g_count; }
int coop_split_max() { return max_guests(); }
int coop_split_next_player() { return free_slot() + 1; }
int coop_split_last_player() {
    int panes = g_panes;
    for (int i = k_slots - 1; i > 0; i--)
        if (panes & (1 << i)) return i + 1;
    return 0;
}
bool coop_split_pending() { return g_request != 0; }
bool coop_split_failed() { return g_failed; }
int coop_split_host_pad() { return g_pad; }

// (split map) the block the games of this window share: POSIX shared memory named after player 1's PID
namespace {
std::string g_hud_name;
void hud_unlink() {
    if (!g_hud_name.empty()) shm_unlink(g_hud_name.c_str());
}
}  // namespace

SplitHud *coop_split_hud() {
    static SplitHud *hud;
    static uint64_t tried;
    if (hud) return hud;
    bool guest = is_guest();
    if (!guest && !g_count) return nullptr;
    uint64_t now = now_ms();
    if (tried && now - tried < 1000) return nullptr;
    tried = now;
    unsigned long host = guest ? strtoul(getenv("SAVAGEWIND_COOP_SPLIT_CHILD"), nullptr, 10) : (unsigned long)getpid();
    std::string name = "/SavageWindSplitHud-" + std::to_string(host);
    int fd = guest ? shm_open(name.c_str(), O_RDWR, 0) : shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return nullptr;
    if (!guest && (ftruncate(fd, 0) || ftruncate(fd, sizeof(SplitHud)))) {  // (fresh pages are zero)
        close(fd);
        return nullptr;
    }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < (off_t)sizeof(SplitHud)) {
        close(fd);
        return nullptr;
    }
    void *p = mmap(nullptr, sizeof(SplitHud), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return nullptr;
    auto *h = (SplitHud *)p;
    if (!guest) {
        h->version = SPLIT_HUD_VERSION;
        h->magic = SPLIT_HUD_MAGIC;
        g_hud_name = name;
        atexit(hud_unlink);
    } else if (h->magic != SPLIT_HUD_MAGIC || h->version != SPLIT_HUD_VERSION) {
        munmap(p, sizeof(SplitHud));
        return nullptr;
    }
    return hud = h;
}
int coop_split_hud_slot() { return banner_mode() ? -1 : is_guest() ? g_slot : 0; }

void coop_split_startup() {
    split_audio_device();
    const char *host = getenv("SAVAGEWIND_COOP_SPLIT_CHILD");
    if (!host) return;
    // (dies with player 1, as the Windows job object does; checked after: player 1 may have gone already)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if ((unsigned long)getppid() != strtoul(host, nullptr, 10)) exit(1);
    if (!freopen("savagewind.log", "a", stderr)) exit(1);
    const char *slot = getenv("SAVAGEWIND_COOP_SPLIT_SLOT");
    g_slot = slot ? atoi(slot) : 1;
    if (g_slot < 1 || g_slot >= k_slots) exit(1);
    if (banner_mode()) return;  // (player 1 ends it with SIGKILL)
    struct sigaction sa = {};
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
}

void coop_split_game_path(const char *path) {
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(path), ec);
    if (ec) {
        g_game.clear();
        return;
    }
    if (!p.has_filename() && p != p.root_path()) p = p.parent_path();
    g_game = p.string();
}

void *coop_split_guest_window() {
    const char *value = getenv("SAVAGEWIND_COOP_SPLIT_PARENT");
    if (!is_guest() || !value || !*value) return nullptr;
    long owner = 0;
    if (env_off("SAVAGEWIND_COOP_LINUXSPLIT") || !x_open()) {
        fprintf(stderr, "[split] FATAL: split-screen container unavailable (no X display)\n");
        exit(1);
    }
    XGuard guard;
    g_parent = (Window)strtoul(value, nullptr, 10);
    if (!x_cardinal(g_parent, A_PID, &owner) || (unsigned long)owner != strtoul(getenv("SAVAGEWIND_COOP_SPLIT_CHILD"), nullptr, 10)) {
        fprintf(stderr, "[split] FATAL: split-screen container unavailable\n");
        exit(1);
    }
    const char *half = getenv("SAVAGEWIND_SPLIT");
    g_layout = half && !strcmp(half, "right") ? COOP_SPLIT_LEFT : COOP_SPLIT_TOP;
    XSetWindowAttributes attr = {};
    attr.background_pixel = BlackPixel(g_dpy, DefaultScreen(g_dpy));
    g_child = X.XCreateWindow(g_dpy, g_parent, 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixel, &attr);
    if (!g_child) {
        fprintf(stderr, "[split] FATAL: could not create child window\n");
        exit(1);
    }
    // its pane, for player 1's layout (before it is shown: the map is what player 1 lays out on)
    x_set_cardinal(g_child, A_SLOT, g_slot | (banner_mode() ? k_banner_flag : 0));
    X.XStoreName(g_dpy, g_child, "Savage Wind");
    X.XMapWindow(g_dpy, g_child);
    return (void *)(uintptr_t)g_child;  // (SDL takes it: SDL_PROP_WINDOW_CREATE_X11_WINDOW_NUMBER)
}

bool coop_split_apply(SDL_Window *window) {
    if (g_child) g_pane = window;
    if (g_container) apply_fullscreen();
    return g_container || g_child;
}

bool coop_split_minimized() { return g_minimized; }

bool coop_split_window_focus(SDL_Window *window) {
    if (!window) return false;
    if (!g_container && !g_child) return (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    return g_has_focus;
}

// (X11: SDL's own relative mouse mode works in a child window; Windows needs the native one)
bool coop_split_mouse(SDL_Window *, bool) { return false; }

const char *coop_split_host_half() { return g_fallback; }

void coop_split_poll(SDL_Window *window) {
    if (is_guest()) {
        if (g_stop) {  // Remove: leave at once, like the Windows job kill (a full exit() races Mesa's
            fflush(nullptr);  // shader threads: lavapipe crashed in LLVM during teardown)
            _exit(0);
        }
        if (g_child) focus_state();
        return;
    }
    if (g_container) {
        container_events();
        focus_state();
    }
    for (int i = 1; i < k_slots; i++) {
        Guest &g = g_guests[i];
        if (!g.pid) continue;
        if (reap(g) || (g.stop_deadline && now_ms() >= g.stop_deadline)) {
            if (!g.stop_deadline && g.reaped && g.status) {
                g_failed = true;
                fprintf(stderr, "[split] player %d exited with code %d\n", i + 1, g.status);
            }
            bool removed = g.stop_deadline != 0;
            finish(window, i);
            if (removed && g_request == k_busy) g_request = 0;
        } else if (!coop_net_on() && !g.stop_deadline) {
            finish(window, i);
        }
    }
    update_banner();
    int request = g_request.load();
    if (!request || request == k_busy) return;
    g_request = k_busy;
    if (request > 0) {
        drop_banner();
        g_failed = !start(window, (request - 1) / 4, (request - 1) % 4 + 1);
        update_banner();
    } else if (int last = coop_split_last_player(); last && g_guests[last - 1].pid) {
        // Remove: the last player added leaves at once, like the Windows job kill. (SIGTERM + exit raced Mesa's shader
        // threads: lavapipe crashed in LLVM, and its crash dialog kept a dead pane on screen.)
        kill(g_guests[last - 1].pid, SIGKILL);
        g_guests[last - 1].stop_deadline = now_ms() + 3000;
        return;
    }
    g_request = 0;
}

bool coop_split_event(SDL_Window *window, const SDL_Event *e) {
    if (!g_container && !g_child) return false;
    // (keyboard focus is the X input focus, read each frame: focus_state)
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED || e->type == SDL_EVENT_WINDOW_FOCUS_LOST) return true;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {  // a click gives this pane the keyboard
        XGuard guard;
        Window own = g_child ? g_child : xwin(window);
        if (own) X.XSetInputFocus(g_dpy, own, RevertToParent, CurrentTime);
        if (g_container) g_focus = own;
        return false;
    }
    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
        (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F4 && (e->key.mod & SDL_KMOD_ALT))) {
        if (g_child) send_cmd(CMD_CLOSE);
        else push_quit();
        return true;
    }
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat || input_user_blocked(e)) return false;
    if (e->key.scancode == SDL_SCANCODE_TAB && (e->key.mod & SDL_KMOD_CTRL)) {
        if (g_child) send_cmd(CMD_FOCUS_NEXT);
        else focus_next(0);
        return true;
    }
    if (g_child && (e->key.scancode == SDL_SCANCODE_F11 ||
                    (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_ALT)))) {
        send_cmd(CMD_FULLSCREEN);
        return true;
    }
    return false;
}

void coop_split_shutdown() {
    // (also from the exit handlers, maybe on the control thread: signals only, no X)
    if (g_banner.pid && !g_banner.reaped) kill(g_banner.pid, SIGKILL);
    for (int i = 1; i < k_slots; i++)
        if (g_guests[i].pid && !g_guests[i].reaped) kill(g_guests[i].pid, SIGKILL);
    hud_unlink();
}

void coop_split_reboot(SDL_Window *window) {
    if (!is_guest()) {
        g_request = 0;
        for (int i = 1; i < k_slots; i++)
            if (g_guests[i].pid) finish(window, i);
    }
}

const char *split_audio_device() {
    const char *v = getenv("SAVAGEWIND_AUDIO_DEVICE");
    return v && *v ? v : nullptr;
}

// player 2 in the split window: the whole window is the active one (read on the main thread each frame)
bool split_audio_foreground() { return g_foreground; }
