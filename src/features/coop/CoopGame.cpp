// FTL co-op: game side of the lockstep session.
// Turned on by FTL_COOP=host|guest|solo (environment) or a coop.cfg next to FTL.amd64. Without it nothing changes.
#include "CoopGame.h"
#include "CoopCommands.h"
#include "CoopRng.h"
#include "Global.h"
#include "Seeds.h"
#include "overrides/FileHelper.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>

namespace CoopGame {

static Config g_cfg;
static bool g_cfgLoaded = false;
static FILE* g_log = nullptr;
static std::unique_ptr<coop::TcpTransport> g_transport;
static std::unique_ptr<coop::Session> g_session;
static bool g_applying = false;
static bool g_inLockstep = false;
static bool g_quitRequested = false;

static const float FIXED_SPEED = 16.0f / 60.0f; // FTL: 16 speed units per second, 60 lockstep frames per second

// ---------------------------------------------------------------- config and log

static std::string Env(const char* k, const std::string& def = "")
{
    const char* v = getenv(k);
    return v ? std::string(v) : def;
}

static void LoadConfigFile(std::map<std::string, std::string>& kv)
{
    std::ifstream f("coop.cfg");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        kv[k] = v;
    }
}

const Config& GetConfig()
{
    if (g_cfgLoaded) return g_cfg;
    g_cfgLoaded = true;
    std::map<std::string, std::string> kv;
    LoadConfigFile(kv);
    auto get = [&](const char* envName, const char* cfgName, const std::string& def) {
        std::string e = Env(envName);
        if (!e.empty()) return e;
        auto it = kv.find(cfgName);
        return it != kv.end() ? it->second : def;
    };
    std::string role = get("FTL_COOP", "role", "");
    if (role != "host" && role != "guest" && role != "solo") return g_cfg;
    g_cfg.enabled = true;
    g_cfg.host = role != "guest";
    if (role == "solo") g_cfg.waitPlayers = 1;
    std::string addr = get("FTL_COOP_ADDR", "address", "127.0.0.1");
    size_t colon = addr.rfind(':');
    if (colon != std::string::npos) { g_cfg.port = atoi(addr.substr(colon + 1).c_str()); addr = addr.substr(0, colon); }
    g_cfg.addr = addr;
    g_cfg.port = atoi(get("FTL_COOP_PORT", "port", std::to_string(g_cfg.port)).c_str());
    g_cfg.name = get("FTL_COOP_NAME", "name", g_cfg.host ? "host" : "guest");
    g_cfg.waitPlayers = atoi(get("FTL_COOP_PLAYERS", "players", std::to_string(g_cfg.waitPlayers)).c_str());
    g_cfg.mode = get("FTL_COOP_MODE", "mode", "ffa") == "owncrew" ? Mode::OwnCrew : Mode::FreeForAll;
    g_cfg.userDir = get("FTL_COOP_USERDIR", "userdir", "");
    g_cfg.logPath = get("FTL_COOP_LOG", "log", "coop.log");
    g_cfg.bot = get("FTL_COOP_BOT", "bot", "");
    g_cfg.seed = (unsigned)strtoul(get("FTL_COOP_SEED", "seed", "0").c_str(), nullptr, 10);
    g_cfg.shipType = atoi(get("FTL_COOP_SHIP", "ship", "0").c_str());
    g_cfg.shipVariant = atoi(get("FTL_COOP_VARIANT", "variant", "0").c_str());
    g_cfg.exitAfterFrames = (uint32_t)strtoul(get("FTL_COOP_EXIT_AFTER", "exit_after", "0").c_str(), nullptr, 10);
    g_cfg.delay = (uint32_t)atoi(get("FTL_COOP_DELAY", "delay", "4").c_str());
    g_cfg.window = (uint32_t)atoi(get("FTL_COOP_WINDOW", "window", "12").c_str());
    g_cfg.hashEvery = (uint32_t)atoi(get("FTL_COOP_HASH_EVERY", "hash_every", "30").c_str());
    return g_cfg;
}

bool Active() { return GetConfig().enabled; }
bool InLockstep() { return g_inLockstep; }
bool Applying() { return g_applying; }
coop::Session* GetSession() { return g_session.get(); }
int LocalSlot() { return g_session ? g_session->Slot() : 0; }

void Log(const std::string& line)
{
    if (!g_log && Active()) g_log = fopen(GetConfig().logPath.c_str(), "w");
    if (!g_log) return;
    uint32_t f = g_session ? g_session->Frame() : 0;
    fprintf(g_log, "[%.3f f%u] %s\n", (double)clock() / CLOCKS_PER_SEC, f, line.c_str());
    fflush(g_log);
}

static void Logf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    Log(buf);
}

// ---------------------------------------------------------------- game access

