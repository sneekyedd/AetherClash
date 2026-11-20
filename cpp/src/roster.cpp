// The four fighters, their 19 moves each, and the three stages. Frame data is identical to the Python version.
#include "core.h"

std::vector<FighterSpec> ROSTER;
Move DEATH_RAY;

std::vector<Circle> rectToCircles(double w, double h) {
    double lng = std::max(w, h), sh = std::max(1.0, std::min(w, h));
    int n = lng > sh * 1.6 ? std::max(1, int(std::nearbyint(lng / sh))) : 1;
    double seg = lng / n, r = std::sqrt(seg * sh / PI);
    std::vector<Circle> out;
    for (int i = 0; i < n; ++i) {
        double o = -lng / 2 + (i + 0.5) * seg;
        out.push_back(w >= h ? Circle{o, 0.0, r} : Circle{0.0, o, r});
    }
    return out;
}

Hitbox hb(int start, int end, double x, double y, double w, double h, double damage, double force, double angle, HbFlags f) {
    Hitbox b;
    b.start = start; b.end = end; b.x = x; b.y = y; b.w = w; b.h = h;
    b.circles = rectToCircles(w, h);
    b.damage = damage; b.force = force; b.angle = angle;
    b.gid = f.gid; b.grab = f.grab; b.radial = f.radial; b.pull = f.pull; b.spike = f.spike; b.unblockable = f.unblockable;
    return b;
}

int Move::firstStart() const {
    int s = -1;
    for (auto& b : hitboxes) if (!b.spike) s = s < 0 ? b.start : std::min(s, b.start);
    if (s < 0) for (auto& b : hitboxes) s = s < 0 ? b.start : std::min(s, b.start);
    return s;
}

