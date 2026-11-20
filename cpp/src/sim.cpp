// Effects, the PhysicsEngine and the CombatManager - a direct port of the Python version.
#include "core.h"

Color lerpColor(Color a, Color b, double t) {
    t = clampv(t, 0.0, 1.0);
    return Color(int(a.r + (b.r - a.r) * t), int(a.g + (b.g - a.g) * t), int(a.b + (b.b - a.b) * t));
}

Rng& fxRng() { static Rng r(0xAE7E5); return r; }
Rng& aiRng() { static Rng r(0xC0FFEE); return r; }

// ===================================================================== fighters & projectiles
Fighter::Fighter(const FighterSpec& sp, int slot_, const Palette& cols)
    : name(sp.name), faction(sp.faction), playerName(sp.name), spec(&sp), slot(slot_), id(slot_), colors(cols) {
    const SmashStats& sm = sp.stats.smash;
    const double u = SMASH_UNIT;
    walkSpeed = sm.walk * u; initDash = sm.initialDash * u; dashFrames = sm.dashFrames;
    speed = sm.run * u; runAccel = sm.runAccel * u; traction = sm.traction * u;
    airSpeed = sm.airSpeed * u; airAccel = sm.airAccel * u; airFriction = sm.airFriction * u;
    gravity = sm.gravity * u; maxFall = sm.fall * u;
    jumpSquat = sm.jumpSquat; heavyLanding = sm.heavyLanding;
    double g = gravity;
    auto launch = [g](double height) { return std::sqrt(2 * g * height) + g / 2; };   // sqrt(2 g h) + g / 2
    fullHopH = sm.fullHop * u;
    hopBoostH = fullHopH * FULL_HOP_FRACTION;
    jumpHeight = launch(fullHopH);
    double rest = fullHopH - hopBoostH;          // the speed that rises exactly `rest` more, frame by frame
    afterBoost = -g / 2 + std::sqrt(g * g / 4 + 2 * g * rest);
    shortHop = launch(sm.shortHop * u);
    doubleJump = launch(sm.doubleJump * u);
    vscale = std::sqrt(gravity / sp.stats.gravity);
    weight = sp.stats.weight;
    airJumps = sp.stats.airJumps;
    w = sp.stats.w; h = sp.stats.h;
    moveSet = &sp.moves;
    reset(0, 0, 1);
}

void Fighter::reset(double x_, double y_, int facing_) {
    currentLives = maxLives;
    damage = 0;
    aether = AETHER_START;
    kos = 0;
    respawnAt(x_, y_, facing_, true);
}

void Fighter::respawnAt(double x_, double y_, int facing_, bool fresh) {
    x = x_; y = y_;
    vx = vy = kbx = kby = 0;
    facing = facing_;
    ground = nullptr;
    state = fresh ? "IDLE" : "RESPAWN";
    knockedDown = false;
    move = nullptr; moveFrame = 0; hitLog.clear(); moveFromAir = false;
    jumpsLeft = airJumps;
    moveX = inX = inY = 0;
    aimX = 0; aimY = -1; visAimX = facing; visAimY = 0;
    hitstop = hitstun = stunTimer = lag = 0;
    frozen = false;
    shieldHp = SHIELD_MAX; shieldFrame = 0; shieldStun = 0; shieldHeld = false;
    intangible = 0;
    dodgeTimer = 0; airDodged = false;
    fastFall = jumpCut = false;
    dropTimer = ledgeCd = ledgeTimer = 0;
    ledge = Ledge();
    stateTimer = buffTimer = respawnTimer = runFrames = 0;
    dashT = 0; dashDir = facing; squat = 0; jumpHeld = false;
    turnT = hopT = 0; kbVertical = false;
    flash = 0;
    anim = fxRng().random() * 100;
    grabTarget = grabbedBy = nullptr;
    grabTimer = grabFrames = pommelCd = pommelAnim = 0;
    pose.reset(); cloth.reset(); vis.reset();
    squash = 0;
}

bool Fighter::isCryptic() const {
    size_t a = playerName.find_first_not_of(" \t\r\n"), b = playerName.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    std::string s = playerName.substr(a, b - a + 1);
    for (auto& ch : s) ch = char(std::tolower((unsigned char)ch));
    return s == SECRET_NAME;
}

Projectile::Projectile(Fighter* o, const ProjectileSpec& s, double x_, double y_, int facing_)
    : owner(o), facing(facing_), x(x_), y(y_), w(s.w), h(s.h), vx(s.vx * facing_), vy(s.vy), gravity(s.gravity),
      life(s.life), kind(s.kind), rehit(s.rehit), sturdy(s.sturdy), ghost(s.ghost), grounded(s.grounded) {
    HbFlags f; f.pull = s.pull;
    hit = hb(0, 0, 0, 0, w, h, s.damage, s.force, s.angle, f);
    color = o->colors.accent;
}

// ===================================================================== effects
void Effects::spark(double x, double y, Color c, int n, double speed, int life, int size, double grav,
                    std::optional<double> direction, double spread, PShape shape) {
    Rng& r = fxRng();
    for (int i = 0; i < n; ++i) {
        double a = direction.value_or(0) + r.uniform(-spread, spread);
        double s = speed * r.uniform(0.3, 1.0);
        particles.push_back({x, y, std::cos(a) * s, std::sin(a) * s, r.randint(life / 2, life), life, c, size, grav, shape});
    }
}

