// AETHER CLASH - art: skeleton-animated fighters, cloth physics, attack effects and stage scenery,
// all drawn with code. A port of the ART section of the Python version.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core.h"

constexpr int SPR = 340, SPR_CX = 170, SPR_FEET = 262;
constexpr double HIP_K = 0.43, SH_K = 0.73, HEAD_K = 0.15;
const Color OUTLINE(22, 14, 34);
constexpr int PIXEL = 2;
extern int SS;                         // supersampling (2, or 1 on slow computers)

// how an attack should be animated (the Python version's "info" dict)
struct AnimInfo {
    std::string weapon = "shield", limb, phase, swing;
    bool hot = false;
    bool hasT = false;
    Vec2 T;
    double sprog = 0, spin = 0, wlen = 50, sweep = 1.3;
    std::optional<double> stride, runk, vy, smear;
    int pommel = 0;
};

struct Pose {
    Vec2 hip, sh, head, hf, hb, ff, fb, orb;
    bool hasOrb = false;
    double spin = 0, wp = -0.9;
};

struct VisTrack {
    double anim = 0, stride = 0, runk = 1, land = 0, flip = 0, roll = 0, vy = 0;
    int rollDir = 1, jumps = 1;
    bool ground = false;
};

struct ClothSim {
    struct P4 { double x, y, px, py; };
    std::map<std::string, std::vector<P4>> chains;
    double ox = 0, oy = 0;
    int f = 1;
    void begin(double x, double y, int facing) { ox = x; oy = y; f = facing; }
    std::vector<Vec2> chain(const std::string& name, Vec2 anchor, int n, double seg, double gravity = 0.5,
                            double damp = 0.86, double wave = 0.25, double t = 0.0, double breeze = 0.12);
};

std::vector<Vec2> starPoints(double x, double y, double r, double rot = 0.0);
Pose targetPose(const FighterSpec& spec, const std::string& key, double t, const AnimInfo& info = AnimInfo());
Pose blendPose(const Pose& cur, const Pose& tgt, double rate);
SurfacePtr renderFighterSurface(const FighterSpec& spec, const Palette& col, const Pose& P, int facing, double t,
                                const std::string& key = "idle", Vec2 vel = {0, 0}, const AnimInfo& info = AnimInfo(),
                                ClothSim* cloth = nullptr, Vec2 origin = {0, 0});
SurfacePtr quickSprite(const FighterSpec& spec, const Palette& col, const std::string& key, int facing, double t,
                       double zoom = 1.5);

void slashArc(Surface& s, Vec2 c, double radius, double a0, double a1, double thick, Color outer, Color core);
void drawAttackFx(Surface& surf, const Fighter& c, const Hitbox& box, const std::vector<std::array<double, 3>>& circles,
                  int frame);
void drawDeathRay(Surface& surf, const Fighter& c);
SurfacePtr makeIcon(int size = 256);
SurfacePtr makeBackground(const Stage& stage);
void drawPlatform(Surface& surf, const Platform& p, const StagePalette& pal);
void drawProjectile(Surface& surf, const Projectile& p, int frame);

struct Visuals {
    std::string key = "idle";
    std::optional<Color> tint, outline;     // tint carries its strength in .a
    int alpha = 255;
    double rotation = 0;
    AnimInfo info;
};

struct StateTranslator {
    AnimInfo attackInfo(const Fighter& c);
    VisTrack& track(Fighter& c);
    Visuals determineVisuals(Fighter& c);
    SurfacePtr spriteFor(Fighter& c, Visuals& v);
    void updateVisuals(Surface& surface, const std::vector<Fighter*>& fighters, PhysicsEngine& physics,
                       bool showBoxes = false);
};
