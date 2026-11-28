#!/usr/bin/env bash
# Savage Wind co-op (experimental) - start the game on Linux for playing together online.
#
#   Host:  ./play_coop.sh --code <join code>
#   Join:  ./play_coop.sh --join 1.2.3.4 --code <join code>
#   Menu:  ./play_coop.sh --menu
#          co-op set up in the game (title screen > Co-op, or Options > Co-op); --join/--port/--code are ignored.
#
# Uses the co-op Linux build (savagewind + swgame.so) and the co-op actor overlay (local/overlay next to this
# script), with its own data folder "play" next to it: settings, shader cache and a CO-OP COPY of the save (copied
# from the normal game the first time; the normal save is never written).
# Split screen (players 2-4 on this PC, one window): ./play_coop.sh --split, then Co-op > Split screen > Add player 2.
# It needs X11: on a Wayland desktop (and the Steam Deck) the game runs through XWayland (SDL_VIDEO_DRIVER=x11, the
# default here; --wayland keeps native Wayland, without split screen).
set -euo pipefail

COOP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_DIR="$(dirname "$COOP_DIR")"
XDG_SW="${XDG_DATA_HOME:-$HOME/.local/share}/savagewind"

JOIN=""
ASK_JOIN=0
FROM_WINDOW=0
PORT=47000
CODE=""
FRESH_SAVE=0
PROFILE="play"
PAD=""
MUTE=0
MENU=0
EXCLUSIVE=0
WAYLAND=0

usage() {
    cat <<'EOF'
Usage: ./play_coop.sh [options]

Start Savage Wind co-op on Linux (Host, Join, or in-game Co-op menu).

Options:
  --join <ip[:port]>     Join the host at IPv4 address (or ip:port); omit to host
  --ask-join             Prompt interactively for the host IPv4 address and room code
  --port <1024..65535>   UDP port (default: 47000)
  --code <code>          Room code (both players must use the same code)
  --menu                 Configure co-op inside the game (title screen > Co-op or Options > Co-op)
  --fresh-save           Archive the current co-op save and copy the normal save again
  --profile <name>       Data folder name next to this script (default: play, or play-NAME)
  --pad <1..4|none>      Controller number (1..4) or 'none' (keyboard and mouse only)
  --mute                 Start with audio muted (SAVAGEWIND_NO_AUDIO=1)
  --exclusive            Do not force fullscreen_mode = borderless in play/savagewind.ini
  --foreground           Wait for the game process in the foreground instead of backgrounding
  --split                Split screen on this PC: like --menu; add players in Co-op > Split screen
  --wayland              Native Wayland instead of XWayland (no split screen then)
  -h, --help             Show this help message

Split screen needs X11 (on Wayland desktops and the Steam Deck: XWayland, used by default).
Each extra player has a data folder play/split-playerN (a copy of player 1's save, made once).
EOF
}

FOREGROUND=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --join|-Join)
            JOIN="$2"; shift 2 ;;
        --ask-join|-AskJoin)
            ASK_JOIN=1; shift ;;
        --from-window|-FromWindow)
            FROM_WINDOW=1; shift ;;
        --port|-Port)
            PORT="$2"; shift 2 ;;
        --code|-Code)
            CODE="$2"; shift 2 ;;
        --fresh-save|-FreshSave)
            FRESH_SAVE=1; shift ;;
        --profile|-Profile)
            PROFILE="$2"; shift 2 ;;
        --pad|-Pad)
            PAD="$2"; shift 2 ;;
        --mute|-Mute)
            MUTE=1; shift ;;
        --menu|-Menu)
            MENU=1; shift ;;
        --exclusive|-Exclusive)
            EXCLUSIVE=1; shift ;;
        --foreground|-Foreground)
            FOREGROUND=1; shift ;;
        --split|-Split)
            MENU=1; shift
            # (an old Windows-style half argument, e.g. --split top: not needed, the game lays the panes out)
            [[ ${1:-} =~ ^(top|bottom|left|right)$ ]] && shift
            ;;
        --wayland|-Wayland)
            WAYLAND=1; shift ;;
        -h|--help|-Help)
            usage; exit 0 ;;
        *)
            echo "play_coop.sh: unknown option: $1" >&2
            usage >&2
            exit 1 ;;
    esac
