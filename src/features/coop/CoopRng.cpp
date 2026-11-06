// FTL co-op: ownership of the C library RNG.
// FTL calls libc rand()/srand() in ~100 functions (map, rewards, AI, particles). Hyperspace is preloaded,
// so these definitions win over libc's. Outside co-op they forward to libc unchanged.
#include "CoopRng.h"
#include <dlfcn.h>
#include <cstdint>
#include <cstring>

namespace CoopRng {

static bool g_own = false;
static uint64_t g_state = 1;

void Own(bool on) { g_own = on; }
bool Owned() { return g_own; }
uint64_t GetState() { return g_state; }
void SetState(uint64_t s) { g_state = s; }

// random32's state lives in a global of the game binary (non-PIE, fixed address in 1.6.13 Linux amd64).
static uint64_t* SilState()
{
    static uint64_t* p = nullptr;
    static bool checked = false;
    if (!checked)
    {
        checked = true;
        // Check the code of random32 (0x6615c0) before trusting the address of its state (0xa35ff0).
        static const unsigned char expect[] = { 0x48, 0xb8, 0x2d, 0x7f, 0x95, 0x4c, 0x2d, 0xf4, 0x51, 0x58 };
        void* fn = dlsym(RTLD_DEFAULT, "random32");
        if (fn == (void*)0x6615c0 && memcmp(fn, expect, sizeof(expect)) == 0) p = (uint64_t*)0xa35ff0;
        else if (fn && memcmp(fn, expect, sizeof(expect)) == 0)
        {
            // Same code elsewhere: read the rip-relative operand of `imul state(%rip)` at fn+10.
            const unsigned char* c = (const unsigned char*)fn;
            if (c[10] == 0x48 && c[11] == 0x0f && c[12] == 0xaf && c[13] == 0x05)
            {
                int32_t rel; memcpy(&rel, c + 14, 4);
                p = (uint64_t*)(c + 18 + rel);
            }
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
    if (!CoopRng::g_own)
    {
        static int (*real)(void) = (int (*)(void))dlsym(RTLD_NEXT, "rand");
        return real ? real() : 0;
    }
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
