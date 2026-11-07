#!/bin/bash
# Builds coop-setup.exe (32-bit, static, no extra DLLs) with the devcontainer's MinGW:
#   buildscripts/docker-run-in-devcontainer.sh "Release Files/Windows/coop/setup/build.sh"
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -std=c99 -O2 -Wall -municode -static -s coop-setup.c -lwinhttp -lbcrypt -ladvapi32 -o coop-setup.exe
ls -la coop-setup.exe
