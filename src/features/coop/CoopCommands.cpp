// FTL co-op: player commands. Local input is turned into commands; commands are applied on every copy.
#include "CoopCommands.h"
#include "Global.h"
#include "CrewMember_Extend.h"
#include "CrewBox_Extend.h"
#include <map>
#include <cstring>

using coop::Bytes;
using coop::Reader;
using coop::Writer;

namespace CoopCaptain { void CommandApplied(); }
namespace CoopCommands {

static int g_inputDepth = 0;
bool g_applyingTeleport = false;
static bool g_inGuiLoop = false;
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
static void F32(Writer& w, float f) { uint32_t u; memcpy(&u, &f, 4); w.u32(u); }
static float F32(Reader& r) { uint32_t u = r.u32(); float f; memcpy(&f, &u, 4); return f; }
Bytes Fire(int weapon, int target, bool autoFire, const std::vector<Pointf>& pts)
{
    Writer w; w.u8(C_FIRE); w.u8((uint8_t)weapon); w.u8((uint8_t)target); w.u8(autoFire ? 1 : 0); w.u8((uint8_t)pts.size());
    for (const Pointf& p : pts) { F32(w, p.x); F32(w, p.y); }
    return w.b;
}
Bytes WeaponClear(int weapon) { Writer w; w.u8(C_WEAPON_CLEAR); w.u8((uint8_t)weapon); return w.b; }
Bytes SetMode(CoopGame::Mode m) { Writer w; w.u8(C_SET_MODE); w.u8((uint8_t)m); return w.b; }
Bytes Jump(int loc) { Writer w; w.u8(C_JUMP); w.u16((uint16_t)loc); return w.b; }
Bytes SectorChoice(int s) { Writer w; w.u8(C_SECTOR); w.u8((uint8_t)s); return w.b; }
Bytes AutoFire(bool on) { Writer w; w.u8(C_AUTOFIRE); w.u8(on ? 1 : 0); return w.b; }
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

// Stores: 0 = the store of this beacon's event, 1 = the test store (C_TEST_STORE). A box is named by its list and
// position; every copy built the same store at the same frame.
static Store* g_testStore = nullptr;
}  // namespace CoopCommands
Store* CoopCommands::TestStore() { return CoopCommands::g_testStore; }
namespace CoopCommands {
static Store* StoreOf(int st)
{
    if (st == 1) return g_testStore;
    WorldManager* w = G_->GetWorld();
    return w && w->baseLocationEvent ? w->baseLocationEvent->pStore : nullptr;
}
static StoreBox* StoreBoxAt(int st, int list, int i)
{
    Store* s = StoreOf(st);
    if (!s) return nullptr;
    auto& v = list == 0 ? s->vStoreBoxes : s->vItemBoxes;
    return i >= 0 && i < (int)v.size() ? v[i] : nullptr;
}
static bool FindStoreBox(StoreBox* box, int* st, int* list, int* idx)
{
    for (int s = 0; s <= 1; ++s)
    {
        Store* store = StoreOf(s);
        if (!store) continue;
        for (int l = 0; l <= 1; ++l)
        {
            auto& v = l == 0 ? store->vStoreBoxes : store->vItemBoxes;
            for (size_t i = 0; i < v.size(); ++i) if (v[i] == box) { *st = s; *list = l; *idx = (int)i; return true; }
        }
    }
    return false;
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

namespace CoopCaptain { void CommandApplied(); }
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
    case C_TEST_ADD_SYSTEM: case C_TEST_ADD_CREW: case C_TEST_STORE:
        if (!getenv("FTL_COOP_TEST")) { *reason = "test command"; return false; }
        return slot == 0;
    case C_CHOICE: case C_ASSIGN_CREW: case C_SET_MODE: case C_JUMP: case C_SECTOR: case C_BUY: case C_UPGRADE:
        if (slot != 0) { *reason = "only the captain (host) can do that"; return false; }
        return true;
    case C_PAUSE:
        return true;
    default: break;
    }
    if (mode == CoopGame::Mode::FreeForAll) return true;

