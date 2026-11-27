// Part of runtime/coop.c (included there, same translation unit).
// (2026-11-21, coop-splitaudio) Split screen's player 2 on shared speakers (runtime/split_audio.h): both games play on
// the same speakers, so while the two players are close together (the same sounds twice) this game fades its sound
// out, and back in once they are apart. Player 1 is the room's host: split screen always joins player 1's own room
// (runtime/coop_split.cpp start: coop_net_local_join_port), so the host's state is player 1's position and area.
// Close: the same area and within SPLITAUDIO_NEAR_M; apart again past SPLITAUDIO_FAR_M (the gap keeps it from flapping
// at the edge). No word from the host for 2 s, or not in a world: apart (audible). Off: SAVAGEWIND_COOP_SPLITAUDIO=0.
#define SPLITAUDIO_NEAR_M 25.0f
#define SPLITAUDIO_FAR_M 35.0f

static void splitaudio_frame(void) {
    static int on = -1;
    if (on < 0) {
        on = split_audio_shared();
        if (on) LOG("[splitaudio] shared speakers: fade out within %.0f m of player 1, back in past %.0f m\n",
                    SPLITAUDIO_NEAR_M, SPLITAUDIO_FAR_M);
    }
    if (!on) return;
    static uint64_t frame;
    if (frame++ % 6) return;  // (~10 times a second)
    static bool quiet;
    static int shown = -1;  // the level last logged: 0 silent, 2 audible
    float d = -1;
    CoopState host;
    double age;
    uint64_t session;
    uint64_t pinfo = player_info_ptr();
    if (pinfo && local_in_world() && coop_net_get_host(&host, &age, &session) && age < 2.0 &&
        (host.flags & COOP_FLAG_IN_WORLD)) {
        float me[3];
        memcpy(me, (void *)(uintptr_t)(pinfo + 0x88), 12);
        char area[96];
        if (local_area(area, sizeof area) == host.area) {
            float dx = me[0] - host.pos[0], dy = me[1] - host.pos[1], dz = me[2] - host.pos[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!isfinite(d)) d = -1;
        }
    }
    bool want = d >= 0 && d < (quiet ? SPLITAUDIO_FAR_M : SPLITAUDIO_NEAR_M);
    if (want != quiet) {
        quiet = want;
        split_audio_duck(quiet);
        if (d >= 0) LOG("[splitaudio] player 1 at %.1f m: fading %s\n", d, quiet ? "out" : "in");
        else LOG("[splitaudio] player 1 not near (another area or no news): fading in\n");
    }
    float level = split_audio_level();
    int now = level <= 0.001f ? 0 : level >= 0.999f ? 2 : 1;
    if (now != 1 && now != shown) {
        if (shown >= 0 || now == 0) LOG("[splitaudio] level %.2f (%s)\n", level, now ? "audible" : "silent");
        shown = now;
    }
}
