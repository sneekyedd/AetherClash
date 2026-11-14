#!/usr/bin/env python3
"""
AETHER CLASH - a 2D platform fighter prototype (single-file, Pygame).

Run:      python aether_clash.py
Options:  --demo       watch CPU vs CPU
          --selftest   headless smoke test of every character / stage
          --make-icon FILE.png   save the game icon

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
import json
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

# --- Grabs ------------------------------------------------------------
GRAB_HOLD_FRAMES = 80       # how long a grab holds before the victim breaks free
GRAB_MIN_HOLD = 12          # frames before a held direction turns into a throw
POMMEL_DAMAGE = 2.0
POMMEL_COOLDOWN = 14

# --- Secret ----------------------------------------------------------
SECRET_NAME = "crypticsh"   # players with this name can fire the Death Ray at full Aether
DUEL_TARGET = 25            # correct key presses needed to win the Death Ray duel

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
    Hitboxes sharing a gid can only hit a target once between them (multi-hit moves use several gids).
    spike=True marks a "spike spot": if it connects with an AIRBORNE target they are launched straight down.
    Spike spots are listed before the move's ordinary hitboxes so they take priority when both overlap."""
    d = dict(start=start, end=end, rect=(x, y, w, h), damage=damage, force=force,
             angle=angle, gid=gid, grab=False, radial=False, pull=False, spike=False, unblockable=False)
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
        self.player_name, self.is_cpu = name, False
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
        self.grab_target = self.grabbed_by = None
        self.grab_timer = self.grab_frames = self.pommel_cd = self.pommel_anim = 0
        self.pose = None                  # smoothed skeleton pose (owned by the StateTranslator)
        self.cloth = None                 # rope physics for scarves, hair and flames
        self.squash = 0.0                 # >0 stretched tall (jumping), <0 squashed flat (landing)

    @property
    def is_cryptic(self):
        return self.player_name.strip().lower() == SECRET_NAME

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
    dsmash=A("Seam Splitter", 42, hb(8, 9, 74, 28, 26, 24, 13, 13, 25, spike=True), hb(8, 9, -74, 28, 26, 24, 13, 13, 155, spike=True),
             hb(8, 11, 50, 26, 64, 24, 12, 15, 25), hb(8, 11, -50, 26, 64, 24, 12, 15, 155)),
    nair=A("Whirl", 30, hb(4, 8, 0, 0, 90, 80, 3, 5, 70), hb(12, 16, 0, 0, 96, 84, 5, 9, 50, gid=1)),
    fair=A("Needle Rush", 28, hb(6, 10, 48, -4, 60, 36, 8, 10.5, 40)),
    bair=A("Back Lash", 30, hb(7, 10, -50, -4, 62, 32, 11, 14, 150)),
    uair=A("Sky Thread", 26, hb(5, 10, 4, -54, 56, 48, 7, 10, 85)),
    dair=A("Plumb Line", 34, hb(10, 12, 6, 62, 26, 28, 12, 13, 75, spike=True), hb(10, 16, 6, 44, 40, 56, 8, 9, 70), landing_lag=10),
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
    dsmash=A("Ground Slam", 56, hb(18, 19, 100, 34, 36, 40, 21, 15, 80, spike=True), hb(18, 19, -100, 34, 36, 40, 21, 15, 100, spike=True),
             hb(18, 22, 0, 36, 230, 36, 20, 17.5, 80, radial=True)),
    nair=A("Rolling Boulder", 38, hb(6, 20, 0, 0, 96, 100, 10, 11, 45), landing_lag=10),
    fair=A("Maul Swing", 44, hb(14, 18, 54, 0, 70, 80, 15, 15, 35), landing_lag=12),
    bair=A("Buttress", 36, hb(10, 14, -56, -4, 70, 50, 14, 15, 145), landing_lag=10),
    uair=A("Capstone", 34, hb(8, 13, 0, -64, 80, 50, 12, 13, 90)),
    dair=A("Portcullis", 46, hb(12, 16, 0, 64, 40, 30, 18, 15, 80, spike=True), hb(12, 30, 0, 46, 64, 56, 13, 12, 60),
           impulses={12: (0, 16)}, landing_lag=18),
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
    dsmash=A("Eclipse Ring", 48, hb(10, 11, 92, 26, 30, 38, 14, 13, 30, spike=True), hb(10, 11, -92, 26, 30, 38, 14, 13, 150, spike=True),
             hb(10, 14, 0, 28, 210, 34, 13, 15, 30, radial=True)),
    nair=A("Constellation", 34, hb(5, 8, 0, 0, 110, 110, 2, 3, 80), hb(11, 14, 0, 0, 110, 110, 2, 3, 80, gid=1),
           hb(17, 21, 0, 0, 120, 120, 5, 10, 50, gid=2, radial=True)),
    fair=A("Starfall Fan", 32, hb(8, 12, 54, -2, 70, 56, 9, 11, 45)),
    bair=A("Umbra Kick", 30, hb(7, 10, -52, 0, 64, 34, 10, 13, 150)),
    uair=A("Moonbeam", 30, hb(6, 12, 0, -66, 40, 80, 8, 10.5, 90)),
    dair=A("Meteor Seed", 36, hb(11, 13, 0, 62, 28, 28, 12, 13, 80, spike=True), hb(11, 15, 0, 48, 48, 48, 8, 9, 70), landing_lag=10),
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
    dsmash=A("Magma Stomp", 46, hb(12, 13, 84, 28, 30, 36, 16, 14, 35, spike=True), hb(12, 13, -84, 28, 30, 36, 16, 14, 145, spike=True),
             hb(12, 16, 0, 30, 190, 32, 15, 16, 35, radial=True)),
    nair=A("Cinder Wheel", 30, hb(4, 16, 0, 0, 84, 84, 8, 9.5, 50)),
    fair=A("Hammer Fist", 34, hb(10, 11, 48, 24, 34, 34, 14, 13.5, 45, spike=True), hb(10, 13, 44, 6, 54, 60, 11, 12, 45), landing_lag=10),
    bair=A("Backdraft", 30, hb(7, 10, -46, -6, 56, 40, 12, 14.5, 150)),
    uair=A("Spark Flip", 28, hb(5, 11, 0, -56, 70, 44, 9, 11, 80)),
    dair=A("Meteor Kick", 40, hb(8, 12, 20, 50, 32, 32, 13, 13, 70, spike=True), hb(8, 30, 16, 40, 46, 46, 9, 10, 60),
           impulses={8: (7, 17)}, landing_lag=14),
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
         colors=dict(main=(170, 44, 54), dark=(104, 30, 46), accent=(230, 182, 86), skin=(236, 192, 152),
                     hair=(122, 70, 44), band=(200, 52, 56), wrap=(208, 172, 122), boot=(118, 66, 46)),
         alt=dict(main=(40, 150, 144), dark=(16, 76, 82), accent=(250, 236, 170), skin=(236, 192, 152),
                  hair=(70, 74, 96), band=(250, 236, 170), wrap=(200, 206, 214), boot=(50, 60, 80))),
    dict(name="Bronn", title="The Sentinel", faction="Sentinel", style='sentinel', moves=BRONN_MOVE_SET,
         blurb=["Stone-clad guardian. Slow and heavy with", "armored attacks, a counter stance and", "walls that punish reckless aggression."],
         stats=dict(speed=5.4, air_speed=4.4, jump_height=15.5, double_jump=13.5, gravity=0.78, max_fall=14,
                    weight=124, w=64, h=94),
         bars=(2, 5, 5, 3), ai=dict(mid=['sspecial', 'dspecial'], close=['nspecial']),
         colors=dict(main=(70, 124, 192), dark=(34, 58, 108), accent=(120, 240, 255), skin=(92, 150, 210),
                     stone=(136, 142, 154), steel=(158, 164, 178), belt=(122, 82, 54)),
         alt=dict(main=(176, 120, 60), dark=(90, 56, 24), accent=(255, 226, 150), skin=(204, 150, 84),
                  stone=(150, 140, 130), steel=(170, 160, 150), belt=(70, 50, 40))),
    dict(name="Nyx", title="The Hollow Star", faction="Voidcaller", style='voidcaller', moves=NYX_MOVE_SET,
         blurb=["Floaty star-mage with three jumps.", "Controls space with orbs, lances and a", "gravity well; teleports to recover."],
         stats=dict(speed=6.2, air_speed=5.6, jump_height=13.5, double_jump=12.5, gravity=0.46, max_fall=8.5,
                    weight=88, w=42, h=82, air_jumps=2),
         bars=(3, 3, 2, 5), ai=dict(mid=['nspecial', 'sspecial', 'dspecial'], close=[]),
         colors=dict(main=(104, 70, 156), dark=(52, 34, 88), accent=(255, 122, 230), skin=(18, 10, 32),
                     hat=(118, 80, 168), hatband=(162, 116, 208)),
         alt=dict(main=(36, 70, 120), dark=(14, 30, 60), accent=(140, 255, 220), skin=(8, 16, 34),
                  hat=(48, 92, 150), hatband=(90, 140, 190))),
    dict(name="Rhea", title="The Cinderfist", faction="Kindled", style='kindled', moves=RHEA_MOVE_SET,
         blurb=["Close-range brawler. Short reach but", "every punch hurts. Spend Aether to", "Ignite and hit 30% harder."],
         stats=dict(speed=7.0, air_speed=5.4, jump_height=15, double_jump=13.5, gravity=0.72, max_fall=13,
                    weight=104, w=52, h=84),
         bars=(4, 4, 3, 1), ai=dict(mid=['sspecial', 'dspecial'], close=['nspecial']),
         colors=dict(main=(222, 98, 34), dark=(128, 62, 32), accent=(255, 216, 92), skin=(246, 200, 160),
                     flame=(246, 104, 26), band=(138, 90, 52), wrap=(216, 186, 142)),
         alt=dict(main=(78, 82, 100), dark=(44, 46, 60), accent=(96, 206, 255), skin=(228, 162, 114),
                  flame=(40, 96, 230), band=(30, 32, 44), wrap=(190, 196, 210))),
]
# The secret super move: a comically large gun that blasts the whole screen. Cannot be shielded.
DEATH_RAY = S("Death Ray", 130, 0, hb(56, 60, 0, 0, 8000, 6000, 40, 21, 45, radial=True, unblockable=True),
              impulses={0: (0, 0)}, intangible=(0, 112), no_gravity=(0, 112), effect=('deathray', 56))
DEATH_RAY.slot = 'deathray'

for _spec in ROSTER:
    for _slot, _move in _spec['moves'].items():
        _move.slot = _slot


# =====================================================================
# EFFECTS (particles, rings, pop-up text, screen shake)
# =====================================================================