void Effects::styleBurst(Style style, double x, double y, const Palette& colors, int n, double power) {
    switch (style) {
        case Style::Kindled:
            spark(x, y, Color(255, 140, 40), n, power * 0.7, 50, 4, 0.1);
            spark(x, y, colors.accent, std::max(1, n / 2), power * 0.5, 40, 3, 0.06);
            break;
        case Style::Weaver: spark(x, y, colors.accent, n, power, 34, 3, 0.05, std::nullopt, PI, PShape::Line); break;
        case Style::Sentinel: spark(x, y, Color(150, 156, 172), n, power, 40, 5, 0.45, std::nullopt, PI, PShape::Square); break;
        default: spark(x, y, colors.accent, n, power * 0.6, 52, 5, -0.03, std::nullopt, PI, PShape::Star); break;
    }
}

void Effects::ring(double x, double y, Color c, double r, double grow, int life) { rings.push_back({x, y, r, grow, life, life, c}); }
void Effects::text(double x, double y, const std::string& s, Color c, int life) { texts.push_back({x, y, s, c, life}); }

void Effects::update() {
    for (auto& p : particles) {
        p.x += p.vx; p.y += p.vy; p.vy += p.grav; p.vx *= 0.94; p.vy *= 0.94; p.life -= 1;
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life <= 0; }), particles.end());
    if (particles.size() > 600) particles.erase(particles.begin(), particles.end() - 600);
    for (auto& r : rings) { r.r += r.grow; r.life -= 1; }
    rings.erase(std::remove_if(rings.begin(), rings.end(), [](const Ring& r) { return r.life <= 0; }), rings.end());
    for (auto& t : texts) { t.y -= 0.8; t.life -= 1; }
    texts.erase(std::remove_if(texts.begin(), texts.end(), [](const PopText& t) { return t.life <= 0; }), texts.end());
    shake = std::max(0, shake - 1);
}

void Effects::draw(Surface& surf, const Font& font) const {
    for (auto& p : particles) {
        int r = std::max(1, int(double(p.size) * p.life / p.maxLife));
        switch (p.shape) {
            case PShape::Square: draw::rect(surf, p.color, Rect(int(p.x) - r, int(p.y) - r, r * 2, r * 2)); break;
            case PShape::Line: draw::line(surf, p.color, {p.x, p.y}, {p.x - p.vx * 2.5 - 3, p.y - p.vy * 2.5}, 2); break;
            case PShape::Star:
                draw::line(surf, p.color, {p.x - r, p.y}, {p.x + r, p.y}, 2);
                draw::line(surf, p.color, {p.x, p.y - r}, {p.x, p.y + r}, 2);
                break;
            default: draw::circle(surf, p.color, {double(int(p.x)), double(int(p.y))}, r);
        }
    }
    for (auto& r : rings)
        draw::circle(surf, r.color, {double(int(r.x)), double(int(r.y))}, int(r.r), std::max(1, int(6.0 * r.life / r.maxLife)));
    for (auto& t : texts) {
        auto img = font.render(t.s, t.color);
        surf.blit(*img, t.x - img->w / 2.0, t.y);
    }
}

// ===================================================================== physics
void PhysicsEngine::updateAll() {
    frame += 1;
    for (auto& p : map->platforms) p.update(frame);
    for (Fighter* cp : fighters) {
        Fighter& c = *cp;
        c.frozen = c.hitstop > 0;
        if (c.frozen) { c.hitstop -= 1; continue; }           // "hitstop": both fighters freeze briefly on impact
        if (c.state == "KO" || c.state == "RESPAWN" || c.state == "LEDGE") continue;
        if (c.state == "GRABBED") {                             // held in place in front of the grabber
            if (Fighter* g = c.grabbedBy) {
                c.x = g->x + g->facing * (g->w / 2.0 + c.w / 2.0 - 6);
                c.y = g->y;
                c.facing = -g->facing; c.ground = g->ground;
                c.vx = c.vy = c.kbx = c.kby = 0;
            }
            continue;
        }
        applyMoveEffects(c);
        applyGravity(c);
        applyMovement(c);
        checkPlatformCollision(c);
        checkLedges(c);
        syncGroundState(c);
        if (!map->blast.collidepoint(c.x, c.y - c.h / 2.0)) combat->handleKo(c);
    }
    updateProjectiles();
    updateActiveHitboxes();
}

void PhysicsEngine::applyMoveEffects(Fighter& c) {
    const Move* m = c.move;
    int f = c.moveFrame;
    if (!m) return;
    auto it = m->impulses.find(f);
    if (it != m->impulses.end()) {
        if (it->second.hasX) c.vx = it->second.x * c.facing;
        if (it->second.hasY) {
            c.vy = it->second.y * c.vscale;
            if (it->second.y < 0) c.ground = nullptr;
        }
    }
    if (m->aim.on) {
        if (m->aim.a <= f && f <= m->aim.b) {
            c.vx = c.aimX * m->aim.speed; c.vy = c.aimY * m->aim.speed;
            if (c.vy < 0) c.ground = nullptr;
            if (f == m->aim.a) fx->ring(c.x, c.y - c.h / 2.0, c.colors.accent, 10, 8);
        } else if (f == m->aim.b + 1) {
            c.vx *= 0.1; c.vy = 0;
            fx->ring(c.x, c.y - c.h / 2.0, c.colors.accent, 40, -3);
        }
    }
    if (m->intangible.has(f)) c.intangible = std::max(c.intangible, 2);
    if (m->projectile.on && f == m->projectile.frame) {
        const auto& ps = m->projectile;
        projectiles.push_back(std::make_unique<Projectile>(&c, ps, c.x + ps.x * c.facing, c.y - c.h / 2.0 + ps.y, c.facing));
    }
    if (m->slot == "dashheavy" && !m->hitboxes.empty() && f < m->hitboxes[0].start) {
        double fx_ = c.x + c.facing * c.w * 0.6, fy_ = c.y - c.h * 0.55;      // power gathering: the tell to watch for
        if (f % 3 == 0) fx->spark(fx_, fy_, c.colors.accent, 2, 4, 14, 4, 0, c.facing > 0 ? PI : 0.0, 1.2);
        if (f == 2) fx->ring(fx_, fy_, c.colors.accent, 34, -2, 16);
    }
    if (m->effect == "deathray") {
        if (f < m->effectFrame) fx->shake = std::max(fx->shake, 2 + f / 9);
        else if (f == m->effectFrame) {
            fx->shake = 36;
            combat->nukeBy = &c;                                 // the match ends here (see the Game)
        }
    }
    if (m->effect == "ignite" && f == m->effectFrame) {
        c.buffTimer = 480;
        fx->ring(c.x, c.y - c.h / 2.0, c.colors.accent, 20, 9, 20);
        fx->text(c.x, c.y - c.h - 40, "IGNITED!", c.colors.accent);
    }
}

