#!/usr/bin/env python3
"""
AETHER CLASH - a 2D platform fighter prototype (single-file, Pygame).

Run:      python aether_clash.py
Options:  --demo       watch CPU vs CPU
          --selftest   headless smoke test of every character / stage

Architecture (one class per module of the blueprint):
  1. Data Structures     CharacterData, Move, PlatformData (+ Projectile, Stage)
  2. PhysicsEngine       gravity, movement, platforms, ledges, blast zones, knockback
  3. CombatManager       damage, Aether economy, shields, parries, stun/hit states
  4. CharacterController input -> commands (keyboard, gamepad, or CPU "AIController")
  5. GameManager         menus + the fixed-step loop: Input -> Physics -> Combat -> Render
  6. StateTranslator     maps internal state -> what gets drawn (pose, tint, outline)

Everything is simulated in whole frames at 60 FPS, so the game is deterministic
and every move can be described in "frame data" like a real fighting game.
"""
import math
import os
import random
import sys

import pygame

# =====================================================================
# GLOBAL CONSTANTS & TUNING
# =====================================================================
SCREEN_WIDTH, SCREEN_HEIGHT = 1280, 720
FPS = 60
WORLD_W, WORLD_H = 2400, 1400          # the arena is bigger than the screen; a camera follows the action
BLAST_ZONE = pygame.Rect(280, 120, 1840, 1210)   # leave this box and you lose a life

MAX_LIVES = 3
KNOCKBACK_DIVISOR = 200.0   # Final Force = Base Force * (1.0 + Cumulative Damage / 200.0)
KB_DECAY = 0.5              # launch speed lost per frame (constant, so distance grows with force squared)
HITSTUN_PER_SPEED = 1.5     # frames of hitstun per unit of launch speed
HITSTUN_GRAVITY = 0.3       # gravity is weaker while being launched, so upward KOs are possible
TUMBLE_SPEED = 11.0         # launches faster than this "knock down" the target (spinning tumble)

# --- Aether economy -------------------------------------------------
AETHER_MAX = 100.0
AETHER_START = 25.0
AETHER_PARRY_REWARD = 25.0  # mandatory rule: a parry grants +25
AETHER_PER_DAMAGE_DEALT = 0.6
AETHER_PER_DAMAGE_TAKEN = 0.25
AETHER_PASSIVE = 0.02       # per frame (about 1.2 per second)

# --- Defence --------------------------------------------------------
SHIELD_MAX = 50.0
PARRY_WINDOW = 6            # shield raised within this many frames of the hit = parry
PARRY_STUN = 45             # how long a parried attacker is left helpless
SHIELD_BREAK_STUN = 170
ARMOR_LIMIT = 18            # super armor ignores hits weaker than this
IGNITE_MULT = 1.3
BUFFER_FRAMES = 6           # a button pressed slightly early still counts

WHITE = (255, 255, 255)
PLAYER_TAGS = [(255, 96, 96), (96, 170, 255)]


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def sign(v):
    return (v > 0) - (v < 0)


def lerp_color(a, b, t):
    t = clamp(t, 0.0, 1.0)
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# =====================================================================
# 1. DATA STRUCTURES (Module 1)
# =====================================================================

def hb(start, end, x, y, w, h, damage, force, angle, gid=0, **flags):
    """One hitbox. Active on move frames start..end. (x, y) is its centre relative to the
    fighter's centre (x is 'forward'). angle: 0 = forward, 90 = straight up, 270 = spike.
    Hitboxes sharing a gid can only hit a target once between them (multi-hit moves use several gids)."""
    d = dict(start=start, end=end, rect=(x, y, w, h), damage=damage, force=force,
             angle=angle, gid=gid, grab=False, radial=False, pull=False)
    d.update(flags)
    return d


class Move:
    def __init__(self, name, move_type, duration, hitbox_data=(), required_aether=0.0, damage=None, **extra):
        self.name = name
        self.move_type = move_type            # "Attack", "Special", "Defensive", "Grab"
        self.duration = duration              # total frames the fighter is committed
        self.hitbox_data = list(hitbox_data)
        self.required_aether = required_aether
        self.damage = damage if damage is not None else max([h['damage'] for h in self.hitbox_data], default=0)
        self.impulses = extra.get('impulses', {})     # {frame: (forward_speed or None, vertical_speed or None)}
        self.armor = extra.get('armor')               # (first, last) frames of super armor
        self.counter = extra.get('counter')           # (first, last) frames that parry incoming hits
        self.intangible = extra.get('intangible')     # (first, last) frames of invulnerability
        self.no_gravity = extra.get('no_gravity')     # (first, last) frames gravity is suspended
        self.aim = extra.get('aim')                   # (speed, first, last): fly in the held direction
        self.projectile = extra.get('projectile')     # dict describing a projectile to spawn
        self.effect = extra.get('effect')             # (name, frame) bespoke effect
        self.helpless = extra.get('helpless', False)  # recovery moves leave you unable to act until landing
        self.landing_lag = extra.get('landing_lag', 6)
        self.slot = None


class CharacterData:
    """A fighter: permanent stats plus all per-match runtime state."""

    def __init__(self, name, faction, base_stats, move_set, spec=None, slot=0, colors=None):
        self.name, self.faction, self.spec, self.slot = name, faction, spec or {}, slot
        self.colors = colors or {}
        self.id = slot
        self.max_lives = MAX_LIVES
        self.speed = base_stats['speed']
        self.air_speed = base_stats['air_speed']
        self.jump_height = base_stats['jump_height']        # initial jump velocity (px / frame)
        self.double_jump = base_stats['double_jump']
        self.gravity = base_stats['gravity']
        self.max_fall = base_stats['max_fall']
        self.weight = base_stats['weight']
        self.air_jumps = base_stats.get('air_jumps', 1)
        self.w, self.h = base_stats['w'], base_stats['h']
        self.move_set = move_set
        self.reset(0, 0, 1)

    def reset(self, x, y, facing):
        self.current_lives = self.max_lives
        self.cumulative_damage_taken = 0.0
        self.aether_gauge = AETHER_START
        self.kos = 0
        self.respawn_at(x, y, facing, fresh=True)

    def respawn_at(self, x, y, facing, fresh=False):
        self.x, self.y = float(x), float(y)          # (x, y) = centre of the feet
        self.vx = self.vy = self.kbx = self.kby = 0.0
        self.facing = facing
        self.ground = None
        self.current_state = "IDLE" if fresh else "RESPAWN"
        self.is_knocked_down = False
        self.current_move, self.move_frame, self.hit_log, self.move_from_air = None, 0, set(), False
        self.jumps_left = self.air_jumps
        self.move_x = self.in_x = self.in_y = 0
        self.aim_dir, self.visual_aim = (0, -1), (facing, 0)
        self.hitstop = self.hitstun = self.stun_timer = self.lag = 0
        self.frozen = False
        self.shield_hp, self.shield_frame, self.shield_stun, self.shield_held = SHIELD_MAX, 0, 0, False
        self.intangible = 0
        self.dodge_timer, self.air_dodged = 0, False
        self.fast_fall = self.jump_cut = False
        self.drop_timer = self.ledge_cd = self.ledge_timer = 0
        self.ledge = None
        self.state_timer = 0
        self.buff_timer = 0
        self.respawn_timer = 0
        self.run_frames = 0
        self.flash = 0
        self.anim = random.random() * 100

    @property
    def rect(self):
        return pygame.Rect(int(self.x - self.w / 2), int(self.y - self.h), self.w, self.h)

    @property
    def center(self):
        return (self.x, self.y - self.h / 2)

    @property
    def alive(self):
        return self.current_lives > 0 and self.current_state != "KO"


class PlatformData:
    def __init__(self, x, y, w, h, solid=True, motion=None, ledges=None):
        self.base_x, self.base_y = x, y
        self.x, self.y, self.width, self.height = float(x), float(y), w, h
        self.is_solid = solid                 # solid = walls + ceiling; soft = can jump up / drop through
        self.motion = motion                  # (amp_x, amp_y, period_frames, phase)
        self.ledges = solid if ledges is None else ledges
        self.dx = self.dy = 0.0
        self.prev_top = y
        rng = random.Random(x * 7 + y)
        self.jag = [rng.uniform(0.5, 1.0) for _ in range(8)]
        self.rect = pygame.Rect(x, y, w, h)

    def update(self, frame):
        self.prev_top = self.y
        if self.motion:
            ax, ay, period, phase = self.motion
            a = frame / period * math.tau + phase
            nx, ny = self.base_x + ax * math.sin(a), self.base_y + ay * math.sin(a)
            self.dx, self.dy = nx - self.x, ny - self.y
            self.x, self.y = nx, ny
            self.rect.topleft = (int(nx), int(ny))


class Projectile:
    def __init__(self, owner, spec, x, y, facing):
        self.owner, self.facing = owner, facing
        self.x, self.y = x, y
        self.w, self.h = spec['w'], spec['h']
        self.vx, self.vy = spec.get('vx', 0) * facing, spec.get('vy', 0)
        self.gravity = spec.get('gravity', 0)
        self.life = spec['life']
        self.kind = spec['kind']
        self.rehit = spec.get('rehit', 0)         # >0: can hit the same target again every N frames
        self.sturdy = spec.get('sturdy', False)   # survives hits and blocks enemy projectiles
        self.ghost = spec.get('ghost', False)     # never collides with other projectiles
        self.grounded = spec.get('grounded', False)
        self.hit = hb(0, 0, 0, 0, self.w, self.h, spec['damage'], spec['force'], spec['angle'],
                      pull=spec.get('pull', False))
        self.color = owner.colors['accent']
        self.hit_log, self.age, self.dead = {}, 0, False

    @property
    def rect(self):
        return pygame.Rect(int(self.x - self.w / 2), int(self.y - self.h / 2), self.w, self.h)


