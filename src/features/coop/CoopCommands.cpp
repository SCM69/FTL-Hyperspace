// FTL co-op: player commands. Local input is turned into commands; commands are applied on every copy.
#include "CoopCommands.h"
#include "Global.h"
#include <map>

using coop::Bytes;
using coop::Reader;
using coop::Writer;

namespace CoopCommands {

static int g_inputDepth = 0;
bool InInput() { return g_inputDepth > 0; }
struct InputScope { InputScope() { ++g_inputDepth; } ~InputScope() { --g_inputDepth; } };

// Take an action over when it comes from local input during lockstep (and is not itself being applied).
static bool ShouldTakeOver() { return CoopGame::InLockstep() && InInput() && !CoopGame::Applying(); }

// ---------------------------------------------------------------- builders

Bytes Choice(int index) { Writer w; w.u8(C_CHOICE); w.u8((uint8_t)index); return w.b; }
Bytes Pause(bool paused) { Writer w; w.u8(C_PAUSE); w.u8(paused ? 1 : 0); return w.b; }
Bytes Move(int mgr, int crew, int room) { Writer w; w.u8(C_MOVE); w.u8((uint8_t)mgr); w.u16((uint16_t)crew); w.u16((uint16_t)(int16_t)room); return w.b; }
Bytes Power(int ship, int sys, int delta, bool force) { Writer w; w.u8(C_POWER); w.u8((uint8_t)ship); w.u8((uint8_t)sys); w.u8((uint8_t)(int8_t)delta); w.u8(force ? 1 : 0); return w.b; }
Bytes WeaponPower(int index, bool on) { Writer w; w.u8(C_WEAPON_POWER); w.u8((uint8_t)index); w.u8(on ? 1 : 0); return w.b; }
Bytes DronePower(int index, bool on) { Writer w; w.u8(C_DRONE_POWER); w.u8((uint8_t)index); w.u8(on ? 1 : 0); return w.b; }
Bytes DoorCmd(int ship, int door, bool open) { Writer w; w.u8(C_DOOR); w.u8((uint8_t)ship); w.u16((uint16_t)door); w.u8(open ? 1 : 0); return w.b; }
Bytes AssignCrew(int crew, int slot) { Writer w; w.u8(C_ASSIGN_CREW); w.u16((uint16_t)crew); w.u8((uint8_t)(int8_t)slot); return w.b; }

// ---------------------------------------------------------------- lookups

static ShipManager* Ship(int id) { return G_->GetShipManager(id); }

static int CrewIndex(ShipManager* s, CrewMember* c)
{
    if (!s || !c) return -1;
    for (size_t i = 0; i < s->vCrewList.size(); ++i) if (s->vCrewList[i] == c) return (int)i;
    return -1;
}

static CrewMember* CrewAt(ShipManager* s, int i)
{
    if (!s || i < 0 || i >= (int)s->vCrewList.size()) return nullptr;
    return s->vCrewList[i];
}

static ShipSystem* SystemOf(ShipManager* s, int type)
{
    if (!s) return nullptr;
    for (ShipSystem* sys : s->vSystemList) if (sys && sys->iSystemType == type) return sys;
    return nullptr;
}

static Door* DoorAt(ShipManager* s, int i)
{
    if (!s || i < 0) return nullptr;
    if (i < (int)s->ship.vDoorList.size()) return s->ship.vDoorList[i];
    i -= (int)s->ship.vDoorList.size();
    if (i < (int)s->ship.vOuterAirlocks.size()) return s->ship.vOuterAirlocks[i];
    return nullptr;
}

static int DoorIndex(ShipManager* s, Door* d)
{
    if (!s || !d) return -1;
    for (size_t i = 0; i < s->ship.vDoorList.size(); ++i) if (s->ship.vDoorList[i] == d) return (int)i;
    for (size_t i = 0; i < s->ship.vOuterAirlocks.size(); ++i) if (s->ship.vOuterAirlocks[i] == d) return (int)(s->ship.vDoorList.size() + i);
    return -1;
}

static int ShipIdOf(ShipManager* s)
{
    for (int id = 0; id < 2; ++id) if (Ship(id) == s) return id;
    return -1;
}

// ---------------------------------------------------------------- ownership (host only decides)

static std::map<CrewMember*, int> g_owner; // own-crew mode; filled at run start, kept up to date on the host
static int g_players = 1;

void OnRunStart(int players)
{
    g_owner.clear();
    g_players = players < 1 ? 1 : players;
}

static void EnsureOwners()
{
    // New player crew (start of run, hired, cloned) get an owner in round-robin order of the crew list.
    ShipManager* s = Ship(0);
    if (!s) return;
    int i = 0;
    for (CrewMember* c : s->vCrewList)
    {
        if (c && !g_owner.count(c) && c->iShipId == 0) g_owner[c] = (int)(i % g_players);
        ++i;
    }
}

} // namespace CoopCommands

