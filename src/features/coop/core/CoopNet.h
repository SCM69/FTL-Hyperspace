#pragma once
// FTL co-op: message transport. Game-independent, C++11, POSIX sockets.
// Messages are length-prefixed frames over TCP (reliable, ordered), which is what lockstep needs.
// A Steam P2P transport can implement the same interface later.

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace coop {

typedef std::vector<uint8_t> Bytes;

class Transport
{
public:
    virtual ~Transport() {}
    // Host side: peer ids are assigned by the transport (1..). Guest side: the host is peer 0.
    virtual void Poll() = 0;
    virtual void Send(int peer, const Bytes& msg) = 0;
    virtual void Close(int peer) = 0;

    std::function<void(int peer)> onConnect;
    std::function<void(int peer)> onDisconnect;
    std::function<void(int peer, const Bytes& msg)> onMessage;
};

// TCP implementation. Host: Listen(port). Guest: Connect(host, port).
class TcpTransport : public Transport
{
public:
    TcpTransport();
    ~TcpTransport();
    bool Listen(int port, std::string* err = nullptr);
    bool Connect(const std::string& host, int port, std::string* err = nullptr);
    void Poll() override;
    void Send(int peer, const Bytes& msg) override;
    void Close(int peer) override;
    int ListenPort() const { return m_port; }
    bool Connected() const { for (auto& c : m_conns) if (!c.dead) return true; return false; }

    // Tests: hold every outgoing message this long before it goes on the wire (one-way latency).
    void SetTestLatencyMs(int ms) { m_latencyMs = ms; }

private:
    struct Delayed { double due; int peer; Bytes msg; };
    std::vector<Delayed> m_delayed;
    int m_latencyMs = 0;
    void SendNow(int peer, const Bytes& msg);
    struct Conn { int fd; int id; Bytes in; Bytes out; bool dead; };
    void FlushConn(Conn& c);
    void ReadConn(Conn& c);
    Conn* Find(int peer);

    int m_listenFd;
    int m_port;
    int m_nextId;
    bool m_isGuest;
    std::vector<Conn> m_conns;
};

// Little-endian writer/reader for message bodies.
struct Writer
{
    Bytes b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(v & 0xff); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xffff); u16(v >> 16); }
    void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void bytes(const Bytes& v) { u32((uint32_t)v.size()); b.insert(b.end(), v.begin(), v.end()); }
    void str(const std::string& s) { u32((uint32_t)s.size()); b.insert(b.end(), s.begin(), s.end()); }
};

struct Reader
{
    const uint8_t* p; size_t n; size_t i; bool bad;
    Reader(const Bytes& v) : p(v.data()), n(v.size()), i(0), bad(false) {}
    Reader(const uint8_t* d, size_t len) : p(d), n(len), i(0), bad(false) {}
    bool need(size_t k) { if (i + k > n) { bad = true; return false; } return true; }
    uint8_t u8() { if (!need(1)) return 0; return p[i++]; }
    uint16_t u16() { uint16_t a = u8(); return a | (uint16_t)(u8() << 8); }
    uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
    uint64_t u64() { uint64_t a = u32(); return a | ((uint64_t)u32() << 32); }
    int32_t i32() { return (int32_t)u32(); }
    Bytes bytes() { uint32_t k = u32(); Bytes v; if (!need(k)) return v; v.assign(p + i, p + i + k); i += k; return v; }
    std::string str() { uint32_t k = u32(); std::string s; if (!need(k)) return s; s.assign((const char*)p + i, k); i += k; return s; }
    bool done() const { return !bad && i == n; }
};

} // namespace coop