done

if [[ "$FROM_WINDOW" -eq 1 ]]; then
    JOIN="${SW_WINDOW_JOIN:-}"
    CODE="${SW_WINDOW_CODE:-}"
    unset SW_WINDOW_JOIN SW_WINDOW_CODE
fi

if [[ "$ASK_JOIN" -eq 1 ]]; then
    read -r -p "Host IPv4 address: " JOIN
    JOIN="$(printf '%s' "$JOIN" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    if [[ -z "$JOIN" ]]; then
        echo "Error: Enter the host address to join." >&2
        exit 1
    fi
    read -r -p "Room code: " CODE
fi

if ! [[ "$PORT" =~ ^[0-9]+$ ]] || (( PORT < 1024 || PORT > 65535 )); then
    echo "Error: Port must be between 1024 and 65535." >&2
    exit 1
fi

if [[ -n "$JOIN" ]]; then
    if ! [[ "$JOIN" =~ ^([0-9]{1,3}(\.[0-9]{1,3}){3})(:([0-9]+))?$ ]]; then
        echo "Error: Enter an IPv4 address, optionally followed by :port." >&2
        exit 1
    fi
    ip_part="${BASH_REMATCH[1]}"
    port_part="${BASH_REMATCH[4]:-}"
    IFS='.' read -r o1 o2 o3 o4 <<< "$ip_part"
    for oct in "$o1" "$o2" "$o3" "$o4"; do
        if (( 10#$oct < 0 || 10#$oct > 255 )); then
            echo "Error: Invalid host IP address." >&2
            exit 1
        fi
    done
    if [[ -n "$port_part" ]] && (( 10#$port_part < 1 || 10#$port_part > 65535 )); then
        echo "Error: Invalid host port." >&2
        exit 1
    fi
fi

if [[ -n "$PAD" ]] && ! [[ "$PAD" =~ ^([1-4]|none)$ ]]; then
    echo "Error: --pad must be 1..4 or none." >&2
    exit 1
fi

if ! [[ "$PROFILE" =~ ^play(-[a-zA-Z0-9_-]+)?$ ]]; then
    echo "Error: Profile must be play or play-NAME (letters, digits, underscores and hyphens only)." >&2
    exit 1
fi

DATA_DIR="$COOP_DIR/$PROFILE"
# Reject symlinks in ancestors or existing data tree (matches launcher_common.ps1 Get-CoopDataPath).
ancestor="$DATA_DIR"
while [[ -n "$ancestor" && "$ancestor" != "/" ]]; do
    if [[ -L "$ancestor" ]]; then
        echo "Error: Linked data paths are not supported: $ancestor" >&2
        exit 1
    fi
    ancestor="$(dirname "$ancestor")"
done
if [[ -d "$DATA_DIR" ]]; then
    bad_link="$(find "$DATA_DIR" -type l -print -quit 2>/dev/null || true)"
    if [[ -n "$bad_link" ]]; then
        echo "Error: Linked data paths are not supported: $bad_link" >&2
        exit 1
    fi
fi

# Locate extracted/ game files.
EXTRACTED="${SAVAGEWIND_EXTRACTED:-}"
if [[ -z "$EXTRACTED" ]]; then
    for candidate in \
        "$GAME_DIR/extracted" \
        "$COOP_DIR/upstream/extracted" \
        "$COOP_DIR/upstream-avatar/extracted" \
        "$XDG_SW/extracted"; do
        if [[ -d "$candidate/romfs" ]]; then
            EXTRACTED="$candidate"
            break
        fi
    done
fi
if [[ -z "$EXTRACTED" || ! -d "$EXTRACTED/romfs" ]]; then
    echo "Error: Could not find extracted/romfs. Set SAVAGEWIND_EXTRACTED=/path/to/extracted." >&2
    exit 1
fi

# Locate the Linux co-op executable (savagewind) and game library (swgame.so).
EXE="${SAVAGEWIND_COOP_EXE:-}"
LIBRARY="${SAVAGEWIND_GAME_LIB:-}"
SOURCE_BUILD=0
if [[ -z "$EXE" ]]; then
    # (the public repository: this script is in Co-op/, the build in the folder above; then the developer layout)
    for candidate in \
        "$GAME_DIR/build-linux/savagewind" \
        "$GAME_DIR/build/savagewind" \
        "$COOP_DIR/upstream/build-linux/savagewind" \
        "$COOP_DIR/upstream/build/savagewind" \
        "$COOP_DIR/upstream-avatar/build-linux/savagewind" \
        "$COOP_DIR/upstream-avatar/build/savagewind" \
        "$COOP_DIR/savagewind"; do
        if [[ -x "$candidate" ]]; then
            EXE="$candidate"
            [[ "$candidate" != "$COOP_DIR/savagewind" ]] && SOURCE_BUILD=1
            break
        fi
    done
fi
if [[ -z "$EXE" || ! -x "$EXE" ]]; then
    echo "Error: Co-op Linux executable (savagewind) not found. Build it first or set SAVAGEWIND_COOP_EXE." >&2
    exit 1
fi

if [[ -z "$LIBRARY" ]]; then
    exe_dir="$(dirname "$EXE")"
    if [[ -f "$exe_dir/swgame.so" ]]; then
        LIBRARY="$exe_dir/swgame.so"
    elif [[ -f "$EXTRACTED/swgame.so" ]]; then
        LIBRARY="$EXTRACTED/swgame.so"
    fi
fi
if [[ -z "$LIBRARY" || ! -f "$LIBRARY" ]]; then
    echo "Error: Missing swgame.so - the Linux game library is not built or set up on this PC." >&2
    exit 1
fi

if [[ "$SOURCE_BUILD" -eq 0 && -f "$COOP_DIR/gamelib.id" && -f "$LIBRARY.id" ]]; then
    # (one id per line: the normal one and the one Savage Wind builds for CPUs without AVX2)
    have="$(tr -d '[:space:]' < "$LIBRARY.id")"
    if ! tr -d '\r' < "$COOP_DIR/gamelib.id" | grep -qxF -- "$have"; then
        echo "Error: This co-op build doesn't match your Savage Wind game library ($have)." >&2
        echo "Set up the Savage Wind version this co-op release asks for (see coop-linux.md), then try again." >&2
        exit 1
    fi
fi

OVERLAY="$COOP_DIR/local/overlay"
if [[ -n "${COOP_OVERLAY_DIR:-}" && -d "${COOP_OVERLAY_DIR}" ]]; then
    OVERLAY="$COOP_OVERLAY_DIR"
fi

STAMP="$OVERLAY/coop_overlay_version.txt"
STAMP_OK=0
if [[ -f "$STAMP" ]] && [[ "$(head -n 1 "$STAMP" | tr -d '\r\n')" == "2026-11-21-heap3" ]]; then
    STAMP_OK=1
fi
if [[ "$STAMP_OK" -ne 1 || ! -f "$OVERLAY/coop_astree.dat" || ! -f "$OVERLAY/Actor/Pack/Dm_CoopLinkE.sbactorpack" ]]; then
    echo "Setting up the co-op files from your game files (about a minute, only once)..."
    setup_args=(--game "$EXTRACTED/romfs" --out "$OVERLAY")
    (( EXCLUSIVE )) && setup_args+=(--exclusive)
    bash "$COOP_DIR/setup_coop.sh" "${setup_args[@]}"
fi

for f in "$EXE" "$LIBRARY" "$OVERLAY/Actor/Pack/Dm_CoopLinkE.sbactorpack" "$OVERLAY/Actor/Pack/Dm_CoopSword.sbactorpack"; do
    if [[ ! -f "$f" ]]; then
        echo "Error: Missing $f - the co-op build or overlay is not set up on this PC." >&2
        exit 1
    fi
done

if pgrep -x savagewind >/dev/null 2>&1; then
    echo "Warning: Savage Wind is already running." >&2
fi

# Prepare the co-op data folder once from the normal game's settings and save.
mkdir -p "$DATA_DIR"

shopt -s nullglob
for ttf in "$GAME_DIR"/*.ttf "$XDG_SW"/*.ttf; do
    base="$(basename "$ttf")"
    [[ -f "$DATA_DIR/$base" ]] || cp "$ttf" "$DATA_DIR/$base"
done
shopt -u nullglob

if [[ ! -d "$DATA_DIR/colour-luts" ]]; then
    if [[ -d "$GAME_DIR/colour-luts" ]]; then
        cp -R "$GAME_DIR/colour-luts" "$DATA_DIR/colour-luts"
    elif [[ -d "$XDG_SW/colour-luts" ]]; then
        cp -R "$XDG_SW/colour-luts" "$DATA_DIR/colour-luts"
    fi
fi

INI="$DATA_DIR/savagewind.ini"
if [[ ! -f "$INI" ]]; then
    for src in "$GAME_DIR/savagewind.ini" "$XDG_SW/savagewind.ini"; do
        if [[ -f "$src" ]]; then
            cp "$src" "$INI"
            break
        fi
    done
fi
if [[ "$EXCLUSIVE" -eq 0 && "${SAVAGEWIND_COOP_EXCLUSIVE:-}" != "1" ]]; then
    if [[ ! -f "$INI" ]]; then
        printf 'settings_version = 4\nfullscreen_mode = borderless\n' > "$INI"
    elif grep -qE '^[[:space:]]*fullscreen_mode[[:space:]]*=' "$INI"; then
        sed -i -E 's/^[[:space:]]*fullscreen_mode[[:space:]]*=[[:space:]]*(auto|exclusive)?[[:space:]]*$/fullscreen_mode = borderless/I' "$INI"
    else
        printf '\nfullscreen_mode = borderless\n' >> "$INI"
    fi
fi

if [[ ! -d "$DATA_DIR/cache" ]]; then
    mkdir -p "$DATA_DIR/cache"
    for cdir in "$GAME_DIR/cache" "$XDG_SW/cache"; do
        if [[ -d "$cdir" ]]; then
            find "$cdir" -maxdepth 1 -type f -exec cp {} "$DATA_DIR/cache/" \;
            break
        fi
    done
fi

SAVE_DIR="$DATA_DIR/save"
if [[ "$FRESH_SAVE" -eq 1 && -d "$SAVE_DIR" ]]; then
    old="$SAVE_DIR-old-$(date +%Y%m%d-%H%M%S)"
    mv "$SAVE_DIR" "$old"
    echo "Previous co-op save kept as $old"
fi
if [[ ! -d "$SAVE_DIR" ]]; then
    if [[ -d "$GAME_DIR/save" ]]; then
        cp -R "$GAME_DIR/save" "$SAVE_DIR"
        echo "Co-op save created from your normal save (your normal save stays untouched)."
    elif [[ -d "$XDG_SW/save" ]]; then
        cp -R "$XDG_SW/save" "$SAVE_DIR"
        echo "Co-op save created from your normal save (your normal save stays untouched)."
    else
        mkdir -p "$SAVE_DIR"
        echo "No normal save on this PC yet: the co-op game starts a new adventure."
    fi
fi

new_room_code() {
    local alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
    local out=""
    local byte idx
    for _ in 1 2 3 4 5 6 7 8; do
        byte="$(od -An -N1 -tu1 /dev/urandom | tr -d '[:space:]')"
        idx=$(( byte & 31 ))
        out="${out}${alphabet:$idx:1}"
    done
    printf '%s' "$out"
}

export SAVAGEWIND_DATA_ROOT="$DATA_DIR"
if [[ "$MENU" -eq 1 ]]; then
    unset SAVAGEWIND_SAVE_DIR
    export SAVAGEWIND_DEFAULT_SAVE_DIR="$SAVE_DIR"
else
    export SAVAGEWIND_SAVE_DIR="$SAVE_DIR"
fi
export SAVAGEWIND_SETTINGS="$INI"
export SAVAGEWIND_GAME_LIB="$LIBRARY"
export SAVAGEWIND_NO_LAUNCHER="1"
export SAVAGEWIND_ROMFS_OVERLAY="$OVERLAY"

# Split screen puts every player's window into one X11 window: SDL on its x11 driver (XWayland on a Wayland desktop)
if [[ "$WAYLAND" -eq 1 ]]; then
    unset SDL_VIDEO_DRIVER
elif [[ -n "${DISPLAY:-}" ]]; then
    export SDL_VIDEO_DRIVER=x11
elif [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
    echo "Note: no X display (XWayland) found: split screen is unavailable." >&2
fi

unset SAVAGEWIND_CONTROL SAVAGEWIND_HIDDEN SAVAGEWIND_TEST_WARP SAVAGEWIND_INPUT SAVAGEWIND_LOG_FILES \
      SAVAGEWIND_SPLIT SAVAGEWIND_PAD SAVAGEWIND_NO_AUDIO SAVAGEWIND_WINDOW SAVAGEWIND_COOP_MENU

if [[ "$MENU" -eq 1 ]]; then
    unset SAVAGEWIND_COOP_PORT SAVAGEWIND_COOP_PEER SAVAGEWIND_COOP_CODE SAVAGEWIND_COOP_NAME
    [[ -n "$PAD" ]] && export SAVAGEWIND_PAD="$PAD"
    [[ "$MUTE" -eq 1 ]] && export SAVAGEWIND_NO_AUDIO="1"
    echo "Co-op is set up in the game: title screen > Co-op (or Options > Co-op), then Start."
    echo "Split screen: Co-op > Split screen > Add player 2 (Ctrl+Tab or a click moves the keyboard between players)."
else
    CODE="$(printf '%s' "$CODE" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    if [[ -z "$JOIN" && -z "$CODE" && "${SAVAGEWIND_COOP_OPEN:-}" != "1" ]]; then
        CODE="$(new_room_code)"
    fi
    if [[ -n "$CODE" ]]; then
        export SAVAGEWIND_COOP_CODE="$CODE"
    else
        unset SAVAGEWIND_COOP_CODE
    fi
    [[ -n "$PAD" ]] && export SAVAGEWIND_PAD="$PAD"
    [[ "$MUTE" -eq 1 ]] && export SAVAGEWIND_NO_AUDIO="1"
    if [[ -n "$JOIN" ]]; then
        if [[ "$JOIN" =~ :[0-9]+$ ]]; then
            PEER="$JOIN"
        else
            PEER="${JOIN}:${PORT}"
        fi
        unset SAVAGEWIND_COOP_PORT
        export SAVAGEWIND_COOP_PEER="$PEER"
        echo "Joining $PEER"
    else
        unset SAVAGEWIND_COOP_PEER
        export SAVAGEWIND_COOP_PORT="$PORT"
        echo "Hosting on UDP port $PORT."
        [[ -n "$CODE" ]] && echo "Room code: $CODE"
        [[ -z "$CODE" ]] && echo "Open room: anyone who can reach this port can join."
        echo "Give the other player this PC's public IP address (your router must forward UDP port $PORT to this PC)."
    fi
fi

LOG="$DATA_DIR/savagewind.log"
if [[ -f "$LOG" ]]; then
    mv -f "$LOG" "$DATA_DIR/savagewind.prev.log"
fi

cd "$DATA_DIR"
if [[ "$FOREGROUND" -eq 1 ]]; then
    exec "$EXE" "$EXTRACTED" --play >"$DATA_DIR/stdout.log" 2>"$LOG"
fi

"$EXE" "$EXTRACTED" --play >"$DATA_DIR/stdout.log" 2>"$LOG" &
PID=$!
sleep 2.5
if ! kill -0 "$PID" 2>/dev/null; then
    wait "$PID" || EXIT_CODE=$?
    echo "Error: The game exited during startup (code ${EXIT_CODE:-1}). See $LOG" >&2
    exit 1
fi
echo "Game process started (PID $PID). Log: $LOG"
(
    wait "$PID" 2>/dev/null
    code=$?
    printf '%s game exited, code %d (0x%08X)\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$code" "$code" >> "$DATA_DIR/exit.log"
) >/dev/null 2>&1 &
