# Savage Wind Co-op - changelog

## 0.2.0 (2026-11-27)

Co-op protocol 101: everyone needs this version (it doesn't connect to 0.1.0).

- **Optional tougher enemies:** the host can give enemies more life for each extra player under Options > Co-op >
  "Enemy life" (Normal, +25% ... +200% per extra player), also while playing; everyone uses the host's choice.
  The default is Normal: the game's own life.
- **Split screen for up to 4 players** on one PC: Options > Co-op > Add player 3 / Add player 4. Three or four players
  get a quarter of the window each; each player uses their own controller (a controller already in use is skipped).
  Players 3 and 4 have no sound. "Remove a player" ends the last one added. Needs a strong PC.

- **Split-screen map:** with three players the empty quarter shows a map of everyone, with the game's own pins and
  your shared markers. Regions whose Sheikah Tower nobody has activated yet look unrevealed, as in the game.
- **Tidier Co-op menu:** Online, Split screen and Co-op options (and Give a dish in the lobby) are now sub-menus.
- **Updates:** the launcher checks for a new version when it starts and asks before updating (see COOP-HOWTO).
  From this version on you don't need to download new versions by hand.
- **Linux (experimental, help wanted):** a ready-made x86-64 package (or build from source) for online co-op and
  split screen (split screen uses X11; on Wayland desktops and the Steam Deck it runs through XWayland). I don't have
  a Linux system, so this was only tested in software-rendered Linux containers on Windows, never on a real Linux PC
  or a Steam Deck.
  Expect rough edges, and any feedback is appreciated: please open an issue on GitHub with what worked and what
  didn't (your distro, desktop and GPU help a lot).
- Fixes: Stalkoblin heads stay in step between players; no flickering mouse cursor in other players' panes.
- Privacy: the co-op log now hides the second half of IP addresses (`203.0.x.x`).

## 0.1.0 - first public release (2026-11-26)


Co-op protocol 100. Needs Savage Wind 0.6.0-alpha.6. Everyone in a room needs the same version. It does not connect to
the earlier private test builds, whose room codes were easier to guess.

- **Online co-op** for up to 8 players. One player hosts, the others join with the host's address and a private
  room code. The game opens the co-op port on the host's router by itself (UPnP) and closes it again afterwards.
- **Split screen** on one PC, in one window (side by side or top and bottom), each player with their own
  controller. Player 2 can hear their game on another sound output, such as headphones.
- **See each other:** the other Link appears in your world with their gear, animations, bow, paraglider and horse,
  a name above their head, and an arrow on your map.
- **Shared world:** enemies placed on the map, time of day and weather, chests and items picked up near each other,
  map pins, story, shrine and Divine Beast progress.
- **Puzzles together:** physics objects, switches, doors and elevators stay in step in shrines, Divine Beasts and
  the open world. A shrine opened by one player opens for a partner nearby.
- **Hand things over:** give a cooked dish to a friend next to you, light your torch from theirs.
- **Your save is safe:** co-op plays on its own copy of your save; your normal save is never changed.
- **One program to start:** `Savage Wind Co-op.exe` builds the co-op files from your own game files the first time
  (about a minute) and then starts the game. No game files are included in the download.

Also new since the testers' build:

- **Same Wi-Fi address:** the host's Options > Co-op shows a "Same Wi-Fi address" row; friends on the same home
  network join with it (the internet address often does not work from inside the same network). A copies it.
- **Shrine door scene for both:** when you open a shrine with a partner nearby, their game plays the door-opening
  scene too, at the same time as yours (when they are free; otherwise the door just opens, as before).
- **Shrine monk for both:** when one player talks to the monk at the end of a shrine, the other player anywhere in
  the shrine gets the monk's scene too: both get their spirit orb and leave the shrine together. If the partner
  missed it, they are put on the exit platform instead of being sent back to the entrance elevator.
- **Enemies pick the nearer player:** in a fight, an enemy turns to the other player when that player is clearly the
  nearer one (within about 5 m), not only when hit. After the switch it keeps fighting the nearer player instead
  of running off toward the one who hit it earlier.
- **Fixed:** Stalkoblins (night skeletons) could be dead on one screen and alive on the other. Now they die
  together when their head is destroyed, as in the normal game.
- **Enemy fire, ice and shock reach the other player properly:** a Lizalfos's fireball or ice ball, a Wizzrobe's
  shot or an enemy's fire / ice / shock arrow aimed at your partner now also sets them on fire, freezes or shocks
  them, as in the normal game (before: only plain damage). Off: `SAVAGEWIND_COOP_FOE_ELEM=0`.
- **Grass moves around the other Link** when they walk or run through it.
- **Stop hosting / Stop joining without a restart:** while nobody has connected yet, the lobby's last row is "Stop":
  it goes back to the setup page (fix a wrong address, switch Host / Join). Once someone has connected, Leave still
  restarts the game.
- **Split screen remembers its layout** (left/right or top/bottom) and Player 2's controller choice.
- **Fixed:** after a cutscene, pieces of the other Link (clothes, hair, gear) could stay floating in the air.