class Effects:
    def __init__(self):
        self.particles, self.rings, self.texts, self.shake = [], [], [], 0

    def spark(self, x, y, color, n=10, speed=6.0, life=18, size=4, grav=0.0, direction=None, spread=math.pi,
              shape='circle'):
        for _ in range(n):
            a = (direction if direction is not None else 0) + random.uniform(-spread, spread)
            s = speed * random.uniform(0.3, 1.0)
            self.particles.append([x, y, math.cos(a) * s, math.sin(a) * s, random.randint(life // 2, life), life, color, size, grav, shape])

    def style_burst(self, style, x, y, colors, n=6, power=5.0):
        """Each fighter sheds their own material: embers, thread, stone chips or stardust."""
        if style == 'kindled':
            self.spark(x, y, (255, 140, 40), n, power * 0.7, 50, 4, grav=0.1)
            self.spark(x, y, colors['accent'], max(1, n // 2), power * 0.5, 40, 3, grav=0.06)
        elif style == 'weaver':
            self.spark(x, y, colors['accent'], n, power, 34, 3, grav=0.05, shape='line')
        elif style == 'sentinel':
            self.spark(x, y, (150, 156, 172), n, power, 40, 5, grav=0.45, shape='square')
        else:
            self.spark(x, y, colors['accent'], n, power * 0.6, 52, 5, grav=-0.03, shape='star')

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
        for x, y, vx, vy, life, mx, color, size, _, shape in self.particles:
            r = max(1, int(size * life / mx))
            if shape == 'square':                        # stone chips
                pygame.draw.rect(surf, color, (int(x) - r, int(y) - r, r * 2, r * 2))
            elif shape == 'line':                        # thread snippets
                pygame.draw.line(surf, color, (x, y), (x - vx * 2.5 - 3, y - vy * 2.5), 2)
            elif shape == 'star':                        # stardust
                pygame.draw.line(surf, color, (x - r, y), (x + r, y), 2)
                pygame.draw.line(surf, color, (x, y - r), (x, y + r), 2)
            else:
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
            if c.current_state == "GRABBED":             # held in place in front of the grabber
                g = c.grabbed_by
                if g:
                    c.x, c.y = g.x + g.facing * (g.w / 2 + c.w / 2 - 6), g.y
                    c.facing, c.ground = -g.facing, g.ground
                    c.vx = c.vy = c.kbx = c.kby = 0.0
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
        if m.effect and m.effect[0] == 'deathray':
            if f < m.effect[1]:
                self.fx.shake = max(self.fx.shake, 2 + f // 9)
            elif f == m.effect[1]:
                self.fx.shake = 36
                self.fx.text(c.x, c.y - c.h - 70, "DEATH RAY!!", (255, 90, 90), 90)
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
                # push out on the side we came from; if we were already inside, use the NEAREST side
                # (the old "by travel direction" rule could warp a fighter to the opposite edge)
                prev_x = c.x - dx
                if prev_x <= p.x:
                    from_left = True
                elif prev_x >= p.x + p.width:
                    from_left = False
                else:
                    from_left = c.x < p.x + p.width / 2
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
        c.squash = -min(0.24, 0.06 + abs(c.vy) * 0.014)       # squash on landing, harder after a long fall
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
                if self.frame % 3 == 0 and not box['spike'] and rect.w < 400:     # weapons shed particles
                    self.fx.style_burst(attacker.spec['style'], rect.centerx, rect.centery, attacker.colors, 1, 2.5)
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
        self.pending_duel = None             # set when two "crypticsh" players must duel for the Death Ray

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
        if target.current_state == "GRABBED":
            return 'ignored'
        if not hitbox['grab'] and not hitbox['unblockable']:
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
        spiked = False
        if hitbox['grab']:                             # grabs hold the target; the throw comes later
            if target.current_state in ("HIT", "LEDGE", "GRABBING"):
                return 'ignored'
            return self.start_grab(attacker, target)
        if hitbox['spike'] and target.ground is None:  # spike spot on an airborne target: straight down
            direction, spiked = (0.0, 1.0), True
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
        self.fx.style_burst(target.spec['style'], tx, ty, target.colors, 4 + int(damage * 0.4), 4 + damage * 0.2)
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
        if spiked:
            self.fx.text(tx, ty - target.h, "SPIKE!", (255, 120, 255), 50)
            self.fx.ring(tx, ty, (255, 120, 255), 14, 8, 16)
            self.fx.spark(tx, ty, (255, 160, 255), 14, 12, 22, 5, direction=math.pi / 2, spread=0.5)
        return 'hit'

    # ------------------------------------------------------------ grabs
    def start_grab(self, attacker, target):
        attacker.current_move = None
        attacker.current_state = "GRABBING"
        attacker.grab_target = target
        attacker.grab_timer = GRAB_HOLD_FRAMES + int(target.cumulative_damage_taken * 0.3)
        attacker.grab_frames = attacker.pommel_cd = 0
        attacker.vx = 0.0
        target.current_move = None
        target.current_state = "GRABBED"
        target.grabbed_by = attacker
        target.is_knocked_down = False
        target.hitstun = target.stun_timer = target.dodge_timer = target.lag = 0
        target.ledge = None
        target.vx = target.vy = target.kbx = target.kby = 0.0
        self.fx.ring(target.x, target.y - target.h / 2, WHITE, 14, 4, 10)
        return 'grab'

    def pommel(self, attacker):
        """A quick strike on a held opponent: small damage, keeps the grab going."""
        t = attacker.grab_target
        if not t:
            return
        dmg = POMMEL_DAMAGE * (IGNITE_MULT if attacker.buff_timer > 0 else 1.0)
        t.cumulative_damage_taken += dmg
        attacker.aether_gauge = min(AETHER_MAX, attacker.aether_gauge + dmg * AETHER_PER_DAMAGE_DEALT)
        attacker.pommel_cd, attacker.pommel_anim = POMMEL_COOLDOWN, 9
        t.flash = 6
        tx, ty = t.center
        self.fx.spark(tx, ty - 10, WHITE, 5, 6, 10, 3)
        self.fx.style_burst(t.spec['style'], tx, ty - 10, t.colors, 3, 4)
        self.fx.shake = max(self.fx.shake, 3)

    def release_grab(self, attacker, push=True):
        t, attacker.grab_target = attacker.grab_target, None
        if attacker.current_state == "GRABBING":
            attacker.current_state = "IDLE" if attacker.ground else "JUMPING"
            attacker.lag = 10 if push else attacker.lag
        if t and t.grabbed_by is attacker:
            t.grabbed_by = None
            if t.current_state == "GRABBED":
                t.current_state = "IDLE" if t.ground else "JUMPING"
                if push:
                    t.vx, attacker.vx = attacker.facing * 7, -attacker.facing * 4
        return t

    def throw(self, attacker, dir_x, dir_y):
        """Throw the held opponent in the chosen direction (forward, back, up or down)."""
        box = attacker.move_set['grab'].hitbox_data[0]
        if dir_y < 0:
            ang = 88
        elif dir_y > 0:
            ang = 285                                  # slammed into the floor, they bounce up
        else:
            ang = 42
            if dir_x == -attacker.facing:
                attacker.facing *= -1
        t = self.release_grab(attacker, push=False)
        if not t:
            return
        t.x = attacker.x + attacker.facing * (attacker.w / 2 + t.w / 2)
        dmg = box['damage'] * (IGNITE_MULT if attacker.buff_timer > 0 else 1.0)
        t.cumulative_damage_taken += dmg
        attacker.aether_gauge = min(AETHER_MAX, attacker.aether_gauge + dmg * AETHER_PER_DAMAGE_DEALT)
        t.aether_gauge = min(AETHER_MAX, t.aether_gauge + dmg * AETHER_PER_DAMAGE_TAKEN)
        direction = (math.cos(math.radians(ang)) * attacker.facing, -math.sin(math.radians(ang)))
        speed = self.physics.apply_knockback(attacker, t, box['force'], direction)
        self._enter_hit_state(t, speed)
        attacker.hitstop = t.hitstop = 6
        attacker.lag = 16
        tx, ty = t.center
        self.fx.spark(tx, ty, attacker.colors['accent'], 12, 8, 18, 5)
        self.fx.style_burst(t.spec['style'], tx, ty, t.colors, 6, 6)
        self.fx.shake = max(self.fx.shake, 6)

    # ------------------------------------------------------------ the secret Death Ray
    def request_death_ray(self, c):
        rivals = [o for o in self.physics.active_characters if o is not c and o.alive and o.is_cryptic]
        if rivals:
            self.pending_duel = c                      # both are crypticsh: the Game Manager runs a duel
        else:
            self.fire_death_ray(c)

    def fire_death_ray(self, c):
        if c.grab_target:
            self.release_grab(c)
        c.aether_gauge = 0.0
        self.process_attack(c, DEATH_RAY)

    def _enter_hit_state(self, target, speed):
        target.current_move = None
        target.current_state = "HIT"
        target.is_knocked_down = speed >= TUMBLE_SPEED
        target.hitstun = int(speed * HITSTUN_PER_SPEED) + 4
        target.stun_timer = target.dodge_timer = target.lag = 0
        target.fast_fall = target.jump_cut = False
        target.ledge = None
        target.grabbed_by = None

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
            c.squash *= 0.8
            if c.spec.get('style') == 'kindled' and c.anim % 9 == 0:        # her hair sheds the odd ember
                self.fx.spark(c.x - c.facing * 6, c.y - c.h - 14, c.colors['accent'], 1, 1.6, 44, 3, grav=-0.07,
                              direction=-math.pi / 2, spread=0.9)
            for attr in ('intangible', 'lag', 'ledge_cd', 'drop_timer', 'buff_timer', 'flash', 'shield_stun',
                         'pommel_cd', 'pommel_anim'):
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
            elif st == "GRABBING":
                c.grab_frames += 1
                c.grab_timer -= 1
                t = c.grab_target
                if t is None or t.current_state != "GRABBED" or c.grab_timer <= 0:
                    self.release_grab(c)
            elif st == "GRABBED":
                g = c.grabbed_by
                if g is None or g.current_state != "GRABBING" or g.grab_target is not c:
                    c.grabbed_by = None
                    c.current_state = "IDLE" if c.ground else "JUMPING"
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
                 shield=[pygame.K_LSHIFT, pygame.K_i], grab=[pygame.K_h])
KEYMAP_P2 = dict(left=[pygame.K_LEFT], right=[pygame.K_RIGHT], up=[pygame.K_UP], down=[pygame.K_DOWN],
                 jump=[pygame.K_RCTRL, pygame.K_KP0], attack=[pygame.K_COMMA, pygame.K_KP1],
                 heavy=[pygame.K_PERIOD, pygame.K_KP2], special=[pygame.K_SLASH, pygame.K_KP3],
                 shield=[pygame.K_RSHIFT, pygame.K_KP_ENTER], grab=[pygame.K_m, pygame.K_KP4])
BUTTONS = ('jump', 'attack', 'heavy', 'special', 'shield', 'grab')
PAD_BUTTONS = dict(jump=[0], special=[1], attack=[2], heavy=[3], shield=[4], grab=[5])
ACTIONS = [('left', "Move left"), ('right', "Move right"), ('up', "Up / aim up"), ('down', "Down / drop"),
           ('jump', "Jump"), ('attack', "Attack"), ('heavy', "Heavy attack"), ('special', "Special"),
           ('shield', "Shield / parry"), ('grab', "Grab")]


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
        if st == "GRABBING":                             # holding someone: Attack = pommel, direction = throw
            c.move_x = 0
            if c.pommel_cd == 0 and self._take('attack'):
                self.combat.pommel(c)
            elif c.grab_frames >= GRAB_MIN_HOLD and (inp.x or inp.y):
                self.combat.throw(c, inp.x, inp.y)
            return
        if st in ("HIT", "STUN", "DODGE", "GRABBED") or c.lag > 0:
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
        if grounded and self._take('grab'):
            self.combat.process_attack(c, 'grab')
            return
        if self._take('special'):
            if not inp.x and not inp.y and c.is_cryptic and c.aether_gauge >= AETHER_MAX:
                self.combat.request_death_ray(c)
                return
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
        c.squash = 0.2                                   # stretch upward as they leave the ground
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
        if self._take('attack') or self._take('grab'):
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
        self.cool, self.queue, self.shield_hold, self.jump_hold, self.wait, self.pommels = 40, [], 0, 0, 0, 0

    def _press(self, inp, slot, toward):
        c = self.char
        if slot == 'grab':
            inp.pressed.add('grab')
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
        if state == "GRABBING":                          # pommel a few times, then throw somewhere nasty
            t = c.grab_target
            if c.grab_frames < 2:
                self.pommels = random.randint(0, 3)
            if c.grab_frames < GRAB_MIN_HOLD + 3 or not t:
                return inp
            if self.pommels > 0:
                if c.pommel_cd == 0:
                    inp.pressed.add('attack')
                    self.pommels -= 1
                return inp
            edge = -1 if c.x < (L + R) / 2 else 1
            if t.cumulative_damage_taken > 110:
                inp.x, inp.y = random.choice([(edge, 0), (0, -1)])
            else:
                inp.x, inp.y = random.choice([(edge, 0), (-edge, 0), (0, -1), (0, 1)])
            return inp
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
        if c.is_cryptic and c.aether_gauge >= AETHER_MAX and grounded:
            inp.x = inp.y = 0
            inp.pressed.add('special')
            self.cool = think
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
# ART: skeleton-animated fighters and stage scenery, all drawn with code
# (stand-ins until real PNG assets exist)
# =====================================================================
SPR, SPR_CX, SPR_FEET = 340, 170, 262
OUTLINE = (22, 14, 34)
MAUL_SLOTS = {'fsmash', 'usmash', 'dsmash', 'utilt', 'fair', 'bair', 'uair', 'nair'}   # Bronn swings his maul


def glow(surf, color, pos, radius, alpha):
    radius = max(2, int(radius))
    tmp = pygame.Surface((radius * 2, radius * 2), pygame.SRCALPHA)
    for i in range(4, 0, -1):
        pygame.draw.circle(tmp, (*color, alpha // i), (radius, radius), radius * i // 4)
    surf.blit(tmp, (pos[0] - radius, pos[1] - radius))


def star_points(x, y, r, rot=0.0):
    return [(x + math.cos(rot + i * math.pi / 4) * (r if i % 2 == 0 else r * 0.36),
             y + math.sin(rot + i * math.pi / 4) * (r if i % 2 == 0 else r * 0.36)) for i in range(8)]


def ik(a, b, l1, l2, bend):
    """Two-bone inverse kinematics: where is the elbow/knee if the limb goes from a to b?
    Returns (joint, b_clamped). b is pulled in if it is further than the limb can reach."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    d = math.hypot(dx, dy) or 0.001
    ux, uy = dx / d, dy / d
    reach = l1 + l2 - 0.5
    if d > reach:
        d = reach
        b = (a[0] + ux * d, a[1] + uy * d)
    d = max(d, abs(l1 - l2) + 0.5)
    k = (l1 * l1 - l2 * l2 + d * d) / (2 * d)
    hgt = math.sqrt(max(0.0, l1 * l1 - k * k))
    return (a[0] + ux * k - uy * hgt * bend, a[1] + uy * k + ux * hgt * bend), b


def attack_limb(style, box):
    """Which body part delivers this hitbox, so the animation matches the attack."""
    x, y, _, _ = box['rect']
    if abs(x) < 16 and abs(y) < 16:
        return 'burst' if box['radial'] else 'spin'
    if box['radial'] and y > 18:
        return 'slam'
    if style in ('sentinel', 'kindled') and (y > 18 or x < -10):
        return 'foot'
    return 'hand'


def target_pose(spec, key, t, info=None):
    """The pose a fighter is aiming for this frame, as joint positions.
    Local coordinates: x = forward, y = up is negative, origin = between the feet."""
    info = info or {}
    st = spec['stats']
    w, h, style = st['w'], st['h'], spec['style']
    hipy, shy = -h * 0.38, -h * 0.68
    P = dict(hip=[0.0, hipy], sh=[0.0, shy], head=[0.0, 0.0],
             hf=[w * 0.42, shy + h * 0.14], hb=[w * 0.05, shy + h * 0.2],
             ff=[w * 0.3, 0.0], fb=[-w * 0.28, 0.0], spin=0.0, wp=-0.9)
    br = math.sin(t * 0.07)
    if key == 'idle':
        P['sh'][1] += br * 1.5
        P['hf'][1] += br * 2.5
        P['hb'][1] += br * 2
        if style == 'kindled':                               # boxer's bounce
            b = abs(math.sin(t * 0.12)) * 3
            P['hip'][1] -= b
            P['sh'][1] -= b
            P['hf'], P['hb'] = [w * 0.5, shy + h * 0.06 - b], [w * 0.22, shy + h * 0.12 - b]
    elif key == 'run':
        a = t * 0.36
        s_, c_ = math.sin(a), math.cos(a)
        S = w * 0.62
        P['ff'] = [S * s_, -max(0.0, c_) * h * 0.13]
        P['fb'] = [-S * s_, -max(0.0, -c_) * h * 0.13]
        P['hf'] = [-S * 0.7 * s_ + w * 0.1, shy + h * 0.22 - abs(s_) * 3]      # arms swing opposite the legs
        P['hb'] = [S * 0.7 * s_ - w * 0.1, shy + h * 0.22 - abs(s_) * 3]
        P['sh'] = [h * 0.11, shy + 2 - abs(c_) * 2]
        P['hip'][1] = hipy + 2 - abs(c_) * 2
        P['wp'] = -0.3
    elif key == 'jump':
        P['ff'], P['fb'] = [w * 0.22, -h * 0.16], [-w * 0.3, -h * 0.05]
        P['hf'], P['hb'] = [w * 0.38, shy - h * 0.08], [-w * 0.36, shy + h * 0.02]
        P['wp'] = -1.2
    elif key in ('fall', 'helpless'):
        wig = math.sin(t * 0.5) * 4 if key == 'helpless' else 0
        P['ff'], P['fb'] = [w * 0.3, -h * 0.03], [-w * 0.26, -h * 0.1]
        P['hf'], P['hb'] = [w * 0.5 + wig, shy - h * 0.14], [-w * 0.5 - wig, shy - h * 0.12]
        P['sh'][0] = -2 if key == 'fall' else -h * 0.06
        P['wp'] = -1.5
    elif key == 'hurt':
        P['sh'] = [-h * 0.13, shy + 2]
        P['head'] = [-3.0, 2.0]
        P['hf'], P['hb'] = [w * 0.5, shy - h * 0.02], [w * 0.3, shy + h * 0.12]
        P['ff'], P['fb'] = [w * 0.42, -h * 0.1], [w * 0.1, -h * 0.02]
        P['wp'] = 0.6
    elif key == 'stun':
        sway = math.sin(t * 0.12) * h * 0.06
        P['sh'] = [sway, shy + h * 0.05]
        P['hip'] = [sway * 0.4, hipy + h * 0.03]
        P['hf'], P['hb'] = [sway + w * 0.12, shy + h * 0.38], [sway - w * 0.12, shy + h * 0.38]
        P['head'] = [sway * 0.5, 3.0]
        P['wp'] = 1.3
    elif key == 'shield':
        P['hip'][1] += h * 0.05
        P['sh'][1] += h * 0.06
        P['hf'], P['hb'] = [w * 0.46, shy + h * 0.1], [w * 0.34, shy + h * 0.2]
        P['ff'], P['fb'] = [w * 0.36, 0.0], [-w * 0.34, 0.0]
        P['wp'] = -1.5
    elif key == 'parry':
        P['hf'], P['hb'] = [w * 0.8, shy + h * 0.02], [-w * 0.3, shy + h * 0.22]
        P['sh'] = [h * 0.06, shy]
        P['wp'] = -1.3
    elif key == 'dodge':
        P['sh'] = [h * 0.2, shy + h * 0.14]
        P['hip'] = [0.0, hipy + h * 0.08]
        P['hf'], P['hb'] = [w * 0.2, shy + h * 0.36], [-w * 0.3, shy + h * 0.32]
        P['ff'], P['fb'] = [w * 0.4, 0.0], [-w * 0.4, -h * 0.04]
        P['wp'] = 2.4
    elif key == 'ledge':
        lip = (w / 2 + 4, -(h - 8))
        sw = math.sin(t * 0.06) * 3
        P['hf'], P['hb'] = [lip[0], lip[1]], [lip[0] - 8, lip[1]]
        P['sh'] = [2.0, shy + 5]
        P['ff'], P['fb'] = [sw - 2, -2.0], [sw - 9, -6.0]
        P['wp'] = 1.6
    elif key == 'grab':
        pm = info.get('pommel', 0)
        P['hf'] = [w * 0.85, shy + h * 0.06]
        P['hb'] = [w * 0.7, shy + h * 0.16] if pm <= 0 else \
            ([-w * 0.2, shy - h * 0.06] if pm > 5 else [w * 0.9, shy + h * 0.02])
        P['sh'] = [h * 0.07, shy]
        P['wp'] = -1.4
    elif key == 'grabbed':
        P['hf'], P['hb'] = [w * 0.12, shy + h * 0.32], [-w * 0.1, shy + h * 0.32]
        P['ff'], P['fb'] = [w * 0.1, -4.0], [-w * 0.12, -2.0]
        P['head'] = [-2.0, 2.0]
        P['sh'][0] = -3.0
        P['wp'] = 1.4
    elif key == 'gun':
        P['hf'], P['hb'] = [w * 0.6, shy + h * 0.2], [w * 0.3, shy + h * 0.26]
        P['sh'] = [-h * 0.06, shy + 2]
        P['ff'], P['fb'] = [w * 0.45, 0.0], [-w * 0.5, 0.0]
    elif key == 'attack':
        tx, ty = info.get('T', (w, shy))
        ph, limb = info.get('phase', 'strike'), info.get('limb', 'hand')
        sx, sy = P['sh']
        n = math.hypot(tx - sx, ty - sy) or 1
        dx, dy = (tx - sx) / n, (ty - sy) / n
        ang = math.atan2(dy, dx)
        if limb == 'hand':
            P['hb'] = [-dx * w * 0.4, shy + h * 0.2]
            if ph == 'wind':                                  # pull back, away from the target
                P['hf'] = [sx - dx * h * 0.24, sy - dy * h * 0.24 - h * 0.08]
                P['sh'][0] -= dx * h * 0.06
                P['wp'] = ang - 1.9
            elif ph == 'strike':                              # lunge and extend through the target
                P['hf'] = [tx, ty]
                P['sh'] = [sx + dx * h * 0.1, sy + max(0.0, dy) * h * 0.08]
                P['hip'][0] += dx * h * 0.04
                P['ff'][0] += w * 0.16 * (1 if dx > 0 else 0)
                P['wp'] = ang + 0.3
            else:
                P['hf'] = [sx + dx * h * 0.2, sy + dy * h * 0.2 + h * 0.08]
                P['wp'] = ang + 1.0
        elif limb == 'foot':
            fk = 'ff' if tx >= 0 else 'fb'
            sgn = 1 if tx >= 0 else -1
            P['hf'], P['hb'] = [w * 0.34, shy + h * 0.06], [-w * 0.42, shy + h * 0.12]
            if ph == 'wind':
                P[fk] = [sgn * w * 0.12, -h * 0.22]           # knee up
            elif ph == 'strike':
                P[fk] = [tx, ty]
                P['sh'][0] -= sgn * h * 0.1                   # lean away for balance
                if ty > -h * 0.1 and abs(tx) < w * 0.5:       # stomp straight down: tuck the other leg
                    P['fb' if fk == 'ff' else 'ff'] = [-sgn * w * 0.2, -h * 0.2]
        elif limb == 'spin':
            P['spin'] = info.get('spin', 0.0)
            P['hf'], P['hb'] = [w * 0.85, shy + h * 0.1], [-w * 0.85, shy + h * 0.1]
            P['ff'], P['fb'] = [w * 0.4, -h * 0.06], [-w * 0.4, -h * 0.1]
            P['wp'] = 0.0
        elif limb == 'burst':
            if ph == 'wind':                                  # curl in, gathering energy
                P['hf'], P['hb'] = [w * 0.15, shy + h * 0.12], [w * 0.0, shy + h * 0.14]
                P['hip'][1] += h * 0.06
                P['sh'][1] += h * 0.08
            else:                                             # fling the arms wide
                P['hf'], P['hb'] = [w * 0.8, shy - h * 0.16], [-w * 0.8, shy - h * 0.16]
                P['sh'][1] -= 3
                P['head'] = [0.0, -2.0]
            P['wp'] = -1.57
        elif limb == 'slam':
            if ph == 'wind':                                  # raise both arms overhead
                P['hf'], P['hb'] = [w * 0.2, shy - h * 0.32], [w * 0.0, shy - h * 0.3]
                P['sh'][1] -= 3
                P['wp'] = -1.6
            else:                                             # drive them into the ground
                P['hf'], P['hb'] = [w * 0.6, -h * 0.06], [w * 0.38, -h * 0.04]
                P['sh'] = [h * 0.13, shy + h * 0.17]
                P['hip'][1] += h * 0.08
                P['wp'] = 1.2
        elif limb == 'cast':
            if ph == 'wind':
                P['hf'] = [-w * 0.25, shy + h * 0.08]
                P['wp'] = -2.2
            elif ph == 'strike':
                P['hf'] = [w * 0.95, shy + h * 0.04]
                P['sh'][0] += h * 0.06
                P['wp'] = 0.0
    # ---- character-specific habits
    if style == 'weaver' and key == 'run':                    # blade trails behind in a low runner's carry
        P['hf'], P['wp'] = [-w * 0.45, shy + h * 0.3], 2.7
    elif style == 'sentinel' and key in ('idle', 'run', 'jump', 'fall'):
        P['hf'] = [w * 0.44, shy + h * 0.2]                   # shield always held out front
    elif style == 'voidcaller':
        if key == 'run':
            P['hf'], P['hb'] = [-w * 0.2, shy + h * 0.26], [-w * 0.55, shy + h * 0.2]
        elif key == 'idle':
            P['hf'] = [w * 0.58, shy + h * 0.2 + br * 2]
    if style == 'voidcaller':
        strike = key == 'attack' and info.get('limb') == 'hand' and info.get('phase') == 'strike'
        P['orb'] = list(info['T']) if strike else [P['hf'][0] + 9, P['hf'][1] - 9]
    return P


def blend_pose(cur, tgt, rate):
    """Eases the current pose toward the target so every transition is a smooth motion, not a snap."""
    out = {}
    for k, v in tgt.items():
        c = cur.get(k, v)
        if k == 'spin':
            out[k] = v
        elif isinstance(v, list):
            out[k] = [c[0] + (v[0] - c[0]) * rate, c[1] + (v[1] - c[1]) * rate]
        else:
            out[k] = c + (v - c) * rate
    return out


class ClothSim:
    """Rope physics (Verlet integration) for scarves, hair, plumes, hems and flames.
    Each strand is a chain of points simulated in world space, so it lags behind when the fighter
    moves, whips round when they turn, and settles when they stop."""

    def __init__(self):
        self.chains, self.ox, self.oy, self.f = {}, 0.0, 0.0, 1

    def begin(self, ox, oy, facing):
        self.ox, self.oy, self.f = ox, oy, facing

    def chain(self, name, anchor, n, seg, gravity=0.5, damp=0.86, wave=0.25, t=0.0, breeze=0.12):
        ax, ay = self.ox + anchor[0] * self.f, self.oy + anchor[1]
        pts = self.chains.get(name)
        if pts is None or len(pts) != n + 1 or abs(pts[0][0] - ax) + abs(pts[0][1] - ay) > 160:
            d = 1 if gravity >= 0 else -1                     # (re)start hanging down, or rising for flames
            pts = [[ax - self.f * i * seg * 0.3, ay + d * i * seg * 0.95] * 2 for i in range(n + 1)]
            self.chains[name] = pts
        pts[0][0], pts[0][1] = ax, ay
        phase = len(name) * 1.7
        for i in range(1, n + 1):
            p = pts[i]
            vx, vy = (p[0] - p[2]) * damp, (p[1] - p[3]) * damp
            p[2], p[3] = p[0], p[1]
            p[0] += vx + math.sin(t * 0.21 + i * 1.1 + phase) * wave - self.f * breeze   # a breeze from the front
            p[1] += vy + gravity
        for _ in range(2):                                    # keep every link at its length
            for i in range(1, n + 1):
                a, b = pts[i - 1], pts[i]
                dx, dy = b[0] - a[0], b[1] - a[1]
                d = math.hypot(dx, dy) or 0.001
                b[0], b[1] = a[0] + dx / d * seg, a[1] + dy / d * seg
        return [((p[0] - self.ox) * self.f, p[1] - self.oy) for p in pts]


SS = 2          # fighters are drawn at double size and scaled down, which smooths every edge
                # (the Game Manager drops this to 1 on computers that cannot hold 60 FPS)


def render_fighter_surface(spec, col, P, facing, t, key='idle', vel=(0.0, 0.0), info=None, cloth=None, origin=(0, 0)):
    """Draws a fighter from a skeleton pose onto a transparent surface (feet at SPR_CX, SPR_FEET).
    cloth: the fighter's ClothSim. Without one (menus) the flowing parts use a simple wave instead."""
    info = info or {}
    s = pygame.Surface((SPR * SS, SPR * SS), pygame.SRCALPHA)
    D = pygame.draw
    st = spec['stats']
    w, h, style, f = st['w'], st['h'], spec['style'], facing
    main, dark, acc, skin = col['main'], col['dark'], col['accent'], col['skin']
    mid, light = lerp_color(main, dark, 0.5), lerp_color(main, WHITE, 0.35)
    ink = (36, 20, 34)
    floaty = style == 'voidcaller' and key not in ('ledge', 'grabbed')
    yo = -(8 + math.sin(t * 0.07) * 3) if floaty else 0
    if cloth:
        cloth.begin(origin[0], origin[1] + yo, f)

    def M(p):                                                # local -> (supersampled) surface pixels
        return ((SPR_CX + p[0] * f) * SS, (SPR_FEET + p[1] + yo) * SS)

    def C(color, p, r, width=0):
        x, y = M(p)
        D.circle(s, color, (int(x), int(y)), max(1, int(r * SS)), int(width * SS))

    def poly(color, pts, width=0):
        D.polygon(s, color, [M(p) for p in pts], int(width * SS))

    def line(color, a, b, width):
        D.line(s, color, M(a), M(b), max(1, int(width * SS)))

    def box(color, center, bw, bh, radius=0, width=0):
        r = pygame.Rect(0, 0, int(bw * SS), int(bh * SS))
        r.center = M(center)
        D.rect(s, color, r, int(width * SS), border_radius=int(radius * SS))

    def oval(color, center, bw, bh, width=0):
        r = pygame.Rect(0, 0, int(bw * SS), int(bh * SS))
        r.center = M(center)
        D.ellipse(s, color, r, int(width * SS))

    def halo(color, p, radius, alpha):
        glow(s, color, M(p), radius * SS, min(190, alpha))    # stays under the outline threshold

    hr = max(13, w * 0.33)
    hip, sh = P['hip'], P['sh']
    leg, arm = h * 0.38 / 2 * 1.06, h * 0.2
    lw, aw = max(7, int(w * 0.22)), max(6, int(w * 0.18))
    sf, sb = (sh[0] + 3, sh[1] + 3), (sh[0] - 4, sh[1] + 3)
    hpf, hpb = (hip[0] + 2, hip[1]), (hip[0] - 3, hip[1])
    kf, ff = ik(hpf, P['ff'], leg, leg, -1)
    kb, fb = ik(hpb, P['fb'], leg, leg, -1)
    ef, hf = ik(sf, P['hf'], arm, arm, 1)
    eb, hb_ = ik(sb, P['hb'], arm, arm, 1)
    head = (sh[0] + (sh[0] - hip[0]) * 0.25 + P['head'][0], sh[1] - hr * 0.92 + P['head'][1])
    blink = int(t) % 190 < 7
    pained = key in ('hurt', 'stun', 'grabbed')
    striking = key == 'attack' and info.get('phase') == 'strike'
    tvx, tvy = -vel[0] * 1.3 - 5, -vel[1] * 0.9 + 2          # fallback flow direction when there is no ClothSim
    tn = math.hypot(tvx, tvy) or 1
    tdx, tdy = tvx / tn, tvy / tn
    flow = min(1.6, 0.6 + tn / 14)

    def chain(name, anchor, n, seg, gravity=0.5, damp=0.86, wave=0.25, breeze=0.12):
        if cloth:
            return cloth.chain(name, anchor, n, seg, gravity, damp, wave, t, breeze)
        pts, ph = [anchor], len(name) * 1.7
        for i in range(1, n + 1):
            wv = math.sin(t * 0.22 + i * 0.9 + ph) * (2 + wave * 6) * i / n
            if gravity < 0:                                  # flames lick upward
                pts.append((anchor[0] + tdx * seg * i * 0.35 + wv, anchor[1] - seg * i * 0.95))
            else:
                pts.append((anchor[0] + tdx * seg * i * flow - tdy * wv,
                            anchor[1] + tdy * seg * i * flow + tdx * wv + i * i * 0.3))
        return pts

    def ribbon(pts, color, w0, w1=1.5, profile=None):
        """A smooth tapering strip along a chain of points."""
        n = len(pts)
        left, right = [], []
        for i, p in enumerate(pts):
            a, b = pts[max(0, i - 1)], pts[min(n - 1, i + 1)]
            dx, dy = b[0] - a[0], b[1] - a[1]
            d = math.hypot(dx, dy) or 1
            u = i / (n - 1)
            wd = (profile(u) if profile else w0 + (w1 - w0) * u) / 2
            left.append((p[0] - dy / d * wd, p[1] + dx / d * wd))
            right.append((p[0] + dy / d * wd, p[1] - dx / d * wd))
        poly(color, left + right[::-1])
        C(color, pts[0], (profile(0) if profile else w0) / 2)

    def flame_strand(pts, width, colors, seed=0.0):
        """A tongue of fire along a chain: fat near the base, flickering, tapering to a point."""
        for k, color in zip((1.0, 0.64, 0.32), colors):
            cut = pts[:max(3, int(len(pts) * (0.55 + 0.45 * k)))]
            ribbon(cut, color, 0, profile=lambda u, k=k: 2.2 * k + width * k * (0.5 + 0.5 * math.sin(math.pi * min(1.0, u * 2.6 + 0.2)))
                   * (1 - u) ** 0.6 * (0.82 + 0.18 * math.sin(t * 0.7 + u * 8 + seed)))

    def limb(color, a, j, b, width, color2=None, width2=None):
        c2, w2 = color2 or color, width2 or width
        line(color, a, j, width)
        line(c2, j, b, w2)
        C(color, a, width / 2)
        C(color, j, max(width, w2) / 2)
        C(c2, b, w2 / 2)

    tw, hw = w * 0.56, w * 0.44

    def torso(color, tw_, hw_, drop=4):
        poly(color, [(sh[0] - tw_ / 2, sh[1]), (sh[0] + tw_ / 2, sh[1]),
                     (hip[0] + hw_ / 2, hip[1] + drop), (hip[0] - hw_ / 2, hip[1] + drop)])
        C(color, (sh[0] - tw_ / 2 + 3, sh[1] + 2), 3.5)       # rounded shoulders
        C(color, (sh[0] + tw_ / 2 - 3, sh[1] + 2), 3.5)

    def human_eye(p, r):
        if pained:                                           # squeezed shut: ">"
            line(ink, (p[0] - r, p[1] - r), (p[0] + r * 0.6, p[1]), 1.5)
            line(ink, (p[0] - r, p[1] + r), (p[0] + r * 0.6, p[1]), 1.5)
        elif blink:
            line(ink, (p[0] - r, p[1]), (p[0] + r, p[1]), 1.5)
        else:
            oval(WHITE, p, r * 2.3, r * 2.1)
            C(ink, (p[0] + r * 0.35, p[1] + 0.3), r * 0.72)
            C(WHITE, (p[0] + r * 0.6, p[1] - r * 0.35), r * 0.25)

    if style == 'weaver':
        # ---------------- KAI: slim duelist, thread-blade, twin scarf tails
        neck = (sh[0] - 3, sh[1] + 1)
        hairc, bandc = col.get('hair', dark), col.get('band', acc)
        wrapc, bootc = col.get('wrap', skin), col.get('boot', mid)
        limb(mid, sb, eb, hb_, aw, lerp_color(wrapc, dark, 0.25))
        limb(dark, hpb, kb, fb, lw + 3, dark, lw)                                            # baggy trousers
        ribbon(chain('scarf2', (neck[0], neck[1] + 3), 8, 8, 0.2, 0.9, 0.6, 0.5), lerp_color(acc, dark, 0.3), 10, 3)
        ribbon(chain('scarf1', neck, 9, 8, 0.16, 0.9, 0.6, 0.55), acc, 12, 3)
        torso(main, tw, hw)
        poly(mid, [(sh[0] - tw / 2, sh[1]), (sh[0] - tw * 0.1, sh[1]), (hip[0] - hw * 0.2, hip[1] + 4),
                   (hip[0] - hw / 2, hip[1] + 4)])                                           # shaded back half
        line(acc, (sh[0] + tw * 0.2, sh[1] + 1), (hip[0] + hw * 0.15, hip[1] + 3), 2)        # tunic trim
        for i in range(3):                                                                    # toggles
            ty_ = sh[1] + 6 + i * 6
            line(light, (sh[0] + tw * 0.12, ty_), (sh[0] + tw * 0.3, ty_), 1.5)
        line(dark, (hip[0] - hw / 2, hip[1]), (hip[0] + hw / 2, hip[1]), 6)                  # sash
        C(acc, (hip[0] + hw * 0.3, hip[1]), 3.5)
        ribbon(chain('sash', (hip[0] - hw * 0.3, hip[1] + 2), 4, 6, 0.5, 0.86, 0.3), dark, 5, 2)
        limb(lerp_color(dark, main, 0.15), hpf, kf, ff, lw + 3, lerp_color(dark, main, 0.15), lw)
        poly(main, [(hip[0] - hw / 2 - 1, hip[1] + 2), (hip[0] + hw / 2 + 1, hip[1] + 2), (hip[0] + hw / 2 + 3, hip[1] + 14),
                    (hip[0] + hw * 0.25, hip[1] + 8), (hip[0] + 1, hip[1] + 16), (hip[0] - hw * 0.25, hip[1] + 9),
                    (hip[0] - hw / 2 - 3, hip[1] + 15)])                                     # tattered tunic hem
        for foot in (fb, ff):                                                                # wrapped boots
            oval(bootc, (foot[0] + 3, foot[1] - 4), lw + 8, lw + 2)
            for k in (-9, -12.5):
                line(wrapc, (foot[0] - 4, foot[1] + k), (foot[0] + 4, foot[1] + k), 2)
        oval(acc, (sh[0], sh[1] + 1), tw + 8, 11)                                            # scarf wound round the neck
        oval(lerp_color(acc, dark, 0.3), (sh[0] - 3, sh[1] + 3), tw - 2, 4)
        ribbon(chain('band1', (head[0] - hr * 0.9, head[1] - hr * 0.35), 6, 6, 0.2, 0.9, 0.5, 0.45), bandc, 5, 2)
        ribbon(chain('band2', (head[0] - hr * 0.9, head[1] - hr * 0.25), 5, 6, 0.26, 0.9, 0.5, 0.4),
               lerp_color(bandc, dark, 0.35), 4.5, 2)
        for i in range(8):                                                                   # big spiky hair
            a = math.radians(-18 - i * 27)
            ln = hr * (1.75 + 0.14 * math.sin(t * 0.09 + i * 1.3) + (0.2 if i % 2 else 0))
            poly(hairc if i % 2 else lerp_color(hairc, dark, 0.25),
                 [(head[0] + math.cos(a - 0.34) * hr * 0.8, head[1] + math.sin(a - 0.34) * hr * 0.8),
                  (head[0] + math.cos(a) * ln - 2, head[1] + math.sin(a) * ln * 0.95),
                  (head[0] + math.cos(a + 0.34) * hr * 0.8, head[1] + math.sin(a + 0.34) * hr * 0.8)])
        C(hairc, (head[0] - hr * 0.12, head[1] - hr * 0.22), hr * 0.98)
        C(skin, (head[0] + hr * 0.14, head[1] + hr * 0.16), hr * 0.86)
        for i, fx_ in enumerate((0.95, 0.5, 0.05)):                                          # fringe over the brow
            poly(hairc, [(head[0] + hr * (fx_ - 0.3), head[1] - hr * 0.62), (head[0] + hr * (fx_ + 0.2), head[1] - hr * 0.66),
                         (head[0] + hr * (fx_ + 0.12), head[1] - hr * (0.12 + 0.1 * i))])
        line(bandc, (head[0] - hr * 0.95, head[1] - hr * 0.36), (head[0] + hr * 0.98, head[1] - hr * 0.46), 4)
        line(lerp_color(bandc, WHITE, 0.3), (head[0] - hr * 0.6, head[1] - hr * 0.44), (head[0] + hr * 0.9, head[1] - hr * 0.52), 1)
        human_eye((head[0] + hr * 0.48, head[1] + hr * 0.08), hr * 0.2)
        if not pained:
            line(ink, (head[0] + hr * 0.2, head[1] - hr * 0.22), (head[0] + hr * 0.8, head[1] - hr * 0.1), 1.5)
        if striking or pained:
            C(ink, (head[0] + hr * 0.5, head[1] + hr * 0.55), 1.8)                           # open mouth
        else:
            line(ink, (head[0] + hr * 0.35, head[1] + hr * 0.55), (head[0] + hr * 0.65, head[1] + hr * 0.5), 1)
        limb(main, sf, ef, hf, aw, wrapc)
        for k in (0.3, 0.5, 0.7):                                                            # arm wraps
            line(lerp_color(wrapc, dark, 0.35), (ef[0] + (hf[0] - ef[0]) * k - 3, ef[1] + (hf[1] - ef[1]) * k),
                 (ef[0] + (hf[0] - ef[0]) * k + 3, ef[1] + (hf[1] - ef[1]) * k), 1.2)
        bx, by = math.cos(P['wp']), math.sin(P['wp'])
        tip = (hf[0] + bx * 50, hf[1] + by * 50)
        ribbon(chain('thread', (hf[0] - bx * 8, hf[1] - by * 8), 6, 6, 0.25, 0.9, 0.5), acc, 1.5, 1)
        nx_, ny_ = -by, bx                                                                   # a short, broad steel blade
        poly((96, 102, 122), [(hf[0] + nx_ * 4.6, hf[1] + ny_ * 4.6), (hf[0] - nx_ * 4.6, hf[1] - ny_ * 4.6),
                              (hf[0] + bx * 40 - nx_ * 3.4, hf[1] + by * 40 - ny_ * 3.4), tip,
                              (hf[0] + bx * 40 + nx_ * 3.4, hf[1] + by * 40 + ny_ * 3.4)])
        poly((186, 194, 210), [(hf[0] + nx_ * 3.2, hf[1] + ny_ * 3.2), (hf[0] - nx_ * 3.2, hf[1] - ny_ * 3.2),
                               (hf[0] + bx * 40 - nx_ * 2.2, hf[1] + by * 40 - ny_ * 2.2), (hf[0] + bx * 48, hf[1] + by * 48),
                               (hf[0] + bx * 40 + nx_ * 2.2, hf[1] + by * 40 + ny_ * 2.2)])
        poly(WHITE, [(hf[0] + nx_ * 3.2, hf[1] + ny_ * 3.2), (hf[0] + nx_ * 0.6, hf[1] + ny_ * 0.6),
                     (hf[0] + bx * 46, hf[1] + by * 46), (hf[0] + bx * 40 + nx_ * 2.2, hf[1] + by * 40 + ny_ * 2.2)])
        g_ = 18 + 14 * math.sin(t * 0.09)
        C(WHITE, (hf[0] + bx * g_, hf[1] + by * g_), 1.5)                                    # glint
        line((70, 74, 92), (hf[0] + bx * 3 - by * 9, hf[1] + by * 3 + bx * 9), (hf[0] + bx * 3 + by * 9, hf[1] + by * 3 - bx * 9), 3.5)
        line((120, 86, 60), (hf[0] - bx * 7, hf[1] - by * 7), (hf[0] + bx * 2, hf[1] + by * 2), 3.5)
        C((150, 156, 172), (hf[0] - bx * 8, hf[1] - by * 8), 2.6)                            # pommel ring
        C((70, 74, 92), (hf[0] - bx * 8, hf[1] - by * 8), 2.6, 1)
        C(wrapc, hf, 4.5)

    elif style == 'sentinel':
        # ---------------- BRONN: plated guardian, kite shield + stone maul
        weapon = info.get('weapon', 'shield')
        power = 60 if striking else 0                        # runes flare when he strikes
        steel, stone, beltc = col.get('steel', lerp_color(skin, WHITE, 0.25)), col.get('stone', skin), col.get('belt', dark)

        def maul(hand, a):
            bx, by = math.cos(a), math.sin(a)
            end = (hand[0] + bx * 50, hand[1] + by * 50)
            line((84, 58, 42), (hand[0] - bx * 13, hand[1] - by * 13), end, 6)
            line((134, 98, 66), (hand[0] - bx * 13, hand[1] - by * 13), end, 2.5)
            C((70, 74, 90), (hand[0] - bx * 13, hand[1] - by * 13), 3.5)                      # pommel cap
            blk = [(end[0] + bx * 15 - by * 22, end[1] + by * 15 + bx * 22), (end[0] + bx * 15 + by * 22, end[1] + by * 15 - bx * 22),
                   (end[0] - bx * 15 + by * 22, end[1] - by * 15 - bx * 22), (end[0] - bx * 15 - by * 22, end[1] - by * 15 + bx * 22)]
            poly(stone, blk)                                                                  # a great block of stone
            poly(lerp_color(stone, WHITE, 0.3), [blk[0], blk[1], (end[0] + bx * 7 + by * 22, end[1] + by * 7 - bx * 22),
                                                 (end[0] + bx * 7 - by * 22, end[1] + by * 7 + bx * 22)])
            poly(lerp_color(stone, (0, 0, 0), 0.3), [blk[2], blk[3], (end[0] - bx * 9 - by * 22, end[1] - by * 9 + bx * 22),
                                                     (end[0] - bx * 9 + by * 22, end[1] - by * 9 - bx * 22)])
            poly((52, 56, 70), blk, 2)
            poly((70, 74, 90), [(end[0] + bx * 8, end[1] + by * 8), (end[0] - by * 7, end[1] + bx * 7),
                                (end[0] - bx * 8, end[1] - by * 8), (end[0] + by * 7, end[1] - bx * 7)])    # emblem
            line((52, 56, 70), (end[0] + bx * 4 - by * 20, end[1] + by * 4 + bx * 20),
                 (end[0] - bx * 6 - by * 13, end[1] - by * 6 + bx * 13), 1.2)                 # crack
            if power:
                halo(acc, end, 16, 150)

        def shield(hand, fill):
            cx_, cy_ = hand[0] + 6, hand[1] + 2
            pts = [(cx_ - 14, cy_ - h * 0.28), (cx_ + 14, cy_ - h * 0.28), (cx_ + 16, cy_ + h * 0.05),
                   (cx_, cy_ + h * 0.32), (cx_ - 16, cy_ + h * 0.05)]
            poly(steel, pts)                                                                  # steel rim
            inner = [(cx_ - 10, cy_ - h * 0.24), (cx_ + 10, cy_ - h * 0.24), (cx_ + 12, cy_ + h * 0.04),
                     (cx_, cy_ + h * 0.26), (cx_ - 12, cy_ + h * 0.04)]
            poly(fill, inner)
            poly(lerp_color(fill, WHITE, 0.2), [inner[0], (cx_, inner[0][1]), (cx_, inner[3][1]), inner[4]])
            poly((60, 66, 84), pts, 1.5)
            halo(acc, (cx_, cy_ - 1), 9 + power * 0.08, 130 + power)
            gem = [(cx_, cy_ - 11), (cx_ + 6, cy_ - 1), (cx_, cy_ + 9), (cx_ - 6, cy_ - 1)]
            poly(acc, gem)                                                                    # glowing gem
            poly(WHITE, [gem[0], (cx_ + 2, cy_ - 2), (cx_ - 2, cy_ - 2)])
            poly((40, 110, 130), gem, 1)
            for px_, py_ in ((-10, -h * 0.25), (10, -h * 0.25), (0, h * 0.27), (-13, 0), (13, 0)):   # rivets
                C((70, 74, 92), (cx_ + px_, cy_ + py_), 1.5)

        limb(dark, sb, eb, hb_, aw + 2)
        if weapon == 'maul':
            shield(hb_, mid)
        else:
            maul(hb_, -2.0)
        limb(dark, hpb, kb, fb, lw + 2)
        C(dark, (sh[0] - w * 0.3, sh[1] + 3), w * 0.2)                                       # rear pauldron
        tw2, hw2 = w * 0.8, w * 0.52
        torso(dark, tw2, hw2)
        torso(main, tw2 - 5, hw2 - 5, 2)
        poly(light, [(sh[0] - tw2 / 2 + 5, sh[1] + 2), (sh[0] + tw2 / 2 - 5, sh[1] + 2),
                     (sh[0] + tw2 / 2 - 9, sh[1] + 7), (sh[0] - tw2 / 2 + 9, sh[1] + 7)])    # chest highlight
        for k in (0.36, 0.68):                                                               # segmented stone plates
            yy = sh[1] + (hip[1] - sh[1]) * k
            xw = (tw2 + (hw2 - tw2) * k) / 2 - 3
            line(dark, (sh[0] + (hip[0] - sh[0]) * k - xw, yy), (sh[0] + (hip[0] - sh[0]) * k + xw, yy), 1.5)
        line(dark, (sh[0] + 1, sh[1] + 3), (hip[0] + 1, hip[1] - 2), 1.5)
        line(dark, (sh[0] - 8, sh[1] + 9), (sh[0] - 5, sh[1] + 13), 1)                       # cracks
        line(dark, (sh[0] - 5, sh[1] + 13), (sh[0] - 9, sh[1] + 16), 1)
        line(beltc, (hip[0] - hw2 / 2, hip[1] + 1), (hip[0] + hw2 / 2, hip[1] + 1), 7)       # leather belt
        box(steel, (hip[0] + 2, hip[1] + 1), 8, 7, 1)
        poly(mid, [(hip[0] + 2, hip[1] + 4), (hip[0] + hw2 / 2 + 2, hip[1] + 4),
                   (hip[0] + hw2 / 2 - 2, hip[1] + 16), (hip[0] + 4, hip[1] + 18)])          # tasset
        limb(mid, hpf, kf, ff, lw + 2, lerp_color(dark, main, 0.25))
        C(main, kf, lw / 2 + 3)
        C(dark, kf, lw / 2 + 3, 2)
        C(light, (kf[0] - 1, kf[1] - 1.5), 2)
        for foot in (fb, ff):
            box(dark, (foot[0] + 4, foot[1] - 6), lw + 14, 13, 5)
            line(mid, (foot[0] - 5, foot[1] - 2), (foot[0] + 13, foot[1] - 2), 1.5)
        box(dark, (head[0] - hr * 0.5, head[1] + hr * 0.95), hr * 1.6, 7, 3)                 # gorget
        box(skin, head, hr * 2 + 6, hr * 2 + 5, 7)                                           # bucket great-helm
        box(lerp_color(skin, WHITE, 0.28), (head[0] + 1, head[1] - hr * 0.72), hr * 2 - 2, hr * 0.5, 5)
        poly(mid, [(head[0] - hr - 3, head[1] - hr * 0.4), (head[0] - hr * 0.35, head[1] - hr * 0.4),
                   (head[0] - hr * 0.35, head[1] + hr + 2), (head[0] - hr - 3, head[1] + hr + 2)])   # shaded back plate
        box(dark, head, hr * 2 + 6, hr * 2 + 5, 7, 2.5)
        line(dark, (head[0] - hr - 2, head[1] - hr * 0.42), (head[0] + hr + 2, head[1] - hr * 0.42), 1.5)
        for rx in (-0.7, 0.0, 0.7):                                                          # rivets
            C(dark, (head[0] + hr * rx, head[1] - hr * 0.72), 1.4)
        vis = 0.75 + 0.25 * math.sin(t * 0.13) if not pained else 0.35 + 0.3 * (int(t) % 6 < 3)
        vcol = lerp_color(dark, acc, vis)
        halo(acc, (head[0] + hr * 0.55, head[1]), 12 + power * 0.1, int(130 * vis) + power)
        line((16, 22, 40), (head[0] - hr * 0.1, head[1] - 1), (head[0] + hr + 2, head[1] - 1), 7.5)
        line((16, 22, 40), (head[0] + hr * 0.5, head[1] - 1), (head[0] + hr * 0.5, head[1] + hr * 0.85), 6)
        line(vcol, (head[0], head[1] - 1), (head[0] + hr + 1, head[1] - 1), 1.5 if blink else 5)     # glowing T-visor
        line(vcol, (head[0] + hr * 0.5, head[1] - 1), (head[0] + hr * 0.5, head[1] + hr * 0.75), 3.6)
        for k in (0.3, 0.55):                                                                # breath slits
            line(dark, (head[0] - hr * 0.6, head[1] + hr * k), (head[0] - hr * 0.1, head[1] + hr * k), 1.5)
        C(main, (sh[0] + w * 0.16, sh[1] + 4), w * 0.25)                                     # front pauldron
        C(light, (sh[0] + w * 0.13, sh[1] + 1), w * 0.13)
        C(dark, (sh[0] + w * 0.16, sh[1] + 4), w * 0.25, 2.5)
        line(dark, (sh[0] + w * 0.16 - w * 0.2, sh[1] + 7), (sh[0] + w * 0.16 + w * 0.2, sh[1] + 7), 1.2)
        limb(main, sf, ef, hf, aw + 2, mid)
        C(dark, ef, aw / 2 + 1, 1.2)
        C(skin, hf, aw / 2 + 3)
        C(dark, hf, aw / 2 + 3, 1.5)
        if weapon == 'maul':
            maul(hf, P['wp'])
        else:
            shield(hf, main)

    elif style == 'voidcaller':
        # ---------------- NYX: hovering star-mage, layered robe, bent hat, star orb
        stars = [(math.cos(t * 0.06 + i * math.tau / 3), math.sin(t * 0.06 + i * math.tau / 3), i) for i in range(3)]
        hatc, hatband = col.get('hat', main), col.get('hatband', acc)

        def swirl(j):                                                                         # ribbons of magic coil round her
            a0 = t * 0.055 + j * math.pi
            pts = [(math.cos(a0 + q * 0.2) * w * (1.0 + q * 0.03), sh[1] + h * (0.16 + 0.36 * j) + math.sin(a0 + q * 0.2) * h * 0.12 - q * 1.4)
                   for q in range(12)]
            ribbon(pts, lerp_color(acc, dark, 0.25), 0, profile=lambda u: 3.4 * math.sin(math.pi * u) + 0.5)
            ribbon(pts[2:10], lerp_color(acc, WHITE, 0.4), 0, profile=lambda u: 1.2 * math.sin(math.pi * u) + 0.3)

        swirl(0)

        def orbit_star(cx_, sy_, i, size):
            for back in (2, 1):                                                               # fading trail
                a = t * 0.06 + i * math.tau / 3 - back * 0.16
                poly(lerp_color(acc, dark, 0.35 * back), star_points(math.cos(a) * w * 0.95,
                     sh[1] + h * 0.25 + math.sin(a) * 10, size - back * 1.5, t * 0.05))
            poly(acc, star_points(cx_ * w * 0.95, sh[1] + h * 0.25 + sy_ * 10, size, t * 0.05))
            C(WHITE, (cx_ * w * 0.95, sh[1] + h * 0.25 + sy_ * 10), 1.3)

        for cx_, sy_, i in stars:
            if sy_ < 0:
                orbit_star(cx_, sy_, i, 5)

        def sleeve(elbow, hand, c1, c2):
            n = math.hypot(hand[0] - elbow[0], hand[1] - elbow[1]) or 1
            px, py = -(hand[1] - elbow[1]) / n, (hand[0] - elbow[0]) / n
            sag = 3 + math.sin(t * 0.1) * 1.5
            pts = [elbow, (hand[0] + px * 10, hand[1] + py * 10 + sag), (hand[0] + px * 4 - 3, hand[1] + py * 4 + sag + 4),
                   (hand[0], hand[1] + 1), (hand[0] - px * 5 - 3, hand[1] - py * 5 + 4), (hand[0] - px * 10, hand[1] - py * 10)]
            poly(c1, pts)                                                                     # tattered cuff

        limb(dark, sb, eb, hb_, aw)
        sleeve(eb, hb_, mid, dark)
        hang = chain('robe', (hip[0], hip[1]), 3, -hip[1] / 3, 0.9, 0.82, 0.3)                # the robe swings like a bell
        hx_, mx_ = hang[-1][0], hang[1][0]
        rag = (0, -10, 3, -13, -1, -9, 4, -12, -2, -8, 1)                                    # a ragged, torn hem
        hem = [(hx_ + (i / 10 - 0.5) * w * 1.5, hang[-1][1] + rag[i] + math.sin(t * 0.15 + i) * 2) for i in range(11)]
        ribbon(chain('wisp', (hx_ - w * 0.3, hang[-1][1] - 8), 5, 7, 0.25, 0.9, 0.6, 0.45), dark, 13, 1)   # trailing wisp
        poly(dark, [(sh[0] - tw / 2, sh[1]), (sh[0] + tw / 2, sh[1]), (mx_ + w * 0.5, hip[1] + 6)] + hem[::-1]
             + [(mx_ - w * 0.5, hip[1] + 6)])
        inner_y = hang[-1][1] - 16
        poly(main, [(sh[0] - tw / 2 + 3, sh[1]), (sh[0] + tw / 2 - 3, sh[1]), (mx_ + w * 0.42, hip[1] + 4),
                    (hx_ + w * 0.46, inner_y), (hx_ - w * 0.46, inner_y), (mx_ - w * 0.42, hip[1] + 4)])
        poly(light, [(sh[0] + tw * 0.1, sh[1] + 2), (sh[0] + tw / 2 - 3, sh[1] + 2), (mx_ + w * 0.42, hip[1] + 4),
                     (hx_ + w * 0.46, inner_y), (hx_ + w * 0.3, inner_y)])                    # lit front fold
        line(acc, (hx_ - w * 0.46, inner_y), (hx_ + w * 0.46, inner_y), 2.5)
        line(acc, (hip[0] - hw / 2, hip[1] - 4), (hip[0] + hw / 2, hip[1] - 2), 2.5)         # sash
        ribbon(chain('nsash', (hip[0] - hw * 0.35, hip[1] - 2), 3, 7, 0.5, 0.86, 0.3), acc, 3.5, 1.5)
        for k, (ax, ay) in enumerate(((0.2, 0.3), (-0.25, 0.55), (0.3, 0.75), (-0.1, 0.42))):
            tw_ = 0.6 + 0.4 * math.sin(t * 0.12 + k * 2)                                      # twinkling robe stars
            poly(lerp_color(main, WHITE, tw_), star_points(sh[0] + ax * w * 0.6 + (hx_ - sh[0]) * ay,
                 sh[1] + (inner_y - sh[1]) * ay, 2 + tw_ * 1.5, t * 0.03))
        poly(dark, [(head[0] - hr * 1.2, head[1] + hr * 0.3), (head[0] - hr * 0.95, head[1] - hr * 0.6),
                    (head[0] + hr * 0.7, head[1] - hr * 0.7), (head[0] + hr * 1.1, head[1] + hr * 0.5),
                    (sh[0] + tw / 2 + 3, sh[1] + 5), (sh[0] + 2, sh[1] + 9), (sh[0] - tw / 2 - 4, sh[1] + 6)])    # hood and mantle
        C(skin, (head[0] + hr * 0.1, head[1] + hr * 0.1), hr * 0.92)                         # the void where a face should be
        eye_y = head[1] + hr * 0.38
        for ex in (0.2, 0.62):
            halo(acc, (head[0] + hr * ex, eye_y), 7, 170)
            if blink or pained:
                line(acc, (head[0] + hr * ex - 3, eye_y), (head[0] + hr * ex + 3, eye_y), 1.5)
            else:
                ex_ = head[0] + hr * ex
                poly(acc, [(ex_ - 3.4, eye_y - 0.5), (ex_ + 3.6, eye_y - 2.8), (ex_ + 2.8, eye_y + 3.2)])   # slanted glare
                C(WHITE, (ex_ + 1.3, eye_y), 1)
        oval(hatc, (head[0], head[1] - hr * 0.45), hr * 3.7, hr * 1.05)
        oval(lerp_color(hatc, WHITE, 0.2), (head[0] + hr * 0.5, head[1] - hr * 0.5), hr * 2.2, hr * 0.5)
        oval(dark, (head[0], head[1] - hr * 0.45), hr * 3.7, hr * 1.05, 1.5)
        cone = chain('hat', (head[0], head[1] - hr * 1.2), 3, hr * 0.62, -0.5, 0.84, 0.25)    # the bent tip sways
        base_l, base_r = (head[0] - hr * 0.95, head[1] - hr * 0.5), (head[0] + hr * 0.95, head[1] - hr * 0.5)
        poly(hatc, [base_l, base_r, (cone[1][0] + 5, cone[1][1]), (cone[2][0] + 2.5, cone[2][1]), cone[3],
                    (cone[2][0] - 2.5, cone[2][1]), (cone[1][0] - 5, cone[1][1])])
        poly(lerp_color(hatc, WHITE, 0.22), [base_r, (head[0] + hr * 0.3, head[1] - hr * 0.5), (cone[1][0] + 1, cone[1][1]), (cone[1][0] + 5, cone[1][1])])
        line(hatband, (head[0] - hr * 0.95, head[1] - hr * 0.6), (head[0] + hr * 0.95, head[1] - hr * 0.6), 4)
        charm = chain('charm', cone[3], 2, 5, 0.6, 0.9, 0.2)
        line(acc, charm[0], charm[2], 1)
        poly(acc, star_points(charm[2][0], charm[2][1] + 2, 4.5, t * 0.1))
        limb(dark, sf, ef, hf, aw)
        sleeve(ef, hf, main, acc)
        C(skin, hf, 4)
        orb = P.get('orb', hf)
        if math.hypot(orb[0] - hf[0], orb[1] - hf[1]) > 16:
            line(acc, hf, orb, 1)
        pulse = 1 + 0.15 * math.sin(t * 0.2)
        halo(acc, orb, 19 * pulse, 170)
        C((26, 14, 46), orb, 10.5 * pulse)                                                   # a dark sphere holding a star
        C((70, 40, 110), (orb[0] - 2.5, orb[1] - 3), 4 * pulse)
        C(acc, orb, 10.5 * pulse, 1.6)
        poly(acc, star_points(orb[0], orb[1], 7 * pulse, t * 0.1))
        C(WHITE, orb, 1.6)
        for i in range(3):                                                                    # sparks circling the orb
            a = t * 0.17 + i * math.tau / 3
            C(WHITE, (orb[0] + math.cos(a) * 14, orb[1] + math.sin(a) * 14), 1.2)
        swirl(1)
        for cx_, sy_, i in stars:
            if sy_ >= 0:
                orbit_star(cx_, sy_, i, 6)

    else:
        # ---------------- RHEA: brawler, living flame hair, heavy gauntlets, baggy trousers
        hot = info.get('hot', False)
        fire = (col.get('flame', (240, 92, 24)), acc, (255, 250, 228))
        shade = lerp_color(skin, dark, 0.35)
        front_leg = lerp_color(dark, main, 0.45)
        bandc, wrapc = col.get('band', dark), col.get('wrap', acc)

        def leg2(a, j, b, color):
            limb(color, a, j, b, lw + 4, color, lw)
            for k in (0.72, 0.86):                                                            # ankle wraps
                q = (j[0] + (b[0] - j[0]) * k, j[1] + (b[1] - j[1]) * k)
                line(wrapc, (q[0] - lw / 2, q[1]), (q[0] + lw / 2, q[1]), 2.2)
            poly(color, [(j[0] + (b[0] - j[0]) * 0.55 - lw / 2 - 1, j[1] + (b[1] - j[1]) * 0.55), (j[0] + (b[0] - j[0]) * 0.55 + lw / 2 + 1, j[1] + (b[1] - j[1]) * 0.55),
                         (j[0] + (b[0] - j[0]) * 0.7 + 2, j[1] + (b[1] - j[1]) * 0.7), (j[0] + (b[0] - j[0]) * 0.62, j[1] + (b[1] - j[1]) * 0.62),
                         (j[0] + (b[0] - j[0]) * 0.72 - 3, j[1] + (b[1] - j[1]) * 0.72)])      # torn trouser cuff
            oval(skin, (b[0] + 3, b[1] - 3), 12, 8)
            line(wrapc, (b[0] - 1, b[1] - 5), (b[0] + 6, b[1] - 4), 2)

        def gauntlet(elbow, hand, color, name):
            n = math.hypot(hand[0] - elbow[0], hand[1] - elbow[1]) or 1
            ux, uy = (hand[0] - elbow[0]) / n, (hand[1] - elbow[1]) / n
            if hot:
                halo(acc, hand, 22, 170)
            flame_strand(chain(name, (hand[0] - ux * 2, hand[1] - uy * 2), 5, 6.5 if hot else 4.5, -0.55, 0.78, 0.9),
                         15 if hot else 9.5, fire, 3.0)                                       # her fists always burn
            C(color, hand, aw / 2 + 3.5)                                                      # bare fist, bound in cloth
            for k in (-3.5, -0.5, 2.5):
                line(wrapc, (hand[0] + ux * k - uy * 5, hand[1] + uy * k + ux * 5),
                     (hand[0] + ux * k + uy * 5, hand[1] + uy * k - ux * 5), 1.8)
            for k in (0.45, 0.7):                                                             # forearm wraps
                q = (elbow[0] + (hand[0] - elbow[0]) * k, elbow[1] + (hand[1] - elbow[1]) * k)
                line(wrapc, (q[0] - uy * 4, q[1] + ux * 4), (q[0] + uy * 4, q[1] - ux * 4), 1.8)

        limb(shade, sb, eb, hb_, aw)
        gauntlet(eb, hb_, shade, 'fistb')
        leg2(hpb, kb, fb, dark)
        ribbon(chain('band1', (head[0] - hr * 0.95, head[1] - hr * 0.3), 6, 6, 0.2, 0.9, 0.5, 0.4), bandc, 5.5, 2)
        ribbon(chain('band2', (head[0] - hr * 0.95, head[1] - hr * 0.2), 5, 6, 0.26, 0.9, 0.5, 0.4), lerp_color(bandc, (0, 0, 0), 0.3), 5, 2)
        torso(skin, tw, hw)
        line(shade, (hip[0] + 1, hip[1] - 9), (hip[0] + 1, hip[1] - 2), 1.5)                 # abs
        line(shade, (hip[0] - 4, hip[1] - 6), (hip[0] + 6, hip[1] - 6), 1.5)
        k = 0.55
        poly(main, [(sh[0] - tw / 2, sh[1]), (sh[0] + tw / 2, sh[1]),
                    (sh[0] + tw / 2 + (hip[0] + hw / 2 - sh[0] - tw / 2) * k, sh[1] + (hip[1] - sh[1]) * k),
                    (sh[0] - tw / 2 + (hip[0] - hw / 2 - sh[0] + tw / 2) * k, sh[1] + (hip[1] - sh[1]) * k)])
        poly(mid, [(sh[0] - tw / 2, sh[1]), (sh[0] - tw * 0.12, sh[1]),
                   (sh[0] - tw * 0.16 + (hip[0] - sh[0]) * k, sh[1] + (hip[1] - sh[1]) * k),
                   (sh[0] - tw / 2 + (hip[0] - hw / 2 - sh[0] + tw / 2) * k, sh[1] + (hip[1] - sh[1]) * k)])
        yb = sh[1] + (hip[1] - sh[1]) * k                                                    # torn hem of her top
        for i in range(4):
            xb = sh[0] - tw * 0.42 + i * tw * 0.26
            poly(main if i else mid, [(xb - 1, yb - 1), (xb + tw * 0.24, yb - 1), (xb + tw * 0.1, yb + 4 + (i % 2) * 2)])
        line(lerp_color(main, WHITE, 0.3), (sh[0] - tw * 0.1, sh[1] + 1.5), (sh[0] + tw * 0.45, sh[1] + 1.5), 1.5)
        line(bandc, (hip[0] - hw / 2, hip[1]), (hip[0] + hw / 2, hip[1]), 5)                 # cloth belt
        ribbon(chain('rsash', (hip[0] - hw * 0.4, hip[1] + 2), 4, 7, 0.5, 0.86, 0.3), bandc, 5, 2)
        leg2(hpf, kf, ff, front_leg)
        # the mane: five strands of fire, each a simulated rope that rises instead of hanging
        grow = 1.3 if hot else 1.0
        halo(fire[0], (head[0] - 2, head[1] - hr * 1.5), hr * 2.6 * grow, 90)
        for i, (off, n_, wd) in enumerate(((-1.15, 6, 14), (0.95, 6, 13), (-0.75, 7, 17), (0.6, 7, 16),
                                           (-0.38, 8, 20), (0.25, 9, 20), (-0.06, 10, 23))):
            base = (head[0] + off * hr * 0.85 - 2, head[1] - hr * (0.55 + 0.3 * (1 - abs(off))))
            pts = chain('mane%d' % i, base, n_, hr * 0.46 * grow, -0.62, 0.76, 1.0)
            flame_strand(pts, wd * grow, fire, i * 1.7)
        C(skin, head, hr)
        poly(fire[0], [(head[0] - hr * 0.98, head[1] - hr * 0.2), (head[0] - hr * 0.5, head[1] - hr * 1.05),
                       (head[0] + hr * 0.75, head[1] - hr * 0.9), (head[0] + hr * 0.95, head[1] - hr * 0.42)])   # hairline
        line(bandc, (head[0] - hr, head[1] - hr * 0.3), (head[0] + hr, head[1] - hr * 0.38), 5)
        line(lerp_color(bandc, WHITE, 0.3), (head[0] - hr * 0.7, head[1] - hr * 0.4), (head[0] + hr * 0.9, head[1] - hr * 0.46), 1)
        line(shade, (head[0] + hr * 0.1, head[1] + hr * 0.4), (head[0] + hr * 0.4, head[1] + hr * 0.28), 1.2)   # scar
        human_eye((head[0] + hr * 0.5, head[1] + hr * 0.1), hr * 0.19)
        if not pained:
            line(ink, (head[0] + hr * 0.15, head[1] - hr * 0.2), (head[0] + hr * 0.85, head[1] - hr * 0.02), 2)   # fierce brow
        if striking or pained:
            oval(ink, (head[0] + hr * 0.5, head[1] + hr * 0.58), 5, 4)                       # shout
        else:
            line(ink, (head[0] + hr * 0.3, head[1] + hr * 0.5), (head[0] + hr * 0.7, head[1] + hr * 0.58), 1.2)   # smirk
        limb(skin, sf, ef, hf, aw)
        line(shade, (sf[0] + (ef[0] - sf[0]) * 0.4 - 2, sf[1] + (ef[1] - sf[1]) * 0.4 - 1),
             (sf[0] + (ef[0] - sf[0]) * 0.6 + 2, sf[1] + (ef[1] - sf[1]) * 0.6 + 1), 1.2)     # scar
        gauntlet(ef, hf, skin, 'fistf')

    pts = pygame.mask.from_surface(s, 200).outline(3)         # dark ink line around the silhouette
    if len(pts) > 2:
        D.lines(s, OUTLINE, True, pts, 2 * SS)
    return pygame.transform.smoothscale(s, (SPR, SPR))


def quick_sprite(spec, col, key, facing, t):
    """A fighter in a named pose with no blending (menus, result screen)."""
    return render_fighter_surface(spec, col, target_pose(spec, key, t), facing, t, key)


# ---------------------------------------------------------------------
# Attack effects: every fighter's attacks look like their own weapon
# ---------------------------------------------------------------------
STONE = (132, 138, 156)


def slash_arc(surf, center, radius, a0, a1, thick, outer, core):
    """A tapered crescent swept from angle a0 to a1 (the trail a blade leaves)."""
    if abs(a1 - a0) < 0.05:
        return
    n = 16
    for color, k in ((outer, 1.0), (core, 0.45)):
        out_pts, in_pts = [], []
        for i in range(n + 1):
            u = i / n
            a = a0 + (a1 - a0) * u
            th = thick * k * math.sin(math.pi * u) ** 0.7
            r_out = radius - thick * (1 - k) * 0.3
            out_pts.append((center[0] + math.cos(a) * r_out, center[1] + math.sin(a) * r_out))
            in_pts.append((center[0] + math.cos(a) * (r_out - th), center[1] + math.sin(a) * (r_out - th)))
        pygame.draw.polygon(surf, color, out_pts + in_pts[::-1])


def flame(surf, base, d, length, width, cols):
    """A teardrop tongue of fire pointing along direction d."""
    px, py = -d[1], d[0]
    for color, k in zip(cols, (1.0, 0.68, 0.36)):
        L, W = length * k, width * k
        pygame.draw.polygon(surf, color, [
            (base[0] - px * W / 2, base[1] - py * W / 2),
            (base[0] + d[0] * L * 0.45 - px * W * 0.42, base[1] + d[1] * L * 0.45 - py * W * 0.42),
            (base[0] + d[0] * L, base[1] + d[1] * L),
            (base[0] + d[0] * L * 0.45 + px * W * 0.42, base[1] + d[1] * L * 0.45 + py * W * 0.42),
            (base[0] + px * W / 2, base[1] + py * W / 2)])


def draw_attack_fx(surf, c, box, r, frame):
    """Draws one active hitbox in the style of the fighter who owns it."""
    style, acc, main = c.spec['style'], c.colors['accent'], c.colors['main']
    span = max(1, box['end'] - box['start'] + 1)
    prog = clamp((c.move_frame - box['start'] + 1) / span, 0.0, 1.0)
    px, py = c.x, c.y - c.h * 0.62                           # shoulder pivot
    cx, cy = r.center
    centered = abs(cx - c.x) < 18 and abs(cy - (c.y - c.h / 2)) < 18
    low = box['radial'] and box['rect'][1] > 18              # ground-hugging shockwaves
    n = math.hypot(cx - px, cy - py) or 1
    d = ((cx - px) / n, (cy - py) / n)
    ang = math.atan2(d[1], d[0])
    reach = n + max(r.w, r.h) * 0.45
    slot = c.current_move.slot if c.current_move else ''
    D = pygame.draw
    if box['grab']:                                          # a snatching hand: two closing brackets
        for sgn in (-1, 1):
            off = (1 - prog) * 10 + 6
            D.lines(surf, WHITE, False, [(cx - c.facing * 8, cy + sgn * (r.h / 2 + off)),
                                         (cx + c.facing * 10, cy + sgn * off),
                                         (cx - c.facing * 8, cy + sgn * 2)], 4)
        return
    if style == 'weaver':
        if centered and box['radial']:                       # Aether Burst: a ring of snapped threads
            rad = r.w / 2 * (0.4 + 0.6 * prog)
            D.circle(surf, acc, (cx, cy), int(rad), 5)
            D.circle(surf, WHITE, (cx, cy), int(rad * 0.82), 2)
            for i in range(14):
                a = i * math.tau / 14 + frame * 0.05
                D.line(surf, WHITE if i % 2 else acc, (cx + math.cos(a) * rad * 0.35, cy + math.sin(a) * rad * 0.35),
                       (cx + math.cos(a) * rad * 1.05, cy + math.sin(a) * rad * 1.05), 2)
        elif centered:                                       # spinning blade: a full circle of steel
            a0 = frame * 0.9
            slash_arc(surf, (cx, cy), max(r.w, r.h) / 2, a0, a0 + math.tau * 0.8, 16, acc, WHITE)
        else:                                                # crescent slash following the blade
            sweep = 1.05 * c.facing
            a_end = ang - sweep + 2 * sweep * min(1.0, prog * 1.4 + 0.25)
            slash_arc(surf, (px, py), reach, ang - sweep, a_end, 12 + box['damage'] * 0.9, acc, WHITE)
            tipx, tipy = px + math.cos(a_end) * reach, py + math.sin(a_end) * reach
            D.line(surf, acc, (px + d[0] * 20, py + d[1] * 20), (tipx, tipy), 1)             # the thread
    elif style == 'sentinel':
        dark = c.colors['dark']
        if low:                                              # Ground Slam: stone spikes erupt from the floor
            cnt = max(3, r.w // 34)
            ease = 1 - (1 - prog) ** 2
            for i in range(cnt):
                x = r.left + (i + 0.5) * r.w / cnt
                k = 1 - abs(x - cx) / (r.w / 2) * 0.55
                hgt = (r.h + 56) * k * ease
                leanx = (x - cx) * 0.12
                D.polygon(surf, STONE, [(x - 15, r.bottom), (x + 15, r.bottom), (x + leanx, r.bottom - hgt)])
                D.polygon(surf, dark, [(x - 15, r.bottom), (x + 15, r.bottom), (x + leanx, r.bottom - hgt)], 2)
                D.line(surf, acc, (x, r.bottom - 2), (x + leanx * 0.6, r.bottom - hgt * 0.6), 2)
        elif centered:                                       # Rolling Boulder
            a0 = frame * 0.6
            slash_arc(surf, (cx, cy), max(r.w, r.h) / 2 + 4, a0, a0 + math.tau * 0.7, 18, STONE, acc)
        elif slot in MAUL_SLOTS:                             # maul: a short, heavy arc ending in an impact
            sweep = 0.7 * c.facing
            slash_arc(surf, (px, py), reach, ang - sweep, ang - sweep + 2 * sweep * min(1.0, prog * 1.5 + 0.2),
                      24, STONE, acc)
            if prog > 0.45:
                for i in range(6):
                    a = i * math.tau / 6 + 0.3
                    D.line(surf, acc, (cx + math.cos(a) * 14, cy + math.sin(a) * 14),
                           (cx + math.cos(a) * (26 + prog * 16), cy + math.sin(a) * (26 + prog * 16)), 4)
        else:                                                # shield / body blows: chevron shockwaves
            hh = r.h * 0.42
            for i in range(3):
                off = (i - 1) * 16 + prog * 18
                bx, by = cx + d[0] * off, cy + d[1] * off
                D.lines(surf, acc if i < 2 else WHITE, False, [
                    (bx - d[0] * 12 - d[1] * hh, by - d[1] * 12 + d[0] * hh),
                    (bx + d[0] * 6, by + d[1] * 6),
                    (bx - d[0] * 12 + d[1] * hh, by - d[1] * 12 - d[0] * hh)], 7 - i * 2)
    elif style == 'voidcaller':
        rng = random.Random(hash(slot) + box['gid'] * 13)
        glow(surf, acc, (cx, cy), max(r.w, r.h) * 0.45, 70)
        if low or centered:                                  # rings of orbiting stars
            rect = r.inflate(-10, 0) if low else r
            D.ellipse(surf, acc, rect, 3)
            for i in range(8):
                a = i * math.tau / 8 + frame * 0.18
                sx_, sy_ = rect.centerx + math.cos(a) * rect.w / 2, rect.centery + math.sin(a) * rect.h / 2
                D.polygon(surf, WHITE, star_points(sx_, sy_, 7 + 3 * math.sin(frame * 0.5 + i), frame * 0.1))
        else:                                                # a constellation snaps into being
            pts = [(r.left + r.w * (0.1 + 0.8 * (i + rng.random() * 0.6) / 6),
                    r.top + r.h * rng.uniform(0.12, 0.88)) for i in range(6)]
            shown = max(2, int(len(pts) * min(1.0, prog * 1.5 + 0.3)))
            D.lines(surf, acc, False, pts[:shown], 2)
            for i, p in enumerate(pts[:shown]):
                D.polygon(surf, WHITE, star_points(p[0], p[1], 7 + 3 * math.sin(frame * 0.6 + i * 2), frame * 0.12))
                D.circle(surf, acc, (int(p[0]), int(p[1])), 3)
    else:                                                    # kindled: living fire
        cols = ((255, 96, 24), acc, (255, 250, 225))
        jit = random.Random(frame * 7 + box['gid'])
        if low:                                              # Magma Stomp: flames race along the ground
            cnt = max(4, r.w // 26)
            for i in range(cnt):
                x = r.left + (i + 0.5) * r.w / cnt
                k = 1 - abs(x - cx) / (r.w / 2) * 0.5
                flame(surf, (x, r.bottom), (0.0, -1.0), (r.h + 50) * k * jit.uniform(0.6, 1.1), 22, cols)
        elif centered:                                       # fire wheel / ignition burst
            rad = max(r.w, r.h) * 0.2
            for i in range(9):
                a = i * math.tau / 9 + frame * 0.5
                flame(surf, (cx + math.cos(a) * rad, cy + math.sin(a) * rad), (math.cos(a + 0.6), math.sin(a + 0.6)),
                      max(r.w, r.h) * 0.36 * jit.uniform(0.8, 1.2), 20, cols)
        else:                                                # fire trails off the fist or foot
            ext = (abs(d[0]) * r.w + abs(d[1]) * r.h)
            base = (cx - d[0] * ext * 0.3, cy - d[1] * ext * 0.3)
            for i in range(3):
                off = (i - 1) * max(r.w * abs(d[1]), r.h * abs(d[0])) * 0.24
                b = (base[0] - d[1] * off, base[1] + d[0] * off)
                flame(surf, b, d, ext * (0.95 - abs(i - 1) * 0.25) * jit.uniform(0.8, 1.1), 22 - abs(i - 1) * 6, cols)


def draw_death_ray(surf, c):
    """The comically large gun and its screen-filling beam."""
    f, face = c.move_frame, c.facing
    k = min(1.0, f / 10) if f < 104 else max(0.0, (124 - f) / 20)
    if k <= 0.02:
        return
    k *= 1 + 0.12 * math.sin(min(f, 10) / 10 * math.pi)       # pops in with a little overshoot
    gx = c.x + face * (c.w * 0.5 + 64) + (-face * 16 if 56 <= f < 66 else 0) + (random.uniform(-2, 2) if f < 56 else 0)
    gy = c.y - c.h * 0.5
    D = pygame.draw

    def R(x0, y0, x1, y1):
        xa, xb = gx + face * x0 * k, gx + face * x1 * k
        return pygame.Rect(int(min(xa, xb)), int(gy + y0 * k), int(abs(xb - xa)), int((y1 - y0) * k))

    charge = clamp(f / 56, 0.0, 1.0)
    hot = lerp_color((90, 220, 255), (255, 80, 80), charge)
    D.rect(surf, (48, 44, 62), R(-90, -22, 6, 30), border_radius=8)                          # stock
    D.rect(surf, (70, 60, 50), R(24, 40, 52, 86), border_radius=6)                           # grip
    D.rect(surf, (82, 86, 108), R(-12, -54, 168, 46), border_radius=14)                      # body
    D.rect(surf, OUTLINE, R(-12, -54, 168, 46), 4, border_radius=14)
    D.ellipse(surf, hot, R(6, -100, 124, -40))                                               # energy tank
    D.ellipse(surf, WHITE, R(26, -90, 76, -66))
    D.ellipse(surf, OUTLINE, R(6, -100, 124, -40), 4)
    D.rect(surf, (112, 116, 140), R(160, -38, 300, 30), border_radius=6)                     # barrel
    for i in range(4):
        D.rect(surf, hot, R(176 + i * 30, -44, 188 + i * 30, 36), border_radius=3)           # cooling rings
    D.rect(surf, (48, 44, 62), R(292, -62, 336, 54), border_radius=10)                       # muzzle
    D.rect(surf, hot, R(326, -46, 336, 38))
    for i in range(5):                                                                        # warning stripes
        D.line(surf, (255, 210, 60), (gx + face * (10 + i * 28) * k, gy + 44 * k),
               (gx + face * (24 + i * 28) * k, gy + 22 * k), max(2, int(5 * k)))
    mx, my = gx + face * 338 * k, gy - 4 * k
    if f < 56:                                                # charging: energy gathers at the muzzle
        rad = 8 + f * 1.4
        glow(surf, (255, 90, 90), (mx, my), rad * 2.2, 170)
        D.circle(surf, WHITE, (int(mx), int(my)), int(rad * 0.55))
        for i in range(8):
            a = random.uniform(0, math.tau)
            r1, r2 = rad * 1.2, rad * 1.2 + random.uniform(40, 150)
            D.line(surf, hot, (mx + math.cos(a) * r2, my + math.sin(a) * r2),
                   (mx + math.cos(a) * r1, my + math.sin(a) * r1), 3)
    elif f < 100:                                             # FIRE
        fade = 1.0 if f < 86 else (100 - f) / 14
        hh = (300 + random.uniform(-24, 24)) * fade
        x_end = WORLD_W + 200 if face > 0 else -200
        left, width = min(mx, x_end), abs(x_end - mx)
        for color, kk in (((255, 50, 60), 1.0), ((255, 170, 110), 0.7), (WHITE, 0.4)):
            D.rect(surf, color, (int(left), int(my - hh * kk), int(width), int(hh * kk * 2)),
                   border_radius=int(hh * kk * 0.5))
        for color, kk in (((255, 50, 60), 1.15), ((255, 170, 110), 0.85), (WHITE, 0.5)):
            D.circle(surf, color, (int(mx), int(my)), int(hh * kk))
        for i in range(10):                                   # crackle along the beam
            bx = left + random.uniform(0, width)
            D.line(surf, WHITE, (bx, my - hh), (bx + random.uniform(-60, 60), my - hh - random.uniform(20, 90)), 4)
            D.line(surf, WHITE, (bx, my + hh), (bx + random.uniform(-60, 60), my + hh + random.uniform(20, 90)), 4)


def make_icon(size=256):
    """The game's icon: two crescent slashes (red and blue) clashing in a burst of Aether."""
    s = pygame.Surface((size, size), pygame.SRCALPHA)
    k = size / 256
    bg = pygame.Surface((size, size), pygame.SRCALPHA)
    for y in range(size):
        pygame.draw.line(bg, lerp_color((34, 20, 84), (12, 8, 34), y / size), (0, y), (size, y))
    mask = pygame.Surface((size, size), pygame.SRCALPHA)
    pygame.draw.rect(mask, WHITE, (0, 0, size, size), border_radius=int(52 * k))
    bg.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MIN)
    s.blit(bg, (0, 0))
    c = size / 2
    glow(s, (120, 230, 255), (c, c), 120 * k, 150)
    slash_arc(s, (c - 34 * k, c + 30 * k), 118 * k, -2.2, -0.2, 34 * k, (214, 48, 72), (255, 190, 190))
    slash_arc(s, (c + 34 * k, c + 30 * k), 118 * k, math.pi + 0.2, math.pi + 2.2, 34 * k, (70, 130, 220), (190, 225, 255))
    pygame.draw.polygon(s, (255, 214, 120), star_points(c, c - 14 * k, 70 * k, -math.pi / 2))
    pygame.draw.polygon(s, WHITE, star_points(c, c - 14 * k, 40 * k, -math.pi / 2))
    pygame.draw.rect(s, (255, 214, 120), (3, 3, size - 6, size - 6), max(2, int(8 * k)), border_radius=int(50 * k))
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
        self.dir = os.path.join(app_dir(), 'assets')        # next to the .exe when packaged
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

    def _attack_info(self, c):
        """Works out how the current move should be animated: which limb, aimed where, in which phase."""
        m, f, style = c.current_move, c.move_frame, c.spec['style']
        weapon = 'maul' if (style == 'sentinel' and m.slot in MAUL_SLOTS) else 'shield'
        info = dict(weapon=weapon, hot=True)
        boxes = [b for b in m.hitbox_data if not b['spike']] or m.hitbox_data
        if not boxes:                                        # projectile casts and teleports
            pf = m.projectile['frame'] if m.projectile else (m.aim[1] if m.aim else m.duration // 3)
            info.update(limb='cast', T=(c.w, -c.h * 0.6),
                        phase='wind' if f < pf else 'strike' if f < pf + 9 else 'recover')
            return info
        s, e = min(b['start'] for b in boxes), max(b['end'] for b in boxes)
        live = [b for b in boxes if b['start'] <= f <= b['end']]
        box = max(live, key=lambda b: b['rect'][2] * b['rect'][3]) if live else boxes[0]
        info.update(limb=attack_limb(style, box), T=(box['rect'][0], -c.h / 2 + box['rect'][1]),
                    phase='wind' if f < s else 'strike' if f <= e + 2 else 'recover',
                    spin=-360.0 * clamp((f - s + 1) / (e - s + 2), 0.0, 1.0) * c.facing)
        return info

    def _determine_visuals(self, character):
        c, state = character, character.current_state
        v = dict(sprite_key='idle', tint=None, outline=None, alpha=255, rotation=0, info={})
        if state == "RUNNING":
            v['sprite_key'] = 'run'
        elif state == "JUMPING":
            v['sprite_key'] = 'jump' if c.vy < 0 else 'fall'
        elif state == "ATTACKING" and c.current_move:
            m = c.current_move
            armored = m.armor and m.armor[0] <= c.move_frame <= m.armor[1]
            if m is DEATH_RAY:
                v.update(sprite_key='gun', outline=(255, 90, 90))
            elif m.counter:
                v.update(sprite_key='shield', outline=(120, 255, 150))
            else:
                v.update(sprite_key='attack', info=self._attack_info(c),
                         outline=c.colors['accent'] if armored else WHITE)
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
        elif state == "GRABBING":
            v.update(sprite_key='grab', info=dict(pommel=c.pommel_anim), outline=WHITE)
        elif state == "GRABBED":
            v.update(sprite_key='grabbed', outline=(255, 230, 60))
        elif state == "RESPAWN":
            v['outline'] = WHITE
        if c.buff_timer > 0:
            v['info'] = dict(v['info'], hot=True)
        if c.flash > 0 and not v['tint']:
            v['tint'] = (255, 255, 255, 120)
        if c.intangible > 0 and state not in ("DODGE", "ATTACKING") and (c.anim // 4) % 2:
            v['alpha'] = 120
        if c.current_move and c.current_move.aim and c.current_move.aim[1] <= c.move_frame <= c.current_move.aim[2]:
            v['alpha'] = 0
        return v

    def sprite_for(self, c, v):
        key, info = v['sprite_key'], v['info']
        target = target_pose(c.spec, key, c.anim, info)
        rate = 0.8 if info.get('phase') == 'strike' else 0.42
        c.pose = target if c.pose is None else blend_pose(c.pose, target, rate)
        img = self.assets.get(f"{c.name.lower()}_{key}")
        if img:
            surf = pygame.Surface((SPR, SPR), pygame.SRCALPHA)
            img = img if c.facing > 0 else pygame.transform.flip(img, True, False)
            surf.blit(img, (SPR_CX - img.get_width() // 2, SPR_FEET - img.get_height()))
            return surf
        vel = ((c.vx + c.kbx) * c.facing, c.vy + c.kby)
        if c.cloth is None:
            c.cloth = ClothSim()
        return render_fighter_surface(c.spec, c.colors, c.pose, c.facing, c.anim, key, vel, info, c.cloth, (c.x, c.y))

    def update_visuals(self, surface, characters, physics, show_boxes=False):
        for c in characters.values():
            if c.current_state == "KO":
                continue
            v = self._determine_visuals(c)
            if c.current_state == "RESPAWN":                    # arrive on a little halo platform
                pygame.draw.ellipse(surface, WHITE, (c.x - 50, c.y - 4, 100, 16))
                pygame.draw.ellipse(surface, c.colors['accent'], (c.x - 50, c.y - 4, 100, 16), 3)
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
                    pts = pygame.mask.from_surface(spr, 200).outline(3)
                    if len(pts) > 2:
                        pygame.draw.lines(spr, v['outline'], True, pts, 2)
                if v['alpha'] < 255:
                    spr.set_alpha(v['alpha'])
                rotation = v['rotation'] or c.pose.get('spin', 0)
                if rotation:
                    pivot = pygame.Surface((SPR, SPR), pygame.SRCALPHA)      # rotate around the body centre
                    pivot.blit(spr, (0, SPR // 2 - (SPR_FEET - c.h // 2)))
                    rot = pygame.transform.rotate(pivot, rotation)
                    if v['alpha'] < 255:
                        rot.set_alpha(v['alpha'])
                    surface.blit(rot, rot.get_rect(center=(c.x, c.y - c.h / 2)))
                elif abs(c.squash) > 0.02:                           # squash and stretch, anchored at the feet
                    kx, ky = 1 - c.squash * 0.6, 1 + c.squash
                    spr2 = pygame.transform.smoothscale(spr, (int(SPR * kx), int(SPR * ky)))
                    if v['alpha'] < 255:
                        spr2.set_alpha(v['alpha'])
                    surface.blit(spr2, (c.x - SPR_CX * kx, c.y - SPR_FEET * ky))
                else:
                    surface.blit(spr, (c.x - SPR_CX, c.y - SPR_FEET))
            if c.current_move is DEATH_RAY:
                draw_death_ray(surface, c)
            else:
                for box in physics.active_hitboxes(c):          # weapon trails, flames, constellations...
                    r = physics.hitbox_rect(c, box)
                    if box['spike']:                            # a small glint marks the spike spot
                        pygame.draw.lines(surface, (255, 160, 255), False,
                                          [(r.left, r.centery - 5), (r.centerx, r.bottom), (r.right, r.centery - 5)], 3)
                    else:
                        draw_attack_fx(surface, c, box, r, physics.frame)
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
                    pygame.draw.polygon(surface, (255, 255, 0), star_points(
                        c.x + math.cos(a) * 26, c.y - c.h - 14 + math.sin(a) * 7, 7, c.anim * 0.1))
            if show_boxes:
                pygame.draw.rect(surface, (0, 255, 0), c.rect, 2)
                for box in physics.active_hitboxes(c):
                    if box['rect'][2] < 1000:
                        pygame.draw.rect(surface, (255, 0, 255) if box['spike'] else (255, 0, 0),
                                         physics.hitbox_rect(c, box), 2)


# =====================================================================
# PLAYER PROFILES (a name + optional custom key bindings, saved to disk)
# =====================================================================

def app_dir():
    """Folder of the .exe when packaged, otherwise of this script."""
    return os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))


class ProfileStore:
    FILE = "aether_clash_profiles.json"

    def __init__(self, path=None):
        self.paths = [path] if path else [os.path.join(app_dir(), self.FILE),
                                          os.path.join(os.path.expanduser("~"), "." + self.FILE)]
        self.profiles = []
        for p in self.paths:
            try:
                with open(p, encoding='utf-8') as fh:
                    data = json.load(fh)
                self.profiles = [dict(name=str(d['name'])[:12],
                                      keys={a: [int(k) for k in v] for a, v in d['keys'].items()} if d.get('keys') else None)
                                 for d in data.get('profiles', [])]
                break
            except (OSError, ValueError, KeyError, TypeError, AttributeError):
                continue

    def save(self):
        for p in self.paths:
            try:
                with open(p, 'w', encoding='utf-8') as fh:
                    json.dump({'profiles': self.profiles}, fh, indent=1)
                return True
            except OSError:
                continue
        return False


# =====================================================================
# ONLINE PLAY SCAFFOLDING (not switched on yet)
# The game is already built for it: every controller produces one small InputState per frame and the
# simulation runs in fixed 1/60 s steps, so two computers that exchange inputs stay in sync ("lockstep").
# What exists: the wire format below, NetworkDevice (a remote player's controller), and the relay server
# in aether_server.py. What is left: OnlineClient's socket code and a lobby screen.
# =====================================================================
NET_PROTOCOL_VERSION = 1
DEFAULT_SERVER = ("127.0.0.1", 7777)


def encode_input(inp):
    """Packs one frame of input into a small integer (fits in 2 bytes)."""
    bits = (inp.x + 1) | ((inp.y + 1) << 2) | (int(inp.down_tap) << 4)
    for i, b in enumerate(BUTTONS):
        bits |= (int(b in inp.held) << (5 + i)) | (int(b in inp.pressed) << (5 + len(BUTTONS) + i))
    return bits


def decode_input(bits):
    inp = InputState()
    inp.x, inp.y, inp.down_tap = (bits & 3) - 1, ((bits >> 2) & 3) - 1, bool(bits >> 4 & 1)
    for i, b in enumerate(BUTTONS):
        if bits >> (5 + i) & 1:
            inp.held.add(b)
        if bits >> (5 + len(BUTTONS) + i) & 1:
            inp.pressed.add(b)
    return inp


class NetworkDevice:
    """Input device for a remote player: plays back the inputs received from the network, in order."""
    def __init__(self):
        self.queue = []

    def push(self, bits):
        self.queue.append(bits)

    def poll(self):
        return decode_input(self.queue.pop(0)) if self.queue else InputState()


class OnlineClient:
    """Placeholder for the connection to aether_server.py (see that file for the message format)."""
    def __init__(self, address=DEFAULT_SERVER):
        self.address, self.status = address, "offline"

    def connect(self, player_name):
        raise NotImplementedError("Online play is not implemented yet.")


# =====================================================================
# 5. GAME MANAGER (Module 5 - The Loop)
# =====================================================================

class GameManager:
    P2_MODES = ["CPU - Easy", "CPU - Normal", "CPU - Hard", "Human"]
    TITLE_OPTIONS = ["Local Play", "Online", "Profiles & Controls", "Quit"]

    def __init__(self, headless=False, demo=False, profile_path=None):
        if headless:
            os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
            os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
        pygame.init()
        pygame.display.set_caption("AETHER CLASH")
        try:
            pygame.display.set_icon(make_icon(64))
        except pygame.error:
            pass
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
        self.logo = make_icon(150)
        self.joysticks = []
        try:
            pygame.joystick.init()
            self.joysticks = [pygame.joystick.Joystick(i) for i in range(pygame.joystick.get_count())]
        except pygame.error:
            pass
        self.fx = Effects()
        self.physics_engine = self.combat_manager = None
        self.map_data = self.stages[0]
        self.characters, self.controllers = {}, {}
        self.keys_pressed, self.keys_held = set(), pygame.key.get_pressed()
        self.scene = 'MENU' if demo else 'TITLE'
        self.title_sel = 0
        self.sel, self.stage_sel, self.p2_mode, self.p1_cpu = [0, 1], 0, 2 if demo else 1, demo
        self.profiles = ProfileStore(profile_path)
        self.profile_sel = [0, 0]              # per player: 0 = built-in default, n = saved profile n-1
        self.prof_cursor, self.prof_mode, self.prof_text, self.prof_bind_i, self.prof_edit = 0, None, "", 0, None
        self.paused = self.show_boxes = False
        self.running = True
        self.frame = self.intro = self.end_timer = 0
        self.winner = None
        self.duel = None
        self.cam = pygame.Rect(0, 0, WORLD_W, WORLD_W * 9 // 16)
        self.camf = [600.0, 300.0, 1200.0]

    # ------------------------------------------------------------ profiles
    def slot_profile(self, slot):
        i = self.profile_sel[slot] - 1
        return self.profiles.profiles[i] if 0 <= i < len(self.profiles.profiles) else None

    def slot_keymap(self, slot):
        km = dict(KEYMAP_P1 if slot == 0 else KEYMAP_P2)
        prof = self.slot_profile(slot)
        if prof and prof['keys']:
            km.update(prof['keys'])
        return km

    def slot_name(self, slot, cpu=False):
        prof = self.slot_profile(slot)
        return prof['name'] if prof else ("CPU" if cpu else f"P{slot + 1}")

    # ------------------------------------------------------------ setup
    def setup_game(self, p1=0, p2=1, stage=0, p2_mode=1, p1_cpu=False, names=None):
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
            cpu = p1_cpu if slot == 0 else not human2
            ch.is_cpu = cpu
            ch.player_name = names[slot] if names else self.slot_name(slot, cpu)
            self.characters[slot + 1] = ch
            self.physics_engine.register_character(ch)
            if cpu:
                level = 3 if (slot == 0) else p2_mode + 1
                ctrl = AIController(ch, self.combat_manager, self.physics_engine, min(3, level))
            else:
                ctrl = CharacterController(ch, self.combat_manager,
                                           KeyboardDevice(self.slot_keymap(slot), self, pad1 if slot == 0 else pad2))
            self.controllers[slot + 1] = ctrl
        self.scene, self.paused = 'FIGHT', False
        self.intro, self.end_timer, self.winner, self.frame, self.duel = 100, 0, None, 0, None
        self.camf = [600.0, 300.0, 1200.0]

    # ------------------------------------------------------------ the loop
    def run_game_loop(self):
        """Fixed-timestep loop: real time is measured (delta_time) and spent in exact 1/60 s simulation steps,
        so the physics behaves identically on fast and slow computers."""
        step, accumulator = 1.0 / FPS, 0.0
        while self.running:
            delta_time = min(self.clock.tick(FPS) / 1000.0, 0.1)
            accumulator += delta_time
            self._watch_performance()
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

    def _watch_performance(self):
        """If drawing is too slow for 60 FPS, switch the fighters to the cheaper single-resolution drawing."""
        global SS
        self.slow_frames = getattr(self, 'slow_frames', 0)
        if self.scene == 'FIGHT' and not self.paused and SS > 1:
            self.slow_frames = self.slow_frames + 1 if self.clock.get_rawtime() > 15 else max(0, self.slow_frames - 1)
            if self.slow_frames > 90:
                SS = 1

    def _handle_input_events(self):
        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                self.running = False
            elif e.type == pygame.KEYDOWN:
                self.keys_pressed.add(e.key)
                self._on_key(e.key, getattr(e, 'unicode', ''))
        self.keys_held = pygame.key.get_pressed()

    def _on_key(self, k, text=''):
        K = pygame
        up, down = k in (K.K_w, K.K_UP), k in (K.K_s, K.K_DOWN)
        enter = k in (K.K_RETURN, K.K_KP_ENTER)
        if self.scene == 'TITLE':
            if up or down:
                self.title_sel = (self.title_sel + (1 if down else -1)) % len(self.TITLE_OPTIONS)
            elif enter or k == K.K_SPACE:
                choice = self.TITLE_OPTIONS[self.title_sel]
                if choice == "Quit":
                    self.running = False
                else:
                    self.scene = {'Local Play': 'MENU', 'Online': 'ONLINE'}.get(choice, 'PROFILES')
            elif k == K.K_ESCAPE:
                self.running = False
        elif self.scene == 'ONLINE':
            if enter or k in (K.K_ESCAPE, K.K_BACKSPACE):
                self.scene = 'TITLE'
        elif self.scene == 'PROFILES':
            self._profiles_key(k, text, up, down, enter)
        elif self.scene == 'MENU':
            n, np_ = len(ROSTER), len(self.profiles.profiles) + 1
            if k == K.K_ESCAPE:
                self.scene = 'TITLE'
            elif k in (K.K_a, K.K_d):
                self.sel[0] = (self.sel[0] + (1 if k == K.K_d else -1)) % n
            elif k in (K.K_LEFT, K.K_RIGHT):
                self.sel[1] = (self.sel[1] + (1 if k == K.K_RIGHT else -1)) % n
            elif k in (K.K_q, K.K_e):
                self.profile_sel[0] = (self.profile_sel[0] + (1 if k == K.K_e else -1)) % np_
            elif k in (K.K_LEFTBRACKET, K.K_RIGHTBRACKET):
                self.profile_sel[1] = (self.profile_sel[1] + (1 if k == K.K_RIGHTBRACKET else -1)) % np_
            elif up or down:
                self.stage_sel = (self.stage_sel + (1 if down else -1)) % len(self.stages)
            elif k == K.K_TAB:
                self.p2_mode = (self.p2_mode + 1) % len(self.P2_MODES)
            elif k == K.K_F2:
                self.p1_cpu = not self.p1_cpu
            elif enter:
                self.setup_game(self.sel[0], self.sel[1], self.stage_sel, self.p2_mode, self.p1_cpu)
        elif self.scene == 'FIGHT':
            if k in (K.K_ESCAPE, K.K_p):
                self.paused = not self.paused
            elif k == K.K_F1:
                self.show_boxes = not self.show_boxes
            elif self.paused and k in (K.K_BACKSPACE, K.K_q):
                self.scene = 'MENU'
            elif self.paused and enter:
                self.paused = False
        elif self.scene == 'RESULT':
            if enter or k == K.K_SPACE:
                self.setup_game(self.sel[0], self.sel[1], self.stage_sel, self.p2_mode, self.p1_cpu)
            elif k in (K.K_ESCAPE, K.K_BACKSPACE):
                self.scene = 'MENU'

    def _profiles_key(self, k, text, up, down, enter):
        K, profs = pygame, self.profiles.profiles
        if self.prof_mode == 'name':                          # typing a name
            if k == K.K_ESCAPE:
                self.prof_mode = None
            elif enter and self.prof_text.strip():
                name = self.prof_text.strip()[:12]
                if self.prof_edit is None:                    # brand new profile
                    profs.append(dict(name=name, keys=None))
                    self.prof_cursor = len(profs) - 1
                    self.prof_edit = profs[-1]
                    self.prof_mode = 'ask'
                else:                                         # rename
                    self.prof_edit['name'] = name
                    self.prof_mode = None
                self.profiles.save()
            elif k == K.K_BACKSPACE:
                self.prof_text = self.prof_text[:-1]
            elif text and text.isprintable() and len(self.prof_text) < 12:
                self.prof_text += text
        elif self.prof_mode == 'ask':                         # custom keys or defaults?
            if k == K.K_y:
                self.prof_mode, self.prof_bind_i, self.prof_new_keys = 'bind', 0, {}
            elif k in (K.K_n, K.K_ESCAPE) or enter:
                self.prof_mode = None
        elif self.prof_mode == 'bind':                        # press a key for each action in turn
            if k == K.K_ESCAPE:
                self.prof_mode = None
            else:
                self.prof_new_keys[ACTIONS[self.prof_bind_i][0]] = [k]
                self.prof_bind_i += 1
                if self.prof_bind_i >= len(ACTIONS):
                    self.prof_edit['keys'] = self.prof_new_keys
                    self.profiles.save()
                    self.prof_mode = None
        else:
            cur = profs[self.prof_cursor] if profs and self.prof_cursor < len(profs) else None
            if k == K.K_ESCAPE:
                self.scene = 'TITLE'
            elif (up or down) and profs:
                self.prof_cursor = (self.prof_cursor + (1 if down else -1)) % len(profs)
            elif k == K.K_n and len(profs) < 12:
                self.prof_mode, self.prof_text, self.prof_edit = 'name', "", None
            elif cur and k == K.K_r:
                self.prof_mode, self.prof_text, self.prof_edit = 'name', cur['name'], cur
            elif cur and (enter or k == K.K_k):
                self.prof_edit, self.prof_mode, self.prof_bind_i, self.prof_new_keys = cur, 'bind', 0, {}
            elif cur and k == K.K_x:                          # back to the default keys
                cur['keys'] = None
                self.profiles.save()
            elif cur and k == K.K_DELETE:
                profs.remove(cur)
                self.prof_cursor = max(0, self.prof_cursor - 1)
                self.profile_sel = [0, 0]
                self.profiles.save()

    def _step(self):
        """One 60th of a second of game time."""
        self.frame += 1
        if self.scene != 'FIGHT' or self.paused:
            return
        if self.duel:
            self._step_duel()
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
            if self.combat_manager.pending_duel:
                self._start_duel(self.combat_manager.pending_duel)
                self.combat_manager.pending_duel = None
                return
        self.physics_engine.update_all()                              # 2. physics (calls combat on hits)
        self.combat_manager.check_all_move_durations()                # 3. combat timers / state machine
        self.fx.update()
        self._ambient()
        self._update_camera()
        if self.physics_engine.game_over and self.end_timer == 0 and self.winner is None:
            alive = [c for c in self.characters.values() if c.current_lives > 0]
            self.winner = alive[0] if alive else None
            self.end_timer = 170

    # ------------------------------------------------------------ the Death Ray duel (both players "crypticsh")
    def _new_prompt(self, last=None):
        return random.choice([a for a, _ in ACTIONS if a != last])

    def _start_duel(self, initiator):
        self.duel = dict(counts=[0, 0], prompts=[self._new_prompt(), self._new_prompt()], prev=[(0, 0), (0, 0)],
                         miss=[0, 0], cpu=[40, 40], intro=70, initiator=initiator)

    def _step_duel(self):
        d = self.duel
        self._update_camera()
        if d['intro'] > 0:
            d['intro'] -= 1
            return
        for slot in (0, 1):
            ctrl, c = self.controllers[slot + 1], self.characters[slot + 1]
            d['miss'][slot] = max(0, d['miss'][slot] - 1)
            if isinstance(ctrl, AIController):                        # CPUs "type" at a speed set by their level
                d['cpu'][slot] -= 1
                pressed = set()
                if d['cpu'][slot] <= 0:
                    pressed = {d['prompts'][slot]}
                    d['cpu'][slot] = {1: 26, 2: 18, 3: 12}[ctrl.level] + random.randint(0, 8)
            else:
                inp = ctrl.device.poll()
                px, py = d['prev'][slot]
                pressed = set(inp.pressed)
                if inp.x and inp.x != px:
                    pressed.add('left' if inp.x < 0 else 'right')
                if inp.y and inp.y != py:
                    pressed.add('up' if inp.y < 0 else 'down')
                d['prev'][slot] = (inp.x, inp.y)
            if d['prompts'][slot] in pressed:
                d['counts'][slot] += 1
                d['prompts'][slot] = self._new_prompt(d['prompts'][slot])
                self.fx.spark(c.x, c.y - c.h - 60, c.colors['accent'], 5, 5, 14, 4)
            elif pressed:
                d['miss'][slot] = 12                                  # wrong key: a short lock-out
            if d['counts'][slot] >= DUEL_TARGET:
                for o in self.characters.values():
                    o.aether_gauge = 0.0
                self.duel = None
                self.combat_manager.fire_death_ray(c)
                return
        self.fx.update()

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

    # ------------------------------------------------------------ rendering helpers
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

    def panel(self, rect, border=None, alpha=190):
        p = pygame.Surface(rect.size, pygame.SRCALPHA)
        pygame.draw.rect(p, (10, 8, 30, alpha), p.get_rect(), border_radius=18)
        self.screen.blit(p, rect)
        if border:
            pygame.draw.rect(self.screen, border, rect, 3, border_radius=18)

    def key_names(self, keys):
        return "  or  ".join(pygame.key.name(k).upper() for k in keys)

    def _menu_backdrop(self):
        if not self.thumbs:
            self.thumbs = []
            keep = self.map_data
            for st in self.stages:
                self.map_data, pe = st, self.physics_engine
                self.physics_engine = None
                view = pygame.Rect(400, 330, 1600, 900)
                self._draw_world(view)
                self.thumbs.append(pygame.transform.smoothscale(self.world.subsurface(view), (288, 162)))
                self.physics_engine = pe
            self.map_data = keep
            self.menu_bg = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT))
            for y in range(SCREEN_HEIGHT):
                pygame.draw.line(self.menu_bg, lerp_color((14, 10, 40), (60, 26, 84), y / SCREEN_HEIGHT),
                                 (0, y), (SCREEN_WIDTH, y))
        self.screen.blit(self.menu_bg, (0, 0))

    def _draw_world(self, view):
        """Draws the arena into self.world (only the part the camera sees)."""
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
        scene = self.scene
        if scene in ('TITLE', 'ONLINE', 'PROFILES', 'MENU'):
            self._menu_backdrop()
            {'TITLE': self._render_title, 'ONLINE': self._render_online,
             'PROFILES': self._render_profiles, 'MENU': self._render_menu}[scene]()
            return
        view = self.cam.copy()
        if self.fx.shake and not self.paused:
            view.move_ip(random.randint(-self.fx.shake, self.fx.shake), random.randint(-self.fx.shake, self.fx.shake))
            view.clamp_ip(pygame.Rect(0, 0, WORLD_W, WORLD_H))
        self._draw_world(view)
        pygame.transform.smoothscale(self.world.subsurface(view), (SCREEN_WIDTH, SCREEN_HEIGHT), self.screen)
        self._render_hud(view)
        if scene == 'RESULT':
            self._render_result()

    # ------------------------------------------------------------ in-match HUD
    def _render_hud(self, view):
        scr, scale = self.screen, SCREEN_WIDTH / view.w
        for c in self.characters.values():                           # the Death Ray whites out the whole screen
            if c.current_move is DEATH_RAY and 56 <= c.move_frame < 80:
                flash = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
                flash.fill((255, 235, 235, int(235 * (80 - c.move_frame) / 24)))
                scr.blit(flash, (0, 0))
        heads = {}
        for c in self.characters.values():                           # name tags above the fighters
            if c.current_state == "KO":
                continue
            sx, sy = (c.x - view.x) * scale, (c.y - c.h - 30 - view.y) * scale
            heads[c.slot] = (sx, sy)
            col = PLAYER_TAGS[c.slot]
            self.text(scr, c.player_name, 22, col, (sx, sy - 12))
            pygame.draw.polygon(scr, col, [(sx - 7, sy), (sx + 7, sy), (sx, sy + 9)])
        for c in self.characters.values():                           # damage / stocks / Aether panels
            px = SCREEN_WIDTH * (0.28 if c.slot == 0 else 0.72)
            self.panel(pygame.Rect(px - 150, SCREEN_HEIGHT - 116, 300, 104), PLAYER_TAGS[c.slot], 170)
            pygame.draw.circle(scr, c.colors['dark'], (int(px - 108), SCREEN_HEIGHT - 66), 30)
            pygame.draw.circle(scr, c.colors['main'], (int(px - 108), SCREEN_HEIGHT - 66), 24)
            pygame.draw.circle(scr, c.colors['accent'], (int(px - 108), SCREEN_HEIGHT - 66), 10)
            d = c.cumulative_damage_taken
            col = lerp_color(WHITE, (255, 200, 60), d / 70) if d < 70 else \
                lerp_color((255, 200, 60), (255, 50, 40), (d - 70) / 70) if d < 140 else \
                lerp_color((255, 50, 40), (140, 10, 30), (d - 140) / 80)
            jitter = (random.randint(-3, 3), random.randint(-3, 3)) if c.hitstop > 0 else (0, 0)
            self.text(scr, f"{int(d)}%", 64, col, (px + 10 + jitter[0], SCREEN_HEIGHT - 74 + jitter[1]))
            self.text(scr, f"{c.player_name}  -  {c.name.upper()}", 20, WHITE, (px - 70, SCREEN_HEIGHT - 108), 'topleft')
            for i in range(c.current_lives):
                pygame.draw.circle(scr, c.colors['main'], (int(px + 92 + i * 18), SCREEN_HEIGHT - 80), 6)
                pygame.draw.circle(scr, WHITE, (int(px + 92 + i * 18), SCREEN_HEIGHT - 80), 6, 2)
            bar = pygame.Rect(px - 70, SCREEN_HEIGHT - 38, 208, 14)
            pygame.draw.rect(scr, (20, 30, 50), bar, border_radius=7)
            fill = bar.copy()
            fill.w = int(bar.w * c.aether_gauge / AETHER_MAX)
            ray_ready = c.is_cryptic and c.aether_gauge >= AETHER_MAX
            acol = (255, 120, 120) if c.flash > 0 and c.aether_gauge < 30 else \
                lerp_color((70, 150, 255), (140, 255, 250), c.aether_gauge / AETHER_MAX)
            if ray_ready:
                acol = (255, 70, 70) if (self.frame // 6) % 2 else (255, 230, 120)
            if fill.w > 3:
                pygame.draw.rect(scr, acol, fill, border_radius=7)
            pygame.draw.rect(scr, WHITE, bar, 2, border_radius=7)
            self.text(scr, "DEATH RAY READY - press Special" if ray_ready else "AETHER", 16,
                      (255, 200, 200) if ray_ready else (170, 220, 255), (px - 70, SCREEN_HEIGHT - 52), 'topleft', False)
        mid = (SCREEN_WIDTH // 2, SCREEN_HEIGHT // 2 - 60)
        if self.intro > 40:
            self.text(scr, "READY...", 110, WHITE, mid)
        elif self.intro > 0:
            self.text(scr, "CLASH!", 150, (255, 220, 90), mid)
        if self.end_timer > 0:
            self.text(scr, "GAME!", 170, (255, 90, 90), mid)
        if self.duel:
            self._render_duel(heads)
        if self.paused:
            dim = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
            dim.fill((0, 0, 20, 185))
            scr.blit(dim, (0, 0))
            self.text(scr, "PAUSED", 90, WHITE, (SCREEN_WIDTH // 2, 70))
            self._render_controls(130)
            self.text(scr, "ENTER / ESC resume      Q quit to menu      F1 show hitboxes", 26, (200, 210, 255),
                      (SCREEN_WIDTH // 2, 580))

    def _render_duel(self, heads):
        scr, d = self.screen, self.duel
        dim = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
        dim.fill((30, 0, 10, 130))
        scr.blit(dim, (0, 0))
        self.text(scr, "DEATH RAY DUEL!", 96, (255, 90, 90), (SCREEN_WIDTH // 2, 80))
        self.text(scr, f"Two {SECRET_NAME}s, one gun. First to hit {DUEL_TARGET} keys fires it.", 28, WHITE,
                  (SCREEN_WIDTH // 2, 136))
        if d['intro'] > 0:
            self.text(scr, "GET READY", 70, (255, 220, 120), (SCREEN_WIDTH // 2, 220))
        for slot in (0, 1):
            ctrl, c = self.controllers[slot + 1], self.characters[slot + 1]
            hx, hy = heads.get(slot, (SCREEN_WIDTH * (0.3 + 0.4 * slot), 300))
            hx, hy = clamp(hx, 110, SCREEN_WIDTH - 110), clamp(hy - 124, 190, SCREEN_HEIGHT - 260)
            action = d['prompts'][slot]
            if isinstance(ctrl, AIController):
                label = dict(ACTIONS)[action].split(' /')[0].upper()
            else:
                label = pygame.key.name(ctrl.device.keymap[action][0]).upper()
            miss = d['miss'][slot] > 0
            cap = pygame.Rect(0, 0, max(84, self.font(54).size(label)[0] + 36), 76)
            cap.center = (hx, hy)
            if d['intro'] == 0:
                pygame.draw.rect(scr, (90, 20, 20) if miss else (245, 245, 250), cap, border_radius=12)
                pygame.draw.rect(scr, PLAYER_TAGS[slot], cap, 5, border_radius=12)
                self.text(scr, label, 54, (255, 120, 120) if miss else (20, 16, 40), cap.center, shadow=False)
            bar = pygame.Rect(hx - 90, cap.bottom + 8, 180, 14)
            pygame.draw.rect(scr, (30, 20, 30), bar, border_radius=7)
            pygame.draw.rect(scr, c.colors['accent'], (bar.x, bar.y, int(bar.w * d['counts'][slot] / DUEL_TARGET), bar.h),
                             border_radius=7)
            pygame.draw.rect(scr, WHITE, bar, 2, border_radius=7)
            self.text(scr, f"{d['counts'][slot]} / {DUEL_TARGET}", 24, WHITE, (hx, bar.bottom + 14))

    def _render_controls(self, y):
        scr, kms = self.screen, [self.slot_keymap(0), self.slot_keymap(1)]
        cpu = [isinstance(self.controllers.get(i + 1), AIController) for i in (0, 1)] if self.controllers else [False] * 2
        names = [self.characters[i + 1].player_name if self.characters else self.slot_name(i) for i in (0, 1)]
        self.text(scr, names[0], 26, PLAYER_TAGS[0], (560, y), 'topleft')
        self.text(scr, names[1], 26, PLAYER_TAGS[1], (860, y), 'topleft')
        for i, (a, label) in enumerate(ACTIONS):
            yy = y + 32 + i * 29
            self.text(scr, label, 24, (190, 200, 240), (240, yy), 'topleft')
            for slot, x in ((0, 560), (1, 860)):
                self.text(scr, "(CPU)" if cpu[slot] else self.key_names(kms[slot][a]), 24, WHITE, (x, yy), 'topleft')
        tips = ["Hold a direction with Attack / Heavy / Special for a different move. Tap Jump for a short hop.",
                "Shield just before a hit = PARRY (+25 Aether).  Shield + left/right = roll.  Shield in the air = air dodge.",
                "GRAB, then press a direction to THROW that way, or Attack to pommel first.",
                "Down attacks have a SPIKE spot: land it on an airborne rival to send them straight down."]
        for i, tip in enumerate(tips):
            self.text(scr, tip, 21, (220, 220, 240), (SCREEN_WIDTH // 2, y + 338 + i * 24), shadow=False)

    # ------------------------------------------------------------ menu screens
    def _render_title(self):
        scr, t = self.screen, self.frame
        for i, spec in enumerate(ROSTER):                            # the cast idles along the bottom
            x = (110, 310, 970, 1170)[i]
            spr = quick_sprite(spec, spec['colors'], 'idle', 1 if i < 2 else -1, t + i * 37)
            spr = pygame.transform.scale(spr, (SPR * 3 // 2, SPR * 3 // 2))
            scr.blit(spr, (x - SPR_CX * 3 // 2, 620 - SPR_FEET * 3 // 2))
            self.text(scr, spec['name'].upper(), 26, spec['colors']['accent'], (x, 644))
        scr.blit(self.logo, self.logo.get_rect(center=(SCREEN_WIDTH // 2, 110)))
        self.text(scr, "AETHER CLASH", 110, (255, 220, 120), (SCREEN_WIDTH // 2, 240))
        for i, opt in enumerate(self.TITLE_OPTIONS):
            sel = i == self.title_sel
            r = pygame.Rect(0, 0, 400, 54)
            r.center = (SCREEN_WIDTH // 2, 340 + i * 66)
            self.panel(r, (255, 220, 120) if sel else None, 220 if sel else 120)
            self.text(scr, opt + ("   (coming soon)" if opt == "Online" else ""), 40 if sel else 34,
                      (255, 220, 120) if sel else (200, 200, 230), r.center)
        self.text(scr, "W / S  or  UP / DOWN to choose      ENTER to select", 22, (170, 170, 220), (SCREEN_WIDTH // 2, 700),
                  shadow=False)

    def _render_online(self):
        scr = self.screen
        self.text(scr, "ONLINE PLAY", 96, (255, 220, 120), (SCREEN_WIDTH // 2, 90))
        self.text(scr, "Coming soon", 44, WHITE, (SCREEN_WIDTH // 2, 160))
        box = pygame.Rect(190, 220, 900, 330)
        self.panel(box, (96, 170, 255))
        lines = ["Online matches are not playable yet, but the groundwork is in place:",
                 "",
                 "   -  Every player's controls are already reduced to one tiny message per frame.",
                 "   -  The game runs in exact 1/60 second steps, so two computers stay in sync.",
                 "   -  A match relay server is included (aether_server.py) to pair two players.",
                 "",
                 f"Planned server address:  {DEFAULT_SERVER[0]} : {DEFAULT_SERVER[1]}        Status:  offline",
                 f"Network protocol version:  {NET_PROTOCOL_VERSION}"]
        for i, ln in enumerate(lines):
            self.text(scr, ln, 26, (225, 225, 245), (box.x + 30, box.y + 28 + i * 35), 'topleft', False)
        self.text(scr, "ESC  back", 28, (255, 220, 120), (SCREEN_WIDTH // 2, 620))

    def _render_profiles(self):
        scr, profs = self.screen, self.profiles.profiles
        self.text(scr, "PROFILES & CONTROLS", 76, (255, 220, 120), (SCREEN_WIDTH // 2, 60))
        self.text(scr, "A profile is your player name plus (optionally) your own key bindings. Pick it on the character screen.",
                  22, (200, 200, 235), (SCREEN_WIDTH // 2, 110), shadow=False)
        left = pygame.Rect(60, 140, 420, 470)
        self.panel(left, (255, 220, 120))
        if not profs:
            self.text(scr, "No profiles yet.", 30, WHITE, (left.centerx, left.y + 60))
            self.text(scr, "Press N to make one.", 26, (200, 200, 235), (left.centerx, left.y + 100))
        for i, p in enumerate(profs):
            sel = i == self.prof_cursor
            row = pygame.Rect(left.x + 14, left.y + 14 + i * 37, left.w - 28, 33)
            if sel:
                pygame.draw.rect(scr, (70, 50, 110), row, border_radius=8)
            self.text(scr, p['name'], 30, (255, 220, 120) if sel else WHITE, (row.x + 12, row.centery), 'midleft')
            self.text(scr, "custom keys" if p['keys'] else "default keys", 20, (170, 220, 255) if p['keys'] else (150, 150, 190),
                      (row.right - 12, row.centery), 'midright', False)
        right = pygame.Rect(520, 140, 700, 470)
        self.panel(right, (96, 170, 255))
        cur = profs[self.prof_cursor] if profs and self.prof_cursor < len(profs) else None
        self.text(scr, f"Keys for:  {cur['name']}" if cur else "Built-in default keys", 30, (255, 220, 120),
                  (right.x + 24, right.y + 16), 'topleft')
        show_defaults = not (cur and cur['keys']) and self.prof_mode != 'bind'
        self.text(scr, "as Player 1" if show_defaults else "", 22, PLAYER_TAGS[0], (right.x + 300, right.y + 56), 'topleft', False)
        self.text(scr, "as Player 2" if show_defaults else "", 22, PLAYER_TAGS[1], (right.x + 480, right.y + 56), 'topleft', False)
        for i, (a, label) in enumerate(ACTIONS):
            yy = right.y + 86 + i * 36
            binding = self.prof_mode == 'bind' and i == self.prof_bind_i
            self.text(scr, label, 26, (255, 220, 120) if binding else (200, 205, 240), (right.x + 24, yy), 'topleft', False)
            if self.prof_mode == 'bind':
                done = self.prof_new_keys.get(a)
                txt = self.key_names(done) if done else ("press a key..." if binding else "")
                self.text(scr, txt, 26, WHITE if done else (255, 220, 120), (right.x + 300, yy), 'topleft', False)
            elif cur and cur['keys']:
                self.text(scr, self.key_names(cur['keys'].get(a, [])), 26, WHITE, (right.x + 300, yy), 'topleft', False)
            else:
                self.text(scr, self.key_names(KEYMAP_P1[a]), 24, WHITE, (right.x + 300, yy), 'topleft', False)
                self.text(scr, self.key_names(KEYMAP_P2[a]), 24, WHITE, (right.x + 480, yy), 'topleft', False)
        if self.prof_mode == 'name':
            box = pygame.Rect(0, 0, 620, 150)
            box.center = (SCREEN_WIDTH // 2, 360)
            self.panel(box, (255, 220, 120), 250)
            self.text(scr, "Type your name (up to 12 letters), then press ENTER", 26, WHITE, (box.centerx, box.y + 34))
            cursor = "_" if (self.frame // 20) % 2 else " "
            self.text(scr, self.prof_text + cursor, 54, (255, 220, 120), (box.centerx, box.y + 96))
            help_ = "ENTER confirm      ESC cancel"
        elif self.prof_mode == 'ask':
            box = pygame.Rect(0, 0, 700, 130)
            box.center = (SCREEN_WIDTH // 2, 360)
            self.panel(box, (255, 220, 120), 250)
            self.text(scr, f"Set your own keys for {self.prof_edit['name']}?", 34, WHITE, (box.centerx, box.y + 40))
            self.text(scr, "Y = yes, choose my keys          N = no, use the default keys", 26, (255, 220, 120),
                      (box.centerx, box.y + 90))
            help_ = ""
        elif self.prof_mode == 'bind':
            help_ = f"Press the key you want for:  {ACTIONS[self.prof_bind_i][1].upper()}   ({self.prof_bind_i + 1} of {len(ACTIONS)})      ESC cancel"
        else:
            help_ = "N new profile     ENTER set keys     R rename     X reset to default keys     DELETE remove     ESC back"
        self.text(scr, help_, 24, (255, 220, 120), (SCREEN_WIDTH // 2, 650))

    def _render_menu(self):
        scr, t = self.screen, self.frame
        self.text(scr, "CHOOSE YOUR FIGHTER", 80, (255, 220, 120), (SCREEN_WIDTH // 2, 62))
        for slot in (0, 1):
            spec = ROSTER[self.sel[slot]]
            col = spec['alt'] if (slot == 1 and self.sel[0] == self.sel[1]) else spec['colors']
            x0 = 40 if slot == 0 else SCREEN_WIDTH - 480
            card = pygame.Rect(x0, 120, 440, 350)
            self.panel(card, PLAYER_TAGS[slot])
            cpu = self.p1_cpu if slot == 0 else self.p2_mode != 3
            who = ("CPU - Hard" if self.p1_cpu else "Human") if slot == 0 else self.P2_MODES[self.p2_mode]
            self.text(scr, f"PLAYER {slot + 1}  -  {who}", 26, PLAYER_TAGS[slot], (card.x + 18, card.y + 12), 'topleft')
            pose = 'run' if (t // 180) % 2 else 'idle'
            spr = quick_sprite(spec, col, pose, 1 if slot == 0 else -1, t)
            spr = pygame.transform.scale(spr, (SPR * 3 // 2, SPR * 3 // 2))
            scr.blit(spr, (card.x + 110 - SPR_CX * 3 // 2, card.y + 238 - SPR_FEET * 3 // 2))
            tx = card.x + 210
            self.text(scr, f"<  {spec['name'].upper()}  >", 46, WHITE, (tx, card.y + 46), 'topleft')
            self.text(scr, f"{spec['title']}  -  {spec['faction']}", 24, col['accent'], (tx, card.y + 86), 'topleft')
            for i, (label, val) in enumerate(zip(("Speed", "Power", "Weight", "Range"), spec['bars'])):
                yy = card.y + 120 + i * 23
                self.text(scr, label, 20, (190, 200, 240), (tx, yy), 'topleft', False)
                for k in range(5):
                    pygame.draw.rect(scr, col['main'] if k < val else (50, 46, 80), (tx + 70 + k * 24, yy + 1, 20, 12),
                                     border_radius=3)
            prof = self.slot_profile(slot)
            keys = "custom keys" if (prof and prof['keys']) else "default keys"
            self.text(scr, f"Profile:  <  {self.slot_name(slot, cpu)}  >", 26, (255, 220, 120), (tx, card.y + 216), 'topleft')
            self.text(scr, f"{keys}   -   change with {'Q / E' if slot == 0 else '[  /  ]'}", 18, (170, 170, 220),
                      (tx, card.y + 242), 'topleft', False)
            for i, ln in enumerate(spec['blurb']):
                self.text(scr, ln, 21, (220, 220, 240), (card.x + 18, card.y + 268 + i * 24), 'topleft', False)
        self.text(scr, "A / D  fighter", 24, PLAYER_TAGS[0], (260, 488))
        self.text(scr, "LEFT / RIGHT  fighter   -   TAB: human or CPU", 24, PLAYER_TAGS[1], (SCREEN_WIDTH - 260, 488))
        st = self.stages[self.stage_sel]
        self.text(scr, "VS", 70, WHITE, (SCREEN_WIDTH // 2, 160))
        thumb = self.thumbs[self.stage_sel]
        tr = thumb.get_rect(center=(SCREEN_WIDTH // 2, 320))
        scr.blit(thumb, tr)
        pygame.draw.rect(scr, (255, 220, 120), tr, 3, border_radius=4)
        self.text(scr, f"<  {st.name}  >", 30, (255, 220, 120), (SCREEN_WIDTH // 2, 218))
        self.text(scr, "W / S  change stage", 20, (190, 180, 240), (SCREEN_WIDTH // 2, 418))
        for i, ln in enumerate(st.blurb):
            self.text(scr, ln, 20, (220, 220, 240), (SCREEN_WIDTH // 2, 440 + i * 20), shadow=False)
        km = self.slot_keymap(0)
        n = lambda a: pygame.key.name(km[a][0]).upper()
        self.text(scr, f"P1:  {n('left')}/{n('right')} move   {n('jump')} jump   {n('attack')} attack   {n('heavy')} heavy   "
                       f"{n('special')} special   {n('shield')} shield   {n('grab')} grab", 23, WHITE, (SCREEN_WIDTH // 2, 540))
        self.text(scr, "Hold a direction with an attack button for a different move.   ESC in a match = pause + full controls.",
                  21, (190, 200, 240), (SCREEN_WIDTH // 2, 568))
        self.text(scr, "Knock your rival off the screen. The more damage (%) they have, the further they fly. 3 lives each.",
                  21, (190, 200, 240), (SCREEN_WIDTH // 2, 594))
        if (t // 30) % 2:
            self.text(scr, "PRESS ENTER TO CLASH", 44, (255, 220, 120), (SCREEN_WIDTH // 2, 650))
        self.text(scr, "F2: make Player 1 a CPU (watch mode)     ESC: back", 18, (150, 150, 200), (SCREEN_WIDTH // 2, 700),
                  shadow=False)

    def _render_result(self):
        scr = self.screen
        dim = pygame.Surface((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.SRCALPHA)
        dim.fill((0, 0, 20, 190))
        scr.blit(dim, (0, 0))
        w = self.winner
        if w:
            spr = quick_sprite(w.spec, w.colors, 'parry', 1, self.frame)
            scr.blit(pygame.transform.scale(spr, (SPR * 2, SPR * 2)), (SCREEN_WIDTH // 2 - SPR_CX * 2, 640 - SPR_FEET * 2))
            self.text(scr, f"{w.player_name} WINS!", 110, w.colors['accent'], (SCREEN_WIDTH // 2, 100))
            self.text(scr, f"{w.name}, {w.spec['title']}  -  {w.current_lives} lives left", 30, WHITE, (SCREEN_WIDTH // 2, 170))
        else:
            self.text(scr, "DRAW!", 110, WHITE, (SCREEN_WIDTH // 2, 100))
        self.text(scr, "ENTER: rematch        ESC: character select", 32, (255, 220, 120), (SCREEN_WIDTH // 2, 680))

    def stop_game(self):
        self.running = False


# =====================================================================
# MAIN EXECUTION BLOCK
# =====================================================================

def run_selftest(frames=3600):
    """Headless smoke test: CPU vs CPU on every stage with every character, plus the secret duel."""
    random.seed(7)
    game = GameManager(headless=True, profile_path=os.devnull)
    n = len(ROSTER)
    for i in range(n * len(game.stages) + 1):
        a, b, s = i % n, (i + 1 + i // n) % n, i % len(game.stages)
        secret = i == n * len(game.stages)
        game.setup_game(a, b, s, p2_mode=2, p1_cpu=True, names=[SECRET_NAME] * 2 if secret else None)
        f, duels = 0, 0
        while game.scene == 'FIGHT' and f < frames:
            was = game.duel is not None
            game._step()
            duels += int(game.duel is not None and not was)
            f += 1
            if f % 240 == 0 or (secret and f % 7 == 0):
                game._render_frame()
        c1, c2 = game.characters[1], game.characters[2]
        print(f"{game.map_data.name:20s} {c1.name:5s} vs {c2.name:5s}: {f:5d} frames | lives {c1.current_lives}-{c2.current_lives}"
              f" | dmg {c1.cumulative_damage_taken:5.0f}% {c2.cumulative_damage_taken:5.0f}%"
              + (f" | death ray duels: {duels}" if secret else ""))
    for scene in ('TITLE', 'ONLINE', 'PROFILES', 'MENU'):
        game.scene = scene
        game._render_frame()
    print("SELFTEST OK")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        run_selftest()
    elif "--make-icon" in sys.argv:                                   # used by the release build for the .exe icon
        os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
        pygame.init()
        pygame.display.set_mode((64, 64))
        pygame.image.save(make_icon(256), sys.argv[sys.argv.index("--make-icon") + 1])
    else:
        GameManager(demo="--demo" in sys.argv).run_game_loop()
