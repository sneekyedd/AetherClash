// Skeleton poses, pose blending and the cloth (rope) simulation.
#include <cmath>

#include "art.h"

int SS = 2;

std::vector<Vec2> starPoints(double x, double y, double r, double rot) {
    std::vector<Vec2> out;
    for (int i = 0; i < 8; ++i) {
        double rr = (i % 2 == 0) ? r : r * 0.36;
        out.push_back({x + std::cos(rot + i * PI / 4) * rr, y + std::sin(rot + i * PI / 4) * rr});
    }
    return out;
}

static double strideHalf(double w, double k) { return w * (0.34 + 0.3 * k); }

Pose targetPose(const FighterSpec& spec, const std::string& key, double t, const AnimInfo& info) {
    const auto& st = spec.stats;
    double w = st.w, h = st.h;
    Style style = spec.style;
    double hipy = -h * HIP_K, shy = -h * SH_K;
    Pose P;
    P.hip = {0.0, hipy};
    P.sh = {0.0, shy};
    P.head = {0.0, 0.0};
    P.hf = {w * 0.42, shy + h * 0.14};
    P.hb = {w * 0.05, shy + h * 0.2};
    P.ff = {w * 0.3, 0.0};
    P.fb = {-w * 0.28, 0.0};
    P.spin = 0.0;
    P.wp = -0.9;
    double br = std::sin(t * 0.07);
    bool hasGait = false;
    double gaitSw = 0, gaitBob = 0;
    if (key == "idle") {
        P.sh.y += br * 1.5;
        P.hf.y += br * 2.5;
        P.hb.y += br * 2;
        if (style == Style::Kindled) {
            double b = std::abs(std::sin(t * 0.12)) * 3;
            P.hip.y -= b;
            P.sh.y -= b;
            P.hf = {w * 0.5, shy + h * 0.06 - b};
            P.hb = {w * 0.22, shy + h * 0.12 - b};
        }
    } else if (key == "run") {
        double ph = info.stride ? *info.stride : pymod(t * 0.045, 1.0);
        double k = info.runk ? *info.runk : 1.0;
        double S = strideHalf(w, k), lift = h * (0.06 + 0.12 * k);
        auto foot = [&](double q) -> Vec2 {
            if (q < 0.5) return {S * (1 - 4 * q), 0.0};
            double u = (q - 0.5) * 2;
            double e = u * u * (3 - 2 * u);
            return {S * (-1 + 2 * e), -lift * std::sin(PI * u) - h * 0.02 * k * std::sin(PI * u)};
        };
        P.ff = foot(ph);
        P.fb = foot(pymod(ph + 0.5, 1.0));
        double bob = (0.5 + 0.5 * std::cos(4 * PI * ph)) * h * (0.02 + 0.035 * k);
        double lean = h * (0.03 + 0.11 * k);
        P.hip = {lean * 0.3, hipy + bob};
        P.sh = {lean, shy + bob * 1.15};
        P.head = {lean * 0.15, 0.0};
        double sw = std::cos(2 * PI * ph);
        double A = w * (0.22 + 0.32 * k);
        P.hf = {lean + w * 0.06 - A * sw, shy + h * 0.25 + bob - std::abs(sw) * h * 0.04 * k};
        P.hb = {lean - w * 0.06 + A * sw, shy + h * 0.25 + bob - std::abs(sw) * h * 0.04 * k};
        P.wp = -0.3 + 0.4 * sw;
        hasGait = true;
        gaitSw = sw;
        gaitBob = bob;
    } else if (key == "jump") {
        double r = clampv(-(info.vy ? *info.vy : -8.0) / 12.0, 0.0, 1.0);
        P.ff = {w * (0.18 + 0.08 * r), -h * (0.06 + 0.17 * r)};
        P.fb = {-w * (0.30 - 0.1 * r), -h * (0.02 + 0.1 * r)};
        P.hf = {w * (0.36 - 0.1 * r), shy - h * (0.02 + 0.12 * r)};
        P.hb = {-w * (0.34 - 0.08 * r), shy + h * (0.06 - 0.1 * r)};
        P.sh.x = h * 0.03 * r;
        P.wp = -1.2 - 0.3 * r;
    } else if (key == "fall" || key == "helpless") {
        double r = clampv((info.vy ? *info.vy : 4.0) / 10.0, 0.0, 1.0);
        double wig = key == "helpless" ? std::sin(t * 0.5) * 4 : 0;
        P.ff = {w * (0.28 - 0.06 * r), -h * (0.08 - 0.06 * r)};
        P.fb = {-w * (0.24 + 0.04 * r), -h * (0.14 - 0.06 * r)};
        P.hf = {w * 0.5 + wig, shy - h * (0.04 + 0.12 * r)};
        P.hb = {-w * 0.5 - wig, shy - h * (0.02 + 0.12 * r)};
        P.sh.x = key == "fall" ? -2 : -h * 0.06;
        P.wp = -1.3 - 0.3 * r;
    } else if (key == "tuck") {
        P.hip = {0.0, hipy - h * 0.06};
        P.sh = {h * 0.12, shy + h * 0.1};
        P.head = {3.0, 4.0};
        P.ff = {w * 0.3, -h * 0.34};
        P.fb = {w * 0.08, -h * 0.28};
        P.hf = {w * 0.36, shy + h * 0.3};
        P.hb = {w * 0.2, shy + h * 0.32};
        P.wp = 1.9;
    } else if (key == "land") {
        P.hip = {0.0, hipy + h * 0.1};
        P.sh = {h * 0.08, shy + h * 0.13};
        P.head = {1.0, 2.0};
        P.ff = {w * 0.42, 0.0};
        P.fb = {-w * 0.4, 0.0};
        P.hf = {w * 0.52, shy + h * 0.3};
        P.hb = {-w * 0.34, shy + h * 0.28};
        P.wp = 0.5;
    } else if (key == "hurt") {
        P.sh = {-h * 0.13, shy + 2};
        P.head = {-3.0, 2.0};
        P.hf = {w * 0.5, shy - h * 0.02};
        P.hb = {w * 0.3, shy + h * 0.12};
        P.ff = {w * 0.42, -h * 0.1};
        P.fb = {w * 0.1, -h * 0.02};
        P.wp = 0.6;
    } else if (key == "stun") {
        double sway = std::sin(t * 0.12) * h * 0.06;
        P.sh = {sway, shy + h * 0.05};
        P.hip = {sway * 0.4, hipy + h * 0.03};
        P.hf = {sway + w * 0.12, shy + h * 0.38};
        P.hb = {sway - w * 0.12, shy + h * 0.38};
        P.head = {sway * 0.5, 3.0};
        P.wp = 1.3;
    } else if (key == "shield") {
        P.hip.y += h * 0.05;
        P.sh.y += h * 0.06;
        P.hf = {w * 0.46, shy + h * 0.1};
        P.hb = {w * 0.34, shy + h * 0.2};
        P.ff = {w * 0.36, 0.0};
        P.fb = {-w * 0.34, 0.0};
        P.wp = -1.5;
    } else if (key == "parry") {
        P.hf = {w * 0.8, shy + h * 0.02};
        P.hb = {-w * 0.3, shy + h * 0.22};
        P.sh = {h * 0.06, shy};
        P.wp = -1.3;
    } else if (key == "dodge") {
        P.sh = {h * 0.2, shy + h * 0.14};
        P.hip = {0.0, hipy + h * 0.08};
        P.hf = {w * 0.2, shy + h * 0.36};
        P.hb = {-w * 0.3, shy + h * 0.32};
        P.ff = {w * 0.4, 0.0};
        P.fb = {-w * 0.4, -h * 0.04};
        P.wp = 2.4;
    } else if (key == "ledge") {
        Vec2 lip(w / 2 + 4, -(h - 8));
        double sw = std::sin(t * 0.06) * 3;
        P.hf = lip;
        P.hb = {lip.x - 8, lip.y};
        P.sh = {2.0, shy + 5};
        P.ff = {sw - 2, -2.0};
        P.fb = {sw - 9, -6.0};
        P.wp = 1.6;
    } else if (key == "grab") {
        int pm = info.pommel;
        P.hf = {w * 0.85, shy + h * 0.06};
        P.hb = pm <= 0 ? Vec2(w * 0.7, shy + h * 0.16) : (pm > 5 ? Vec2(-w * 0.2, shy - h * 0.06) : Vec2(w * 0.9, shy + h * 0.02));
        P.sh = {h * 0.07, shy};
        P.wp = -1.4;
    } else if (key == "grabbed") {
        P.hf = {w * 0.12, shy + h * 0.32};
        P.hb = {-w * 0.1, shy + h * 0.32};
        P.ff = {w * 0.1, -4.0};
        P.fb = {-w * 0.12, -2.0};
        P.head = {-2.0, 2.0};
        P.sh.x = -3.0;
        P.wp = 1.4;
    } else if (key == "gun") {
        P.hf = {w * 0.6, shy + h * 0.2};
        P.hb = {w * 0.3, shy + h * 0.26};
        P.sh = {-h * 0.06, shy + 2};
        P.ff = {w * 0.45, 0.0};
        P.fb = {-w * 0.5, 0.0};
    } else if (key == "attack") {
        Vec2 T = info.hasT ? info.T : Vec2(w, shy);
        double tx = T.x, ty = T.y;
        std::string ph = info.phase.empty() ? "strike" : info.phase;
        std::string limb = info.limb.empty() ? "hand" : info.limb;
        double sx = P.sh.x, sy = P.sh.y;
        double n = std::hypot(tx - sx, ty - sy);
        if (n == 0) n = 1;
        double dx = (tx - sx) / n, dy = (ty - sy) / n;
        double ang = std::atan2(dy, dx);
        const std::string& mode = info.swing;
        if (limb == "hand" && !mode.empty()) {
            double L = info.wlen, sp = info.sprog;
            double reach = clampv(n - L * 0.62, h * 0.18, h * 0.38);
            P.hb = {-dx * w * 0.4, shy + h * 0.22};
            double wp, hand_r;
            if (mode == "thrust") {
                wp = ang;
                double kk = ph == "wind" ? 0.45 : ph == "strike" ? 0.8 + 0.2 * std::min(1.0, sp * 2.5) : 0.7;
                hand_r = kk * reach;
                if (ph == "wind") {
                    P.sh.x -= dx * h * 0.06;
                    P.hip.x -= dx * h * 0.03;
                } else if (ph == "strike") {
                    P.sh = {sx + dx * h * 0.12, sy + std::max(0.0, dy) * h * 0.08};
                    P.hip.x += dx * h * 0.05;
                    P.ff.x += w * 0.22 * (dx > 0 ? 1 : 0);
                }
            } else {
                double sweep = info.sweep;
                double a0 = ang - sweep, a1 = ang + sweep * 0.85;
                if (ph == "wind") {
                    wp = a0 - 0.4;
                    hand_r = reach * 0.8;
                    P.sh.x -= dx * h * 0.05;
                } else if (ph == "strike") {
                    wp = a0 + (a1 - a0) * (sp * sp * (3 - 2 * sp));
                    hand_r = reach;
                    P.sh = {sx + dx * h * 0.08, sy + std::max(0.0, dy) * h * 0.06};
                    P.hip.x += dx * h * 0.04;
                    P.ff.x += w * 0.14 * (dx > 0 ? 1 : 0);
                } else {
                    wp = a1 + 0.15;
                    hand_r = reach * 0.85;
                }
            }
            P.hf = {sx + std::cos(wp) * hand_r, sy + std::sin(wp) * hand_r};
            P.wp = wp;
        } else if (limb == "hand") {
            P.hb = {-dx * w * 0.4, shy + h * 0.2};
            if (ph == "wind") {
                P.hf = {sx - dx * h * 0.24, sy - dy * h * 0.24 - h * 0.08};
                P.sh.x -= dx * h * 0.06;
                P.wp = ang - 1.9;
            } else if (ph == "strike") {
                P.hf = {tx, ty};
                P.sh = {sx + dx * h * 0.1, sy + std::max(0.0, dy) * h * 0.08};
                P.hip.x += dx * h * 0.04;
                P.ff.x += w * 0.16 * (dx > 0 ? 1 : 0);
                P.wp = ang + 0.3;
            } else {
                P.hf = {sx + dx * h * 0.2, sy + dy * h * 0.2 + h * 0.08};
                P.wp = ang + 1.0;
            }
        } else if (limb == "foot") {
            bool front = tx >= 0;
            Vec2& fk = front ? P.ff : P.fb;
            Vec2& other = front ? P.fb : P.ff;
            double sg = front ? 1 : -1;
            P.hf = {w * 0.34, shy + h * 0.06};
            P.hb = {-w * 0.42, shy + h * 0.12};
            if (ph == "wind") {
                fk = {sg * w * 0.12, -h * 0.22};
            } else if (ph == "strike") {
                fk = {tx, ty};
                P.sh.x -= sg * h * 0.1;
                if (ty > -h * 0.1 && std::abs(tx) < w * 0.5) other = {-sg * w * 0.2, -h * 0.2};
            }
        } else if (limb == "spin") {
            P.spin = info.spin;
            P.hf = {w * 0.85, shy + h * 0.1};
            P.hb = {-w * 0.85, shy + h * 0.1};
            P.ff = {w * 0.4, -h * 0.06};
            P.fb = {-w * 0.4, -h * 0.1};
            P.wp = 0.0;
        } else if (limb == "burst") {
            if (ph == "wind") {
                P.hf = {w * 0.15, shy + h * 0.12};
                P.hb = {w * 0.0, shy + h * 0.14};
                P.hip.y += h * 0.06;
                P.sh.y += h * 0.08;
            } else {
                P.hf = {w * 0.8, shy - h * 0.16};
                P.hb = {-w * 0.8, shy - h * 0.16};
                P.sh.y -= 3;
                P.head = {0.0, -2.0};
            }
            P.wp = -1.57;
        } else if (limb == "slam") {
            if (ph == "wind") {
                P.hf = {w * 0.05, shy - h * 0.3};
                P.hb = {-w * 0.05, shy - h * 0.28};
                P.sh = {-h * 0.04, shy - 3};
                P.wp = -2.3;
            } else {
                P.hf = {w * 0.36, -h * 0.34};
                P.hb = {w * 0.22, -h * 0.32};
                P.sh = {h * 0.13, shy + h * 0.17};
                P.hip.y += h * 0.08;
                P.ff.x += w * 0.1;
                P.wp = 0.42;
            }
        } else if (limb == "cast") {
            if (ph == "wind") {
                P.hf = {-w * 0.25, shy + h * 0.08};
                P.wp = -2.2;
            } else if (ph == "strike") {
                P.hf = {w * 0.95, shy + h * 0.04};
                P.sh.x += h * 0.06;
                P.wp = 0.0;
            }
        }
    }
    // ---- character-specific habits
    if (style == Style::Weaver) {
        if (key == "run") {
            P.wp = 2.45 + 0.25 * gaitSw;
        } else if (key == "idle") {
            P.hf = {w * 0.46, shy + h * 0.2 + br * 1.5};
            P.hb = {-w * 0.18, shy + h * 0.06};
            P.ff = {w * 0.36, 0.0};
            P.fb = {-w * 0.36, 0.0};
            P.wp = -0.55 + br * 0.05;
        }
    } else if (style == Style::Sentinel && (key == "idle" || key == "run" || key == "jump" || key == "fall" ||
                                            key == "shield" || key == "parry" || key == "land")) {
        double bob = hasGait ? gaitBob : 0.0;
        double sx = key == "run" ? P.sh.x : 0;
        P.hf = {w * 0.44 + sx, shy + h * 0.2 + bob};
        P.hb = {-w * 0.34 + sx, shy - h * 0.02 + bob};
    } else if (style == Style::Kindled && key == "run") {
        P.hf = {P.sh.x + w * 0.42 + gaitSw * w * 0.08, shy + h * 0.06 + gaitBob};
        P.hb = {P.sh.x + w * 0.22 - gaitSw * w * 0.08, shy + h * 0.1 + gaitBob};
    } else if (style == Style::Voidcaller) {
        if (key == "run") {
            P.hf = {-w * 0.2, shy + h * 0.26};
            P.hb = {-w * 0.55, shy + h * 0.2};
        } else if (key == "idle") {
            P.hf = {w * 0.58, shy + h * 0.2 + br * 2};
        }
    }
    if (style == Style::Voidcaller) {
        bool strike = key == "attack" && info.limb == "hand" && info.phase == "strike";
        P.orb = strike ? info.T : Vec2(P.hf.x + 9, P.hf.y - 9);
        P.hasOrb = true;
    }
    return P;
}

