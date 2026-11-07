#pragma once
#include "CoopGame.h"
#include <vector>
struct Pointf;
struct Store;

namespace CoopCommands {

enum Type : uint8_t
{
    C_CHOICE = 1,        // u8 choice index (event dialog). Captain only.
    C_PAUSE = 2,         // u8 paused
    C_MOVE = 3,          // u8 manager ship id, u16 crew index, i16 room id
    C_POWER = 4,         // u8 ship id, u8 system type, i8 delta (+n / -1), u8 force
    C_WEAPON_POWER = 5,  // u8 weapon index, u8 on
    C_DRONE_POWER = 6,   // u8 drone index, u8 on
    C_DOOR = 7,          // u8 ship id, u16 door index (inner doors, then airlocks), u8 open
    C_ASSIGN_CREW = 8,   // u16 crew index, i8 slot (-1 = nobody). Captain only.
    C_SET_MODE = 9,      // u8 mode. Captain only.
    C_FIRE = 10,         // u8 weapon index, u8 target ship id, u8 autofire, u8 n, n x (f32 x, f32 y)
    C_WEAPON_CLEAR = 11, // u8 weapon index
    C_AUTOFIRE = 12,     // u8 on
    C_JUMP = 13,         // u16 location index in StarMap::locations. Captain only.
    C_CLOAK = 14,        // u8 on
    C_BATTERY = 15,      // u8 on, u8 flag
    C_HACK_PULSE = 16,   // -
    C_HACK_TARGET = 17,  // u8 target ship id, u8 system type
    C_MIND = 18,         // u8 ship id, i16 room, u8 n, n x u16 crew index (in that ship's crew list)
    C_TELEPORT = 19,     // i16 target room, i32 command (CombatControl teleport command)
    C_SECTOR = 20,       // u8 next sector index (StarMap::finalSectorChoice). Captain only.
    C_CREW_POWER = 21,   // u8 ship id (whose crew list), u16 crew index, u8 power index (Hyperspace crew ability)
    C_BUY = 22,          // u8 store (0 = this beacon's, 1 = test store), u8 list (0 store boxes, 1 item boxes), u8 index. Captain only.
    C_UPGRADE = 23,      // u8 system type (255 = reactor), u8 levels, u16 scrap cost. Captain only.
    C_TEST_STORE = 102,      // - (tests only: FTL_COOP_TEST=1): a store for this sector, like Hyperspace's STORE command
    C_TEST_ADD_CREW = 101,   // str race (tests only: FTL_COOP_TEST=1)
    C_TEST_ADD_SYSTEM = 100, // u8 system type (tests only: FTL_COOP_TEST=1)
};

// Is an input event (click, key) being handled right now? Only then are actions taken over.
bool InInput();

bool Allowed(int slot, const coop::Bytes& cmd, CoopGame::Mode mode, std::string* reason);
void Apply(int slot, const coop::Bytes& cmd);
void OnRunStart(int players);
void ShowMessage(const std::string& text);
std::string Describe(const coop::Bytes& cmd);

// Builders (used by the hooks and the bot)
coop::Bytes Choice(int index);
coop::Bytes Pause(bool paused);
coop::Bytes Move(int managerShipId, int crewIndex, int roomId);
coop::Bytes Power(int shipId, int systemType, int delta, bool force);
coop::Bytes WeaponPower(int index, bool on);
coop::Bytes DronePower(int index, bool on);
coop::Bytes DoorCmd(int shipId, int doorIndex, bool open);
coop::Bytes AssignCrew(int crewIndex, int slot);
coop::Bytes Fire(int weaponIndex, int targetShip, bool autoFire, const std::vector<Pointf>& points);
coop::Bytes WeaponClear(int weaponIndex);
coop::Bytes AutoFire(bool on);
coop::Bytes Jump(int locationIndex);
coop::Bytes SectorChoice(int sectorIndex);
Store* TestStore();   // the store made by C_TEST_STORE (tests)
int PendingUpgradeScrap();   // host: scrap held by upgrade-screen clicks not accepted yet
coop::Bytes SetMode(CoopGame::Mode m);

} // namespace CoopCommands

namespace CoopBot {
void BeforeFrame(uint32_t frame);
}

namespace CoopGame {
void RequestManualStart();
Mode CurrentMode();
void SetCurrentMode(Mode m);
}

namespace CoopOverlay {
void Post(const std::string& text);
}