namespace CoopGame {
int CrewOwner(CrewMember* crew)
{
    CoopCommands::EnsureOwners();
    auto it = CoopCommands::g_owner.find(crew);
    return it == CoopCommands::g_owner.end() ? -1 : it->second;
}
void SetCrewOwner(int crewIndex, int slot)
{
    CrewMember* c = CoopCommands::CrewAt(CoopCommands::Ship(0), crewIndex);
    if (c) CoopCommands::g_owner[c] = slot;
}
} // namespace CoopGame

namespace CoopCommands {

// A slot staffs a room when one of its crew stands in it (on the player ship).
static bool SlotStaffsRoom(int slot, int roomId)
{
    ShipManager* s = Ship(0);
    if (!s) return false;
    for (CrewMember* c : s->vCrewList)
        if (c && c->iRoomId == roomId && c->currentShipId == 0 && CoopGame::CrewOwner(c) == slot) return true;
    return false;
}

static bool AnyPlayerStaffsRoom(int roomId)
{
    ShipManager* s = Ship(0);
    if (!s) return false;
    for (CrewMember* c : s->vCrewList)
        if (c && c->iRoomId == roomId && c->currentShipId == 0 && CoopGame::CrewOwner(c) >= 0) return true;
    return false;
}

// Own-crew rule for a system: whoever staffs its room; an unstaffed system belongs to the captain.
static bool MaySystem(int slot, ShipSystem* sys, std::string* reason)
{
    if (!sys) { *reason = "no such system"; return false; }
    if (SlotStaffsRoom(slot, sys->roomId)) return true;
    if (slot == 0 && !AnyPlayerStaffsRoom(sys->roomId)) return true;
    *reason = "that system is staffed by another player (or nobody: captain's)";
    return false;
}

bool Allowed(int slot, const Bytes& cmd, CoopGame::Mode mode, std::string* reason)
{
    Reader r(cmd);
    uint8_t t = r.u8();
    switch (t)
    {
    case C_CHOICE: case C_ASSIGN_CREW: case C_SET_MODE:
        if (slot != 0) { *reason = "only the captain (host) can do that"; return false; }
        return true;
    case C_PAUSE:
        return true;
    default: break;
    }
    if (mode == CoopGame::Mode::FreeForAll) return true;

    switch (t)
    {
    case C_MOVE:
    {
        int mgr = r.u8(); int idx = r.u16();
        CrewMember* c = CrewAt(Ship(mgr), idx);
        if (!c) { *reason = "no such crew member"; return false; }
        int owner = CoopGame::CrewOwner(c);
        if (owner == slot || owner < 0) return true;
        *reason = "that crew member belongs to another player";
        return false;
    }
    case C_POWER:
    {
        int ship = r.u8(); int type = r.u8();
        if (ship != 0) { *reason = "not our ship"; return false; }
        return MaySystem(slot, SystemOf(Ship(0), type), reason);
    }
    case C_WEAPON_POWER:
        return MaySystem(slot, SystemOf(Ship(0), 3 /* weapons */), reason);
    case C_DRONE_POWER:
        return MaySystem(slot, SystemOf(Ship(0), 4 /* drones */), reason);
    case C_DOOR:
        return true; // doors stay shared in both modes
    default:
        *reason = "unknown command";
        return false;
    }
}

std::string Describe(const Bytes& cmd)
{
    Reader r(cmd);
    char buf[128];
    switch (r.u8())
    {
    case C_CHOICE: snprintf(buf, sizeof(buf), "choice %d", r.u8()); break;
    case C_PAUSE: snprintf(buf, sizeof(buf), "pause %d", r.u8()); break;
    case C_MOVE: { int m = r.u8(); int c = r.u16(); int room = (int16_t)r.u16(); snprintf(buf, sizeof(buf), "move ship%d crew%d room%d", m, c, room); break; }
    case C_POWER: { int s = r.u8(); int t = r.u8(); int d = (int8_t)r.u8(); snprintf(buf, sizeof(buf), "power ship%d sys%d %+d", s, t, d); break; }
    case C_WEAPON_POWER: { int i = r.u8(); snprintf(buf, sizeof(buf), "weapon %d power %d", i, r.u8()); break; }
    case C_DRONE_POWER: { int i = r.u8(); snprintf(buf, sizeof(buf), "drone %d power %d", i, r.u8()); break; }
    case C_DOOR: { int s = r.u8(); int d = r.u16(); snprintf(buf, sizeof(buf), "door ship%d %d open %d", s, d, r.u8()); break; }
    case C_ASSIGN_CREW: { int c = r.u16(); snprintf(buf, sizeof(buf), "assign crew%d slot%d", c, (int8_t)r.u8()); break; }
    case C_SET_MODE: snprintf(buf, sizeof(buf), "mode %d", r.u8()); break;
    default: snprintf(buf, sizeof(buf), "unknown"); break;
    }
    return buf;
}

void Apply(int slot, const Bytes& cmd)
{
    Reader r(cmd);
    uint8_t t = r.u8();
    CommandGui* gui = G_->GetWorld()->commandGui;
    CoopGame::Log("APPLY slot" + std::to_string(slot) + " " + Describe(cmd));
    switch (t)
    {
    case C_CHOICE:
    {
        int idx = r.u8();
        if (gui && gui->choiceBox.bOpen) gui->choiceBox.KeyDown((SDLKey)(SDLK_1 + idx));
        break;
    }
    case C_PAUSE:
    {
        bool p = r.u8() != 0;
        if (gui) gui->SetPaused(p, false);
        break;
    }
    case C_MOVE:
    {
        int mgr = r.u8(); int idx = r.u16(); int room = (int16_t)r.u16();
        ShipManager* s = Ship(mgr);
        CrewMember* c = CrewAt(s, idx);
        if (s && c) s->CommandCrewMoveRoom(c, room);
        break;
    }
    case C_POWER:
    {
        int ship = r.u8(); int type = r.u8(); int delta = (int8_t)r.u8(); bool force = r.u8() != 0;
        ShipSystem* sys = SystemOf(Ship(ship), type);
        if (!sys) break;
        if (delta > 0) sys->IncreasePower(delta, force);
        else sys->DecreasePower(force);
        break;
    }
    case C_WEAPON_POWER:
    {
        int i = r.u8(); bool on = r.u8() != 0;
        ShipManager* s = Ship(0);
        if (!s || !s->weaponSystem || i >= (int)s->weaponSystem->weapons.size()) break;
        ProjectileFactory* w = s->weaponSystem->weapons[i];
        if (on) s->PowerWeapon(w, true, false); else s->DePowerWeapon(w, true);
        break;
    }
    case C_DRONE_POWER:
    {
        int i = r.u8(); bool on = r.u8() != 0;
        ShipManager* s = Ship(0);
        if (!s || !s->droneSystem || i >= (int)s->droneSystem->drones.size()) break;
        Drone* d = s->droneSystem->drones[i];
        if (on) s->PowerDrone(d, 1, true, false); else s->DePowerDrone(d, true);
        break;
    }
    case C_DOOR:
    {
        int ship = r.u8(); int idx = r.u16(); bool open = r.u8() != 0;
        Door* d = DoorAt(Ship(ship), idx);
        if (!d) break;
        if (open) d->Open(); else d->Close();
        break;
    }
    case C_ASSIGN_CREW:
    {
        int idx = r.u16(); int s = (int8_t)r.u8();
        CoopGame::SetCrewOwner(idx, s);
        break;
    }
    default: break;
    }
}

void ShowMessage(const std::string& text)
{
    CoopGame::Log("MSG " + text);
    CommandGui* gui = G_ && G_->GetCApp() && G_->GetCApp()->world ? G_->GetWorld()->commandGui : nullptr;
    if (gui) gui->crewControl.StartCrewMessage(text);
}

} // namespace CoopCommands

