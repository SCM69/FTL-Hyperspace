#pragma once
// FTL co-op: game side. Off unless the FTL_COOP environment variable (or coop.cfg) asks for it.
#include "core/CoopLockstep.h"
#include <string>

class CrewMember;

namespace CoopGame {

enum class Mode : uint8_t { FreeForAll = 0, OwnCrew = 1 };

struct Config
{
    bool enabled = false;
    bool host = false;
    std::string addr = "127.0.0.1";
    int port = 27890;
    std::string name = "player";
    int waitPlayers = 0;          // host: start automatically when this many players are in (0 = manual, F9)
    Mode mode = Mode::FreeForAll;
    std::string userDir;          // save folder override (tests)
    std::string logPath;          // co-op log file
    std::string bot;              // test bot script name
    unsigned seed = 0;            // 0 = random
    int shipType = 0, shipVariant = 0;
    uint32_t exitAfterFrames = 0; // tests: quit after this many lockstep frames
    uint32_t delay = 4, window = 12, hashEvery = 30;
    bool autoResync = true;       // host reloads everyone from its save after a desync
    bool startContinue = false;   // host starts from its continue save instead of a new run
    uint32_t testResyncAt = 0;    // tests: host resyncs once at this lockstep frame
};

const Config& GetConfig();
bool Active();                    // co-op configured
bool InLockstep();                // lockstep running in a game
bool Applying();                  // true while a networked command is applied
coop::Session* GetSession();
int LocalSlot();
void Log(const std::string& line);
void RequestResync(const char* why);

// Commands from the local player. Returns true when the action was taken over by co-op
// (the caller must not perform it locally).
bool SubmitLocal(const coop::Bytes& cmd);

// Ownership (own-crew mode)
int CrewOwner(CrewMember* crew);   // slot, or -1 for nobody
void SetCrewOwner(int crewIndex, int slot);

} // namespace CoopGame
