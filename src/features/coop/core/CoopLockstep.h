#pragma once
// FTL co-op: deterministic lockstep session. Game-independent, C++11.
//
// Model: every copy runs the full game. The host is the clock and the judge:
//  - Players submit commands (opaque bytes). Guests send them to the host.
//  - The host filters each command through Adapter::Allowed (the ship-sharing mode),
//    then schedules all accepted commands of a frame in a bundle for frame f + delay,
//    and sends the bundle to every guest.
//  - A copy may run frame f only when it holds bundle f. It applies the bundle's commands
//    in bundle order, then runs one game frame with a fixed time step.
//  - Every copy reports a state hash every hashEvery frames; the host compares them.
//  - The host does not run more than `window` frames ahead of the slowest guest.

#include "CoopNet.h"
#include <map>
#include <deque>
#include <set>

namespace coop {

enum MsgType : uint8_t
{
    MSG_HELLO = 1,   // guest -> host: protocol version, build id, player name
    MSG_WELCOME = 2, // host -> guest: slot, settings blob
    MSG_CMD = 3,     // guest -> host: command bytes
    MSG_BUNDLE = 4,  // host -> guest: frame, [slot, command]...
    MSG_DONE = 5,    // guest -> host: last finished frame (+ hash when it is a hash frame)
    MSG_REJECT = 6,  // host -> guest: a command was not allowed, with reason
    MSG_START = 7,   // host -> guest: start lockstep at frame 0 with this start blob (seed, save...)
    MSG_DESYNC = 8,  // host -> guest: hashes differed at frame
    MSG_CHAT = 9,    // either way: text (also used for status lines)
    MSG_PING = 10,   // host -> guest: f64 host time (ms)
    MSG_PONG = 11,   // guest -> host: the same f64, sent back at once
};

static const uint32_t PROTOCOL_VERSION = 2;

struct Command
{
    uint8_t slot;
    Bytes data;
};

class Adapter
{
public:
    virtual ~Adapter() {}
    // Host only: may `slot` issue this command now? Fill reason when not.
    virtual bool Allowed(int slot, const Bytes& cmd, std::string* reason) { (void)slot; (void)cmd; (void)reason; return true; }
    // Every copy: apply one command, at the start of a frame, in bundle order.
    virtual void Apply(int slot, const Bytes& cmd) = 0;
    // Every copy: hash of the shared simulation state after a frame.
    virtual uint64_t Hash() = 0;
    // Optional notifications.
    virtual void OnLog(const std::string& line) { (void)line; }
    virtual void OnStart(const Bytes& startBlob) { (void)startBlob; }
    virtual void OnDesync(uint32_t frame, int slot, uint64_t mine, uint64_t theirs) { (void)frame; (void)slot; (void)mine; (void)theirs; }
    virtual void OnReject(const std::string& reason) { (void)reason; }
    virtual void OnPlayerJoined(int slot, const std::string& name) { (void)slot; (void)name; }
    virtual void OnPlayerLeft(int slot) { (void)slot; }
};

struct Settings
{
    uint32_t delay = 4;       // input delay in frames
    uint32_t window = 20;     // max frames the host may lead the slowest guest
    uint32_t hashEvery = 30;  // frames between hash checks
    int maxPlayers = 8;       // including the host
    std::string buildId;      // guests with a different build are refused
    uint32_t timeoutMs = 20000; // drop a player (guest: the host) silent this long; 0 = never. The host pings twice a second.
};

class Session
{
public:
    Session(Transport* t, Adapter* a, const Settings& s, bool isHost, const std::string& name);

    bool IsHost() const { return m_isHost; }
    int Slot() const { return m_slot; }               // host is slot 0; -1 until welcomed
    bool Started() const { return m_started; }
    uint32_t Frame() const { return m_frame; }        // next frame to run
    uint32_t Epoch() const { return m_epoch; }
    int PlayerCount() const;
    std::vector<int> Slots() const;
    std::string PlayerName(int slot) const;

