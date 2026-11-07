// Test for the Steam transport (CoopSteamNet) with an in-process fake Steam. No game, no Steam client.
// Build: g++ -std=c++11 -O1 -Isrc/features/coop/core tests/coop/steamnet_test.cpp src/features/coop/core/*.cpp
// The fake follows the Steam rules the transport relies on (see playbook/networking-steam-spacewar.md):
//  - lobby create/join finish after a few polls; the owner is the creator; the next member becomes owner when the
//    owner leaves;
//  - the first packets from a user you have not accepted (and never sent to) are held until you accept them;
//  - packets over 1 MB fail; sends can fail at random ("buffer full") and must be retried;
//  - a new member can show up in the member list later than their first packet (switch).

#include "CoopLockstep.h"
#include "CoopSteamNet.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

using namespace coop;

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++g_fail; } else printf("ok: %s\n", msg); } while (0)

static double g_clock = 0;
static double FakeNow() { return g_clock; }

struct FakeWorld
{
    struct Lobby { uint64_t owner; std::vector<uint64_t> members; std::map<std::string, std::string> data; int max; };
    struct Packet { uint64_t from; Bytes data; };
    struct Call { int readyAt; bool create; uint64_t lobby; bool ok; uint64_t user; };
    std::map<uint64_t, Lobby> lobbies;
    std::map<uint64_t, std::deque<Packet>> inbox;          // by receiver
    std::map<uint64_t, std::deque<Packet>> held;           // receiver -> held packets from strangers
    std::map<uint64_t, std::set<uint64_t>> accepted;       // receiver -> users whose packets get through
    std::map<uint64_t, std::string> names;
    std::map<uint64_t, uint64_t> playing;                  // user -> lobby shown in their Steam status
    std::map<uint64_t, Call> calls;
    std::map<uint64_t, std::map<uint64_t, int>> visibleAt; // lobby -> member -> tick they show up in the list
    uint64_t nextLobby = 0x0186000000000100ULL, nextCall = 1;
    int tick = 0;
    int sendFailPercent = 0, memberLag = 0;
    uint64_t bytesSent = 0, sendFails = 0, bigRefused = 0;
};