Pose blendPose(const Pose& c, const Pose& t, double rate) {
    auto L = [rate](Vec2 a, Vec2 b) { return Vec2(a.x + (b.x - a.x) * rate, a.y + (b.y - a.y) * rate); };
    Pose o;
    o.hip = L(c.hip, t.hip);
    o.sh = L(c.sh, t.sh);
    o.head = L(c.head, t.head);
    o.hf = L(c.hf, t.hf);
    o.hb = L(c.hb, t.hb);
    o.ff = L(c.ff, t.ff);
    o.fb = L(c.fb, t.fb);
    o.hasOrb = t.hasOrb;
    if (t.hasOrb) o.orb = c.hasOrb ? L(c.orb, t.orb) : t.orb;
    o.spin = t.spin;
    o.wp = c.wp + (t.wp - c.wp) * rate;
    return o;
}

std::vector<Vec2> ClothSim::chain(const std::string& name, Vec2 anchor, int n, double seg, double gravity, double damp,
                                  double wave, double t, double breeze) {
    double ax = ox + anchor.x * f, ay = oy + anchor.y;
    auto it = chains.find(name);
    if (it == chains.end() || int(it->second.size()) != n + 1 ||
        std::abs(it->second[0].x - ax) + std::abs(it->second[0].y - ay) > 160) {
        double d = gravity >= 0 ? 1 : -1;
        std::vector<P4> pts;
        for (int i = 0; i <= n; ++i) {
            double x = ax - f * i * seg * 0.3, y = ay + d * i * seg * 0.95;
            pts.push_back({x, y, x, y});
        }
        it = chains.insert_or_assign(name, std::move(pts)).first;
    }
    auto& pts = it->second;
    pts[0].x = ax;
    pts[0].y = ay;
    double phase = name.size() * 1.7;
    for (int i = 1; i <= n; ++i) {
        P4& p = pts[i];
        double vx = (p.x - p.px) * damp, vy = (p.y - p.py) * damp;
        p.px = p.x;
        p.py = p.y;
        p.x += vx + std::sin(t * 0.21 + i * 1.1 + phase) * wave - f * breeze;
        p.y += vy + gravity;
    }
    for (int k = 0; k < 2; ++k)
        for (int i = 1; i <= n; ++i) {
            P4 &a = pts[i - 1], &b = pts[i];
            double dx = b.x - a.x, dy = b.y - a.y;
            double d = std::hypot(dx, dy);
            if (d == 0) d = 0.001;
            b.x = a.x + dx / d * seg;
            b.y = a.y + dy / d * seg;
        }
    std::vector<Vec2> out;
    for (auto& p : pts) out.push_back({(p.x - ox) * f, p.y - oy});
    return out;
}
