# Savage Wind Co-op on Linux (experimental)

Online and LAN co-op for the Linux x86-64 version of Savage Wind (Vulkan + SDL3). Linux players can host or join
other Linux or Windows players running the same co-op protocol version.

> **Split screen on Linux (experimental, X11):** up to 4 players on one PC in one window, as on Windows. It needs
> X11: on a Wayland desktop or the Steam Deck the game runs through **XWayland** (`Co-op/play_coop.sh` starts it with
> `SDL_VIDEO_DRIVER=x11` by default). A pure Wayland window cannot hold the other players' windows, so with
> `--wayland` there is no split screen. See "Split screen" below.

## Requirements

- Linux x86-64 (glibc 2.35+, for example Ubuntu 22.04, Debian 12, Fedora 36 or newer), a Vulkan 1.3 driver and a CPU
  with SSE4.2 (AVX2 recommended), as for [Savage Wind itself](https://github.com/Revivedx/SavageWindRecomp/blob/main/docs/linux.md).
- Your own legally dumped Savage Wind game files (BOTW 1.9.0).

## Quick start (ready-made package)

No compiler or Python needed.

1. Set up normal **Savage Wind 0.6.0-alpha.6** for Linux (the
   [x86_64 AppImage](https://github.com/Revivedx/SavageWindRecomp/releases/tag/v0.6.0-alpha.6)): choose your game files
   on its start screen, let it finish and check that the game plays.
2. Download `SavageWind-Coop-linux-x86_64.tar.gz` from the
   [co-op releases](https://github.com/Jaumjvrs/SavageWind-Coop/releases/latest) and extract it anywhere you can write:

   ```sh
   tar -xzf SavageWind-Coop-linux-x86_64.tar.gz
   ```

3. Start it:

   ```sh
   bash Co-op/play_coop.sh --menu
   ```

The first start builds the co-op files from your game files (about a minute). The script finds the game files in
`~/.local/share/savagewind/extracted`; if yours are elsewhere, put `SAVAGEWIND_EXTRACTED=/path/to/extracted` in front
of the command.

To update, extract the newer package over the same place (your co-op saves in `Co-op/play` stay). There is no
automatic update on Linux.

## Building from source (instead of the package)

Needs Python 3.10-3.13 with `venv` and `pip`, and these packages (Debian 13 / Ubuntu 25.04+ names):

```sh
sudo apt install clang lld make pkg-config python3-venv python3-pip \
    libsdl3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libvulkan-dev libshaderc-dev zlib1g-dev
```

All commands start in the folder of this repository (the one with the `Makefile`).

1. Get the source:

   ```sh
   git clone https://github.com/Jaumjvrs/SavageWind-Coop.git
   cd SavageWind-Coop
   ```

2. Put (or symlink) your `extracted/` folder there, then generate and build the game program and the game code
   library:

   ```sh
   python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
   make gen          # once: generates C from your extracted/exefs (~10-15 min)
   make -j$(nproc)   # builds build/savagewind and build/swgame.so
   ```

3. Build the co-op files once from your own `extracted/romfs`:

   ```sh
   bash Co-op/setup_coop.sh
   ```

   This creates a Python virtualenv in `Co-op/tools-ext/oead-venv`, installs `oead==1.3.0` and writes
   `Co-op/local/overlay` (`Dm_CoopLinkE.sbactorpack`, `Dm_CoopSword.sbactorpack`, `coop_astree.dat`).
   (`play_coop.sh` runs this step itself if the files are missing.)

## Playing

The commands are the same for the package and a source build.

- **Set up inside the game's Co-op menu (recommended):**

  ```sh
  bash Co-op/play_coop.sh --menu
  ```

  On the title screen choose **Co-op** (or **Options > Co-op** in game), choose **Host** or **Join**, set your name,
  port (`47000` by default) and room code, and press **Start**.

- **Host directly from the command line:**

  ```sh
  bash Co-op/play_coop.sh --code MYCODE
  ```

  (Omit `--code` to generate a random 8-character room code.)

- **Join a host directly from the command line:**

  ```sh
  bash Co-op/play_coop.sh --join 203.0.113.5 --code MYCODE
  ```

Your normal Savage Wind save is never touched: `play_coop.sh` creates a separate co-op data folder at `Co-op/play/`
(copied on the first run from the save in the repository folder or in `~/.local/share/savagewind/`; without either it
starts a new adventure). The co-op log is written to `Co-op/play/savagewind.log`.

### Split screen

```sh
bash Co-op/play_coop.sh --split
```

Then **Co-op > Split screen > Add player 2** (and 3, 4). Player 1's window becomes one window with a pane per player
(halves for two, quarters for three or four; with three, the free quarter shows the players' map). Each player has
their own game, controller (the **Player N controller** row) and data folder `Co-op/play/split-playerN` (a copy of player
1's save, made the first time). **Remove a player** ends the last one added; closing the window ends everyone.

- Keyboard and mouse belong to one pane at a time: click a pane, or press **Ctrl+Tab** to go to the next one.
- **F11** (or Alt+Enter) in any pane: the whole window fullscreen.
- Sound: Player 2 sound can be Off, Shared speakers or Own device (Co-op > Split screen), as on Windows; players 3 and
  4 are silent.
- Off switch: `SAVAGEWIND_COOP_LINUXSPLIT=0` (the Split screen rows are hidden).
- How it works: player 1 makes a plain X11 window and moves its own game window into it; the other players' games are
  started by player 1, each makes an X11 child window of it, and they end together with player 1.

### Networking notes on Linux

- Automatic router port forwarding (UPnP) in the in-game menu is Windows-only; on Linux, the menu shows
  `Forward UDP 47000 to this PC in your router's settings.` Forward **UDP port 47000** in your router when hosting over
  the internet, or use a mesh VPN such as Tailscale / WireGuard.
- If your Linux firewall (`ufw` / `firewalld` / `nftables`) blocks incoming UDP when hosting, allow UDP port `47000`
  (or whichever `--port` you chose).

## What was tested vs. not tested

I don't have a Linux system: everything below ran in Linux containers on a Windows PC.

### Tested
- The ready-made package (built on Ubuntu 22.04) on Debian 13: extracting it, building the co-op files without Python,
  and starting the game as a host up to the opening movie. The game library used was built from this source, not by
  the official Savage Wind AppImage: that match is expected but not tried.
- A full Linux x86-64 build (clang 19, SDL3 3.2.10, FFmpeg 7.1, lld). The Linux game library id differs from the
  Windows one, which is expected.
- The network unit tests (`tests/test_coop_net.c`) on Linux.
- `setup_coop.sh` building the co-op files from the game files.
- Booting into the world with software rendering (`llvmpipe`) and joining a Windows host.
- Split screen under a bare X server without a window manager: adding and removing players, switching the keyboard,
  resizing and exiting. The 3D world renders black there (for one player too), so the picture is not verified.

### Not tested
- Any real Linux desktop or Steam Deck, with a real graphics card, controllers, mouse camera, fullscreen or sound.
- Linux as the host with a Windows guest, two Linux PCs, or long play sessions.
- Automatic router port forwarding does not exist on Linux (forward the port by hand).