void PhysicsEngine::applyGravity(Fighter& c) {
    if (c.ground) return;
    const Move* m = c.move;
    if (m && m->noGravity.has(c.moveFrame)) return;
    if (c.hopT > 0) {                                     // the sped-up start of a full hop
        c.hopT -= 1;
        c.vy = -c.hopBoostH / FULL_HOP_FRAMES;
        if (c.hopT == 0) c.vy = -c.afterBoost;            // the rest of the jump rises as normal
        return;
    }
    double g = c.gravity * (c.hitstun > 0 ? HITSTUN_GRAVITY : 1.0);
    double cap = c.maxFall * (c.fastFall ? FAST_FALL_MULT : 1.0);
    if (c.hitstun > 0 && c.kbVertical) cap = VERTICAL_LAUNCH_FALL;
    c.vy = std::min(c.vy + g, std::max(cap, c.vy));
    if (c.fastFall && c.vy > 0) c.vy = std::max(c.vy, cap);
}

void PhysicsEngine::applyMovement(Fighter& c) {
    const std::string& st = c.state;
    const Move* m = c.move;
    bool scripted = m && m->noGravity.has(c.moveFrame);
    if (c.ground) {
        if ((st == "IDLE" || st == "RUNNING" || st == "PARRY_SUCCESS") && c.lag == 0) groundMove(c);
        else if (st == "DODGE") c.vx *= 0.93;
        else if (!scripted) {                             // attacking, shielding, jump squat, landing: slide to a stop
            bool plant = m && m->slot == "dashheavy" && !m->hitboxes.empty() && c.moveFrame < m->hitboxes[0].start - 2;
            c.vx = approach(c.vx, 0.0, c.traction * (plant ? 3.0 : 1.0));   // a dash attack keeps its run speed and slides
            c.dashT = c.turnT = 0;
        }
    } else if (!scripted) {
        if ((st == "JUMPING" || st == "HELPLESS" || st == "ATTACKING") && c.moveX) {
            double nv = c.vx + c.moveX * c.airAccel;
            if (std::abs(nv) > c.airSpeed)                 // at top speed (or coasting faster): no more speed
                nv = sgn(nv) * (std::abs(c.vx) > c.airSpeed ? std::max(std::abs(c.vx) - c.airFriction, c.airSpeed) : c.airSpeed);
            c.vx = nv;
        } else {
            c.vx = approach(c.vx, 0.0, c.airFriction);
        }
    }
    double sp = std::hypot(c.kbx, c.kby);                // knockback decays at a constant rate
    if (sp > 0) {
        double ns = std::max(0.0, sp - KB_DECAY * (c.ground ? 2.0 : 1.0));
        c.kbx = c.kbx * ns / sp; c.kby = c.kby * ns / sp;
    }
}

void PhysicsEngine::groundMove(Fighter& c) {
    int d = c.moveX;
    if (d == 0) {
        c.dashT = c.turnT = 0;
        c.vx = approach(c.vx, 0.0, c.traction);
        return;
    }
    if (c.turnT > 0) {                                    // turnaround dash: a short wait before accelerating
        c.turnT -= 1;
        c.vx = approach(c.vx, 0.0, c.initDash / TURN_DASH_DELAY);
        if (c.turnT == 0) { c.dashDir = d; c.dashT = c.dashFrames; c.vx = d * c.initDash; }
        return;
    }
    if (c.dashT > 0) {                                    // in the initial dash
        if (d != c.dashDir) { c.turnT = TURN_DASH_DELAY; c.dashT = 0; return; }   // reversed: a dash dance
        c.dashT -= 1;
        c.vx = d * c.initDash;
        return;
    }
    if (c.vx * d < 0 && std::abs(c.vx) > c.walkSpeed * 0.5) { c.vx = approach(c.vx, 0.0, c.traction * 2); return; }
    if (std::abs(c.vx) < c.walkSpeed * 0.5) {             // from (nearly) standing: dash
        c.dashDir = d; c.dashT = c.dashFrames; c.vx = d * c.initDash;
        return;
    }
    if (std::abs(c.vx) < c.speed) c.vx = approach(c.vx, d * c.speed, c.runAccel);
    else c.vx = approach(c.vx, d * c.speed, c.traction);
}

void PhysicsEngine::launchJump(Fighter& c) {
    if (c.jumpHeld) {                                     // full hop, with Ultimate's sped-up start
        c.hopT = FULL_HOP_FRAMES + 1;                     // (+1: the take-off happens at the end of a frame)
        c.vy = -c.hopBoostH / FULL_HOP_FRAMES;
    } else {
        c.vy = -c.shortHop;
    }
    double top = c.airSpeed * 1.15;
    c.vx = clampv(c.vx * 0.85 + c.moveX * c.airSpeed * 0.25, -top, top);
    c.ground = nullptr; c.fastFall = false; c.dashT = 0;
    c.squash = 0.2;
    c.state = "JUMPING";
}