    // Host: begin lockstep for everyone with this blob (seed, mode, save file...).
    void Start(const Bytes& startBlob);
    // Host: start over from frame 0 for everyone with a new blob (resync from a save, after a late join).
    void Restart(const Bytes& startBlob);
    // Host: let players join while a game runs (the game then restarts everyone from a save).
    void SetAllowLateJoin(bool on) { m_allowLateJoin = on; }
    // Queue a local command (sent to host, or queued directly on the host).
    void Submit(const Bytes& cmd);
    // Poll the network. Call every game loop iteration.
    void Poll();
    // True when frame Frame() may run now; then the bundle has been applied via Adapter::Apply.
    bool BeginFrame();
    // After the game frame ran: hash/ack bookkeeping, advances Frame().
    void EndFrame();

    // Diagnostics
    uint32_t StallFrames() const { return m_stalls; }
    // Host: lowest frame every guest has finished (or Frame() with no guests).
    uint32_t MinGuestDone() const;
    // Host: measured round trip (ms, smoothed) and the input delay / window it chose.
    double RttMs() const { return m_rttMs; }
    uint32_t CurrentDelay() const { return m_s.delay; }
    uint32_t CurrentWindow() const { return m_s.window; }
    // Host: adapt delay and window to the slowest guest's round trip (on by default).
    void SetAdaptive(bool on) { m_adaptive = on; }
    // Consecutive bundles already here, starting at Frame() (guests use it to catch up).
    uint32_t ReadyAhead() const { uint32_t n = 0; while (m_bundles.count(m_frame + n)) ++n; return n; }
    uint32_t DesyncCount() const { return m_desyncs; }
    uint32_t TimeoutDrops() const { return m_timeoutDrops; }
    void SendChat(const std::string& text);

private:
    void OnConnect(int peer);
    void OnDisconnect(int peer);
    void OnMessage(int peer, const Bytes& msg);
    void HostScheduleUpTo(uint32_t frame);
    void HostAcceptCommand(int slot, const Bytes& cmd);
    void HostCompare(uint32_t frame);
    void Log(const std::string& s) { if (m_adapter) m_adapter->OnLog(s); }

    struct Peer { int peerId; int slot; std::string name; bool welcomed; uint32_t doneFrame; bool hasDone; double heardMs; };

    Transport* m_t;
    Adapter* m_adapter;
    Settings m_s;
    bool m_isHost;
    std::string m_name;
    int m_slot;
    bool m_started;
    uint32_t m_frame;
    uint32_t m_scheduledUpTo;   // host: bundles exist for frames < this
    std::map<uint32_t, std::vector<Command>> m_bundles;
    std::deque<Command> m_pending; // host: accepted commands waiting for a bundle
    std::map<int, Peer> m_peers;   // host: by transport peer id
    std::map<int, std::string> m_names; // everyone: slot -> name
    std::map<uint32_t, std::map<int, uint64_t>> m_hashes; // host: frame -> slot -> hash
    uint32_t m_stalls;
    uint32_t m_desyncs;
    uint32_t m_timeoutDrops = 0;
    bool m_allowLateJoin = false;
    uint32_t m_epoch = 0;          // increases with every Start/Restart; stale messages are dropped
    bool m_adaptive = true;
    double m_rttMs = 0;
    double m_lastPing = 0;
    double m_hostHeardMs = 0;       // guest: last message from the host (0 = not connected)
    void CheckTimeouts();
    std::map<int, double> m_peerRtt;         // host: slot -> smoothed round trip
    void Adapt();
    void ResetFrames();
};

// FNV-1a 64 helpers for building state hashes.
struct Hasher
{
    uint64_t h = 1469598103934665603ULL;
    void add(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; } }
    template <class T> void val(const T& v) { add(&v, sizeof(v)); }
};

} // namespace coop
