# Savage Wind co-op — how to play (experimental)

Two players in the same Hyrule: online on two PCs, or split screen on one PC. This is an unofficial, experimental
mod for [Savage Wind](https://github.com/Revivedx/SavageWindRecomp). Expect bugs. What changed is in
`CHANGELOG.md`.

## Quick start

**You need:** Windows, and a working Savage Wind set up with **your own** game files (the normal Savage Wind setup;
the game must start and play normally first). The co-op files contain no game files: each player's co-op files are
built from their own game files.

1. Download `SavageWind-Coop.zip` (everyone in the room needs the **same** version).
2. Right-click it, **Extract All**, and put its `Co-op` folder inside your Savage Wind folder, next to
   `savagewind.exe`: the result is `Savage Wind\Co-op\Savage Wind Co-op.exe`.
3. Double-click **`Savage Wind Co-op.exe`**. The first time (and after a newer zip) it prepares the co-op files from
   your game files, which takes about a minute. On the title screen choose **Co-op**:
   - **Split screen (one PC):** pick the layout and player 2's controller, then **Add player 2**. Done.
   - **Online, hosting:** Mode **Host**, your name, **Start**. The game opens UDP port 47000 on your router by itself
     when the router allows it, and shows the **address** and **room code** to send your friends. If Windows asks
     whether the game may use the network, click **Allow**.
   - **Online, joining:** Mode **Join**, the host's address, port 47000, the host's room code, **Start**.
4. Back at the title screen choose **Continue** or **New Game** and play. When you are both in the world, the other
   player appears after a few seconds.

Coming from an older version with a `coop-dev` folder? Your co-op saves are copied over the first time you start the
new version; when everything works, you can delete the old `coop-dev` folder.

Your normal save is never changed: co-op plays on its own copy.

## Updates

When you start `Savage Wind Co-op.exe`, it checks GitHub for a newer release and asks if you want to update (keeping
your saves and settings). If you are offline or GitHub cannot be reached, the game starts normally. To turn update
checks off, set the environment variable `SAVAGEWIND_COOP_UPDATE=0` or add `enabled = 0` to `Co-op\local\updater.ini`.

## Hosting online

The host's PC must be reachable from the internet on **UDP port 47000** (only this port).

- When you host, the game asks your router to forward the port to your PC (UPnP) and closes it again when you stop.
  Options > Co-op shows how it went and the **address** to send. You can turn this off there ("Open router port
  automatically"). If the router does not answer, forward UDP 47000 to your PC by hand in the router's settings
  ("port forwarding").
- If the game says your router's address is **shared (CGNAT)**, your internet company does not let anyone reach you
  from outside: let your friend host, or use a VPN such as Tailscale (then use the VPN address and skip the router).
- On the same home network (same Wi-Fi), join with the host PC's local address, not the internet one (that one
  often does not work from inside the same network). The host sees it in Options > Co-op on the "Same Wi-Fi
  address" row (A copies it). No router port needed.
- Windows may warn that `Savage Wind Co-op.exe` comes from the internet ("Windows protected your PC"): click
  **More info** > **Run anyway**. It is not signed, like most free mods; its source code is on GitHub.
- The first time, Windows may ask whether `savagewind.exe` may use the network: allow it.
- The **room code** keeps strangers out: the game ignores anyone without the same code. Don't post it publicly.

## Saves

**Main** is your co-op save (a copy of your normal save, made the first time). Options > Co-op > **Save** can make a
**New save** for a different group; choose it and press **Use this save** (the game restarts at the title screen,
so save first). Each PC keeps its own saves.

## Split screen (both players on one PC)

Options > Co-op: choose a layout (top/bottom or side by side) and player 2's controller, then **Add player 2**.
Both games share one window. Player 2 loads a save in their half (their own copy, in the `split-player2` folder).

- Click a half or press **Ctrl+Tab** to move the keyboard and mouse there. **F11**: fullscreen. **Remove player 2**
  ends their game; closing the window ends both.
- **Player 2 sound:** **Off** (only player 1's game makes sound), **own device** (player 2 hears everything on another
  output, e.g. headphones, chosen in the next row), or **shared speakers** (player 2's game plays quieter, without
  music, and goes silent while the two of you are close together). A change applies the next time you add player 2.
- Two games at once need a strong PC: lower the graphics settings in each half if it stutters.
- **Up to four players:** after player 2, pick the next player's controller and press **Add player 3**, then
  **Add player 4**. With three or four players each game gets a quarter of the window (player 1 top left); with three, the
  fourth quarter shows a map with everyone's dot and name, and each player's hearts and stamina
  (and "In a shrine" when someone is in one), with the map's stamps and pins. The map zooms out as you spread out; **Map zoom (3 players)** (Near / Normal / Far) sets
  how close it gets. Players 3 and 4 play without sound. **Remove a player** ends the last one added. Each extra game is a full copy of the game:
  four at once need a very strong PC (lots of memory and a fast processor); use the Potato graphics preset in each.

## What is shared

Each player plays their own copy of the game; the mod keeps the important parts in step:

- Each other's Link (gear, animations, riding), enemies placed on the map (the nearer player's game runs them), time
  of day and weather, chests and items picked up near each other, physics objects and puzzles in shrines and nearby
  in the open world, switches, doors, Divine Beast and shrine progress, map pins and stamps (Options > Co-op > Shared map markers: Off / Pins only / All), cooked dishes you hand over.
- Shrines and towers: when one of you opens a shrine, it opens for a partner nearby. Each rides the elevator alone,
  or both together when both stand on it.

## Known limits

- The other player only shows when you are both in the same area (the overworld, or the same shrine / Divine Beast).
- Each player gets their own loot from enemies and broken objects (both get a share).
- Birds, fish and insects that appear at random are each player's own (random enemies and animals are shared).
- If the other player stops for 3 seconds (closed game, loading screen), they are hidden until they are back.
- Everyone in a room is trusted: a player in your room can change shared things in your game (items, progress).
  Only play with people you know.

## Off switches

Every co-op feature can be turned off with an environment variable `SAVAGEWIND_COOP_<NAME>=0` set before the game
starts (for example `SAVAGEWIND_COOP_SPLITAUDIO=0`). Players normally never need these; the names are in the
source code (`runtime/coop`).

Enemy life: enemies have the game's normal life by default. For a harder game with more players, the host can give
them more life for each extra player in Options > Co-op > "Enemy life"; the others use the host's choice.

## Problems

- Log: `Co-op\play\savagewind.log`. Lines starting with `[coop]` show the connection (`peer ... connected`).
- "not the same join code": the room codes differ.
- "runs co-op protocol N, this build has M": the two PCs have different versions. Everyone extracts the newest zip.
- "Missing ..." or a setup error: extract the zip again and start `Savage Wind Co-op.exe` again.
- The co-op version only works with the Savage Wind version it was built for; if Savage Wind updates, wait for a
  matching co-op zip.
- When asking for help, share only the error text or the co-op log, privately. Never send game files, saves or the
  `extracted` folder.
