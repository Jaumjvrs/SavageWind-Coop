#!/usr/bin/env bash
# Savage Wind co-op setup for Linux (once per PC): builds the co-op actor overlay (local/overlay next to this script)
# from THIS PC's own game files using system python3 + oead from pip.
#
# Usage:
#   ./setup_coop.sh [--game <extracted/romfs>] [--out <overlay-dir>] [--exclusive]
set -euo pipefail

COOP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_DIR="$(dirname "$COOP_DIR")"
XDG_SW="${SAVAGEWIND_DATA_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/savagewind}"

ROMFS="${SAVAGEWIND_ROMFS:-}"
OUT_DIR="${COOP_OVERLAY_DIR:-$COOP_DIR/local/overlay}"
EXCLUSIVE=0

usage() {
    cat <<'EOF'
Usage: ./setup_coop.sh [options]

Builds the co-op RomFS actor overlay (local/overlay next to this script) from your
own Savage Wind game files (extracted/romfs).

Options:
  --game <dir>       Path to extracted/romfs (or parent folder containing extracted/romfs)
  --out <dir>        Output directory for the overlay (default: local/overlay next to this script)
  --exclusive        Do not force fullscreen_mode = borderless in play/savagewind.ini
  -h, --help         Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game|-Game)
            ROMFS="$2"; shift 2 ;;
        --out|-Out)
            OUT_DIR="$2"; shift 2 ;;
        --exclusive|-Exclusive)
            EXCLUSIVE=1; shift ;;
        -h|--help|-Help)
            usage; exit 0 ;;
        *)
            echo "setup_coop.sh: unknown option: $1" >&2
            usage >&2
            exit 1 ;;
    esac
done

if [[ -n "$ROMFS" && -d "$ROMFS/extracted/romfs" ]]; then
    ROMFS="$ROMFS/extracted/romfs"
elif [[ -n "$ROMFS" && -d "$ROMFS/romfs" ]]; then
    ROMFS="$ROMFS/romfs"
fi

if [[ -z "$ROMFS" ]]; then
    for candidate in \
        "$GAME_DIR/extracted/romfs" \
        "$COOP_DIR/upstream/extracted/romfs" \
        "$COOP_DIR/upstream-avatar/extracted/romfs" \
        "$XDG_SW/extracted/romfs"; do
        if [[ -f "$candidate/Pack/TitleBG.pack" ]]; then
            ROMFS="$candidate"
            break
        fi
    done
fi

if [[ -z "$ROMFS" || ! -f "$ROMFS/Pack/TitleBG.pack" ]]; then
    echo "Error: No game files found (looked for Pack/TitleBG.pack under extracted/romfs)." >&2
    echo "Put your extracted/ folder next to the source (see docs/coop-linux.md), or pass --game /path/to/extracted/romfs." >&2
    exit 1
fi

init_coop_ini() {
    local data_dir="$1"
    local ini="$data_dir/savagewind.ini"
    mkdir -p "$data_dir"
    if [[ ! -f "$ini" ]]; then
        for src in "$GAME_DIR/savagewind.ini" "$XDG_SW/savagewind.ini"; do
            if [[ -f "$src" ]]; then
                cp "$src" "$ini"
                break
            fi
        done
    fi
    if [[ "$EXCLUSIVE" -eq 1 || "${SAVAGEWIND_COOP_EXCLUSIVE:-}" == "1" ]]; then
        return 0
    fi
    if [[ ! -f "$ini" ]]; then
        printf 'settings_version = 4\nfullscreen_mode = borderless\n' > "$ini"
        return 0
    fi
    if grep -qE '^[[:space:]]*fullscreen_mode[[:space:]]*=' "$ini"; then
        sed -i -E 's/^[[:space:]]*fullscreen_mode[[:space:]]*=[[:space:]]*(auto|exclusive)?[[:space:]]*$/fullscreen_mode = borderless/I' "$ini"
    else
        printf '\nfullscreen_mode = borderless\n' >> "$ini"
    fi
}

find_or_create_python() {
    if command -v python3 >/dev/null 2>&1; then
        if python3 -c "import importlib.metadata; assert importlib.metadata.version('oead') == '1.3.0'" >/dev/null 2>&1; then
            echo "python3"
            return 0
        fi
    else
        echo "Error: python3 not found. Install Python 3.10-3.13 and python3-venv, then run setup_coop.sh again." >&2
        exit 1
    fi

    local venv="$COOP_DIR/tools-ext/oead-venv"
    if [[ -f "$venv/Scripts/python.exe" && ! -x "$venv/bin/python" ]]; then
        venv="$COOP_DIR/tools-ext/oead-venv-linux"
    fi
    local py="$venv/bin/python"
    if [[ ! -x "$py" ]]; then
        echo "Creating Python environment at $venv..." >&2
        mkdir -p "$(dirname "$venv")"
        python3 -m venv "$venv"
    fi
    if ! "$py" -c "import importlib.metadata; assert importlib.metadata.version('oead') == '1.3.0'" >/dev/null 2>&1; then
        echo "Installing oead==1.3.0 into $venv..." >&2
        "$py" -m pip install 'oead==1.3.0' >&2
    fi
    echo "$py"
}

if [[ -x "$COOP_DIR/overlay-builder/coop-overlay" ]]; then
    # (the ready-made package: the builder with its own Python)
    BUILDER=("$COOP_DIR/overlay-builder/coop-overlay")
else
    PY="$(find_or_create_python)"
    BUILDER=("$PY" "$COOP_DIR/build_overlay.py")
fi
echo "Building the co-op files from your game files (about a minute)..."
"${BUILDER[@]}" --play --game "$ROMFS" --out "$OUT_DIR"
init_coop_ini "$COOP_DIR/play"
echo 'Co-op setup done. Start the game with play_coop.sh (next to this script).'