static bool overlaps(const Fighter& c, const Platform& p) {
    return c.x - c.w / 2.0 < p.x + p.width && c.x + c.w / 2.0 > p.x && c.y - c.h < p.y + p.height && c.y > p.y;
}

void PhysicsEngine::checkPlatformCollision(Fighter& c) {
    double dx = c.vx + c.kbx, dy = c.vy + c.kby;
    double half = c.w / 2.0, margin = c.w * 0.3;
    if (c.ground) {                                       // ride the platform we stand on
        Platform* g = c.ground;
        c.x += g->dx;
        c.y = g->y;
        if (dy < 0) c.ground = nullptr;
        else {
            dy = 0; c.vy = 0; c.kby = std::min(c.kby, 0.0);
            if (!(g->x - margin <= c.x + dx && c.x + dx <= g->x + g->width + margin)) {
                c.ground = nullptr;                       // walked off the edge...
                for (auto& p : map->platforms)            // ...unless another platform continues at this height
                    if (&p != g && std::abs(p.y - c.y) < 2 && p.x - margin <= c.x + dx && c.x + dx <= p.x + p.width + margin)
                        c.ground = &p;
            }
        }
    }
    double prevY = c.y;
    c.x += dx;                                            // horizontal step: solid platforms act as walls
    for (auto& p : map->platforms) {
        if (p.solid && overlaps(c, p) && c.y > p.y + 8) {
            double prevX = c.x - dx;
            bool fromLeft = prevX <= p.x ? true : (prevX >= p.x + p.width ? false : c.x < p.x + p.width / 2.0);
            c.x = fromLeft ? p.x - half : p.x + p.width + half;
            c.vx = 0;
            c.kbx *= -0.4;
        }
    }
    c.y += dy;                                            // vertical step
    if (c.ground) return;
    if (dy < 0) {
        for (auto& p : map->platforms)                    // bump head on the underside of solid platforms
            if (p.solid && overlaps(c, p)) { c.y = p.y + p.height + c.h; c.vy = 0; c.kby = 0; }
        return;
    }
    for (auto& p : map->platforms) {                      // landing: feet crossed the top surface this frame
        if (!p.solid && c.dropTimer > 0) continue;
        if (prevY <= p.prevTop + 1 && c.y >= p.y && p.x - margin <= c.x && c.x <= p.x + p.width + margin) {
            land(c, p);
            break;
        }
    }
}

void PhysicsEngine::land(Fighter& c, Platform& p) {
    c.squash = -std::min(0.24, 0.06 + std::abs(c.vy) * 0.014);
    bool hard = c.fastFall || c.vy >= c.maxFall * 0.95;
    c.y = p.y; c.vy = 0; c.ground = &p;
    c.kby = 0;
    c.jumpsLeft = c.airJumps;
    c.airDodged = c.fastFall = c.jumpCut = false;
    c.ledgeCd = 0;
    if (std::abs(c.vx + c.kbx) > 3 || c.state != "JUMPING")
        fx->spark(c.x, c.y, Color(220, 220, 220), 5, 3, 12, 3, 0, -PI / 2, 1.3);
    const Move* m = c.move;
    if (m && c.moveFromAir && (m->type == "Attack" || m->helpless)) {
        combat->endMove(c);                               // aerials are cancelled by landing (with landing lag)
        c.lag = m->type == "Attack" ? m->landingLag : 10;
    } else if (c.state == "HELPLESS") {
        c.state = "IDLE"; c.lag = 8;
    } else if (c.state == "DODGE") {
        c.dodgeTimer = std::min(c.dodgeTimer, 8);
    } else if (c.state == "JUMPING" && c.hitstun == 0) {
        c.lag = hard ? c.heavyLanding : LIGHT_LANDING;
    }
}

void PhysicsEngine::syncGroundState(Fighter& c) {
    const std::string& st = c.state;
    if (!c.ground && (st == "IDLE" || st == "RUNNING" || st == "SHIELD" || st == "JUMPSQUAT")) c.state = "JUMPING";
    else if (c.ground && st == "JUMPING") c.state = "IDLE";
}

void PhysicsEngine::checkLedges(Fighter& c) {
    if (c.ground || c.ledgeCd > 0 || c.hitstun > 0 || c.inY == 1 || c.vy + c.kby <= 0) return;
    const Move* m = c.move;
    if (!(c.state == "JUMPING" || c.state == "HELPLESS" || (m && m->helpless))) return;
    double top = c.y - c.h;
    for (auto& p : map->platforms) {
        if (!p.ledges) continue;
        for (auto [lx, inward] : {std::pair<double, int>{p.x, 1}, std::pair<double, int>{p.x + p.width, -1}}) {
            double gap = (lx - c.x) * inward;             // how far outside the edge we are
            if (0 <= gap && gap <= c.w / 2.0 + 28 && -26 <= top - p.y && top - p.y <= 46) {
                if (m) combat->endMove(c);
                c.state = "LEDGE";
                c.ledge = Ledge{true, &p, lx, inward};
                c.x = lx - inward * (c.w / 2.0 + 2); c.y = p.y + c.h - 8;
                c.vx = c.vy = c.kbx = c.kby = 0;
                c.facing = inward;
                c.ledgeTimer = 0;
                c.jumpsLeft = c.airJumps;
                c.airDodged = c.fastFall = false;
                c.intangible = std::max(c.intangible, 20);
                return;
            }
        }
    }
}

