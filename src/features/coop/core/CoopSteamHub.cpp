#include "CoopSteamHub.h"

namespace coop {

// Messages, both ways length-prefixed frames (TcpTransport).
enum : uint8_t
{
    HUB_HELLO = 1, HUB_CREATE = 2, HUB_JOIN = 3, HUB_LEAVE = 4, HUB_SETDATA = 5, HUB_SEND = 6, HUB_ACCEPT = 7, HUB_CLOSE = 8,
    HUB_CALLDONE = 20, HUB_LOBBY = 21, HUB_PLAYING = 22, HUB_PACKET = 23,
};

bool HubSteam::Connect(const std::string& host, int port, std::string* err)
{
    m_t.onMessage = [this](int, const Bytes& msg) {
        Reader r(msg);
        uint8_t op = r.u8();
        if (op == HUB_CALLDONE) { uint64_t call = r.u64(); bool ok = r.u8() != 0; uint64_t lobby = r.u64(); m_done[call] = Done{ ok, lobby }; }
        else if (op == HUB_LOBBY)
        {
            uint64_t id = r.u64(); Lobby l; l.owner = r.u64();
            uint32_t n = r.u32(); for (uint32_t i = 0; i < n && !r.bad; ++i) l.members.push_back(r.u64());
            uint32_t d = r.u32(); for (uint32_t i = 0; i < d && !r.bad; ++i) { std::string k = r.str(); l.data[k] = r.str(); }
            if (l.members.empty()) m_lobbies.erase(id); else m_lobbies[id] = l;
        }
        else if (op == HUB_PLAYING)
        {
            m_playing.clear();
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && !r.bad; ++i)
            {
                FriendLobby f; f.user = r.u64(); f.lobby = r.u64(); f.name = r.str();
                m_names[f.user] = f.name;
                if (f.user != m_me && f.lobby) m_playing.push_back(f);
            }
        }
        else if (op == HUB_PACKET) { uint64_t from = r.u64(); m_packets.push_back(std::make_pair(from, r.bytes())); }
    };
    if (!m_t.Connect(host, port, err)) return false;
    Writer w; w.u8(HUB_HELLO); w.u64(m_me); w.str(m_name); Out(w);
    return true;
}

void HubSteam::Pump() { m_t.Poll(); }

std::string HubSteam::Name(uint64_t user)
{
    if (user == m_me) return m_name;
    auto it = m_names.find(user);
    return it == m_names.end() ? std::to_string(user) : it->second;
}

uint64_t HubSteam::CreateLobby(int maxMembers)
{
    uint64_t call = m_nextCall++;
    Writer w; w.u8(HUB_CREATE); w.u64(call); w.u32((uint32_t)maxMembers); Out(w);
    return call;
}

uint64_t HubSteam::JoinLobby(uint64_t lobby)
{
    uint64_t call = m_nextCall++;
    Writer w; w.u8(HUB_JOIN); w.u64(call); w.u64(lobby); Out(w);
    return call;
}

int HubSteam::CallResult(uint64_t call, bool, uint64_t* lobby)
{
    Pump();
    auto it = m_done.find(call);
    if (it == m_done.end()) return 0;
    *lobby = it->second.lobby;
    int r = it->second.ok ? 1 : -1;
    m_done.erase(it);
    return r;
}

void HubSteam::LeaveLobby(uint64_t lobby) { Writer w; w.u8(HUB_LEAVE); w.u64(lobby); Out(w); m_lobbies.erase(lobby); }

std::vector<uint64_t> HubSteam::Members(uint64_t lobby)
{
    Pump();
    auto it = m_lobbies.find(lobby);
    return it == m_lobbies.end() ? std::vector<uint64_t>() : it->second.members;
}

uint64_t HubSteam::Owner(uint64_t lobby)
{
    Pump();
    auto it = m_lobbies.find(lobby);
    return it == m_lobbies.end() ? 0 : it->second.owner;
}

void HubSteam::SetData(uint64_t lobby, const std::string& k, const std::string& v)
{
    Writer w; w.u8(HUB_SETDATA); w.u64(lobby); w.str(k); w.str(v); Out(w);
    m_lobbies[lobby].data[k] = v;
}

std::string HubSteam::GetData(uint64_t lobby, const std::string& k)
{
    Pump();
    auto it = m_lobbies.find(lobby);
    if (it == m_lobbies.end()) return "";
    auto d = it->second.data.find(k);
    return d == it->second.data.end() ? "" : d->second;
}

std::vector<SteamApi::FriendLobby> HubSteam::FriendLobbies() { Pump(); return m_playing; }

bool HubSteam::Send(uint64_t user, const uint8_t* data, uint32_t size)
{
    if (size > 1024u * 1024u) return false;   // like Steam's reliable packets
    Writer w; w.u8(HUB_SEND); w.u64(user); w.bytes(Bytes(data, data + size)); m_t.Send(0, w.b);
    return true;
}

bool HubSteam::Receive(Bytes* out, uint64_t* from)
{
    Pump();
    if (m_packets.empty()) return false;
    *from = m_packets.front().first; out->swap(m_packets.front().second); m_packets.pop_front();
    return true;
}

void HubSteam::Accept(uint64_t user) { Writer w; w.u8(HUB_ACCEPT); w.u64(user); Out(w); }
void HubSteam::Close(uint64_t user) { Writer w; w.u8(HUB_CLOSE); w.u64(user); Out(w); }

} // namespace coop