static CommandGui* Gui()
{
    if (!G_ || !G_->GetCApp() || !G_->GetCApp()->world) return nullptr;
    return G_->GetWorld()->commandGui;
}

static bool InGame()
{
    CommandGui* gui = Gui();
    return gui && G_->GetWorld()->bStartedGame && !G_->GetCApp()->menu.bOpen && G_->GetShipManager(0);
}

// ---------------------------------------------------------------- hashing

static void HashShip(coop::Hasher& h, ShipManager* s, bool detailCrew)
{
    if (!s) { h.val(0xdead); return; }
    h.val(s->ship.hullIntegrity.first);
    h.val(s->currentScrap); h.val(s->fuel_count); h.val(s->tempMissileCount); h.val(s->tempDroneCount);
    h.val(s->bJumping); h.val(s->bDestroyed);
    for (ShipSystem* sys : s->vSystemList)
    {
        if (!sys) continue;
        h.val(sys->iSystemType); h.val(sys->powerState.first); h.val(sys->powerState.second);
        h.val(sys->healthState.first); h.val(sys->fDamage); h.val(sys->iLockCount); h.val(sys->iBatteryPower);
    }
    if (s->weaponSystem)
        for (ProjectileFactory* w : s->weaponSystem->weapons) { if (w) { h.val(w->cooldown.first); h.val(w->powered); } }
    if (detailCrew)
        for (CrewMember* c : s->vCrewList) { if (c) { h.val(c->x); h.val(c->y); h.val(c->health.first); h.val(c->iRoomId); } }
    for (Room* r : s->ship.vRoomList) { (void)r; }
    for (Door* d : s->ship.vDoorList) { if (d) { h.val(d->bOpen); h.val(d->health); } }
}

struct HashParts { uint64_t all, rng, time, player, crew, enemy, ui; };

static HashParts ComputeHash()
{
    HashParts p;
    coop::Hasher hr; hr.val(CoopRng::GetState()); hr.val(CoopRng::GetSil()); p.rng = hr.h;
    coop::Hasher ht; ht.val(G_->GetCFPS()->RunningTime); p.time = ht.h;
    coop::Hasher hp; HashShip(hp, G_->GetShipManager(0), false); p.player = hp.h;
    coop::Hasher hc;
    if (ShipManager* s = G_->GetShipManager(0)) for (CrewMember* c : s->vCrewList) { if (c) { hc.val(c->x); hc.val(c->y); hc.val(c->health.first); } }
    if (ShipManager* e = G_->GetShipManager(1)) for (CrewMember* c : e->vCrewList) { if (c) { hc.val(c->x); hc.val(c->y); hc.val(c->health.first); } }
    p.crew = hc.h;
    coop::Hasher he; HashShip(he, G_->GetShipManager(1), false); p.enemy = he.h;
    coop::Hasher hu; CommandGui* gui = Gui(); if (gui) { hu.val(gui->bPaused); hu.val(gui->choiceBox.bOpen); } p.ui = hu.h;
    coop::Hasher all; all.val(p.rng); all.val(p.time); all.val(p.player); all.val(p.crew); all.val(p.enemy); all.val(p.ui);
    p.all = all.h;
    return p;
}

// ---------------------------------------------------------------- start of a run

enum class StartStage { None, OpenBuilder, Finish, WaitInGame };
static StartStage g_startStage = StartStage::None;
static int g_startWait = 0;
struct StartInfo { unsigned seed = 0; int shipType = 0, variant = 0; Mode mode = Mode::FreeForAll; uint8_t players = 1; };
static StartInfo g_start;

static coop::Bytes MakeStartBlob()
{
    unsigned seed = GetConfig().seed;
    if (!seed) seed = (unsigned)time(nullptr) ^ (unsigned)clock();
    coop::Writer w;
    w.u32(seed); w.u8((uint8_t)GetConfig().shipType); w.u8((uint8_t)GetConfig().shipVariant);
    w.u8((uint8_t)GetConfig().mode); w.u8((uint8_t)(g_session ? g_session->PlayerCount() : 1));
    return w.b;
}

// ---------------------------------------------------------------- adapter