    switch (t)
    {
    case C_MOVE: case C_CREW_POWER:
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
    case C_FIRE: case C_WEAPON_CLEAR: case C_AUTOFIRE:
        return MaySystem(slot, SystemOf(Ship(0), 3 /* weapons */), reason);
    case C_CLOAK: return MaySystem(slot, SystemOf(Ship(0), 10 /* cloaking */), reason);
    case C_BATTERY: return MaySystem(slot, SystemOf(Ship(0), 12 /* battery */), reason);
    case C_HACK_PULSE: case C_HACK_TARGET: return MaySystem(slot, SystemOf(Ship(0), 15 /* hacking */), reason);
    case C_MIND: return MaySystem(slot, SystemOf(Ship(0), 14 /* mind */), reason);
    case C_TELEPORT: return MaySystem(slot, SystemOf(Ship(0), 9 /* teleporter */), reason);
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
    case C_FIRE: { int i = r.u8(); int t = r.u8(); int a = r.u8(); int n = r.u8(); float x = n ? F32(r) : 0, y = n ? F32(r) : 0;
        snprintf(buf, sizeof(buf), "fire weapon%d at ship%d auto%d n%d (%.1f,%.1f)", i, t, a, n, x, y); break; }
    case C_WEAPON_CLEAR: snprintf(buf, sizeof(buf), "clear weapon%d", r.u8()); break;
    case C_AUTOFIRE: snprintf(buf, sizeof(buf), "autofire %d", r.u8()); break;
    case C_JUMP: snprintf(buf, sizeof(buf), "jump to location %d", r.u16()); break;
    case C_SECTOR: snprintf(buf, sizeof(buf), "next sector %d", r.u8()); break;
    case C_CREW_POWER: { int sh = r.u8(); int ci = r.u16(); snprintf(buf, sizeof(buf), "crew power ship%d crew%d power%d", sh, ci, r.u8()); break; }
    case C_TEST_ADD_CREW: snprintf(buf, sizeof(buf), "test add crew %s", r.str().c_str()); break;
    case C_BUY: { int st = r.u8(); int li = r.u8(); snprintf(buf, sizeof(buf), "buy store%d list%d box%d", st, li, r.u8()); break; }
    case C_UPGRADE: { int sy = r.u8(); int lv = r.u8(); snprintf(buf, sizeof(buf), "upgrade sys%d +%d cost %d", sy, lv, r.u16()); break; }
    case C_TEST_STORE: snprintf(buf, sizeof(buf), "test store"); break;
    case C_CLOAK: snprintf(buf, sizeof(buf), "cloak %d", r.u8()); break;
    case C_BATTERY: snprintf(buf, sizeof(buf), "battery %d", r.u8()); break;
    case C_HACK_PULSE: snprintf(buf, sizeof(buf), "hack pulse"); break;
    case C_HACK_TARGET: { int sh = r.u8(); snprintf(buf, sizeof(buf), "hack ship%d sys%d", sh, r.u8()); break; }
    case C_MIND: { int sh = r.u8(); int room = (int16_t)r.u16(); snprintf(buf, sizeof(buf), "mind control ship%d room%d n%d", sh, room, r.u8()); break; }
    case C_TEST_ADD_SYSTEM: snprintf(buf, sizeof(buf), "test add system %d", r.u8()); break;
    case C_TELEPORT: { int room = (int16_t)r.u16(); snprintf(buf, sizeof(buf), "teleport room%d command%d", room, r.i32()); break; }
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
        // Same effect as ChoiceBox::KeyDown, without depending on the player's dialog-hotkey setting:
        // pick an available choice; CommandGui::OnLoop then acts on selectedChoice.
        if (gui && gui->choiceBox.bOpen && idx < (int)gui->choiceBox.choices.size() && gui->choiceBox.choices[idx].type != 1)
            gui->choiceBox.selectedChoice = idx;
        CoopGame::Log("choice: open=" + std::to_string((int)(gui && gui->choiceBox.bOpen)) + " n=" +
                      std::to_string(gui ? (int)gui->choiceBox.choices.size() : -1) + " selected=" +
                      std::to_string(gui ? gui->choiceBox.selectedChoice : -9));
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
    case C_FIRE:
    {
        int i = r.u8(); int target = r.u8(); bool autoFire = r.u8() != 0; int n = r.u8();
        std::vector<Pointf> pts;
        for (int k = 0; k < n; ++k) { float x = F32(r); float y = F32(r); pts.push_back(Pointf(x, y)); }
        ShipManager* s = Ship(0);
        if (!s || !s->weaponSystem || i >= (int)s->weaponSystem->weapons.size()) break;
        ProjectileFactory* w = s->weaponSystem->weapons[i];
        w->SetAutoFire(autoFire);
        w->Fire(pts, target);
        break;
    }
    case C_WEAPON_CLEAR:
    {
        int i = r.u8();
        ShipManager* s = Ship(0);
        if (s && s->weaponSystem && i < (int)s->weaponSystem->weapons.size()) s->weaponSystem->weapons[i]->ClearAiming();
        break;
    }
    case C_AUTOFIRE:
    {
        bool on = r.u8() != 0;
        if (gui) { CoopGame::Log("autofire apply"); gui->combatControl.weapControl.SetAutofiring(on, false); }
        break;
    }
    case C_JUMP:
    {
        // What StarMap::MouseClick does for a reachable beacon: StarMap::GetNewLocation then starts the jump.
        int idx = r.u16();
        StarMap& map = G_->GetWorld()->starMap;
        if (idx >= (int)map.locations.size() || !map.currentLoc) break;
        Location* loc = map.locations[idx];
        bool connected = false;
        for (Location* c : map.currentLoc->connectedLocations) if (c == loc) connected = true;
        if (!connected) { CoopGame::Log("jump ignored: not connected"); break; }
        // A player always closes the store and ship screens before opening the map; jumping with the store open
        // crashed both copies (2026-11-06). Close them on every copy first.
        if (gui && gui->storeScreens.bOpen) gui->storeScreens.Close();
        if (gui && gui->shipScreens.bOpen) gui->shipScreens.Close();
        map.potentialLoc = loc;
        map.readyToTravel = true;
        break;
    }
    case C_SECTOR:
    {
        // What Hyperspace's StarMap::OnLoop does with finalSectorChoice (CustomMap.cpp), on every copy at this frame.
        int choice = r.u8();
        StarMap& map = G_->GetWorld()->starMap;
        if (!map.NeighboringSector(choice)) { CoopGame::Log("sector choice ignored: not a neighbouring sector"); break; }
        if (gui && gui->storeScreens.bOpen) gui->storeScreens.Close();
        if (gui && gui->shipScreens.bOpen) gui->shipScreens.Close();
        map.SelectNewSector(choice);
        map.AdvanceWorldLevel();
        map.readyToTravel = true;
        map.finalSectorChoice = -1;
        map.potentialLoc = &map.dummyNewSector;
        if (map.bOpen) map.Close();
        CoopGame::Logf("sector chosen: world level now %d, readyToTravel %d", map.worldLevel, (int)map.readyToTravel);
        break;
    }
    case C_CREW_POWER:
    {
        // What CrewBox::MouseClick / the ability hotkey do (CustomCrew.cpp), with the same checks.
        int ship = r.u8(); int idx = r.u16(); int p = r.u8();
        CrewMember* c = CrewAt(Ship(ship), idx);
        if (!c) break;
        CrewMember_Extend* ex = CM_EX(c);
        if (p >= (int)ex->crewPowers.size()) break;
        ActivatedPower* power = ex->crewPowers[p];
        bool ready = power->enabled && power->PowerReady() == PowerReadyState::POWER_READY && c->GetPowerOwner() == 0;
        if (ready) power->PreparePower();
        CoopGame::Logf("crew power: %s power %d %s", c->species.c_str(), p, ready ? "activated" : "not ready (ignored)");
        break;
    }
    case C_BUY:
    {
        int st = r.u8(); int li = r.u8(); int i = r.u8();
        StoreBox* box = StoreBoxAt(st, li, i);
        ShipManager* sm = Ship(0);
        if (!box || !sm) { CoopGame::Log("buy ignored: no such store box"); break; }
        // StoreBox::MouseClick buys only what the player can afford and hold; do the same check here.
        bool ok = box->count > 0 && sm->currentScrap >= box->desc.cost && box->CanHold();
        int before = sm->currentScrap;
        if (ok) box->Purchase();
        CoopCaptain::CommandApplied();
        CoopGame::Logf("buy: %s (cost %d) %s, scrap %d -> %d", box->pBlueprint ? box->pBlueprint->name.c_str() : "item", box->desc.cost,
                       ok ? "bought" : "refused", before, sm->currentScrap);
        break;
    }
    case C_UPGRADE:
    {
        int type = r.u8(); int levels = r.u8(); int cost = r.u16();
        ShipManager* sm = Ship(0);
        if (!sm || sm->currentScrap < cost || levels <= 0) { CoopGame::Log("upgrade ignored (scrap)"); break; }
        if (type == 255)
        {
            // The reactor: the upgrade screen's own button applies it (ReactorButton::Accept).
            ReactorButton& rb = gui->upgradeScreen.reactorButton;
            if (!rb.ship) { CoopGame::Log("upgrade ignored: reactor button not set up"); break; }
            sm->currentScrap -= cost;
            int saved = rb.tempUpgrade; rb.tempUpgrade = levels; rb.Accept(); rb.tempUpgrade = saved;
        }
        else
        {
            ShipSystem* sys = SystemOf(sm, type);
            if (!sys) { CoopGame::Log("upgrade ignored: no such system"); break; }
            sm->currentScrap -= cost;
            sys->UpgradeSystem(levels);
        }
        CoopCaptain::CommandApplied();
        CoopGame::Logf("upgrade: sys %d +%d for %d scrap, scrap now %d", type, levels, cost, sm->currentScrap);
        break;
    }
    case C_TEST_STORE:
    {
        if (gui) g_testStore = gui->CreateNewStore(G_->GetWorld()->starMap.worldLevel);
        if (Ship(0)) Ship(0)->currentScrap += 500;   // something to spend
        CoopGame::Logf("test store created (%d store boxes, %d item boxes)", g_testStore ? (int)g_testStore->vStoreBoxes.size() : -1,
                       g_testStore ? (int)g_testStore->vItemBoxes.size() : -1);
        break;
    }
    case C_TEST_ADD_CREW:
    {
        std::string race = r.str();
        ShipManager* sm = Ship(0);
        CrewMember* c = sm ? sm->AddCrewMemberFromString("Test", race, false, 0, true, true) : nullptr;
        if (c) CoopGame::Logf("test add crew %s: %d abilities", race.c_str(), (int)CM_EX(c)->crewPowers.size());
        else CoopGame::Log("test add crew " + race + ": failed");
        break;
    }
    case C_TEST_ADD_SYSTEM:
    {
        int type = r.u8();
        ShipManager* sm = Ship(0);
        if (sm && !SystemOf(sm, type)) { bool fit = sm->CanFitSystem(type); if (fit) sm->AddSystem(type); CoopGame::Log("test add system " + std::to_string(type) + (fit ? " ok" : " (no room)")); }
        break;
    }
    case C_CLOAK:
    {
        bool on = r.u8() != 0;
        ShipManager* sm = Ship(0);
        // Commands are checked like the game's own buttons check them (a network command must not do what the UI
        // would refuse: a hacking pulse with no target froze both copies, 2026-11-06).
        if (sm && sm->cloakSystem && (!on || sm->cloakSystem->Ready()) && !((ShipSystem*)sm->cloakSystem)->GetLocked())
            sm->cloakSystem->SetTurnedOn(on);
        break;
    }
    case C_BATTERY:
    {
        bool on = r.u8() != 0; bool flag = r.u8() != 0;
        ShipManager* sm = Ship(0);
        if (sm && sm->batterySystem) sm->batterySystem->SetTurnedOn(on, flag);
        break;
    }
    case C_HACK_PULSE:
    {
        ShipManager* sm = Ship(0);
        if (sm && sm->hackingSystem && Ship(1) && sm->hackingSystem->bHacking && sm->hackingSystem->CanPulse() &&
            !((ShipSystem*)sm->hackingSystem)->GetLocked())
            sm->hackingSystem->InitiatePulse();
        break;
    }
    case C_HACK_TARGET:
    {
        int ship = r.u8(); int type = r.u8();
        ShipManager* sm = Ship(0);
        ShipSystem* target = SystemOf(Ship(ship), type);
        if (sm && sm->hackingSystem && target && ship == 1 && sm->hackingSystem->CanHack() && sm->tempDroneCount > 0)
            sm->hackingSystem->StartHacking(target);
        break;
    }
    case C_MIND:
    {
        int ship = r.u8(); int room = (int16_t)r.u16(); int n = r.u8();
        ShipManager* target = Ship(ship);
        ShipManager* sm = Ship(0);
        std::vector<CrewMember*> crew;
        for (int k = 0; k < n; ++k) { CrewMember* c = CrewAt(target, r.u16()); if (c) crew.push_back(c); }
        if (sm && sm->mindSystem && target && !crew.empty() && sm->mindSystem->CanUse())
            sm->mindSystem->QueueMindControl(&crew, room, ship);
        break;
    }
    case C_TELEPORT:
    {
        int room = (int16_t)r.u16(); int command = r.i32();
        CompleteShip* me = G_->GetWorld()->playerShip;
        ShipManager* sm = Ship(0);
        if (me && sm && sm->teleportSystem && (sm->teleportSystem->CanSend() || sm->teleportSystem->Charged()))
        { CoopCommands::g_applyingTeleport = true; me->InitiateTeleport(room, command); CoopCommands::g_applyingTeleport = false; }
        break;
    }
    case C_ASSIGN_CREW:
    {
        int idx = r.u16(); int s = (int8_t)r.u8();
        CoopGame::SetCrewOwner(idx, s);
        break;
    }
    case C_SET_MODE:
    {
        CoopGame::Mode m = (CoopGame::Mode)r.u8();
        CoopGame::SetCurrentMode(m);
        CoopOverlay::Post(m == CoopGame::Mode::OwnCrew ? "Ship sharing: each player commands their own crew" : "Ship sharing: free-for-all");
        break;
    }
    default: break;
    }
}

