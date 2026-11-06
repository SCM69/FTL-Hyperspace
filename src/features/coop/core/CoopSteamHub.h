#pragma once
// FTL co-op tests: a stand-in for Steam that several game copies on one PC share (one PC can only sign in one Steam
// account). Each copy connects over loopback TCP to tests/coop/fakesteam_hub.py, which keeps the lobbies, the
// "playing" status shown to friends, and passes packets between copies, holding a stranger's packets until they are
// accepted like Steam does. Used only when FTL_COOP_FAKE_STEAM=<host:port> is set.

#include "CoopSteamNet.h"
#include <deque>

namespace coop {

class HubSteam : public SteamApi
{
public:
    HubSteam(uint64_t me, const std::string& name) : m_me(me), m_name(name) {}
    bool Connect(const std::string& host, int port, std::string* err);
    void Pump();   // reads hub messages; called from every query

    uint64_t Me() override { return m_me; }
    std::string Name(uint64_t user) override;
    uint64_t CreateLobby(int maxMembers) override;
    uint64_t JoinLobby(uint64_t lobby) override;
    int CallResult(uint64_t call, bool isCreate, uint64_t* lobby) override;
    void LeaveLobby(uint64_t lobby) override;
    std::vector<uint64_t> Members(uint64_t lobby) override;
    uint64_t Owner(uint64_t lobby) override;
    void SetData(uint64_t lobby, const std::string& key, const std::string& value) override;
    std::string GetData(uint64_t lobby, const std::string& key) override;
    std::vector<FriendLobby> FriendLobbies() override;
    bool Send(uint64_t user, const uint8_t* data, uint32_t size) override;
    bool Receive(Bytes* out, uint64_t* from) override;
    void Accept(uint64_t user) override;
    void Close(uint64_t user) override;

private:
    struct Lobby { uint64_t owner = 0; std::vector<uint64_t> members; std::map<std::string, std::string> data; };
    struct Done { bool ok; uint64_t lobby; };
    void Out(const Writer& w) { m_t.Send(0, w.b); m_t.Poll(); }
    TcpTransport m_t;
    uint64_t m_me;
    std::string m_name;
    uint64_t m_nextCall = 1;
    std::map<uint64_t, Lobby> m_lobbies;
    std::map<uint64_t, Done> m_done;
    std::vector<FriendLobby> m_playing;
    std::map<uint64_t, std::string> m_names;
    std::deque<std::pair<uint64_t, Bytes>> m_packets;
};

} // namespace coop
