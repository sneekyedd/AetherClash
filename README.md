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

## Online

The start menu has an **Online** entry, but online play is not implemented yet. The groundwork is there: a
compact per-frame input format, a `NetworkDevice` controller, and `aether_server.py`, a small relay server that
pairs two players (`python aether_server.py`).

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

## Custom art

Put PNGs in an `assets/` folder next to the script and they replace the placeholder drawings:
`kai_idle.png`, `bronn_attack.png`, ... (poses: idle, run, jump, fall, attack, hurt, stun, shield, parry, dodge, ledge, helpless; drawn facing right) and `bg_atoll.png`, `bg_loom.png`, `bg_crucible.png` (2400x1400).