class Stage:
    def __init__(self, key, name, blurb, platforms, palette):
        self.key, self.name, self.blurb, self.platforms, self.palette = key, name, blurb, platforms, palette
        solids = [p for p in platforms if p.is_solid]
        self.left = min(p.x for p in solids)
        self.right = max(p.x + p.width for p in solids)
        self.top = min(p.y for p in solids)
        self.blast = BLAST_ZONE
        self.spawns = [(WORLD_W // 2 - 300, self.top, 1), (WORLD_W // 2 + 300, self.top, -1)]
        self.respawn = (WORLD_W // 2, self.top - 380)
        self.bg = None

    def reset(self):
        for p in self.platforms:
            p.x, p.y, p.dx, p.dy, p.prev_top = float(p.base_x), float(p.base_y), 0.0, 0.0, p.base_y
            p.update(0)


def build_stages():
    P = PlatformData
    return [
        Stage('atoll', "Halcyon Atoll", ["A calm sky-island at dusk,", "with one drifting cloud-step."], [
            P(750, 820, 900, 110),
            P(850, 675, 220, 14, solid=False), P(1330, 675, 220, 14, solid=False),
            P(1100, 525, 200, 14, solid=False, motion=(200, 0, 540, 0)),
        ], dict(sky=((38, 44, 110), (250, 150, 118)), rock=(92, 74, 96), top=(120, 214, 150), soft=(255, 236, 200))),
        Stage('loom', "The Great Loom", ["Two islands stitched together by", "a thread-bridge you can drop through."], [
            P(600, 820, 460, 110), P(1340, 820, 460, 110),
            P(1050, 820, 300, 14, solid=False),
            P(1080, 650, 240, 14, solid=False),
            P(640, 675, 170, 14, solid=False), P(1590, 675, 170, 14, solid=False),
        ], dict(sky=((10, 8, 34), (72, 34, 104)), rock=(54, 44, 92), top=(255, 200, 110), soft=(255, 214, 130))),
        Stage('crucible', "Sentinel's Crucible", ["A small forge-top arena.", "Rising lifts guard the edges."], [
            P(820, 840, 760, 110),
            P(1100, 695, 200, 14, solid=False),
            P(590, 700, 170, 14, solid=False, motion=(0, 150, 420, 0)),
            P(1640, 700, 170, 14, solid=False, motion=(0, 150, 420, math.pi)),
        ], dict(sky=((18, 12, 20), (150, 52, 24)), rock=(58, 52, 60), top=(150, 230, 255), soft=(255, 150, 80))),
    ]


# =====================================================================
# CHARACTER ROSTER  (frame data for every move lives here)
# =====================================================================

def A(name, dur, *boxes, **kw):
    return Move(name, "Attack", dur, boxes, 0.0, **kw)


def S(name, dur, cost, *boxes, **kw):
    return Move(name, kw.pop('kind', "Special"), dur, boxes, cost, **kw)


def G(name, dur, x, w, h, dmg, force):
    return Move(name, "Grab", dur, [hb(dur // 5, dur // 5 + 3, x, -4, w, h, dmg, force, 45, grab=True)], 0.0)


KAI_MOVE_SET = dict(
    jab=A("Quick Jab", 14, hb(3, 5, 38, -6, 44, 28, 3, 4, 60)),
    ftilt=A("Thread Slash", 22, hb(5, 8, 48, -4, 64, 30, 7, 9, 35)),
    utilt=A("Crescent Stitch", 22, hb(4, 9, 10, -52, 70, 50, 6, 9, 85)),
    dtilt=A("Low Hem", 18, hb(4, 6, 44, 30, 60, 18, 5, 8, 75)),
    dash=A("Slipknot", 28, hb(5, 12, 34, 6, 56, 34, 7, 10, 60), impulses={0: (11, None)}),
    fsmash=A("Burst Strike", 40, hb(12, 15, 58, -6, 76, 40, 15, 16.5, 38)),
    usmash=A("Spindle Rise", 40, hb(9, 14, 6, -64, 60, 70, 13, 16, 88)),
    dsmash=A("Seam Splitter", 42, hb(8, 11, 50, 26, 64, 24, 12, 15, 25), hb(8, 11, -50, 26, 64, 24, 12, 15, 155)),
    nair=A("Whirl", 30, hb(4, 8, 0, 0, 90, 80, 3, 5, 70), hb(12, 16, 0, 0, 96, 84, 5, 9, 50, gid=1)),
    fair=A("Needle Rush", 28, hb(6, 10, 48, -4, 60, 36, 8, 10.5, 40)),
    bair=A("Back Lash", 30, hb(7, 10, -50, -4, 62, 32, 11, 14, 150)),
    uair=A("Sky Thread", 26, hb(5, 10, 4, -54, 56, 48, 7, 10, 85)),
    dair=A("Plumb Line", 34, hb(10, 16, 6, 48, 36, 52, 10, 12, 270), landing_lag=10),
    nspecial=S("Thread Needle", 26, 8, projectile=dict(frame=8, x=30, y=-8, w=30, h=8, vx=16, life=48,
                                                        damage=5, force=6, angle=30, kind='needle')),
    sspecial=S("Stitch Dash", 34, 15, hb(7, 14, 10, 0, 74, 50, 10, 11.5, 55),
               impulses={6: (24, 0), 15: (4, None)}, no_gravity=(6, 14)),
    uspecial=S("Skyhook", 40, 0, hb(5, 9, 6, -30, 56, 70, 2, 4, 85), hb(10, 14, 6, -30, 56, 70, 2, 4, 85, gid=1),
               hb(15, 19, 6, -34, 64, 76, 5, 11, 80, gid=2), impulses={5: (3, -19)}, helpless=True),
    dspecial=S("Aether Burst", 46, 30, hb(14, 20, 0, -4, 200, 180, 15, 17, 60, radial=True)),
    grab=G("Snare", 30, 34, 36, 40, 7, 10),
)

BRONN_MOVE_SET = dict(
    jab=A("Gauntlet Check", 22, hb(6, 9, 46, -8, 50, 36, 6, 6.5, 50)),
    ftilt=A("Shield Bash", 30, hb(9, 13, 54, -6, 64, 48, 11, 12, 35)),
    utilt=A("Lintel Lift", 30, hb(8, 13, 8, -66, 84, 48, 10, 12, 88)),
    dtilt=A("Foundation Kick", 26, hb(7, 10, 52, 36, 70, 22, 9, 10, 30)),
    dash=A("Rampart Rush", 40, hb(8, 18, 40, 0, 60, 70, 12, 13, 50), impulses={0: (10, None)}, armor=(4, 16)),
    fsmash=A("Keystone Hammer", 54, hb(20, 24, 70, -10, 90, 70, 22, 19, 40), armor=(14, 24)),
    usmash=A("Tower Crown", 50, hb(15, 21, 0, -70, 100, 100, 19, 18, 90)),
    dsmash=A("Ground Slam", 56, hb(18, 22, 0, 36, 230, 36, 20, 17.5, 80, radial=True)),
    nair=A("Rolling Boulder", 38, hb(6, 20, 0, 0, 96, 100, 10, 11, 45), landing_lag=10),
    fair=A("Maul Swing", 44, hb(14, 18, 54, 0, 70, 80, 15, 15, 35), landing_lag=12),
    bair=A("Buttress", 36, hb(10, 14, -56, -4, 70, 50, 14, 15, 145), landing_lag=10),
    uair=A("Capstone", 34, hb(8, 13, 0, -64, 80, 50, 12, 13, 90)),
    dair=A("Portcullis", 46, hb(12, 30, 0, 50, 60, 50, 16, 14, 270), impulses={12: (0, 16)}, landing_lag=18),
    nspecial=S("Guard Stance", 44, 0, counter=(4, 26), kind="Defensive"),
    sspecial=S("Bulwark Charge", 50, 15, hb(11, 30, 40, -4, 60, 80, 14, 15, 45),
               impulses={10: (14, None)}, armor=(8, 30)),
    uspecial=S("Rising Bastion", 50, 0, hb(8, 20, 0, -30, 80, 90, 12, 13, 80),
               impulses={8: (4, -19)}, armor=(4, 20), helpless=True),
    dspecial=S("Stone Wall", 40, 25, projectile=dict(frame=14, x=80, y=-14, w=52, h=120, life=260, gravity=0.8,
                                                      damage=12, force=13, angle=65, kind='wall', rehit=50,
                                                      sturdy=True, grounded=True)),
    grab=G("Iron Grip", 36, 40, 44, 50, 10, 12),
)

NYX_MOVE_SET = dict(
    jab=A("Spark Tap", 16, hb(4, 6, 40, -8, 46, 26, 3, 4.5, 45)),
    ftilt=A("Orbit Swipe", 26, hb(7, 11, 56, -6, 74, 30, 8, 10, 35)),
    utilt=A("Halo", 26, hb(6, 12, 0, -60, 90, 36, 7, 9.5, 90)),
    dtilt=A("Umbral Sweep", 22, hb(5, 8, 50, 32, 72, 18, 6, 8, 80)),
    dash=A("Comet Slide", 32, hb(6, 14, 36, 10, 60, 36, 8, 10, 65), impulses={0: (10, None)}),
    fsmash=A("Nova Lance", 46, hb(15, 18, 80, -8, 120, 30, 16, 17, 35)),
    usmash=A("Zenith Flare", 46, hb(12, 18, 0, -76, 70, 110, 15, 16.5, 90)),
    dsmash=A("Eclipse Ring", 48, hb(10, 14, 0, 28, 210, 34, 13, 15, 30, radial=True)),
    nair=A("Constellation", 34, hb(5, 8, 0, 0, 110, 110, 2, 3, 80), hb(11, 14, 0, 0, 110, 110, 2, 3, 80, gid=1),
           hb(17, 21, 0, 0, 120, 120, 5, 10, 50, gid=2, radial=True)),
    fair=A("Starfall Fan", 32, hb(8, 12, 54, -2, 70, 56, 9, 11, 45)),
    bair=A("Umbra Kick", 30, hb(7, 10, -52, 0, 64, 34, 10, 13, 150)),
    uair=A("Moonbeam", 30, hb(6, 12, 0, -66, 40, 80, 8, 10.5, 90)),
    dair=A("Meteor Seed", 36, hb(11, 15, 0, 50, 44, 44, 11, 12, 270), landing_lag=10),
    nspecial=S("Star Orb", 30, 8, projectile=dict(frame=12, x=34, y=-6, w=34, h=34, vx=6, life=110,
                                                   damage=8, force=9.5, angle=45, kind='orb')),
    sspecial=S("Comet Lance", 36, 15, projectile=dict(frame=14, x=40, y=-6, w=64, h=14, vx=22, life=40,
                                                       damage=11, force=12, angle=30, kind='lance')),
    uspecial=S("Blink", 34, 0, aim=(34, 10, 15), intangible=(8, 17), no_gravity=(0, 17), helpless=True),
    dspecial=S("Gravity Well", 38, 25, projectile=dict(frame=14, x=120, y=-10, w=130, h=130, life=160,
                                                        damage=2, force=6, angle=90, kind='well', rehit=14,
                                                        pull=True, ghost=True)),
    grab=G("Tether", 38, 60, 80, 30, 8, 10.5),
)

RHEA_MOVE_SET = dict(
    jab=A("One-Two", 18, hb(3, 5, 36, -8, 44, 30, 3, 3, 80), hb(9, 11, 40, -8, 48, 32, 4, 6, 45, gid=1)),
    ftilt=A("Ember Hook", 24, hb(6, 9, 44, -8, 52, 36, 9, 10.5, 40)),
    utilt=A("Flare Upper", 24, hb(5, 10, 14, -50, 50, 64, 8, 10, 85)),
    dtilt=A("Coal Sweep", 20, hb(5, 8, 44, 32, 60, 20, 7, 8.5, 70)),
    dash=A("Wildfire Knee", 30, hb(5, 13, 34, -2, 50, 50, 10, 11.5, 55), impulses={0: (12, None)}),
    fsmash=A("Kiln Breaker", 46, hb(14, 18, 56, -8, 70, 50, 19, 18, 38), impulses={12: (8, None)}),
    usmash=A("Eruption", 44, hb(10, 16, 8, -66, 56, 90, 16, 17, 90)),
    dsmash=A("Magma Stomp", 46, hb(12, 16, 0, 30, 190, 32, 15, 16, 35, radial=True)),
    nair=A("Cinder Wheel", 30, hb(4, 16, 0, 0, 84, 84, 8, 9.5, 50)),
    fair=A("Hammer Fist", 34, hb(10, 13, 44, 6, 54, 60, 13, 13, 300), landing_lag=10),
    bair=A("Backdraft", 30, hb(7, 10, -46, -6, 56, 40, 12, 14.5, 150)),
    uair=A("Spark Flip", 28, hb(5, 11, 0, -56, 70, 44, 9, 11, 80)),
    dair=A("Meteor Kick", 40, hb(8, 30, 16, 40, 46, 46, 11, 11.5, 60), impulses={8: (7, 17)}, landing_lag=14),
    nspecial=S("Flare Jab", 34, 10, hb(12, 16, 66, -6, 96, 72, 14, 15.5, 45)),
    sspecial=S("Comet Tackle", 40, 15, hb(9, 22, 30, 0, 60, 60, 12, 13.5, 50), impulses={8: (17, -5)}),
    uspecial=S("Pyre Uppercut", 42, 0, hb(6, 10, 10, -30, 56, 76, 5, 5, 88),
               hb(11, 20, 10, -36, 60, 80, 9, 13.5, 75, gid=1), impulses={6: (5, -19)}, helpless=True),
    dspecial=S("Ignite", 36, 30, hb(18, 22, 0, 0, 130, 120, 5, 8, 70, radial=True), effect=('ignite', 18)),
    grab=G("Clinch", 30, 34, 36, 44, 9, 11.5),
)

ROSTER = [
    dict(name="Kai", title="The Weaver", faction="Weaver", style='weaver', moves=KAI_MOVE_SET,
         blurb=["Thread-blade duelist. Fastest on the", "ground, strings quick hits into combos,", "but light and easy to launch."],
         stats=dict(speed=8.2, air_speed=6.2, jump_height=15, double_jump=14, gravity=0.68, max_fall=12,
                    weight=86, w=44, h=78),
         bars=(5, 2, 2, 3), ai=dict(mid=['nspecial', 'sspecial'], close=['dspecial']),
         colors=dict(main=(214, 48, 72), dark=(110, 22, 44), accent=(255, 214, 120), skin=(255, 222, 196)),
         alt=dict(main=(40, 170, 160), dark=(16, 80, 84), accent=(255, 240, 170), skin=(255, 222, 196))),
    dict(name="Bronn", title="The Sentinel", faction="Sentinel", style='sentinel', moves=BRONN_MOVE_SET,
         blurb=["Stone-clad guardian. Slow and heavy with", "armored attacks, a counter stance and", "walls that punish reckless aggression."],
         stats=dict(speed=5.4, air_speed=4.4, jump_height=15.5, double_jump=13.5, gravity=0.78, max_fall=14,
                    weight=124, w=64, h=94),
         bars=(2, 5, 5, 3), ai=dict(mid=['sspecial', 'dspecial'], close=['nspecial']),
         colors=dict(main=(70, 120, 190), dark=(34, 56, 104), accent=(150, 230, 255), skin=(120, 132, 150)),
         alt=dict(main=(176, 120, 60), dark=(90, 56, 24), accent=(255, 226, 150), skin=(140, 124, 110))),
    dict(name="Nyx", title="The Hollow Star", faction="Voidcaller", style='voidcaller', moves=NYX_MOVE_SET,
         blurb=["Floaty star-mage with three jumps.", "Controls space with orbs, lances and a", "gravity well; teleports to recover."],
         stats=dict(speed=6.2, air_speed=5.6, jump_height=13.5, double_jump=12.5, gravity=0.46, max_fall=8.5,
                    weight=88, w=42, h=82, air_jumps=2),
         bars=(3, 3, 2, 5), ai=dict(mid=['nspecial', 'sspecial', 'dspecial'], close=[]),
         colors=dict(main=(112, 72, 196), dark=(46, 28, 96), accent=(255, 150, 240), skin=(30, 20, 60)),
         alt=dict(main=(36, 70, 130), dark=(14, 30, 64), accent=(140, 255, 220), skin=(12, 24, 50))),
    dict(name="Rhea", title="The Cinderfist", faction="Kindled", style='kindled', moves=RHEA_MOVE_SET,
         blurb=["Close-range brawler. Short reach but", "every punch hurts. Spend Aether to", "Ignite and hit 30% harder."],
         stats=dict(speed=7.0, air_speed=5.4, jump_height=15, double_jump=13.5, gravity=0.72, max_fall=13,
                    weight=104, w=52, h=84),
         bars=(4, 4, 3, 1), ai=dict(mid=['sspecial', 'dspecial'], close=['nspecial']),
         colors=dict(main=(235, 120, 40), dark=(120, 48, 20), accent=(255, 224, 90), skin=(232, 180, 140)),
         alt=dict(main=(70, 74, 92), dark=(30, 32, 44), accent=(96, 206, 255), skin=(232, 180, 140))),
]
for _spec in ROSTER:
    for _slot, _move in _spec['moves'].items():
        _move.slot = _slot


# =====================================================================
# EFFECTS (particles, rings, pop-up text, screen shake)
# =====================================================================

class Effects:
    def __init__(self):
        self.particles, self.rings, self.texts, self.shake = [], [], [], 0

    def spark(self, x, y, color, n=10, speed=6.0, life=18, size=4, grav=0.0, direction=None, spread=math.pi):
        for _ in range(n):
            a = (direction if direction is not None else 0) + random.uniform(-spread, spread)
            s = speed * random.uniform(0.3, 1.0)
            self.particles.append([x, y, math.cos(a) * s, math.sin(a) * s, random.randint(life // 2, life), life, color, size, grav])

    def ring(self, x, y, color, r=16, grow=6, life=14):
        self.rings.append([x, y, r, grow, life, life, color])

    def text(self, x, y, s, color, life=45):
        self.texts.append([x, y, s, color, life])

    def update(self):
        for p in self.particles:
            p[0] += p[2]; p[1] += p[3]; p[3] += p[8]; p[2] *= 0.94; p[3] *= 0.94; p[4] -= 1
        self.particles = [p for p in self.particles if p[4] > 0][-600:]
        for r in self.rings:
            r[2] += r[3]; r[4] -= 1
        self.rings = [r for r in self.rings if r[4] > 0]
        for t in self.texts:
            t[1] -= 0.8; t[4] -= 1
        self.texts = [t for t in self.texts if t[4] > 0]
        self.shake = max(0, self.shake - 1)

    def draw(self, surf, font):
        for x, y, vx, vy, life, mx, color, size, _ in self.particles:
            r = max(1, int(size * life / mx))
            pygame.draw.circle(surf, color, (int(x), int(y)), r)
        for x, y, r, _, life, mx, color in self.rings:
            pygame.draw.circle(surf, color, (int(x), int(y)), int(r), max(1, int(6 * life / mx)))
        for x, y, s, color, life in self.texts:
            img = font.render(s, True, color)
            surf.blit(img, (x - img.get_width() / 2, y))


# =====================================================================
# 2. PHYSICS ENGINE (Module 2)
# =====================================================================

class PhysicsEngine:
    def __init__(self, map_data, combat_manager, fx):
        self.map = map_data                  # a Stage: .platforms, .blast
        self.combat_manager = combat_manager
        self.fx = fx
        self.active_characters = []
        self.projectiles = []
        self.frame = 0
        self.game_over = False

    def register_character(self, character):
        self.active_characters.append(character)

    # ------------------------------------------------------------ main update
    def update_all(self):
        """Runs once per frame: platforms, movement, collisions, blast zones, hitboxes."""
        self.frame += 1
        for p in self.map.platforms:
            p.update(self.frame)
        for c in self.active_characters:
            c.frozen = c.hitstop > 0
            if c.frozen:
                c.hitstop -= 1                       # "hitstop": both fighters freeze briefly on impact
                continue
            if c.current_state in ("KO", "RESPAWN"):
                continue
            if c.current_state == "LEDGE":
                continue
            self._apply_move_effects(c)
            self._apply_gravity(c)
            self._apply_movement(c)
            self._check_platform_collision(c)
            self._check_ledges(c)
            self._sync_ground_state(c)
            if not self.map.blast.collidepoint(c.x, c.y - c.h / 2):
                self.combat_manager.handle_ko(c)
        self._update_projectiles()
        self._update_active_hitboxes()

    # ------------------------------------------------------------ per-move scripted motion
    def _apply_move_effects(self, c):
        m, f = c.current_move, c.move_frame
        if not m:
            return
        if f in m.impulses:
            ix, iy = m.impulses[f]
            if ix is not None:
                c.vx = ix * c.facing
            if iy is not None:
                c.vy = iy
                if iy < 0:
                    c.ground = None
        if m.aim:
            speed, a0, a1 = m.aim
            if a0 <= f <= a1:
                c.vx, c.vy = c.aim_dir[0] * speed, c.aim_dir[1] * speed
                if c.vy < 0:
                    c.ground = None
                if f == a0:
                    self.fx.ring(c.x, c.y - c.h / 2, c.colors['accent'], 10, 8)
            elif f == a1 + 1:
                c.vx, c.vy = c.vx * 0.1, 0
                self.fx.ring(c.x, c.y - c.h / 2, c.colors['accent'], 40, -3)
        if m.intangible and m.intangible[0] <= f <= m.intangible[1]:
            c.intangible = max(c.intangible, 2)
        if m.projectile and f == m.projectile['frame']:
            ps = m.projectile
            self.projectiles.append(Projectile(c, ps, c.x + ps['x'] * c.facing, c.y - c.h / 2 + ps['y'], c.facing))
        if m.effect and f == m.effect[1] and m.effect[0] == 'ignite':
            c.buff_timer = 480
            self.fx.ring(c.x, c.y - c.h / 2, c.colors['accent'], 20, 9, 20)
            self.fx.text(c.x, c.y - c.h - 40, "IGNITED!", c.colors['accent'])

    def _apply_gravity(self, c):
        if c.ground:
            return
        m = c.current_move
        if m and m.no_gravity and m.no_gravity[0] <= c.move_frame <= m.no_gravity[1]:
            return
        g = c.gravity * (HITSTUN_GRAVITY if c.hitstun > 0 else 1.0)
        cap = c.max_fall * (1.6 if c.fast_fall else 1.0)
        c.vy = min(c.vy + g, max(cap, c.vy))
        if c.fast_fall and c.vy > 0:
            c.vy = max(c.vy, cap)

    def _apply_movement(self, c):
        """Turns the controller's intent (move_x) into velocity. Positions change in the collision step."""
        st = c.current_state
        m = c.current_move
        scripted = m and m.no_gravity and m.no_gravity[0] <= c.move_frame <= m.no_gravity[1]
        if c.ground:
            if st in ("IDLE", "RUNNING", "PARRY_SUCCESS") and c.lag == 0:
                target = c.move_x * c.speed
                accel = c.speed * 0.22
                c.vx += clamp(target - c.vx, -accel, accel)
            elif st == "DODGE":
                c.vx *= 0.93
            elif not scripted:
                c.vx *= 0.88
        elif not scripted:
            if st in ("JUMPING", "HELPLESS", "ATTACKING") and c.move_x:
                nv = c.vx + c.move_x * c.air_speed * 0.09
                if abs(nv) <= c.air_speed or abs(nv) < abs(c.vx):
                    c.vx = nv
                elif abs(c.vx) > c.air_speed:
                    c.vx *= 0.97
            else:
                c.vx *= 0.95
        # knockback is its own velocity that decays at a constant rate
        sp = math.hypot(c.kbx, c.kby)
        if sp > 0:
            ns = max(0.0, sp - KB_DECAY * (2.0 if c.ground else 1.0))
            c.kbx, c.kby = c.kbx * ns / sp, c.kby * ns / sp

    # ------------------------------------------------------------ platforms
    @staticmethod
    def _overlaps(c, p):
        return (c.x - c.w / 2 < p.x + p.width and c.x + c.w / 2 > p.x and
                c.y - c.h < p.y + p.height and c.y > p.y)

    def _check_platform_collision(self, c):
        """Moves the character, then resolves walls, ceilings and landings."""
        dx, dy = c.vx + c.kbx, c.vy + c.kby
        half, margin = c.w / 2, c.w * 0.3
        if c.ground:                                     # ride the platform we stand on
            g = c.ground
            c.x += g.dx
            c.y = g.y
            if dy < 0:
                c.ground = None
            else:
                dy = 0
                c.vy = 0
                c.kby = min(c.kby, 0)
                if not (g.x - margin <= c.x + dx <= g.x + g.width + margin):
                    c.ground = None                      # walked off the edge...
                    for p in self.map.platforms:         # ...unless another platform continues at this height
                        if p is not g and abs(p.y - c.y) < 2 and p.x - margin <= c.x + dx <= p.x + p.width + margin:
                            c.ground = p
        prev_y = c.y
        # horizontal step: solid platforms act as walls
        c.x += dx
        for p in self.map.platforms:
            if p.is_solid and self._overlaps(c, p) and c.y > p.y + 8:
                from_left = dx > 0 if dx else c.x < p.x + p.width / 2
                c.x = p.x - half if from_left else p.x + p.width + half
                c.vx = 0
                c.kbx *= -0.4
        # vertical step
        c.y += dy
        if c.ground:
            return
        if dy < 0:
            for p in self.map.platforms:                 # bump head on the underside of solid platforms
                if p.is_solid and self._overlaps(c, p):
                    c.y = p.y + p.height + c.h
                    c.vy = 0
                    c.kby = 0
            return
        for p in self.map.platforms:                     # landing: feet crossed the top surface this frame
            if not p.is_solid and (c.drop_timer > 0):
                continue
            if prev_y <= p.prev_top + 1 and c.y >= p.y and p.x - margin <= c.x <= p.x + p.width + margin:
                self._land(c, p)
                break

    def _land(self, c, p):
        c.y, c.vy, c.ground = p.y, 0.0, p
        c.kby = 0.0
        c.jumps_left = c.air_jumps
        c.air_dodged = c.fast_fall = c.jump_cut = False
        c.ledge_cd = 0
        if abs(c.vx + c.kbx) > 3 or c.current_state != "JUMPING":
            self.fx.spark(c.x, c.y, (220, 220, 220), 5, 3, 12, 3, direction=-math.pi / 2, spread=1.3)
        m = c.current_move
        if m and c.move_from_air and (m.move_type == "Attack" or m.helpless):
            self.combat_manager.end_move(c)              # aerials are cancelled by landing (with landing lag)
            c.lag = m.landing_lag if m.move_type == "Attack" else 10
        elif c.current_state == "HELPLESS":
            c.current_state = "IDLE"
            c.lag = 8
        elif c.current_state == "DODGE":
            c.dodge_timer = min(c.dodge_timer, 8)

    def _sync_ground_state(self, c):
        st = c.current_state
        if c.ground is None and st in ("IDLE", "RUNNING", "SHIELD"):
            c.current_state = "JUMPING"
        elif c.ground is not None and st == "JUMPING":
            c.current_state = "IDLE"

    # ------------------------------------------------------------ ledges
    def _check_ledges(self, c):
        if c.ground or c.ledge_cd > 0 or c.hitstun > 0 or c.in_y == 1 or c.vy + c.kby <= 0:
            return
        m = c.current_move
        if not (c.current_state in ("JUMPING", "HELPLESS") or (m and m.helpless)):
            return
        top = c.y - c.h
        for p in self.map.platforms:
            if not p.ledges:
                continue
            for lx, inward in ((p.x, 1), (p.x + p.width, -1)):
                gap = (lx - c.x) * inward               # how far outside the edge we are
                if 0 <= gap <= c.w / 2 + 28 and -26 <= top - p.y <= 46:
                    if m:
                        self.combat_manager.end_move(c)
                    c.current_state = "LEDGE"
                    c.ledge = (p, lx, inward)
                    c.x, c.y = lx - inward * (c.w / 2 + 2), p.y + c.h - 8
                    c.vx = c.vy = c.kbx = c.kby = 0.0
                    c.facing = inward
                    c.ledge_timer = 0
                    c.jumps_left = c.air_jumps
                    c.air_dodged = c.fast_fall = False
                    c.intangible = max(c.intangible, 20)
                    return

    # ------------------------------------------------------------ projectiles
    def _update_projectiles(self):
        for p in self.projectiles:
            p.age += 1
            p.life -= 1
            p.vy += p.gravity
            prev_bottom = p.y + p.h / 2
            p.x += p.vx
            p.y += p.vy
            if p.grounded and p.vy > 0:
                for pl in self.map.platforms:
                    if prev_bottom <= pl.y + 1 and p.y + p.h / 2 >= pl.y and pl.x <= p.x <= pl.x + pl.width:
                        p.y, p.vy, p.gravity = pl.y - p.h / 2, 0, 0
                        self.fx.spark(p.x, pl.y, (200, 200, 210), 8, 5, 14, 4, direction=-math.pi / 2, spread=1.4)
            if p.life <= 0 or not self.map.blast.collidepoint(p.x, p.y):
                p.dead = True
        live = [p for p in self.projectiles if not p.dead]
        for i, a in enumerate(live):                     # projectiles of different owners cancel each other
            for b in live[i + 1:]:
                if a.owner is not b.owner and not a.ghost and not b.ghost and a.rect.colliderect(b.rect):
                    for q in (a, b):
                        if not q.sturdy:
                            q.dead = True
                            self.fx.spark(q.x, q.y, q.color, 8, 5)
        for p in live:
            if p.dead:
                continue
            for t in self.active_characters:
                if t is p.owner or not t.alive:
                    continue
                last = p.hit_log.get(t.id)
                if last is not None and (p.rehit == 0 or p.age - last < p.rehit):
                    continue
                if p.rect.colliderect(t.rect):
                    p.hit_log[t.id] = p.age
                    result = self.combat_manager.handle_collision_event(p.owner, p.hit, t, source=p)
                    if result != 'ignored' and result != 'reflect' and not p.rehit:
                        p.dead = True
        self.projectiles = [p for p in self.projectiles if not p.dead]

    # ------------------------------------------------------------ hitboxes
    def hitbox_rect(self, c, box):
        x, y, w, h = box['rect']
        return pygame.Rect(int(c.x + x * c.facing - w / 2), int(c.y - c.h / 2 + y - h / 2), w, h)

    def active_hitboxes(self, c):
        m = c.current_move
        if not m or c.current_state != "ATTACKING":
            return []
        return [b for b in m.hitbox_data if b['start'] <= c.move_frame <= b['end']]

    def _update_active_hitboxes(self):
        """AABB check of every active hitbox against every other fighter's hurtbox."""
        events = []
        for attacker in self.active_characters:
            if attacker.frozen:
                continue
            for box in self.active_hitboxes(attacker):
                rect = self.hitbox_rect(attacker, box)
                for target in self.active_characters:
                    key = (target.id, box['gid'])
                    if target is attacker or not target.alive or key in attacker.hit_log:
                        continue
                    if rect.colliderect(target.rect):
                        attacker.hit_log.add(key)
                        events.append((attacker, box, target))
        for attacker, box, target in events:             # resolved together so two attacks can trade
            self.combat_manager.handle_collision_event(attacker, box, target)

    # ------------------------------------------------------------ knockback
    def apply_knockback(self, attacker, target, base_force, direction):
        """CORE MECHANIC: the more damage you have taken, the further you fly."""
        knockback_modifier = 1.0 + (target.cumulative_damage_taken / KNOCKBACK_DIVISOR)
        final_force = base_force * knockback_modifier
        speed = final_force * 200.0 / (100.0 + target.weight)     # heavier fighters resist the same force
        dx, dy = direction
        if target.ground and dy > 0.2:                   # spiked into the floor: bounce back up
            dy = -dy * 0.8
        target.vx = target.vy = 0.0
        target.kbx, target.kby = speed * dx, speed * dy
        if target.kby < 0:
            target.ground = None
        return speed

    def signal_game_over(self):
        self.game_over = True


# =====================================================================
# 3. COMBAT MANAGER (Module 3)
# =====================================================================

class CombatManager:
    def __init__(self, physics_engine, fx):
        self.physics = physics_engine        # injected after construction (the two modules reference each other)
        self.fx = fx

    # ------------------------------------------------------------ starting / ending moves
    def process_attack(self, attacker, move):
        if isinstance(move, str):
            move = attacker.move_set[move]
        if attacker.aether_gauge < move.required_aether:
            attacker.flash = 20
            self.fx.text(attacker.x, attacker.y - attacker.h - 36, "NO AETHER", (150, 220, 255), 30)
            return False
        attacker.aether_gauge -= move.required_aether
        attacker.current_move, attacker.move_frame, attacker.hit_log = move, 0, set()
        attacker.move_from_air = attacker.ground is None
        attacker.current_state = "ATTACKING"
        attacker.fast_fall = False
        ix, iy = attacker.in_x, attacker.in_y
        n = math.hypot(ix, iy)
        attacker.aim_dir = (ix / n, iy / n) if n else (0, -1)
        if move.hitbox_data:
            bx, by = move.hitbox_data[0]['rect'][0] * attacker.facing, move.hitbox_data[0]['rect'][1]
            n = math.hypot(bx, by)
            attacker.visual_aim = (bx / n, by / n) if n > 8 else (attacker.facing, 0)
        else:
            attacker.visual_aim = (attacker.facing, 0)
        return True

    def end_move(self, c):
        m = c.current_move
        c.current_move = None
        if c.current_state == "ATTACKING":
            if c.ground is None:
                c.current_state = "HELPLESS" if (m and m.helpless) else "JUMPING"
            else:
                c.current_state = "IDLE"

    # ------------------------------------------------------------ hit resolution
    def handle_collision_event(self, attacker, hitbox, target, source=None):
        """Called by the Physics Engine when a hitbox (or projectile 'source') touches a hurtbox."""
        if target.intangible > 0 or target.current_state in ("KO", "RESPAWN"):
            return 'ignored'
        tm = target.current_move
        tx, ty = target.center
        damage = hitbox['damage'] * (IGNITE_MULT if attacker.buff_timer > 0 else 1.0)
        # --- defensive answers -------------------------------------------------
        if not hitbox['grab']:
            if tm and tm.counter and tm.counter[0] <= target.move_frame <= tm.counter[1]:
                return self.handle_parry(target, attacker, hitbox, source, counter=True)
            if target.current_state == "SHIELD":
                if target.shield_frame <= PARRY_WINDOW:
                    return self.handle_parry(target, attacker, hitbox, source)
                target.shield_hp -= damage * 1.1 + 1
                target.shield_stun = int(damage * 0.5) + 3
                target.vx += sign(tx - attacker.x) * 3
                if source is None:
                    attacker.hitstop = 4
                self.fx.ring(tx, ty, WHITE, 30, 3, 8)
                if target.shield_hp <= 0:
                    self._shield_break(target)
                return 'shield'
        # --- direction ---------------------------------------------------------
        force = hitbox['force']
        if hitbox['grab']:
            if target.current_state in ("HIT", "LEDGE"):
                return 'ignored'
            ang = 40                                   # throws: hold a direction to choose where
            if attacker.in_y < 0:
                ang = 88
            elif attacker.in_y > 0:
                ang = 300
            elif attacker.in_x == -attacker.facing:
                attacker.facing *= -1
            direction = (math.cos(math.radians(ang)) * attacker.facing, -math.sin(math.radians(ang)))
            target.x = attacker.x + attacker.facing * (attacker.w / 2 + target.w / 2)
        elif hitbox['pull'] and source is not None:
            vx, vy = source.x - tx, source.y - ty
            n = math.hypot(vx, vy)
            direction = (vx / n, vy / n) if n > 12 else (0, -1)
        elif hitbox['radial']:
            ox, oy = (source.x, source.y) if source else attacker.center
            vx, vy = tx - ox, (ty - oy) - 40
            n = math.hypot(vx, vy) or 1
            direction = (vx / n, vy / n)
        else:
            facing = source.facing if source else attacker.facing
            a = math.radians(hitbox['angle'])
            direction = (math.cos(a) * facing, -math.sin(a))
        # --- 1. apply damage + Aether --------------------------------------------
        target.cumulative_damage_taken += damage
        attacker.aether_gauge = min(AETHER_MAX, attacker.aether_gauge + damage * AETHER_PER_DAMAGE_DEALT)
        target.aether_gauge = min(AETHER_MAX, target.aether_gauge + damage * AETHER_PER_DAMAGE_TAKEN)
        stop = min(14, int(3 + damage * 0.45))
        target.hitstop = stop
        if source is None:
            attacker.hitstop = stop
        self.fx.spark(tx, ty, attacker.colors['accent'], 6 + int(damage), 4 + damage * 0.4, 20, 5)
        self.fx.spark(tx, ty, WHITE, 4, 9, 10, 3)
        self.fx.shake = max(self.fx.shake, int(damage * 0.5))
        # --- 2. super armor: take the damage but not the launch -------------------
        if tm and tm.armor and tm.armor[0] <= target.move_frame <= tm.armor[1] and not hitbox['grab'] \
                and damage < ARMOR_LIMIT:
            target.flash = 8
            return 'hit'
        # --- 3. dynamic knockback + hit / stun state ------------------------------
        speed = self.physics.apply_knockback(attacker, target, force, direction)
        self._enter_hit_state(target, speed)
        if speed > 22:
            self.fx.ring(tx, ty, attacker.colors['accent'], 20, 10, 16)
        return 'hit'

    def _enter_hit_state(self, target, speed):
        target.current_move = None
        target.current_state = "HIT"
        target.is_knocked_down = speed >= TUMBLE_SPEED
        target.hitstun = int(speed * HITSTUN_PER_SPEED) + 4
        target.stun_timer = target.dodge_timer = target.lag = 0
        target.fast_fall = target.jump_cut = False
        target.ledge = None

    def _shield_break(self, target):
        target.shield_hp = SHIELD_MAX * 0.4
        target.current_state = "STUN"
        target.is_knocked_down = True
        target.stun_timer = SHIELD_BREAK_STUN
        target.vy, target.ground = -11, None
        self.fx.ring(target.x, target.y - target.h / 2, (255, 255, 120), 20, 12, 22)
        self.fx.text(target.x, target.y - target.h - 40, "SHIELD BREAK!", (255, 255, 120), 60)
        self.fx.shake = 12

    def handle_parry(self, defender, attacker, hitbox, source=None, counter=False):
        """A perfectly timed shield (or Bronn's Guard Stance): no damage, +25 Aether, attacker punished."""
        defender.aether_gauge = min(AETHER_MAX, defender.aether_gauge + AETHER_PARRY_REWARD)
        dx, dy = defender.center
        self.fx.ring(dx, dy, (120, 255, 150), 20, 9, 18)
        self.fx.spark(dx, dy, (160, 255, 190), 16, 9, 20, 4)
        self.fx.text(defender.x, defender.y - defender.h - 40, "PARRY!  +25", (120, 255, 150), 50)
        self.fx.shake = max(self.fx.shake, 6)
        defender.hitstop = 8
        if not counter:
            defender.current_move = None
            defender.current_state = "PARRY_SUCCESS"
            defender.state_timer = 24
        defender.flash = 14
        if source is not None:                         # projectiles are sent back
            source.owner, source.vx, source.facing = defender, -source.vx * 1.3, -source.facing
            source.color = defender.colors['accent']
            source.hit_log = {}
            source.life = max(source.life, 40)
            return 'reflect'
        attacker.hitstop = 8
        attacker.current_move = None                   # penalize the aggressive player
        attacker.current_state = "STUN"
        attacker.is_knocked_down = True
        attacker.stun_timer = PARRY_STUN
        if counter:                                    # Guard Stance also hits back
            ax, ay = attacker.center
            attacker.cumulative_damage_taken += 10
            away = sign(attacker.x - defender.x) or defender.facing
            speed = self.physics.apply_knockback(defender, attacker, 13, (away * 0.7, -0.7))
            self._enter_hit_state(attacker, speed)
            self.fx.spark(ax, ay, defender.colors['accent'], 14, 8, 20, 5)
        return 'parry'

    # ------------------------------------------------------------ KO / respawn
    def handle_ko(self, c):
        b = self.physics.map.blast
        bx, by = clamp(c.x, b.left, b.right), clamp(c.y - c.h / 2, b.top, b.bottom)
        ang = math.atan2(WORLD_H / 2 - by, WORLD_W / 2 - bx)
        col = c.colors['main']
        self.fx.spark(bx, by, col, 50, 34, 40, 9, direction=ang, spread=0.35)
        self.fx.spark(bx, by, WHITE, 30, 26, 30, 6, direction=ang, spread=0.5)
        self.fx.ring(bx, by, col, 30, 16, 26)
        self.fx.shake = 18
        c.current_lives -= 1
        c.current_state = "KO"
        c.current_move = None
        c.is_knocked_down = False
        c.respawn_timer = 75
        c.vx = c.vy = c.kbx = c.kby = 0
        c.hitstun = c.hitstop = 0
        for o in self.physics.active_characters:
            if o is not c:
                o.kos += 1
        if c.current_lives <= 0:
            self.physics.signal_game_over()

    # ------------------------------------------------------------ per-frame timers
    def check_all_move_durations(self):
        """Advances every timer that drives the state machine (moves, stun, shields, respawns...)."""
        stage = self.physics.map
        for c in self.physics.active_characters:
            c.anim += 1
            if c.current_state == "KO":
                if c.current_lives > 0:
                    c.respawn_timer -= 1
                    if c.respawn_timer <= 0:
                        c.cumulative_damage_taken = 0.0
                        c.respawn_at(stage.respawn[0], stage.respawn[1], 1 if c.slot == 0 else -1)
                        c.state_timer = 150
                        c.intangible = 240
                continue
            if c.frozen:
                continue
            c.aether_gauge = min(AETHER_MAX, c.aether_gauge + AETHER_PASSIVE)
            for attr in ('intangible', 'lag', 'ledge_cd', 'drop_timer', 'buff_timer', 'flash', 'shield_stun'):
                v = getattr(c, attr)
                if v > 0:
                    setattr(c, attr, v - 1)
            st = c.current_state
            if c.current_move:
                c.move_frame += 1
                if c.move_frame >= c.current_move.duration:
                    self.end_move(c)
            if c.hitstun > 0:
                c.hitstun -= 1
                if c.hitstun == 0 and st == "HIT":
                    c.current_state = "JUMPING" if c.ground is None else "IDLE"
                    c.is_knocked_down = False
            if st == "STUN":
                c.stun_timer -= 1
                if c.stun_timer <= 0:
                    c.current_state = "JUMPING" if c.ground is None else "IDLE"
                    c.is_knocked_down = False
            elif st in ("PARRY_SUCCESS", "RESPAWN"):
                c.state_timer -= 1
                if c.state_timer <= 0:
                    c.current_state = "JUMPING" if c.ground is None else "IDLE"
            elif st == "DODGE":
                c.dodge_timer -= 1
                if c.dodge_timer <= 0:
                    c.current_state = "JUMPING" if c.ground is None else "IDLE"
                    c.lag = 4
            elif st == "LEDGE":
                c.ledge_timer += 1
                if c.ledge_timer > 200:
                    c.current_state, c.ledge_cd, c.ledge = "JUMPING", 40, None
            if st == "SHIELD":
                c.shield_frame += 1
                c.shield_hp -= 0.22
                if c.shield_hp <= 0:
                    self._shield_break(c)
            else:
                c.shield_hp = min(SHIELD_MAX, c.shield_hp + 0.1)
            if c.buff_timer > 0 and c.anim % 3 == 0:
                self.fx.spark(c.x + random.uniform(-c.w / 2, c.w / 2), c.y - random.uniform(0, c.h),
                              c.colors['accent'], 1, 2, 16, 4, grav=-0.25)


# =====================================================================
# 4. CHARACTER CONTROLLER (Module 4)
# =====================================================================

class InputState:
    """One frame of input, the same shape whether it came from a keyboard, a gamepad or the CPU."""
    def __init__(self):
        self.x, self.y = 0, 0            # stick: x -1/0/1, y -1 (up) / 0 / 1 (down)
        self.pressed, self.held = set(), set()   # 'jump', 'attack', 'heavy', 'special', 'shield'
        self.down_tap = False


KEYMAP_P1 = dict(left=[pygame.K_a], right=[pygame.K_d], up=[pygame.K_w], down=[pygame.K_s],
                 jump=[pygame.K_SPACE], attack=[pygame.K_j], heavy=[pygame.K_k], special=[pygame.K_l],
                 shield=[pygame.K_LSHIFT, pygame.K_i])
KEYMAP_P2 = dict(left=[pygame.K_LEFT], right=[pygame.K_RIGHT], up=[pygame.K_UP], down=[pygame.K_DOWN],
                 jump=[pygame.K_RCTRL, pygame.K_KP0], attack=[pygame.K_COMMA, pygame.K_KP1],
                 heavy=[pygame.K_PERIOD, pygame.K_KP2], special=[pygame.K_SLASH, pygame.K_KP3],
                 shield=[pygame.K_RSHIFT, pygame.K_KP_ENTER])
BUTTONS = ('jump', 'attack', 'heavy', 'special', 'shield')
PAD_BUTTONS = dict(jump=[0], special=[1], attack=[2], heavy=[3], shield=[4, 5])


class KeyboardDevice:
    def __init__(self, keymap, game, joystick=None):
        self.keymap, self.game, self.joy = keymap, game, joystick
        self.prev_pad, self.prev_down = set(), False

    def poll(self):
        inp, held, pressed, km = InputState(), self.game.keys_held, self.game.keys_pressed, self.keymap
        down = lambda name: any(held[k] for k in km[name])
        inp.x = int(down('right')) - int(down('left'))
        inp.y = int(down('down')) - int(down('up'))
        for b in BUTTONS:
            if down(b):
                inp.held.add(b)
            if any(k in pressed for k in km[b]):
                inp.pressed.add(b)
        if self.joy:                                      # optional gamepad, merged with the keys
            try:
                ax, ay = self.joy.get_axis(0), self.joy.get_axis(1)
                hat = self.joy.get_hat(0) if self.joy.get_numhats() else (0, 0)
                inp.x = inp.x or (sign(ax) if abs(ax) > 0.4 else 0) or hat[0]
                inp.y = inp.y or (sign(ay) if abs(ay) > 0.5 else 0) or -hat[1]
                now = {b for b, ids in PAD_BUTTONS.items()
                       if any(i < self.joy.get_numbuttons() and self.joy.get_button(i) for i in ids)}
                inp.held |= now
                inp.pressed |= now - self.prev_pad
                self.prev_pad = now
            except pygame.error:
                self.joy = None
        inp.down_tap = inp.y == 1 and not self.prev_down
        self.prev_down = inp.y == 1
        return inp


class CharacterController:
    def __init__(self, character_data, combat_manager, device=None):
        self.char = character_data
        self.combat = combat_manager
        self.device = device
        self.buffer = {}
        self.prev_x = 0

    def read_input(self):
        return self.device.poll()

    def _take(self, button):
        if self.buffer.get(button, 0) > 0:
            self.buffer[button] = 0
            return True
        return False

    def process_input(self, inp):
        """Translates raw input into commands, depending on what the fighter is currently able to do."""
        c = self.char
        c.in_x, c.in_y = inp.x, inp.y
        c.shield_held = 'shield' in inp.held
        for b in self.buffer:
            self.buffer[b] -= 1
        for b in inp.pressed:
            self.buffer[b] = BUFFER_FRAMES
        prev_x, self.prev_x = self.prev_x, inp.x
        st = c.current_state
        if st == "KO" or c.hitstop > 0:
            return
        if st == "RESPAWN":
            if inp.x or inp.y or inp.pressed:
                c.current_state, c.intangible = "JUMPING", 100
            return
        if st in ("HIT", "STUN", "DODGE") or c.lag > 0:
            c.move_x = 0
            return
        c.move_x = inp.x
        if inp.down_tap and c.ground is None and c.vy > 0 and st != "LEDGE":
            c.fast_fall = True
        if c.jump_cut and (c.vy >= 0 or c.ground):
            c.jump_cut = False
        if c.jump_cut and 'jump' not in inp.held:        # released jump early = short hop
            c.vy *= 0.45
            c.jump_cut = False
        if st in ("ATTACKING", "HELPLESS"):
            return
        if st == "LEDGE":
            self._ledge_options(inp)
            return
        if st == "SHIELD":
            self._shield_options(inp, prev_x)
            return
        # ---------------- free to act: IDLE / RUNNING / JUMPING / PARRY_SUCCESS ----------------
        grounded = c.ground is not None
        if grounded:
            if inp.x:
                c.run_frames = c.run_frames + 1 if c.facing == inp.x else 0
                c.facing = inp.x
            else:
                c.run_frames = 0
            if st != "PARRY_SUCCESS":
                c.current_state = "RUNNING" if inp.x else "IDLE"
        if self._take('special'):
            if inp.x:
                c.facing = inp.x
            slot = 'uspecial' if inp.y < 0 else 'dspecial' if inp.y > 0 else 'sspecial' if inp.x else 'nspecial'
            self.combat.process_attack(c, slot)
            return
        heavy = self._take('heavy')
        if heavy or self._take('attack'):
            if grounded:
                if c.shield_held and not heavy:
                    slot = 'grab'
                elif heavy:
                    slot = 'usmash' if inp.y < 0 else 'dsmash' if inp.y > 0 else 'fsmash'
                elif inp.y < 0:
                    slot = 'utilt'
                elif inp.y > 0:
                    slot = 'dtilt'
                elif inp.x:
                    slot = 'dash' if c.run_frames > 12 else 'ftilt'
                else:
                    slot = 'jab'
            else:
                slot = ('uair' if inp.y < 0 else 'dair' if inp.y > 0 else
                        'fair' if inp.x == c.facing else 'bair' if inp.x == -c.facing else 'nair')
            self.combat.process_attack(c, slot)
            return
        if self._take('jump'):
            self._jump()
            return
        if grounded:
            if c.shield_held and c.shield_hp > 4:
                c.current_state, c.shield_frame = "SHIELD", 0
            elif inp.down_tap and not c.ground.is_solid:  # drop through soft platforms
                c.ground, c.drop_timer, c.vy = None, 10, 1.0
        elif 'shield' in inp.pressed and not c.air_dodged:
            self._dodge(inp.x, inp.y, air=True)

    def _jump(self):
        c = self.char
        if c.ground is not None or c.current_state == "LEDGE":
            c.vy, c.jump_cut = -c.jump_height, True
        elif c.jumps_left > 0:
            c.jumps_left -= 1
            c.vy = -c.double_jump
            c.vx = c.in_x * c.air_speed * 0.8 if c.in_x else c.vx
            self.combat.fx.ring(c.x, c.y, c.colors['accent'], 10, 4, 10)
        else:
            return
        c.ground, c.fast_fall = None, False
        c.current_state = "JUMPING"

    def _dodge(self, x, y, air=False):
        c = self.char
        c.current_state = "DODGE"
        c.intangible = max(c.intangible, 16)
        if air:
            c.air_dodged, c.dodge_timer = True, 26
            n = math.hypot(x, y)
            if n:
                c.vx, c.vy = x / n * 10, y / n * 10
        else:
            c.dodge_timer = 24 if x else 20
            c.vx = x * 11

    def _shield_options(self, inp, prev_x):
        c = self.char
        if c.shield_stun > 0:
            return
        if self._take('attack'):
            self.combat.process_attack(c, 'grab')
        elif self._take('jump'):
            self._jump()
        elif inp.down_tap:
            self._dodge(0, 0)                            # spot dodge
        elif inp.x and not prev_x:
            self._dodge(inp.x, 0)                        # roll
        elif not c.shield_held:
            c.current_state, c.lag = "IDLE", 5

    def _ledge_options(self, inp):
        c = self.char
        p, lx, inward = c.ledge
        if c.ledge_timer < 8:
            return
        if self._take('jump'):
            self._jump()
            c.ledge_cd, c.ledge = 30, None
        elif inp.y < 0 or inp.x == inward or self._take('attack'):
            c.x, c.y, c.ground = lx + inward * (c.w / 2 + 8), p.y, p      # climb up
            c.current_state, c.lag, c.ledge = "IDLE", 10, None
            c.intangible = max(c.intangible, 14)
        elif inp.y > 0 or inp.x == -inward:
            c.current_state, c.ledge_cd, c.ledge = "JUMPING", 30, None    # let go

    def update_state(self):
        """Keeps blueprint compatibility: is_knocked_down mirrors the stun / tumble states."""
        c = self.char
        if c.current_state not in ("HIT", "STUN"):
            c.is_knocked_down = False

    def tick(self):
        self.process_input(self.read_input())
        self.update_state()


class AIController(CharacterController):
    """CPU opponent. It produces the same InputState a human would, so it obeys every rule."""
    LEVELS = {1: (30, 0.02, 0.45), 2: (16, 0.09, 0.7), 3: (8, 0.22, 0.9)}   # think delay, guard chance, aggression

    def __init__(self, character_data, combat_manager, physics, level=2):
        super().__init__(character_data, combat_manager)
        self.physics, self.level = physics, level
        self.cool, self.queue, self.shield_hold, self.jump_hold, self.wait = 40, [], 0, 0, 0

    def _press(self, inp, slot, toward):
        c = self.char
        if slot == 'grab':
            inp.held.add('shield'); inp.pressed.add('attack')
            return
        kind = 'special' if 'special' in slot else 'heavy' if 'smash' in slot else 'attack'
        if slot[0] == 'u':
            inp.y = -1
        elif slot[0] == 'd' and slot != 'dash':
            inp.y = 1
        elif slot in ('ftilt', 'fsmash', 'sspecial', 'dash'):
            inp.x = toward
        elif slot == 'fair':
            inp.x = c.facing
        elif slot == 'bair':
            inp.x = -c.facing
        else:
            inp.x = 0
            if c.facing != toward and c.ground is not None:      # turn first, act next frame
                inp.x = toward
                self.queue.append((0, 0, kind))
                return
        inp.pressed.add(kind)

    def read_input(self):
        inp, c, st = InputState(), self.char, self.physics.map
        if self.jump_hold > 0:
            self.jump_hold -= 1
            inp.held.add('jump')
        if self.queue:
            inp.x, inp.y, b = self.queue.pop(0)
            if b:
                inp.pressed.add(b)
            return inp
        others = [o for o in self.physics.active_characters if o is not c and o.alive]
        think, guard, aggro = self.LEVELS[self.level]
        self.cool -= 1
        L, R, top = st.left, st.right, st.top
        state = c.current_state
        if state == "LEDGE":
            self.wait += 1
            if self.wait > 14:
                inp.y, self.wait = -1, 0
            return inp
        if state == "RESPAWN":
            if c.state_timer < 110:
                inp.y = 1
            return inp
        # ---------------- recovery ----------------
        if c.ground is None and (c.x < L - 5 or c.x > R + 5 or c.y > top + 12):
            tx = clamp(c.x, L + 60, R - 60)
            if c.y > top + 10 and L - 40 < c.x < R + 40:        # under the stage: swim out sideways first
                inp.x = -1 if c.x < (L + R) / 2 else 1
            else:
                inp.x = sign(tx - c.x)
            if state == "JUMPING" and c.vy >= 0:
                far = abs(tx - c.x) > 220
                if c.jumps_left > 0 and (c.y > top - 40 or far):
                    inp.pressed.add('jump')
                    self.jump_hold = 16
                elif c.jumps_left == 0 and (c.y > top + 50 or abs(tx - c.x) > 330):
                    inp.y = -1
                    inp.pressed.add('special')
            return inp
        if not others or state in ("HIT", "STUN", "ATTACKING", "DODGE", "HELPLESS"):
            return inp
        o = others[0]
        dx, dy = o.x - c.x, o.y - c.y
        adx, toward = abs(dx), sign(dx) or 1
        o_off = o.x < L - 20 or o.x > R + 20 or o.current_state in ("RESPAWN",)
        grounded = c.ground is not None
        # ---------------- defence ----------------
        if self.shield_hold > 0:
            self.shield_hold -= 1
            inp.held.add('shield')
            if o.current_state == "ATTACKING" or self.shield_hold > 6:
                return inp
        if grounded and o.current_state == "ATTACKING" and adx < 190 and abs(dy) < 130 and random.random() < guard:
            if c.name == "Bronn" and random.random() < 0.4:
                self._press(inp, 'nspecial', toward)
            elif random.random() < 0.25:
                inp.held.add('shield'); inp.x = -toward       # roll away
                self.queue.append((-toward, 0, None))
            else:
                self.shield_hold = 14
                inp.held.add('shield')
            return inp
        # ---------------- movement ----------------
        reach = 60 + (c.w + o.w) / 2
        target_x = clamp(o.x, L + 50, R - 50) if o_off else o.x
        if abs(target_x - c.x) > (reach if not o_off else 20):
            inp.x = sign(target_x - c.x)
        if grounded:
            if c.ground.is_solid and ((c.x < L + 36 and inp.x < 0) or (c.x > R - 36 and inp.x > 0)):
                inp.x = 0
            if dy < -90 and adx < 260 and random.random() < 0.06:
                inp.pressed.add('jump')
                self.jump_hold = 14
            elif dy > 60 and not c.ground.is_solid and random.random() < 0.08:
                inp.down_tap, inp.y = True, 1
        elif dy < -120 and c.vy > 0 and c.jumps_left > 0 and adx < 200 and random.random() < 0.05:
            inp.pressed.add('jump')
            self.jump_hold = 12
        # ---------------- offence ----------------
        if self.cool > 0 or state not in ("IDLE", "RUNNING", "JUMPING", "PARRY_SUCCESS") or c.lag > 0:
            return inp
        if o.intangible > 30 or random.random() > aggro:
            self.cool = think // 2
            return inp
        hints, dmg = c.spec['ai'], o.cumulative_damage_taken
        afford = lambda s: c.aether_gauge >= c.move_set[s].required_aether
        choice = None
        if grounded:
            if adx < reach + 20 and abs(dy) < 70:
                if o.current_state == "SHIELD" and random.random() < 0.7:
                    choice = 'grab'
                else:
                    pool = ['jab', 'jab', 'ftilt', 'ftilt', 'dtilt', 'utilt', 'grab', 'fsmash', 'dsmash']
                    pool += ['fsmash', 'fsmash', 'usmash'] * (1 + int(dmg > 140) * 2) if dmg > 90 else []
                    pool += [s for s in hints['close'] if afford(s) and s != 'nspecial' or (s == 'nspecial' and c.name != 'Bronn' and afford(s))]
                    choice = random.choice(pool)
            elif dy < -60 and adx < 100:
                choice = random.choice(['utilt', 'usmash', 'utilt'] if dy > -170 else ['jump'])
            elif adx < 520 and abs(dy) < 150:
                pool = [s for s in hints['mid'] if afford(s)]
                if c.name == "Rhea" and c.buff_timer > 0 and 'dspecial' in pool:
                    pool.remove('dspecial')
                if pool and random.random() < 0.45:
                    choice = random.choice(pool)
                elif adx < 240 and c.run_frames > 12:
                    choice = 'dash'
        elif adx < reach + 40 and abs(dy) < 120:
            safe = L < c.x < R
            if dy < -50:
                choice = 'uair'
            elif dy > 60:
                choice = 'dair' if safe else 'nair'
            else:
                choice = random.choice(['nair', 'fair' if toward == c.facing else 'bair'])
        if choice == 'jump':
            inp.pressed.add('jump')
            self.jump_hold = 14
            self.queue += [(toward, 0, None)] * 8 + [(0, -1, 'attack')]
        elif choice:
            self._press(inp, choice, toward)
        self.cool = think + random.randint(0, think) if choice else 4
        return inp


# =====================================================================
# ART: procedural sprites and stage scenery (stand-ins until real PNG assets exist)
# =====================================================================
SPR, SPR_CX, SPR_FEET = 260, 130, 215


def glow(surf, color, pos, radius, alpha):
    tmp = pygame.Surface((radius * 2, radius * 2), pygame.SRCALPHA)
    for i in range(4, 0, -1):
        pygame.draw.circle(tmp, (*color, alpha // i), (radius, radius), radius * i // 4)
    surf.blit(tmp, (pos[0] - radius, pos[1] - radius))


def render_fighter_surface(spec, col, key, facing, t, prog=0.0, aim=(1, 0)):
    """Draws a fighter in a pose onto a transparent 260x260 surface (feet at SPR_CX, SPR_FEET)."""
    s = pygame.Surface((SPR, SPR), pygame.SRCALPHA)
    w, h, style, f = spec['stats']['w'], spec['stats']['h'], spec['style'], facing
    main, dark, acc, skin = col['main'], col['dark'], col['accent'], col['skin']
    cx, fy = SPR_CX, SPR_FEET
    D = pygame.draw
    bob = math.sin(t * 0.08) * 2 if key in ('idle', 'shield', 'stun', 'parry') else 0
    lean = {'run': 8, 'attack': 9, 'hurt': -10, 'jump': 2, 'fall': -2, 'helpless': -5, 'dodge': 12,
            'stun': math.sin(t * 0.2) * 7}.get(key, 0) * f
    crouch = 7 if key in ('shield', 'stun') else 0
    hover = 8 + math.sin(t * 0.07) * 3 if style == 'voidcaller' else 0
    fy2 = fy - hover
    hip = (cx, fy2 - h * 0.36 + crouch)
    sh = (cx + lean, fy2 - h * 0.76 + crouch + bob)
    hr = max(13, int(w * 0.33))
    head = (int(cx + lean * 1.3), int(fy2 - h + hr + crouch + bob))
    tw = w * 0.84
    lw = max(6, int(w * 0.2))
    # ---- legs / robe
    if style == 'voidcaller':
        sway = math.sin(t * 0.1) * 4 - lean * 0.6
        hem = [(cx - w * 0.6 + sway, fy2), (cx - w * 0.3 + sway, fy2 - 8), (cx + sway, fy2 + 2),
               (cx + w * 0.3 + sway, fy2 - 8), (cx + w * 0.6 + sway, fy2)]
        D.polygon(s, dark, [(sh[0] - tw / 2, sh[1]), (sh[0] + tw / 2, sh[1])] + hem[::-1])
        D.polygon(s, main, [(sh[0] - tw / 2 + 4, sh[1]), (sh[0] + tw / 2 - 4, sh[1]),
                            (cx + w * 0.34 + sway, fy2 - 18), (cx - w * 0.34 + sway, fy2 - 18)])
    else:
        if key == 'run':
            a = t * 0.45
            feet = [(cx + math.sin(a) * w * 0.55, fy - max(0, math.cos(a)) * 9),
                    (cx - math.sin(a) * w * 0.55, fy - max(0, -math.cos(a)) * 9)]
        elif key in ('jump', 'fall', 'helpless', 'hurt', 'ledge', 'dodge'):
            feet = [(cx - f * w * 0.34, fy - 12), (cx + f * w * 0.16, fy - 3)]
        elif key == 'attack':
            feet = [(cx - f * w * 0.42, fy), (cx + f * w * 0.36, fy)]
        else:
            feet = [(cx - w * 0.28, fy), (cx + w * 0.28, fy)]
        for fx_, fy_ in feet:
            D.line(s, dark, hip, (fx_, fy_), lw)
            D.circle(s, dark, (int(fx_ + f * 3), int(fy_ - 3)), lw // 2 + 2)
        D.polygon(s, main, [(sh[0] - tw / 2, sh[1]), (sh[0] + tw / 2, sh[1]),
                            (hip[0] + tw * 0.4, hip[1] + 6), (hip[0] - tw * 0.4, hip[1] + 6)])
        D.line(s, acc, (hip[0] - tw * 0.4, hip[1] + 2), (hip[0] + tw * 0.4, hip[1] + 2), 4)
    # ---- arms
    ext = math.sin(clamp(prog, 0, 1) * math.pi)
    reach = w * 0.5 + 18
    hands = {
        'idle': (sh[0] + f * 12, sh[1] + h * 0.3), 'run': (sh[0] + f * 16 * math.cos(t * 0.45), sh[1] + h * 0.26),
        'jump': (sh[0] + f * 14, sh[1] - 8), 'fall': (sh[0] + f * 16, sh[1] - 14),
        'hurt': (sh[0] - f * 20, sh[1] - 16), 'helpless': (sh[0] - f * 12, sh[1] - 20),
        'shield': (sh[0] + f * 20, sh[1] + 12), 'ledge': (sh[0] + f * 12, sh[1] - 28),
        'stun': (sh[0] + f * 6, sh[1] + h * 0.34), 'parry': (sh[0] + f * 22, sh[1] - 4),
        'dodge': (sh[0] - f * 10, sh[1] + 16),
        'attack': (sh[0] + aim[0] * (14 + ext * reach), sh[1] + 8 + aim[1] * (6 + ext * reach)),
    }
    hand = hands.get(key, hands['idle'])
    back = (2 * sh[0] - hand[0] - f * 4, sh[1] + h * 0.26)
    aw = max(5, int(w * 0.17))
    D.line(s, dark, (sh[0] - f * tw * 0.3, sh[1] + 4), back, aw)
    # ---- head + per-character details
    if style == 'weaver':
        for i in range(6):                                   # trailing scarf
            p0 = (sh[0] - f * (i * 10), sh[1] + 2 + math.sin(t * 0.2 + i * 0.9) * 4 + i * 1.5)
            p1 = (sh[0] - f * ((i + 1) * 10), sh[1] + 2 + math.sin(t * 0.2 + (i + 1) * 0.9) * 4 + (i + 1) * 1.5)
            D.line(s, acc, p0, p1, 7 - i)
        D.circle(s, skin, head, hr)
        D.polygon(s, dark, [(head[0] + f * hr, head[1] - 4), (head[0] + f * 4, head[1] - hr - 6),
                            (head[0] - f * (hr + 14), head[1] - hr - 2), (head[0] - f * (hr + 4), head[1] + 2),
                            (head[0] - f * 4, head[1] - 5)])
        D.line(s, (40, 20, 30), (head[0] + f * hr * 0.3, head[1]), (head[0] + f * hr * 0.75, head[1] - 2), 3)
    elif style == 'sentinel':
        helm = pygame.Rect(0, 0, hr * 2 + 4, hr * 2 + 2)
        helm.center = head
        D.rect(s, skin, helm, border_radius=8)
        D.rect(s, dark, helm, 3, border_radius=8)
        D.line(s, acc, (head[0] - (hr - 6) * (1 if f < 0 else 0.1), head[1]),
               (head[0] + (hr - 6) * (1 if f > 0 else 0.1), head[1]), 5)
        D.polygon(s, main, [(head[0] - 5, helm.top), (head[0] + 5, helm.top), (head[0], helm.top - 12)])
        for sx in (-1, 1):
            D.circle(s, main, (int(sh[0] + sx * tw * 0.5), int(sh[1] + 4)), int(w * 0.25))
            D.circle(s, dark, (int(sh[0] + sx * tw * 0.5), int(sh[1] + 4)), int(w * 0.25), 3)
    elif style == 'voidcaller':
        D.circle(s, skin, head, hr)
        tip = (head[0] - f * 20 + math.sin(t * 0.1) * 3, head[1] - hr * 2.7)
        D.polygon(s, main, [(head[0] - hr - 10, head[1] - hr * 0.35), (head[0] + hr + 10, head[1] - hr * 0.35),
                            (head[0] + 4, head[1] - hr * 1.5), tip, (head[0] - 6, head[1] - hr * 1.3)])
        D.line(s, acc, (head[0] - hr - 10, head[1] - hr * 0.35), (head[0] + hr + 10, head[1] - hr * 0.35), 4)
        for ex in (0.15, 0.6):
            D.circle(s, acc, (int(head[0] + f * hr * ex), head[1] + 3), 3)
        for i in range(3):                                   # orbiting stars
            a = t * 0.06 + i * math.tau / 3
            px, py = cx + math.cos(a) * 40, sh[1] + 20 + math.sin(a) * 14
            D.polygon(s, acc, [(px, py - 6), (px + 2, py - 2), (px + 6, py), (px + 2, py + 2),
                               (px, py + 6), (px - 2, py + 2), (px - 6, py), (px - 2, py - 2)])
    else:                                                    # kindled
        for i in range(5):                                   # flame hair
            fx_ = head[0] + (i - 2) * hr * 0.42 - f * 3
            ht = hr * (1.0 + 0.45 * math.sin(t * 0.3 + i * 1.7)) + (6 if i == 2 else 0)
            D.polygon(s, acc if i % 2 else main, [(fx_ - 6, head[1] - hr * 0.5), (fx_ + 6, head[1] - hr * 0.5),
                                                  (fx_ - f * 5, head[1] - hr * 0.6 - ht)])
        D.circle(s, skin, head, hr)
        D.line(s, dark, (head[0] - hr, head[1] - 5), (head[0] + hr, head[1] - 5), 5)
        D.circle(s, (50, 24, 16), (int(head[0] + f * hr * 0.45), head[1] + 2), 3)
        D.circle(s, acc, (int(back[0]), int(back[1])), 9)
    # ---- front arm + held item
    D.line(s, main if style != 'voidcaller' else dark, (sh[0] + f * tw * 0.25, sh[1] + 5), hand, aw)
    hx, hy = int(hand[0]), int(hand[1])
    if style == 'weaver':
        d = aim if key == 'attack' else (f * 0.8, -0.6)
        tip = (hx + d[0] * 44, hy + d[1] * 44)
        D.line(s, (255, 255, 255), (hx, hy), tip, 5)
        D.line(s, (150, 230, 255), (hx, hy), tip, 2)
        D.circle(s, skin, (hx, hy), 5)
    elif style == 'sentinel':
        shield = pygame.Rect(0, 0, 22, int(h * 0.5))
        shield.center = (hx + f * 6, hy)
        D.rect(s, main, shield, border_radius=6)
        D.rect(s, acc, shield, 3, border_radius=6)
        D.circle(s, acc, shield.center, 4)
    elif style == 'voidcaller':
        glow(s, acc, (hx, hy), 14, 150)
        D.circle(s, WHITE, (hx, hy), 4)
    else:
        if key == 'attack':
            glow(s, acc, (hx, hy), 20, 160)
        D.circle(s, acc, (hx, hy), 10)
        D.circle(s, dark, (hx, hy), 10, 2)
    return s


def make_background(stage):
    """Pre-renders the stage's sky and distant scenery once (it never changes)."""
    bg = pygame.Surface((WORLD_W, WORLD_H))
    top, bottom = stage.palette['sky']
    for y in range(WORLD_H):
        pygame.draw.line(bg, lerp_color(top, bottom, (y / WORLD_H) ** 1.3), (0, y), (WORLD_W, y))
    rng = random.Random(len(stage.name) * 97)
    if stage.key == 'atoll':
        glow(bg, (255, 220, 160), (1500, 880), 520, 90)
        pygame.draw.circle(bg, (255, 236, 190), (1500, 880), 150)
        for _ in range(26):
            x, y, r = rng.randint(0, WORLD_W), rng.randint(150, 1250), rng.randint(60, 190)
            cl = pygame.Surface((r * 3, r), pygame.SRCALPHA)
            pygame.draw.ellipse(cl, (255, 225, 215, 40), cl.get_rect())
            bg.blit(cl, (x - r, y))
        for _ in range(9):
            x, y, w = rng.randint(100, WORLD_W - 100), rng.randint(350, 1150), rng.randint(90, 260)
            c = lerp_color((70, 60, 120), bottom, y / WORLD_H * 0.6)
            pygame.draw.polygon(bg, c, [(x - w, y), (x + w, y), (x + w * 0.3, y + w * 0.9), (x - w * 0.2, y + w * 0.6)])
            pygame.draw.rect(bg, lerp_color(c, (150, 220, 160), 0.4), (x - w, y - 8, w * 2, 10), border_radius=5)
    elif stage.key == 'loom':
        for _ in range(420):
            x, y = rng.randint(0, WORLD_W), rng.randint(0, WORLD_H)
            pygame.draw.circle(bg, lerp_color((120, 110, 170), WHITE, rng.random()), (x, y), rng.choice([1, 1, 1, 2]))
        glow(bg, (200, 190, 255), (700, 420), 380, 70)
        pygame.draw.circle(bg, (232, 226, 255), (700, 420), 130)
        pygame.draw.circle(bg, (200, 190, 240), (660, 380), 28)
        pygame.draw.circle(bg, (206, 196, 244), (740, 460), 18)
        threads = pygame.Surface((WORLD_W, WORLD_H), pygame.SRCALPHA)
        for i in range(34):                                   # the loom's warp and weft
            x = i * 76 - 60
            pygame.draw.line(threads, (255, 200, 120, 26), (x, 0), (x + 420, WORLD_H), 3)
            pygame.draw.line(threads, (150, 140, 255, 22), (x + 500, 0), (x - 100, WORLD_H), 2)
        bg.blit(threads, (0, 0))
        for x in (330, 2070):                                 # the great spindles
            pygame.draw.rect(bg, (30, 24, 62), (x - 40, 250, 80, WORLD_H))
            pygame.draw.ellipse(bg, (60, 46, 110), (x - 110, 560, 220, 420))
            for k in range(8):
                pygame.draw.line(bg, (255, 200, 120), (x - 96, 620 + k * 40), (x + 96, 640 + k * 40), 2)
    else:
        glow(bg, (255, 120, 40), (1200, 1500), 1100, 80)
        for layer, (base, colr) in enumerate([(780, (46, 24, 30)), (930, (30, 16, 22))]):
            pts, x = [(0, WORLD_H)], 0
            while x < WORLD_W + 200:
                pts.append((x, base - rng.randint(60, 380 - layer * 120)))
                x += rng.randint(120, 260)
                pts.append((x, base + rng.randint(0, 60)))
                x += rng.randint(40, 120)
            pts.append((WORLD_W, WORLD_H))
            pygame.draw.polygon(bg, colr, pts)
        for x in (420, 760, 1640, 1980):                      # sentinel statues' pillars
            pygame.draw.rect(bg, (22, 14, 20), (x - 34, 420, 68, WORLD_H))
            pygame.draw.rect(bg, (22, 14, 20), (x - 50, 400, 100, 30))
            pygame.draw.circle(bg, (150, 230, 255), (x, 480), 9)
            glow(bg, (150, 230, 255), (x, 480), 40, 80)
        pygame.draw.rect(bg, (255, 130, 40), (0, 1290, WORLD_W, 110))
        pygame.draw.rect(bg, (255, 200, 90), (0, 1290, WORLD_W, 8))
    return bg


def draw_platform(surf, p, pal):
    x, y, w, h = int(p.x), int(p.y), p.width, p.height
    if p.is_solid:
        pts = [(x, y + 30), (x + w, y + 30)]
        n = len(p.jag)
        for i in range(n):
            pts.append((x + w - (i + 0.5) * w / n, y + 30 + (h + 40) * p.jag[i] * math.sin((i + 0.5) / n * math.pi) ** 0.6))
        pygame.draw.polygon(surf, lerp_color(pal['rock'], (0, 0, 0), 0.35), pts)
        pygame.draw.rect(surf, pal['rock'], (x, y, w, 44), border_radius=10)
        pygame.draw.rect(surf, pal['top'], (x - 4, y - 2, w + 8, 14), border_radius=7)
        pygame.draw.rect(surf, lerp_color(pal['top'], WHITE, 0.5), (x, y - 2, w, 4), border_radius=2)
    else:
        pygame.draw.rect(surf, lerp_color(pal['soft'], (0, 0, 0), 0.55), (x, y, w, h), border_radius=7)
        pygame.draw.rect(surf, pal['soft'], (x, y, w, 5), border_radius=3)
        if p.motion:
            glow(surf, pal['soft'], (x + w // 2, y + h + 6), 22, 90)


def draw_projectile(surf, p, frame):
    x, y, c = int(p.x), int(p.y), p.color
    if p.kind == 'needle':
        pygame.draw.line(surf, c, (x - p.facing * 26, y), (x + p.facing * 12, y), 5)
        pygame.draw.line(surf, WHITE, (x - p.facing * 10, y), (x + p.facing * 16, y), 3)
    elif p.kind == 'orb':
        r = 15 + int(math.sin(frame * 0.3) * 3)
        glow(surf, c, (x, y), r * 2, 130)
        pygame.draw.circle(surf, WHITE, (x, y), r - 6)
        pygame.draw.circle(surf, c, (x, y), r, 4)
    elif p.kind == 'lance':
        glow(surf, c, (x, y), 34, 110)
        pygame.draw.polygon(surf, c, [(x - p.facing * 46, y), (x, y - 9), (x + p.facing * 38, y), (x, y + 9)])
        pygame.draw.line(surf, WHITE, (x - p.facing * 26, y), (x + p.facing * 34, y), 3)
    elif p.kind == 'wall':
        r = p.rect
        fade = clamp(p.life / 40, 0.35, 1)
        stone = lerp_color((40, 44, 56), (128, 136, 156), fade)
        pygame.draw.rect(surf, stone, r, border_radius=6)
        pygame.draw.rect(surf, lerp_color(stone, (0, 0, 0), 0.4), r, 4, border_radius=6)
        pygame.draw.line(surf, c, (r.centerx, r.top + 14), (r.centerx, r.bottom - 14), 4)
        pygame.draw.line(surf, c, (r.left + 12, r.centery - 16), (r.right - 12, r.centery - 16), 4)
    elif p.kind == 'well':
        glow(surf, c, (x, y), 70, 70)
        for i in range(3):
            rr = int(60 - ((frame * 1.5 + i * 20) % 60))
            if rr > 3:
                pygame.draw.circle(surf, c, (x, y), rr, 3)
        pygame.draw.circle(surf, (10, 4, 24), (x, y), 12)


class AssetLibrary:
    """PHASE 3 hook. Drop PNGs into ./assets and they replace the placeholder art automatically:
       <fighter>_<pose>.png (e.g. kai_idle.png, bronn_attack.png; drawn facing right) and bg_<stage>.png."""
    def __init__(self):
        self.dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets')
        self.cache = {}

    def get(self, name):
        if name not in self.cache:
            path = os.path.join(self.dir, name + '.png')
            try:
                self.cache[name] = pygame.image.load(path).convert_alpha() if os.path.exists(path) else None
            except pygame.error:
                self.cache[name] = None
        return self.cache[name]


# =====================================================================
# 6. STATE TRANSLATOR (Module 6)
# =====================================================================

class StateTranslator:
    """Maps a fighter's internal state to what the player sees: pose, tint, outline, transparency."""

    def __init__(self, assets):
        self.assets = assets

    def _determine_visuals(self, character):
        c, state = character, character.current_state
        v = dict(sprite_key='idle', tint=None, outline=None, alpha=255, rotation=0)
        if state == "RUNNING":
            v['sprite_key'] = 'run'
        elif state == "JUMPING":
            v['sprite_key'] = 'jump' if c.vy < 0 else 'fall'
        elif state == "ATTACKING":
            armored = c.current_move and c.current_move.armor and \
                c.current_move.armor[0] <= c.move_frame <= c.current_move.armor[1]
            guard = c.current_move and c.current_move.counter
            v.update(sprite_key='shield' if guard else 'attack',
                     outline=(120, 255, 150) if guard else c.colors['accent'] if armored else WHITE)
        elif state == "HIT":
            v.update(sprite_key='hurt', tint=(255, 240, 120, 150 if c.hitstop > 0 else 60), outline=(255, 230, 60))
            if c.is_knocked_down:
                v['rotation'] = -c.anim * 14 * sign(c.kbx or 1)
        elif state == "STUN":
            v.update(sprite_key='stun', tint=(255, 255, 0, 70), outline=(255, 255, 0))
        elif state == "SHIELD":
            v['sprite_key'] = 'shield'
        elif state == "PARRY_SUCCESS":
            v.update(sprite_key='parry', tint=(0, 255, 90, 110), outline=(0, 255, 90))
        elif state == "DODGE":
            v.update(sprite_key='dodge', alpha=110)
        elif state == "LEDGE":
            v['sprite_key'] = 'ledge'
        elif state == "HELPLESS":
            v.update(sprite_key='helpless', tint=(0, 0, 0, 90))
        elif state == "RESPAWN":
            v['outline'] = WHITE
        if c.flash > 0 and not v['tint']:
            v['tint'] = (255, 255, 255, 120)
        if c.intangible > 0 and state != "DODGE" and (c.anim // 4) % 2:
            v['alpha'] = 120
        if c.current_move and c.current_move.aim and c.current_move.aim[1] <= c.move_frame <= c.current_move.aim[2]:
            v['alpha'] = 0
        return v

    def sprite_for(self, c, v):
        img = self.assets.get(f"{c.name.lower()}_{v['sprite_key']}")
        if img:
            surf = pygame.Surface((SPR, SPR), pygame.SRCALPHA)
            img = img if c.facing > 0 else pygame.transform.flip(img, True, False)
            surf.blit(img, (SPR_CX - img.get_width() // 2, SPR_FEET - img.get_height()))
            return surf
        prog = c.move_frame / max(1, c.current_move.duration * 0.7) if c.current_move else 0
        return render_fighter_surface(c.spec, c.colors, v['sprite_key'], c.facing, c.anim, prog, c.visual_aim)

    def update_visuals(self, surface, characters, physics, show_boxes=False):
        for c in characters.values():
            if c.current_state == "KO":
                continue
            v = self._determine_visuals(c)
            if c.current_state == "RESPAWN":                    # arrive on a little halo platform
                pygame.draw.ellipse(surface, WHITE, (c.x - 50, c.y - 4, 100, 16))
                pygame.draw.ellipse(surface, c.colors['accent'], (c.x - 50, c.y - 4, 100, 16), 3)
            for box in physics.active_hitboxes(c):              # attack swooshes
                r = physics.hitbox_rect(c, box)
                fxs = pygame.Surface((r.w + 12, r.h + 12), pygame.SRCALPHA)
                col = WHITE if box['grab'] else c.colors['accent']
                pygame.draw.ellipse(fxs, (*col, 90), fxs.get_rect())
                pygame.draw.ellipse(fxs, (255, 255, 255, 210), fxs.get_rect(), 3)
                surface.blit(fxs, (r.x - 6, r.y - 6))
            if v['alpha'] > 0:
                spr = self.sprite_for(c, v)
                if v['tint']:
                    r, g, b, a = v['tint']
                    k = a / 255
                    if (r, g, b) == (0, 0, 0):
                        spr.fill((int(255 - 255 * k),) * 3 + (255,), special_flags=pygame.BLEND_RGBA_MULT)
                    else:
                        spr.fill((int(r * k), int(g * k), int(b * k), 0), special_flags=pygame.BLEND_RGBA_ADD)
                if v['outline']:
                    pts = pygame.mask.from_surface(spr, 60).outline(3)
                    if len(pts) > 2:
                        pygame.draw.lines(spr, v['outline'], True, pts, 3)
                if v['alpha'] < 255:
                    spr.set_alpha(v['alpha'])
                pos = (c.x - SPR_CX, c.y - SPR_FEET)
                if v['rotation']:
                    pivot = pygame.Surface((SPR, SPR), pygame.SRCALPHA)      # rotate around the body centre
                    pivot.blit(spr, (0, SPR // 2 - (SPR_FEET - c.h // 2)))
                    rot = pygame.transform.rotate(pivot, v['rotation'])
                    if v['alpha'] < 255:
                        rot.set_alpha(v['alpha'])
                    surface.blit(rot, rot.get_rect(center=(c.x, c.y - c.h / 2)))
                else:
                    surface.blit(spr, pos)
            cx, cy = c.center
            if c.current_state == "SHIELD":
                rad = int((c.h * 0.42 + 14) * (0.55 + 0.45 * c.shield_hp / SHIELD_MAX))
                bubble = pygame.Surface((rad * 2, rad * 2), pygame.SRCALPHA)
                col = (120, 255, 150) if c.shield_frame <= PARRY_WINDOW else PLAYER_TAGS[c.slot % 2]
                pygame.draw.circle(bubble, (*col, 100), (rad, rad), rad)
                pygame.draw.circle(bubble, (255, 255, 255, 200), (rad, rad), rad, 3)
                surface.blit(bubble, (cx - rad, cy - rad))
            if c.current_state == "STUN":
                for i in range(3):
                    a = c.anim * 0.15 + i * math.tau / 3
                    pygame.draw.circle(surface, (255, 255, 0), (int(c.x + math.cos(a) * 26), int(c.y - c.h - 14 + math.sin(a) * 7)), 5)
            if show_boxes:
                pygame.draw.rect(surface, (0, 255, 0), c.rect, 2)
                for box in physics.active_hitboxes(c):
                    pygame.draw.rect(surface, (255, 0, 0), physics.hitbox_rect(c, box), 2)


# =====================================================================
# 5. GAME MANAGER (Module 5 - The Loop)
# =====================================================================

class GameManager:
    P2_MODES = ["CPU - Easy", "CPU - Normal", "CPU - Hard", "Human"]

    def __init__(self, headless=False, demo=False):
        if headless:
            os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
        pygame.init()
        pygame.display.set_caption("AETHER CLASH")
        self.screen = pygame.display.set_mode((SCREEN_WIDTH, SCREEN_HEIGHT))
        self.clock = pygame.time.Clock()
        self.world = pygame.Surface((WORLD_W, WORLD_H)).convert()
        self.fonts = {}
        self.assets = AssetLibrary()
        self.state_translator = StateTranslator(self.assets)
        self.stages = build_stages()
        for st in self.stages:
            st.bg = (self.assets.get('bg_' + st.key) or make_background(st)).convert()
        self.thumbs = None
        self.joysticks = []
        try:
            pygame.joystick.init()
            self.joysticks = [pygame.joystick.Joystick(i) for i in range(pygame.joystick.get_count())]
        except pygame.error:
            pass
        self.fx = Effects()
        self.physics_engine = self.combat_manager = self.map_data = None
        self.characters, self.controllers = {}, {}
        self.keys_pressed, self.keys_held = set(), pygame.key.get_pressed()
        self.scene = 'MENU'
        self.sel, self.stage_sel, self.p2_mode, self.p1_cpu = [0, 1], 0, 1, demo
        self.paused = self.show_boxes = False
        self.running = True
        self.frame = self.intro = self.end_timer = 0
        self.winner = None
        self.cam = pygame.Rect(0, 0, WORLD_W, WORLD_W * 9 // 16)
        self.camf = [600.0, 300.0, 1200.0]
        if demo:
            self.p2_mode = 2

    # ------------------------------------------------------------ setup
    def setup_game(self, p1=0, p2=1, stage=0, p2_mode=1, p1_cpu=False):
        self.map_data = self.stages[stage]
        self.map_data.reset()
        self.fx = Effects()
        self.combat_manager = CombatManager(None, self.fx)           # physics dependency injected next
        self.physics_engine = PhysicsEngine(self.map_data, self.combat_manager, self.fx)
        self.combat_manager.physics = self.physics_engine
        self.characters, self.controllers = {}, {}
        human2 = p2_mode == 3
        pads = list(self.joysticks)
        pad2 = pads.pop(0) if (human2 and pads) else None            # with one pad and two humans, P2 gets it
        pad1 = pads.pop(0) if pads else None
        for slot, idx in enumerate((p1, p2)):
            spec = ROSTER[idx]
            colors = spec['alt'] if (slot == 1 and p1 == p2) else spec['colors']
            ch = CharacterData(spec['name'], spec['faction'], spec['stats'], spec['moves'], spec, slot, colors)
            sx, sy, facing = self.map_data.spawns[slot]
            ch.reset(sx, sy, facing)
            self.characters[slot + 1] = ch
            self.physics_engine.register_character(ch)
            cpu = p1_cpu if slot == 0 else not human2
            if cpu:
                level = 3 if (slot == 0) else p2_mode + 1
                ctrl = AIController(ch, self.combat_manager, self.physics_engine, min(3, level))
            else:
                ctrl = CharacterController(ch, self.combat_manager,
                                           KeyboardDevice(KEYMAP_P1 if slot == 0 else KEYMAP_P2, self,
                                                          pad1 if slot == 0 else pad2))
            ch.is_cpu = cpu
            self.controllers[slot + 1] = ctrl
        self.scene, self.paused = 'FIGHT', False
        self.intro, self.end_timer, self.winner, self.frame = 100, 0, None, 0
        self.camf = [600.0, 300.0, 1200.0]

    # ------------------------------------------------------------ the loop
    def run_game_loop(self):
        """Fixed-timestep loop: real time is measured (delta_time) and spent in exact 1/60 s simulation steps,
        so the physics behaves identically on fast and slow computers."""
        step, accumulator = 1.0 / FPS, 0.0
        while self.running:
            delta_time = min(self.clock.tick(FPS) / 1000.0, 0.1)
            accumulator += delta_time
            self._handle_input_events()                               # 1. input
            steps = 0
            while accumulator >= step * 0.99 and steps < 3:
                self._step()                                          # 2. physics + 3. combat
                accumulator -= step
                steps += 1
                self.keys_pressed = set()
            self._render_frame()                                      # 4. state translation + 5. render
            pygame.display.flip()
        pygame.quit()

    def _handle_input_events(self):
        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                self.running = False
            elif e.type == pygame.KEYDOWN:
                self.keys_pressed.add(e.key)
                self._on_key(e.key)
        self.keys_held = pygame.key.get_pressed()

    def _on_key(self, k):
        K = pygame
        if self.scene == 'MENU':
            n = len(ROSTER)
            if k == K.K_ESCAPE:
                self.running = False
            elif k in (K.K_a, K.K_d):
                self.sel[0] = (self.sel[0] + (1 if k == K.K_d else -1)) % n
            elif k in (K.K_LEFT, K.K_RIGHT):
                self.sel[1] = (self.sel[1] + (1 if k == K.K_RIGHT else -1)) % n
            elif k in (K.K_w, K.K_s, K.K_UP, K.K_DOWN):
                self.stage_sel = (self.stage_sel + (1 if k in (K.K_s, K.K_DOWN) else -1)) % len(self.stages)
            elif k == K.K_TAB:
                self.p2_mode = (self.p2_mode + 1) % len(self.P2_MODES)
            elif k == K.K_F2:
                self.p1_cpu = not self.p1_cpu
            elif k in (K.K_RETURN, K.K_KP_ENTER):
                self.setup_game(self.sel[0], self.sel[1], self.stage_sel, self.p2_mode, self.p1_cpu)
        elif self.scene == 'FIGHT':
            if k in (K.K_ESCAPE, K.K_p):
                self.paused = not self.paused
            elif k == K.K_F1:
                self.show_boxes = not self.show_boxes
            elif self.paused and k in (K.K_BACKSPACE, K.K_q):
                self.scene = 'MENU'
            elif self.paused and k == K.K_RETURN:
                self.paused = False
        elif self.scene == 'RESULT':
            if k in (K.K_RETURN, K.K_KP_ENTER, K.K_SPACE):
                self.setup_game(self.sel[0], self.sel[1], self.stage_sel, self.p2_mode, self.p1_cpu)
            elif k in (K.K_ESCAPE, K.K_BACKSPACE):
                self.scene = 'MENU'

    def _step(self):
        """One 60th of a second of game time."""
        self.frame += 1
        if self.scene != 'FIGHT' or self.paused:
            return
        if self.intro > 0:
            self.intro -= 1
            if self.intro > 40:                                       # "READY..." - nobody may move yet
                self._update_camera()
                return
        if self.end_timer > 0:
            self.end_timer -= 1
            if self.end_timer == 0:
                self.scene = 'RESULT'
                return
            if self.end_timer > 60 and self.end_timer % 3:            # dramatic slow motion on the final KO
                return
        else:
            for ctrl in self.controllers.values():
                ctrl.tick()                                           # 1. input -> commands
        self.physics_engine.update_all()                              # 2. physics (calls combat on hits)
        self.combat_manager.check_all_move_durations()                # 3. combat timers / state machine
        self.fx.update()
        self._ambient()
        self._update_camera()
        if self.physics_engine.game_over and self.end_timer == 0 and self.winner is None:
            alive = [c for c in self.characters.values() if c.current_lives > 0]
            self.winner = alive[0] if alive else None
            self.end_timer = 170

    def _ambient(self):
        if self.frame % 6 == 0:
            pal = self.map_data.palette
            x, y = random.uniform(self.cam.left, self.cam.right), random.uniform(self.cam.top, self.cam.bottom)
            if self.map_data.key == 'crucible':
                self.fx.spark(x, self.cam.bottom, (255, 160, 60), 1, 3, 90, 4, grav=-0.12, direction=-1.57, spread=0.5)
            else:
                self.fx.spark(x, y, pal['soft'], 1, 0.6, 70, 3, grav=-0.01)

    def _update_camera(self):
        pts = [(c.x, c.y - c.h / 2) for c in self.characters.values() if c.current_state != "KO"]
        if not pts:
            pts = [self.map_data.respawn]
        pts.append((WORLD_W / 2, self.map_data.top - 120))
        x0, x1 = min(p[0] for p in pts) - 300, max(p[0] for p in pts) + 300
        y0, y1 = min(p[1] for p in pts) - 240, max(p[1] for p in pts) + 220
        w = clamp(max(x1 - x0, (y1 - y0) * 16 / 9, 1050), 1050, WORLD_W)
        tx, ty = (x0 + x1) / 2 - w / 2, (y0 + y1) / 2 - w * 9 / 32
        for i, target in enumerate((tx, ty, w)):
            self.camf[i] += (target - self.camf[i]) * 0.08
        w = self.camf[2]
        h = w * 9 / 16
        x, y = clamp(self.camf[0], 0, WORLD_W - w), clamp(self.camf[1], 0, WORLD_H - h)
        self.cam = pygame.Rect(int(x), int(y), int(w), int(h))

    # ------------------------------------------------------------ rendering
    def font(self, size):
        if size not in self.fonts:
            self.fonts[size] = pygame.font.Font(None, size)
        return self.fonts[size]

    def text(self, surf, s, size, color, pos, anchor='center', shadow=True):
        img = self.font(size).render(s, True, color)
        r = img.get_rect(**{anchor: pos})
        if shadow:
            surf.blit(self.font(size).render(s, True, (0, 0, 0)), r.move(2, 3))
        surf.blit(img, r)
        return r

    def _draw_world(self, view):
        """Draws the arena into self.world (only the part the camera sees) and scales it to the target."""
        st, world = self.map_data, self.world
        world.set_clip(view)
        world.blit(st.bg, view.topleft, view)
        for p in st.platforms:
            draw_platform(world, p, st.palette)
        if self.physics_engine:
            for pr in self.physics_engine.projectiles:
                draw_projectile(world, pr, self.frame)
            self.state_translator.update_visuals(world, self.characters, self.physics_engine, self.show_boxes)
            self.fx.draw(world, self.font(30))
            if self.show_boxes:
                pygame.draw.rect(world, (255, 0, 0), st.blast, 4)
        world.set_clip(None)

    def _render_frame(self):
        if self.scene == 'MENU':
            self._render_menu()
            return
        view = self.cam.copy()
        if self.fx.shake and not self.paused:
            view.move_ip(random.randint(-self.fx.shake, self.fx.shake), random.randint(-self.fx.shake, self.fx.shake))
            view.clamp_ip(pygame.Rect(0, 0, WORLD_W, WORLD_H))
        self._draw_world(view)
        pygame.transform.smoothscale(self.world.subsurface(view), (SCREEN_WIDTH, SCREEN_HEIGHT), self.screen)
        self._render_hud(view)
        if self.scene == 'RESULT':
            self._render_result()

    def _render_hud(self, view):
        scr, scale = self.screen, SCREEN_WIDTH / view.w
        for c in self.characters.values():                           # name tags above the fighters
            if c.current_state == "KO":
                continue
            sx, sy = (c.x - view.x) * scale, (c.y - c.h - 26 - view.y) * scale
            col = PLAYER_TAGS[c.slot]
            self.text(scr, "CPU" if c.is_cpu else f"P{c.slot + 1}", 22, col, (sx, sy - 12))
            pygame.draw.polygon(scr, col, [(sx - 7, sy), (sx + 7, sy), (sx, sy + 9)])
        for c in self.characters.values():                           # damage / stocks / Aether panels
            px = SCREEN_WIDTH * (0.28 if c.slot == 0 else 0.72)
            panel = pygame.Surface((300, 104), pygame.SRCALPHA)
            pygame.draw.rect(panel, (10, 10, 24, 170), panel.get_rect(), border_radius=16)
            pygame.draw.rect(panel, (*PLAYER_TAGS[c.slot], 255), panel.get_rect(), 3, border_radius=16)
            scr.blit(panel, (px - 150, SCREEN_HEIGHT - 116))
            pygame.draw.circle(scr, c.colors['dark'], (int(px - 108), SCREEN_HEIGHT - 66), 30)
            pygame.draw.circle(scr, c.colors['main'], (int(px - 108), SCREEN_HEIGHT - 66), 24)
            pygame.draw.circle(scr, c.colors['accent'], (int(px - 108), SCREEN_HEIGHT - 66), 10)
            d = c.cumulative_damage_taken
            col = lerp_color(WHITE, (255, 200, 60), d / 70) if d < 70 else \
                lerp_color((255, 200, 60), (255, 50, 40), (d - 70) / 70) if d < 140 else \
                lerp_color((255, 50, 40), (140, 10, 30), (d - 140) / 80)
            jitter = (random.randint(-3, 3), random.randint(-3, 3)) if c.hitstop > 0 else (0, 0)
            self.text(scr, f"{int(d)}%", 64, col, (px + 10 + jitter[0], SCREEN_HEIGHT - 74 + jitter[1]))
            self.text(scr, c.name.upper(), 22, WHITE, (px - 70, SCREEN_HEIGHT - 108), 'topleft')
            for i in range(c.current_lives):
                pygame.draw.circle(scr, c.colors['main'], (int(px + 84 + i * 20), SCREEN_HEIGHT - 98), 7)
                pygame.draw.circle(scr, WHITE, (int(px + 84 + i * 20), SCREEN_HEIGHT - 98), 7, 2)
            bar = pygame.Rect(px - 70, SCREEN_HEIGHT - 38, 208, 14)
            pygame.draw.rect(scr, (20, 30, 50), bar, border_radius=7)
            fill = bar.copy()
            fill.w = int(bar.w * c.aether_gauge / AETHER_MAX)
            acol = (255, 120, 120) if c.flash > 0 and c.aether_gauge < 30 else \
                lerp_color((70, 150, 255), (140, 255, 250), c.aether_gauge / AETHER_MAX)
            if fill.w > 3:
                pygame.draw.rect(scr, acol, fill, border_radius=7)
            pygame.draw.rect(scr, WHITE, bar, 2, border_radius=7)
            self.text(scr, "AETHER", 16, (170, 220, 255), (px - 70, SCREEN_HEIGHT - 52), 'topleft', False)
        mid = (SCREEN_WIDTH // 2, SCREEN_HEIGHT // 2 - 60)
        if self.intro > 40:
            self.text(scr, "READY...", 110, WHITE, mid)
        elif self.intro > 0:
            self.text(scr, "CLASH!", 150, (255, 220, 90), mid)
        if self.end_timer > 0:
            self.text(scr, "GAME!", 170, (255, 90, 90), mid)
        if self.paused:
            dim = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
            dim.fill((0, 0, 20, 170))
            scr.blit(dim, (0, 0))
            self.text(scr, "PAUSED", 100, WHITE, (SCREEN_WIDTH // 2, 110))
            self._render_controls(190)
            self.text(scr, "ENTER / ESC resume      Q quit to menu      F1 show hitboxes", 26, (200, 210, 255),
                      (SCREEN_WIDTH // 2, 540))

    def _render_controls(self, y):
        rows = [("", "PLAYER 1", "PLAYER 2"),
                ("Move / aim", "A D / W S", "Arrow keys"),
                ("Jump  (tap = short hop, again in air = double jump)", "Space", "Right Ctrl  or Numpad 0"),
                ("Attack  (+ direction = different move)", "J", ",   or Numpad 1"),
                ("Heavy / smash attack  (+ up / down)", "K", ".   or Numpad 2"),
                ("Special  (+ direction, costs Aether)", "L", "/   or Numpad 3"),
                ("Shield  (tap just before a hit = PARRY)", "Left Shift  or I", "Right Shift  or Numpad Enter"),
                ("Grab = Shield + Attack    Roll = Shield + left/right    Air dodge = Shield in the air", "", ""),
                ("Down = fast-fall / drop through thin platforms.  Hang on ledges, press Up to climb.", "", "")]
        for i, (a, b, c) in enumerate(rows):
            col = (255, 220, 120) if i == 0 else WHITE
            self.text(self.screen, a, 24, (190, 200, 240), (120, y + i * 34), 'topleft')
            self.text(self.screen, b, 24, col, (700, y + i * 34), 'topleft')
            self.text(self.screen, c, 24, col, (900, y + i * 34), 'topleft')

    def _render_menu(self):
        scr = self.screen
        if not self.thumbs:
            self.thumbs = []
            for st in self.stages:
                self.map_data, pe = st, self.physics_engine
                self.physics_engine = None
                view = pygame.Rect(400, 330, 1600, 900)
                self._draw_world(view)
                self.thumbs.append(pygame.transform.smoothscale(self.world.subsurface(view), (288, 162)))
                self.physics_engine = pe
            self.menu_bg = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT))
            for y in range(SCREEN_HEIGHT):
                pygame.draw.line(self.menu_bg, lerp_color((14, 10, 40), (60, 26, 84), y / SCREEN_HEIGHT),
                                 (0, y), (SCREEN_WIDTH, y))
        scr.blit(self.menu_bg, (0, 0))
        t = self.frame
        self.text(scr, "AETHER CLASH", 104, (255, 220, 120), (SCREEN_WIDTH // 2, 58))
        self.text(scr, "a platform fighter prototype", 26, (190, 180, 240), (SCREEN_WIDTH // 2, 108))
        for slot in (0, 1):
            spec = ROSTER[self.sel[slot]]
            col = spec['alt'] if (slot == 1 and self.sel[0] == self.sel[1]) else spec['colors']
            x0 = 40 if slot == 0 else SCREEN_WIDTH - 480
            card = pygame.Rect(x0, 140, 440, 330)
            panel = pygame.Surface(card.size, pygame.SRCALPHA)
            pygame.draw.rect(panel, (10, 8, 30, 190), panel.get_rect(), border_radius=18)
            scr.blit(panel, card)
            pygame.draw.rect(scr, PLAYER_TAGS[slot], card, 3, border_radius=18)
            who = ("CPU - Hard" if self.p1_cpu else "Human") if slot == 0 else self.P2_MODES[self.p2_mode]
            self.text(scr, f"PLAYER {slot + 1}  -  {who}", 26, PLAYER_TAGS[slot], (card.x + 18, card.y + 12), 'topleft')
            pose = 'run' if (t // 180) % 2 else 'idle'
            spr = render_fighter_surface(spec, col, pose, 1 if slot == 0 else -1, t)
            spr = pygame.transform.scale(spr, (SPR * 3 // 2, SPR * 3 // 2))
            scr.blit(spr, (card.x - 80 + (0 if slot == 0 else 20), card.y - 50))
            tx = card.x + 210
            self.text(scr, f"<  {spec['name'].upper()}  >", 46, WHITE, (tx, card.y + 46), 'topleft')
            self.text(scr, f"{spec['title']}  -  {spec['faction']}", 24, col['accent'], (tx, card.y + 86), 'topleft')
            for i, (label, val) in enumerate(zip(("Speed", "Power", "Weight", "Range"), spec['bars'])):
                yy = card.y + 122 + i * 24
                self.text(scr, label, 20, (190, 200, 240), (tx, yy), 'topleft', False)
                for k in range(5):
                    pygame.draw.rect(scr, col['main'] if k < val else (50, 46, 80), (tx + 70 + k * 24, yy + 1, 20, 12),
                                     border_radius=3)
            for i, line in enumerate(spec['blurb']):
                self.text(scr, line, 21, (220, 220, 240), (card.x + 18, card.y + 244 + i * 24), 'topleft', False)
        self.text(scr, "A / D", 24, PLAYER_TAGS[0], (260, 486))
        self.text(scr, "LEFT / RIGHT   -   TAB: human or CPU", 24, PLAYER_TAGS[1], (SCREEN_WIDTH - 260, 486))
        st = self.stages[self.stage_sel]
        self.text(scr, "VS", 80, WHITE, (SCREEN_WIDTH // 2, 200))
        thumb = self.thumbs[self.stage_sel]
        tr = thumb.get_rect(center=(SCREEN_WIDTH // 2, 350))
        scr.blit(thumb, tr)
        pygame.draw.rect(scr, (255, 220, 120), tr, 3, border_radius=4)
        self.text(scr, f"<  {st.name}  >", 30, (255, 220, 120), (SCREEN_WIDTH // 2, 250))
        self.text(scr, "W / S  change stage", 20, (190, 180, 240), (SCREEN_WIDTH // 2, 448))
        for i, line in enumerate(st.blurb):
            self.text(scr, line, 20, (220, 220, 240), (SCREEN_WIDTH // 2, 468 + i * 20), shadow=False)
        self.text(scr, "P1:  A/D move   SPACE jump   J attack   K heavy   L special   LEFT SHIFT shield / parry",
                  23, WHITE, (SCREEN_WIDTH // 2, 540))
        self.text(scr, "Hold a direction with an attack button for a different move.   ESC in a match = pause + full controls.",
                  21, (190, 200, 240), (SCREEN_WIDTH // 2, 568))
        self.text(scr, "Knock your rival off the screen. The more damage (%) they have, the further they fly. 3 lives each.",
                  21, (190, 200, 240), (SCREEN_WIDTH // 2, 594))
        if (t // 30) % 2:
            self.text(scr, "PRESS ENTER TO CLASH", 44, (255, 220, 120), (SCREEN_WIDTH // 2, 650))
        self.text(scr, "F2: make Player 1 a CPU (watch mode)     ESC: quit", 18, (150, 150, 200), (SCREEN_WIDTH // 2, 700),
                  shadow=False)

    def _render_result(self):
        scr = self.screen
        dim = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
        dim.fill((0, 0, 20, 190))
        scr.blit(dim, (0, 0))
        w = self.winner
        if w:
            spr = render_fighter_surface(w.spec, w.colors, 'parry', 1, self.frame)
            scr.blit(pygame.transform.scale(spr, (SPR * 2, SPR * 2)), (SCREEN_WIDTH // 2 - SPR, 150))
            who = "CPU" if w.is_cpu else f"PLAYER {w.slot + 1}"
            self.text(scr, f"{w.name.upper()} WINS!", 110, w.colors['accent'], (SCREEN_WIDTH // 2, 100))
            self.text(scr, f"{who}  -  {w.spec['title']}  -  {w.current_lives} lives left", 30, WHITE, (SCREEN_WIDTH // 2, 170))
        else:
            self.text(scr, "DRAW!", 110, WHITE, (SCREEN_WIDTH // 2, 100))
        self.text(scr, "ENTER: rematch        ESC: character select", 32, (255, 220, 120), (SCREEN_WIDTH // 2, 660))

    def stop_game(self):
        self.running = False


# =====================================================================
# MAIN EXECUTION BLOCK
# =====================================================================

def run_selftest(frames=3600):
    """Headless smoke test: CPU vs CPU on every stage with every character."""
    random.seed(7)
    game = GameManager(headless=True)
    n = len(ROSTER)
    for i in range(n * len(game.stages)):
        a, b, s = i % n, (i + 1 + i // n) % n, i % len(game.stages)
        game.setup_game(a, b, s, p2_mode=2, p1_cpu=True)
        f = 0
        while game.scene == 'FIGHT' and f < frames:
            game._step()
            f += 1
            if f % 240 == 0:
                game._render_frame()
        c1, c2 = game.characters[1], game.characters[2]
        print(f"{game.map_data.name:20s} {c1.name:5s} vs {c2.name:5s}: {f:5d} frames | lives {c1.current_lives}-{c2.current_lives}"
              f" | dmg {c1.cumulative_damage_taken:5.0f}% {c2.cumulative_damage_taken:5.0f}% | scene {game.scene}")
    print("SELFTEST OK")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        run_selftest()
    else:
        GameManager(demo="--demo" in sys.argv).run_game_loop()
