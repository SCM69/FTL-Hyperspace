#pragma once
// FTL co-op: Steam lobbies and P2P through the Steam library FTL already loads (see CoopSteam.cpp).
#include "core/CoopSteamNet.h"
#include <string>

namespace CoopSteam {
// The game's Steam, or nullptr with the reason in *err (Steam not running, library too old...).
coop::SteamApi* Open(std::string* err);
}
