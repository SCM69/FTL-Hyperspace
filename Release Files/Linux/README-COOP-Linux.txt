FTL Co-op for Linux (work in progress)
======================================

A modified FTL-Hyperspace (GPL-3.0-or-later, see LICENSE and COPYRIGHT.md) that adds online co-op for up to
8 players. Not an official Hyperspace release. Source: https://github.com/SCM69/FTL-Hyperspace/tree/coop (COOP.md)

Needs the Steam version of FTL for Linux (1.6.13), Steam running, and the other players as Steam friends.
Linux and Windows players cannot play together; to join Windows players, use the Windows version under Proton.

Install
  1. Unzip and run ./coop-install.sh (or ./coop-install.sh "/path/to/FTL Faster Than Light").
     It checks the game version, copies the library into the FTL data folder, writes data/coop.cfg and makes the
     game's start script load the library (backup data/FTL.before-coop). Nothing else is changed.
  2. Start FTL from Steam. On the main menu, press H to host or J to join a Steam friend who hosts
     (+ / - sets the number of players, M the ship sharing).

A Steam update or "Verify integrity of game files" restores the start script: run ./coop-install.sh again.
Remove: ./coop-install.sh --uninstall

Settings: data/coop.cfg (role=menu|host|guest, players, mode=ffa|owncrew, friend=, net=steam|tcp, timeout).
Keys (host): F9 start now, F10 resync everyone, F11 ship sharing, F7 give selected crew to the next player.
Problems: data/coop.log says what happened.