void ShowMessage(const std::string& text)
{
    CoopGame::Log("MSG " + text);
    CoopOverlay::Post(text);
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

HOOK_METHOD_PRIORITY(ChoiceBox, KeyDown, 9000, (SDLKey key) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ChoiceBox::KeyDown -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver())
    {
        if (key >= SDLK_1 && key <= SDLK_9) { CoopGame::SubmitLocal(Choice(key - SDLK_1)); return true; }
        return false;
    }
    return super(key);
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

static int WeaponIndex(ShipManager* s, ProjectileFactory* w)
{
    if (!s || !s->weaponSystem || !w) return -1;
    auto& v = s->weaponSystem->weapons;
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == w) return (int)i;
    return -1;
}

HOOK_METHOD_PRIORITY(WeaponControl, Fire, 9000, (std::vector<Pointf>& points, int target, bool autoFire) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponControl::Fire -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver())
    {
        int idx = WeaponIndex(this->shipManager, this->armedWeapon);
        if (idx >= 0) CoopGame::SubmitLocal(CoopCommands::Fire(idx, target, autoFire, points));
        // Local UI only: drop the armed weapon as the original does after firing.
        this->armedWeapon = nullptr;
        this->armedSlot = -1;
        return;
    }
    super(points, target, autoFire);
}

