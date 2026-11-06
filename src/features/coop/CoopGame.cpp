// FTL co-op: game side of the lockstep session.
// Turned on by FTL_COOP=host|guest|solo (environment) or a coop.cfg next to FTL.amd64. Without it nothing changes.
#include "CoopGame.h"
#include "CoopCommands.h"
#include "CoopRng.h"
#include "Global.h"
#include "Seeds.h"
#include "overrides/FileHelper.h"

#include <cstdarg>
#include <sys/mman.h>
#include <unistd.h>
#include <cstddef>
#include <dlfcn.h>
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
uint32_t g_catchupFrames = 0;
static uint32_t g_totalFramesAtRestart = 0; // frames run in earlier epochs   // extra frames run in the same loop (guest catching up)

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
    g_cfg.autoResync = get("FTL_COOP_AUTO_RESYNC", "auto_resync", "1") == "1";
    g_cfg.startContinue = get("FTL_COOP_START", "start", "new") == "continue";
    g_cfg.testResyncAt = (uint32_t)strtoul(get("FTL_COOP_TEST_RESYNC_AT", "test_resync_at", "0").c_str(), nullptr, 10);
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
    uint32_t e = g_session ? g_session->Epoch() : 0;
    fprintf(g_log, "[%.3f f%u e%u] %s\n", (double)clock() / CLOCKS_PER_SEC, f, e, line.c_str());
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
        for (ProjectileFactory* w : s->weaponSystem->weapons) { if (w) { h.val(w->cooldown.first); h.val(w->powered); h.val(w->autoFiring); h.val(w->targetId); h.val((uint32_t)w->targets.size()); } }
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
    coop::Hasher hu; CommandGui* gui = Gui(); if (gui) { hu.val(gui->bPaused); hu.val(gui->choiceBox.bOpen); }
    {
        StarMap& map = G_->GetWorld()->starMap;
        int cur = -1;
        for (size_t i = 0; i < map.locations.size(); ++i) if (map.locations[i] == map.currentLoc) cur = (int)i;
        hu.val(cur); hu.val(map.worldLevel); hu.val(map.readyToTravel);
    }
    p.ui = hu.h;
    coop::Hasher all; all.val(p.rng); all.val(p.time); all.val(p.player); all.val(p.crew); all.val(p.enemy); all.val(p.ui);
    p.all = all.h;
    return p;
}

// ---------------------------------------------------------------- shared pause

