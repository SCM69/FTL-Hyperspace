# FTL Co-op (modified Hyperspace)

This branch is a **modified version of [FTL-Hyperspace](https://github.com/FTL-Hyperspace/FTL-Hyperspace)**
(GPL-3.0-or-later, see LICENSE and COPYRIGHT.md) that adds online co-op for up to 8 players to
FTL: Faster Than Light. It is not an official Hyperspace release. Changes from upstream:
`src/features/coop/`, `tests/coop/`, `libzhlgen/test/functions/ELF_amd64/1.6.13/ParticleEmitter.zhl`,
and small edits in `src/Global.cpp` and `src/zhl/zhl.cpp` (`HYPERSPACE_NO_DIALOGS`).

Status: **work in progress, not released.** Tested only on Linux, Steam FTL 1.6.13.

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