void PhysicsEngine::updateProjectiles() {
    for (auto& pp : projectiles) {
        Projectile& p = *pp;
        p.age += 1; p.life -= 1;
        p.vy += p.gravity;
        double prevBottom = p.y + p.h / 2;
        p.x += p.vx; p.y += p.vy;
        if (p.grounded && p.vy > 0)
            for (auto& pl : map->platforms)
                if (prevBottom <= pl.y + 1 && p.y + p.h / 2 >= pl.y && pl.x <= p.x && p.x <= pl.x + pl.width) {
                    p.y = pl.y - p.h / 2; p.vy = 0; p.gravity = 0;
                    fx->spark(p.x, pl.y, Color(200, 200, 210), 8, 5, 14, 4, 0, -PI / 2, 1.4);
                }
        if (p.life <= 0 || !map->blast.collidepoint(p.x, p.y)) p.dead = true;
    }
    std::vector<Projectile*> live;
    for (auto& p : projectiles) if (!p->dead) live.push_back(p.get());
    for (size_t i = 0; i < live.size(); ++i)              // projectiles of different owners cancel each other
        for (size_t j = i + 1; j < live.size(); ++j) {
            Projectile* a = live[i]; Projectile* b = live[j];
            if (a->owner != b->owner && !a->ghost && !b->ghost && a->rect().colliderect(b->rect()))
                for (Projectile* q : {a, b})
                    if (!q->sturdy) { q->dead = true; fx->spark(q->x, q->y, q->color, 8, 5); }
        }
    for (Projectile* p : live) {
        if (p->dead) continue;
        for (Fighter* t : fighters) {
            if (t == p->owner || !t->alive()) continue;
            auto it = p->hitLog.find(t->id);
            if (it != p->hitLog.end() && (p->rehit == 0 || p->age - it->second < p->rehit)) continue;
            if (p->rect().colliderect(t->rect())) {
                p->hitLog[t->id] = p->age;
                std::string r = combat->handleCollision(*p->owner, p->hit, *t, p);
                if (r != "ignored" && r != "reflect" && !p->rehit) p->dead = true;
            }
        }
    }
    projectiles.erase(std::remove_if(projectiles.begin(), projectiles.end(), [](const auto& p) { return p->dead; }), projectiles.end());
}

std::vector<std::array<double, 3>> PhysicsEngine::hitboxCircles(const Fighter& c, const Hitbox& box) {
    std::vector<std::array<double, 3>> out;
    for (auto& ci : box.circles) out.push_back({c.x + (box.x + ci.dx) * c.facing, c.y - c.h / 2.0 + box.y + ci.dy, ci.r});
    return out;
}

Rect PhysicsEngine::hitboxRect(const Fighter& c, const Hitbox& box) const {
    auto cs = hitboxCircles(c, box);
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (auto& [x, y, r] : cs) { x0 = std::min(x0, x - r); x1 = std::max(x1, x + r); y0 = std::min(y0, y - r); y1 = std::max(y1, y + r); }
    return Rect(int(x0), int(y0), std::max(1, int(x1 - x0)), std::max(1, int(y1 - y0)));
}

std::vector<const Hitbox*> PhysicsEngine::activeHitboxes(const Fighter& c) const {
    std::vector<const Hitbox*> out;
    if (!c.move || c.state != "ATTACKING") return out;
    for (auto& b : c.move->hitboxes)
        if (b.start <= c.moveFrame && c.moveFrame <= b.end) out.push_back(&b);
    return out;
}

static bool circleHitsRect(double cx, double cy, double r, const Rect& rc) {
    double nx = clampv(cx, double(rc.left()), double(rc.right())), ny = clampv(cy, double(rc.top()), double(rc.bottom()));
    return (cx - nx) * (cx - nx) + (cy - ny) * (cy - ny) <= r * r;
}

void PhysicsEngine::updateActiveHitboxes() {
    struct Ev { Fighter* a; const Hitbox* b; Fighter* t; };
    std::vector<Ev> events;
    for (Fighter* a : fighters) {
        if (a->frozen) continue;
        for (const Hitbox* box : activeHitboxes(*a)) {
            Rect rc = hitboxRect(*a, *box);
            if (frame % 3 == 0 && !box->spike && rc.w < 400)  // weapons shed particles
                fx->styleBurst(a->style(), rc.centerx(), rc.centery(), a->colors, 1, 2.5);
            for (Fighter* t : fighters) {
                auto key = std::make_pair(t->id, box->gid);
                if (t == a || !t->alive() || a->hitLog.count(key)) continue;
                Rect body = t->rect();
                bool hit = false;
                for (auto& [x, y, r] : hitboxCircles(*a, *box)) hit = hit || circleHitsRect(x, y, r, body);
                if (hit) { a->hitLog.insert(key); events.push_back({a, box, t}); }
            }
        }
    }
    for (auto& e : events) combat->handleCollision(*e.a, *e.b, *e.t);   // resolved together so two attacks can trade
}

double PhysicsEngine::applyKnockback(Fighter&, Fighter& target, double baseForce, double dx, double dy) {
    double modifier = 1.0 + target.damage / KNOCKBACK_DIVISOR;        // CORE MECHANIC
    double finalForce = baseForce * modifier;
    double speed = finalForce * 200.0 / (100.0 + target.weight) * LAUNCH_SCALE;
    double deg = pymod(std::atan2(-dy, dx != 0 ? dx : 1e-9) * 180.0 / PI, 360.0);
    target.kbVertical = 70 <= deg && deg <= 110;
    if (target.ground && dy > 0.2) dy = -dy * 0.8;      // spiked into the floor: bounce back up
    target.vx = target.vy = 0;
    target.kbx = speed * dx; target.kby = speed * dy;
    if (target.kby < 0) target.ground = nullptr;
    return speed;
}