namespace {
// shorthand flags
HbFlags G_(int gid) { HbFlags f; f.gid = gid; return f; }
HbFlags SPIKE() { HbFlags f; f.spike = true; return f; }
HbFlags RADIAL(int gid = 0) { HbFlags f; f.radial = true; f.gid = gid; return f; }

struct MoveB {                     // a small builder so the move lists read like the Python ones
    Move m;
    MoveB(const std::string& name, const std::string& type, int dur, std::vector<Hitbox> boxes, double cost) {
        m.name = name; m.type = type; m.duration = dur; m.hitboxes = std::move(boxes); m.requiredAether = cost;
        for (auto& b : m.hitboxes) m.damage = std::max(m.damage, b.damage);
    }
    MoveB& imp(int f, std::optional<double> ix, std::optional<double> iy) {
        Impulse i; i.hasX = ix.has_value(); i.hasY = iy.has_value(); i.x = ix.value_or(0); i.y = iy.value_or(0);
        m.impulses[f] = i; return *this;
    }
    MoveB& armor(int a, int b) { m.armor = rng_(a, b); return *this; }
    MoveB& counter(int a, int b) { m.counter = rng_(a, b); return *this; }
    MoveB& intang(int a, int b) { m.intangible = rng_(a, b); return *this; }
    MoveB& nograv(int a, int b) { m.noGravity = rng_(a, b); return *this; }
    MoveB& aim(double sp, int a, int b) { m.aim.on = true; m.aim.speed = sp; m.aim.a = a; m.aim.b = b; return *this; }
    MoveB& proj(ProjectileSpec p) { p.on = true; m.projectile = p; return *this; }
    MoveB& effect(const std::string& e, int f) { m.effect = e; m.effectFrame = f; return *this; }
    MoveB& helpless() { m.helpless = true; return *this; }
    MoveB& llag(int l) { m.landingLag = l; return *this; }
    operator Move() const { return m; }
};
MoveB A(const std::string& n, int dur, std::vector<Hitbox> boxes = {}) { return MoveB(n, "Attack", dur, std::move(boxes), 0); }
MoveB S(const std::string& n, int dur, double cost, std::vector<Hitbox> boxes = {}, const std::string& kind = "Special") {
    return MoveB(n, kind, dur, std::move(boxes), cost);
}
MoveB G(const std::string& n, int dur, double x, double w, double h, double dmg, double force) {
    HbFlags f; f.grab = true;
    return MoveB(n, "Grab", dur, {hb(dur / 5, dur / 5 + 3, x, -4, w, h, dmg, force, 45, f)}, 0);
}
ProjectileSpec P(int frame, double x, double y, double w, double h, double vx, int life, double dmg, double force,
                 double angle, const std::string& kind) {
    ProjectileSpec p;
    p.frame = frame; p.x = x; p.y = y; p.w = w; p.h = h; p.vx = vx; p.life = life; p.damage = dmg; p.force = force;
    p.angle = angle; p.kind = kind;
    return p;
}

std::map<std::string, Move> kaiMoves() {
    return {
        {"jab", A("Quick Jab", 14, {hb(3, 5, 38, -6, 44, 28, 3, 4, 60)})},
        {"ftilt", A("Thread Slash", 22, {hb(5, 8, 48, -4, 64, 30, 7, 9, 35)})},
        {"utilt", A("Crescent Stitch", 22, {hb(4, 9, 10, -52, 70, 50, 6, 9, 85)})},
        {"dtilt", A("Low Hem", 18, {hb(4, 6, 44, 30, 60, 18, 5, 8, 75)})},
        {"dash", A("Slipknot", 26, {hb(5, 11, 34, 4, 58, 34, 6, 11.5, 38)})},
        {"dashheavy", A("Seamrender", 50, {hb(22, 26, 52, -4, 80, 46, 17, 19.5, 40)}).imp(20, 16, std::nullopt)},
        {"fsmash", A("Burst Strike", 40, {hb(12, 15, 58, -6, 76, 40, 15, 16.5, 38)})},
        {"usmash", A("Spindle Rise", 40, {hb(9, 14, 6, -64, 60, 70, 13, 16, 88)})},
        {"dsmash", A("Seam Splitter", 42, {hb(8, 9, 74, 28, 26, 24, 13, 13, 25, SPIKE()), hb(8, 9, -74, 28, 26, 24, 13, 13, 155, SPIKE()),
                                           hb(8, 11, 50, 26, 64, 24, 12, 15, 25), hb(8, 11, -50, 26, 64, 24, 12, 15, 155)})},
        {"nair", A("Whirl", 30, {hb(4, 8, 0, 0, 90, 80, 3, 5, 70), hb(12, 16, 0, 0, 96, 84, 5, 9, 50, G_(1))})},
        {"fair", A("Needle Rush", 28, {hb(6, 10, 48, -4, 60, 36, 8, 10.5, 40)})},
        {"bair", A("Back Lash", 30, {hb(7, 10, -50, -4, 62, 32, 11, 14, 150)})},
        {"uair", A("Sky Thread", 26, {hb(5, 10, 4, -54, 56, 48, 7, 10, 85)})},
        {"dair", A("Plumb Line", 34, {hb(10, 12, 6, 62, 26, 28, 12, 13, 75, SPIKE()), hb(10, 16, 6, 44, 40, 56, 8, 9, 70)}).llag(10)},
        {"nspecial", S("Thread Needle", 26, 8).proj(P(8, 30, -8, 30, 8, 16, 48, 5, 6, 30, "needle"))},
        {"sspecial", S("Stitch Dash", 34, 15, {hb(7, 14, 10, 0, 74, 50, 10, 11.5, 55)}).imp(6, 24, 0).imp(15, 4, std::nullopt).nograv(6, 14)},
        {"uspecial", S("Skyhook", 40, 0, {hb(5, 9, 6, -30, 56, 70, 2, 4, 85), hb(10, 14, 6, -30, 56, 70, 2, 4, 85, G_(1)),
                                          hb(15, 19, 6, -34, 64, 76, 5, 11, 80, G_(2))}).imp(5, 3, -19).helpless()},
        {"dspecial", S("Aether Burst", 46, 30, {hb(14, 20, 0, -4, 200, 180, 15, 17, 60, RADIAL())})},
        {"grab", G("Snare", 30, 34, 36, 40, 7, 10)},
    };
}

std::map<std::string, Move> bronnMoves() {
    ProjectileSpec wall = P(14, 80, -14, 52, 120, 0, 260, 12, 13, 65, "wall");
    wall.gravity = 0.8; wall.rehit = 50; wall.sturdy = true; wall.grounded = true;
    return {
        {"jab", A("Gauntlet Check", 22, {hb(6, 9, 46, -8, 50, 36, 6, 6.5, 50)})},
        {"ftilt", A("Shield Bash", 30, {hb(9, 13, 54, -6, 64, 48, 11, 12, 35)})},
        {"utilt", A("Lintel Lift", 30, {hb(8, 13, 8, -66, 84, 48, 10, 12, 88)})},
        {"dtilt", A("Foundation Kick", 26, {hb(7, 10, 52, 36, 70, 22, 9, 10, 30)})},
        {"dash", A("Rampart Rush", 36, {hb(8, 16, 40, 0, 60, 70, 10, 12.5, 40)}).armor(4, 14)},
        {"dashheavy", A("Avalanche Charge", 62, {hb(26, 32, 58, -6, 92, 84, 22, 20.5, 42)}).imp(24, 14, std::nullopt).armor(10, 32)},
        {"fsmash", A("Keystone Hammer", 54, {hb(20, 24, 70, -10, 90, 70, 22, 19, 40)}).armor(14, 24)},
        {"usmash", A("Tower Crown", 50, {hb(15, 21, 0, -70, 100, 100, 19, 18, 90)})},
        {"dsmash", A("Ground Slam", 56, {hb(18, 19, 100, 34, 36, 40, 21, 15, 80, SPIKE()), hb(18, 19, -100, 34, 36, 40, 21, 15, 100, SPIKE()),
                                         hb(18, 22, 0, 36, 230, 36, 20, 17.5, 80, RADIAL())})},
        {"nair", A("Rolling Boulder", 38, {hb(6, 20, 0, 0, 96, 100, 10, 11, 45)}).llag(10)},
        {"fair", A("Maul Swing", 44, {hb(14, 18, 54, 0, 70, 80, 15, 15, 35)}).llag(12)},
        {"bair", A("Buttress", 36, {hb(10, 14, -56, -4, 70, 50, 14, 15, 145)}).llag(10)},
        {"uair", A("Capstone", 34, {hb(8, 13, 0, -64, 80, 50, 12, 13, 90)})},
        {"dair", A("Portcullis", 46, {hb(12, 16, 0, 64, 40, 30, 18, 15, 80, SPIKE()), hb(12, 30, 0, 46, 64, 56, 13, 12, 60)})
                     .imp(12, 0, 16).llag(18)},
        {"nspecial", S("Guard Stance", 44, 0, {}, "Defensive").counter(4, 26)},
        {"sspecial", S("Bulwark Charge", 50, 15, {hb(11, 30, 40, -4, 60, 80, 14, 15, 45)}).imp(10, 14, std::nullopt).armor(8, 30)},
        {"uspecial", S("Rising Bastion", 50, 0, {hb(8, 20, 0, -30, 80, 90, 12, 13, 80)}).imp(8, 4, -19).armor(4, 20).helpless()},
        {"dspecial", S("Stone Wall", 40, 25).proj(wall)},
        {"grab", G("Iron Grip", 36, 40, 44, 50, 10, 12)},
    };
}

std::map<std::string, Move> nyxMoves() {
    ProjectileSpec well = P(14, 120, -10, 130, 130, 0, 160, 2, 6, 90, "well");
    well.rehit = 14; well.pull = true; well.ghost = true;
    return {
        {"jab", A("Spark Tap", 16, {hb(4, 6, 40, -8, 46, 26, 3, 4.5, 45)})},
        {"ftilt", A("Orbit Swipe", 26, {hb(7, 11, 56, -6, 74, 30, 8, 10, 35)})},
        {"utilt", A("Halo", 26, {hb(6, 12, 0, -60, 90, 36, 7, 9.5, 90)})},
        {"dtilt", A("Umbral Sweep", 22, {hb(5, 8, 50, 32, 72, 18, 6, 8, 80)})},
        {"dash", A("Comet Slide", 30, {hb(6, 13, 36, 8, 62, 36, 7, 11, 38)})},
        {"dashheavy", A("Supernova Rush", 54, {hb(22, 27, 50, -6, 96, 90, 17, 19.5, 42)}).imp(20, 15, std::nullopt)},
        {"fsmash", A("Nova Lance", 46, {hb(15, 18, 80, -8, 120, 30, 16, 17, 35)})},
        {"usmash", A("Zenith Flare", 46, {hb(12, 18, 0, -76, 70, 110, 15, 16.5, 90)})},
        {"dsmash", A("Eclipse Ring", 48, {hb(10, 11, 92, 26, 30, 38, 14, 13, 30, SPIKE()), hb(10, 11, -92, 26, 30, 38, 14, 13, 150, SPIKE()),
                                          hb(10, 14, 0, 28, 210, 34, 13, 15, 30, RADIAL())})},
        {"nair", A("Constellation", 34, {hb(5, 8, 0, 0, 110, 110, 2, 3, 80), hb(11, 14, 0, 0, 110, 110, 2, 3, 80, G_(1)),
                                         hb(17, 21, 0, 0, 120, 120, 5, 10, 50, RADIAL(2))})},
        {"fair", A("Starfall Fan", 32, {hb(8, 12, 54, -2, 70, 56, 9, 11, 45)})},
        {"bair", A("Umbra Kick", 30, {hb(7, 10, -52, 0, 64, 34, 10, 13, 150)})},
        {"uair", A("Moonbeam", 30, {hb(6, 12, 0, -66, 40, 80, 8, 10.5, 90)})},
        {"dair", A("Meteor Seed", 36, {hb(11, 13, 0, 62, 28, 28, 12, 13, 80, SPIKE()), hb(11, 15, 0, 48, 48, 48, 8, 9, 70)}).llag(10)},
        {"nspecial", S("Star Orb", 30, 8).proj(P(12, 34, -6, 34, 34, 6, 110, 8, 9.5, 45, "orb"))},
        {"sspecial", S("Comet Lance", 36, 15).proj(P(14, 40, -6, 64, 14, 22, 40, 11, 12, 30, "lance"))},
        {"uspecial", S("Blink", 34, 0).aim(34, 10, 15).intang(8, 17).nograv(0, 17).helpless()},
        {"dspecial", S("Gravity Well", 38, 25).proj(well)},
        {"grab", G("Tether", 38, 60, 80, 30, 8, 10.5)},
    };
}

std::map<std::string, Move> rheaMoves() {
    return {
        {"jab", A("One-Two", 18, {hb(3, 5, 36, -8, 44, 30, 3, 3, 80), hb(9, 11, 40, -8, 48, 32, 4, 6, 45, G_(1))})},
        {"ftilt", A("Ember Hook", 24, {hb(6, 9, 44, -8, 52, 36, 9, 10.5, 40)})},
        {"utilt", A("Flare Upper", 24, {hb(5, 10, 14, -50, 50, 64, 8, 10, 85)})},
        {"dtilt", A("Coal Sweep", 20, {hb(5, 8, 44, 32, 60, 20, 7, 8.5, 70)})},
        {"dash", A("Wildfire Knee", 28, {hb(5, 12, 34, -2, 50, 50, 8, 12, 40)})},
        {"dashheavy", A("Meteor Rush", 52, {hb(20, 24, 50, -6, 72, 60, 20, 19.5, 40)}).imp(18, 17, std::nullopt)},
        {"fsmash", A("Kiln Breaker", 46, {hb(14, 18, 56, -8, 70, 50, 19, 18, 38)}).imp(12, 8, std::nullopt)},
        {"usmash", A("Eruption", 44, {hb(10, 16, 8, -66, 56, 90, 16, 17, 90)})},
        {"dsmash", A("Magma Stomp", 46, {hb(12, 13, 84, 28, 30, 36, 16, 14, 35, SPIKE()), hb(12, 13, -84, 28, 30, 36, 16, 14, 145, SPIKE()),
                                         hb(12, 16, 0, 30, 190, 32, 15, 16, 35, RADIAL())})},
        {"nair", A("Cinder Wheel", 30, {hb(4, 16, 0, 0, 84, 84, 8, 9.5, 50)})},
        {"fair", A("Hammer Fist", 34, {hb(10, 11, 48, 24, 34, 34, 14, 13.5, 45, SPIKE()), hb(10, 13, 44, 6, 54, 60, 11, 12, 45)}).llag(10)},
        {"bair", A("Backdraft", 30, {hb(7, 10, -46, -6, 56, 40, 12, 14.5, 150)})},
        {"uair", A("Spark Flip", 28, {hb(5, 11, 0, -56, 70, 44, 9, 11, 80)})},
        {"dair", A("Meteor Kick", 40, {hb(8, 12, 20, 50, 32, 32, 13, 13, 70, SPIKE()), hb(8, 30, 16, 40, 46, 46, 9, 10, 60)})
                     .imp(8, 7, 17).llag(14)},
        {"nspecial", S("Flare Jab", 34, 10, {hb(12, 16, 66, -6, 96, 72, 14, 15.5, 45)})},
        {"sspecial", S("Comet Tackle", 40, 15, {hb(9, 22, 30, 0, 60, 60, 12, 13.5, 50)}).imp(8, 17, -5)},
        {"uspecial", S("Pyre Uppercut", 42, 0, {hb(6, 10, 10, -30, 56, 76, 5, 5, 88), hb(11, 20, 10, -36, 60, 80, 9, 13.5, 75, G_(1))})
                         .imp(6, 5, -19).helpless()},
        {"dspecial", S("Ignite", 36, 30, {hb(18, 22, 0, 0, 130, 120, 5, 8, 70, RADIAL())}).effect("ignite", 18)},
        {"grab", G("Clinch", 30, 34, 36, 44, 9, 11.5)},
    };
}

Palette pal(Color m, Color d, Color a, Color s, std::map<std::string, Color> extra) { return Palette{m, d, a, s, std::move(extra)}; }
}  // namespace