// ---------------------------------------------------------------- hooks: input scope

using namespace CoopCommands;

HOOK_METHOD_PRIORITY(CApp, OnLButtonDown, 9000, (int x, int y) -> void) { LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLButtonDown -> Begin (CoopCommands.cpp)\n") InputScope s; super(x, y); }
HOOK_METHOD_PRIORITY(CApp, OnLButtonUp, 9000, (int x, int y) -> void) { LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLButtonUp -> Begin (CoopCommands.cpp)\n") InputScope s; super(x, y); }
HOOK_METHOD_PRIORITY(CApp, OnRButtonDown, 9000, (int x, int y) -> void) { LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRButtonDown -> Begin (CoopCommands.cpp)\n") InputScope s; super(x, y); }
HOOK_METHOD_PRIORITY(CApp, OnRButtonUp, 9000, (int x, int y) -> void) { LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRButtonUp -> Begin (CoopCommands.cpp)\n") InputScope s; super(x, y); }
HOOK_METHOD_PRIORITY(CApp, OnMButtonDown, 9000, (int x, int y) -> void) { LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnMButtonDown -> Begin (CoopCommands.cpp)\n") InputScope s; super(x, y); }
HOOK_METHOD_PRIORITY(CApp, OnKeyDown, 9000, (SDLKey k) -> void) { LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnKeyDown -> Begin (CoopCommands.cpp)\n") InputScope s; super(k); }
HOOK_METHOD_PRIORITY(CApp, OnKeyUp, 9000, (SDLKey k) -> void) { LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnKeyUp -> Begin (CoopCommands.cpp)\n") InputScope s; super(k); }

// ---------------------------------------------------------------- hooks: actions taken over

HOOK_METHOD_PRIORITY(CommandGui, SpaceBar, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::SpaceBar -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver()) { CoopGame::SubmitLocal(Pause(!this->bPaused)); return; }
    super();
}

HOOK_METHOD_PRIORITY(ChoiceBox, MouseClick, 9000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ChoiceBox::MouseClick -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver())
    {
        int c = this->GetPotentialChoice();
        if (c >= 0) CoopGame::SubmitLocal(Choice(c));
        return;
    }
    super(mX, mY);
}

HOOK_METHOD_PRIORITY(ChoiceBox, KeyDown, 9000, (SDLKey key) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ChoiceBox::KeyDown -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver())
    {
        if (key >= SDLK_1 && key <= SDLK_9) CoopGame::SubmitLocal(Choice(key - SDLK_1));
        return;
    }
    super(key);
}

HOOK_METHOD_PRIORITY(ShipManager, CommandCrewMoveRoom, 9000, (CrewMember* crew, int roomId) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::CommandCrewMoveRoom -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver())
    {
        int mgr = ShipIdOf(this), idx = CrewIndex(this, crew);
        if (mgr >= 0 && idx >= 0) CoopGame::SubmitLocal(Move(mgr, idx, roomId));
        return true;
    }
    return super(crew, roomId);
}

HOOK_METHOD_PRIORITY(ShipSystem, IncreasePower, 9000, (int amount, bool force) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::IncreasePower -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver())
    {
        CoopGame::SubmitLocal(Power(this->_shipObj.iShipId, this->iSystemType, amount, force));
        return true;
    }
    return super(amount, force);
}

HOOK_METHOD_PRIORITY(ShipSystem, DecreasePower, 9000, (bool force) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::DecreasePower -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver())
    {
        CoopGame::SubmitLocal(Power(this->_shipObj.iShipId, this->iSystemType, -1, force));
        return true;
    }
    return super(force);
}

HOOK_METHOD_PRIORITY(ShipManager, PowerWeapon, 9000, (ProjectileFactory* weapon, bool userDriven, bool force) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::PowerWeapon -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver() && this->weaponSystem)
    {
        auto& v = this->weaponSystem->weapons;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == weapon) CoopGame::SubmitLocal(WeaponPower((int)i, true));
        return true;
    }
    return super(weapon, userDriven, force);
}

