#pragma once
#include <cstdint>

namespace CoopRng {
void Own(bool on);          // take over libc rand/srand (lockstep) or forward to libc
bool Owned();
uint64_t GetState();
void SetState(uint64_t s);
bool SilAvailable();        // random32 state found
uint64_t GetSil();
void SetSil(uint64_t v);
}
