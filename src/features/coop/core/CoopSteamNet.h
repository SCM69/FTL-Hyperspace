#pragma once
// FTL co-op: transport over Steam (friends-only lobby + Steam P2P packets, relayed by Steam when needed).
// Game-independent, C++11. The Steam calls go through SteamApi so tests can run with a fake Steam.
//
// Host: creates a friends-only lobby with key "ftlcoop" and the build id. Every lobby member other than the host
// becomes a peer (1..). Guest: finds a friend whose Steam status shows a lobby of this game (or a given lobby id),
// joins it, checks the build id, and talks to the lobby owner (peer 0). Players leave by leaving the lobby (Steam
// also removes them when their game exits).
// Messages use the same length-prefixed frames as the TCP transport, sent as reliable, ordered P2P packets.

#include "CoopNet.h"
#include <map>
#include <set>

namespace coop {

class SteamApi
{
public:
    virtual ~SteamApi() {}
    virtual uint64_t Me() = 0;
    virtual std::string Name(uint64_t user) = 0;
    // Asynchronous calls return a handle (0 = the call failed at once). CallResult: 0 still running, 1 done (lobby
    // id in *lobby), -1 failed.
    virtual uint64_t CreateLobby(int maxMembers) = 0;
    virtual uint64_t JoinLobby(uint64_t lobby) = 0;
    virtual int CallResult(uint64_t call, bool isCreate, uint64_t* lobby) = 0;
    virtual void LeaveLobby(uint64_t lobby) = 0;
    virtual std::vector<uint64_t> Members(uint64_t lobby) = 0;
    virtual uint64_t Owner(uint64_t lobby) = 0;
    virtual void SetData(uint64_t lobby, const std::string& key, const std::string& value) = 0;
    virtual std::string GetData(uint64_t lobby, const std::string& key) = 0;
    struct FriendLobby { uint64_t user; uint64_t lobby; std::string name; };
    // Friends who are playing this game and are in a lobby.
    virtual std::vector<FriendLobby> FriendLobbies() = 0;
    // Reliable, ordered packet to a user. False: not sent now (try again later).
    virtual bool Send(uint64_t user, const uint8_t* data, uint32_t size) = 0;
    // Next received packet, if any.
    virtual bool Receive(Bytes* out, uint64_t* from) = 0;
    virtual void Accept(uint64_t user) = 0;
    virtual void Close(uint64_t user) = 0;
};

// Length-prefixed frames (shared with the TCP transport).
void AppendFrame(Bytes& out, const Bytes& msg);
// Takes every complete frame from the front of `in`. False when the stream is broken (frame too large).
bool PopFrames(Bytes& in, const std::function<void(const Bytes&)>& onFrame);

class SteamTransport : public Transport
{
public:
    explicit SteamTransport(SteamApi* api) : m_api(api) {}
    ~SteamTransport();
    // Host: open a lobby. `data` is stored in the lobby (the build id under "build").
    void Host(int maxMembers, const std::map<std::string, std::string>& data);
    // Guest: join `lobby` if nonzero, else a friend's lobby whose friend name contains `friendName` (any friend when
    // empty) and whose "build" equals `build`.
    void Join(uint64_t lobby, const std::string& friendName, const std::string& build);

    void Poll() override;
    void Send(int peer, const Bytes& msg) override;
    void Close(int peer) override;

    bool Connected() const { return !m_conns.empty(); }
    uint64_t Lobby() const { return m_lobby; }
    // One line for the screen: what the transport is doing.
    std::string Status() const { return m_status; }
    // Log lines (searching, joined, wrong build...).
    std::function<void(const std::string&)> onLog;

    static const uint32_t kChunk = 128 * 1024;   // Steam's reliable packets may be up to 1 MB
    void SetNow(double (*now)()) { m_now = now; }

private:
    struct Conn { uint64_t user; int id; Bytes in; Bytes out; };
    enum class State { Idle, Creating, Open, Searching, Joining, InLobby, Failed };
    void Log(const std::string& s) { if (onLog) onLog(s); }
    void HostPoll();
    void GuestPoll();
    void SyncMembers();
    void Flush(Conn& c);
    void Drop(size_t index);
    Conn* ByUser(uint64_t user);
    double Now() const;

    SteamApi* m_api;
    bool m_isHost = false;
    State m_state = State::Idle;
    uint64_t m_call = 0;
    uint64_t m_lobby = 0;
    uint64_t m_owner = 0;
    int m_maxMembers = 8;
    std::map<std::string, std::string> m_data;
    uint64_t m_wantLobby = 0;
    std::string m_friendName, m_build;
    std::map<uint64_t, double> m_skipUntil;   // guest: lobbies that did not fit, retried later
    double m_nextSearch = 0, m_nextMembers = 0;
    std::vector<Conn> m_conns;
    std::map<uint64_t, Bytes> m_early;        // host: data from users not yet seen as members
    int m_nextId = 1;
    std::string m_status;
    std::string m_lastSearchNote;
    double (*m_now)() = nullptr;
};

} // namespace coop
