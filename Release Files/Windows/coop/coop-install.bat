@echo off
setlocal
:: FTL Co-op for Windows (FTL 1.6.9 + Hyperspace). Put this file, Hyperspace.dll and xinput1_4.dll into the FTL
:: folder (where FTLGame.exe is) and run it there. It only checks the game version and writes coop.cfg if there is none.
::   coop-install.bat            check and set up
::   coop-install.bat uninstall  remove the co-op files again
pushd "%~dp0"

if /i "%~1"=="uninstall" goto :uninstall

if not exist "FTLGame.exe" (
    echo FTLGame.exe is not in this folder. Put these files next to FTLGame.exe and run this again.
    goto :end
)
if not exist "Hyperspace.dll" echo WARNING: Hyperspace.dll is missing from this folder.
if not exist "xinput1_4.dll" echo WARNING: xinput1_4.dll is missing from this folder.

set "SHA="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "FTLGame.exe" SHA1') do if not defined SHA set "SHA=%%H"
set "SHA=%SHA: =%"
if /i not "%SHA%"=="eadd0a60dbf80c5a7c2a6879f8ae241afb553ade" (
    echo.
    echo FTLGame.exe is not the tested FTL 1.6.9 ^(SHA-1 %SHA%^).
    echo Roll it back first with https://github.com/FTL-Hyperspace/FTL-Version-Rollback ^(see README.txt^).
    echo Everyone in a co-op game needs the same FTLGame.exe and the same co-op files.
    echo.
)

if exist "coop.cfg" (
    echo coop.cfg already exists; not changed.
) else (
    > "coop.cfg" (
        echo # FTL Co-op settings. Lines starting with # are ignored.
        echo # role: host ^(you run the game others join^) or guest ^(you join a host^). Remove the line to play alone.
        echo role=host
        echo # guest: the host's address and port, for example 203.0.113.7:27890 ^(the host must forward TCP port 27890,
        echo # or everyone uses a VPN such as a LAN tunnel^)
        echo address=127.0.0.1:27890
        echo port=27890
        echo name=Captain
        echo # host: start automatically when this many players ^(you included^) are connected; 0 = press F9
        echo players=2
        echo # host: ffa ^(anyone controls anything^) or owncrew ^(everyone commands their own crew^); F11 switches in a run
        echo mode=ffa
        echo # host: new ^(new run^) or continue ^(your saved run^)
        echo start=new
    )
    echo Wrote coop.cfg.
)
echo.
echo Edit coop.cfg ^(role, address, players^), then start FTL from Steam as usual.
echo The host: allow FTL through Windows Firewall when Windows asks.
goto :end

:uninstall
if exist "Hyperspace.dll" del "Hyperspace.dll"
if exist "xinput1_4.dll" del "xinput1_4.dll"
if exist "coop.cfg" move /y "coop.cfg" "coop.cfg.removed" >nul
echo Removed Hyperspace.dll and xinput1_4.dll; coop.cfg kept as coop.cfg.removed.

:end
popd
pause
