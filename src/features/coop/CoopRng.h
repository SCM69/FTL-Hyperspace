#pragma once
#include <cstdint>
#include <string>

namespace CoopRng {
void Own(bool on);          // take over libc rand/srand (lockstep) or forward to libc
bool Owned();
uint64_t GetState();
void SetState(uint64_t s);
bool SilAvailable();        // random32 state found
uint64_t GetSil();
void SetSil(uint64_t v);
// Debug trace (FTL_COOP_RNGTRACE=1): count calls per caller address between hash frames.
void TraceCall(int which, void* caller);
std::string TraceDump();   // returns 'which:addr:count ...' sorted, and resets
bool Tracing();
void MarkMainThread();      // call on the game thread
uint32_t ForeignCalls();    // rand() calls from other threads while owned
std::string ForeignCallers();
extern uint32_t g_randCalls, g_randCallsAll, g_r32Calls, g_r32Foreign;
bool IsMainThread();
// Windows: redirect FTLGame.exe's rand/srand imports (false if they were not found). Linux: always true.
bool InstallHooks();
extern int g_importsPatched;
}