// ===================================================================== combat
bool CombatManager::processAttack(Fighter& a, const Move& m) {
    if (a.aether < m.requiredAether) {
        a.flash = 20;
        fx->text(a.x, a.y - a.h - 36, "NO AETHER", Color(150, 220, 255), 30);
        return false;
    }
    a.aether -= m.requiredAether;
    a.move = &m; a.moveFrame = 0; a.hitLog.clear();
    a.moveFromAir = a.ground == nullptr;
    a.state = "ATTACKING";
    a.fastFall = false;
    double n = std::hypot(a.inX, a.inY);
    if (n) { a.aimX = a.inX / n; a.aimY = a.inY / n; } else { a.aimX = 0; a.aimY = -1; }
    if (!m.hitboxes.empty()) {
        double bx = m.hitboxes[0].x * a.facing, by = m.hitboxes[0].y;
        double nn = std::hypot(bx, by);
        if (nn > 8) { a.visAimX = bx / nn; a.visAimY = by / nn; } else { a.visAimX = a.facing; a.visAimY = 0; }
    } else {
        a.visAimX = a.facing; a.visAimY = 0;
    }
    return true;
}

void CombatManager::endMove(Fighter& c) {
    const Move* m = c.move;
    c.move = nullptr;
    if (c.state == "ATTACKING") {
        if (!c.ground) c.state = (m && m->helpless) ? "HELPLESS" : "JUMPING";
        else c.state = "IDLE";
    }
}

std::string CombatManager::handleCollision(Fighter& attacker, const Hitbox& box, Fighter& target, Projectile* source) {
    if (target.intangible > 0 || target.state == "KO" || target.state == "RESPAWN") return "ignored";
    const Move* tm = target.move;
    Vec2 tc = target.center();
    double tx = tc.x, ty = tc.y;
    double damage = box.damage * (attacker.buffTimer > 0 ? IGNITE_MULT : 1.0);
    if (target.state == "GRABBED") return "ignored";
    if (!box.grab && !box.unblockable) {
        if (tm && tm->counter.has(target.moveFrame)) return handleParry(target, attacker, box, source, true);
        if (target.state == "SHIELD") {
            if (target.shieldFrame <= PARRY_WINDOW) return handleParry(target, attacker, box, source);
            target.shieldHp -= damage * 1.1 + 1;
            target.shieldStun = int(damage * 0.5) + 3;
            target.vx += sgn(tx - attacker.x) * 3;
            if (!source) attacker.hitstop = 4;
            fx->ring(tx, ty, WHITE, 30, 3, 8);
            if (target.shieldHp <= 0) shieldBreak(target);
            return "shield";
        }
    }
    double force = box.force;
    bool spiked = false;
    double dx, dy;
    if (box.grab) {                                      // grabs hold the target; the throw comes later
        if (target.state == "HIT" || target.state == "LEDGE" || target.state == "GRABBING") return "ignored";
        return startGrab(attacker, target);
    }
    if (box.spike && !target.ground) { dx = 0; dy = 1; spiked = true; }
    else if (box.pull && source) {
        double vx = source->x - tx, vy = source->y - ty, n = std::hypot(vx, vy);
        if (n > 12) { dx = vx / n; dy = vy / n; } else { dx = 0; dy = -1; }
    } else if (box.radial) {
        double ox, oy;
        if (source) { ox = source->x; oy = source->y; } else { Vec2 ac = attacker.center(); ox = ac.x; oy = ac.y; }
        double vx = tx - ox, vy = (ty - oy) - 40, n = std::hypot(vx, vy);
        if (n == 0) n = 1;
        dx = vx / n; dy = vy / n;
    } else {
        int facing = source ? source->facing : attacker.facing;
        double a = radians(box.angle);
        dx = std::cos(a) * facing; dy = -std::sin(a);
    }
    target.damage += damage;
    attacker.aether = std::min(AETHER_MAX, attacker.aether + damage * AETHER_PER_DAMAGE_DEALT);
    target.aether = std::min(AETHER_MAX, target.aether + damage * AETHER_PER_DAMAGE_TAKEN);
    int stop = std::min(14, int(3 + damage * 0.45));
    target.hitstop = stop;
    if (!source) attacker.hitstop = stop;
    fx->spark(tx, ty, attacker.colors.accent, 6 + int(damage), 4 + damage * 0.4, 20, 5);
    fx->spark(tx, ty, WHITE, 4, 9, 10, 3);
    fx->styleBurst(target.style(), tx, ty, target.colors, 4 + int(damage * 0.4), 4 + damage * 0.2);
    fx->shake = std::max(fx->shake, int(damage * 0.5));
    if (tm && tm->armor.has(target.moveFrame) && !box.grab && damage < ARMOR_LIMIT) {   // super armor
        target.flash = 8;
        return "hit";
    }
    double speed = physics->applyKnockback(attacker, target, force, dx, dy);
    enterHitState(target, speed);
    if (speed > 16) fx->ring(tx, ty, attacker.colors.accent, 20, 10, 16);
    if (spiked) {
        fx->text(tx, ty - target.h, "SPIKE!", Color(255, 120, 255), 50);
        fx->ring(tx, ty, Color(255, 120, 255), 14, 8, 16);
        fx->spark(tx, ty, Color(255, 160, 255), 14, 12, 22, 5, 0, PI / 2, 0.5);
    }
    return "hit";
}

std::string CombatManager::startGrab(Fighter& a, Fighter& t) {
    a.move = nullptr;
    a.state = "GRABBING";
    a.grabTarget = &t;
    a.grabTimer = GRAB_HOLD_FRAMES + int(t.damage * 0.3);
    a.grabFrames = a.pommelCd = 0;
    a.vx = 0;
    t.move = nullptr;
    t.state = "GRABBED";
    t.grabbedBy = &a;
    t.knockedDown = false;
    t.hitstun = t.stunTimer = t.dodgeTimer = t.lag = 0;
    t.ledge = Ledge();
    t.vx = t.vy = t.kbx = t.kby = 0;
    fx->ring(t.x, t.y - t.h / 2.0, WHITE, 14, 4, 10);
    return "grab";
}

