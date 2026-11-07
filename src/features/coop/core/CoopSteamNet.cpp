#include "CoopSteamNet.h"

#include <algorithm>
#include <chrono>

namespace coop {

static const uint32_t MAX_MSG = 64u * 1024u * 1024u;

void AppendFrame(Bytes& out, const Bytes& msg)
{
    uint32_t n = (uint32_t)msg.size();
    uint8_t hdr[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
    out.insert(out.end(), hdr, hdr + 4);
    out.insert(out.end(), msg.begin(), msg.end());
}

bool PopFrames(Bytes& in, const std::function<void(const Bytes&)>& onFrame)
{
    size_t off = 0;
    bool ok = true;
    while (in.size() - off >= 4)
    {
        uint32_t n = in[off] | (in[off + 1] << 8) | (in[off + 2] << 16) | ((uint32_t)in[off + 3] << 24);
        if (n > MAX_MSG) { ok = false; break; }
        if (in.size() - off - 4 < n) break;
        Bytes msg(in.begin() + off + 4, in.begin() + off + 4 + n);
        off += 4 + n;
        onFrame(msg);
    }
    if (off) in.erase(in.begin(), in.begin() + off);
    return ok;
}

static double SteadyNow()
{
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

double SteamTransport::Now() const { return m_now ? m_now() : SteadyNow(); }

SteamTransport::~SteamTransport()
{
    for (auto& c : m_conns) m_api->Close(c.user);
    if (m_lobby) m_api->LeaveLobby(m_lobby);
}

void SteamTransport::Host(int maxMembers, const std::map<std::string, std::string>& data)
{
    m_isHost = true;
    m_maxMembers = maxMembers;
    m_data = data;
    m_data["ftlcoop"] = "1";
    m_call = m_api->CreateLobby(maxMembers);
    m_state = m_call ? State::Creating : State::Failed;
    m_status = m_call ? "Steam: creating a lobby..." : "Steam: could not create a lobby (is Steam running?)";
    Log(m_status);
}

void SteamTransport::Join(uint64_t lobby, const std::string& friendName, const std::string& build)
{
    m_isHost = false;
    m_wantLobby = lobby;
    m_friendName = friendName;
    m_build = build;
    m_state = State::Searching;
    m_nextSearch = 0;
    m_status = "Steam: looking for a friend hosting FTL co-op...";
}

SteamTransport::Conn* SteamTransport::ByUser(uint64_t user)
{
    for (auto& c : m_conns) if (c.user == user) return &c;
    return nullptr;
}

void SteamTransport::Drop(size_t index)
{
    int id = m_conns[index].id;
    m_api->Close(m_conns[index].user);
    m_conns.erase(m_conns.begin() + index);
    if (onDisconnect) onDisconnect(id);
}

void SteamTransport::Send(int peer, const Bytes& msg)
{
    for (auto& c : m_conns)
        if (c.id == peer) { AppendFrame(c.out, msg); Flush(c); return; }
}

void SteamTransport::Close(int peer)
{
    for (size_t i = 0; i < m_conns.size(); ++i)
        if (m_conns[i].id == peer)
        {
            Flush(m_conns[i]);
            if (m_isHost) m_closed.insert(m_conns[i].user);
            Drop(i);
            if (!m_isHost && m_lobby)
            {
                m_api->LeaveLobby(m_lobby);
                m_lobby = 0;
                m_state = State::Idle;
                m_status = "Steam: left the host's lobby";
                Log(m_status);
            }
            return;
        }
}

void SteamTransport::Flush(Conn& c)
{
    size_t off = 0;
    while (off < c.out.size())
    {
        uint32_t n = (uint32_t)std::min<size_t>(kChunk, c.out.size() - off);
        if (!m_api->Send(c.user, c.out.data() + off, n)) break;   // Steam's buffer is full: the rest goes next poll
        off += n;
    }
    if (off) c.out.erase(c.out.begin(), c.out.begin() + off);
}

// Host: every lobby member except us is a peer; members who left are dropped.
void SteamTransport::SyncMembers()
{
    std::vector<uint64_t> members = m_api->Members(m_lobby);
    uint64_t me = m_api->Me();
    for (size_t i = 0; i < m_conns.size();)
    {
        bool still = false;
        for (uint64_t m : members) if (m == m_conns[i].user) still = true;
        if (still) ++i;
        else { Log("Steam: " + m_api->Name(m_conns[i].user) + " left the lobby"); Drop(i); }
    }
    for (auto it = m_closed.begin(); it != m_closed.end();)
    {
        bool still = false;
        for (uint64_t m : members) if (m == *it) still = true;
        if (still) ++it; else it = m_closed.erase(it);   // left: may join again as a new player
    }
    for (uint64_t m : members)
    {
        if (m == me || ByUser(m) || m_closed.count(m)) continue;
        m_api->Accept(m);
        Conn c; c.user = m; c.id = m_nextId++;
        m_conns.push_back(c);
        Log("Steam: " + m_api->Name(m) + " joined the lobby (peer " + std::to_string(c.id) + ")");
        if (onConnect) onConnect(c.id);
        auto e = m_early.find(m);
        if (e != m_early.end()) { m_conns.back().in.swap(e->second); m_early.erase(e); }
    }
}

void SteamTransport::HostPoll()
{
    if (m_state == State::Creating)
    {
        uint64_t lobby = 0;
        int r = m_api->CallResult(m_call, true, &lobby);
        if (r == 0) return;
        if (r < 0 || !lobby) { m_state = State::Failed; m_status = "Steam: creating the lobby failed"; Log(m_status); return; }
        m_lobby = lobby;
        for (auto& kv : m_data) m_api->SetData(m_lobby, kv.first, kv.second);
        m_state = State::Open;
        m_status = "Steam lobby open: friends set role=guest and start FTL";
        Log("Steam: lobby " + std::to_string(m_lobby) + " open");
        // Read it back: says in the log whether Steam stored what we set (owner, members, data).
        std::vector<uint64_t> members = m_api->Members(m_lobby);
        Log("Steam: lobby check: owner is me " + std::to_string(m_api->Owner(m_lobby) == m_api->Me()) + ", members " +
            std::to_string(members.size()) + ", ftlcoop=" + m_api->GetData(m_lobby, "ftlcoop") + ", build=" + m_api->GetData(m_lobby, "build"));
    }
    if (m_state != State::Open) return;
    double now = Now();
    if (now >= m_nextMembers) { m_nextMembers = now + 0.25; SyncMembers(); }
}

void SteamTransport::GuestPoll()
{
    double now = Now();
    if (m_state == State::Searching && now >= m_nextSearch)
    {
        m_nextSearch = now + 2.0;
        uint64_t pick = 0;
        std::string who, note;
        if (m_wantLobby) { pick = m_wantLobby; who = "lobby " + std::to_string(pick); }
        else
        {
            for (auto& f : m_api->FriendLobbies())
            {
                if (!m_friendName.empty() && f.name.find(m_friendName) == std::string::npos) continue;
                auto skip = m_skipUntil.find(f.lobby);
                if (skip != m_skipUntil.end() && skip->second > now) { note = f.name + "'s game does not fit (see coop.log)"; continue; }
                pick = f.lobby; who = f.name;
                break;
            }
        }
        if (!pick)
        {
            m_status = note.empty() ? "Steam: no friend is hosting FTL co-op yet (waiting)" : "Steam: " + note;
            if (m_status != m_lastSearchNote) { Log(m_status); m_lastSearchNote = m_status; }
            return;
        }
        m_call = m_api->JoinLobby(pick);
        if (!m_call) { m_skipUntil[pick] = now + 10; return; }
        m_state = State::Joining;
        m_status = "Steam: joining " + who + "...";
        Log(m_status);
    }
    if (m_state == State::Joining)
    {
        uint64_t lobby = 0;
        int r = m_api->CallResult(m_call, false, &lobby);
        if (r == 0) return;
        if (r < 0 || !lobby)
        {
            Log("Steam: joining failed; trying again");
            if (lobby) m_skipUntil[lobby] = now + 10;
            m_state = State::Searching; m_nextSearch = now + 2;
            return;
        }
        std::string mark = m_api->GetData(lobby, "ftlcoop"), build = m_api->GetData(lobby, "build");
        if (mark != "1" || (!m_build.empty() && build != m_build))
        {
            Log(mark != "1" ? "Steam: that lobby is not an FTL co-op game; leaving it"
                            : "Steam: that host runs a different co-op build (" + build + ", mine " + m_build + "); leaving");
            m_api->LeaveLobby(lobby);
            m_skipUntil[lobby] = now + 30;
            m_state = State::Searching; m_nextSearch = now + 2;
            return;
        }
        m_lobby = lobby;
        m_owner = m_api->Owner(lobby);
        m_state = State::InLobby;
        Conn c; c.user = m_owner; c.id = 0;
        m_conns.push_back(c);
        m_status = "Steam: in " + m_api->Name(m_owner) + "'s lobby";
        Log(m_status);
        if (onConnect) onConnect(0);
    }
    if (m_state == State::InLobby && now >= m_nextMembers)
    {
        m_nextMembers = now + 0.5;
        // The host is gone when they left the lobby (Steam then makes someone else the owner).
        uint64_t owner = m_api->Owner(m_lobby);
        bool hostThere = false;
        for (uint64_t m : m_api->Members(m_lobby)) if (m == m_owner) hostThere = true;
        if (!hostThere || owner != m_owner)
        {
            Log("Steam: the host left the lobby");
            m_api->LeaveLobby(m_lobby);
            m_lobby = 0;
            m_state = State::Idle;
            m_status = "Steam: the host left";
            for (size_t i = m_conns.size(); i-- > 0;) Drop(i);
        }
    }
}

void SteamTransport::Poll()
{
    m_api->Tick();
    if (m_isHost) HostPoll(); else GuestPoll();
    // Incoming packets.
    Bytes pkt; uint64_t from = 0;
    while (m_api->Receive(&pkt, &from))
    {
        Conn* c = ByUser(from);
        if (!c && m_isHost && m_state == State::Open && !m_closed.count(from))
        {
            SyncMembers();   // they may have joined since the last look
            c = ByUser(from);
            if (!c)
            {
                Bytes& e = m_early[from];   // not a member (yet): keep a little, ignore the rest
                if (e.size() + pkt.size() <= 4u * 1024u * 1024u) e.insert(e.end(), pkt.begin(), pkt.end());
                continue;
            }
        }
        if (!c) continue;
        c->in.insert(c->in.end(), pkt.begin(), pkt.end());
    }
    for (size_t i = 0; i < m_conns.size(); ++i)
    {
        Flush(m_conns[i]);
        int id = m_conns[i].id;
        Bytes in; in.swap(m_conns[i].in);
        bool ok = PopFrames(in, [&](const Bytes& msg) { if (onMessage) onMessage(id, msg); });
        // A callback may have dropped connections; find ours again.
        Conn* c = nullptr;
        for (auto& k : m_conns) if (k.id == id) c = &k;
        if (!c) { i = (size_t)-1; continue; }   // restart; rare (only on disconnects)
        if (!in.empty()) c->in.insert(c->in.begin(), in.begin(), in.end());
        if (!ok) { Log("Steam: broken data from peer " + std::to_string(id)); for (size_t k = 0; k < m_conns.size(); ++k) if (m_conns[k].id == id) { Drop(k); break; } i = (size_t)-1; }
    }
}

} // namespace coop
