// Split screen: player 2's sound (Options > Co-op > Player 2 sound; runtime/coop_split.cpp starts player 2's game
// with the environment below). Off: player 2's game opens no sound (SAVAGEWIND_NO_AUDIO=1). Own device: its full sound
// on the output device SAVAGEWIND_AUDIO_DEVICE names (another output, such as headphones). Shared speakers
// (SAVAGEWIND_SPLIT_AUDIO=shared): the default device at SPLIT_SHARED_VOLUME (SAVAGEWIND_AUDIO_GAIN), the music as
// silence (SAVAGEWIND_NO_MUSIC=1, runtime/services/fs.c), and faded out while the two players are close together
// (runtime/coop/splitaudio.inc) so the same sounds do not play twice.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SPLIT_AUDIO_OFF, SPLIT_AUDIO_DEVICE, SPLIT_AUDIO_SHARED, SPLIT_AUDIO_COUNT };
#define SPLIT_SHARED_VOLUME 0.5f  // player 2's volume on shared speakers (times its own volume setting)
#define SPLIT_AUDIO_FADE_S 1.0f   // the fade out / in near player 1

// runtime/coop_split.cpp
const char *split_audio_device(void);   // UTF-8 name from SAVAGEWIND_AUDIO_DEVICE (NULL: the default device)
bool split_audio_shared(void);          // this game is player 2 on shared speakers (SAVAGEWIND_SPLIT_AUDIO=shared)
bool split_audio_foreground(void);      // player 2 in the split window: the whole window is the active one
bool split_audio_child(void);           // this game is split screen's player 2
int split_audio_devices(char (*names)[128], int max);  // the playback devices' names (UTF-8); count
bool split_audio_default_name(char *out, int cap);      // the device the default output is now

// runtime/services/audio.c: the output's fade (1 audible, 0 silent), over SPLIT_AUDIO_FADE_S
void split_audio_duck(bool quiet);
float split_audio_level(void);

#ifdef __cplusplus
}
#endif