HOOK_METHOD_PRIORITY(ProjectileFactory, ClearAiming, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ProjectileFactory::ClearAiming -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver())
    {
        int idx = WeaponIndex(G_->GetShipManager(0), this);
        if (idx >= 0) { CoopGame::SubmitLocal(CoopCommands::WeaponClear(idx)); return; }
    }
    super();
}

HOOK_METHOD_PRIORITY(WeaponControl, SetAutofiring, 9000, (bool on, bool simple) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponControl::SetAutofiring -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver()) { CoopGame::SubmitLocal(CoopCommands::AutoFire(on)); return; }
    super(on, simple);
}

// Star map: a click on a beacon arms the jump (readyToTravel). Take that over and send it as a command instead.
HOOK_METHOD_PRIORITY(StarMap, MouseClick, 9000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> StarMap::MouseClick -> Begin (CoopCommands.cpp)\n")
    bool takeOver = ShouldTakeOver();
    // The next sector is the captain's choice; the host's pick is sent as C_SECTOR (StarMap::OnLoop hook below).
    if (takeOver && this->bChoosingNewSector && CoopGame::GetSession() && !CoopGame::GetSession()->IsHost())
    {
        ShowMessage("The captain (host) chooses the next sector");
        return;
    }
    bool wasReady = this->readyToTravel;
    super(mX, mY);
    if (takeOver && !wasReady && this->readyToTravel && this->potentialLoc)
    {
        this->readyToTravel = false;
        for (size_t i = 0; i < this->locations.size(); ++i)
            if (this->locations[i] == this->potentialLoc) CoopGame::SubmitLocal(CoopCommands::Jump((int)i));
    }
}

