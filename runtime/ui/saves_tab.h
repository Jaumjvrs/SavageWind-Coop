// The Saves tab of the start screen's Settings (runtime/ui/saves_tab.mm), drawn by menu.mm as tab 3.
#pragma once

#include <string>

void profile_failure_text(int code, std::string *head, std::string *hint);
void saves_tab_set_game_files(const char *root);  // the extracted game files (<root>/romfs/Pack/Bootup.pack): launcher.mm
void saves_tab_draw(void);                        // the page, into the current window
bool saves_tab_busy(void);                        // an import or a backup is running or waiting for the player's yes
void saves_tab_unload(void);                      // before the start screen closes: pictures freed, a waiting import dropped
