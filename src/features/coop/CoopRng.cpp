// FTL co-op: ownership of the game's random number generators.
// - libc rand()/srand(): FTL calls them in ~100 functions (map, rewards, AI, particles). Hyperspace is preloaded,
//   so these definitions win over libc's. Outside co-op they forward to libc unchanged.
// - random32(): FTL's own LCG; its state is a global in the game binary (see SilState).
// Debug aids: per-caller trace (FTL_COOP_RNGTRACE=1), call counters, calls from other threads.
#include "CoopRng.h"
#ifdef _WIN32
#include <windows.h>
#include "../../zhl/SigScan.h"
#else
#include <dlfcn.h>
#endif
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
#ifdef _WIN32
    HMODULE mod = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod)
        && mod != GetModuleHandleA(nullptr) && GetModuleFileNameA(mod, path, sizeof(path)))
    {
        const char* base = strrchr(path, '\\');
        snprintf(buf, sizeof(buf), "%s+%lx", base ? base + 1 : path, (unsigned long)(a - (uintptr_t)mod));
    }
    else snprintf(buf, sizeof(buf), "%lx", (unsigned long)a);
    return buf;
#else
    Dl_info info;
    if (a >= 0x1000000 && dladdr(addr, &info) && info.dli_fname)
    {
        const char* base = strrchr(info.dli_fname, '/');
        snprintf(buf, sizeof(buf), "%s+%lx", base ? base + 1 : info.dli_fname, (unsigned long)(a - (uintptr_t)info.dli_fbase));
    }
    else snprintf(buf, sizeof(buf), "%lx", (unsigned long)a);
    return buf;
#endif
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

// random32's state lives in a global of the game binary.
static uint64_t* SilState()
{
    static uint64_t* p = nullptr;
    static bool checked = false;
    if (!checked)
    {
        checked = true;
#ifdef _WIN32
        // Windows 1.6.9 srandom32 (same pattern as Hyperspace's win32 Global.zhl):
        //   push ebp ; mov dword [state+4], 0 ; mov ebp,esp ; and esp,-8 ; mov eax,[ebp+8] ; mov [state], eax
        SigScan scan("55c705????????00000000" "89e583e4f88b4508a3????????");
        if (scan.Scan())
        {
            const unsigned char* c = (const unsigned char*)scan.GetAddress();
            uint32_t hi, lo; memcpy(&hi, c + 3, 4); memcpy(&lo, c + 20, 4); // 55 | c7 05 hi imm32 | 89 e5 | 83 e4 f8 | 8b 45 08 | a3 lo
            if (hi == lo + 4) p = (uint64_t*)(uintptr_t)lo;
        }
#else
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
#endif
    }
    return p;
}

bool SilAvailable() { return SilState() != nullptr; }
uint64_t GetSil() { uint64_t* p = SilState(); return p ? *p : 0; }
void SetSil(uint64_t v) { uint64_t* p = SilState(); if (p) *p = v; }

// ---------------------------------------------------------------- rand/srand
// Linux: Hyperspace is preloaded, so the exported rand/srand below win over libc's for the game binary.
// Windows: FTLGame.exe imports rand/srand from the C runtime DLL; InstallHooks points those import slots at
// CoopRand/CoopSrand (the DLL's own calls bind to the definitions below at link time). msvcrt's RAND_MAX is 0x7fff
// and the game's code is compiled against it, so the Windows results are 15 bits.

static int (*g_realRand)(void) = nullptr;
static void (*g_realSrand)(unsigned int) = nullptr;

#ifdef _WIN32
static const uint32_t kRandMask = 0x7fff;
#else
static const uint32_t kRandMask = 0x7fffffff;
#endif

static int RealRand()
{
#ifdef _WIN32
    if (!g_realRand) g_realRand = (int (*)(void))(void*)GetProcAddress(GetModuleHandleA("msvcrt.dll"), "rand");
#else
    if (!g_realRand) g_realRand = (int (*)(void))dlsym(RTLD_NEXT, "rand");
#endif
    return g_realRand ? g_realRand() : 0;
}

static void RealSrand(unsigned int seed)
{
#ifdef _WIN32
    if (!g_realSrand) g_realSrand = (void (*)(unsigned int))(void*)GetProcAddress(GetModuleHandleA("msvcrt.dll"), "srand");
#else
    if (!g_realSrand) g_realSrand = (void (*)(unsigned int))dlsym(RTLD_NEXT, "srand");
#endif
    if (g_realSrand) g_realSrand(seed);
}

static int __attribute__((noinline)) CoopRand(void* caller)
{
    ++g_randCallsAll;
    if (!g_own) return RealRand();
    ++g_randCalls;
    if (!t_main)
    {
        if (g_foreign < 8) g_foreignCaller[g_foreign] = caller;
        ++g_foreign;
    }
    if (Tracing()) TraceCall(0, caller);
    g_state = g_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((g_state >> 33) & kRandMask);
}

static void CoopSrand(unsigned int seed)
{
    if (!g_own) { RealSrand(seed); return; }
    g_state = seed;
}

#ifdef _WIN32
static int __cdecl IatRand(void) { return CoopRand(__builtin_return_address(0)); }
static void __cdecl IatSrand(unsigned int seed) { CoopSrand(seed); }

// Points every import of rand/srand in FTLGame.exe at IatRand/IatSrand. Returns how many slots were changed.
static int PatchImports()
{
    unsigned char* base = (unsigned char*)GetModuleHandleA(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int changed = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d)
    {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* slots = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            void* repl = nullptr;
            if (!strcmp(fn, "rand")) { g_realRand = (int (*)(void))(uintptr_t)slots->u1.Function; repl = (void*)&IatRand; }
            else if (!strcmp(fn, "srand")) { g_realSrand = (void (*)(unsigned int))(uintptr_t)slots->u1.Function; repl = (void*)&IatSrand; }
            if (!repl) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old)) continue;
            slots->u1.Function = (DWORD)(uintptr_t)repl;
            VirtualProtect(&slots->u1.Function, sizeof(DWORD), old, &old);
            ++changed;
        }
    }
    return changed;
}
#endif

int g_importsPatched = 0;

bool InstallHooks()
{
#ifdef _WIN32
    static bool done = false;
    if (!done) { done = true; g_realRand = nullptr; g_realSrand = nullptr; g_importsPatched = PatchImports(); }
    return g_importsPatched >= 2; // both imports found
#else
    return true; // symbol interposition
#endif
}

} // namespace CoopRng

extern "C" {

__attribute__((visibility("default"))) int rand(void)
{
    return CoopRng::CoopRand(__builtin_return_address(0));
}

__attribute__((visibility("default"))) void srand(unsigned int seed)
{
    CoopRng::CoopSrand(seed);
}

}