// The sector choice (a click on a sector, or its jump button) lands in finalSectorChoice, which Hyperspace's
// StarMap::OnLoop (priority 9999) then acts on locally. Run first (hooks are chained with the lowest priority
// outermost, so -10000 wraps every other hook): the host sends it as a command, and no copy acts on it outside lockstep.
HOOK_METHOD_PRIORITY(StarMap, OnLoop, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> StarMap::OnLoop -> Begin (CoopCommands.cpp)\n")
    if (CoopGame::InLockstep() && !CoopGame::Applying() && this->finalSectorChoice >= 0)
    {
        coop::Session* s = CoopGame::GetSession();
        if (s && s->IsHost() && this->NeighboringSector(this->finalSectorChoice)) CoopGame::SubmitLocal(SectorChoice(this->finalSectorChoice));
        this->finalSectorChoice = -1;
    }
    super();
}

// ---------------------------------------------------------------- crew abilities (Hyperspace)
// A player activates an ability with the "lockdown" hotkey (selected crew) or the crew box's ability button; Hyperspace's
// hooks (CustomCrew.cpp, priority 0) then call PreparePower on this copy only. Wrap them (-10000 is outermost) and
// send a command instead, choosing the power exactly as they do.

static void SubmitCrewPower(CrewMember* c, int powerIndex)
{
    for (int ship = 0; ship <= 1; ++ship)
    {
        int idx = CoopCommands::CrewIndex(CoopCommands::Ship(ship), c);
        if (idx < 0) continue;
        Writer w; w.u8(C_CREW_POWER); w.u8((uint8_t)ship); w.u16((uint16_t)idx); w.u8((uint8_t)powerIndex);
        CoopGame::SubmitLocal(w.b);
        return;
    }
}

