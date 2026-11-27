// Standalone HID tests use SDL window focus without the co-op process manager.
#pragma once
#include "coop_split.h"

int coop_split_host_pad(void) { return -2; }
bool coop_split_window_focus(SDL_Window *w) { return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS); }
bool coop_split_mouse(SDL_Window *w, bool relative) { (void)w; (void)relative; return false; }
