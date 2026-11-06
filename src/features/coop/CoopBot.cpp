// FTL co-op: in-game test bots (FTL_COOP_BOT). They submit commands like a player would.
//  probe : captain answers event dialogs with option 1, nothing else.
//  chaos : every copy issues random crew moves, power changes, door toggles, weapon power and rare pauses.
// The bot draws from its own generator so it never consumes the game's random numbers.
#include "CoopCommands.h"
#include "Global.h"
#include <random>
#include <cstdio>

namespace CoopBot {

static std::mt19937 g_rng;
static bool g_seeded = false;

static int Rand(int n) { return n > 0 ? (int)(g_rng() % (unsigned)n) : 0; }

void BeforeFrame(uint32_t frame)
{
    const CoopGame::Config& cfg = CoopGame::GetConfig();
    if (cfg.bot.empty()) return;
    if (!g_seeded) { g_seeded = true; g_rng.seed(1234u + 7919u * (unsigned)CoopGame::LocalSlot() + cfg.seed); }
    CommandGui* gui = G_->GetWorld()->commandGui;
    ShipManager* ship = G_->GetShipManager(0);
    if (!gui || !ship) return;
    bool captain = CoopGame::LocalSlot() == 0;
    if (frame % 600 == 0)
    {
        ShipManager* en = G_->GetShipManager(1);
        char buf[256];
        snprintf(buf, sizeof(buf), "BOT status hull=%d fuel=%d ftl=%.1f/%.1f ready=%d jumping=%d enemy=%s enemyHull=%d choice=%d paused=%d crew=%d",
                 ship->ship.hullIntegrity.first, ship->fuel_count, ship->jump_timer.first, ship->jump_timer.second, (int)ship->lastJumpReady,
                 (int)ship->bJumping, en ? "yes" : "no", en ? en->ship.hullIntegrity.first : -1, (int)gui->choiceBox.bOpen,
                 (int)gui->bPaused, (int)ship->vCrewList.size());
        CoopGame::Log(buf);
        std::string sys;
        for (ShipSystem* y : ship->vSystemList)
            if (y) sys += std::to_string(y->iSystemType) + ":" + std::to_string(y->powerState.first) + "/" + std::to_string(y->powerState.second) +
                          "@" + std::to_string(y->roomId) + (y->bManned ? "M" : "") + " ";
        std::string crew;
        for (CrewMember* c : ship->vCrewList) if (c) crew += std::to_string(c->iRoomId) + (c->currentShipId == 0 ? "" : "x") + " ";
        CoopGame::Log("BOT systems " + sys + "| crew rooms " + crew);
        char b2[200];
        snprintf(b2, sizeof(b2), "BOT world time=%.2f hostile=%d autoPaused=%d menuPause=%d intruders=%d flying=%d charging=%d",
                 G_->GetCFPS()->RunningTime, (int)G_->GetWorld()->HostileEnvironment(), (int)gui->bAutoPaused,
                 (int)gui->menu_pause, ship->iIntruderCount, (int)ship->ShipFlying(), (int)ship->JumpCharging());
        CoopGame::Log(b2);
        std::string open;
        for (size_t i = 0; i < gui->focusWindows.size(); ++i)
        {
            FocusWindow* w = gui->focusWindows[i];
            if (!w || !w->bOpen) continue;
            const char* n = w == (FocusWindow*)gui->starMap ? "starMap" : w == (FocusWindow*)&gui->choiceBox ? "choiceBox" :
                            w == (FocusWindow*)&gui->menuBox ? "menu" : w == (FocusWindow*)&gui->gameOverScreen ? "gameOver" :
                            w == (FocusWindow*)&gui->optionsBox ? "options" : w == (FocusWindow*)&gui->inputBox ? "input" :
                            w == (FocusWindow*)&gui->shipScreens ? "shipScreens" : w == (FocusWindow*)&gui->storeScreens ? "store" :
                            w == (FocusWindow*)&gui->leaveCrewDialog ? "leaveCrew" : w == (FocusWindow*)&gui->writeErrorDialog ? "writeError" : "other";
            open += std::string(n) + "#" + std::to_string(i) + " ";
        }
        CoopGame::Log("BOT open windows: " + open);
    }

    // Both bots: the captain clears event dialogs so the run keeps going.
    if (captain && gui->choiceBox.bOpen && frame % 45 == 20)
    {
        // First available option (type 1 = requirement not met).
        for (size_t i = 0; i < gui->choiceBox.choices.size(); ++i)
            if (gui->choiceBox.choices[i].type != 1) { CoopGame::SubmitLocal(CoopCommands::Choice((int)i)); break; }
    }

    // Captain: jump to a random neighbouring beacon when the FTL drive is charged and nothing else is going on.
    if (captain && frame % 60 == 30 && !gui->choiceBox.bOpen && !G_->GetShipManager(1) && !ship->bJumping &&
        (ship->lastJumpReady || ship->jump_timer.first >= ship->jump_timer.second) && ship->fuel_count > 0)
    {
        StarMap& map = G_->GetWorld()->starMap;
        if (map.currentLoc && !map.currentLoc->connectedLocations.empty() && !map.bChoosingNewSector)
        {
            Location* target = map.currentLoc->connectedLocations[Rand((int)map.currentLoc->connectedLocations.size())];
            for (size_t i = 0; i < map.locations.size(); ++i)
                if (map.locations[i] == target) CoopGame::SubmitLocal(CoopCommands::Jump((int)i));
        }
    }

    if (cfg.bot != "chaos") return;

    // Fight: fire a random weapon at a random enemy room now and then.
    ShipManager* enemy = G_->GetShipManager(1);
    if (enemy && ship->weaponSystem && !ship->weaponSystem->weapons.empty() && !enemy->ship.vRoomList.empty() && Rand(20) == 0)
    {
        int w = Rand((int)ship->weaponSystem->weapons.size());
        Room* room = enemy->ship.vRoomList[Rand((int)enemy->ship.vRoomList.size())];
        std::vector<Pointf> pts;
        pts.push_back(Pointf(room->rect.x + room->rect.w / 2.f, room->rect.y + room->rect.h / 2.f));
        CoopGame::SubmitLocal(CoopCommands::Fire(w, 1, Rand(2) != 0, pts));
    }
    // The captain keeps crew 0 as pilot so the FTL drive charges (engines 1, piloting 6 are never powered down).
    if (captain && frame % 120 == 60 && !ship->vCrewList.empty())
    {
        ShipSystem* pilot = nullptr;
        for (ShipSystem* sys : ship->vSystemList) if (sys && sys->iSystemType == 6) pilot = sys;
        CrewMember* c0 = ship->vCrewList[0];
        if (pilot && c0 && c0->iRoomId != pilot->roomId && c0->currentShipId == 0)
            CoopGame::SubmitLocal(CoopCommands::Move(0, 0, pilot->roomId));
    }
    if (frame < 60 || Rand(12) != 0) return; // about 5 commands per second per copy

    switch (Rand(10))
    {
    case 0: case 1: case 2:
    {
        if (ship->vCrewList.empty() || ship->ship.vRoomList.empty()) break;
        int c = Rand((int)ship->vCrewList.size());
        if (c == 0) break; // the pilot stays put
        CrewMember* cm = ship->vCrewList[c];
        if (!cm || cm->currentShipId != 0) break;
        CoopGame::SubmitLocal(CoopCommands::Move(0, c, Rand((int)ship->ship.vRoomList.size())));
        break;
    }
    case 3: case 4:
    {
        if (ship->vSystemList.empty()) break;
        ShipSystem* s = ship->vSystemList[Rand((int)ship->vSystemList.size())];
        if (!s) break;
        int delta = Rand(2) ? 1 : -1;
        if (delta < 0 && (s->iSystemType == 1 || s->iSystemType == 6)) delta = 1; // keep engines and piloting up
        CoopGame::SubmitLocal(CoopCommands::Power(0, s->iSystemType, delta, false));
        break;
    }
    case 5: case 6:
    {
        int n = (int)(ship->ship.vDoorList.size() + ship->ship.vOuterAirlocks.size());
        if (n) CoopGame::SubmitLocal(CoopCommands::DoorCmd(0, Rand(n), Rand(2) != 0));
        break;
    }
    case 7: case 8:
    {
        if (ship->weaponSystem && !ship->weaponSystem->weapons.empty())
            CoopGame::SubmitLocal(CoopCommands::WeaponPower(Rand((int)ship->weaponSystem->weapons.size()), Rand(2) != 0));
        break;
    }
    case 9:
        if (Rand(4) == 0) CoopGame::SubmitLocal(CoopCommands::Pause(!gui->bPaused));
        break;
    }
}

} // namespace CoopBot
