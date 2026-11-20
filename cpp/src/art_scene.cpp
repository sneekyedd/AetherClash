// Attack effects, the Death Ray, the icon, stage scenery and the StateTranslator
// (which turns a fighter's game state into the pose, tint and outline the player sees).
#include <cmath>

#include "art.h"

using V = Vec2;
using Pts = std::vector<Vec2>;

static const std::set<std::string> MAUL_SLOTS = {"fsmash", "usmash", "dsmash", "utilt", "fair", "bair", "uair", "nair", "dashheavy"};
static const std::set<std::string> THRUSTS = {"jab", "fair", "dash", "nspecial", "sspecial", "grab"};

static Pts rev(Pts p) { std::reverse(p.begin(), p.end()); return p; }
static Pts cat(Pts a, const Pts& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
static long floordiv(long a, long b) { long q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }

void slashArc(Surface& s, V c, double radius, double a0, double a1, double thick, Color outer, Color core) {
    if (std::abs(a1 - a0) < 0.05) return;
    int n = 16;
    for (auto [color, k] : {std::pair<Color, double>{outer, 1.0}, {core, 0.45}}) {
        Pts op, ip;
        for (int i = 0; i <= n; ++i) {
            double u = double(i) / n;
            double a = a0 + (a1 - a0) * u;
            double th = thick * k * std::pow(std::sin(PI * u), 0.7);
            double r_out = radius - thick * (1 - k) * 0.3;
            op.push_back({c.x + std::cos(a) * r_out, c.y + std::sin(a) * r_out});
            ip.push_back({c.x + std::cos(a) * (r_out - th), c.y + std::sin(a) * (r_out - th)});
        }
        draw::polygon(s, color, cat(op, rev(ip)));
    }
}

void drawAttackFx(Surface& surf, const Fighter& c, const Hitbox& box, const std::vector<std::array<double, 3>>& circles,
                  int frame) {
    Style style = c.spec->style;
    Color acc = c.colors.accent;
    int span = std::max(1, box.end - box.start + 1);
    double prog = clampv(double(c.moveFrame - box.start + 1) / span, 0.0, 1.0);
    double fade = 1.0 - 0.45 * prog;
    double bx = c.x, by = c.y - c.h / 2.0;
    for (auto& ci : circles) {
        double cx = ci[0], cy = ci[1];
        int R = int(ci[2]);
        if (R < 2 || R > 1500) continue;
        int size = R * 2 + 8;
        Surface s(size, size, true);
        int o = size / 2;
        V O(o, o);
        double ang = std::abs(cx - bx) + std::abs(cy - by) > 4 ? std::atan2(cy - by, cx - bx) : (c.facing > 0 ? 0.0 : PI);
        if (box.grab) {
            draw::circle(s, Color(255, 255, 255, int(70 * fade)), O, R);
            for (int k = 0; k < 8; ++k) {
                double a0 = k * TAU / 8 + frame * 0.15;
                Pts pts;
                for (int q = 0; q < 6; ++q) pts.push_back({o + std::cos(a0 + q * 0.06) * R, o + std::sin(a0 + q * 0.06) * R});
                draw::lines(s, Color(255, 255, 255, 230), false, pts, 3);
            }
            surf.blit(s, cx - o, cy - o);
            continue;
        }
        if (box.spike) {
            draw::circle(s, Color(255, 110, 255, 90), O, R);
            draw::circle(s, Color(255, 190, 255, 235), O, R, 2);
            draw::lines(s, WHITE, false, {{o - R * 0.5, o - R * 0.2}, {double(o), o + R * 0.45}, {o + R * 0.5, o - R * 0.2}}, 2);
            surf.blit(s, cx - o, cy - o);
            continue;
        }
        draw::circle(s, acc.withAlpha(int(80 * fade)), O, R);
        draw::circle(s, lerpColor(acc, WHITE, 0.55).withAlpha(int(150 * fade)), O, std::max(2, R - 3), std::max(1, R / 7));
        if (style == Style::Weaver) {
            double sweep = 1.25;
            double a0 = ang - sweep * c.facing;
            double a1 = a0 + 2 * sweep * c.facing * std::min(1.0, prog * 1.6 + 0.2);
            int n = 14;
            Pts outer, inner;
            for (int q = 0; q <= n; ++q) {
                double a = a0 + (a1 - a0) * q / n;
                outer.push_back({o + std::cos(a) * (R - 2), o + std::sin(a) * (R - 2)});
                double rr = R - 2 - R * 0.42 * std::sin(PI * q / n);
                inner.push_back({o + std::cos(a) * rr + R * 0.12 * std::cos(ang), o + std::sin(a) * rr + R * 0.12 * std::sin(ang)});
            }
            draw::polygon(s, lerpColor(acc, WHITE, 0.35).withAlpha(235), cat(outer, rev(inner)));
            draw::lines(s, Color(255, 255, 255, 255), false, outer, 2);
            for (int k = 0; k < 3; ++k) {
                double a = ang + (k - 1) * 0.7 + frame * 0.07;
                draw::line(s, acc.withAlpha(200), O, V(o + std::cos(a) * R * 0.85, o + std::sin(a) * R * 0.85), 1);
            }
        } else if (style == Style::Sentinel) {
            Color stone = c.colors.get("stone", Color(136, 142, 154));
            draw::circle(s, stone.withAlpha(int(120 * fade)), O, int(R * (0.35 + 0.35 * prog)));
            Rng rng(uint64_t(box.gid * 31 + floordiv(long(cx), 40)));
            for (int k = 0; k < 6; ++k) {
                double a = k * TAU / 6 + rng.uniform(-0.3, 0.3);
                double mid_r = R * rng.uniform(0.35, 0.6);
                V p1(o + std::cos(a) * R * 0.15, o + std::sin(a) * R * 0.15);
                V p2(o + std::cos(a + 0.25) * mid_r, o + std::sin(a + 0.25) * mid_r);
                V p3(o + std::cos(a) * (R - 3), o + std::sin(a) * (R - 3));
                draw::lines(s, Color(30, 40, 60, 220), false, {p1, p2, p3}, 2);
                draw::line(s, acc.withAlpha(230), p2, p3, 1);
            }
            draw::circle(s, acc.withAlpha(240), O, R, 3);
        } else if (style == Style::Voidcaller) {
            draw::circle(s, Color(20, 10, 44, int(120 * fade)), O, std::max(2, R - 4));
            Rng rng(uint64_t(box.gid * 17 + 3));
            for (int k = 0; k < 7; ++k) {
                double a = rng.uniform(0, TAU), d = rng.uniform(0.1, 0.75) * R;
                double tw = 0.5 + 0.5 * std::sin(frame * 0.4 + k);
                draw::circle(s, Color(255, 255, 255, int(150 + 100 * tw)), V(int(o + std::cos(a) * d), int(o + std::sin(a) * d)), 1 + (k % 2));
            }
            for (int k = 0; k < 5; ++k) {
                double a = k * TAU / 5 + frame * 0.12;
                draw::polygon(s, Color(255, 255, 255, 245), starPoints(o + std::cos(a) * (R - 3), o + std::sin(a) * (R - 3), 4 + R * 0.08, frame * 0.1));
            }
            draw::circle(s, acc.withAlpha(240), O, R, 2);
        } else {
            draw::circle(s, Color(240, 90, 20, int(130 * fade)), O, std::max(2, R - 2));
            draw::circle(s, Color(255, 200, 80, int(170 * fade)), O, int(R * 0.62));
            draw::circle(s, Color(255, 250, 220, int(200 * fade)), O, int(R * 0.3));
            Rng rng(uint64_t(frame * 7 + box.gid));
            for (int k = 0; k < 8; ++k) {
                double a = k * TAU / 8 + frame * 0.2;
                double ln = R * rng.uniform(0.75, 0.98);
                double px = -std::sin(a), py = std::cos(a);
                V base(o + std::cos(a) * R * 0.35, o + std::sin(a) * R * 0.35);
                draw::polygon(s, Color(255, 150, 40, 230), {{base.x - px * R * 0.16, base.y - py * R * 0.16},
                                                            {o + std::cos(a) * ln, o + std::sin(a) * ln},
                                                            {base.x + px * R * 0.16, base.y + py * R * 0.16}});
            }
            draw::circle(s, Color(255, 230, 120, 240), O, R, 2);
        }
        surf.blit(s, cx - o, cy - o);
    }
}

void drawDeathRay(Surface& surf, const Fighter& c) {
    int f = c.moveFrame, face = c.facing;
    double k = f < 104 ? std::min(1.0, f / 10.0) : std::max(0.0, (124 - f) / 20.0);
    if (k <= 0.02) return;
    k *= 1 + 0.12 * std::sin(std::min(f, 10) / 10.0 * PI);
    Rng& rnd = fxRng();
    double gx = c.x + face * (c.w * 0.5 + 64) + ((56 <= f && f < 66) ? -face * 16 : 0) + (f < 56 ? rnd.uniform(-2, 2) : 0);
    double gy = c.y - c.h * 0.5;
    auto R = [&](double x0, double y0, double x1, double y1) {
        double xa = gx + face * x0 * k, xb = gx + face * x1 * k;
        return Rect(int(std::min(xa, xb)), int(gy + y0 * k), int(std::abs(xb - xa)), int((y1 - y0) * k));
    };
    double charge = clampv(f / 56.0, 0.0, 1.0);
    Color hot = lerpColor(Color(90, 220, 255), Color(255, 80, 80), charge);
    draw::rect(surf, Color(48, 44, 62), R(-90, -22, 6, 30), 0, 8);
    draw::rect(surf, Color(70, 60, 50), R(24, 40, 52, 86), 0, 6);
    draw::rect(surf, Color(82, 86, 108), R(-12, -54, 168, 46), 0, 14);
    draw::rect(surf, OUTLINE, R(-12, -54, 168, 46), 4, 14);
    draw::ellipse(surf, hot, R(6, -100, 124, -40));
    draw::ellipse(surf, WHITE, R(26, -90, 76, -66));
    draw::ellipse(surf, OUTLINE, R(6, -100, 124, -40), 4);
    draw::rect(surf, Color(112, 116, 140), R(160, -38, 300, 30), 0, 6);
    for (int i = 0; i < 4; ++i) draw::rect(surf, hot, R(176 + i * 30, -44, 188 + i * 30, 36), 0, 3);
    draw::rect(surf, Color(48, 44, 62), R(292, -62, 336, 54), 0, 10);
    draw::rect(surf, hot, R(326, -46, 336, 38));
    for (int i = 0; i < 5; ++i)
        draw::line(surf, Color(255, 210, 60), V(gx + face * (10 + i * 28) * k, gy + 44 * k),
                   V(gx + face * (24 + i * 28) * k, gy + 22 * k), std::max(2, int(5 * k)));
    double mx = gx + face * 338 * k, my = gy - 4 * k;
    if (f < 56) {
        double rad = 8 + f * 1.4;
        glow(surf, Color(255, 90, 90), V(mx, my), rad * 2.2, 170);
        draw::circle(surf, WHITE, V(int(mx), int(my)), int(rad * 0.55));
        for (int i = 0; i < 8; ++i) {
            double a = rnd.uniform(0, TAU);
            double r1 = rad * 1.2, r2 = rad * 1.2 + rnd.uniform(40, 150);
            draw::line(surf, hot, V(mx + std::cos(a) * r2, my + std::sin(a) * r2), V(mx + std::cos(a) * r1, my + std::sin(a) * r1), 3);
        }
    } else if (f < 100) {
        double fade = f < 86 ? 1.0 : (100 - f) / 14.0;
        double hh = (300 + rnd.uniform(-24, 24)) * fade;
        double x_end = face > 0 ? WORLD_W + 200 : -200;
        double left = std::min(mx, x_end), width = std::abs(x_end - mx);
        const std::pair<Color, double> beam[3] = {{Color(255, 50, 60), 1.0}, {Color(255, 170, 110), 0.7}, {WHITE, 0.4}};
        for (auto& [color, kk] : beam)
            draw::rect(surf, color, Rect(int(left), int(my - hh * kk), int(width), int(hh * kk * 2)), 0, int(hh * kk * 0.5));
        const std::pair<Color, double> muzzle[3] = {{Color(255, 50, 60), 1.15}, {Color(255, 170, 110), 0.85}, {WHITE, 0.5}};
        for (auto& [color, kk] : muzzle) draw::circle(surf, color, V(int(mx), int(my)), int(hh * kk));
        for (int i = 0; i < 10; ++i) {
            double bx = left + rnd.uniform(0, width);
            draw::line(surf, WHITE, V(bx, my - hh), V(bx + rnd.uniform(-60, 60), my - hh - rnd.uniform(20, 90)), 4);
            draw::line(surf, WHITE, V(bx, my + hh), V(bx + rnd.uniform(-60, 60), my + hh + rnd.uniform(20, 90)), 4);
        }
    }
}

SurfacePtr makeIcon(int size) {
    auto s = Surface::make(size, size, true);
    double k = size / 256.0;
    Surface bg(size, size, true);
    for (int y = 0; y < size; ++y)
        draw::line(bg, lerpColor(Color(34, 20, 84), Color(12, 8, 34), double(y) / size), V(0, y), V(size, y));
    Surface mask(size, size, true);
    draw::rect(mask, WHITE, Rect(0, 0, size, size), 0, int(52 * k));
    bg.blit(mask, 0, 0, Blend::RGBA_MIN);
    s->blit(bg, 0, 0);
    double c = size / 2.0;
    glow(*s, Color(120, 230, 255), V(c, c), 120 * k, 150);
    slashArc(*s, V(c - 34 * k, c + 30 * k), 118 * k, -2.2, -0.2, 34 * k, Color(214, 48, 72), Color(255, 190, 190));
    slashArc(*s, V(c + 34 * k, c + 30 * k), 118 * k, PI + 0.2, PI + 2.2, 34 * k, Color(70, 130, 220), Color(190, 225, 255));
    draw::polygon(*s, Color(255, 214, 120), starPoints(c, c - 14 * k, 70 * k, -PI / 2));
    draw::polygon(*s, WHITE, starPoints(c, c - 14 * k, 40 * k, -PI / 2));
    draw::rect(*s, Color(255, 214, 120), Rect(3, 3, size - 6, size - 6), std::max(2, int(8 * k)), int(50 * k));
    return s;
}

SurfacePtr makeBackground(const Stage& stage) {
    auto bgp = Surface::make(WORLD_W, WORLD_H, false);
    Surface& bg = *bgp;
    Color top = stage.palette.skyTop, bottom = stage.palette.skyBottom;
    for (int y = 0; y < WORLD_H; ++y) bg.fill(lerpColor(top, bottom, std::pow(double(y) / WORLD_H, 1.3)), Rect(0, y, WORLD_W, 1));
    Rng rng(uint64_t(stage.name.size() * 97));
    if (stage.key == "atoll") {
        glow(bg, Color(255, 220, 160), V(1500, 880), 520, 90);
        draw::circle(bg, Color(255, 236, 190), V(1500, 880), 150);
        for (int i = 0; i < 26; ++i) {
            int x = rng.randint(0, WORLD_W), y = rng.randint(150, 1250), r = rng.randint(60, 190);
            Surface cl(r * 3, r, true);
            draw::ellipse(cl, Color(255, 225, 215, 40), cl.rect());
            bg.blit(cl, x - r, y);
        }
        for (int i = 0; i < 9; ++i) {
            int x = rng.randint(100, WORLD_W - 100), y = rng.randint(350, 1150), w = rng.randint(90, 260);
            Color c = lerpColor(Color(70, 60, 120), bottom, double(y) / WORLD_H * 0.6);
            draw::polygon(bg, c, {{double(x - w), double(y)}, {double(x + w), double(y)}, {x + w * 0.3, y + w * 0.9}, {x - w * 0.2, y + w * 0.6}});
            draw::rect(bg, lerpColor(c, Color(150, 220, 160), 0.4), Rect(x - w, y - 8, w * 2, 10), 0, 5);
        }
    } else if (stage.key == "loom") {
        for (int i = 0; i < 420; ++i) {
            int x = rng.randint(0, WORLD_W), y = rng.randint(0, WORLD_H);
            Color c = lerpColor(Color(120, 110, 170), WHITE, rng.random());
            draw::circle(bg, c, V(x, y), rng.choice(std::vector<int>{1, 1, 1, 2}));
        }
        glow(bg, Color(200, 190, 255), V(700, 420), 380, 70);
        draw::circle(bg, Color(232, 226, 255), V(700, 420), 130);
        draw::circle(bg, Color(200, 190, 240), V(660, 380), 28);
        draw::circle(bg, Color(206, 196, 244), V(740, 460), 18);
        Surface threads(WORLD_W, WORLD_H, true);
        for (int i = 0; i < 34; ++i) {
            int x = i * 76 - 60;
            draw::line(threads, Color(255, 200, 120, 26), V(x, 0), V(x + 420, WORLD_H), 3);
            draw::line(threads, Color(150, 140, 255, 22), V(x + 500, 0), V(x - 100, WORLD_H), 2);
        }
        bg.blit(threads, 0, 0);
        for (int x : {330, 2070}) {
            draw::rect(bg, Color(30, 24, 62), Rect(x - 40, 250, 80, WORLD_H));
            draw::ellipse(bg, Color(60, 46, 110), Rect(x - 110, 560, 220, 420));
            for (int k = 0; k < 8; ++k) draw::line(bg, Color(255, 200, 120), V(x - 96, 620 + k * 40), V(x + 96, 640 + k * 40), 2);
        }
    } else {
        glow(bg, Color(255, 120, 40), V(1200, 1500), 1100, 80);
        const std::pair<int, Color> layers[2] = {{780, Color(46, 24, 30)}, {930, Color(30, 16, 22)}};
        for (int layer = 0; layer < 2; ++layer) {
            int base = layers[layer].first;
            Pts pts{{0, double(WORLD_H)}};
            int x = 0;
            while (x < WORLD_W + 200) {
                pts.push_back({double(x), double(base - rng.randint(60, 380 - layer * 120))});
                x += rng.randint(120, 260);
                pts.push_back({double(x), double(base + rng.randint(0, 60))});
                x += rng.randint(40, 120);
            }
            pts.push_back({double(WORLD_W), double(WORLD_H)});
            draw::polygon(bg, layers[layer].second, pts);
        }
        for (int x : {420, 760, 1640, 1980}) {
            draw::rect(bg, Color(22, 14, 20), Rect(x - 34, 420, 68, WORLD_H));
            draw::rect(bg, Color(22, 14, 20), Rect(x - 50, 400, 100, 30));
            draw::circle(bg, Color(150, 230, 255), V(x, 480), 9);
            glow(bg, Color(150, 230, 255), V(x, 480), 40, 80);
        }
        draw::rect(bg, Color(255, 130, 40), Rect(0, 1290, WORLD_W, 110));
        draw::rect(bg, Color(255, 200, 90), Rect(0, 1290, WORLD_W, 8));
    }
    return bgp;
}

void drawPlatform(Surface& surf, const Platform& p, const StagePalette& pal) {
    int x = int(p.x), y = int(p.y), w = p.width, h = p.height;
    if (p.solid) {
        Pts pts{{double(x), double(y + 30)}, {double(x + w), double(y + 30)}};
        int n = int(p.jag.size());
        for (int i = 0; i < n; ++i)
            pts.push_back({x + w - (i + 0.5) * w / n, y + 30 + (h + 40) * p.jag[i] * std::pow(std::sin((i + 0.5) / n * PI), 0.6)});
        draw::polygon(surf, lerpColor(pal.rock, Color(0, 0, 0), 0.35), pts);
        draw::rect(surf, pal.rock, Rect(x, y, w, 44), 0, 10);
        draw::rect(surf, pal.top, Rect(x - 4, y - 2, w + 8, 14), 0, 7);
        draw::rect(surf, lerpColor(pal.top, WHITE, 0.5), Rect(x, y - 2, w, 4), 0, 2);
    } else {
        draw::rect(surf, lerpColor(pal.soft, Color(0, 0, 0), 0.55), Rect(x, y, w, h), 0, 7);
        draw::rect(surf, pal.soft, Rect(x, y, w, 5), 0, 3);
        if (p.moving) glow(surf, pal.soft, V(x + w / 2, y + h + 6), 22, 90);
    }
}

void drawProjectile(Surface& surf, const Projectile& p, int frame) {
    int x = int(p.x), y = int(p.y);
    Color c = p.color;
    int f = p.facing;
    if (p.kind == "needle") {
        draw::line(surf, c, V(x - f * 26, y), V(x + f * 12, y), 5);
        draw::line(surf, WHITE, V(x - f * 10, y), V(x + f * 16, y), 3);
    } else if (p.kind == "orb") {
        int r = 15 + int(std::sin(frame * 0.3) * 3);
        glow(surf, c, V(x, y), r * 2, 130);
        draw::circle(surf, WHITE, V(x, y), r - 6);
        draw::circle(surf, c, V(x, y), r, 4);
    } else if (p.kind == "lance") {
        glow(surf, c, V(x, y), 34, 110);
        draw::polygon(surf, c, {{double(x - f * 46), double(y)}, {double(x), double(y - 9)}, {double(x + f * 38), double(y)}, {double(x), double(y + 9)}});
        draw::line(surf, WHITE, V(x - f * 26, y), V(x + f * 34, y), 3);
    } else if (p.kind == "wall") {
        Rect r = p.rect();
        double fade = clampv(p.life / 40.0, 0.35, 1.0);
        Color stone = lerpColor(Color(40, 44, 56), Color(128, 136, 156), fade);
        draw::rect(surf, stone, r, 0, 6);
        draw::rect(surf, lerpColor(stone, Color(0, 0, 0), 0.4), r, 4, 6);
        draw::line(surf, c, V(r.centerx(), r.top() + 14), V(r.centerx(), r.bottom() - 14), 4);
        draw::line(surf, c, V(r.left() + 12, r.centery() - 16), V(r.right() - 12, r.centery() - 16), 4);
    } else if (p.kind == "well") {
        glow(surf, c, V(x, y), 70, 70);
        for (int i = 0; i < 3; ++i) {
            int rr = int(60 - pymod(frame * 1.5 + i * 20, 60));
            if (rr > 3) draw::circle(surf, c, V(x, y), rr, 3);
        }
        draw::circle(surf, Color(10, 4, 24), V(x, y), 12);
    }
}

// =====================================================================
// StateTranslator
// =====================================================================
static const char* attackLimb(Style style, const Hitbox& b) {
    if (std::abs(b.x) < 16 && std::abs(b.y) < 16) return b.radial ? "burst" : "spin";
    if (b.radial && b.y > 18) return "slam";
    if ((style == Style::Sentinel || style == Style::Kindled) && (b.y > 18 || b.x < -10)) return "foot";
    return "hand";
}

static std::vector<const Hitbox*> mainBoxes(const Move& m) {
    std::vector<const Hitbox*> out;
    for (auto& b : m.hitboxes) if (!b.spike) out.push_back(&b);
    if (out.empty()) for (auto& b : m.hitboxes) out.push_back(&b);
    return out;
}

AnimInfo StateTranslator::attackInfo(const Fighter& c) {
    const Move& m = *c.move;
    int f = c.moveFrame;
    Style style = c.spec->style;
    AnimInfo info;
    info.weapon = (style == Style::Sentinel && MAUL_SLOTS.count(m.slot)) ? "maul" : "shield";
    info.hot = true;
    auto boxes = mainBoxes(m);
    if (boxes.empty()) {
        int pf = m.projectile.on ? m.projectile.frame : (m.aim.on ? m.aim.a : m.duration / 3);
        info.limb = "cast";
        info.hasT = true;
        info.T = {double(c.w), -c.h * 0.6};
        info.phase = f < pf ? "wind" : f < pf + 9 ? "strike" : "recover";
        return info;
    }
    int s = boxes[0]->start, e = boxes[0]->end;
    for (auto* b : boxes) { s = std::min(s, b->start); e = std::max(e, b->end); }
    const Hitbox* box = nullptr;
    for (auto* b : boxes)
        if (b->start <= f && f <= b->end && (!box || b->w * b->h > box->w * box->h)) box = b;
    if (!box) box = boxes[0];
    std::string limb = attackLimb(style, *box);
    if (style == Style::Sentinel && info.weapon == "maul" && limb != "spin" && limb != "burst" && limb != "slam") limb = "hand";
    info.limb = limb;
    info.hasT = true;
    info.T = {box->x, -c.h / 2.0 + box->y};
    info.phase = f < s ? "wind" : f <= e + 2 ? "strike" : "recover";
    double sp = clampv(double(f - s + 1) / (e - s + 2), 0.0, 1.0);
    info.sprog = sp;
    info.spin = -360.0 * sp * c.facing;
    if (limb == "hand" && style == Style::Weaver) {
        info.swing = THRUSTS.count(m.slot) ? "thrust" : "swing";
        info.wlen = 50;
        info.sweep = 1.3;
    } else if (limb == "hand" && style == Style::Sentinel && info.weapon == "maul") {
        info.swing = "swing";
        info.wlen = 50;
        info.sweep = 1.1;
    }
    return info;
}

VisTrack& StateTranslator::track(Fighter& c) {
    if (!c.vis) {
        c.vis = std::make_shared<VisTrack>();
        auto& v = *c.vis;
        v.anim = c.anim;
        v.rollDir = c.facing;
        v.ground = c.ground != nullptr;
        v.jumps = c.airJumps;
    }
    VisTrack& vs = *c.vis;
    double dt = c.anim - vs.anim;
    vs.anim = c.anim;
    if (dt <= 0 || dt > 30) return vs;
    bool grounded = c.ground != nullptr;
    if (c.state == "RUNNING") {
        double k = clampv((std::abs(c.vx) / std::max(0.1, c.speed) - 0.35) / 0.55, 0.0, 1.0);
        vs.runk += (k - vs.runk) * 0.2;
        vs.stride = pymod(vs.stride + std::abs(c.vx) * dt / (4 * c.w * (0.34 + 0.3 * vs.runk)), 1.0);
    }
    if (grounded && !vs.ground && (c.state == "IDLE" || c.state == "RUNNING" || c.state == "JUMPING"))
        vs.land = vs.vy > 9 ? 11 : 7;
    if (grounded) {
        vs.jumps = c.airJumps;
    } else if (c.jumpsLeft < vs.jumps) {
        vs.jumps = c.jumpsLeft;
        if (c.spec->style == Style::Weaver || c.spec->style == Style::Kindled) vs.flip = 26;
    }
    if (c.state == "DODGE") {
        if (vs.roll == 0) vs.rollDir = std::abs(c.vx) > 1 ? sgn(c.vx) : 0;
        vs.roll += dt;
    } else {
        vs.roll = 0;
    }
    if (c.state != "JUMPING") vs.flip = 0;
    vs.land = std::max(0.0, vs.land - dt);
    vs.flip = std::max(0.0, vs.flip - dt);
    vs.ground = grounded;
    vs.vy = c.vy + c.kby;
    return vs;
}

Visuals StateTranslator::determineVisuals(Fighter& c) {
    const std::string& state = c.state;
    Visuals v;
    VisTrack vs = c.vis ? *c.vis : VisTrack();
    bool hasVis = bool(c.vis);
    if (state == "RUNNING") {
        v.key = "run";
    } else if ((state == "IDLE" && hasVis && vs.land) || state == "JUMPSQUAT") {
        v.key = "land";
    } else if (state == "JUMPING" && hasVis && vs.flip) {
        v.key = "tuck";
        v.rotation = -360.0 * (1 - vs.flip / 26) * c.facing;
    } else if (state == "JUMPING") {
        v.key = c.vy < 0 ? "jump" : "fall";
    } else if (state == "ATTACKING" && c.move) {
        const Move& m = *c.move;
        bool armored = m.armor.has(c.moveFrame);
        if (&m == &DEATH_RAY) {
            v.key = "gun";
            v.outline = Color(255, 90, 90);
        } else if (m.counter.on) {
            v.key = "shield";
            v.outline = Color(120, 255, 150);
        } else {
            v.key = "attack";
            v.info = attackInfo(c);
            v.outline = armored ? c.colors.accent : WHITE;
        }
    } else if (state == "HIT") {
        v.key = "hurt";
        v.tint = Color(255, 240, 120, c.hitstop > 0 ? 150 : 60);
        v.outline = Color(255, 230, 60);
        if (c.knockedDown) v.rotation = -c.anim * 14 * (c.kbx != 0 ? sgn(c.kbx) : 1);
    } else if (state == "STUN") {
        v.key = "stun";
        v.tint = Color(255, 255, 0, 70);
        v.outline = Color(255, 255, 0);
    } else if (state == "SHIELD") {
        v.key = "shield";
    } else if (state == "PARRY_SUCCESS") {
        v.key = "parry";
        v.tint = Color(0, 255, 90, 110);
        v.outline = Color(0, 255, 90);
    } else if (state == "DODGE") {
        if (hasVis && vs.rollDir && c.ground) {
            v.key = "tuck";
            v.alpha = 150;
            v.rotation = -360.0 * clampv(vs.roll / 22, 0.0, 1.0) * vs.rollDir;
        } else {
            v.key = "dodge";
            v.alpha = 110;
        }
    } else if (state == "LEDGE") {
        v.key = "ledge";
    } else if (state == "HELPLESS") {
        v.key = "helpless";
        v.tint = Color(0, 0, 0, 90);
    } else if (state == "GRABBING") {
        v.key = "grab";
        v.info = AnimInfo();
        v.info.pommel = c.pommelAnim;
        v.outline = WHITE;
    } else if (state == "GRABBED") {
        v.key = "grabbed";
        v.outline = Color(255, 230, 60);
    } else if (state == "RESPAWN") {
        v.outline = WHITE;
    }
    if (c.buffTimer > 0) v.info.hot = true;
    if (c.flash > 0 && !v.tint) v.tint = Color(255, 255, 255, 120);
    if (c.intangible > 0 && state != "DODGE" && state != "ATTACKING" && long(std::floor(c.anim / 4)) % 2) v.alpha = 120;
    if (c.move && c.move->aim.on && c.move->aim.a <= c.moveFrame && c.moveFrame <= c.move->aim.b) v.alpha = 0;
    return v;
}

SurfacePtr StateTranslator::spriteFor(Fighter& c, Visuals& v) {
    AnimInfo info = v.info;
    if (c.vis) {
        info.stride = c.vis->stride;
        info.runk = c.vis->runk;
    } else {
        info.stride = 0.0;
        info.runk = 1.0;
    }
    info.vy = c.vy + c.kby;
    Pose target = targetPose(*c.spec, v.key, c.anim, info);
    double rate = info.phase == "strike" ? 0.9 : v.key == "run" ? 0.5 : 0.42;
    std::optional<double> prevWp;
    if (c.pose) prevWp = c.pose->wp;
    if (!c.pose) c.pose = std::make_shared<Pose>(target);
    else *c.pose = blendPose(*c.pose, target, rate);
    if (info.swing == "swing" && info.phase == "strike" && prevWp && std::abs(c.pose->wp - *prevWp) > 0.12) info.smear = *prevWp;
    Vec2 vel((c.vx + c.kbx) * c.facing, c.vy + c.kby);
    if (!c.cloth) c.cloth = std::make_shared<ClothSim>();
    return renderFighterSurface(*c.spec, c.colors, *c.pose, c.facing, c.anim, v.key, vel, info, c.cloth.get(), {c.x, c.y});
}

void StateTranslator::updateVisuals(Surface& surface, const std::vector<Fighter*>& fighters, PhysicsEngine& physics, bool showBoxes) {
    for (Fighter* c : fighters) track(*c);
    for (Fighter* cp : fighters) {
        Fighter& c = *cp;
        if (c.state == "KO") continue;
        Visuals v = determineVisuals(c);
        if (c.state == "RESPAWN") {
            Rect r(int(c.x - 50), int(c.y - 4), 100, 16);
            draw::ellipse(surface, WHITE, r);
            draw::ellipse(surface, c.colors.accent, r, 3);
        }
        if (v.alpha > 0) {
            SurfacePtr spr = spriteFor(c, v);
            if (v.tint) {
                Color tc = *v.tint;
                double k = tc.a / 255.0;
                if (tc.r == 0 && tc.g == 0 && tc.b == 0) {
                    int g = int(255 - 255 * k);
                    spr->fill(Color(g, g, g, 255), Blend::RGBA_MULT);
                } else {
                    spr->fill(Color(int(tc.r * k), int(tc.g * k), int(tc.b * k), 0), Blend::RGBA_ADD);
                }
            }
            if (v.outline) outlineSilhouette(*spr, 200, *v.outline, 2);
            if (v.alpha < 255) spr->setAlpha(v.alpha);
            double rotation = v.rotation ? v.rotation : (c.pose ? c.pose->spin : 0);
            if (rotation) {
                Surface pivot(SPR, SPR, true);
                pivot.blit(*spr, 0, SPR / 2 - (SPR_FEET - c.h / 2));
                auto rot = xform::rotate(pivot, rotation);
                if (v.alpha < 255) rot->setAlpha(v.alpha);
                Rect rr = rot->rect();
                rr.setCenter(c.x, c.y - c.h / 2.0);
                surface.blit(*rot, rr.x, rr.y);
            } else if (std::abs(c.squash) > 0.02) {
                double kx = 1 - c.squash * 0.6, ky = 1 + c.squash;
                auto spr2 = xform::smoothscale(*spr, int(SPR * kx), int(SPR * ky));
                if (v.alpha < 255) spr2->setAlpha(v.alpha);
                surface.blit(*spr2, c.x - SPR_CX * kx, c.y - SPR_FEET * ky);
            } else {
                surface.blit(*spr, c.x - SPR_CX, c.y - SPR_FEET);
            }
        }
        if (c.move == &DEATH_RAY) {
            drawDeathRay(surface, c);
        } else {
            for (const Hitbox* box : physics.activeHitboxes(c))
                drawAttackFx(surface, c, *box, PhysicsEngine::hitboxCircles(c, *box), physics.frame);
        }
        Vec2 cc = c.center();
        if (c.state == "SHIELD") {
            int rad = int((c.h * 0.42 + 14) * (0.55 + 0.45 * c.shieldHp / SHIELD_MAX));
            if (rad > 0) {
                Surface bubble(rad * 2, rad * 2, true);
                Color colr = c.shieldFrame <= PARRY_WINDOW ? Color(120, 255, 150) : PLAYER_TAGS[c.slot % 2];
                draw::circle(bubble, colr.withAlpha(100), V(rad, rad), rad);
                draw::circle(bubble, Color(255, 255, 255, 200), V(rad, rad), rad, 3);
                surface.blit(bubble, cc.x - rad, cc.y - rad);
            }
        }
        if (c.state == "STUN") {
            for (int i = 0; i < 3; ++i) {
                double a = c.anim * 0.15 + i * TAU / 3;
                draw::polygon(surface, Color(255, 255, 0), starPoints(c.x + std::cos(a) * 26, c.y - c.h - 14 + std::sin(a) * 7, 7, c.anim * 0.1));
            }
        }
        if (showBoxes) {
            draw::rect(surface, Color(0, 255, 0), c.rect(), 2);
            for (const Hitbox* box : physics.activeHitboxes(c))
                for (auto& ci : PhysicsEngine::hitboxCircles(c, *box))
                    if (ci[2] < 1000)
                        draw::circle(surface, box->spike ? Color(255, 0, 255) : Color(255, 0, 0), V(int(ci[0]), int(ci[1])), int(ci[2]), 2);
        }
    }
}
