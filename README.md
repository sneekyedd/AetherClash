# Aether Clash

A 2D platform fighter prototype in a single Python file, drawn entirely with Pygame shapes (no image files needed).

Knock your rival off the screen. Every hit adds to their damage **%**, and the higher it is, the further they fly. Each fighter has 3 lives.

## Run it

```bash
pip install pygame
```

```bash
python aether_clash.py
```

`python aether_clash.py --demo` watches two CPUs fight. `python aether_clash.py --selftest` runs a windowless test of every fighter and stage.

## Controls

| Action | Player 1 | Player 2 |
| --- | --- | --- |
| Move / aim | A D / W S | Arrow keys |
| Jump (tap = short hop, again in the air = double jump) | Space | Right Ctrl or Numpad 0 |
| Attack | J | `,` or Numpad 1 |
| Heavy (smash) attack | K | `.` or Numpad 2 |
| Special (costs Aether) | L | `/` or Numpad 3 |
| Shield (tap just before a hit = parry) | Left Shift or I | Right Shift or Numpad Enter |
| Grab | H | M or Numpad 4 |

- Hold a direction while pressing Attack, Heavy or Special to get a different move (18 moves per fighter).
- After a grab: press a direction to throw that way, or Attack to pommel (small damage) first.
- Down attacks have a small **spike spot**. Land it on an airborne rival and they are launched straight down.
- Shield + left/right = roll. Shield in the air = air dodge.
- Down = fast-fall, or drop through thin platforms. You grab ledges automatically; press Up to climb.
- Esc pauses and shows the controls. F1 shows hitboxes (spike spots in magenta). Gamepads are picked up automatically.

## Profiles

**Profiles & Controls** on the start menu lets you create a profile: a player name plus, if you want, your own key
bindings. Pick a profile on the character screen (Q / E for Player 1, [ / ] for Player 2). Profiles are saved in
`aether_clash_profiles.json` next to the game.

## Online play

Choose **Online** on the start menu, type the lobby server's address (for example `myserver.example.com:7777`) and
press Enter. Then either press **H** to host a lobby, or pick a friend's lobby from the list and press Enter.
Both players choose a fighter, the host chooses the stage, and the match starts when both press Enter.

- Your name online is the profile selected for Player 1 (make one under Profiles & Controls).
- You always use Player 1's keys online, whichever side you are on.
- Both players must be on the same game version.
- There is no pausing online; Esc leaves the match and returns both players to the lobby.

How it works: the two games run the same simulation and only exchange button presses through the server, a few
frames ahead ("lockstep"). The server does no game logic, so it needs almost no CPU, memory or bandwidth.

## Hosting the lobby server

The server is one small Python file, `aether_server.py`. It needs **one TCP port (7777)** open to the internet.

**With Docker** (from a copy of this repository on the server):

```bash
docker compose -f server/docker-compose.yml up -d --build
```

**Without Docker** (Python 3.9 or newer):

```bash
python server/run_server.py
```

Then on your router, forward **TCP port 7777** to the server's local IP address. Players connect to your public IP
address or domain name followed by `:7777`.

`server/run_server.py` keeps the server up to date: every 15 minutes it checks GitHub for a newer
`aether_server.py` and restarts onto it once nobody is connected. While the repository is private it needs a
read-only GitHub token in the `GITHUB_TOKEN` setting.

## Automatic updates

The packaged game checks GitHub for a newer release each time it starts. If there is one it downloads the new
`AetherClash.exe`, swaps it in and restarts. This only works while the repository's releases are publicly
downloadable.

## Fighters

| Fighter | Style |
| --- | --- |
| **Kai, the Weaver** | Fastest, combo-oriented thread-blade duelist. Light, so he is launched easily. |
| **Bronn, the Sentinel** | Slow heavyweight with armored attacks, a counter stance and a summoned stone wall. |
| **Nyx, the Hollow Star** | Floaty mage with three jumps, projectiles, a gravity well and a teleport. |
| **Rhea, the Cinderfist** | Short-range brawler who can spend Aether to hit 30% harder for a while. |

## Rules of the engine

- Knockback: `Final Force = Base Force * (1.0 + Cumulative Damage / 200.0)`, then scaled by the target's weight.
- Aether: gained by dealing and taking damage, slowly over time, and **+25 for a parry**. Specials spend it.
- Everything is simulated in fixed 1/60 s frames, so move timings ("frame data") are exact and repeatable.

## Artwork

The fighters are drawn from the sprite images in `assets/<fighter>/`: one image per move (`jab.png`, `fsmash.png`,
...) plus movement poses (`idle`, `run_1`, `run_2`, ..., `jump`, `fall`, `hurt`, `shield`, ...). These are packed
into the `.exe`. Any fighter or pose without an image falls back to the nearest pose, or to the code-drawn fighter.

To change the art, put an `assets` folder next to the game; it takes priority over the built-in images.

- A still pose is `assets/kai/idle.png`; an animation is `run_1.png`, `run_2.png`, ... played in number order.
- Draw the fighter facing right on a transparent background.
- `assets/kai/sprite.json` holds the size and alignment: `scale`, `fps`, `anchors` (where the body centre and feet
  are in each image, so effects painted into a move do not shift the fighter) and `zoom` (per-image size tweak).
- Stage backgrounds: `assets/bg_atoll.png`, `bg_loom.png`, `bg_crucible.png` (2400x1400).
