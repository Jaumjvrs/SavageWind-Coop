#pragma once
#include <stdbool.h>

typedef struct SDL_Window SDL_Window;
typedef union SDL_Event SDL_Event;

#ifdef __cplusplus
extern "C" {
#endif

enum { COOP_SPLIT_TOP, COOP_SPLIT_LEFT };

bool coop_split_available(void);
bool coop_split_request(bool add, int layout, int pad);  // guest pad 1..4
int coop_split_pid(void);  // 0 when off or still launching
bool coop_split_pending(void);
bool coop_split_failed(void);
int coop_split_host_pad(void);  // -2: usual input settings, -1: keyboard only, 1..4: controller

void coop_split_startup(void);
void coop_split_game_path(const char *path);
void *coop_split_guest_window(void);  // native child window, before SDL creates its Vulkan surface
bool coop_split_apply(SDL_Window *window);  // handles size/fullscreen for embedded windows
bool coop_split_minimized(void);
bool coop_split_window_focus(SDL_Window *window);
bool coop_split_mouse(SDL_Window *window, bool relative);  // native camera input when embedded
const char *coop_split_host_half(void);  // legacy two-window fallback, host's half
void coop_split_poll(SDL_Window *window);
bool coop_split_event(SDL_Window *window, const SDL_Event *event);
void coop_split_reboot(SDL_Window *window);
void coop_split_shutdown(void);

#ifdef __cplusplus
}
#endif
