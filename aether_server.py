#!/usr/bin/env python3
"""
AETHER CLASH - lobby and match relay server.

Run:  python aether_server.py [port]        (default port 7777, or the AETHER_PORT environment variable)

Players connect, see the open lobbies, host one or join one. Once two players share a lobby the server
simply passes their messages to each other. It never simulates the game: both players run the same
fixed-step simulation and only exchange button inputs, so the server needs almost no CPU or memory.

Messages are one JSON object per line.
  client -> server   {"type": "hello", "name": "Edd", "version": 2}
  server -> client   {"type": "welcome", "players": 3}
  client -> server   {"type": "list"}                 -> {"type": "rooms", "rooms": [{"code": "KQZM", "host": "Edd"}]}
  client -> server   {"type": "host"}                 -> {"type": "hosted", "code": "KQZM"}
  client -> server   {"type": "join", "code": "KQZM"} -> both get {"type": "paired", "slot": 0|1, "opponent": "...", "seed": n}
  client -> server   {"type": "leave"}                (leave the lobby; the other player gets {"type": "opponent_left"})
  anything else sent while paired is passed straight to the other player (fighter picks, inputs, ...)
"""
import asyncio
import json
import os
import random
import socket
import sys

PROTOCOL_VERSION = 2
MAX_LINE = 4096
clients = {}            # writer -> {"name": str, "room": code or None}
rooms = {}              # code -> {"host": writer, "guest": writer or None}


def note_players():
    """Lets the auto-updating launcher (server/run_server.py) see whether anyone is connected."""
    path = os.environ.get("AETHER_STATUS_FILE")
    if path:
        try:
            with open(path, "w") as fh:
                fh.write(str(len(clients)))
        except OSError:
            pass


def send(writer, **msg):
    if not writer.is_closing():
        writer.write((json.dumps(msg, separators=(",", ":")) + "\n").encode())


def peer_of(writer):
    room = rooms.get(clients.get(writer, {}).get("room"))
    if not room:
        return None
    return room["guest"] if room["host"] is writer else room["host"]


def leave_room(writer):
    info = clients.get(writer)
    code = info and info["room"]
    room = rooms.get(code)
    if not room:
        return
    info["room"] = None
    other = peer_of_room(room, writer)
    if room["host"] is writer:                       # the host left: the lobby closes
        rooms.pop(code, None)
        if other:
            clients[other]["room"] = None
            send(other, type="opponent_left")
    else:                                            # the guest left: the lobby opens up again
        room["guest"] = None
        if other:
            send(other, type="opponent_left")


def peer_of_room(room, writer):
    return room["guest"] if room["host"] is writer else room["host"]


def new_code():
    while True:
        code = "".join(random.choice("ABCDEFGHJKLMNPQRSTUVWXYZ") for _ in range(4))
        if code not in rooms:
            return code


async def handle(reader, writer):
    sock = writer.get_extra_info("socket")
    if sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)     # send every input at once, no batching
    try:
        hello = json.loads(await reader.readline() or b"{}")
        if hello.get("type") != "hello" or hello.get("version") != PROTOCOL_VERSION:
            send(writer, type="error", reason="This game version does not match the server. Update the game.")
            return
        clients[writer] = {"name": str(hello.get("name", "Player"))[:12] or "Player", "room": None}
        send(writer, type="welcome", players=len(clients))
        note_players()
        while True:
            line = await reader.readline()
            if not line:
                break
            if len(line) > MAX_LINE:
                continue
            info = clients[writer]
            peer = peer_of(writer)
            if peer and line.startswith(b'{"type":"i"'):                # the hot path: a frame of input
                peer.write(line)
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            kind = msg.get("type")
            if kind == "list":
                send(writer, type="rooms", rooms=[{"code": c, "host": clients[r["host"]]["name"]}
                                                  for c, r in rooms.items() if r["guest"] is None])
            elif kind == "host":
                leave_room(writer)
                code = new_code()
                rooms[code] = {"host": writer, "guest": None}
                info["room"] = code
                send(writer, type="hosted", code=code)
            elif kind == "join":
                room = rooms.get(str(msg.get("code", "")).upper())
                if not room or room["guest"] is not None or room["host"] is writer:
                    send(writer, type="error", reason="That lobby is full or no longer exists.")
                    continue
                leave_room(writer)
                room["guest"] = writer
                info["room"] = clients[room["host"]]["room"]
                seed = random.randrange(1 << 30)                         # both sides must use the same random numbers
                send(room["host"], type="paired", slot=0, opponent=info["name"], seed=seed)
                send(writer, type="paired", slot=1, opponent=clients[room["host"]]["name"], seed=seed)
            elif kind == "leave":
                leave_room(writer)
            elif peer:
                peer.write(line)                                         # relay untouched
    except (ConnectionError, ValueError, asyncio.IncompleteReadError):
        pass
    finally:
        leave_room(writer)
        clients.pop(writer, None)
        note_players()
        writer.close()


async def main(port):
    note_players()
    server = await asyncio.start_server(handle, "0.0.0.0", port)
    print(f"Aether Clash lobby server listening on port {port} (protocol {PROTOCOL_VERSION})", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("AETHER_PORT", 7777))))
    except KeyboardInterrupt:
        pass
