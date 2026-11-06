#include "CoopNet.h"
#include "CoopSteamNet.h"

#include <cerrno>
#include <ctime>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace coop {

#ifdef _WIN32
// Sockets are SOCKET (UINT_PTR) on Windows; FTL is 32-bit, so they fit in the int fields used here.
static void CloseSocket(int fd) { ::closesocket((SOCKET)fd); }
static bool WouldBlock() { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK; }
static std::string LastError() { return "socket error " + std::to_string(WSAGetLastError()); }
static bool StartSockets()
{
    static bool ok = false, tried = false;
    if (!tried) { tried = true; WSADATA w; ok = WSAStartup(MAKEWORD(2, 2), &w) == 0; }
    return ok;
}
static const int kNoSignal = 0;
#else
static void CloseSocket(int fd) { ::close(fd); }
static bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
static std::string LastError() { return strerror(errno); }
static bool StartSockets() { return true; }
static const int kNoSignal = MSG_NOSIGNAL;
#endif

static void SetNonBlocking(int fd)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

#ifdef _WIN32
// Windows ignores SO_SNDTIMEO for connect(): connect non-blocking and wait up to 2 s with select().
static int ConnectWithTimeout(int fd, const sockaddr* addr, int len)
{
    u_long nb = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
    if (::connect((SOCKET)fd, addr, len) == 0) return 0;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return -1;
    fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex); FD_SET((SOCKET)fd, &wr); FD_SET((SOCKET)fd, &ex);
    timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
    if (select(0, nullptr, &wr, &ex, &tv) <= 0 || FD_ISSET((SOCKET)fd, &ex)) { WSASetLastError(WSAETIMEDOUT); return -1; }
    int soerr = 0; int sl = sizeof(soerr);
    getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl);
    if (soerr) { WSASetLastError(soerr); return -1; }
    return 0;
}
#endif

TcpTransport::TcpTransport() : m_listenFd(-1), m_port(0), m_nextId(1), m_isGuest(false) {}

TcpTransport::~TcpTransport()
{
    for (auto& c : m_conns) if (c.fd >= 0) CloseSocket(c.fd);
    if (m_listenFd >= 0) CloseSocket(m_listenFd);
}

bool TcpTransport::Listen(int port, std::string* err)
{
    if (!StartSockets()) { if (err) *err = "WSAStartup failed"; return false; }
    m_listenFd = (int)::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) { if (err) *err = LastError(); return false; }
#ifndef _WIN32
    int one = 1; // (on Windows SO_REUSEADDR would let a second host steal the port)
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (::bind(m_listenFd, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(m_listenFd, 16) != 0)
    {
        if (err) *err = LastError();
        CloseSocket(m_listenFd); m_listenFd = -1;
        return false;
    }
    socklen_t len = sizeof(a);
    getsockname(m_listenFd, (sockaddr*)&a, &len);
    m_port = ntohs(a.sin_port);
    SetNonBlocking(m_listenFd);
    return true;
}

bool TcpTransport::Connect(const std::string& host, int port, std::string* err)
{
    m_isGuest = true;
    if (!StartSockets()) { if (err) *err = "WSAStartup failed"; return false; }
    addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res)
    {
        if (err) *err = "cannot resolve " + host;
        return false;
    }
    int fd = (int)::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    // Connect with a short timeout: an unreachable host must not freeze the game; the caller retries.
#ifdef _WIN32
    int rc = fd >= 0 ? ConnectWithTimeout(fd, res->ai_addr, (int)res->ai_addrlen) : -1;
#else
    // (Linux honours SO_SNDTIMEO for connect)
    if (fd >= 0) { timeval tv; tv.tv_sec = 2; tv.tv_usec = 0; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)); }
    int rc = fd >= 0 ? ::connect(fd, res->ai_addr, res->ai_addrlen) : -1;
#endif
    freeaddrinfo(res);
    if (rc != 0)
    {
        if (err) *err = LastError();
        if (fd >= 0) CloseSocket(fd);
        return false;
    }
    SetNonBlocking(fd);
    Conn c; c.fd = fd; c.id = 0; c.dead = false;
    m_conns.push_back(c);
    if (onConnect) onConnect(0);
    return true;
}

TcpTransport::Conn* TcpTransport::Find(int peer)
{
    for (auto& c : m_conns) if (c.id == peer && !c.dead) return &c;
    return nullptr;
}

static double NowMs()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void TcpTransport::Send(int peer, const Bytes& msg)
{
    if (m_latencyMs > 0) { m_delayed.push_back(Delayed{ NowMs() + m_latencyMs, peer, msg }); return; }
    SendNow(peer, msg);
}

void TcpTransport::SendNow(int peer, const Bytes& msg)
{
    Conn* c = Find(peer);
    if (!c) return;
    AppendFrame(c->out, msg);
    FlushConn(*c);
}

void TcpTransport::Close(int peer)
{
    Conn* c = Find(peer);
    if (c) { FlushConn(*c); c->dead = true; }
}

void TcpTransport::FlushConn(Conn& c)
{
    while (!c.out.empty() && !c.dead)
    {
        long k = ::send(c.fd, (const char*)c.out.data(), (int)c.out.size(), kNoSignal);
        if (k > 0) c.out.erase(c.out.begin(), c.out.begin() + k);
        else if (k < 0 && WouldBlock()) break;
        else { c.dead = true; }
    }
}

void TcpTransport::ReadConn(Conn& c)
{
    uint8_t buf[65536];
    for (;;)
    {
        long k = ::recv(c.fd, (char*)buf, sizeof(buf), 0);
        if (k > 0) c.in.insert(c.in.end(), buf, buf + k);
        else if (k == 0) { c.dead = true; break; }
        else if (WouldBlock()) break;
        else { c.dead = true; break; }
    }
    int id = c.id;
    if (!PopFrames(c.in, [&](const Bytes& msg) { if (onMessage) onMessage(id, msg); })) c.dead = true;
}

void TcpTransport::Poll()
{
    if (!m_delayed.empty())
    {
        double now = NowMs();
        size_t k = 0;
        while (k < m_delayed.size() && m_delayed[k].due <= now) { SendNow(m_delayed[k].peer, m_delayed[k].msg); ++k; }
        if (k) m_delayed.erase(m_delayed.begin(), m_delayed.begin() + k);
    }
    if (m_listenFd >= 0)
    {
        for (;;)
        {
#ifdef _WIN32
            SOCKET as = ::accept((SOCKET)m_listenFd, nullptr, nullptr);
            if (as == INVALID_SOCKET) break;
            int fd = (int)as;
#else
            int fd = ::accept(m_listenFd, nullptr, nullptr);
            if (fd < 0) break;
#endif
            SetNonBlocking(fd);
            Conn c; c.fd = fd; c.id = m_nextId++; c.dead = false;
            m_conns.push_back(c);
            if (onConnect) onConnect(c.id);
        }
    }
    // Index loop: callbacks may Send(), which never adds connections, but be safe anyway.
    for (size_t i = 0; i < m_conns.size(); ++i)
    {
        if (!m_conns[i].dead) { FlushConn(m_conns[i]); ReadConn(m_conns[i]); }
    }
    for (size_t i = 0; i < m_conns.size();)
    {
        if (m_conns[i].dead)
        {
            int id = m_conns[i].id;
            CloseSocket(m_conns[i].fd);
            m_conns.erase(m_conns.begin() + i);
            if (onDisconnect) onDisconnect(id);
        }
        else ++i;
    }
}

} // namespace coop
