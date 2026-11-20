# Aether Clash

A 2D platform fighter written in C++ (with SDL2 for the window, keyboard and gamepads). Every fighter, stage and
effect is drawn by code; there are no image files to install.

Knock your rival off the screen. Every hit adds to their damage **%**, and the higher it is, the further they fly. Each fighter has 3 lives.

## Play it

Download `AetherClash.exe` from the newest release on GitHub and double-click it. Nothing else is needed.

## Build it yourself

You need CMake, a C++17 compiler and SDL2 (on Ubuntu: `sudo apt install cmake g++ libsdl2-dev`).

```bash
cmake -S cpp -B build
```

```bash
cmake --build build -j
```

```bash
./build/AetherClash
```

- `./build/AetherClash --demo` watches two CPUs fight.
- `./build/AetherClash --selftest` runs a windowless test of every fighter and stage, the secret ending and every screen.
- `cpp/cmake/mingw-w64.cmake` cross-compiles the Windows .exe from Linux; `.github/workflows/release.yml` shows the exact steps.

The code, in `cpp/src/`:

| File | What it does |
| --- | --- |
| `gfx.*` | the drawing library: shapes, blending, scaling, rotation, outlines, text, PNG |
| `core.h`, `roster.cpp` | rules, tuning values, the four fighters' moves and the three stages |
| `sim.cpp` | physics, hit detection, knockback, the combat state machine, effects |
| `control.cpp` | buttons to actions, input buffering, and the CPU player |
| `art_*.cpp` | skeleton animation, cloth physics, the fighters' drawings, attack effects, scenery |
| `game.cpp`, `screens.cpp` | the main loop, menus, HUD, profiles, settings and online lockstep |
| `net.cpp` | the lobby connection, HTTP downloads and the auto-updater |
| `annihilation.cpp` | the Death Ray ending |

The original Python version is kept in `legacy/` for reference.

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

- Hold a direction while pressing Attack, Heavy or Special to get a different move (19 moves per fighter).
- **Dash attacks:** straight out of a dash or run, Attack is a quick hit that knocks the opponent away (good for
  making space), and Heavy is a slow, telegraphed lunge that KOs at high damage. You keep your running speed and
  slide into the hit, as in Smash. (Pressing a direction and Attack on the same frame still gives the forward tilt.)
- After a grab: press a direction to throw that way, or Attack to pommel (small damage) first.
- Down attacks have a small **spike spot**. Land it on an airborne rival and they are launched straight down.
- Shield + left/right = roll. Shield in the air = air dodge.
- Down = fast-fall, or drop through thin platforms. You grab ledges automatically; press Up to climb.
- **F11** (or Alt+Enter, or Full Screen on the start menu) switches full screen on and off; the game remembers it.
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

The server is one small Python file, `aether_server.py` (it stays in Python: it runs on the server, not on
players' computers, and the C++ game talks to it exactly as before). It needs **one TCP port (7777)** open to the internet.
The same port also hands players the latest version of the game (see Automatic updates).

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

`server/run_server.py` restarts the server if it ever stops, switches to a newer `aether_server.py` from GitHub
(checked every 15 minutes, applied only when nobody is connected), and keeps a copy of the newest game release
(checked every 5 minutes).

## Automatic updates

Every time the packaged game opens, it checks for a newer release before the title screen. If there is one, it
downloads it, checks it is intact, swaps it in and restarts into it. Press Esc on that screen to skip.

It looks in two places:

1. **Your lobby server**, which keeps a copy of the newest release. This works while the GitHub repository is
   private, because only the server needs access to GitHub.
2. **GitHub itself**, which only works if the repository is public.

To make the lobby-server route work:

1. **Give the server read access to the repository.** Create a fine-grained personal access token on GitHub
   (Settings, Developer settings, Personal access tokens) for this repository only, with read-only
   **Contents** access. Put it in `GITHUB_TOKEN` in `server/docker-compose.yml` (or set it as an environment
   variable when running `server/run_server.py`), then restart the server.
2. **Tell the game where the server is.** In the GitHub repository, go to Settings, Secrets and variables,
   Actions, Variables, and add a variable named `AETHER_SERVER` with your address, for example
   `myserver.example.com:7777`. Every build from then on knows the server: it is used for updates and filled in
   on the Online screen. Copies built before that learn the address the first time they connect online.

## Fighters

| Fighter | Style |
| --- | --- |
| **Kai, the Weaver** | Fastest, combo-oriented thread-blade duelist. Light, so he is launched easily. |
| **Bronn, the Sentinel** | Slow heavyweight with armored attacks, a counter stance and a summoned stone wall. |
| **Nyx, the Hollow Star** | Floaty mage with three jumps, projectiles, a gravity well and a teleport. |
| **Rhea, the Cinderfist** | Short-range brawler who can spend Aether to hit 30% harder for a while. |

## Rules of the engine

Movement copies **Super Smash Bros. Ultimate**. Each fighter uses a real Ultimate fighter's movement values, taken
from SmashWiki's attribute tables:

| Fighter | Moves like | Notes |
| --- | --- | --- |
| Kai | Sheik | very fast run, quick 7-frame dash, high jumps, falls fast |
| Bronn | Ganondorf | slow 15-frame dash, short jumps, heavy |
| Nyx | Mewtwo | fast but floaty; Mewtwo's huge double jump is split over Nyx's two |
| Rhea | Terry | grounded brawler with a quick run acceleration |

What that means in play:

- **Running:** tapping a direction gives an initial dash (a burst at a set speed) that you can reverse instantly
  to dash-dance. After it you run, building up to top speed. Let go and you skid to a stop by your traction.
- **Jumping:** 3 frames of crouch (jump squat) before you leave the ground. Hold jump for a full hop, tap it for a
  short hop (about half as high). Full hops rise quickly at first, as in Ultimate.
- **Falling:** fast-falling is 1.6x your falling speed. Landings have 2 frames of lag, or a little more after a
  fast-fall.
- **Getting hit:** launch speed decays by a fixed amount every frame and hitstun is 0.4x the knockback, as in Smash.
  Knockback still follows `Final Force = Base Force * (1.0 + (Cumulative Damage / 200.0))`, scaled by weight.
- **Aether:** gained by dealing and taking damage, slowly over time, and **+25 for a parry**. Specials spend it.
- Everything is simulated in fixed 1/60 s frames, so move timings ("frame data") are exact and repeatable.

## Artwork

The fighters are drawn by code. Their attacks are circles: every hitbox is a circle (long attacks are a chain of
circles), and what you see on screen is exactly that circle, in the fighter's own style. Press F1 in a match to
outline the real hitboxes in red.

The sprite images cut from the supplied artwork sheets are kept in `art/custom_sprites/` but are not used for now.
(The C++ game does not load fighter images yet; the Python version in `legacy/` still can.)
