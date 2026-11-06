// FTL co-op: ownership of the game's random number generators.
// - libc rand()/srand(): FTL calls them in ~100 functions (map, rewards, AI, particles). Hyperspace is preloaded,
//   so these definitions win over libc's. Outside co-op they forward to libc unchanged.
// - random32(): FTL's own LCG; its state is a global in the game binary (see SilState).
// Debug aids: per-caller trace (FTL_COOP_RNGTRACE=1), call counters, calls from other threads.
#include "CoopRng.h"
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace CoopRng {

static bool g_own = false;
static uint64_t g_state = 1;
static thread_local bool t_main = false;

uint32_t g_randCalls = 0, g_randCallsAll = 0, g_r32Calls = 0, g_r32Foreign = 0;
static uint32_t g_foreign = 0;
static void* g_foreignCaller[8];

void Own(bool on) { g_own = on; }
bool Owned() { return g_own; }
uint64_t GetState() { return g_state; }
void SetState(uint64_t s) { g_state = s; }
void MarkMainThread() { t_main = true; }
bool IsMainThread() { return t_main; }
uint32_t ForeignCalls() { return g_foreign; }

static std::string Where(void* addr)
{
    // FTL is non-PIE (stable addresses); code in shared libraries is printed as a library-relative offset.
    char buf[96];
    uintptr_t a = (uintptr_t)addr;
    Dl_info info;
    if (a >= 0x1000000 && dladdr(addr, &info) && info.dli_fname)
    {
        const char* base = strrchr(info.dli_fname, '/');
        snprintf(buf, sizeof(buf), "%s+%lx", base ? base + 1 : info.dli_fname, (unsigned long)(a - (uintptr_t)info.dli_fbase));
    }
    else snprintf(buf, sizeof(buf), "%lx", (unsigned long)a);
    return buf;
}

std::string ForeignCallers()
{
    std::string out;
    for (uint32_t i = 0; i < g_foreign && i < 8; ++i) out += Where(g_foreignCaller[i]) + " ";
    return out;
}

// Trace: a small fixed table, so tracing costs little and changes timing as little as possible.
struct TraceSlot { int which; void* addr; int count; };
static TraceSlot g_trace[512];
static int g_traceN = 0;
static int g_tracing = -1;

bool Tracing()
{
    if (g_tracing < 0) { const char* e = getenv("FTL_COOP_RNGTRACE"); g_tracing = (e && e[0] == '1') ? 1 : 0; }
    return g_tracing == 1;
}

void TraceCall(int which, void* caller)
{
    if (!Tracing() || !g_own) return;
    for (int i = 0; i < g_traceN; ++i)
        if (g_trace[i].addr == caller && g_trace[i].which == which) { ++g_trace[i].count; return; }
    if (g_traceN < 512) { g_trace[g_traceN].which = which; g_trace[g_traceN].addr = caller; g_trace[g_traceN].count = 1; ++g_traceN; }
}

std::string TraceDump()
{
    std::string out;
    char buf[32];
    for (int i = 0; i < g_traceN; ++i)
    {
        snprintf(buf, sizeof(buf), "%d:", g_trace[i].which);
        out += buf + Where(g_trace[i].addr);
        snprintf(buf, sizeof(buf), ":%d ", g_trace[i].count);
        out += buf;
    }
    g_traceN = 0;
    return out;
}

// random32's state lives in a global of the game binary (non-PIE, fixed address in 1.6.13 Linux amd64).
static uint64_t* SilState()
{
    static uint64_t* p = nullptr;
    static bool checked = false;
    if (!checked)
    {
        checked = true;
        // random32 (0x6615c0) is detoured by Hyperspace (CrewNames.cpp), so read the state address from
        // srandom32 (0x6617b0), which is not hooked: mov %edi,%eax ; mov %rax,state(%rip) = 89 f8 48 89 05 rel32.
        // The state is _sil_random_state at 0xa35ff0.
        const unsigned char* c = (const unsigned char*)0x6617b0;
        if (c[0] == 0x89 && c[1] == 0xf8 && c[2] == 0x48 && c[3] == 0x89 && c[4] == 0x05)
        {
            int32_t rel; memcpy(&rel, c + 5, 4);
            uint64_t* q = (uint64_t*)(c + 9 + rel);
            if (q == (uint64_t*)0xa35ff0) p = q;
        }
    }
    return p;
}

bool SilAvailable() { return SilState() != nullptr; }
uint64_t GetSil() { uint64_t* p = SilState(); return p ? *p : 0; }
void SetSil(uint64_t v) { uint64_t* p = SilState(); if (p) *p = v; }

} // namespace CoopRng

extern "C" {

__attribute__((visibility("default"))) int rand(void)
{
    ++CoopRng::g_randCallsAll;
    if (!CoopRng::g_own)
    {
        static int (*real)(void) = (int (*)(void))dlsym(RTLD_NEXT, "rand");
        return real ? real() : 0;
    }
    ++CoopRng::g_randCalls;
    if (!CoopRng::t_main)
    {
        if (CoopRng::g_foreign < 8) CoopRng::g_foreignCaller[CoopRng::g_foreign] = __builtin_return_address(0);
        ++CoopRng::g_foreign;
    }
    if (CoopRng::Tracing()) CoopRng::TraceCall(0, __builtin_return_address(0));
    CoopRng::g_state = CoopRng::g_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((CoopRng::g_state >> 33) & 0x7fffffff);
}

__attribute__((visibility("default"))) void srand(unsigned int seed)
{
    if (!CoopRng::g_own)
    {
        static void (*real)(unsigned int) = (void (*)(unsigned int))dlsym(RTLD_NEXT, "srand");
        if (real) real(seed);
        return;
    }
    CoopRng::g_state = seed;
}

}
