#!/usr/bin/env python3
"""
AETHER CLASH - match relay server (groundwork for online play; the game does not connect to it yet).

Run:  python aether_server.py [port]        (default port 7777)

It pairs players two at a time and passes their messages to each other. It never simulates the game:
both players run the same fixed-step simulation and only exchange inputs.

Messages are one JSON object per line:
  client -> server   {"type": "hello", "name": "Kai main", "version": 1}
  server -> client   {"type": "waiting"}                         (no opponent yet)
  server -> client   {"type": "start", "slot": 0, "seed": 123, "opponent": "..."}
  client -> server   {"type": "input", "frame": 42, "bits": 517}  (see encode_input in aether_clash.py)
                     any other message type is also passed straight to the opponent
  server -> client   {"type": "opponent_left"}
"""
import asyncio
import json
import random
import sys

PROTOCOL_VERSION = 1
waiting = None          # (name, writer) of the player waiting for an opponent
peers = {}              # writer -> opponent's writer


def send(writer, **msg):
    writer.write((json.dumps(msg) + "\n").encode())


async def handle(reader, writer):
    global waiting
    try:
        hello = json.loads(await reader.readline() or b"{}")
        if hello.get("type") != "hello" or hello.get("version") != PROTOCOL_VERSION:
            send(writer, type="error", reason="bad hello or version")
            return
        name = str(hello.get("name", "Player"))[:12]
        if waiting and not waiting[1].is_closing():
            other_name, other = waiting
            waiting = None
            peers[writer], peers[other] = other, writer
            seed = random.randrange(1 << 30)          # both sides must use the same random numbers
            send(other, type="start", slot=0, seed=seed, opponent=name)
            send(writer, type="start", slot=1, seed=seed, opponent=other_name)
        else:
            waiting = (name, writer)
            send(writer, type="waiting")
        while True:
            line = await reader.readline()
            if not line:
                break
            peer = peers.get(writer)
            if peer and not peer.is_closing():
                peer.write(line)                      # relay untouched
    except (ConnectionError, ValueError):
        pass
    finally:
        if waiting and waiting[1] is writer:
            waiting = None
        peer = peers.pop(writer, None)
        if peer:
            peers.pop(peer, None)
            if not peer.is_closing():
                send(peer, type="opponent_left")
        writer.close()


async def main(port=7777):
    server = await asyncio.start_server(handle, "0.0.0.0", port)
    print(f"Aether Clash relay listening on port {port}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else 7777))
    except KeyboardInterrupt:
        pass
