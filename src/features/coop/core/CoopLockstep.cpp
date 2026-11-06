#include "CoopLockstep.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace coop {

Session::Session(Transport* t, Adapter* a, const Settings& s, bool isHost, const std::string& name)
    : m_t(t), m_adapter(a), m_s(s), m_isHost(isHost), m_name(name), m_slot(isHost ? 0 : -1),
      m_started(false), m_frame(0), m_scheduledUpTo(0), m_stalls(0), m_desyncs(0)
{
    m_t->onConnect = [this](int p) { OnConnect(p); };
    m_t->onDisconnect = [this](int p) { OnDisconnect(p); };
    m_t->onMessage = [this](int p, const Bytes& m) { OnMessage(p, m); };
    if (isHost) m_names[0] = name;
}

int Session::PlayerCount() const { return (int)m_names.size(); }

std::vector<int> Session::Slots() const
{
    std::vector<int> v;
    for (auto& kv : m_names) v.push_back(kv.first);
    return v;
}

std::string Session::PlayerName(int slot) const
{
    auto it = m_names.find(slot);
    return it == m_names.end() ? std::string() : it->second;
}

void Session::OnConnect(int peer)
{
    if (m_isHost)
    {
        Peer p; p.peerId = peer; p.slot = -1; p.welcomed = false; p.doneFrame = 0; p.hasDone = false;
        m_peers[peer] = p;
        Log("peer " + std::to_string(peer) + " connected");
    }
    else
    {
        Writer w; w.u8(MSG_HELLO); w.u32(PROTOCOL_VERSION); w.str(m_s.buildId); w.str(m_name);
        m_t->Send(0, w.b);
    }
}

void Session::OnDisconnect(int peer)
{
    if (m_isHost)
    {
        auto it = m_peers.find(peer);
        if (it == m_peers.end()) return;
        int slot = it->second.slot;
        m_peers.erase(it);
        if (slot >= 0)
        {
            m_names.erase(slot);
            Log("slot " + std::to_string(slot) + " left");
            if (m_adapter) m_adapter->OnPlayerLeft(slot);
            // Tell the others, so their player lists match.
            Writer w; w.u8(MSG_CHAT); w.i32(-1); w.str("left " + std::to_string(slot));
            for (auto& kv : m_peers) if (kv.second.welcomed) m_t->Send(kv.first, w.b);
        }
    }
    else
    {
        Log("host connection lost");
        if (m_adapter) m_adapter->OnPlayerLeft(0);
    }
}

void Session::Start(const Bytes& startBlob)
{
    if (!m_isHost || m_started) return;
    m_started = true;
    m_frame = 0;
    // Frames 0..delay-1 have no commands (nobody could have sent any for them).
    for (uint32_t f = 0; f < m_s.delay; ++f) m_bundles[f];
    m_scheduledUpTo = m_s.delay;
    ++m_epoch;
    Writer w; w.u8(MSG_START); w.u32(m_epoch); w.u32(m_s.delay); w.bytes(startBlob);
    for (auto& kv : m_peers) if (kv.second.welcomed) m_t->Send(kv.first, w.b);
    Log("lockstep started with " + std::to_string(PlayerCount()) + " players");
    if (m_adapter) m_adapter->OnStart(startBlob);
}

void Session::ResetFrames()
{
    m_frame = 0;
    m_bundles.clear();
    m_pending.clear();
    m_hashes.clear();
    for (auto& kv : m_peers) { kv.second.doneFrame = 0; kv.second.hasDone = false; }
}

void Session::Restart(const Bytes& startBlob)
{
    if (!m_isHost) return;
    m_started = false;
    ResetFrames();
    m_scheduledUpTo = 0;
    Start(startBlob);
}

void Session::Submit(const Bytes& cmd)
{
    if (m_isHost) HostAcceptCommand(0, cmd);
    else
    {
        Writer w; w.u8(MSG_CMD); w.u32(m_epoch); w.bytes(cmd);
        m_t->Send(0, w.b);
    }
}

void Session::HostAcceptCommand(int slot, const Bytes& cmd)
{
    std::string reason;
    if (m_adapter && !m_adapter->Allowed(slot, cmd, &reason))
    {
        Log("rejected command from slot " + std::to_string(slot) + ": " + reason);
        if (slot == 0) { if (m_adapter) m_adapter->OnReject(reason); return; }
        for (auto& kv : m_peers)
        {
            if (kv.second.slot == slot)
            {
                Writer w; w.u8(MSG_REJECT); w.str(reason);
                m_t->Send(kv.first, w.b);
            }
        }
        return;
    }
    Command c; c.slot = (uint8_t)slot; c.data = cmd;
    m_pending.push_back(c);
}