HOOK_METHOD_PRIORITY(ShipManager, DePowerWeapon, 9000, (ProjectileFactory* weapon, bool userDriven) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DePowerWeapon -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver() && this->weaponSystem)
    {
        auto& v = this->weaponSystem->weapons;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == weapon) CoopGame::SubmitLocal(WeaponPower((int)i, false));
        return true;
    }
    return super(weapon, userDriven);
}

HOOK_METHOD_PRIORITY(ShipManager, PowerDrone, 9000, (Drone* drone, int roomId, bool userDriven, bool force) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::PowerDrone -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver() && this->droneSystem)
    {
        auto& v = this->droneSystem->drones;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == drone) CoopGame::SubmitLocal(DronePower((int)i, true));
        return true;
    }
    return super(drone, roomId, userDriven, force);
}

HOOK_METHOD_PRIORITY(ShipManager, DePowerDrone, 9000, (Drone* drone, bool userDriven) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DePowerDrone -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver() && this->droneSystem)
    {
        auto& v = this->droneSystem->drones;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == drone) CoopGame::SubmitLocal(DronePower((int)i, false));
        return true;
    }
    return super(drone, userDriven);
}

// Door clicks: CrewControl::MouseMove picks the door under the cursor, LButton toggles it inline.
HOOK_METHOD_PRIORITY(CrewControl, LButton, 9000, (int mX, int mY, int wX, int wY, bool shiftHeld) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewControl::LButton -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver() && this->selectedDoor && this->shipManager)
    {
        ShipManager* s = this->shipManager;
        int idx = DoorIndex(s, this->selectedDoor);
        if (idx >= 0)
        {
            CoopGame::SubmitLocal(DoorCmd(ShipIdOf(s), idx, !this->selectedDoor->bOpen));
            return;
        }
    }
    super(mX, mY, wX, wY, shiftHeld);
}