HOOK_METHOD_PRIORITY(CrewControl, KeyDown, -10000, (SDLKey key) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewControl::KeyDown -> Begin (CoopCommands.cpp)\n")
    if (!ShouldTakeOver() || key != Settings::GetHotkey("lockdown")) { super(key); return; }
    for (CrewMember* c : this->selectedCrew)
    {
        CrewMember_Extend* ex = CM_EX(c);
        if (!ex->hasSpecialPower || c->GetPowerOwner() != 0) continue;
        bool any = false;
        for (size_t i = 0; i < ex->crewPowers.size(); ++i)   // "always" hotkey powers
        {
            ActivatedPower* p = ex->crewPowers[i];
            if (p->enabled && p->def->onHotkey == ActivatedPowerDefinition::HOTKEY_ALWAYS && p->PowerReady() == PowerReadyState::POWER_READY)
            { SubmitCrewPower(c, (int)i); any = true; }
        }
        if (any) continue;
        for (size_t i = 0; i < ex->crewPowers.size(); ++i)   // else the first "first" hotkey power
        {
            ActivatedPower* p = ex->crewPowers[i];
            if (p->enabled && p->def->onHotkey == ActivatedPowerDefinition::HOTKEY_FIRST && p->PowerReady() == PowerReadyState::POWER_READY)
            { SubmitCrewPower(c, (int)i); break; }
        }
    }
}

HOOK_METHOD_PRIORITY(CrewBox, MouseClick, -10000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewBox::MouseClick -> Begin (CoopCommands.cpp)\n")
    if (!ShouldTakeOver()) return super();
    for (ActivatedPowerButton& b : CBOX_EX(this)->powerButtons)
    {
        if (!b.button.bActive || !b.button.bHover) continue;
        if (b.power->PowerReady() == PowerReadyState::POWER_READY && this->pCrew->GetPowerOwner() == 0)
        {
            CrewMember_Extend* ex = CM_EX(this->pCrew);
            for (size_t i = 0; i < ex->crewPowers.size(); ++i) if (ex->crewPowers[i] == b.power) SubmitCrewPower(this->pCrew, (int)i);
        }
        return true;
    }
    return super();
}