void Session::HostScheduleUpTo(uint32_t frame)
{
    while (m_scheduledUpTo <= frame)
    {
        uint32_t f = m_scheduledUpTo++;
        std::vector<Command>& b = m_bundles[f];
        while (!m_pending.empty()) { b.push_back(m_pending.front()); m_pending.pop_front(); }
        Writer w; w.u8(MSG_BUNDLE); w.u32(f); w.u16((uint16_t)b.size());
        for (auto& c : b) { w.u8(c.slot); w.bytes(c.data); }
        for (auto& kv : m_peers) if (kv.second.welcomed) m_t->Send(kv.first, w.b);
    }
}

void Session::Poll()
{
    m_t->Poll();
}

bool Session::BeginFrame()
{
    if (!m_started) return false;
    if (m_isHost)
    {
        // Do not lead the slowest guest by more than `window` frames.
        for (auto& kv : m_peers)
        {
            const Peer& p = kv.second;
            if (!p.welcomed || p.slot < 0) continue;
            uint32_t done = p.hasDone ? p.doneFrame + 1 : 0;
            if (m_frame >= done + m_s.window) { ++m_stalls; return false; }
        }
        HostScheduleUpTo(m_frame + m_s.delay);
    }
    auto it = m_bundles.find(m_frame);
    if (it == m_bundles.end()) { ++m_stalls; return false; }
    if (m_adapter) for (auto& c : it->second) m_adapter->Apply(c.slot, c.data);
    m_bundles.erase(it);
    return true;
}

void Session::EndFrame()
{
    uint32_t f = m_frame;
    bool hashFrame = m_s.hashEvery && (f % m_s.hashEvery) == 0;
    uint64_t h = hashFrame && m_adapter ? m_adapter->Hash() : 0;
    if (m_isHost)
    {
        if (hashFrame) { m_hashes[f][0] = h; HostCompare(f); }
    }
    else
    {
        Writer w; w.u8(MSG_DONE); w.u32(m_epoch); w.u32(f); w.u8(hashFrame ? 1 : 0); if (hashFrame) w.u64(h);
        m_t->Send(0, w.b);
    }
    ++m_frame;
}

void Session::HostCompare(uint32_t frame)
{
    auto it = m_hashes.find(frame);
    if (it == m_hashes.end()) return;
    auto mine = it->second.find(0);
    if (mine == it->second.end()) return;
    for (auto& kv : it->second)
    {
        if (kv.first == 0 || kv.second == mine->second) continue;
        ++m_desyncs;
        char buf[160];
        snprintf(buf, sizeof(buf), "DESYNC frame %u slot %d host %016llx guest %016llx", frame, kv.first,
                 (unsigned long long)mine->second, (unsigned long long)kv.second);
        Log(buf);
        if (m_adapter) m_adapter->OnDesync(frame, kv.first, mine->second, kv.second);
        Writer w; w.u8(MSG_DESYNC); w.u32(frame); w.i32(kv.first);
        for (auto& p : m_peers) if (p.second.welcomed) m_t->Send(p.first, w.b);
    }
    // Keep only recent frames.
    while (!m_hashes.empty() && m_hashes.begin()->first + m_s.window * 4 + m_s.hashEvery * 4 < frame)
        m_hashes.erase(m_hashes.begin());
}

void Session::SendChat(const std::string& text)
{
    Writer w; w.u8(MSG_CHAT); w.i32(m_slot); w.str(text);
    if (m_isHost) { for (auto& kv : m_peers) if (kv.second.welcomed) m_t->Send(kv.first, w.b); }
    else m_t->Send(0, w.b);
}

