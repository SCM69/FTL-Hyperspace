#!/usr/bin/env python3
"""Stand-in for Steam lobbies and P2P, shared by several FTL co-op test copies on one PC.

Usage: fakesteam_hub.py <port> [--lag-ms N]
Each copy (FTL_COOP_FAKE_STEAM=127.0.0.1:<port>) connects over TCP. Frames: u32 little-endian length + body.
Protocol: see src/features/coop/core/CoopSteamHub.cpp. Steam rules kept: lobby owner is the creator, the next
member becomes owner when the owner leaves, packets from a user the receiver has not accepted (and never sent to)
are held until accepted, everyone sees which lobby each player is in (like the friends list's game info).
"""
import selectors
import socket
import struct
import sys
import time

HELLO, CREATE, JOIN, LEAVE, SETDATA, SEND, ACCEPT, CLOSE = 1, 2, 3, 4, 5, 6, 7, 8
CALLDONE, LOBBY, PLAYING, PACKET = 20, 21, 22, 23


class R:
    def __init__(self, b):
        self.b, self.i = b, 0

    def take(self, n):
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def u8(self): return self.take(1)[0]
    def u32(self): return struct.unpack('<I', self.take(4))[0]
    def u64(self): return struct.unpack('<Q', self.take(8))[0]
    def bytes(self): return self.take(self.u32())
    def str(self): return self.bytes().decode('utf-8', 'replace')


def wstr(s):
    b = s.encode() if isinstance(s, str) else s
    return struct.pack('<I', len(b)) + b


class Client:
    def __init__(self, sock):
        self.sock, self.inbuf, self.out = sock, bytearray(), bytearray()
        self.me, self.name = 0, '?'
        self.accepted = set()


