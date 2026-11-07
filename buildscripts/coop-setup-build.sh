#!/bin/bash
# FTL co-op: builds Release Files/Windows/coop/setup/coop-setup.exe (path without spaces for docker-run-in-devcontainer.sh).
exec "$(dirname "$0")/../Release Files/Windows/coop/setup/build.sh"
