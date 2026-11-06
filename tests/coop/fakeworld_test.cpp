// Fake-world test for the FTL co-op lockstep core: no game, real loopback TCP.
// Build: g++ -std=c++11 -O1 -I../../src/features/coop/core fakeworld_test.cpp ../../src/features/coop/core/*.cpp
// Each "copy" runs a toy deterministic simulation (units with positions, an RNG, a pause flag).
// Checks: every copy reaches the target frame, all hash checks agree, the own-crew mode rejects
// commands for other players' units, and a deliberately broken copy (mutation) is caught as a desync.

#include "CoopLockstep.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace coop;

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++g_fail; } else printf("ok: %s\n", msg); } while (0)

struct ToyWorld : Adapter
{
    // Shared state
    uint64_t rng = 1;
    std::vector<int32_t> pos;   // 8 units
    std::vector<int32_t> goal;
    int32_t paused = 0;
    // Config
    int mode = 0;               // 0 free-for-all, 1 own-crew (unit u belongs to slot u % players)
    int players = 1;
    bool mutateApplyTwice = false; // mutation twin: apply commands twice on this copy
    // Stats
    int applied = 0, rejected = 0, desyncSeen = 0, rejectsReceived = 0;
    std::string tag;

    ToyWorld() : pos(8, 0), goal(8, 0) {}
    uint32_t rand32() { rng = rng * 0x5851f42d4c957f2dULL + 1; return (uint32_t)(rng >> 33); }

    bool Allowed(int slot, const Bytes& cmd, std::string* reason) override
    {
        if (cmd.size() < 2) { *reason = "bad size"; return false; }
        if (cmd[0] == 'P') return true; // pause toggle: anyone (setting in the real mod)
        int unit = cmd[1] % 8;
        if (mode == 1 && unit % players != slot) { *reason = "not your crew"; ++rejected; return false; }
        return true;
    }
    void Apply(int slot, const Bytes& cmd) override
    {
        (void)slot;
        int times = mutateApplyTwice ? 2 : 1;
        for (int t = 0; t < times; ++t)
        {
            if (cmd[0] == 'P') paused ^= 1;
            else if (cmd[0] == 'M') goal[cmd[1] % 8] += (int8_t)cmd[2];
        }
        ++applied;
    }
    void Step()
    {
        if (paused) return;
        for (size_t i = 0; i < pos.size(); ++i)
        {
            if (pos[i] < goal[i]) ++pos[i]; else if (pos[i] > goal[i]) --pos[i];
            if (rand32() % 97 == 0) goal[i] += (int)(rand32() % 5) - 2; // "AI"
        }
    }
    uint64_t Hash() override
    {
        Hasher h; h.val(rng); h.val(paused);
        for (auto v : pos) h.val(v);
        for (auto v : goal) h.val(v);
        return h.h;
    }
    void OnStart(const Bytes& blob) override
    {
        Reader r(blob); rng = r.u64(); mode = r.u8(); players = r.u8();
    }
    void OnDesync(uint32_t, int, uint64_t, uint64_t) override { ++desyncSeen; }
    void OnReject(const std::string&) override { ++rejectsReceived; }
    void OnLog(const std::string& line) override { if (getenv("COOP_TEST_VERBOSE")) printf("[%s] %s\n", tag.c_str(), line.c_str()); }
};

struct Copy
{
    std::unique_ptr<TcpTransport> t;
    std::unique_ptr<ToyWorld> w;
    std::unique_ptr<Session> s;
};

struct Result { bool allReached; uint32_t desyncs; int rejected; int rejectsReceived; uint32_t stalls; int players; };

