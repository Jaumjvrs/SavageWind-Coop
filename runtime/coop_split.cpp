// Each player owns a process, SDL window and Vulkan surface. The Windows container only lays out the two HWNDs.
#include "coop_split.h"
#include "coop_split_hud.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "coop_net.h"
#include "paths.h"
#include "services/save_profiles.h"
#include "settings.h"
#include "split_audio.h"
#include "window_size.h"
#include "ui/menu.h"
bool input_user_blocked(const SDL_Event *e);
void input_handle_event(const SDL_Event *e);
}

namespace {
namespace fs = std::filesystem;

constexpr wchar_t k_class[] = L"SavageWindCoopSplit";
constexpr UINT k_focus_next = WM_APP + 1, k_focus_guest = WM_APP + 2, k_fullscreen = WM_APP + 3;
constexpr int k_remove = -1, k_busy = -2;
// (4-player split screen) panes: 0 is player 1 (the game that made the container), 1..3 the players it started.
// Two players: halves (the chosen layout); three or four: quarters, player n in quarter n (left to right, top to
// bottom). SAVAGEWIND_COOP_SPLIT4=0: one extra player only, as before.
constexpr int k_slots = 4;
struct Guest {
    std::atomic<HANDLE> job;
    HANDLE process, stop_event, data_owner;
    ULONGLONG stop_deadline;
    int pad;
};
Guest g_guests[k_slots];  // ([0]: player 1 itself, unused)
std::atomic<int> g_request, g_pid, g_pad{-2}, g_count;
std::atomic<int> g_panes;  // player 1: bit n while player n+1's game runs (bit 0 too then); also the container's
                           // GWLP_USERDATA, which the guests read to find their pane
std::atomic<bool> g_failed;
std::atomic<bool> g_minimized;
HANDLE g_stop_event;  // a guest: player 1 sets it to end this game
int g_slot = 1;       // a guest: its pane
// (split banner) with three players the empty quarter shows the start screen's picture: a small process of this
// exe (SAVAGEWIND_COOP_SPLIT_BANNER=1) that draws only the boot picture and never loads the game.
// SAVAGEWIND_COOP_SPLITBANNER=0: the quarter stays black.
constexpr wchar_t k_banner_class[] = L"SavageWindCoopBanner";  // (not k_class: Ctrl+Tab and focus skip it)
Guest g_banner;
int g_banner_slot;     // player 1: the pane it fills (0: none)
bool g_banner_broken;  // player 1: it failed once; not again this session
bool banner_mode() { return getenv("SAVAGEWIND_COOP_SPLIT_BANNER") != nullptr; }

HWND g_container, g_parent, g_child, g_host;
WNDPROC g_sdl_host_proc;
SDL_Window *g_pane;
int g_layout;
const char *g_fallback;
bool g_fullscreen;
bool g_mouse_held;
RECT g_mouse_clip;
wchar_t g_high_surrogate;
WINDOWPLACEMENT g_windowed{sizeof(WINDOWPLACEMENT)}, g_host_placement{sizeof(WINDOWPLACEMENT)};
LONG_PTR g_host_style, g_host_exstyle;
fs::path g_game;

bool env_off(const char *key) {
    const char *v = getenv(key);
    return v && !strcmp(v, "0");
}

bool is_guest() { return getenv("SAVAGEWIND_COOP_SPLIT_CHILD") != nullptr; }

int max_guests() {
    // (more than one extra player needs the shared window: the two-window fallback has only two halves)
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

HWND hwnd(SDL_Window *window) {
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

void release_mouse() {
    if (g_mouse_held) ClipCursor(nullptr);
    g_mouse_held = false;
}

// SDL's Win32 focus check expects a foreground HWND. Windows makes our common
// container the foreground window and assigns keyboard focus to its child.
bool pane_message(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) {
    switch (msg) {
    case WM_SETCURSOR:
        // (2026-11-27) A guest's pane is our window class, whose class cursor is the arrow: each re-centring of the
        // held mouse (coop_split_mouse) made Windows draw that arrow before SDL hid it again, a flicker in the middle
        // of players 2-4's panes. While the game hides the pointer, the pane shows none.
        if (LOWORD(lp) == HTCLIENT && (g_mouse_held || !SDL_CursorVisible())) {
            SetCursor(nullptr);
            *result = TRUE;
            return true;
        }
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        SetFocus(w);
        break;
    case WM_KILLFOCUS:
        release_mouse();
        SDL_ResetKeyboard();
        g_high_surrogate = 0;
        break;
    case WM_CHAR:
        if (!g_pane || !SDL_TextInputActive(g_pane) || wp < 0x20 || wp == 0x7f) break;
        if (IS_HIGH_SURROGATE(wp)) {
            g_high_surrogate = (wchar_t)wp;
        } else {
            wchar_t utf16[] = {g_high_surrogate ? g_high_surrogate : (wchar_t)wp, g_high_surrogate ? (wchar_t)wp : L'\0', 0};
            g_high_surrogate = 0;
            char utf8[5];
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16, -1, utf8, sizeof utf8, nullptr, nullptr)) {
                SDL_Event e = {};
                e.type = SDL_EVENT_TEXT_INPUT;
                e.text.windowID = SDL_GetWindowID(g_pane);
                e.text.text = utf8;
                if (!input_user_blocked(&e)) menu_handle_event(&e);
            }
        }
        *result = 0;
        return true;
    }
    return false;
}

LRESULT CALLBACK host_pane_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result;
    if (pane_message(w, msg, wp, lp, &result)) return result;
    return CallWindowProcW(g_sdl_host_proc, w, msg, wp, lp);
}

