# FTL Co-op (modified Hyperspace)

This branch is a **modified version of [FTL-Hyperspace](https://github.com/FTL-Hyperspace/FTL-Hyperspace)**
(GPL-3.0-or-later, see LICENSE and COPYRIGHT.md) that adds online co-op for up to 8 players to
FTL: Faster Than Light. It is not an official Hyperspace release. Changes from upstream:
`src/features/coop/`, `tests/coop/`, `Release Files/Linux/coop-install.sh`, `Release Files/Windows/coop/`,
`ParticleEmitter.zhl` and `BatterySystem.zhl` in `libzhlgen/test/functions/ELF_amd64/1.6.13/` and `win32/1.6.9/`,
a ws2_32 link line in `CMakeLists.txt`, and small edits in `src/Global.cpp`, `src/zhl/zhl.cpp` (`HYPERSPACE_NO_DIALOGS`)
and `src/game/Utilities/Resources.cpp`.

Status: **work in progress, not released.** Tested with bots on Linux (Steam FTL 1.6.13, native) and on the
Windows build (FTL 1.6.9) running under Proton Experimental on Linux; not yet on real Windows. Windows and Linux
players cannot play together (different programs, so the simulations would differ); a Linux player can join
Windows players by running the Windows game through Proton.

Windows under Proton, 2026-11-06 (tests/coop/batch.sh with BATCH_WIN=1, all PASS): 2 copies fight, 4 copies fight
own-crew, 2 specials, 2 keys, 2 captain (ffa and own-crew, with the captain resync), 4 and 8 copies chaos, late join
(third player after 20 s), 80 ms simulated latency.

## How it works

Deterministic lockstep. Every player runs the full game. The host collects all players' commands
(crew moves, power, weapons, doors, pause, event choices, jumps), checks them against the
ship-sharing mode, and schedules them for a future frame; every copy applies them on that frame
with a fixed time step and shared random seeds. Copies compare state hashes; on a mismatch the
host saves and everyone reloads that save (also used for late joiners and to continue a run).

Ship sharing (host setting, F11 in a run): **free-for-all**, or **own crew** (each player commands
their own crew members and the systems they staff). Jumps and event choices belong to the host.

## Build

Like Hyperspace (see wiki/Building-on-Linux.md):

    buildscripts/docker-run-in-devcontainer.sh buildscripts/linux-1.6.13/build-releaseonly.sh

Build for Windows:

    buildscripts/docker-run-in-devcontainer.sh buildscripts/windows/build-releaseonly.sh
    "Release Files/Windows/coop/package.sh" build-windows-release/Hyperspace.dll FTL-Coop-Windows.zip

## Install (Windows, FTL 1.6.9)

Roll FTL back to 1.6.9 with [FTL-Version-Rollback](https://github.com/FTL-Hyperspace/FTL-Version-Rollback), copy
`Hyperspace.dll`, `xinput1_4.dll` and `coop-install.bat` from the zip next to `FTLGame.exe`, run `coop-install.bat`
(checks the 1.6.9 SHA-1, writes `coop.cfg`), edit `coop.cfg`. `README-COOP.txt` in the zip is the player guide.
Under Proton the launch option `WINEDLLOVERRIDES="xinput1_4=n,b" %command%` is required (without it Wine uses its own xinput1_4 and the mod does not load; checked with Proton Experimental).

Windows specifics: `rand`/`srand` are taken over by pointing FTLGame.exe's msvcrt imports at the mod (15-bit
results, as msvcrt's RAND_MAX); random32's state and the three `menu_pause` writes in `CommandGui::OnLoop` are found
by scanning code, and only patched when the expected instructions are there.

## Install (Linux, Steam FTL 1.6.13)

Put `Hyperspace.1.6.13.amd64.so` (from the build) next to `Release Files/Linux/coop-install.sh` and run
`./coop-install.sh` (or `./coop-install.sh "/path/to/FTL Faster Than Light"`). It checks the game's SHA-256, copies the
library into `data/`, writes `data/coop.cfg`, and changes the start script `data/FTL` so that only the game loads the
library (backup `data/FTL.before-coop`). Edit `coop.cfg`, start FTL from Steam. `./coop-install.sh --uninstall` undoes
it. The Hyperspace.ftl data mod is not needed. A Steam update or file check restores `data/FTL`: run the installer again.

## Run (environment variables or a `coop.cfg` next to `FTL.amd64`)

    # host                                  # guest
    FTL_COOP=host FTL_COOP_PLAYERS=2        FTL_COOP=guest FTL_COOP_ADDR=<host ip>:27890
    LD_PRELOAD=./Hyperspace.1.6.13.amd64.so ./FTL.amd64

The host's run starts when `FTL_COOP_PLAYERS` players are connected, or with F9. Host keys: F10 resyncs everyone
from the host's save, F11 switches ship sharing, F7 gives the selected crew members to the next player (own-crew mode
shows P1..P8 over each crew member). Players can join a running game (everyone reloads the host's save). Direct TCP on port 27890: internet play needs port forwarding or a VPN.
`coop.cfg` keys: `role`, `address`, `port`, `name`, `players`, `mode` (ffa|owncrew), `start` (new|continue).

## Tests

    g++ -std=c++11 -Isrc/features/coop/core tests/coop/fakeworld_test.cpp src/features/coop/core/*.cpp && ./a.out
    tests/coop/test_compare_logs.sh
    tests/coop/run_copies.sh -s <built .so> -g <copy of the game> -o <out dir> -n 4 -f 3600 -b fight

`run_copies.sh` runs N copies of the game under headless gamescope with an in-game bot
(`probe`, `chaos`, `fight`, `keys`) and compares their hash logs.

## Known gaps

Store, upgrades, equipment and the crew screen are the captain's: guests' clicks there are ignored, and closing
them after a change makes everyone reload the host's save. Sector-exit choices and Hyperspace crew abilities are not
commands yet: using them makes the host resync everyone. A resync waits for a quiet moment (no jump, no dialog). No lobby or Steam networking yet. Write-up and notes:
[coop-anything games/faster-than-light](https://github.com/cranebeaker/coop-anything).
