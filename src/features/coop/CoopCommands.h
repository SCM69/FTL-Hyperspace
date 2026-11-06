#pragma once
#include "CoopGame.h"

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

} // namespace CoopCommands

namespace CoopBot {
void BeforeFrame(uint32_t frame);
}

namespace CoopGame {
void RequestManualStart();
}