class FakeSteam : public SteamApi
{
public:
    FakeSteam(FakeWorld* w, uint64_t me, const std::string& name) : m_w(w), m_me(me) { w->names[me] = name; }
    uint64_t Me() override { return m_me; }
    std::string Name(uint64_t u) override { return m_w->names.count(u) ? m_w->names[u] : "?"; }
    uint64_t CreateLobby(int max) override
    {
        uint64_t id = m_w->nextCall++;
        m_w->calls[id] = FakeWorld::Call{ m_w->tick + 3, true, 0, true, m_me };
        m_w->calls[id].lobby = 0; (void)max; m_max = max;
        return id;
    }
    uint64_t JoinLobby(uint64_t lobby) override
    {
        uint64_t id = m_w->nextCall++;
        m_w->calls[id] = FakeWorld::Call{ m_w->tick + 3, false, lobby, true, m_me };
        return id;
    }
    int CallResult(uint64_t call, bool isCreate, uint64_t* lobby) override
    {
        auto it = m_w->calls.find(call);
        if (it == m_w->calls.end() || it->second.create != isCreate) return -1;
        if (m_w->tick < it->second.readyAt) return 0;
        FakeWorld::Call c = it->second; m_w->calls.erase(it);
        if (c.create)
        {
            uint64_t id = m_w->nextLobby++;
            m_w->lobbies[id] = FakeWorld::Lobby{ m_me, { m_me }, {}, m_max };
            m_w->playing[m_me] = id;
            *lobby = id; return 1;
        }
        *lobby = c.lobby;
        auto l = m_w->lobbies.find(c.lobby);
        if (l == m_w->lobbies.end() || (int)l->second.members.size() >= l->second.max) return -1;
        l->second.members.push_back(m_me);
        m_w->visibleAt[c.lobby][m_me] = m_w->tick + m_w->memberLag;
        return 1;
    }
    void LeaveLobby(uint64_t lobby) override
    {
        auto l = m_w->lobbies.find(lobby);
        if (l == m_w->lobbies.end()) return;
        auto& m = l->second.members;
        for (size_t i = 0; i < m.size(); ++i) if (m[i] == m_me) { m.erase(m.begin() + i); break; }
        if (m_w->playing.count(m_me) && m_w->playing[m_me] == lobby) m_w->playing.erase(m_me);
        if (m.empty()) m_w->lobbies.erase(l);
        else if (l->second.owner == m_me) l->second.owner = m[0];
    }
    std::vector<uint64_t> Members(uint64_t lobby) override
    {
        std::vector<uint64_t> v;
        auto l = m_w->lobbies.find(lobby);
        if (l == m_w->lobbies.end()) return v;
        for (uint64_t u : l->second.members)
            if (u == m_me || m_w->visibleAt[lobby][u] <= m_w->tick) v.push_back(u);
        return v;
    }
    uint64_t Owner(uint64_t lobby) override { auto l = m_w->lobbies.find(lobby); return l == m_w->lobbies.end() ? 0 : l->second.owner; }
    void SetData(uint64_t lobby, const std::string& k, const std::string& v) override
    {
        auto l = m_w->lobbies.find(lobby);
        if (l != m_w->lobbies.end() && l->second.owner == m_me) l->second.data[k] = v;
    }
    std::string GetData(uint64_t lobby, const std::string& k) override
    {
        auto l = m_w->lobbies.find(lobby);
        if (l == m_w->lobbies.end() || !l->second.data.count(k)) return "";
        return l->second.data[k];
    }
    std::vector<FriendLobby> FriendLobbies() override
    {
        std::vector<FriendLobby> v;
        for (auto& kv : m_w->playing) if (kv.first != m_me) v.push_back(FriendLobby{ kv.first, kv.second, Name(kv.first) });
        return v;
    }
    bool Send(uint64_t user, const uint8_t* d, uint32_t n) override
    {
        if (n > 1024u * 1024u) { ++m_w->bigRefused; return false; }
        if (m_w->sendFailPercent && rand() % 100 < m_w->sendFailPercent) { ++m_w->sendFails; return false; }
        m_w->accepted[m_me].insert(user);   // sending to someone accepts their packets
        FakeWorld::Packet p{ m_me, Bytes(d, d + n) };
        if (m_w->accepted[user].count(m_me)) m_w->inbox[user].push_back(p);
        else m_w->held[user].push_back(p);
        m_w->bytesSent += n;
        return true;
    }
    bool Receive(Bytes* out, uint64_t* from) override
    {
        auto& q = m_w->inbox[m_me];
        if (q.empty()) return false;
        *out = q.front().data; *from = q.front().from; q.pop_front();
        return true;
    }
    void Accept(uint64_t user) override
    {
        m_w->accepted[m_me].insert(user);
        auto& h = m_w->held[m_me];
        for (auto it = h.begin(); it != h.end();)
            if (it->from == user) { m_w->inbox[m_me].push_back(*it); it = h.erase(it); } else ++it;
    }
    void Close(uint64_t user) override { m_w->accepted[m_me].erase(user); }

private:
    FakeWorld* m_w;
    uint64_t m_me;
    int m_max = 8;
};

struct ToyWorld : Adapter
{
    uint64_t rng = 1; int32_t pos[8] = {}; int left = 0, joined = 0, rejects = 0; std::string tag; Bytes lastBlob;
    void Apply(int, const Bytes& cmd) override { if (cmd.size() >= 2) pos[cmd[0] % 8] += (int8_t)cmd[1]; }
    void Step() { for (auto& p : pos) { rng = rng * 6364136223846793005ULL + 1; p += (int)((rng >> 60) & 1); } }
    uint64_t Hash() override { Hasher h; h.val(rng); for (auto p : pos) h.val(p); return h.h; }
    void OnStart(const Bytes& b) override { lastBlob = b; Reader r(b); rng = r.u64(); for (auto& p : pos) p = 0; }
    void OnPlayerLeft(int) override { ++left; }
    void OnPlayerJoined(int, const std::string&) override { ++joined; }
    void OnReject(const std::string&) override { ++rejects; }
    void OnLog(const std::string& s) override { if (getenv("COOP_TEST_VERBOSE")) printf("[%s] %s\n", tag.c_str(), s.c_str()); }
};