class GameAdapter : public coop::Adapter
{
public:
    bool Allowed(int slot, const coop::Bytes& cmd, std::string* reason) override
    {
        return CoopCommands::Allowed(slot, cmd, g_start.mode, reason);
    }
    void Apply(int slot, const coop::Bytes& cmd) override
    {
        g_applying = true;
        CoopCommands::Apply(slot, cmd);
        g_applying = false;
    }
    uint64_t Hash() override
    {
        HashParts p = ComputeHash();
        Logf("HASH all=%016llx rng=%016llx time=%016llx player=%016llx crew=%016llx enemy=%016llx ui=%016llx",
             (unsigned long long)p.all, (unsigned long long)p.rng, (unsigned long long)p.time, (unsigned long long)p.player,
             (unsigned long long)p.crew, (unsigned long long)p.enemy, (unsigned long long)p.ui);
        return p.all;
    }
    void OnLog(const std::string& line) override { Log("net: " + line); }
    void OnStart(const coop::Bytes& blob) override
    {
        coop::Reader r(blob);
        g_start.seed = r.u32(); g_start.shipType = r.u8(); g_start.variant = r.u8();
        g_start.mode = (Mode)r.u8(); g_start.players = r.u8();
        Logf("START seed=%u ship=%d variant=%d mode=%d players=%d", g_start.seed, g_start.shipType, g_start.variant,
             (int)g_start.mode, g_start.players);
        CoopCommands::OnRunStart(g_start.players);
        g_startStage = StartStage::OpenBuilder;
        g_startWait = 0;
    }
    void OnDesync(uint32_t frame, int slot, uint64_t mine, uint64_t theirs) override
    {
        Logf("DESYNC frame=%u slot=%d mine=%016llx theirs=%016llx", frame, slot, (unsigned long long)mine, (unsigned long long)theirs);
    }
    void OnReject(const std::string& reason) override { Log("REJECTED " + reason); CoopCommands::ShowMessage("Not allowed: " + reason); }
    void OnPlayerJoined(int slot, const std::string& name) override { Logf("JOIN slot=%d name=%s", slot, name.c_str()); }
    void OnPlayerLeft(int slot) override { Logf("LEAVE slot=%d", slot); }
};
static GameAdapter g_adapter;

bool SubmitLocal(const coop::Bytes& cmd)
{
    if (!g_inLockstep || !g_session) return false;
    g_session->Submit(cmd);
    return true;
}

static void InitSession()
{
    const Config& c = GetConfig();
    if (!c.userDir.empty()) FileHelperExtension::setUserFolder(c.userDir);
    if (!CoopRng::SilAvailable())
    {
        Log("ERROR: random32 state not found; this FTL build is not supported. Co-op is off.");
        g_cfg.enabled = false;
        return;
    }
    coop::Settings s;
    s.delay = c.delay; s.window = c.window; s.hashEvery = c.hashEvery; s.maxPlayers = 8;
    s.buildId = std::string(__DATE__) + " " + __TIME__;
    if (!Env("FTL_COOP_BUILD_ID").empty()) s.buildId = Env("FTL_COOP_BUILD_ID");
    g_transport.reset(new coop::TcpTransport());
    // The session installs the transport callbacks, so it must exist before Connect (which says hello).
    g_session.reset(new coop::Session(g_transport.get(), &g_adapter, s, c.host, c.name));
    std::string err;
    bool ok = c.host ? (Env("FTL_COOP") == "solo" ? true : g_transport->Listen(c.port, &err))
                     : g_transport->Connect(c.addr, c.port, &err);
    if (!ok)
    {
        Log("ERROR: network: " + err);
        CoopCommands::ShowMessage("Co-op network error: " + err);
        return;
    }
    Logf("co-op %s ready: %s:%d name=%s mode=%s bot=%s build=%s", c.host ? "host" : "guest", c.addr.c_str(), c.port,
         c.name.c_str(), c.mode == Mode::OwnCrew ? "owncrew" : "ffa", c.bot.c_str(), s.buildId.c_str());
}

// Run start: driven from CApp::OnLoop, a few frames per stage like the Hyperspace test scenarios.
static void StepStart()
{
    if (g_startStage == StartStage::None) return;
    ++g_startWait;
    MainMenu& menu = G_->GetCApp()->menu;
    if (g_startStage == StartStage::OpenBuilder)
    {
        if (!menu.bOpen) return; // must be at the main menu
        menu.shipBuilder.Open();
        g_startStage = StartStage::Finish; g_startWait = 0;
    }
    else if (g_startStage == StartStage::Finish && g_startWait >= 10)
    {
        menu.shipBuilder.SwitchShip(g_start.shipType, g_start.variant);
        // Every RNG the game uses starts from the shared seed.
        CoopRng::Own(true);
        CoopRng::SetState(g_start.seed);
        CoopRng::SetSil(g_start.seed);
        SetSeed(g_start.seed);
        menu.shipBuilder.Finish();
        Log("ship builder finished");
        g_startStage = StartStage::WaitInGame; g_startWait = 0;
    }
    else if (g_startStage == StartStage::WaitInGame && InGame())
    {
        // Same seeds again at the first lockstep frame: anything consumed while loading is forgotten.
        CoopRng::SetState(g_start.seed ^ 0x9e3779b9u);
        CoopRng::SetSil(g_start.seed ^ 0x7f4a7c15u);
        Global::seededRng = std::mt19937(g_start.seed);
        G_->GetCFPS()->RunningTime = 0.f;
        g_inLockstep = true;
        g_startStage = StartStage::None;
        Logf("in game, lockstep frames begin (waited %d loops)", g_startWait);
    }
}

