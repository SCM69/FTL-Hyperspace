#include "CoopNet.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace coop {

static const uint32_t MAX_MSG = 64u * 1024u * 1024u; // save transfers can be large

static void SetNonBlocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

TcpTransport::TcpTransport() : m_listenFd(-1), m_port(0), m_nextId(1), m_isGuest(false) {}

TcpTransport::~TcpTransport()
{
    for (auto& c : m_conns) if (c.fd >= 0) ::close(c.fd);
    if (m_listenFd >= 0) ::close(m_listenFd);
}

bool TcpTransport::Listen(int port, std::string* err)
{
    m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) { if (err) *err = strerror(errno); return false; }
    int one = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (::bind(m_listenFd, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(m_listenFd, 16) != 0)
    {
        if (err) *err = strerror(errno);
        ::close(m_listenFd); m_listenFd = -1;
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
    addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res)
    {
        if (err) *err = "cannot resolve " + host;
        return false;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    // Blocking connect keeps this simple; it happens once, before the game starts.
    int rc = fd >= 0 ? ::connect(fd, res->ai_addr, res->ai_addrlen) : -1;
    freeaddrinfo(res);
    if (rc != 0)
    {
        if (err) *err = strerror(errno);
        if (fd >= 0) ::close(fd);
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

void TcpTransport::Send(int peer, const Bytes& msg)
{
    Conn* c = Find(peer);
    if (!c) return;
    uint32_t n = (uint32_t)msg.size();
    uint8_t hdr[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
    c->out.insert(c->out.end(), hdr, hdr + 4);
    c->out.insert(c->out.end(), msg.begin(), msg.end());
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
        ssize_t k = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (k > 0) c.out.erase(c.out.begin(), c.out.begin() + k);
        else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { c.dead = true; }
    }
}

void TcpTransport::ReadConn(Conn& c)
{
    uint8_t buf[65536];
    for (;;)
    {
        ssize_t k = ::recv(c.fd, buf, sizeof(buf), 0);
        if (k > 0) c.in.insert(c.in.end(), buf, buf + k);
        else if (k == 0) { c.dead = true; break; }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { c.dead = true; break; }
    }
    size_t off = 0;
    while (c.in.size() - off >= 4)
    {
        uint32_t n = c.in[off] | (c.in[off + 1] << 8) | (c.in[off + 2] << 16) | ((uint32_t)c.in[off + 3] << 24);
        if (n > MAX_MSG) { c.dead = true; break; }
        if (c.in.size() - off - 4 < n) break;
        Bytes msg(c.in.begin() + off + 4, c.in.begin() + off + 4 + n);
        off += 4 + n;
        if (onMessage) onMessage(c.id, msg);
    }
    if (off) c.in.erase(c.in.begin(), c.in.begin() + off);
}

void TcpTransport::Poll()
{
    if (m_listenFd >= 0)
    {
        for (;;)
        {
            int fd = ::accept(m_listenFd, nullptr, nullptr);
            if (fd < 0) break;
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
            ::close(m_conns[i].fd);
            m_conns.erase(m_conns.begin() + i);
            if (onDisconnect) onDisconnect(id);
        }
        else ++i;
    }
}

} // namespace coop