void Session::OnMessage(int peer, const Bytes& msg)
{
    Reader r(msg);
    uint8_t type = r.u8();
    if (m_isHost)
    {
        auto pit = m_peers.find(peer);
        if (pit == m_peers.end()) return;
        Peer& p = pit->second;
        if (type == MSG_HELLO)
        {
            uint32_t ver = r.u32(); std::string build = r.str(); std::string name = r.str();
            std::string refuse;
            if (ver != PROTOCOL_VERSION) refuse = "protocol version differs";
            else if (build != m_s.buildId) refuse = "mod build differs: host " + m_s.buildId + ", you " + build;
            else if (PlayerCount() >= m_s.maxPlayers) refuse = "session is full";
            else if (m_started && !m_allowLateJoin) refuse = "game already started";
            if (!refuse.empty())
            {
                Writer w; w.u8(MSG_REJECT); w.str(refuse);
                m_t->Send(peer, w.b);
                m_t->Close(peer);
                Log("refused peer " + std::to_string(peer) + ": " + refuse);
                return;
            }
            int slot = 1;
            while (m_names.count(slot)) ++slot;
            p.slot = slot; p.name = name; p.welcomed = true;
            m_names[slot] = name;
            Writer w; w.u8(MSG_WELCOME); w.u8((uint8_t)slot);
            w.u16((uint16_t)m_names.size());
            for (auto& kv : m_names) { w.u8((uint8_t)kv.first); w.str(kv.second); }
            m_t->Send(peer, w.b);
            // Tell the others.
            Writer j; j.u8(MSG_CHAT); j.i32(-1); j.str("joined " + std::to_string(slot) + " " + name);
            for (auto& kv : m_peers) if (kv.second.welcomed && kv.first != peer) m_t->Send(kv.first, j.b);
            Log("slot " + std::to_string(slot) + " joined: " + name);
            if (m_adapter) m_adapter->OnPlayerJoined(slot, name);
        }
        else if (!p.welcomed) return;
        else if (type == MSG_CMD)
        {
            uint32_t epoch = r.u32();
            Bytes cmd = r.bytes();
            if (!r.bad && m_started && epoch == m_epoch) HostAcceptCommand(p.slot, cmd);
        }
        else if (type == MSG_DONE)
        {
            uint32_t epoch = r.u32();
            uint32_t f = r.u32(); uint8_t hasHash = r.u8();
            if (epoch != m_epoch) return; // from before a restart
            if (!p.hasDone || f > p.doneFrame) { p.doneFrame = f; p.hasDone = true; }
            if (hasHash) { m_hashes[f][p.slot] = r.u64(); HostCompare(f); }
        }
        else if (type == MSG_CHAT)
        {
            r.i32(); std::string text = r.str();
            Writer w; w.u8(MSG_CHAT); w.i32(p.slot); w.str(text);
            for (auto& kv : m_peers) if (kv.second.welcomed && kv.first != peer) m_t->Send(kv.first, w.b);
            Log("chat " + std::to_string(p.slot) + ": " + text);
        }
    }
    else
    {
        if (type == MSG_WELCOME)
        {
            m_slot = r.u8();
            uint16_t n = r.u16();
            m_names.clear();
            for (uint16_t i = 0; i < n; ++i) { int s = r.u8(); m_names[s] = r.str(); }
            Log("welcomed as slot " + std::to_string(m_slot));
        }
        else if (type == MSG_START)
        {
            m_epoch = r.u32();
            uint32_t delay = r.u32();
            Bytes blob = r.bytes();
            ResetFrames();
            m_started = true;
            // Frames before the first scheduled bundle carry no commands (the host never sends them).
            for (uint32_t f = 0; f < delay; ++f) m_bundles[f];
            Log("lockstep started");
            if (m_adapter) m_adapter->OnStart(blob);
        }
        else if (type == MSG_BUNDLE)
        {
            uint32_t f = r.u32(); uint16_t n = r.u16();
            std::vector<Command>& b = m_bundles[f];
            for (uint16_t i = 0; i < n && !r.bad; ++i) { Command c; c.slot = r.u8(); c.data = r.bytes(); b.push_back(c); }
        }
        else if (type == MSG_REJECT)
        {
            std::string reason = r.str();
            Log("host rejected: " + reason);
            if (m_adapter) m_adapter->OnReject(reason);
        }
        else if (type == MSG_DESYNC)
        {
            uint32_t f = r.u32(); int slot = r.i32();
            ++m_desyncs;
            Log("host reports desync at frame " + std::to_string(f) + " slot " + std::to_string(slot));
            if (m_adapter) m_adapter->OnDesync(f, slot, 0, 0);
        }
        else if (type == MSG_CHAT)
        {
            int from = r.i32(); std::string text = r.str();
            if (from == -1 && text.compare(0, 7, "joined ") == 0)
            {
                size_t sp = text.find(' ', 7);
                int s = atoi(text.substr(7, sp - 7).c_str());
                m_names[s] = sp == std::string::npos ? "" : text.substr(sp + 1);
            }
            else if (from == -1 && text.compare(0, 5, "left ") == 0)
            {
                m_names.erase(atoi(text.substr(5).c_str()));
            }
            Log("chat " + std::to_string(from) + ": " + text);
        }
    }
}

} // namespace coop