// FTL pauses the simulation while any focus window is open (menu_pause, recomputed in CommandGui::OnLoop from the
// window list; IsPaused() = bPaused || menu_pause). In co-op a player's own windows (store, ship screen, map, Esc
// menu) must not pause only their copy. The three writes in CommandGui::OnLoop are patched out and menu_pause is
// set every frame from shared state only: an event dialog or the game-over screen is open.
static bool PatchBytes(uintptr_t addr, const unsigned char* expect, const unsigned char* repl, size_t n)
{
    if (memcmp((void*)addr, expect, n) != 0) return false;
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = addr & ~(uintptr_t)(page - 1);
    size_t len = (addr + n) - start;
    if (mprotect((void*)start, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    memcpy((void*)addr, repl, n);
    mprotect((void*)start, len, PROT_READ | PROT_EXEC);
    return true;
}

static bool PatchMenuPause()
{
    static const unsigned char set0[7] = { 0xc6, 0x83, 0x92, 0x32, 0x00, 0x00, 0x00 };
    static const unsigned char set1[7] = { 0xc6, 0x83, 0x92, 0x32, 0x00, 0x00, 0x01 };
    static const unsigned char nop7[7] = { 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00 };
    bool ok = PatchBytes(0x4fec6b, set0, nop7, 7);
    ok = PatchBytes(0x4fecc5, set1, nop7, 7) && ok;
    ok = PatchBytes(0x4fed03, set1, nop7, 7) && ok;
    return ok;
}

static void SetSharedMenuPause()
{
    CommandGui* gui = Gui();
    if (!gui) return;
    gui->menu_pause = gui->choiceBox.bOpen || gui->gameOverScreen.bOpen;
}

// ---------------------------------------------------------------- start of a run

enum class StartStage { None, OpenBuilder, Finish, LoadToMenu, LoadFile, WaitInGame };
static StartStage g_startStage = StartStage::None;
static int g_startWait = 0;
enum StartKind : uint8_t { START_NEW = 0, START_LOAD = 1 };
struct StartInfo { uint8_t kind = START_NEW; unsigned seed = 0; int shipType = 0, variant = 0; Mode mode = Mode::FreeForAll;
                   uint8_t players = 1; coop::Bytes save; std::vector<int8_t> owners; };
static StartInfo g_start;
static uint32_t g_resyncs = 0;

static unsigned NewSeed()
{
    unsigned seed = GetConfig().seed;
    if (!seed || g_resyncs) seed = (unsigned)time(nullptr) ^ (unsigned)clock() ^ (g_resyncs * 2654435761u) ^ GetConfig().seed;
    return seed ? seed : 1;
}

static coop::Bytes MakeStartBlob()
{
    coop::Writer w;
    w.u8(START_NEW); w.u32(NewSeed()); w.u8((uint8_t)GetConfig().shipType); w.u8((uint8_t)GetConfig().shipVariant);
    w.u8((uint8_t)g_start.mode); w.u8((uint8_t)(g_session ? g_session->PlayerCount() : 1));
    w.u16(0);
    return w.b;
}

static bool ReadFile(const std::string& path, coop::Bytes& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

static bool WriteFile(const std::string& path, const coop::Bytes& data)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

// Blob for "everyone loads this save": used to continue a saved run, to resync after a desync, after a late join.
static bool MakeLoadBlob(coop::Bytes& blob, std::string* err)
{
    std::string path = FileHelper::getSaveFile();
    coop::Bytes save;
    if (!ReadFile(path, save) || save.empty()) { *err = "no save file at " + path; return false; }
    coop::Writer w;
    w.u8(START_LOAD); w.u32(NewSeed()); w.u8(0); w.u8(0);
    w.u8((uint8_t)g_start.mode); w.u8((uint8_t)(g_session ? g_session->PlayerCount() : 1));
    // Crew ownership survives the reload (by crew index on the player ship).
    std::vector<int8_t> owners;
    if (ShipManager* s = G_->GetWorld() && G_->GetWorld()->playerShip ? G_->GetShipManager(0) : nullptr)
        for (CrewMember* c : s->vCrewList) owners.push_back((int8_t)CrewOwner(c));
    w.u16((uint16_t)owners.size());
    for (int8_t o : owners) w.u8((uint8_t)o);
    w.bytes(save);
    blob = w.b;
    return true;
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
        Logf("HASH all=%016llx rng=%016llx time=%016llx player=%016llx crew=%016llx enemy=%016llx ui=%016llx nrand=%u nrandall=%u nr32=%u",
             (unsigned long long)p.all, (unsigned long long)p.rng, (unsigned long long)p.time, (unsigned long long)p.player,
             (unsigned long long)p.crew, (unsigned long long)p.enemy, (unsigned long long)p.ui, CoopRng::g_randCalls, CoopRng::g_randCallsAll, CoopRng::g_r32Calls);
        if (CoopRng::Tracing()) Log("RNGTRACE " + CoopRng::TraceDump());
        return p.all;
    }
    void OnLog(const std::string& line) override { Log("net: " + line); }
    void OnStart(const coop::Bytes& blob) override
    {
        coop::Reader r(blob);
        g_start.kind = r.u8(); g_start.seed = r.u32(); g_start.shipType = r.u8(); g_start.variant = r.u8();
        g_start.mode = (Mode)r.u8(); g_start.players = r.u8();
        g_start.owners.clear();
        uint16_t no = r.u16();
        for (uint16_t i = 0; i < no; ++i) g_start.owners.push_back((int8_t)r.u8());
        g_start.save = g_start.kind == START_LOAD ? r.bytes() : coop::Bytes();
        Logf("START kind=%s seed=%u ship=%d variant=%d mode=%d players=%d save=%zu bytes epoch=%u",
             g_start.kind == START_LOAD ? "load" : "new", g_start.seed, g_start.shipType, g_start.variant,
             (int)g_start.mode, g_start.players, g_start.save.size(), g_session ? g_session->Epoch() : 0);
        CoopCommands::OnRunStart(g_start.players);
        if (g_inLockstep && g_session) g_totalFramesAtRestart += g_session->Frame();
        g_inLockstep = false; // until this copy is in the (re)started game
        g_startStage = g_start.kind == START_LOAD ? StartStage::LoadToMenu : StartStage::OpenBuilder;
        g_startWait = 0;
    }
    void OnDesync(uint32_t frame, int slot, uint64_t mine, uint64_t theirs) override
    {
        Logf("DESYNC frame=%u slot=%d mine=%016llx theirs=%016llx", frame, slot, (unsigned long long)mine, (unsigned long long)theirs);
        if (g_session && g_session->IsHost() && GetConfig().autoResync) RequestResync("desync");
    }
    void OnReject(const std::string& reason) override { Log("REJECTED " + reason); CoopCommands::ShowMessage("Not allowed: " + reason); }
    void OnPlayerJoined(int slot, const std::string& name) override
    {
        Logf("JOIN slot=%d name=%s", slot, name.c_str());
        CoopOverlay::Post(name + " joined as player " + std::to_string(slot + 1));
        if (g_session && g_session->IsHost() && g_inLockstep) RequestResync("late join");
    }
    void OnPlayerLeft(int slot) override
    {
        Logf("LEAVE slot=%d", slot);
        CoopOverlay::Post(slot == 0 ? "Lost the connection to the host" : "Player " + std::to_string(slot + 1) + " left");
    }
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
    if (!PatchMenuPause())
    {
        Log("ERROR: CommandGui::OnLoop code differs from FTL 1.6.13 Linux amd64; co-op is off.");
        g_cfg.enabled = false;
        return;
    }
    if (!CoopRng::SilAvailable())
    {
        Log("ERROR: random32 state not found; this FTL build is not supported. Co-op is off.");
        g_cfg.enabled = false;
        return;
    }
    {
        void* global = dlsym(RTLD_DEFAULT, "rand");
        Logf("rand interposition: global lookup %s our rand (%p vs %p)", global == (void*)&rand ? "==" : "!=", global, (void*)&rand);
    }
    Logf("layout check: ShipManager jump_timer=%zu (DWARF 1544) fuel_count=%zu (1552) bJumping=%zu (1652) lastJumpReady=%zu (2257)",
         offsetof(ShipManager, jump_timer), offsetof(ShipManager, fuel_count), offsetof(ShipManager, bJumping), offsetof(ShipManager, lastJumpReady));
    coop::Settings s;
    s.delay = c.delay; s.window = c.window; s.hashEvery = c.hashEvery; s.maxPlayers = 8;
    s.buildId = std::string(__DATE__) + " " + __TIME__;
    if (!Env("FTL_COOP_BUILD_ID").empty()) s.buildId = Env("FTL_COOP_BUILD_ID");
    g_transport.reset(new coop::TcpTransport());
    // The session installs the transport callbacks, so it must exist before Connect (which says hello).
    g_session.reset(new coop::Session(g_transport.get(), &g_adapter, s, c.host, c.name));
    g_session->SetAllowLateJoin(true);
    g_start.mode = c.mode;
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
    else if (g_startStage == StartStage::LoadToMenu)
    {
        // Back to the main menu first (the game auto-saves when its menu opens over a game; wait for that).
        if (!menu.bOpen) { menu.Open(); g_startWait = 0; return; }
        if (g_startWait < 3) return;
        g_startStage = StartStage::LoadFile; g_startWait = 0;
    }
    else if (g_startStage == StartStage::LoadFile)
    {
        // Same as the main menu's Continue: TutorialManager::Stop, WorldManager::LoadGame, MainMenu::Close.
        std::string path = FileHelper::getSaveFile();
        if (!WriteFile(path, g_start.save)) { Log("ERROR: cannot write " + path); g_startStage = StartStage::None; return; }
        CoopRng::Own(true);
        CoopRng::SetState(g_start.seed);
        CoopRng::SetSil(g_start.seed);
        SetSeed(g_start.seed);
        G_->GetTutorialManager()->Stop();
        G_->GetWorld()->LoadGame(path);
        menu.Close();
        Log("save loaded: " + std::to_string(g_start.save.size()) + " bytes");
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
        for (size_t i = 0; i < g_start.owners.size(); ++i) SetCrewOwner((int)i, g_start.owners[i]);
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
        coop::Bytes blob; std::string err;
        if (GetConfig().startContinue && MakeLoadBlob(blob, &err)) g_session->Start(blob);
        else
        {
            if (GetConfig().startContinue) Log("no save to continue (" + err + "), starting a new run");
            g_session->Start(MakeStartBlob());
        }
    }
}

void RequestResync(const char* why)
{
    if (!g_session || !g_session->IsHost() || !g_inLockstep) return;
    static clock_t last = 0;
    clock_t now = clock();
    if (last && (double)(now - last) / CLOCKS_PER_SEC < 2.0 && strcmp(why, "late join") != 0) return; // no resync storms
    last = now;
    G_->GetWorld()->SaveGame();
    coop::Bytes blob; std::string err;
    if (!MakeLoadBlob(blob, &err)) { Log(std::string("ERROR: resync failed: ") + err); return; }
    ++g_resyncs;
    Logf("RESYNC #%u (%s): everyone reloads the host's save", g_resyncs, why);
    CoopOverlay::Post(std::string("Resyncing everyone from the host's save (") + why + ")");
    g_session->Restart(blob);
}

Mode CurrentMode() { return g_start.mode; }
void SetCurrentMode(Mode m) { g_start.mode = m; }

void RequestManualStart()
{
    if (g_session && g_session->IsHost() && !g_session->Started() && G_->GetCApp()->menu.bOpen)
        g_session->Start(MakeStartBlob());
}

static void Quit(const char* why)
{
    if (g_quitRequested) return;
    g_quitRequested = true;
    Logf("SUMMARY frames=%u desyncs=%u stalls=%u players=%d catchup=%u resyncs=%u reason=%s", g_session ? g_session->Frame() : 0,
         g_session ? g_session->DesyncCount() : 0, g_session ? g_session->StallFrames() : 0,
         g_session ? g_session->PlayerCount() : 0, g_catchupFrames, g_resyncs, why);
    Logf("RNG foreign-thread rand calls: %u [%s] random32: %u", CoopRng::ForeignCalls(), CoopRng::ForeignCallers().c_str(), CoopRng::g_r32Foreign);
    G_->GetCApp()->OnRequestExit();
}

// Called from the CApp::OnLoop hook. Returns how many game frames to run now (0 = stall).
int BeforeLoop()
{
    static bool inited = false;
    if (!inited) { inited = true; CoopRng::MarkMainThread(); InitSession(); }
    if (!Active() || !g_session) return 1;
    g_session->Poll();
    static int loops = 0;
    if (!g_session->Started() && (++loops % 120) == 1)
    {
        CApp* app = G_->GetCApp();
        Logf("status: loop=%d menu.bOpen=%d title=%d builder=%d world=%p started=%d players=%d", loops, (int)app->menu.bOpen,
             (int)app->menu.IsTitle(), (int)app->menu.shipBuilder.bOpen, (void*)app->world,
             app->world ? (int)app->world->bStartedGame : -1, g_session->PlayerCount());
    }
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
    SetSharedMenuPause();
    CoopBot::BeforeFrame(g_session->Frame());
    return true;
}

void RunFrameEnd()
{
    G_->GetCFPS()->SpeedFactor = FIXED_SPEED; // in case something in the frame recomputed it
    g_session->EndFrame();
    uint32_t f = g_session->Frame();
    static bool testResyncDone = false;
    if (GetConfig().testResyncAt && !testResyncDone && g_session->IsHost() && f == GetConfig().testResyncAt)
    {
        testResyncDone = true;
        RequestResync("test");
        return;
    }
    // Tests: total frames over all epochs (a resync starts frame numbers again).
    static uint32_t framesBefore = 0, lastEpoch = 1;
    if (g_session->Epoch() != lastEpoch) { lastEpoch = g_session->Epoch(); framesBefore = g_totalFramesAtRestart; }
    // Tests: FTL_COOP_EXIT_EPOCH=N counts only frames of epoch N and later (late-join tests).
    static uint32_t exitEpoch = getenv("FTL_COOP_EXIT_EPOCH") ? (uint32_t)atoi(getenv("FTL_COOP_EXIT_EPOCH")) : 0;
    if (exitEpoch)
    {
        if (g_session->Epoch() >= exitEpoch && GetConfig().exitAfterFrames && f >= GetConfig().exitAfterFrames) Quit("exit_after reached");
    }
    else if (GetConfig().exitAfterFrames && framesBefore + f >= GetConfig().exitAfterFrames) Quit("exit_after reached");
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
    // Test switch: a guest skips K loops out of every 97 (a hitch), so it falls behind and must catch up.
    static int hitch = getenv("FTL_COOP_TEST_HITCH") ? atoi(getenv("FTL_COOP_TEST_HITCH")) : 0;
    static int hitchLoop = 0;
    if (hitch > 0 && !CoopGame::GetSession()->IsHost() && (++hitchLoop % 97) < hitch) return;
    // Lockstep: the host is the clock and runs one frame per loop. A guest runs the next frame when its bundle
    // is here, and up to 3 extra frames when it has fallen behind (more bundles waiting than the input delay).
    for (int i = 0; i < 4; ++i)
    {
        if (i > 0 && (CoopGame::GetSession()->IsHost() || CoopGame::GetSession()->ReadyAhead() <= CoopGame::GetConfig().delay + 1)) break;
        if (!RunFrameBegin()) break;
        if (i > 0) ++CoopGame::g_catchupFrames;
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
    if (CoopGame::Active() && key == SDLK_F10) { CoopGame::RequestResync("F10"); return; }
    if (CoopGame::Active() && key == SDLK_F11 && CoopGame::GetSession() && CoopGame::GetSession()->IsHost())
    {
        CoopGame::SubmitLocal(CoopCommands::SetMode(CoopGame::CurrentMode() == CoopGame::Mode::OwnCrew ? CoopGame::Mode::FreeForAll : CoopGame::Mode::OwnCrew));
        return;
    }
    super(key);
}

// RNG trace for random32 (Hyperspace also hooks it; this only counts callers).
HOOK_GLOBAL_PRIORITY(random32, 9000, () -> unsigned int)
{
    LOG_HOOK("HOOK_GLOBAL_PRIORITY -> random32 -> Begin (CoopGame.cpp)\n")
    if (CoopRng::Owned())
    {
        ++CoopRng::g_r32Calls;
        if (!CoopRng::IsMainThread()) ++CoopRng::g_r32Foreign;
    }
    if (CoopRng::Tracing()) CoopRng::TraceCall(1, __builtin_return_address(0));
    return super();
}

// Animation::Update draws random32 for cosmetic choices (random flicker frame, random frame sound). It is called from
// both simulation and UI code, and how often depends on rendering, so a copy that catches up (several frames
// without a render) drew a different number of values (seen with FTL_COOP_TEST_HITCH, 2026-11-06).
// Keep these draws out of the shared stream.
HOOK_METHOD_PRIORITY(Animation, Update, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Animation::Update -> Begin (CoopGame.cpp)\n")
    if (!CoopGame::InLockstep()) { super(); return; }
    uint64_t s = CoopRng::GetSil();
    super();
    CoopRng::SetSil(s);
}

// Particle effects (fire extinguishers, crew effects) draw random32 in ParticleEmitter::Birth; how many particles are
// born depends on rendering as well (seen in chaos runs, 2026-11-06). Cosmetic: keep it out of the shared stream.
HOOK_METHOD_PRIORITY(ParticleEmitter, OnUpdate, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ParticleEmitter::OnUpdate -> Begin (CoopGame.cpp)\n")
    if (!CoopGame::InLockstep()) { super(); return; }
    uint64_t s = CoopRng::GetSil();
    super();
    CoopRng::SetSil(s);
}

// Background nebula clouds draw random32 in SpaceManager::OnLoopNebulas a varying number of times (seen with the
// fight bot, seed 32, 2026-11-06). The clouds are visual; a nebula's gameplay effect comes from the location.
HOOK_METHOD_PRIORITY(SpaceManager, OnLoopNebulas, 9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::OnLoopNebulas -> Begin (CoopGame.cpp)\n")
    if (!CoopGame::InLockstep()) { super(); return; }
    uint64_t s = CoopRng::GetSil();
    super();
    CoopRng::SetSil(s);
}