// ---------------------------------------------------------------- store and upgrades (the captain's)
// A purchase reaches StoreBox::Purchase (after the click's checks and any confirm dialog). On the host, outside a
// command, send it as C_BUY instead; the derived Purchase functions are hooked too because Linux inlines the base one.
// A box that is in no known store list (a Hyperspace custom store page) is bought locally; the captain-screen resync
// then brings everyone along as before.
static bool g_inPurchaseHook = false;
static bool TakeOverPurchase(StoreBox* box)
{
    if (g_inPurchaseHook || !CoopGame::InLockstep() || CoopGame::Applying()) return false;
    coop::Session* s = CoopGame::GetSession();
    if (!s || !s->IsHost()) return false;
    int st, list, idx;
    if (!FindStoreBox(box, &st, &list, &idx)) { CoopGame::Log("purchase from an unknown store box: bought locally"); return false; }
    Writer w; w.u8(C_BUY); w.u8((uint8_t)st); w.u8((uint8_t)list); w.u8((uint8_t)idx);
    CoopGame::SubmitLocal(w.b);
    return true;
}
#define COOP_PURCHASE_HOOK(cls) \
HOOK_METHOD_PRIORITY(cls, Purchase, -10000, () -> void) \
{ \
    LOG_HOOK("HOOK_METHOD_PRIORITY -> " #cls "::Purchase -> Begin (CoopCommands.cpp)\n") \
    if (TakeOverPurchase((StoreBox*)this)) return; \
    bool outer = !g_inPurchaseHook; g_inPurchaseHook = true; \
    super(); \
    if (outer) g_inPurchaseHook = false; \
}
COOP_PURCHASE_HOOK(StoreBox)
COOP_PURCHASE_HOOK(WeaponStoreBox)
COOP_PURCHASE_HOOK(DroneStoreBox)
COOP_PURCHASE_HOOK(AugmentStoreBox)
COOP_PURCHASE_HOOK(ItemStoreBox)
COOP_PURCHASE_HOOK(RepairStoreBox)
COOP_PURCHASE_HOOK(CrewStoreBox)
COOP_PURCHASE_HOOK(SystemStoreBox)

// Scrap moved by the upgrade screen's clicks (UpgradeBox / ReactorButton click, right click, Undo) on this copy,
// measured around the outermost of those calls. Accept below undoes the rest, so it returns to 0.
static int g_pendingUpgradeScrap = 0;
static int g_upgradeDepth = 0;
static bool HostOutsideCommand();
int CoopCommands::PendingUpgradeScrap() { return g_pendingUpgradeScrap; }
struct UpgradeScrapMeter
{
    ShipManager* s; int before; bool outer;
    explicit UpgradeScrapMeter(ShipManager* ship) : s(ship), before(ship ? ship->currentScrap : 0), outer(g_upgradeDepth++ == 0) {}
    ~UpgradeScrapMeter() { --g_upgradeDepth; if (outer && s && s->iShipId == 0 && HostOutsideCommand()) g_pendingUpgradeScrap += before - s->currentScrap; }
};
static bool HostOutsideCommand()
{
    coop::Session* s = CoopGame::GetSession();
    return CoopGame::InLockstep() && !CoopGame::Applying() && s && s->IsHost();
}
HOOK_METHOD_PRIORITY(UpgradeBox, MouseClick, -10000, (int x, int y) -> void)
{ LOG_HOOK("HOOK_METHOD_PRIORITY -> UpgradeBox::MouseClick -> Begin (CoopCommands.cpp)\n") UpgradeScrapMeter m(this->ship); super(x, y); }
HOOK_METHOD_PRIORITY(UpgradeBox, MouseRightClick, -10000, (int x, int y) -> void)
{ LOG_HOOK("HOOK_METHOD_PRIORITY -> UpgradeBox::MouseRightClick -> Begin (CoopCommands.cpp)\n") UpgradeScrapMeter m(this->ship); super(x, y); }
HOOK_METHOD_PRIORITY(UpgradeBox, Undo, -10000, () -> void)
{ LOG_HOOK("HOOK_METHOD_PRIORITY -> UpgradeBox::Undo -> Begin (CoopCommands.cpp)\n") UpgradeScrapMeter m(this->ship); super(); }
HOOK_METHOD_PRIORITY(ReactorButton, OnClick, -10000, () -> void)
{ LOG_HOOK("HOOK_METHOD_PRIORITY -> ReactorButton::OnClick -> Begin (CoopCommands.cpp)\n") UpgradeScrapMeter m(this->ship); super(); }
HOOK_METHOD_PRIORITY(ReactorButton, OnRightClick, -10000, () -> void)
{ LOG_HOOK("HOOK_METHOD_PRIORITY -> ReactorButton::OnRightClick -> Begin (CoopCommands.cpp)\n") UpgradeScrapMeter m(this->ship); super(); }
HOOK_METHOD_PRIORITY(ReactorButton, Undo, -10000, () -> void)
{ LOG_HOOK("HOOK_METHOD_PRIORITY -> ReactorButton::Undo -> Begin (CoopCommands.cpp)\n") UpgradeScrapMeter m(this->ship); super(); }

// Upgrades: clicking an upgrade box takes the scrap on this copy at once (Undo gives it back); closing the screen calls
// Accept. On the host, Accept undoes its own pending levels (measuring their cost) and sends C_UPGRADE instead.

HOOK_METHOD_PRIORITY(UpgradeBox, Accept, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> UpgradeBox::Accept -> Begin (CoopCommands.cpp)\n")
    if (HostOutsideCommand() && this->tempUpgrade > 0 && this->system && this->ship && this->ship->iShipId == 0)
    {
        int levels = this->tempUpgrade, before = this->ship->currentScrap;
        for (int k = 0; k < 16 && this->tempUpgrade > 0; ++k) this->Undo();
        int cost = this->ship->currentScrap - before;
        if (this->tempUpgrade == 0 && cost >= 0)
        {
            Writer w; w.u8(C_UPGRADE); w.u8((uint8_t)this->system->iSystemType); w.u8((uint8_t)levels); w.u16((uint16_t)cost);
            CoopGame::SubmitLocal(w.b);
            return;
        }
        CoopGame::Log("upgrade undo did not behave as expected: applied locally");
    }
    super();
}

HOOK_METHOD_PRIORITY(ReactorButton, Accept, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ReactorButton::Accept -> Begin (CoopCommands.cpp)\n")
    if (HostOutsideCommand() && this->tempUpgrade > 0 && this->ship && this->ship->iShipId == 0)
    {
        int levels = this->tempUpgrade, before = this->ship->currentScrap;
        for (int k = 0; k < 32 && this->tempUpgrade > 0; ++k) this->Undo();
        int cost = this->ship->currentScrap - before;
        if (this->tempUpgrade == 0 && cost >= 0)
        {
            Writer w; w.u8(C_UPGRADE); w.u8(255); w.u8((uint8_t)levels); w.u16((uint16_t)cost);
            CoopGame::SubmitLocal(w.b);
            return;
        }
        CoopGame::Log("reactor undo did not behave as expected: applied locally");
    }
    super();
}

// ---------------------------------------------------------------- special systems

HOOK_METHOD_PRIORITY(CloakingSystem, SetTurnedOn, 9000, (bool on) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CloakingSystem::SetTurnedOn -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver()) { Writer w; w.u8(C_CLOAK); w.u8(on ? 1 : 0); CoopGame::SubmitLocal(w.b); return; }
    super(on);
}