// Puts a player's window in its pane of the container; panes: the container's GWLP_USERDATA (see g_panes)
void place_pane(HWND parent, HWND child, int slot, UINT flags) {
    RECT r;
    if (!child || IsIconic(parent) || !GetClientRect(parent, &r) || r.right < 2 || r.bottom < 2) return;
    int panes = (int)GetWindowLongPtrW(parent, GWLP_USERDATA) | 1 | 1 << slot, players = 0;
    for (int i = 0; i < k_slots; i++) players += panes >> i & 1;
    bool second = slot != 0;
    if (players > 2) {
        int w = r.right / 2, h = r.bottom / 2;
        if (slot & 1) r.left = w;
        else r.right = w;
        if (slot & 2) r.top = h;
        else r.bottom = h;
    } else if (g_layout == COOP_SPLIT_LEFT) {
        if (second) r.left = r.right / 2;
        else r.right /= 2;
    } else {
        if (second) r.top = r.bottom / 2;
        else r.bottom /= 2;
    }
    SetWindowPos(child, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | flags);
}

void apply_fullscreen() {
    bool fullscreen = g_settings.fullscreen && !getenv("SAVAGEWIND_WINDOW");
    if (g_fullscreen == fullscreen) return;
    g_fullscreen = fullscreen;
    if (fullscreen) {
        GetWindowPlacement(g_container, &g_windowed);
        MONITORINFO mon = {sizeof mon};
        GetMonitorInfoW(MonitorFromWindow(g_container, MONITOR_DEFAULTTONEAREST), &mon);
        if (IsZoomed(g_container)) ShowWindow(g_container, SW_RESTORE);
        SetWindowLongPtrW(g_container, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        const RECT &r = mon.rcMonitor;
        SetWindowPos(g_container, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        SetWindowLongPtrW(g_container, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPlacement(g_container, &g_windowed);
        SetWindowPos(g_container, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK split_window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        if (w == g_child) PostMessageW(g_parent, WM_CLOSE, 0, 0);
        else push_quit();  // keep the native window alive until the presenter has stopped
        return 0;
    }
    if (w == g_child && msg == k_focus_guest) {
        SetFocus(w);
        return 0;
    }
    if (w == g_child && banner_mode()) {  // (the banner never takes the keyboard or the mouse)
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_SETFOCUS) return 0;
    }
    LRESULT result;
    if (w == g_child && !banner_mode() && pane_message(w, msg, wp, lp, &result)) return result;
    if (w == g_container) switch (msg) {
    case WM_SIZE:
        g_minimized = wp == SIZE_MINIMIZED;
        if (wp != SIZE_MINIMIZED) place_pane(w, g_host, 0, 0);
        return 0;
    case WM_DPICHANGED:
        if (!g_fullscreen) {
            const RECT &r = *(const RECT *)lp;
            SetWindowPos(w, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_SETFOCUS:
        if (g_host) SetFocus(g_host);
        return 0;
    case k_focus_next: {  // Ctrl+Tab in the pane wp (0: player 1's): the next player's pane, after the last player 1's
        HWND next = FindWindowExW(w, (HWND)wp, k_class, nullptr);
        if (next) PostMessageW(next, k_focus_guest, 0, 0);
        else if (g_host) SetFocus(g_host);
        return 0;
    }
    case k_fullscreen:
        g_settings.fullscreen = !g_settings.fullscreen;
        apply_fullscreen();
        settings_save();
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

bool register_window_class() {
    WNDCLASSW cls = {};
    cls.lpfnWndProc = split_window_proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(1));  // the exe's Savage Wind icon (taskbar, title bar)
    cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    cls.lpszClassName = k_class;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    cls.lpszClassName = k_banner_class;
    return RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool make_container(SDL_Window *window) {
    if (!register_window_class()) return false;
    HWND game = hwnd(window);
    MONITORINFO mon = {sizeof mon};
    if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &mon)) return false;
    const RECT &work = mon.rcWork;
    int w = work.right - work.left, h = work.bottom - work.top;
    w = w * 4 / 5, h = h * 4 / 5;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    g_container = CreateWindowExW(WS_EX_APPWINDOW, k_class, L"Savage Wind", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  work.left + (work.right - work.left - w) / 2,
                                  work.top + (work.bottom - work.top - h) / 2, w, h,
                                  nullptr, nullptr, instance, nullptr);
    if (!g_container) return false;
    SendMessageW(g_container, WM_SETICON, ICON_BIG, SendMessageW(game, WM_GETICON, ICON_BIG, 0));
    SendMessageW(g_container, WM_SETICON, ICON_SMALL, SendMessageW(game, WM_GETICON, ICON_SMALL, 0));
    SDL_SetWindowFullscreen(window, false);
    SDL_SyncWindow(window);
    GetWindowPlacement(game, &g_host_placement);
    if (IsZoomed(game)) ShowWindow(game, SW_RESTORE);
    g_host_style = GetWindowLongPtrW(game, GWL_STYLE);
    g_host_exstyle = GetWindowLongPtrW(game, GWL_EXSTYLE);
    ShowWindow(game, SW_HIDE);
    SetWindowLongPtrW(game, GWL_STYLE, (g_host_style & ~(WS_OVERLAPPEDWINDOW | WS_POPUP | WS_VISIBLE)) |
                                       WS_CHILD | WS_CLIPSIBLINGS);
    SetWindowLongPtrW(game, GWL_EXSTYLE, g_host_exstyle & ~WS_EX_APPWINDOW);
    SetParent(game, g_container);
    if (GetParent(game) != g_container) {
        SetWindowLongPtrW(game, GWL_STYLE, g_host_style);
        SetWindowLongPtrW(game, GWL_EXSTYLE, g_host_exstyle);
        DestroyWindow(g_container);
        g_container = nullptr;
        ShowWindow(game, SW_SHOW);
        winsize_apply(window);
        return false;
    }
    g_host = game;
    g_pane = window;
    g_sdl_host_proc = (WNDPROC)SetWindowLongPtrW(game, GWLP_WNDPROC, (LONG_PTR)host_pane_proc);
    place_pane(g_container, game, 0, SWP_FRAMECHANGED);
    ShowWindow(g_container, SW_SHOWNORMAL);
    ShowWindow(game, SW_SHOWNA);
    SetForegroundWindow(g_container);
    SetFocus(game);
    return true;
}

void restore_window(SDL_Window *window) {
    if (g_container) {
        release_mouse();
        ShowWindow(g_host, SW_HIDE);
        SetWindowLongPtrW(g_host, GWLP_WNDPROC, (LONG_PTR)g_sdl_host_proc);
        SetParent(g_host, nullptr);
        SetWindowLongPtrW(g_host, GWL_STYLE, g_host_style);
        SetWindowLongPtrW(g_host, GWL_EXSTYLE, g_host_exstyle);
        SetWindowPos(g_host, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPlacement(g_host, &g_host_placement);
        DestroyWindow(g_container);
        g_container = nullptr;
        g_host = nullptr;
        g_pane = nullptr;
        g_fullscreen = false;
        g_minimized = false;
        SDL_RaiseWindow(window);
    }
    if (g_fallback) {
        g_fallback = nullptr;
        SDL_SetWindowBordered(window, true);
        SetWindowPlacement(hwnd(window), &g_host_placement);
    }
    if (g_settings.fullscreen && !getenv("SAVAGEWIND_WINDOW")) winsize_apply(window);
}

std::wstring widen(const char *s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n);
    out.pop_back();
    return out;
}

bool linked(const fs::path &p) {
    DWORD attr = GetFileAttributesW(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
}

void check_path(const fs::path &path) {
    for (auto p = path; !p.empty() && p != p.root_path(); p = p.parent_path())
        if (linked(p)) throw std::runtime_error("linked data path");
}

void check_tree(const fs::path &path) {
    check_path(path);
    if (!fs::is_directory(path)) return;
    for (const auto &entry : fs::recursive_directory_iterator(path))
        if (linked(entry.path())) throw std::runtime_error("linked data path");
}

void copy_once(const fs::path &src, const fs::path &dst, bool directory) {
    if (fs::exists(dst)) return;
    check_tree(src);
    if (!fs::exists(src) && !directory) return;
    // Publish the save only when the copy is complete. A crash leaves a uniquely named staging folder.
    fs::path stage = dst;
    stage += L".copy-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (fs::exists(stage) || linked(stage)) throw std::runtime_error("split save staging path exists");
    try {
        if (fs::exists(src)) fs::copy(src, stage, fs::copy_options::recursive);
        else fs::create_directory(stage);
        if (!MoveFileExW(stage.c_str(), dst.c_str(), 0)) throw std::runtime_error("could not publish split data copy");
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(stage, ignored);
        throw;
    }
}

fs::path prepare_data(int slot) {
    fs::path root(widen(rt_data_root()));
    fs::path data = root / (L"split-player" + std::to_wstring(slot + 1));
    check_tree(data);
    fs::create_directory(data);
    // Two hosts sharing a data root must not launch guests that write the same save.
    HANDLE &owner = g_guests[slot].data_owner;
    owner = CreateFileW((data / L"split-owner.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (owner == INVALID_HANDLE_VALUE) {
        owner = nullptr;
        throw std::runtime_error("Player " + std::to_string(slot + 1) + "'s data folder is already in use or cannot be opened");
    }
    copy_once(fs::absolute(fs::path(widen(sp_session_save_dir()))), data / L"save", true);
    copy_once(fs::absolute(fs::path(widen(settings_file()))), data / L"savagewind.ini", false);
    copy_once(root / L"colour-luts", data / L"colour-luts", false);
    for (const auto &entry : fs::directory_iterator(root))
        if (_wcsicmp(entry.path().extension().c_str(), L".ttf") == 0)
            copy_once(entry.path(), data / entry.path().filename(), false);
    return data;
}

// (split banner) its folder: the settings (graphics card, boot picture) and fonts of player 1, an empty save folder
fs::path prepare_banner_data() {
    fs::path root(widen(rt_data_root()));
    fs::path data = root / L"split-banner";
    check_tree(data);
    fs::create_directories(data / L"save");
    copy_once(fs::absolute(fs::path(widen(settings_file()))), data / L"savagewind.ini", false);
    for (const auto &entry : fs::directory_iterator(root))
        if (_wcsicmp(entry.path().extension().c_str(), L".ttf") == 0)
            copy_once(entry.path(), data / entry.path().filename(), false);
    return data;
}

using Env = std::vector<std::pair<std::wstring, std::wstring>>;

void env_set(Env &env, const wchar_t *key, const std::wstring &value) {
    for (auto &item : env)
        if (!_wcsicmp(item.first.c_str(), key)) { item.second = value; return; }
    env.emplace_back(key, value);
}

std::vector<wchar_t> child_environment(const Env &overrides) {
    std::vector<std::wstring> vars;
    wchar_t *block = GetEnvironmentStringsW();
    if (!block) throw std::runtime_error("could not read the process environment");
    for (const wchar_t *p = block; *p; p += wcslen(p) + 1) {
        const wchar_t *eq = wcschr(p + 1, L'=');
        if (!eq) continue;
        std::wstring key(p, eq);
        bool overridden = false;
        for (const auto &item : overrides)
            if (!_wcsicmp(key.c_str(), item.first.c_str())) { overridden = true; break; }
        if (!overridden) vars.emplace_back(p);
    }
    FreeEnvironmentStringsW(block);
    for (const auto &item : overrides)
        if (!item.second.empty()) vars.push_back(item.first + L"=" + item.second);
    std::sort(vars.begin(), vars.end(), [](const std::wstring &a, const std::wstring &b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> result;
    for (const auto &var : vars) result.insert(result.end(), var.c_str(), var.c_str() + var.size() + 1);
    result.push_back(L'\0');
    return result;
}

std::wstring stop_event_name(DWORD host, int slot) {
    return L"Local\\SavageWindSplitStop-" + std::to_wstring(host) + L"-" + std::to_wstring(slot);
}

std::wstring widen_utf8(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    out.pop_back();
    return out;
}

// Player 2's sound (runtime/split_audio.h) as the settings choose it; SAVAGEWIND_COOP_SPLITAUDIO=0: always Off
int split_audio_mode() {
    if (env_off("SAVAGEWIND_COOP_SPLITAUDIO")) return SPLIT_AUDIO_OFF;
    int mode = g_settings.split_audio;
    return mode > SPLIT_AUDIO_OFF && mode < SPLIT_AUDIO_COUNT ? mode : SPLIT_AUDIO_OFF;
}

// banner: the split banner in pane slot (no game, no co-op, no sound, no controller)
bool spawn_guest(const fs::path &data, int port, int pad, bool embed, int slot, bool banner = false) {
    const char *control = getenv("SAVAGEWIND_COOP_SPLIT_CONTROL");
    bool scratch = getenv("SAVAGEWIND_CONTROL") && control && *control;
    const wchar_t *half = g_layout == COOP_SPLIT_LEFT ? L"right" : L"bottom";
    // (4-player split screen) the sound choice is player 2's; players 3 and 4 play silently
    int audio = slot == 1 ? split_audio_mode() : SPLIT_AUDIO_OFF;
    std::wstring name = L"Player " + std::to_wstring(slot + 1);
    // (dev) the scratch test control port of player n is SAVAGEWIND_COOP_SPLIT_CONTROL + n - 2
    std::wstring control_port = scratch ? std::to_wstring(atoi(control) + slot - 1) : L"";
    Env env = {
        // (coop-splitaudio) player 2's sound: none, its own device, or shared speakers (quieter, no music, faded out
        // near player 1)
        {L"SAVAGEWIND_NO_AUDIO", audio == SPLIT_AUDIO_OFF ? L"1" : L""},
        {L"SAVAGEWIND_AUDIO_DEVICE", audio == SPLIT_AUDIO_DEVICE ? widen_utf8(g_settings.split_audio_device) : L""},
        {L"SAVAGEWIND_SPLIT_AUDIO", audio == SPLIT_AUDIO_SHARED ? L"shared" : audio == SPLIT_AUDIO_DEVICE ? L"device" : L""},
        {L"SAVAGEWIND_NO_MUSIC", audio == SPLIT_AUDIO_SHARED ? L"1" : L""},
        {L"SAVAGEWIND_COOP_SPLIT_CHILD", std::to_wstring(GetCurrentProcessId())},
        {L"SAVAGEWIND_COOP_SPLIT_PARENT", embed ? std::to_wstring((uintptr_t)g_container) : L""},
        {L"SAVAGEWIND_DATA_ROOT", data.native()},
        {L"SAVAGEWIND_SAVE_DIR", (data / L"save").native()},
        {L"SAVAGEWIND_DEFAULT_SAVE_DIR", L""},
        {L"SAVAGEWIND_SETTINGS", (data / L"savagewind.ini").native()},
        {L"SAVAGEWIND_NO_LAUNCHER", L"1"},
        {L"SAVAGEWIND_HIDDEN", L""}, {L"SAVAGEWIND_WINDOW", L""},
        {L"SAVAGEWIND_SPLIT", half}, {L"SAVAGEWIND_PAD", std::to_wstring(pad)},
        {L"SAVAGEWIND_COOP_PORT", L""}, {L"SAVAGEWIND_COOP_BIND", L""},
        {L"SAVAGEWIND_COOP_PEER", L"127.0.0.1:" + std::to_wstring(port)},
        {L"SAVAGEWIND_COOP_CODE", widen(coop_net_room_code())}, {L"SAVAGEWIND_COOP_NAME", name},
        {L"SAVAGEWIND_LAUNCH_FLAGS_APPLIED", L""}, {L"SAVAGEWIND_REEXEC", L""},
        {L"SAVAGEWIND_CONTROL", control_port},
        {L"SAVAGEWIND_COOP_SPLIT_CONTROL", L""},
        {L"SAVAGEWIND_COOP_SPLIT_SLOT", std::to_wstring(slot)},
    };
    if (banner) {
        audio = SPLIT_AUDIO_OFF;
        for (const wchar_t *key : {L"SAVAGEWIND_AUDIO_DEVICE", L"SAVAGEWIND_SPLIT_AUDIO", L"SAVAGEWIND_COOP_PEER",
                                  L"SAVAGEWIND_COOP_CODE", L"SAVAGEWIND_COOP_NAME", L"SAVAGEWIND_CONTROL",
                                  L"SAVAGEWIND_NO_BOOT_SPLASH"})
            env_set(env, key, L"");
        env_set(env, L"SAVAGEWIND_NO_AUDIO", L"1");
        env_set(env, L"SAVAGEWIND_PAD", L"none");
        env_set(env, L"SAVAGEWIND_COOP_SPLIT_BANNER", L"1");
    } else {
        env_set(env, L"SAVAGEWIND_COOP_SPLIT_BANNER", L"");
    }
    if (audio == SPLIT_AUDIO_SHARED) {
        wchar_t gain[32];
        const char *base = getenv("SAVAGEWIND_AUDIO_GAIN");  // (a gain player 1 was started with applies to both)
        swprintf(gain, 32, L"%.3f", (base && *base ? atof(base) : 1.0) * SPLIT_SHARED_VOLUME);
        env.emplace_back(L"SAVAGEWIND_AUDIO_GAIN", gain);
    }
    if (!banner) fprintf(stderr, "[split] Player %d sound: %s%s%s\n", slot + 1,
            audio == SPLIT_AUDIO_SHARED ? "shared speakers" : audio == SPLIT_AUDIO_DEVICE ? "own device " : "off",
            audio == SPLIT_AUDIO_DEVICE ? "\"" : "",
            audio == SPLIT_AUDIO_DEVICE ? (std::string(g_settings.split_audio_device) + "\"").c_str() : "");
    for (const wchar_t *key : {L"SAVAGEWIND_GAME_LIB", L"SAVAGEWIND_ROMFS_OVERLAY"}) {
        const wchar_t *v = _wgetenv(key);
        if (v && *v) env.emplace_back(key, fs::absolute(fs::path(v)).native());
    }
    for (const wchar_t *key : {L"SAVAGEWIND_HOST_INPUT", L"SAVAGEWIND_BIND_TEST", L"SAVAGEWIND_REBOOT_AT",
                              L"SAVAGEWIND_SHOT_AT", L"SAVAGEWIND_LAUNCHER_MODE", L"SAVAGEWIND_LAUNCHER_SHOT",
                              L"SAVAGEWIND_LAUNCHER_EXTRACT", L"SAVAGEWIND_LAUNCHER_SETUP", L"SAVAGEWIND_LAUNCHER_DELETE"})
        env.emplace_back(key, L"");
    if (!scratch) {
        for (const wchar_t *key : {L"SAVAGEWIND_INPUT", L"SAVAGEWIND_TEST_WARP", L"SAVAGEWIND_TEST_ITEMS",
                                  L"SAVAGEWIND_TEST_FLAGS", L"SAVAGEWIND_NO_USER_INPUT"})
            env.emplace_back(key, L"");
    }
    auto vars = child_environment(env);
    wchar_t exe[32768];
    DWORD length = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    if (!length || length == std::size(exe)) return false;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"" + g_game.native() + L"\" --play --no-launch-flags";

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();
    PROCESS_INFORMATION process = {};
    bool ok = false;
    if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) {
        // The job is assigned as part of creation, so even a host crash during launch cannot leave an orphan.
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit) &&
            UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof job,
                                      nullptr, nullptr)) {
            ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, false,
                                CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                vars.data(), data.c_str(), &startup.StartupInfo, &process);
        }
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
    if (!ok) {
        fprintf(stderr, "[split] CreateProcess/job failed (%lu)\n", GetLastError());
        CloseHandle(job);
        return false;
    }
    CloseHandle(process.hThread);
    if (banner) {
        g_banner.job = job;
        g_banner.process = process.hProcess;
        fprintf(stderr, "[split] banner PID %lu in pane %d\n", process.dwProcessId, slot + 1);
        return true;
    }
    g_guests[slot].job = job;
    g_guests[slot].process = process.hProcess;
    fprintf(stderr, "[split] Player %d PID %lu, %s, controller %d\n", slot + 1, process.dwProcessId,
            embed ? "embedded" : "two windows", pad);
    g_pid = (int)process.dwProcessId;
    return true;
}

// Player 1: the container's panes after a player came or went; player 1's own pane follows at once, the others at
// their next frame (coop_split_poll)
void publish_panes() {
    if (!g_container) return;
    SetWindowLongPtrW(g_container, GWLP_USERDATA, g_panes.load() | (g_banner_slot ? 1 << g_banner_slot : 0));
    place_pane(g_container, g_host, 0, 0);
}

void close_guest(Guest &g) {
    if (HANDLE job = g.job.exchange(nullptr)) {
        // Keep the container alive until the guest has stopped presenting. Closing
        // the last job handle alone can race destruction of the guest's HWND.
        if (TerminateJobObject(job, 0)) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = {};
            for (int i = 0; i < 200 && QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                     &info, sizeof info, nullptr) && info.ActiveProcesses; i++) Sleep(5);
        }
        CloseHandle(job);
    }
}

// (split banner) gone from its pane (a player takes the pane, or fewer than three play)
void drop_banner() {
    if (!g_banner_slot) return;
    close_guest(g_banner);
    if (g_banner.process) CloseHandle(g_banner.process), g_banner.process = nullptr;
    g_banner_slot = 0;
    publish_panes();
}

// (split banner) up in the empty quarter while exactly three play; SAVAGEWIND_COOP_SPLITBANNER=0: never
void update_banner() {
    if (g_banner_slot && WaitForSingleObject(g_banner.process, 0) == WAIT_OBJECT_0) {
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
    for (HANDLE *h : {&g.process, &g.stop_event, &g.data_owner})
        if (*h) CloseHandle(*h), *h = nullptr;
    g.stop_deadline = 0;
    g.pad = 0;
    g_panes &= ~(1 << slot);
    if (g_panes == 1) g_panes = 0;
}

bool start(SDL_Window *window, int layout, int pad) {
    if (g_game.empty()) return false;
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
        // (4-player split screen) a controller already in use: the first free one instead
        auto used = [&](int p) {
            if (p == host_pad) return true;
            for (int i = 1; i < k_slots; i++)
                if (g_guests[i].process && g_guests[i].pad == p) return true;
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
    bool started_room = false;
    bool launched = false;
    try {
        if (!coop_net_on()) {
            CoopNetConfig config = {};
            config.mode = COOP_MODE_HOST;
            config.loopback = true;
            const char *port = getenv("SAVAGEWIND_COOP_SPLIT_PORT");
            if (port && *port) config.port = atoi(port);  // 0 asks Windows to choose a free UDP port
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
            bool embed = first ? !env_off("SAVAGEWIND_COOP_SPLITEMBED") : g_container != nullptr;
            HANDLE &stop = g_guests[slot].stop_event;
            stop = CreateEventW(nullptr, TRUE, FALSE, stop_event_name(GetCurrentProcessId(), slot).c_str());
            if (!stop) {
                fprintf(stderr, "[split] could not create Player %d's stop event (%lu)\n", slot + 1, GetLastError());
            } else if (embed && first && !make_container(window)) {
                fprintf(stderr, "[split] could not create the split-screen container\n");
            } else {
                g_panes |= 1 | 1 << slot;
                publish_panes();
                launched = spawn_guest(data, port, pad, embed, slot);
                if (launched) {
                    g_pad = host_pad;
                    g_guests[slot].pad = pad;
                    g_count++;
                    if (first && embed) winsize_apply(window);
                    else if (first) {
                        SDL_SetWindowFullscreen(window, false);
                        SDL_SyncWindow(window);
                        GetWindowPlacement(hwnd(window), &g_host_placement);
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

// A player's game has ended (or is ended now): its pane goes; with the last one, player 1's window is restored
void finish(SDL_Window *window, int slot) {
    close_guest(g_guests[slot]);
    WaitForSingleObject(g_guests[slot].process, 1000);
    release_slot(slot);
    g_count--;
    g_pid = 0;
    for (int i = 1; i < k_slots; i++)
        if (g_guests[i].process) g_pid = (int)GetProcessId(g_guests[i].process);
    if (g_count > 0) {
        publish_panes();
        return;
    }
    g_count = 0;
    g_pad = -2;
    drop_banner();
    restore_window(window);
}

}  // namespace

bool coop_split_available() {
    const char *legacy = getenv("SAVAGEWIND_SPLIT");
    return !is_guest() && !(legacy && *legacy);
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

// (split map) the block the games of this window share (coop_split_hud.h): player 1 makes it with its first extra
// player and keeps it while it runs; the others open it (again at most once a second while it isn't there)
SplitHud *coop_split_hud() {
    static SplitHud *hud;
    static ULONGLONG tried;
    if (hud) return hud;
    bool guest = is_guest();
    if (!guest && !g_count) return nullptr;
    ULONGLONG now = GetTickCount64();
    if (tried && now - tried < 1000) return nullptr;
    tried = now;
    DWORD host = guest ? strtoul(getenv("SAVAGEWIND_COOP_SPLIT_CHILD"), nullptr, 10) : GetCurrentProcessId();
    std::wstring name = L"Local\SavageWindSplitHud-" + std::to_wstring(host);
    HANDLE map = guest ? OpenFileMappingW(FILE_MAP_ALL_ACCESS, false, name.c_str())
                       : CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SplitHud), name.c_str());
    if (!map) return nullptr;
    auto *h = (SplitHud *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SplitHud));
    if (!h) {
        CloseHandle(map);
        return nullptr;
    }
    if (!guest) {  // (fresh pages are zero)
        h->version = SPLIT_HUD_VERSION;
        h->magic = SPLIT_HUD_MAGIC;
    } else if (h->magic != SPLIT_HUD_MAGIC || h->version != SPLIT_HUD_VERSION) {
        UnmapViewOfFile(h);
        CloseHandle(map);
        return nullptr;
    }
    return hud = h;  // (the mapping stays open until the process ends)
}
int coop_split_hud_slot() { return banner_mode() ? -1 : is_guest() ? g_slot : 0; }

void coop_split_startup() {
    split_audio_device();  // (read once here, before the sound threads ask)
    const char *host = getenv("SAVAGEWIND_COOP_SPLIT_CHILD");
    if (!host) return;
    if (!freopen("savagewind.log", "a", stderr)) exit(1);
    const char *slot = getenv("SAVAGEWIND_COOP_SPLIT_SLOT");
    g_slot = slot ? atoi(slot) : 1;
    if (g_slot < 1 || g_slot >= k_slots) exit(1);
    if (banner_mode()) return;  // (player 1 ends it with its job)
    g_stop_event = OpenEventW(SYNCHRONIZE, false, stop_event_name(strtoul(host, nullptr, 10), g_slot).c_str());
    if (!g_stop_event) exit(1);
}

void coop_split_game_path(const char *path) {
    std::error_code ec;
    g_game = fs::absolute(fs::path(widen(path)), ec);
    if (ec) g_game.clear();
    else if (!g_game.has_filename() && g_game != g_game.root_path()) g_game = g_game.parent_path();
}

void *coop_split_guest_window() {
    const char *value = getenv("SAVAGEWIND_COOP_SPLIT_PARENT");
    if (!is_guest() || !value) return nullptr;
    g_parent = (HWND)(uintptr_t)strtoull(value, nullptr, 10);
    DWORD owner = 0;
    GetWindowThreadProcessId(g_parent, &owner);
    if (!owner || owner != strtoul(getenv("SAVAGEWIND_COOP_SPLIT_CHILD"), nullptr, 10) ||
        !register_window_class()) {
        fprintf(stderr, "[split] FATAL: split-screen container unavailable\n");
        exit(1);
    }
    const char *half = getenv("SAVAGEWIND_SPLIT");
    g_layout = half && !strcmp(half, "right") ? COOP_SPLIT_LEFT : COOP_SPLIT_TOP;
    g_child = CreateWindowExW(WS_EX_TOOLWINDOW | (banner_mode() ? WS_EX_NOACTIVATE : 0),
                              banner_mode() ? k_banner_class : k_class, L"Savage Wind", WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 1, 1, g_parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_child) {
        fprintf(stderr, "[split] FATAL: could not create child window (%lu)\n", GetLastError());
        exit(1);
    }
    place_pane(g_parent, g_child, g_slot, 0);
    return g_child;
}

bool coop_split_apply(SDL_Window *window) {
    if (g_child) g_pane = window;
    if (g_container) apply_fullscreen();
    return g_container || g_child;
}

bool coop_split_minimized() {
    return g_minimized;
}

bool coop_split_window_focus(SDL_Window *window) {
    if (!window) return false;
    HWND parent = g_container ? g_container : g_parent;
    if (!parent) return (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    return GetForegroundWindow() == parent && GetFocus() == hwnd(window);
}

bool coop_split_mouse(SDL_Window *window, bool relative) {
    if (!g_container && !g_child) return false;
    if (!relative) {
        release_mouse();
        return true;
    }
    HWND pane = hwnd(window);
    RECT clip;
    if (!GetClientRect(pane, &clip)) return true;
    MapWindowPoints(pane, nullptr, (POINT *)&clip, 2);
    if (IsRectEmpty(&clip)) return true;
    int x = (clip.left + clip.right) / 2, y = (clip.top + clip.bottom) / 2;
    if (!g_mouse_held || !EqualRect(&clip, &g_mouse_clip)) {
        g_mouse_held = ClipCursor(&clip) != 0;
        g_mouse_clip = clip;
        if (g_mouse_held) SetCursorPos(x, y);
        return true;
    }
    // SDL only enables its relative mode on the foreground SDL window.
    // Keep the native pointer centred within this pane while camera input is active.
    POINT p;
    if (GetCursorPos(&p) && (p.x != x || p.y != y)) {
        SetCursorPos(x, y);
        SDL_Event e = {};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = SDL_GetWindowID(window);
        e.motion.xrel = (float)(p.x - x);
        e.motion.yrel = (float)(p.y - y);
        input_handle_event(&e);
    }
    return true;
}

const char *coop_split_host_half() { return g_fallback; }

void coop_split_poll(SDL_Window *window) {
    if (is_guest()) {
        if (g_stop_event && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) exit(0);
        if (g_child) {
            g_minimized = IsIconic(g_parent) != 0;
            static RECT last;
            static LONG_PTR last_panes;
            RECT r;
            LONG_PTR panes = GetWindowLongPtrW(g_parent, GWLP_USERDATA);
            if (GetClientRect(g_parent, &r) && (!EqualRect(&r, &last) || panes != last_panes)) {
                place_pane(g_parent, g_child, g_slot, 0);
                last = r;
                last_panes = panes;
            }
        }
        return;
    }
    for (int i = 1; i < k_slots; i++) {
        Guest &g = g_guests[i];
        if (!g.process) continue;
        if (WaitForSingleObject(g.process, 0) == WAIT_OBJECT_0 ||
            (g.stop_deadline && GetTickCount64() >= g.stop_deadline)) {
            DWORD code = 0;
            GetExitCodeProcess(g.process, &code);
            if (!g.stop_deadline && code) {
                g_failed = true;
                fprintf(stderr, "[split] player %d exited with code %lu\n", i + 1, code);
            }
            bool removed = g.stop_deadline != 0;
            finish(window, i);
            if (removed && g_request == k_busy) g_request = 0;
        }
        else if (!coop_net_on() && !g.stop_deadline) finish(window, i);
    }
    update_banner();
    int request = g_request.load();
    if (!request || request == k_busy) return;
    g_request = k_busy;
    if (request > 0) {
        drop_banner();  // (the new player may take its pane; it comes back below while three play)
        g_failed = !start(window, (request - 1) / 4, (request - 1) % 4 + 1);
        update_banner();
    } else if (int last = coop_split_last_player(); last && g_guests[last - 1].process) {
        // Remove: the last player added (the highest number) leaves
        SetEvent(g_guests[last - 1].stop_event);
        g_guests[last - 1].stop_deadline = GetTickCount64() + 3000;
        return;
    }
    g_request = 0;
}

bool coop_split_event(SDL_Window *, const SDL_Event *e) {
    if (!g_container && !g_child) return false;
    // SDL reports the foreground container as unfocused. input_mouse_update uses native pane focus.
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED || e->type == SDL_EVENT_WINDOW_FOCUS_LOST) return true;
    if (g_mouse_held && e->type == SDL_EVENT_MOUSE_MOTION) return true;  // camera deltas come from the native cursor
    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
        (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F4 && (e->key.mod & SDL_KMOD_ALT))) {
        if (g_child) PostMessageW(g_parent, WM_CLOSE, 0, 0);
        else push_quit();
        return true;
    }
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat || input_user_blocked(e)) return false;
    if (e->key.scancode == SDL_SCANCODE_TAB && (e->key.mod & SDL_KMOD_CTRL)) {
        if (g_child) PostMessageW(g_parent, k_focus_next, (WPARAM)g_child, 0);
        else if (g_container) PostMessageW(g_container, k_focus_next, 0, 0);
        return true;
    }
    if (g_child && (e->key.scancode == SDL_SCANCODE_F11 ||
                    (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_ALT)))) {
        PostMessageW(g_parent, k_fullscreen, 0, 0);
        return true;
    }
    return false;
}

void coop_split_shutdown() {
    // Also called by the Vulkan exit handler, which can run on the control thread.
    close_guest(g_banner);
    for (int i = 1; i < k_slots; i++) close_guest(g_guests[i]);
}

void coop_split_reboot(SDL_Window *window) {
    if (!is_guest()) {
        g_request = 0;
        for (int i = 1; i < k_slots; i++)
            if (g_guests[i].process) finish(window, i);
    }
}

const char *split_audio_device() {
    // (from the wide environment: a device name with accents survives, which the ANSI copy would not)
    static std::string name;
    static bool read;
    if (!read) {
        read = true;
        const wchar_t *v = _wgetenv(L"SAVAGEWIND_AUDIO_DEVICE");
        int n = v && *v ? WideCharToMultiByte(CP_UTF8, 0, v, -1, nullptr, 0, nullptr, nullptr) : 0;
        if (n > 1) {
            name.assign(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, v, -1, name.data(), n, nullptr, nullptr);
            name.pop_back();
        }
    }
    return name.empty() ? nullptr : name.c_str();
}

bool split_audio_foreground() {
    HWND parent = g_parent;
    return parent && GetForegroundWindow() == parent;
}

#elif defined(__linux__)
#include "coop_split_linux.inc"  // (X11 / XWayland: the same design, see there)
#else
#include "split_audio.h"
const char *split_audio_device() {
    const char *v = getenv("SAVAGEWIND_AUDIO_DEVICE");
    return v && *v ? v : nullptr;
}
bool split_audio_foreground() { return false; }
bool coop_split_available() { return false; }
bool coop_split_request(bool, int, int) { return false; }
int coop_split_pid() { return 0; }
int coop_split_count() { return 0; }
int coop_split_max() { return 0; }
int coop_split_next_player() { return 2; }
int coop_split_last_player() { return 0; }
bool coop_split_pending() { return false; }
bool coop_split_failed() { return false; }
int coop_split_host_pad() { return -2; }
SplitHud *coop_split_hud() { return nullptr; }
int coop_split_hud_slot() { return -1; }
void coop_split_startup() {}
void coop_split_game_path(const char *) {}
void *coop_split_guest_window() { return nullptr; }
bool coop_split_apply(SDL_Window *) { return false; }
bool coop_split_minimized() { return false; }
bool coop_split_window_focus(SDL_Window *window) { return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS); }
bool coop_split_mouse(SDL_Window *, bool) { return false; }
const char *coop_split_host_half() { return nullptr; }
void coop_split_poll(SDL_Window *) {}
bool coop_split_event(SDL_Window *, const SDL_Event *) { return false; }
void coop_split_reboot(SDL_Window *) {}
void coop_split_shutdown() {}
#endif

bool split_audio_shared() {
    const char *v = getenv("SAVAGEWIND_SPLIT_AUDIO");
    const char *off = getenv("SAVAGEWIND_COOP_SPLITAUDIO");
    return v && !strcmp(v, "shared") && !(off && !strcmp(off, "0"));
}

bool split_audio_child() { return getenv("SAVAGEWIND_COOP_SPLIT_CHILD") != nullptr; }

int split_audio_devices(char (*names)[128], int max) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return 0;
    int n = 0, k = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; ids && i < n && k < max; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (name && *name && strlen(name) < sizeof names[0]) snprintf(names[k++], sizeof names[0], "%s", name);
    }
    SDL_free(ids);
    return k;
}

bool split_audio_default_name(char *out, int cap) {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) return false;
    const char *name = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    if (!name || !*name) return false;
    snprintf(out, cap, "%s", name);
    return true;
}