void buildRoster() {
    if (!ROSTER.empty()) return;
    using C = Color;
    // movement copied from Smash Ultimate (SmashWiki attribute tables, Smash units):
    // Kai = Sheik, Bronn = Ganondorf, Nyx = Mewtwo (its one huge double jump split over two), Rhea = Terry
    ROSTER.push_back(FighterSpec{"Kai", "The Weaver", "Weaver", Style::Weaver, kaiMoves(),
        {"Thread-blade duelist. Fastest on the", "ground, strings quick hits into combos,", "but light and easy to launch."},
        BaseStats{0.68, 86, 44, 78, 1, SmashStats{1.47, 2.178, 7, 2.42, 0.165, 0.106, 1.155, 0.09, 0.015, 0.15, 1.75, 39, 18.75, 40, 4}},
        {5, 2, 2, 3}, {"nspecial", "sspecial"}, {"dspecial"},
        pal(C(170, 44, 54), C(104, 30, 46), C(230, 182, 86), C(236, 192, 152),
            {{"hair", C(122, 70, 44)}, {"band", C(200, 52, 56)}, {"wrap", C(208, 172, 122)}, {"boot", C(118, 66, 46)}}),
        pal(C(40, 150, 144), C(16, 76, 82), C(250, 236, 170), C(236, 192, 152),
            {{"hair", C(70, 74, 96)}, {"band", C(250, 236, 170)}, {"wrap", C(200, 206, 214)}, {"boot", C(50, 60, 80)}})});
    ROSTER.push_back(FighterSpec{"Bronn", "The Sentinel", "Sentinel", Style::Sentinel, bronnMoves(),
        {"Stone-clad guardian. Slow and heavy with", "armored attacks, a counter stance and", "walls that punish reckless aggression."},
        BaseStats{0.78, 124, 64, 94, 1, SmashStats{0.767, 1.87, 15, 1.34, 0.13893, 0.125, 0.83, 0.04, 0.015, 0.108, 1.65, 25.49, 12.24, 26, 5}},
        {2, 5, 5, 3}, {"sspecial", "dspecial"}, {"nspecial"},
        pal(C(70, 124, 192), C(34, 58, 108), C(120, 240, 255), C(92, 150, 210),
            {{"stone", C(136, 142, 154)}, {"steel", C(158, 164, 178)}, {"belt", C(122, 82, 54)}}),
        pal(C(176, 120, 60), C(90, 56, 24), C(255, 226, 150), C(204, 150, 84),
            {{"stone", C(150, 140, 130)}, {"steel", C(170, 160, 150)}, {"belt", C(70, 50, 40)}})});
    ROSTER.push_back(FighterSpec{"Nyx", "The Hollow Star", "Voidcaller", Style::Voidcaller, nyxMoves(),
        {"Floaty star-mage with three jumps.", "Controls space with orbs, lances and a", "gravity well; teleports to recover."},
        BaseStats{0.46, 88, 42, 82, 2, SmashStats{1.26, 2.255, 10, 2.255, 0.11, 0.116, 1.313, 0.07, 0.016, 0.082, 1.55, 31.11, 17.5, 28.7, 5}},
        {3, 3, 2, 5}, {"nspecial", "sspecial", "dspecial"}, {},
        pal(C(104, 70, 156), C(52, 34, 88), C(255, 122, 230), C(18, 10, 32), {{"hat", C(118, 80, 168)}, {"hatband", C(162, 116, 208)}}),
        pal(C(36, 70, 120), C(14, 30, 60), C(140, 255, 220), C(8, 16, 34), {{"hat", C(48, 92, 150)}, {"hatband", C(90, 140, 190)}})});
    ROSTER.push_back(FighterSpec{"Rhea", "The Cinderfist", "Kindled", Style::Kindled, rheaMoves(),
        {"Close-range brawler. Short reach but", "every punch hurts. Spend Aether to", "Ignite and hit 30% harder."},
        BaseStats{0.72, 104, 52, 84, 1, SmashStats{0.85, 1.65, 10, 1.72, 0.16, 0.111, 0.95, 0.06, 0.012, 0.09, 1.48, 27, 15.2, 29, 4}},
        {4, 4, 3, 1}, {"sspecial", "dspecial"}, {"nspecial"},
        pal(C(222, 98, 34), C(128, 62, 32), C(255, 216, 92), C(246, 200, 160),
            {{"flame", C(246, 104, 26)}, {"band", C(138, 90, 52)}, {"wrap", C(216, 186, 142)}}),
        pal(C(78, 82, 100), C(44, 46, 60), C(96, 206, 255), C(228, 162, 114),
            {{"flame", C(40, 96, 230)}, {"band", C(30, 32, 44)}, {"wrap", C(190, 196, 210)}})});
    for (auto& spec : ROSTER)
        for (auto& [slot, m] : spec.moves) m.slot = slot;
    // The secret super move: a comically large gun. Firing it ends the match in a win for whoever fired it.
    DEATH_RAY = S("Death Ray", 130, 0).imp(0, 0, 0).intang(0, 112).nograv(0, 112).effect("deathray", 56);
    DEATH_RAY.slot = "deathray";
}