HOOK_METHOD_PRIORITY(BatterySystem, SetTurnedOn, 9000, (bool on, bool tutorial) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> BatterySystem::SetTurnedOn -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver()) { Writer w; w.u8(C_BATTERY); w.u8(on ? 1 : 0); w.u8(tutorial ? 1 : 0); CoopGame::SubmitLocal(w.b); return; }
    super(on, tutorial);
}

HOOK_METHOD_PRIORITY(HackingSystem, InitiatePulse, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> HackingSystem::InitiatePulse -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver()) { Writer w; w.u8(C_HACK_PULSE); CoopGame::SubmitLocal(w.b); return; }
    super();
}

HOOK_METHOD_PRIORITY(HackingSystem, StartHacking, 9000, (ShipSystem* sys) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> HackingSystem::StartHacking -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver() && sys)
    {
        Writer w; w.u8(C_HACK_TARGET); w.u8((uint8_t)sys->_shipObj.iShipId); w.u8((uint8_t)sys->iSystemType);
        CoopGame::SubmitLocal(w.b);
        return;
    }
    super(sys);
}

HOOK_METHOD_PRIORITY(MindSystem, QueueMindControl, 9000, (std::vector<CrewMember*>* crew, int roomId, int shipId) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MindSystem::QueueMindControl -> Begin (CoopCommands.cpp)\n")
    if (ShouldTakeOver() && crew)
    {
        ShipManager* target = CoopCommands::Ship(shipId);
        Writer w; w.u8(C_MIND); w.u8((uint8_t)shipId); w.u16((uint16_t)(int16_t)roomId);
        std::vector<int> idx;
        for (CrewMember* c : *crew) { int i = CoopCommands::CrewIndex(target, c); if (i >= 0) idx.push_back(i); }
        w.u8((uint8_t)idx.size());
        for (int i : idx) w.u16((uint16_t)i);
        CoopGame::SubmitLocal(w.b);
        return;
    }
    super(crew, roomId, shipId);
}

// The teleporter is started from CommandGui::OnLoop (from the local UI's teleport selection), not from an input
// handler: take over InitiateTeleport on the player ship while CommandGui::OnLoop runs.
HOOK_METHOD_PRIORITY(CommandGui, OnLoop, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::OnLoop -> Begin (CoopCommands.cpp)\n")
    CoopCommands::g_inGuiLoop = true;
    super();
    CoopCommands::g_inGuiLoop = false;
}

HOOK_METHOD_PRIORITY(CompleteShip, InitiateTeleport, 9000, (int targetRoom, int command) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CompleteShip::InitiateTeleport -> Begin (CoopCommands.cpp)\n")
    if (CoopGame::InLockstep() && CoopCommands::g_inGuiLoop && !CoopCommands::g_applyingTeleport && !CoopGame::Applying() &&
        this == G_->GetWorld()->playerShip)
    {
        Writer w; w.u8(C_TELEPORT); w.u16((uint16_t)(int16_t)targetRoom); w.i32(command);
        CoopGame::SubmitLocal(w.b);
        return;
    }
    super(targetRoom, command);
}
