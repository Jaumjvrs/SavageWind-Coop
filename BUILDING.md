# Build the co-op mod yourself

You don't have to trust the `.exe` files in the release: you can build the same `Co-op` folder from this source code
on your own PC. It takes about 15-30 minutes the first time (mostly installing the tools) and about 5-10 minutes after
that.

No game files are needed or used to build it. Your own game files are only used later, on the first start, when the
co-op starter makes the co-op files from them (the same as with the release zip).

## What you need

- Windows 10 or 11, 64-bit, with [Git for Windows](https://git-scm.com/download/win).
- About 3 GB of free disk space for the build tools.
- Savage Wind **0.6.0-alpha.6** already set up and working (to play; not needed for the build itself).

## Steps

1. Get the source, keeping its LF line endings (needed: the build checks it):

   ```powershell
   git clone -c core.autocrlf=false https://github.com/Jaumjvrs/SavageWind-Coop.git
   cd SavageWind-Coop
   ```

2. Build. The first time, add `-Setup`: it runs Savage Wind's own `tools\setup_windows.ps1`, which installs
   [MSYS2](https://www.msys2.org/) (the clang compiler, make and the libraries) and Python 3.12. Leave it out on
   later builds.

   ```powershell
   powershell -ExecutionPolicy Bypass -File Co-op\build_coop.ps1 -Setup
   ```

3. The result is `build\coop-dist\Co-op` (and the same folder zipped, `build\coop-dist\SavageWind-Coop.zip`). Put
   that `Co-op` folder inside your Savage Wind folder, next to `savagewind.exe`, and start
   `Co-op\Savage Wind Co-op.exe`, as in the [player guide](Co-op/COOP-HOWTO.md).

## What the build does

`Co-op\build_coop.ps1` starts `Co-op/build_coop.sh`, which you can read before running it:

| Step | Makes | From |
|---|---|---|
| 1 | `savagewind.exe`: the game program with the co-op code | `runtime/` (Savage Wind's source plus `runtime/coop*`), `make` |
| 2 | `Savage Wind Co-op.exe`: the starter (one-time setup, then starts the game) | `Co-op/launcher/coop_launcher.c` |
| 3 | `py\`: a portable Python 3.12 with [oead](https://pypi.org/project/oead/) | your Python 3.12 install; oead from PyPI (pip) |
| 4 | the `Co-op` folder and zip, with `manifest.json` (SHA-256 of each file) | the files above, `Co-op/build_overlay.py`, the guide |

The only download during the build itself is oead (pinned to 1.3.0) through pip; `-Setup` downloads MSYS2's packages and
Python 3.12 the first time.

## Playing with people who use the release zip

A build from the same source plays with the release zip: what has to match between players is the co-op protocol
(100 for 0.1.x), which comes from the source, not from who built it. Build from the tag of the release your friends
use (for example `git checkout v0.1.0`) to be sure.

Your `savagewind.exe` won't be byte-for-byte the same as the release's (compilers put build details into the
program), so its SHA-256 differs from the release's `manifest.json`. That is normal.

## If the build stops

- **"game library id ..., expected ..."**: the clone has Windows (CRLF) line endings. Clone again with
  `-c core.autocrlf=false` as in step 1. (The id makes sure the co-op program fits the game code your Savage Wind
  built; the starter refuses to start with a wrong one.)
- **"missing clang / make / windres" or "missing Python 3.12"**: run step 2 with `-Setup`.
- Anything else: open an issue with the last lines the build printed (no game files, please).

## For developers

The co-op code is part of the normal Savage Wind build: see [docs/SAVAGE-WIND-README.md](docs/SAVAGE-WIND-README.md)
and [docs/BUILD-windows.md](docs/BUILD-windows.md). `make` without a `generated/` folder builds only the program,
which is all the co-op mod ships; the game code library (`swgame.dll`) is always built on each player's PC by Savage
Wind's own setup, from their own game files. Protocol unit tests: `tests/test_coop_net.c` (run by the GitHub workflow
in `.github/workflows/coop-tests.yml`).