class Hub:
    def __init__(self, port, lag):
        self.sel = selectors.DefaultSelector()
        self.ls = socket.socket()
        self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(('127.0.0.1', port))
        self.ls.listen(32)
        self.ls.setblocking(False)
        self.sel.register(self.ls, selectors.EVENT_READ, None)
        self.clients = {}        # sock -> Client
        self.by_id = {}          # steam id -> Client
        self.lobbies = {}        # id -> {'owner', 'members': [], 'data': {}, 'max'}
        self.held = {}           # receiver id -> [(from, data)]
        self.next_lobby = 0x0186000000001000
        self.lag = lag / 1000.0
        self.delayed = []        # (due, client, frame)
        self.stats = {'packets': 0, 'bytes': 0, 'held': 0}

    def send(self, c, body):
        frame = struct.pack('<I', len(body)) + body
        if self.lag:
            self.delayed.append((time.monotonic() + self.lag, c, frame))
        else:
            c.out += frame

    def lobby_msg(self, lid):
        l = self.lobbies.get(lid)
        b = bytes([LOBBY]) + struct.pack('<Q', lid)
        if not l:
            return b + struct.pack('<QII', 0, 0, 0)
        b += struct.pack('<QI', l['owner'], len(l['members']))
        b += b''.join(struct.pack('<Q', m) for m in l['members'])
        b += struct.pack('<I', len(l['data'])) + b''.join(wstr(k) + wstr(v) for k, v in l['data'].items())
        return b

    def broadcast_lobby(self, lid):
        msg = self.lobby_msg(lid)
        for c in self.clients.values():
            self.send(c, msg)

    def playing_of(self, uid):
        for lid, l in self.lobbies.items():
            if uid in l['members']:
                return lid
        return 0

    def broadcast_playing(self):
        known = [c for c in self.clients.values() if c.me]
        b = bytes([PLAYING]) + struct.pack('<I', len(known))
        for c in known:
            b += struct.pack('<QQ', c.me, self.playing_of(c.me)) + wstr(c.name)
        for c in self.clients.values():
            self.send(c, b)

    def leave(self, c, lid):
        l = self.lobbies.get(lid)
        if not l or c.me not in l['members']:
            return
        l['members'].remove(c.me)
        if not l['members']:
            del self.lobbies[lid]
        elif l['owner'] == c.me:
            l['owner'] = l['members'][0]
        self.broadcast_lobby(lid)
        self.broadcast_playing()

    def deliver(self, to, frm, data):
        rc = self.by_id.get(to)
        if not rc:
            return
        if frm in rc.accepted:
            self.send(rc, bytes([PACKET]) + struct.pack('<Q', frm) + wstr(data))
        else:
            self.held.setdefault(to, []).append((frm, data))
            self.stats['held'] += 1

    def handle(self, c, body):
        r = R(body)
        op = r.u8()
        if op == HELLO:
            c.me, c.name = r.u64(), r.str()
            self.by_id[c.me] = c
            for lid in self.lobbies:
                self.send(c, self.lobby_msg(lid))
            self.broadcast_playing()
        elif op == CREATE:
            call, mx = r.u64(), r.u32()
            lid = self.next_lobby
            self.next_lobby += 1
            self.lobbies[lid] = {'owner': c.me, 'members': [c.me], 'data': {}, 'max': mx}
            self.broadcast_lobby(lid)
            self.broadcast_playing()
            self.send(c, bytes([CALLDONE]) + struct.pack('<QBQ', call, 1, lid))
        elif op == JOIN:
            call, lid = r.u64(), r.u64()
            l = self.lobbies.get(lid)
            ok = l is not None and len(l['members']) < l['max']
            if ok and c.me not in l['members']:
                l['members'].append(c.me)
                self.broadcast_lobby(lid)
                self.broadcast_playing()
            self.send(c, bytes([CALLDONE]) + struct.pack('<QBQ', call, 1 if ok else 0, lid))
        elif op == LEAVE:
            self.leave(c, r.u64())
        elif op == SETDATA:
            lid, k, v = r.u64(), r.str(), r.str()
            l = self.lobbies.get(lid)
            if l and l['owner'] == c.me:
                l['data'][k] = v
                self.broadcast_lobby(lid)
        elif op == SEND:
            to, data = r.u64(), r.bytes()
            c.accepted.add(to)
            self.stats['packets'] += 1
            self.stats['bytes'] += len(data)
            self.deliver(to, c.me, data)
        elif op == ACCEPT:
            u = r.u64()
            c.accepted.add(u)
            keep = []
            for frm, data in self.held.get(c.me, []):
                if frm == u:
                    self.send(c, bytes([PACKET]) + struct.pack('<Q', frm) + wstr(data))
                else:
                    keep.append((frm, data))
            self.held[c.me] = keep
        elif op == CLOSE:
            c.accepted.discard(r.u64())

    def drop(self, c):
        self.sel.unregister(c.sock)
        c.sock.close()
        del self.clients[c.sock]
        if c.me:
            # Steam removes a player from their lobbies when the game exits.
            for lid in [lid for lid, l in self.lobbies.items() if c.me in l['members']]:
                self.leave(c, lid)
            self.by_id.pop(c.me, None)
            self.broadcast_playing()

    def run(self):
        print('fakesteam hub listening', flush=True)
        while True:
            now = time.monotonic()
            due = [d for d in self.delayed if d[0] <= now]
            if due:
                self.delayed = [d for d in self.delayed if d[0] > now]
                for _, c, frame in due:
                    if c.sock in self.clients:
                        c.out += frame
            for key, ev in self.sel.select(timeout=0.005):
                if key.data is None:
                    s, _ = self.ls.accept()
                    s.setblocking(False)
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    c = Client(s)
                    self.clients[s] = c
                    self.sel.register(s, selectors.EVENT_READ, c)
                    continue
                c = key.data
                try:
                    data = c.sock.recv(1 << 20)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    data = b''
                if not data:
                    self.drop(c)
                    continue
                c.inbuf += data
                while len(c.inbuf) >= 4:
                    n = struct.unpack('<I', c.inbuf[:4])[0]
                    if len(c.inbuf) < 4 + n:
                        break
                    body = bytes(c.inbuf[4:4 + n])
                    del c.inbuf[:4 + n]
                    self.handle(c, body)
            for c in list(self.clients.values()):
                while c.out:
                    try:
                        k = c.sock.send(c.out)
                    except (BlockingIOError, InterruptedError):
                        break
                    except OSError:
                        c.out = bytearray()
                        break
                    del c.out[:k]


if __name__ == '__main__':
    port = int(sys.argv[1])
    lag = int(sys.argv[sys.argv.index('--lag-ms') + 1]) if '--lag-ms' in sys.argv else 0
    Hub(port, lag).run()