// ===================================================================== stages
Platform::Platform(double x_, double y_, int w_, int h_, bool solid_)
    : baseX(x_), baseY(y_), x(x_), y(y_), width(w_), height(h_), solid(solid_), ledges(solid_), prevTop(y_) {
    Rng r(uint64_t(x_ * 7 + y_));
    for (auto& j : jag) j = r.uniform(0.5, 1.0);
}

void Platform::update(int frame) {
    prevTop = y;
    if (moving) {
        double a = frame / period * TAU + phase;
        double nx = baseX + ampX * std::sin(a), ny = baseY + ampY * std::sin(a);
        dx = nx - x; dy = ny - y;
        x = nx; y = ny;
    }
}

void Stage::finish() {
    left = 1e9; right = -1e9; top = 1e9;
    for (auto& p : platforms)
        if (p.solid) { left = std::min(left, p.x); right = std::max(right, p.x + p.width); top = std::min(top, p.y); }
    spawns = {{{double(WORLD_W / 2 - 300), top, 1.0}, {double(WORLD_W / 2 + 300), top, -1.0}}};
    respawnX = WORLD_W / 2; respawnY = top - 380;
}

void Stage::reset() {
    for (auto& p : platforms) {
        p.x = p.baseX; p.y = p.baseY; p.dx = p.dy = 0; p.prevTop = p.baseY;
        p.update(0);
    }
}