void CombatManager::pommel(Fighter& a) {
    Fighter* t = a.grabTarget;
    if (!t) return;
    double dmg = POMMEL_DAMAGE * (a.buffTimer > 0 ? IGNITE_MULT : 1.0);
    t->damage += dmg;
    a.aether = std::min(AETHER_MAX, a.aether + dmg * AETHER_PER_DAMAGE_DEALT);
    a.pommelCd = POMMEL_COOLDOWN; a.pommelAnim = 9;
    t->flash = 6;
    Vec2 tc = t->center();
    fx->spark(tc.x, tc.y - 10, WHITE, 5, 6, 10, 3);
    fx->styleBurst(t->style(), tc.x, tc.y - 10, t->colors, 3, 4);
    fx->shake = std::max(fx->shake, 3);
}

Fighter* CombatManager::releaseGrab(Fighter& a, bool push) {
    Fighter* t = a.grabTarget;
    a.grabTarget = nullptr;
    if (a.state == "GRABBING") {
        a.state = a.ground ? "IDLE" : "JUMPING";
        if (push) a.lag = 10;
    }
    if (t && t->grabbedBy == &a) {
        t->grabbedBy = nullptr;
        if (t->state == "GRABBED") {
            t->state = t->ground ? "IDLE" : "JUMPING";
            if (push) { t->vx = a.facing * 7; a.vx = -a.facing * 4; }
        }
    }
    return t;
}

void CombatManager::throwHeld(Fighter& a, int dirX, int dirY) {
    const Hitbox& box = a.moveSet->at("grab").hitboxes[0];
    double ang;
    if (dirY < 0) ang = 88;
    else if (dirY > 0) ang = 285;                        // slammed into the floor, they bounce up
    else {
        ang = 42;
        if (dirX == -a.facing) a.facing *= -1;
    }
    Fighter* t = releaseGrab(a, false);
    if (!t) return;
    t->x = a.x + a.facing * (a.w / 2.0 + t->w / 2.0);
    double dmg = box.damage * (a.buffTimer > 0 ? IGNITE_MULT : 1.0);
    t->damage += dmg;
    a.aether = std::min(AETHER_MAX, a.aether + dmg * AETHER_PER_DAMAGE_DEALT);
    t->aether = std::min(AETHER_MAX, t->aether + dmg * AETHER_PER_DAMAGE_TAKEN);
    double speed = physics->applyKnockback(a, *t, box.force, std::cos(radians(ang)) * a.facing, -std::sin(radians(ang)));
    enterHitState(*t, speed);
    a.hitstop = t->hitstop = 6;
    a.lag = 16;
    Vec2 tc = t->center();
    fx->spark(tc.x, tc.y, a.colors.accent, 12, 8, 18, 5);
    fx->styleBurst(t->style(), tc.x, tc.y, t->colors, 6, 6);
    fx->shake = std::max(fx->shake, 6);
}

void CombatManager::requestDeathRay(Fighter& c) {
    for (Fighter* o : physics->fighters)
        if (o != &c && o->alive() && o->isCryptic()) { pendingDuel = &c; return; }   // both are crypticsh: a duel
    fireDeathRay(c);
}

void CombatManager::fireDeathRay(Fighter& c) {
    if (c.grabTarget) releaseGrab(c);
    c.aether = 0;
    processAttack(c, DEATH_RAY);
}

void CombatManager::enterHitState(Fighter& t, double speed) {
    t.move = nullptr;
    t.state = "HIT";
    t.knockedDown = speed >= TUMBLE_SPEED;
    t.hitstun = std::max(4, int(speed * HITSTUN_PER_SPEED) - 1);   // 0.4 x knockback - 1, at least 4
    t.hopT = 0;
    t.stunTimer = t.dodgeTimer = t.lag = 0;
    t.fastFall = t.jumpCut = false;
    t.ledge = Ledge();
    t.grabbedBy = nullptr;
}

void CombatManager::shieldBreak(Fighter& t) {
    t.shieldHp = SHIELD_MAX * 0.4;
    t.state = "STUN";
    t.knockedDown = true;
    t.stunTimer = SHIELD_BREAK_STUN;
    t.vy = -11; t.ground = nullptr;
    fx->ring(t.x, t.y - t.h / 2.0, Color(255, 255, 120), 20, 12, 22);
    fx->text(t.x, t.y - t.h - 40, "SHIELD BREAK!", Color(255, 255, 120), 60);
    fx->shake = 12;
}

std::string CombatManager::handleParry(Fighter& d, Fighter& a, const Hitbox&, Projectile* source, bool counter) {
    d.aether = std::min(AETHER_MAX, d.aether + AETHER_PARRY_REWARD);
    Vec2 dc = d.center();
    fx->ring(dc.x, dc.y, Color(120, 255, 150), 20, 9, 18);
    fx->spark(dc.x, dc.y, Color(160, 255, 190), 16, 9, 20, 4);
    fx->text(d.x, d.y - d.h - 40, "PARRY!  +25", Color(120, 255, 150), 50);
    fx->shake = std::max(fx->shake, 6);
    d.hitstop = 8;
    if (!counter) { d.move = nullptr; d.state = "PARRY_SUCCESS"; d.stateTimer = 24; }
    d.flash = 14;
    if (source) {                                        // projectiles are sent back
        source->owner = &d; source->vx = -source->vx * 1.3; source->facing = -source->facing;
        source->color = d.colors.accent;
        source->hitLog.clear();
        source->life = std::max(source->life, 40);
        return "reflect";
    }
    a.hitstop = 8;
    a.move = nullptr;                                    // penalize the aggressive player
    a.state = "STUN";
    a.knockedDown = true;
    a.stunTimer = PARRY_STUN;
    if (counter) {                                       // Guard Stance also hits back
        Vec2 ac = a.center();
        a.damage += 10;
        int away = sgn(a.x - d.x);
        if (!away) away = d.facing;
        double speed = physics->applyKnockback(d, a, 13, away * 0.7, -0.7);
        enterHitState(a, speed);
        fx->spark(ac.x, ac.y, d.colors.accent, 14, 8, 20, 5);
    }
    return "parry";
}