static void HostMaybeStart()
{
    if (!g_session || !g_session->IsHost() || g_session->Started()) return;
    if (!G_->GetCApp()->menu.bOpen) return;
    int want = GetConfig().waitPlayers;
    if (want > 0 && g_session->PlayerCount() >= want)
    {
        Logf("all %d players present, starting", want);
        g_session->Start(MakeStartBlob());
    }
}

void RequestManualStart()
{
    if (g_session && g_session->IsHost() && !g_session->Started() && G_->GetCApp()->menu.bOpen)
        g_session->Start(MakeStartBlob());
}

static void Quit(const char* why)
{
    if (g_quitRequested) return;
    g_quitRequested = true;
    Logf("SUMMARY frames=%u desyncs=%u stalls=%u players=%d reason=%s", g_session ? g_session->Frame() : 0,
         g_session ? g_session->DesyncCount() : 0, g_session ? g_session->StallFrames() : 0,
         g_session ? g_session->PlayerCount() : 0, why);
    G_->GetCApp()->OnRequestExit();
}

// Called from the CApp::OnLoop hook. Returns how many game frames to run now (0 = stall).
int BeforeLoop()
{
    static bool inited = false;
    if (!inited) { inited = true; InitSession(); }
    if (!Active() || !g_session) return 1;
    g_session->Poll();
    HostMaybeStart();
    StepStart();
    if (!g_inLockstep) return 1;
    if (!InGame())
    {
        // Back at the menu (game over, quit): lockstep ends for this copy.
        Log("left the game; lockstep stopped");
        g_inLockstep = false;
        CoopRng::Own(false);
        return 1;
    }
    return -1; // caller drives frames through RunFrame()
}

bool RunFrameBegin()
{
    if (!g_session->BeginFrame()) return false;
    CoopBot::BeforeFrame(g_session->Frame());
    return true;
}

void RunFrameEnd()
{
    G_->GetCFPS()->SpeedFactor = FIXED_SPEED; // in case something in the frame recomputed it
    g_session->EndFrame();
    uint32_t f = g_session->Frame();
    if (GetConfig().exitAfterFrames && f >= GetConfig().exitAfterFrames) Quit("exit_after reached");
}

bool FramesQueued() { return g_session && g_session->Started(); }

} // namespace CoopGame

// ---------------------------------------------------------------- hooks

using namespace CoopGame;

HOOK_METHOD_PRIORITY(CApp, OnLoop, -1000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLoop -> Begin (CoopGame.cpp)\n")
    int n = BeforeLoop();
    if (n >= 0) { super(); return; }
    // Lockstep: run the next frame when its bundle is here; run up to 3 extra to catch up.
    for (int i = 0; i < 4; ++i)
    {
        if (!RunFrameBegin()) break;
        super();
        RunFrameEnd();
        if (!CoopGame::InLockstep()) break;
    }
}

// Fixed time step: CFPS::OnLoop computes SpeedFactor from the wall clock.
HOOK_METHOD(CFPS, OnLoop, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CFPS::OnLoop -> Begin (CoopGame.cpp)\n")
    super();
    if (CoopGame::InLockstep()) this->SpeedFactor = 16.0f / 60.0f;
}

HOOK_METHOD(CFPS, GetSpeedFactor, () -> float)
{
    LOG_HOOK("HOOK_METHOD -> CFPS::GetSpeedFactor -> Begin (CoopGame.cpp)\n")
    if (CoopGame::InLockstep()) return 16.0f / 60.0f;
    return super();
}

// Rendering may draw random numbers (particles, flicker). Keep them out of the simulation's streams.
HOOK_METHOD_PRIORITY(CApp, OnRender, -1000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRender -> Begin (CoopGame.cpp)\n")
    if (!CoopGame::InLockstep()) { super(); return; }
    uint64_t r = CoopRng::GetState(), s = CoopRng::GetSil();
    std::mt19937 seeded = Global::seededRng;
    super();
    CoopRng::SetState(r); CoopRng::SetSil(s);
    Global::seededRng = seeded;
}

// Losing window focus pauses FTL. In co-op that would pause only one copy: the pause is a command instead.
HOOK_METHOD(CApp, OnInputBlur, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnInputBlur -> Begin (CoopGame.cpp)\n")
    if (CoopGame::Active()) return;
    super();
}

HOOK_METHOD(CApp, OnInputFocus, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnInputFocus -> Begin (CoopGame.cpp)\n")
    if (CoopGame::Active()) return;
    super();
}

HOOK_METHOD(CApp, OnKeyDown, (SDLKey key) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnKeyDown -> Begin (CoopGame.cpp)\n")
    if (CoopGame::Active() && key == SDLK_F9) { CoopGame::RequestManualStart(); return; }
    super(key);
}