std::vector<Stage> buildStages() {
    using C = Color;
    std::vector<Stage> out;
    {
        Stage s; s.key = "atoll"; s.name = "Halcyon Atoll"; s.blurb = {"A calm sky-island at dusk,", "with one drifting cloud-step."};
        s.platforms = {Platform(750, 820, 900, 110), Platform(850, 675, 220, 14, false), Platform(1330, 675, 220, 14, false),
                       Platform(1100, 525, 200, 14, false).motion(200, 0, 540, 0)};
        s.palette = {C(38, 44, 110), C(250, 150, 118), C(92, 74, 96), C(120, 214, 150), C(255, 236, 200)};
        s.finish(); out.push_back(std::move(s));
    }
    {
        Stage s; s.key = "loom"; s.name = "The Great Loom"; s.blurb = {"Two islands stitched together by", "a thread-bridge you can drop through."};
        s.platforms = {Platform(600, 820, 460, 110), Platform(1340, 820, 460, 110), Platform(1050, 820, 300, 14, false),
                       Platform(1080, 650, 240, 14, false), Platform(640, 675, 170, 14, false), Platform(1590, 675, 170, 14, false)};
        s.palette = {C(10, 8, 34), C(72, 34, 104), C(54, 44, 92), C(255, 200, 110), C(255, 214, 130)};
        s.finish(); out.push_back(std::move(s));
    }
    {
        Stage s; s.key = "crucible"; s.name = "Sentinel's Crucible"; s.blurb = {"A small forge-top arena.", "Rising lifts guard the edges."};
        s.platforms = {Platform(820, 840, 760, 110), Platform(1100, 695, 200, 14, false),
                       Platform(590, 700, 170, 14, false).motion(0, 150, 420, 0),
                       Platform(1640, 700, 170, 14, false).motion(0, 150, 420, PI)};
        s.palette = {C(18, 12, 20), C(150, 52, 24), C(58, 52, 60), C(150, 230, 255), C(255, 150, 80)};
        s.finish(); out.push_back(std::move(s));
    }
    for (auto& s : out) s.reset();
    return out;
}
