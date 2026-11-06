FTL Co-op for Windows (work in progress)
========================================

A modified FTL-Hyperspace (GPL-3.0-or-later, see LICENSE and COPYRIGHT.md) that adds online co-op for up to
8 players. Everyone runs the game; the host shares everyone's orders so all copies play the same run.
Not an official Hyperspace release. Source: https://github.com/SCM69/FTL-Hyperspace/tree/coop (COOP.md)

Everyone in a game needs:
  - Windows FTL 1.6.9 (Hyperspace's version). Steam FTL is newer: roll it back with
    https://github.com/FTL-Hyperspace/FTL-Version-Rollback (Code -> Download ZIP, copy into the FTL folder,
    run rollback.bat).
  - The same co-op files from this zip. Windows and Linux players cannot play together.

Install
  1. Copy Hyperspace.dll, xinput1_4.dll and coop-install.bat into the FTL folder (where FTLGame.exe is,
     usually C:\Program Files (x86)\Steam\steamapps\common\FTL Faster Than Light).
  2. Run coop-install.bat there. It checks the game version and writes coop.cfg.
  3. Edit coop.cfg with Notepad:
       host:   role=host   players=<how many of you>
       guests: role=guest  address=<host's IP>:27890
  4. Start FTL from Steam. The host's run starts when everyone is connected (or the host presses F9).

Internet play: the host forwards TCP port 27890 on their router, or everyone joins the same VPN
(for example a LAN tunnel), and allows FTL through Windows Firewall when Windows asks.

Keys (host): F9 start now, F10 resync everyone, F11 switch ship sharing (free-for-all / own crew),
F7 give selected crew to the next player. Jumps, event choices, store and upgrades are the host's.

Linux players (Proton): the Windows game and these files work under Proton. In Steam, set FTL's launch
options to:  WINEDLLOVERRIDES="xinput1_4=n,b" %command%

Problems: coop.log in the FTL folder says what happened. Remove: run "coop-install.bat uninstall".