static Result RunScenario(int nPlayers, int mode, uint32_t frames, unsigned seed, bool mutate)
{
    srand(seed);
    Settings st; st.buildId = "test"; st.maxPlayers = 8; st.delay = 3; st.window = 10; st.hashEvery = 15;
    std::vector<Copy> copies(nPlayers);
    copies[0].t.reset(new TcpTransport());
    std::string err;
    if (!copies[0].t->Listen(0, &err)) { printf("listen failed: %s\n", err.c_str()); exit(2); }
    int port = copies[0].t->ListenPort();
    for (int i = 0; i < nPlayers; ++i)
    {
        Copy& c = copies[i];
        if (i > 0) c.t.reset(new TcpTransport());
        c.w.reset(new ToyWorld());
        c.w->tag = "p" + std::to_string(i);
        c.s.reset(new Session(c.t.get(), c.w.get(), st, i == 0, "bot" + std::to_string(i)));
        if (i > 0)
        {
            if (!c.t->Connect("127.0.0.1", port, &err)) { printf("connect failed: %s\n", err.c_str()); exit(2); }
            if (mutate && i == nPlayers - 1) c.w->mutateApplyTwice = true;
        }
    }
    // Wait until everyone is welcomed.
    for (int it = 0; it < 2000; ++it)
    {
        for (auto& c : copies) c.s->Poll();
        bool all = copies[0].s->PlayerCount() == nPlayers;
        for (int i = 1; i < nPlayers; ++i) all = all && copies[i].s->Slot() > 0;
        if (all) break;
    }
    Writer blob; blob.u64(0xC0FFEEULL + seed); blob.u8((uint8_t)mode); blob.u8((uint8_t)nPlayers);
    copies[0].s->Start(blob.b);

    // Run: every iteration, each copy is polled; a random subset is "slow" and skips its frame.
    for (int iter = 0; iter < (int)frames * 20; ++iter)
    {
        bool allDone = true;
        for (int i = 0; i < nPlayers; ++i)
        {
            Copy& c = copies[i];
            c.s->Poll();
            if (c.s->Frame() >= frames) continue;
            allDone = false;
            if (rand() % 4 == 0) continue; // slow machine this tick
            if (c.s->Started() && rand() % 6 == 0)
            {
                Bytes cmd;
                if (rand() % 25 == 0) { cmd.push_back('P'); cmd.push_back(0); }
                else { cmd.push_back('M'); cmd.push_back((uint8_t)(rand() % 8)); cmd.push_back((uint8_t)(rand() % 7 - 3)); }
                c.s->Submit(cmd);
            }
            if (c.s->BeginFrame()) { c.w->Step(); c.s->EndFrame(); }
        }
        if (allDone) break;
    }
    // Drain the last DONE messages so the host compares the final hashes.
    for (int it = 0; it < 200; ++it) for (auto& c : copies) c.s->Poll();

    Result r; r.allReached = true; r.desyncs = copies[0].s->DesyncCount(); r.stalls = 0; r.rejectsReceived = 0;
    r.rejected = copies[0].w->rejected; r.players = copies[0].s->PlayerCount();
    for (auto& c : copies) { r.allReached = r.allReached && c.s->Frame() >= frames; r.stalls += c.s->StallFrames(); r.rejectsReceived += c.w->rejectsReceived; }
    return r;
}

int main()
{
    // 1. Two players, free-for-all.
    Result a = RunScenario(2, 0, 600, 1, false);
    CHECK(a.allReached, "2 players FFA: every copy reached frame 600");
    CHECK(a.desyncs == 0, "2 players FFA: no desync");
    CHECK(a.rejected == 0, "2 players FFA: nothing rejected");

    // 2. Eight players, own-crew.
    Result b = RunScenario(8, 1, 600, 2, false);
    CHECK(b.players == 8, "8 players own-crew: host sees 8 players");
    CHECK(b.allReached, "8 players own-crew: every copy reached frame 600");
    CHECK(b.desyncs == 0, "8 players own-crew: no desync");
    CHECK(b.rejected > 0, "8 players own-crew: commands for others' crew were rejected on the host");
    CHECK(b.rejectsReceived > 0, "8 players own-crew: guests were told about rejections");

    // 3. Many seeds, 4 players, both modes.
    int bad = 0;
    for (unsigned seed = 10; seed < 40; ++seed)
    {
        Result r = RunScenario(4, seed % 2, 300, seed, false);
        if (!r.allReached || r.desyncs) { printf("  seed %u failed (reached=%d desyncs=%u)\n", seed, r.allReached, r.desyncs); ++bad; }
    }
    CHECK(bad == 0, "30 seeds x 4 players: all reached, no desync");

    // 4. Mutation twin: one copy applies commands twice. The test must notice.
    Result m = RunScenario(3, 0, 600, 3, true);
    CHECK(m.desyncs > 0, "mutation twin (double apply on one guest) is detected as desync");

    // 5. Ninth player is refused.
    {
        Settings st; st.buildId = "test"; st.maxPlayers = 2;
        TcpTransport ht, g1, g2; ToyWorld hw, w1, w2; std::string err;
        ht.Listen(0, &err);
        Session hs(&ht, &hw, st, true, "host"), s1(&g1, &w1, st, false, "g1"), s2(&g2, &w2, st, false, "g2");
        g1.Connect("127.0.0.1", ht.ListenPort(), &err);
        for (int i = 0; i < 200; ++i) { hs.Poll(); s1.Poll(); }
        g2.Connect("127.0.0.1", ht.ListenPort(), &err);
        for (int i = 0; i < 200; ++i) { hs.Poll(); s1.Poll(); s2.Poll(); }
        CHECK(hs.PlayerCount() == 2 && s2.Slot() == -1 && w2.rejectsReceived == 1, "player over the limit is refused");
    }
    // 6. Different build is refused.
    {
        Settings hsS; hsS.buildId = "A"; Settings gS; gS.buildId = "B";
        TcpTransport ht, g1; ToyWorld hw, w1; std::string err;
        ht.Listen(0, &err);
        Session hs(&ht, &hw, hsS, true, "host"), s1(&g1, &w1, gS, false, "g1");
        g1.Connect("127.0.0.1", ht.ListenPort(), &err);
        for (int i = 0; i < 200; ++i) { hs.Poll(); s1.Poll(); }
        CHECK(hs.PlayerCount() == 1 && w1.rejectsReceived == 1, "guest with a different mod build is refused");
    }

    printf(g_fail ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