void CombatManager::handleKo(Fighter& c) {
    const Rect& b = physics->map->blast;
    double bx = clampv(c.x, double(b.left()), double(b.right())), by = clampv(c.y - c.h / 2.0, double(b.top()), double(b.bottom()));
    double ang = std::atan2(WORLD_H / 2.0 - by, WORLD_W / 2.0 - bx);
    Color col = c.colors.main;
    fx->spark(bx, by, col, 50, 34, 40, 9, 0, ang, 0.35);
    fx->spark(bx, by, WHITE, 30, 26, 30, 6, 0, ang, 0.5);
    fx->ring(bx, by, col, 30, 16, 26);
    fx->shake = 18;
    c.currentLives -= 1;
    c.state = "KO";
    c.move = nullptr;
    c.knockedDown = false;
    c.respawnTimer = 75;
    c.vx = c.vy = c.kbx = c.kby = 0;
    c.hitstun = c.hitstop = 0;
    for (Fighter* o : physics->fighters) if (o != &c) o->kos += 1;
    if (c.currentLives <= 0) physics->gameOver = true;
}

void CombatManager::checkAllMoveDurations() {
    Stage* stage = physics->map;
    for (Fighter* cp : physics->fighters) {
        Fighter& c = *cp;
        c.anim += 1;
        c.ticks += 1;
        if (c.state == "KO") {
            if (c.currentLives > 0) {
                c.respawnTimer -= 1;
                if (c.respawnTimer <= 0) {
                    c.damage = 0;
                    c.respawnAt(stage->respawnX, stage->respawnY, c.slot == 0 ? 1 : -1);
                    c.stateTimer = 150;
                    c.intangible = 240;
                }
            }
            continue;
        }
        if (c.frozen) continue;
        c.aether = std::min(AETHER_MAX, c.aether + AETHER_PASSIVE);
        c.squash *= 0.8;
        if (c.style() == Style::Kindled && c.ticks % 9 == 0)     // her hair sheds the odd ember
            fx->spark(c.x - c.facing * 6, c.y - c.h - 14, c.colors.accent, 1, 1.6, 44, 3, -0.07, -PI / 2, 0.9);
        for (int* v : {&c.intangible, &c.lag, &c.ledgeCd, &c.dropTimer, &c.buffTimer, &c.flash, &c.shieldStun, &c.pommelCd, &c.pommelAnim})
            if (*v > 0) *v -= 1;
        std::string st = c.state;
        if (c.move) {
            c.moveFrame += 1;
            if (c.moveFrame >= c.move->duration) endMove(c);
        }
        if (c.hitstun > 0) {
            c.hitstun -= 1;
            if (c.hitstun == 0 && st == "HIT") { c.state = c.ground ? "IDLE" : "JUMPING"; c.knockedDown = false; }
        }
        if (st == "STUN") {
            c.stunTimer -= 1;
            if (c.stunTimer <= 0) { c.state = c.ground ? "IDLE" : "JUMPING"; c.knockedDown = false; }
        } else if (st == "PARRY_SUCCESS" || st == "RESPAWN") {
            c.stateTimer -= 1;
            if (c.stateTimer <= 0) c.state = c.ground ? "IDLE" : "JUMPING";
        } else if (st == "JUMPSQUAT") {
            c.squat -= 1;
            if (c.squat <= 0) physics->launchJump(c);
        } else if (st == "DODGE") {
            c.dodgeTimer -= 1;
            if (c.dodgeTimer <= 0) { c.state = c.ground ? "IDLE" : "JUMPING"; c.lag = 4; }
        } else if (st == "GRABBING") {
            c.grabFrames += 1;
            c.grabTimer -= 1;
            Fighter* t = c.grabTarget;
            if (!t || t->state != "GRABBED" || c.grabTimer <= 0) releaseGrab(c);
        } else if (st == "GRABBED") {
            Fighter* g = c.grabbedBy;
            if (!g || g->state != "GRABBING" || g->grabTarget != &c) {
                c.grabbedBy = nullptr;
                c.state = c.ground ? "IDLE" : "JUMPING";
            }
        } else if (st == "LEDGE") {
            c.ledgeTimer += 1;
            if (c.ledgeTimer > 200) { c.state = "JUMPING"; c.ledgeCd = 40; c.ledge = Ledge(); }
        }
        if (st == "SHIELD") {
            c.shieldFrame += 1;
            c.shieldHp -= 0.22;
            if (c.shieldHp <= 0) shieldBreak(c);
        } else {
            c.shieldHp = std::min(SHIELD_MAX, c.shieldHp + 0.1);
        }
        if (c.buffTimer > 0 && c.ticks % 3 == 0) {
            Rng& r = fxRng();
            fx->spark(c.x + r.uniform(-c.w / 2.0, c.w / 2.0), c.y - r.uniform(0, c.h), c.colors.accent, 1, 2, 16, 4, -0.25);
        }
    }
}
