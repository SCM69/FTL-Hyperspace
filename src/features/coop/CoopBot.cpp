// FTL co-op: in-game test bots (FTL_COOP_BOT). They submit commands like a player would.
//  probe : captain answers event dialogs with option 1, nothing else.
//  chaos : every copy issues random crew moves, power changes, door toggles, weapon power and rare pauses.
// The bot draws from its own generator so it never consumes the game's random numbers.
#include "CoopCommands.h"
#include "Global.h"
#include <random>

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

    // Both bots: the captain clears event dialogs so the run keeps going.
    if (captain && gui->choiceBox.bOpen && frame % 45 == 20)
        CoopGame::SubmitLocal(CoopCommands::Choice(0));

    if (cfg.bot != "chaos") return;
    if (frame < 60 || Rand(12) != 0) return; // about 5 commands per second per copy

    switch (Rand(10))
    {
    case 0: case 1: case 2:
    {
        if (ship->vCrewList.empty() || ship->ship.vRoomList.empty()) break;
        int c = Rand((int)ship->vCrewList.size());
        CrewMember* cm = ship->vCrewList[c];
        if (!cm || cm->currentShipId != 0) break;
        CoopGame::SubmitLocal(CoopCommands::Move(0, c, Rand((int)ship->ship.vRoomList.size())));
        break;
    }
    case 3: case 4:
    {
        if (ship->vSystemList.empty()) break;
        ShipSystem* s = ship->vSystemList[Rand((int)ship->vSystemList.size())];
        if (s) CoopGame::SubmitLocal(CoopCommands::Power(0, s->iSystemType, Rand(2) ? 1 : -1, false));
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
