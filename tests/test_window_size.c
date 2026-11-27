#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"
#include "coop_split.h"
#include "window_size.h"

Settings g_settings;
bool settings_handheld_size(int *w, int *h) { (void)w; (void)h; return false; }
void settings_fps_for_display(float hz) { (void)hz; }
bool coop_split_apply(SDL_Window *w) { (void)w; return false; }
const char *coop_split_host_half(void) { return NULL; }

int main(void) {
    const char *aspect = winsize_aspect(1600, 900);
    assert(aspect != NULL);
    assert(!strcmp(aspect, "16:9"));

    char name[64];
    winsize_name((WinSize){1600, 900}, NULL, name, sizeof name);
    assert(!strcmp(name, "1600 x 900  (16:9)"));

    WinSize list[WINSIZE_MAX];
    int n = winsize_list(list, false);
    assert(n > 0);

    bool found = false;
    for (int i = 0; i < n; i++) {
        if (list[i].w == 1600 && list[i].h == 900) {
            found = true;
            break;
        }
    }
    assert(found);

    printf("test_window_size: ok\n");
    return 0;
}