struct Player
{
    std::unique_ptr<FakeSteam> api;
    std::unique_ptr<SteamTransport> t;
    std::unique_ptr<ToyWorld> w;
    std::unique_ptr<Session> s;
    std::vector<std::string> log;
};

static Player* MakePlayer(FakeWorld& fw, uint64_t id, const std::string& name, bool host, const std::string& build)
{
    Player* p = new Player();
    p->api.reset(new FakeSteam(&fw, id, name));
    p->t.reset(new SteamTransport(p->api.get()));
    p->t->SetNow(FakeNow);
    p->t->onLog = [p, name](const std::string& s) { p->log.push_back(s); if (getenv("COOP_TEST_VERBOSE")) printf("[%s net] %s\n", name.c_str(), s.c_str()); };
    p->w.reset(new ToyWorld()); p->w->tag = name;
    Settings st; st.buildId = build; st.delay = 3; st.window = 10; st.hashEvery = 10; st.timeoutMs = 1000;
    p->s.reset(new Session(p->t.get(), p->w.get(), st, host, name));
    p->s->SetAllowLateJoin(true);
    return p;
}

static bool LogHas(Player& p, const std::string& what)
{
    for (auto& l : p.log) if (l.find(what) != std::string::npos) return true;
    return false;
}

static void PollAll(FakeWorld& fw, std::vector<std::unique_ptr<Player>>& ps, int ticks)
{
    for (int i = 0; i < ticks; ++i) { ++fw.tick; g_clock += 0.05; for (auto& p : ps) if (p) p->s->Poll(); }
}

static void RunFrames(FakeWorld& fw, std::vector<std::unique_ptr<Player>>& ps, uint32_t until)
{
    for (int it = 0; it < (int)until * 30; ++it)
    {
        ++fw.tick; g_clock += 0.02;
        bool done = true;
        for (auto& p : ps)
        {
            if (!p) continue;
            p->s->Poll();
            if (p->s->Frame() >= until) continue;
            done = false;
            if (rand() % 4 == 0) continue;
            if (p->s->Started() && rand() % 5 == 0) { Bytes m; m.push_back((uint8_t)(rand() % 8)); m.push_back(1); p->s->Submit(m); }
            if (p->s->BeginFrame()) { p->w->Step(); p->s->EndFrame(); }
        }
        if (done) break;
    }
    PollAll(fw, ps, 50);
}

static Bytes Blob(uint64_t seed, size_t extra)
{
    Writer w; w.u64(seed);
    for (size_t i = 0; i < extra; ++i) w.u8((uint8_t)(i * 31 + 7));
    return w.b;
}

