// FTL co-op: store, upgrades, equipment and crew manifest are the captain's (host's).
// Guests can look but their clicks there are ignored. The host changes things locally; while one of these screens is
// open, hash mismatches are expected and ignored, and when it closes everyone reloads the host's save if the ship
// changed. Not yet individual commands (see COOP.md "Known gaps").
#include "CoopCommands.h"
#include "Global.h"

namespace CoopCaptain {

static bool g_open = false;      // host: a captain screen is open
static bool g_dirty = false;     // host: a desync was seen while it was open
static uint64_t g_before = 0;

static bool IsGuestInGame() { return CoopGame::InLockstep() && CoopGame::GetSession() && !CoopGame::GetSession()->IsHost(); }
static bool IsHostInGame() { return CoopGame::InLockstep() && CoopGame::GetSession() && CoopGame::GetSession()->IsHost(); }

static uint64_t ShipFingerprint()
{
    coop::Hasher h;
    ShipManager* s = G_->GetShipManager(0);
    if (!s) return 0;
    h.val(s->currentScrap); h.val(s->fuel_count); h.val(s->tempMissileCount); h.val(s->tempDroneCount);
    h.val(s->ship.hullIntegrity.first); h.val((uint32_t)s->vCrewList.size()); h.val((uint32_t)s->vSystemList.size());
    for (ShipSystem* sys : s->vSystemList) if (sys) { h.val(sys->iSystemType); h.val(sys->powerState.second); h.val(sys->powerState.first); }
    if (s->weaponSystem) for (ProjectileFactory* w : s->weaponSystem->weapons) h.val((void*)(w ? w->blueprint : nullptr));
    if (s->droneSystem) for (Drone* d : s->droneSystem->drones) h.val((void*)d);
    return h.h;
}

bool SuppressDesync()
{
    if (g_open) { g_dirty = true; return true; }
    return false;
}

static bool IsCaptainScreen(TabbedWindow* w)
{
    CommandGui* gui = G_->GetWorld() ? G_->GetWorld()->commandGui : nullptr;
    return gui && (w == &gui->shipScreens || w == &gui->storeScreens);
}

static void Blocked() { CoopCommands::ShowMessage("Only the captain (host) can buy, upgrade or change equipment"); }

} // namespace CoopCaptain

using namespace CoopCaptain;

HOOK_METHOD_PRIORITY(TabbedWindow, Open, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> TabbedWindow::Open -> Begin (CoopCaptainScreens.cpp)\n")
    super();
    if (IsHostInGame() && IsCaptainScreen(this) && !g_open) { g_open = true; g_dirty = false; g_before = ShipFingerprint(); }
}

HOOK_METHOD_PRIORITY(TabbedWindow, Close, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> TabbedWindow::Close -> Begin (CoopCaptainScreens.cpp)\n")
    bool was = g_open && IsCaptainScreen(this);
    super();
    if (!was) return;
    CommandGui* gui = G_->GetWorld()->commandGui;
    if (gui->shipScreens.bOpen || gui->storeScreens.bOpen) return; // the other one is still open
    g_open = false;
    if (IsHostInGame() && (g_dirty || ShipFingerprint() != g_before)) CoopGame::RequestResyncSoon("captain changed the ship");
}

HOOK_METHOD_PRIORITY(Store, MouseClick, 9000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Store::MouseClick -> Begin (CoopCaptainScreens.cpp)\n")
    if (IsGuestInGame()) { Blocked(); return; }
    super(x, y);
}

HOOK_METHOD_PRIORITY(Upgrades, MouseClick, 9000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Upgrades::MouseClick -> Begin (CoopCaptainScreens.cpp)\n")
    if (IsGuestInGame()) { Blocked(); return; }
    super(x, y);
}

HOOK_METHOD_PRIORITY(Equipment, MouseClick, 9000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Equipment::MouseClick -> Begin (CoopCaptainScreens.cpp)\n")
    if (IsGuestInGame()) { Blocked(); return; }
    super(x, y);
}

HOOK_METHOD_PRIORITY(Equipment, MouseUp, 9000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Equipment::MouseUp -> Begin (CoopCaptainScreens.cpp)\n")
    if (IsGuestInGame()) return;
    super(x, y);
}

HOOK_METHOD_PRIORITY(CrewManifest, MouseClick, 9000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewManifest::MouseClick -> Begin (CoopCaptainScreens.cpp)\n")
    if (IsGuestInGame()) { Blocked(); return; }
    super(x, y);
}
