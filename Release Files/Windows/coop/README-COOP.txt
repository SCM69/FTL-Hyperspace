FTL Co-op for Windows (work in progress)
========================================

A modified FTL-Hyperspace (GPL-3.0-or-later, see LICENSE and COPYRIGHT.md) that adds online co-op for up to
8 players. Everyone runs the game; the host shares everyone's orders so all copies play the same run.
Not an official Hyperspace release. Source: https://github.com/SCM69/FTL-Hyperspace/tree/coop (COOP.md)

Everyone in a game needs FTL from Steam, the same co-op files from this zip, Steam running, and the other players
as Steam friends. Windows and Linux players cannot play together (a Linux player can use Proton, see below).

Install
  1. Unzip anywhere and run coop-setup.exe. It finds FTL in your Steam library and:
       - rolls FTLGame.exe back to 1.6.9 (the version Hyperspace needs). It downloads the matching patch from
         github.com/FTL-Hyperspace/FTL-Version-Rollback, checks it, and keeps your original as FTLGame_orig.exe;
       - copies Hyperspace.dll and xinput1_4.dll into the FTL folder;
       - writes coop.cfg (if there is none).
     If FTL is somewhere else: coop-setup.exe "D:\Games\FTL Faster Than Light"
  2. Start FTL from Steam. On the main menu, press H to host or J to join a Steam friend who hosts
     (+ / - sets the number of players, M the ship sharing). The host's run starts when everyone is in
     (or the host presses F9).

A Steam update or "Verify integrity of game files" brings back the newer FTLGame.exe: run coop-setup.exe again.
Remove everything: coop-setup.exe uninstall (restores FTLGame_orig.exe).
No internet for the setup? Put the patch file (for example Steam-1.6.14.bps from that project) next to
coop-setup.exe; it is checked the same way.

No IP addresses, port forwarding or VPN: Steam connects the players (through Valve's relays when needed).
Without Steam, set net=tcp in coop.cfg: guests set address=<host's IP>:27890, and the host forwards TCP port
27890 on their router (or everyone joins the same VPN) and allows FTL through Windows Firewall.

Settings (coop.cfg in the FTL folder, open with Notepad): role=menu|host|guest, players, mode=ffa|owncrew,
friend=<part of the host's Steam name> (with several friends hosting), timeout (seconds before a silent player
is dropped).

Keys (host): F9 start now, F10 resync everyone, F11 switch ship sharing (free-for-all / own crew),
F7 give selected crew to the next player. Jumps, event choices, the sector map, store and upgrades are the host's.

Linux players (Proton): the Windows game and these files work under Proton. In Steam, set FTL's launch
options to:  WINEDLLOVERRIDES="xinput1_4=n,b" %command%

Problems: coop.log in the FTL folder says what happened; coop-setup.log next to coop-setup.exe for the setup.