int main()
{
    // 1. Eight players find the host through their friends list, play 600 frames; then the host restarts everyone
    //    from a 3 MB "save" (larger than one Steam packet). Sends fail at random 20% of the time.
    {
        FakeWorld fw; fw.sendFailPercent = 20;
        std::vector<std::unique_ptr<Player>> ps;
        ps.emplace_back(MakePlayer(fw, 1001, "Host", true, "B1"));
        ps[0]->t->Host(8, { { "build", "B1" } });
        for (int i = 1; i < 8; ++i) { ps.emplace_back(MakePlayer(fw, 1001 + i, "Guest" + std::to_string(i), false, "B1")); ps[i]->t->Join(0, "", "B1"); }
        PollAll(fw, ps, 400);
        bool allIn = ps[0]->s->PlayerCount() == 8;
        for (int i = 1; i < 8; ++i) allIn = allIn && ps[i]->s->Slot() > 0;
        CHECK(allIn, "8 players: everyone found the host's lobby through the friends list and was welcomed");
        ps[0]->s->Start(Blob(42, 0));
        RunFrames(fw, ps, 600);
        bool reached = true; for (auto& p : ps) reached = reached && p->s->Frame() >= 600;
        char msg[200];
        snprintf(msg, sizeof(msg), "8 players over Steam: all reached frame 600, desyncs %u, %llu failed sends retried",
                 ps[0]->s->DesyncCount(), (unsigned long long)fw.sendFails);
        CHECK(reached && ps[0]->s->DesyncCount() == 0 && fw.sendFails > 0, msg);
        Bytes big = Blob(43, 3u * 1024u * 1024u);
        ps[0]->s->Restart(big);
        RunFrames(fw, ps, 300);
        bool same = true; reached = true;
        for (auto& p : ps) { same = same && p->w->lastBlob == big; reached = reached && p->s->Frame() >= 300; }
        snprintf(msg, sizeof(msg), "3 MB restart blob arrived intact on all 8 (no packet over 1 MB: %llu refused), frame 300, desyncs %u",
                 (unsigned long long)fw.bigRefused, ps[0]->s->DesyncCount());
        CHECK(same && reached && fw.bigRefused == 0 && ps[0]->s->DesyncCount() == 0, msg);

        // A guest leaves the lobby: the host drops them.
        ps[3]->s.reset(); ps[3]->t.reset(); ps[3].reset();   // the transport's destructor leaves the lobby
        PollAll(fw, ps, 40);
        CHECK(ps[0]->s->PlayerCount() == 7 && ps[0]->w->left == 1, "a guest who leaves the lobby is dropped by the host (7 players left)");
        // The host leaves: guests notice.
        ps[0]->s.reset(); ps[0]->t.reset(); ps[0].reset();
        PollAll(fw, ps, 40);
        int noticed = 0; for (auto& p : ps) if (p && p->w->left > 0 && LogHas(*p, "host left")) ++noticed;
        CHECK(noticed == 6, "when the host leaves, all 6 remaining guests see the host leave");
    }

    // 2. Choosing a lobby: two friends host (one a different build), one friend is in a non-co-op lobby. A guest with
    //    a name filter joins the right one; a guest without a filter skips the wrong build and the non-co-op lobby.
    {
        FakeWorld fw;
        std::vector<std::unique_ptr<Player>> ps;
        ps.emplace_back(MakePlayer(fw, 2001, "Alice", true, "OLD"));  ps[0]->t->Host(8, { { "build", "OLD" } });
        ps.emplace_back(MakePlayer(fw, 2002, "Bob", true, "B1"));     ps[1]->t->Host(8, { { "build", "B1" } });
        PollAll(fw, ps, 10);
        // Carol is in some other lobby of the game (no "ftlcoop" key).
        FakeSteam carol(&fw, 2000, "Carol");   // lowest id: the fake lists her first
        uint64_t c = carol.CreateLobby(4); fw.tick += 5; uint64_t carolLobby = 0; carol.CallResult(c, true, &carolLobby);
        ps.emplace_back(MakePlayer(fw, 2004, "Dave", false, "B1"));   ps[2]->t->Join(0, "", "B1");
        ps.emplace_back(MakePlayer(fw, 2005, "Erin", false, "B1"));   ps[3]->t->Join(0, "Bob", "B1");
        PollAll(fw, ps, 600);
        CHECK(ps[3]->s->Slot() > 0 && ps[1]->s->PlayerCount() == 3, "guest with name filter joined Bob's lobby");
        CHECK(ps[2]->s->Slot() > 0 && ps[0]->s->PlayerCount() == 1 && LogHas(*ps[2], "different co-op build") &&
              LogHas(*ps[2], "not an FTL co-op game"),
              "guest without filter skipped the other build and the non-co-op lobby, joined the matching host");
        CHECK(carolLobby != 0, "(fake: Carol's lobby existed)");
    }

    // 3. Steam shows a new member later than their first packets arrive (member list lag), and holds packets from
    //    strangers until accepted: the hello still gets through and the guest is welcomed.
    {
        FakeWorld fw; fw.memberLag = 30;
        std::vector<std::unique_ptr<Player>> ps;
        ps.emplace_back(MakePlayer(fw, 3001, "Host", true, "B1")); ps[0]->t->Host(8, { { "build", "B1" } });
        PollAll(fw, ps, 10);
        ps.emplace_back(MakePlayer(fw, 3002, "G", false, "B1")); ps[1]->t->Join(0, "", "B1");
        PollAll(fw, ps, 200);
        CHECK(ps[1]->s->Slot() == 1 && ps[0]->s->PlayerCount() == 2, "member list lag + held stranger packets: guest still welcomed");
        // Late join while running, then play on.
        ps[0]->s->Start(Blob(5, 100));
        RunFrames(fw, ps, 200);
        ps.emplace_back(MakePlayer(fw, 3003, "Late", false, "B1")); ps[2]->t->Join(0, "", "B1");
        PollAll(fw, ps, 200);
        ps[0]->s->Restart(Blob(6, 5000));
        RunFrames(fw, ps, 300);
        bool ok = ps[0]->s->PlayerCount() == 3; for (auto& p : ps) ok = ok && p->s->Frame() >= 300;
        CHECK(ok && ps[0]->s->DesyncCount() == 0, "late joiner over Steam: 3 players reach frame 300 after the restart, no desync");
    }

    // 4. A guest given a lobby id joins it directly; a full lobby fails and is retried, not a crash.
    {
        FakeWorld fw;
        std::vector<std::unique_ptr<Player>> ps;
        ps.emplace_back(MakePlayer(fw, 4001, "Host", true, "B1")); ps[0]->t->Host(2, { { "build", "B1" } });
        PollAll(fw, ps, 10);
        uint64_t lobby = ps[0]->t->Lobby();
        fw.playing.clear();   // nobody shows a lobby in their status: only the id works
        ps.emplace_back(MakePlayer(fw, 4002, "G1", false, "B1")); ps[1]->t->Join(lobby, "", "B1");
        ps.emplace_back(MakePlayer(fw, 4003, "G2", false, "B1"));
        PollAll(fw, ps, 100);
        ps[2]->t->Join(lobby, "", "B1");
        PollAll(fw, ps, 200);
        CHECK(lobby && ps[1]->s->Slot() == 1, "joining by lobby id works without the friends list");
        CHECK(ps[2]->s->Slot() == -1 && LogHas(*ps[2], "joining failed"), "a full lobby: the extra guest keeps waiting (join fails, retried)");
    }

    // 5. A guest's game freezes but Steam keeps it in the lobby: the host drops it after the timeout and does not take it
    //    back while it stays; after it leaves and joins again, it is a player again.
    {
        FakeWorld fw;
        std::vector<std::unique_ptr<Player>> ps;
        ps.emplace_back(MakePlayer(fw, 5001, "Host", true, "B1")); ps[0]->t->Host(8, { { "build", "B1" } });
        PollAll(fw, ps, 10);
        for (int i = 1; i <= 2; ++i) { ps.emplace_back(MakePlayer(fw, 5001 + i, "G" + std::to_string(i), false, "B1")); ps[i]->t->Join(0, "", "B1"); }
        PollAll(fw, ps, 100);
        Settings st; (void)st;
        ps[0]->s->Start(Blob(8, 0));
        RunFrames(fw, ps, 100);
        // Freeze G2 (never polled) for 1.5 s of real time with a 1 s timeout on the host.
        std::unique_ptr<Player> frozen; frozen.swap(ps[2]);
        for (int i = 0; i < 300; ++i) { ++fw.tick; g_clock += 0.005; for (auto& p : ps) if (p) p->s->Poll(); usleep(5000); }
        bool dropped = ps[0]->s->TimeoutDrops() == 1 && ps[0]->s->PlayerCount() == 2;
        CHECK(dropped, "Steam: a frozen guest still in the lobby is dropped after the timeout");
        PollAll(fw, ps, 50);
        CHECK(ps[0]->s->PlayerCount() == 2, "Steam: the dropped guest is not taken back while it stays in the lobby");
        // It wakes up, notices the host's silence (its own timeout), leaves the lobby, and joins again as a new player.
        frozen.swap(ps[2]);
        for (int i = 0; i < 300 && ps[2]->w->left == 0; ++i) { PollAll(fw, ps, 1); usleep(5000); }
        CHECK(ps[2]->w->left == 1 && LogHas(*ps[2], "left the host's lobby"), "Steam: the woken guest sees the host as gone and leaves the lobby");
        ps[2]->t->Join(0, "", "B1");
        PollAll(fw, ps, 200);
        CHECK(ps[0]->s->PlayerCount() == 3 && ps[2]->s->Slot() > 0, "Steam: after leaving and joining again it is a player again");
    }

    printf(g_fail ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
