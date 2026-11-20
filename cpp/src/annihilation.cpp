// THE DEATH RAY ENDING (the crypticsh secret). Firing the Death Ray ends the match on the spot: the beam whites
// out the screen, the blast fills it with burning rock, then the camera pulls back into space: the planet has been
// struck, molten cracks spread across it, it shakes, swells and bursts apart along those cracks, and the title slams in.
#include <algorithm>
#include <cmath>

#include "game.h"

double easeOut(double u) { u = clampv(u, 0.0, 1.0); return 1 - std::pow(1 - u, 3); }
double easeIn(double u) { u = clampv(u, 0.0, 1.0); return u * u * u; }

static void softBlob(Surface& surf, Color c, int alpha, Vec2 pos, double r_) {
    int r = std::max(1, int(r_));
    Surface tmp(r * 2, r * 2, true);
    draw::circle(tmp, c.withAlpha(alpha), Vec2(r, r), r);
    surf.blit(tmp, pos.x - r, pos.y - r);
}

static void whiteOverlay(Surface& scr, int a, Color c = Color(255, 246, 230)) {
    if (a <= 0) return;
    Surface ov(scr.w, scr.h, false);
    ov.fill(c);
    ov.setAlpha(a);
    scr.blit(ov, 0, 0);
}

static double dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

Annihilation::Annihilation(Game* game, Fighter* sh) : g(game), shooter(sh), name(sh->playerName) {
    double R = ER;
    P = {R + 0.5 * R, R - 0.3 * R};
    buildSpace();
    buildBlast();
    buildEarth();
    buildCracks();
    glowLayer = Surface::make(2 * ER, 2 * ER, true);
    coreLayer = Surface::make(2 * ER, 2 * ER, true);
    viewX = W / 2.0;
    viewY = H / 2.0;
    viewR = R;
    boomX = W / 2.0;
    boomY = H / 2.0;
}

// ------------------------------------------------------------ building the pieces
void Annihilation::buildSpace() {
    space = Surface::make(W, H, false);
    Surface& bg = *space;
    bg.fill(Color(3, 3, 10));
    Vec2 a(W * 0.80, -90), b(W * 0.30, H + 90);
    double dx = b.x - a.x, dy = b.y - a.y, L = std::hypot(dx, dy);
    double nx = -dy / L, ny = dx / L;
    auto band = [&](double u, double off) { return Vec2(a.x + dx * u + nx * off, a.y + dy * u + ny * off); };
    const std::vector<Color> dust = {Color(150, 118, 88), Color(112, 92, 80), Color(176, 150, 126)};
    for (int i = 0; i < 150; ++i) {
        Color c = rng.choice(dust);
        double u = rng.random(), off = rng.gauss(0, 62);
        int rad = rng.randint(24, 78), al = rng.randint(16, 34);
        glow(bg, c, band(u, off), rad, al);
    }
    for (int i = 0; i < 70; ++i) {
        double u = rng.random(), off = rng.gauss(0, 22);
        softBlob(bg, Color(4, 2, 2), 70, band(u, off), rng.randint(8, 30));
    }
    const std::vector<Color> bases = {Color(255, 255, 255), Color(200, 214, 255), Color(255, 232, 200)};
    for (int i = 0; i < 1300; ++i) {
        Vec2 p;
        if (i < 700) { double u = rng.random(); p = band(u, rng.gauss(0, 75)); }
        else { double x = rng.uniform(0, W); p = Vec2(x, rng.uniform(0, H)); }
        Color base = rng.choice(bases);
        double k = std::pow(rng.uniform(0.2, 1.0), 1.6);
        Color col(int(base.r * k), int(base.g * k), int(base.b * k));
        if (rng.random() < 0.025) {
            glow(bg, base, p, 6, 90);
            draw::circle(bg, base, Vec2(int(p.x), int(p.y)), 1);
        } else if (p.x >= 0 && p.x < W && p.y >= 0 && p.y < H) {
            bg.setAt(int(p.x), int(p.y), col);
        }
    }
    for (int i = 0; i < 46; ++i) {
        double x = rng.uniform(0, W), y = rng.uniform(0, H), ph = rng.uniform(0, 6.3);
        twinkle.push_back({x, y, ph});
    }
}

void Annihilation::buildBlast() {
    blastBg = Surface::make(W, H, false);
    blastBg->fill(Color(40, 6, 0));
    for (int i = 0; i < 48; ++i) {
        double u = i / 47.0;
        Color col = u < 0.6 ? lerpColor(Color(96, 16, 0), Color(255, 140, 40), u / 0.6)
                            : lerpColor(Color(255, 140, 40), Color(255, 246, 214), (u - 0.6) / 0.4);
        draw::circle(*blastBg, col, Vec2(W / 2, H / 2), int(820 * (1 - u) + 20));
    }
    for (int i = 0; i < 80; ++i) {
        int size = rng.randint(10, 46);
        Rock r;
        r.img = makeRock(size);
        r.a = rng.uniform(0, TAU);
        r.d0 = rng.uniform(0, 140);
        r.v = rng.uniform(2.0, 9.0);
        r.acc = rng.uniform(0.05, 0.35);
        r.rot = rng.uniform(0, 360);
        r.spin = rng.uniform(-5, 5);
        r.s0 = rng.uniform(0.25, 0.7);
        r.grow = rng.uniform(0.01, 0.05);
        r.delay = rng.randint(0, 22);
        rocks.push_back(r);
    }
}

SurfacePtr Annihilation::makeRock(int size) {
    int n = rng.randint(6, 10);
    std::vector<double> angs;
    for (int i = 0; i < n; ++i) angs.push_back(rng.uniform(0, TAU));
    std::sort(angs.begin(), angs.end());
    int c = size + 3;
    std::vector<Vec2> pts;
    for (double a : angs) {
        double k1 = rng.uniform(0.62, 1.0), k2 = rng.uniform(0.62, 1.0);
        pts.push_back({c + std::cos(a) * size * k1, c + std::sin(a) * size * k2});
    }
    auto s = Surface::make(c * 2, c * 2, true);
    draw::polygon(*s, Color(68, 40, 28), pts);
    std::vector<Vec2> inner;
    for (auto& p : pts) inner.push_back({c + (p.x - c) * 0.6 + size * 0.15, c + (p.y - c) * 0.6 + size * 0.12});
    draw::polygon(*s, Color(40, 22, 16), inner);
    for (int i = 0; i < 3; ++i) {
        double a = rng.uniform(0, TAU);
        draw::line(*s, Color(255, 150, 50), Vec2(c + std::cos(a) * size * 0.2, c + std::sin(a) * size * 0.2),
                   Vec2(c + std::cos(a) * size * 0.8, c + std::sin(a) * size * 0.8), 2);
    }
    draw::polygon(*s, Color(255, 170, 70), pts, 2);
    return s;
}

void Annihilation::buildEarth() {
    const int n = 2 * ER;
    const double R = ER;
    Rng rs(11);
    auto noise = [&](int cw, int ch) {
        std::vector<float> grid(size_t(ch + 2) * (cw + 1));
        for (auto& v : grid) v = float(rs.random());
        for (int y = 0; y < ch + 2; ++y) grid[size_t(y) * (cw + 1) + cw] = grid[size_t(y) * (cw + 1)];
        std::vector<float> out(size_t(TH) * TW);
        for (int j = 0; j < TH; ++j) {
            double ys = TH > 1 ? double(ch) * j / (TH - 1) : 0;
            int y0 = int(std::floor(ys));
            double fy = ys - y0;
            fy = fy * fy * (3 - 2 * fy);
            int y1 = std::min(y0 + 1, ch + 1);
            for (int i = 0; i < TW; ++i) {
                double xs = double(cw) * i / TW;
                int x0 = int(std::floor(xs));
                double fx = xs - x0;
                fx = fx * fx * (3 - 2 * fx);
                int x1 = x0 + 1;
                auto G = [&](int y, int x) { return grid[size_t(y) * (cw + 1) + x]; };
                double a = G(y0, x0), b = G(y0, x1), c = G(y1, x0), d = G(y1, x1);
                out[size_t(j) * TW + i] = float((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy);
            }
        }
        return out;
    };
    auto fbm = [&](int cw, int ch, int octaves, double gain = 0.5) {
        std::vector<float> total(size_t(TH) * TW, 0.0f);
        double amp = 1.0, norm = 0;
        for (int o = 0; o < octaves; ++o) {
            auto nz = noise(cw << o, ch << o);
            for (size_t k = 0; k < total.size(); ++k) total[k] += float(nz[k] * amp);
            norm += amp;
            amp *= gain;
        }
        for (auto& v : total) v = float(v / norm);
        return total;
    };
    auto f = fbm(6, 3, 6, 0.6);
    std::vector<float> sorted = f;
    std::sort(sorted.begin(), sorted.end());
    double qi = 0.68 * (sorted.size() - 1);
    size_t q0 = size_t(qi);
    double thr = sorted[q0] + (sorted[std::min(q0 + 1, sorted.size() - 1)] - sorted[q0]) * (qi - q0);
    auto n2 = fbm(10, 5, 4);
    auto cloud = fbm(8, 3, 5, 0.6);
    auto cityNoise = noise(70, 35);
    size_t N = size_t(TH) * TW;
    tex.assign(N * 3, 0);
    lights.assign(N, 0);
    for (int j = 0; j < TH; ++j) {
        double lat = (0.5 - (j + 0.5) / TH) * PI;
        for (int i = 0; i < TW; ++i) {
            size_t k = size_t(j) * TW + i;
            bool land = f[k] > thr;
            double deep = clampv((thr - f[k]) / 0.18, 0.0, 1.0);
            double oc[3] = {26 * (1 - deep) + 8 * deep, 90 * (1 - deep) + 26 * deep, 150 * (1 - deep) + 66 * deep};
            double dry = clampv((0.3 - std::abs(std::abs(lat) - 0.4)) * 3.0 + (n2[k] - 0.5) * 0.9, 0.0, 1.0);
            double height = clampv((f[k] - thr) / 0.2, 0.0, 1.0);
            double lc[3] = {50 * (1 - dry) + 160 * dry, 98 * (1 - dry) + 138 * dry, 42 * (1 - dry) + 90 * dry};
            const double rockc[3] = {72, 74, 54};
            for (int c = 0; c < 3; ++c) lc[c] = lc[c] * (1 - 0.35 * height) + rockc[c] * 0.35 * height;
            double col[3];
            for (int c = 0; c < 3; ++c) col[c] = land ? lc[c] : oc[c];
            bool ice = (std::abs(lat) + (n2[k] - 0.5) * 0.3) > 1.2;
            if (ice) { col[0] = 236; col[1] = 242; col[2] = 248; }
            double ca = clampv((cloud[k] - 0.53) / 0.16, 0.0, 1.0) * 0.9;
            for (int c = 0; c < 3; ++c) tex[k * 3 + c] = float(col[c] * (1 - ca) + 252 * ca);
            bool city = cityNoise[k] > 0.62 && rs.random() < 0.35;
            lights[k] = (land && !ice && city) ? float(1 - ca) : 0.0f;
        }
    }
    const double tilt = 0.4;
    double Lx = -0.62, Ly = -0.34, Lz = 0.71, Ln = std::sqrt(Lx * Lx + Ly * Ly + Lz * Lz);
    Lx /= Ln; Ly /= Ln; Lz /= Ln;
    alpha.assign(size_t(n) * n, 0);
    for (int yy = 0; yy < n; ++yy)
        for (int xx = 0; xx < n; ++xx) {
            double x = (xx + 0.5 - R) / R, y = (yy + 0.5 - R) / R;
            double rr = x * x + y * y;
            alpha[size_t(yy) * n + xx] = uint8_t(clampv((1 - std::sqrt(rr)) * R, 0.0, 1.0) * 255);
            if (rr >= 1) continue;
            double z = std::sqrt(std::max(0.0, 1 - rr));
            double y2 = y * std::cos(tilt) - z * std::sin(tilt), z2 = y * std::sin(tilt) + z * std::cos(tilt);
            pix.push_back(yy * n + xx);
            iy.push_back(clampv(int((0.5 - std::asin(clampv(-y2, -1.0, 1.0)) / PI) * TH), 0, TH - 1));
            lon.push_back(float(std::atan2(x, z2)));
            double d = x * Lx + y * Ly + z * Lz;
            shade.push_back(float(0.045 + 0.955 * clampv(d, 0.0, 1.0)));
            night.push_back(float(clampv(0.1 - d, 0.0, 0.35) / 0.35));
            double rim = std::pow(1 - z, 2.6);
            double kk = 0.2 + 0.8 * clampv(d + 0.25, 0.0, 1.0);
            atmo.push_back(float(rim * 70 * kk));
            atmo.push_back(float(rim * 130 * kk));
            atmo.push_back(float(rim * 255 * kk));
        }
    discWhite = Surface::make(n, n, true);
    for (size_t k = 0; k < alpha.size(); ++k) discWhite->px[k] = (uint32_t(alpha[k]) << 24) | 0xFFFFFFu;
    earthImg = renderEarth();
}

SurfacePtr Annihilation::renderEarth() {
    const int n = 2 * ER;
    auto out = Surface::make(n, n, true);
    for (size_t k = 0; k < alpha.size(); ++k) out->px[k] = uint32_t(alpha[k]) << 24;
    for (size_t m = 0; m < pix.size(); ++m) {
        long txl = long((lon[m] + rot) * (TW / (2 * PI)));
        int tx = int(((txl % TW) + TW) % TW);
        size_t k = size_t(iy[m]) * TW + tx;
        double c[3];
        const double lc[3] = {255, 196, 110};
        for (int ch = 0; ch < 3; ++ch)
            c[ch] = tex[k * 3 + ch] * shade[m] + lights[k] * night[m] * lc[ch] + atmo[m * 3 + ch];
        int p = pix[m];
        out->px[size_t(p)] = (uint32_t(alpha[size_t(p)]) << 24) | (uint32_t(clampv(int(c[0]), 0, 255)) << 16) |
                             (uint32_t(clampv(int(c[1]), 0, 255)) << 8) | uint32_t(clampv(int(c[2]), 0, 255));
    }
    return out;
}

void Annihilation::buildCracks() {
    const double R = ER;
    const int S = 15;
    const double rings[6] = {0, 46, 114, 198, 300, 470};
    std::vector<double> baseA;
    for (int i = 0; i < S; ++i) baseA.push_back(i * TAU / S + rng.uniform(-0.15, 0.15));
    std::map<std::pair<int, int>, Vec2> V;
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < S; ++j) {
            if (i == 0) { V[{i, j}] = P; continue; }
            double a = baseA[j] + rng.uniform(-0.1, 0.1), d = rings[i] * rng.uniform(0.86, 1.14);
            V[{i, j}] = {P.x + std::cos(a) * d, P.y + std::sin(a) * d};
        }
    struct PathKey { char kind; int i, j; bool operator<(const PathKey& o) const { return std::tie(kind, i, j) < std::tie(o.kind, o.i, o.j); } };
    std::map<PathKey, size_t> index;
    std::vector<std::pair<PathKey, std::vector<Vec2>>> paths;     // in the order they were made
    auto edge = [&](Vec2 p, Vec2 q, PathKey key) -> std::vector<Vec2> {
        auto it = index.find(key);
        if (it != index.end()) return paths[it->second].second;
        double L = dist(p, q);
        if (L == 0) L = 1;
        double nx = -(q.y - p.y) / L, ny = (q.x - p.x) / L;
        int k = L > 60 ? 5 : 3;
        std::vector<Vec2> pts{p};
        for (int m = 1; m < k; ++m) {
            double u = double(m) / k, off = rng.uniform(-0.13, 0.13) * L;
            pts.push_back({p.x + (q.x - p.x) * u + nx * off, p.y + (q.y - p.y) * u + ny * off});
        }
        pts.push_back(q);
        index[key] = paths.size();
        paths.push_back({key, pts});
        return pts;
    };
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < S; ++j) {
            int j2 = (j + 1) % S;
            auto a_ = edge(V[{i, j}], V[{i + 1, j}], {'r', i, j});
            auto b_ = edge(V[{i + 1, j}], V[{i + 1, j2}], {'c', i + 1, j});
            auto c_ = edge(V[{i, j2}], V[{i + 1, j2}], {'r', i, j2});
            std::vector<Vec2> poly = a_;
            poly.insert(poly.end(), b_.begin() + 1, b_.end());
            std::vector<Vec2> cr(c_.rbegin(), c_.rend());
            poly.insert(poly.end(), cr.begin() + 1, cr.end());
            if (i > 0) {
                auto e = edge(V[{i, j}], V[{i, j2}], {'c', i, j});
                std::vector<Vec2> er(e.rbegin(), e.rend());
                if (er.size() > 2) poly.insert(poly.end(), er.begin() + 1, er.end() - 1);
            }
            cells.push_back(poly);
        }
    for (auto& [key, pts] : paths)
        for (size_t m = 0; m + 1 < pts.size(); ++m) {
            Vec2 p = pts[m], q = pts[m + 1];
            Vec2 mid((p.x + q.x) / 2, (p.y + q.y) / 2);
            if (dist(mid, Vec2(R, R)) > R * 0.985) continue;
            double birth = key.kind == 'r' ? T_CRACK + dist(p, P) / 2.4 : T_CRACK + 30 + dist(mid, P) / 2.0 + rng.uniform(0, 14);
            segs.push_back({birth, p, q, key.kind == 'r'});
        }
    std::stable_sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.birth < b.birth; });
    for (int i = 0; i < 20; ++i) craterJ.push_back(rng.uniform(0.72, 1.22));
}

// ------------------------------------------------------------ running it
void Annihilation::step(bool fast) {
    if (t < T_BLAST) {                                        // the beam is still firing on the stage
        shooter->moveFrame = std::min(shooter->moveFrame + 1, 99);
        g->fx.update();
        g->fx.shake = 8 + t / 3;
    }
    if (T_SPACE - 2 <= t && t < T_BOOM) {
        if (t < T_CRACK && !fast) {                           // the planet turns until it starts to break
            rot += 0.0018;
            if (t % 2 == 0) earthImg = renderEarth();
        }
        while (segI < segs.size() && segs[segI].birth <= t) drawCrack(segs[segI++]);
        comp = composite();
        earthView();
    }
    if (t == T_BOOM) boom();
    if (boomed) updateDebris();
    if (t == T_TEXT + 10) titleShake = 22;
    titleShake = std::max(0, titleShake - 1);
    ++t;
    if (t >= T_END) done = true;
}

void Annihilation::skip() {
    if (t < 20) return;
    if (t >= T_TEXT) { done = true; return; }
    while (t < T_TEXT) step(true);
}

void Annihilation::drawCrack(const Seg& s) {
    draw::line(*glowLayer, Color(120, 22, 6, 160), s.p, s.q, s.big ? 8 : 6);
    draw::line(*glowLayer, Color(255, 96, 24, 235), s.p, s.q, s.big ? 4 : 3);
    draw::line(*coreLayer, Color(255, 230, 150, 255), s.p, s.q, s.big ? 2 : 1);
}

SurfacePtr Annihilation::composite() {
    auto c = earthImg->copy();
    double heat = easeIn(double(t - T_CRACK) / (T_BOOM - T_CRACK));
    if (heat > 0) c->fill(Color(255, int(255 - 80 * heat), int(255 - 120 * heat)), Blend::RGB_MULT);
    c->blit(*glowLayer, 0, 0);
    c->blit(*coreLayer, 0, 0);
    if (heat > 0.25 && std::sin(t * 0.35) > 0) c->blit(*coreLayer, 0, 0, Blend::RGB_ADD);
    double cr = (7 + 30 * easeOut((t - T_SPACE) / 170.0)) * (1 + 0.05 * std::sin(t * 0.5));
    const std::pair<double, Color> layers[4] = {{1.3, Color(96, 20, 6)}, {1.0, Color(232, 82, 22)}, {0.7, Color(255, 172, 64)},
                                                {0.4, Color(255, 242, 196)}};
    for (auto& [scale, col] : layers) {
        std::vector<Vec2> pts;
        for (int i = 0; i < 20; ++i)
            pts.push_back({P.x + std::cos(i * TAU / 20) * cr * scale * craterJ[size_t(i)],
                           P.y + std::sin(i * TAU / 20) * cr * scale * craterJ[size_t(i)] * 0.9});
        draw::polygon(*c, col, pts);
    }
    c->blit(*discWhite, 0, 0, Blend::RGBA_MULT);
    return c;
}

void Annihilation::earthView() {
    double R = ER;
    double u = easeOut((t - T_SPACE) / 160.0);
    double rad = 820 + (262 - 820) * u;
    rad *= 1 - 0.035 * clampv((t - 240) / 180.0, 0.0, 1.0);
    if (t > T_BOOM - 20) rad *= 1 + 0.07 * easeIn((t - (T_BOOM - 20)) / 20.0);
    double k = rad / R, focus = 1 - u;
    double cx = W / 2.0 - (P.x - R) * k * focus, cy = H / 2.0 - (P.y - R) * k * focus;
    double amp = 11 * easeIn(double(t - 250) / (T_BOOM - 250));
    if (amp > 0.3) {
        cx += rng.uniform(-amp, amp);
        cy += rng.uniform(-amp, amp);
    }
    viewX = cx;
    viewY = cy;
    viewR = rad;
}

void Annihilation::boom() {
    const double R = ER;
    double cx = viewX, cy = viewY, rad = viewR, k = rad / R;
    boomX = cx;
    boomY = cy;
    boomK = k;
    for (auto& poly : cells) {
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (auto& p : poly) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
        Rect box = Rect(int(x0) - 2, int(y0) - 2, int(x1 - x0) + 5, int(y1 - y0) + 5).clip(Rect(0, 0, 2 * ER, 2 * ER));
        if (box.w < 3 || box.h < 3) continue;
        std::vector<Vec2> local;
        for (auto& p : poly) local.push_back({p.x - box.x, p.y - box.y});
        Surface s(box.w, box.h, true);
        s.blit(*comp, 0, 0, box);
        Surface cut(box.w, box.h, true);
        draw::polygon(cut, WHITE, local);
        s.blit(cut, 0, 0, Blend::RGBA_MULT);
        draw::lines(s, Color(255, 150, 50), true, local, 3);
        draw::lines(s, Color(255, 236, 170), true, local, 1);
        s.blit(*discWhite, 0, 0, box, Blend::RGBA_MULT);
        if (countOpaque(s, 40) < 60) continue;
        double mx = box.x + box.w / 2.0, my = box.y + box.h / 2.0;
        double dx = mx - R, dy = my - R, d = std::hypot(dx, dy);
        if (d == 0) d = 1;
        double sp = (0.7 + rng.uniform(0, 2.6)) * (0.5 + d / R) * k * 0.9;
        auto img = xform::smoothscale(s, std::max(1, int(box.w * k)), std::max(1, int(box.h * k)));
        double spin = rng.uniform(-2.2, 2.2);
        fragments.push_back({img, cx + dx * k, cy + dy * k, dx / d * sp, dy / d * sp, 0.0, spin});
    }
    boomed = true;
    const std::vector<Color> cols = {Color(255, 210, 120), Color(255, 140, 50), Color(255, 90, 30), Color(150, 140, 130)};
    const std::vector<int> sizes = {1, 1, 2, 2, 3};
    for (int i = 0; i < 650; ++i) {
        double a = rng.uniform(0, TAU), sp = rng.expovariate(1 / 4.2) + 0.6;
        int life = rng.randint(160, 520);
        Color c = rng.choice(cols);
        int sz = rng.choice(sizes);
        particles.push_back({cx + std::cos(a) * rad * 0.2, cy + std::sin(a) * rad * 0.2, std::cos(a) * sp, std::sin(a) * sp * 0.85,
                             life, 0, c, sz});
    }
}

void Annihilation::updateDebris() {
    const std::vector<Color> fire = {Color(255, 160, 60), Color(255, 100, 30)};
    for (auto& f : fragments) {
        f.x += f.vx;
        f.y += f.vy;
        f.vx *= 0.996;
        f.vy *= 0.996;
        f.ang += f.spin;
        if (t - T_BOOM < 140 && rng.random() < 0.3) {
            double vx = -f.vx * 0.2 + rng.uniform(-0.4, 0.4), vy = -f.vy * 0.2 + rng.uniform(-0.4, 0.4);
            int life = rng.randint(20, 50);
            particles.push_back({f.x, f.y, vx, vy, life, 0, rng.choice(fire), 2});
        }
    }
    for (auto& p : particles) {
        p.x += p.vx;
        p.y += p.vy;
        p.vx *= 0.993;
        p.vy *= 0.993;
        p.age += 1;
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Part& p) { return p.age >= p.life; }), particles.end());
}

// ------------------------------------------------------------ drawing it
void Annihilation::render(Surface& scr, bool text) {
    if (t < T_BLAST) {
        whiteOverlay(scr, int(255 * easeIn((t - 4) / double(T_BLAST - 6))));
        return;
    }
    if (t < T_SPACE) {
        renderBlast(scr);
        return;
    }
    renderSpace(scr);
    if (t >= T_TEXT && text) renderText(scr);
}

void Annihilation::renderBlast(Surface& scr) {
    int tt = t - T_BLAST, dur = T_SPACE - T_BLAST;
    int sh = int(14 * (1 - double(tt) / dur));
    int ox = rng.randint(-sh, sh), oy = rng.randint(-sh, sh);
    scr.blit(*blastBg, ox, oy);
    glow(scr, Color(255, 240, 200), Vec2(W / 2 + ox, H / 2 + oy), 260 + tt * 6, 150);
    for (auto& r : rocks) {
        int q = tt - r.delay;
        if (q < 0) continue;
        double d = r.d0 + r.v * q + r.acc * q * q;
        double sc = r.s0 * std::pow(1 + r.grow * q, 2);
        auto img = xform::rotozoom(*r.img, r.rot + r.spin * q, sc);
        double x = W / 2.0 + std::cos(r.a) * d + ox, y = H / 2.0 + std::sin(r.a) * d * 0.8 + oy;
        Rect rr = img->rect();
        rr.setCenter(x, y);
        scr.blit(*img, rr.x, rr.y);
    }
    Rng er(uint64_t(tt) * 2654435761u + 1);
    const std::vector<Color> ember = {Color(255, 220, 140), Color(255, 150, 60)};
    for (int i = 0; i < 60; ++i) {
        double a = er.uniform(0, TAU), d = er.uniform(40, 700) + tt * 6;
        Vec2 p(W / 2.0 + std::cos(a) * d, H / 2.0 + std::sin(a) * d * 0.8);
        Vec2 q(W / 2.0 + std::cos(a) * (d - 26), H / 2.0 + std::sin(a) * (d - 26) * 0.8);
        draw::line(scr, er.choice(ember), q, p, 2);
    }
    if (tt < 16) whiteOverlay(scr, int(255 * (1 - tt / 16.0)));
    if (tt > dur - 10) whiteOverlay(scr, int(255 * (tt - (dur - 10)) / 10));
}

void Annihilation::blitPlanet(Surface& scr, const Surface& img, double cx, double cy, double rad) {
    double k = rad / ER;
    Rect full(int(cx - rad), int(cy - rad), int(2 * rad), int(2 * rad));
    Rect vis = full.clip(scr.rect());
    if (vis.w <= 0 || vis.h <= 0) return;
    Rect src = Rect(int((vis.x - full.x) / k), int((vis.y - full.y) / k), int(vis.w / k) + 2, int(vis.h / k) + 2).clip(img.rect());
    if (src.w <= 0 || src.h <= 0) return;
    Surface part(std::max(1, int(src.w * k)), std::max(1, int(src.h * k)), true);
    xform::smoothscaleInto(img, src, part);
    scr.blit(part, full.x + src.x * k, full.y + src.y * k);
}

void Annihilation::renderSpace(Surface& scr) {
    const double R = ER;
    scr.blit(*space, 0, 0);
    for (auto& tw : twinkle) {
        double k = 0.5 + 0.5 * std::sin(t * 0.06 + tw.ph);
        draw::polygon(scr, lerpColor(Color(40, 40, 70), WHITE, k), starPoints(tw.x, tw.y, 2 + 3 * k));
    }
    double u = easeOut((t - T_SPACE) / 160.0);
    if (u > 0.6) {
        double mx = W * 0.105, my = H * 0.25;
        draw::circle(scr, Color(150, 152, 162), Vec2(int(mx), int(my)), 11);
        draw::circle(scr, Color(40, 40, 52), Vec2(int(mx + 5), int(my + 2)), 9);
        draw::circle(scr, Color(190, 190, 200), Vec2(int(mx - 4), int(my - 3)), 3);
    }
    if (!boomed) {
        double cx = viewX, cy = viewY, rad = viewR;
        if (rad < 520) glow(scr, Color(110, 170, 255), Vec2(cx, cy), rad * 1.09, 120);
        double heat = easeIn(double(t - T_CRACK) / (T_BOOM - T_CRACK));
        if (heat > 0.05) glow(scr, Color(255, 110, 30), Vec2(cx, cy), rad * (1.08 + 0.12 * heat), int(150 * heat));
        if (comp) blitPlanet(scr, *comp, cx, cy, rad);
        double k = rad / R;
        double ix = cx + (P.x - R) * k, iy_ = cy + (P.y - R) * k;
        int tt = t - T_SPACE;
        if (tt < 60) glow(scr, Color(255, 236, 190), Vec2(ix, iy_), 40 + (420 - 40) * (1 - easeOut(tt / 60.0)), int(230 * (1 - tt / 60.0)));
        glow(scr, Color(255, 120, 40), Vec2(ix, iy_), 34 * k * (1 + 0.1 * std::sin(t * 0.4)), 120);
        if (tt < 14) whiteOverlay(scr, int(255 * (1 - tt / 14.0)));
        return;
    }
    int tt = t - T_BOOM;
    double r = std::min(560.0, 30.0 + 13.0 * tt);
    const struct { double scale; Color col; int a; } balls[3] = {{1.35, Color(120, 30, 10), 130 - tt},
                                                                 {1.0, Color(255, 140, 40), 230 - int(tt * 2.2)},
                                                                 {0.5, Color(255, 244, 210), 255 - tt * 6}};
    for (auto& b : balls)
        if (b.a > 0) glow(scr, b.col, Vec2(boomX, boomY), std::max(4.0, r * b.scale), b.a);
    for (auto& f : fragments) {
        double fade = clampv(1 - (tt - 260) / 260.0, 0.0, 1.0);
        if (fade <= 0) continue;
        auto img = xform::rotate(*f.img, f.ang);
        if (fade < 1) img->setAlpha(int(255 * fade));
        Rect rr = img->rect();
        rr.setCenter(f.x, f.y);
        scr.blit(*img, rr.x, rr.y);
    }
    if (!ov) ov = Surface::make(W, H, true);
    ov->fill(Color(0, 0, 0, 0));
    const struct { int delay; double speed; Color col; } waves[2] = {{4, 26, Color(255, 214, 160)}, {14, 18, Color(150, 190, 255)}};
    for (auto& wv : waves) {
        int q = tt - wv.delay;
        if (q <= 0) continue;
        double w = 40 + wv.speed * q;
        int a = std::max(0, 235 - int(q * 1.7));
        if (a > 0) {
            Rect rect(0, 0, int(w), int(w * 0.2));
            rect.setCenter(int(boomX), int(boomY));
            draw::ellipse(*ov, wv.col.withAlpha(a), rect, std::max(2, int(9 - q * 0.04)));
        }
    }
    for (auto& p : particles) {
        double life = 1 - double(p.age) / p.life;
        Color col = lerpColor(Color(60, 40, 40), p.col, life).withAlpha(int(255 * life));
        if (std::abs(p.vx) + std::abs(p.vy) > 2.5)
            draw::line(*ov, col, Vec2(p.x, p.y), Vec2(p.x - p.vx * 1.8, p.y - p.vy * 1.8), p.size);
        else
            draw::circle(*ov, col, Vec2(int(p.x), int(p.y)), p.size);
    }
    scr.blit(*ov, 0, 0);
    if (tt < 34) whiteOverlay(scr, int(255 * std::pow(1 - tt / 34.0, 2)), Color(255, 250, 240));
}

void Annihilation::buildTitle() {
    const Font& f = g->font(178);
    std::string word = "ANNIHILATION";
    auto mask = f.render(word, WHITE);
    int w = mask->w, h = mask->h;
    Surface grad(w, h, true);
    for (int y = 0; y < h; ++y) {
        double u = double(y) / h;
        Color col = u < 0.5 ? lerpColor(Color(255, 128, 96), Color(206, 22, 12), u / 0.5)
                            : lerpColor(Color(206, 22, 12), Color(84, 0, 0), (u - 0.5) / 0.5);
        if (0.26 < u && u < 0.34) col = lerpColor(col, Color(255, 226, 206), 0.6);
        grad.fill(col, Rect(0, y, w, 1));
    }
    grad.blit(*mask, 0, 0, Blend::RGBA_MULT);
    int pad = 14;
    title = Surface::make(w + pad * 2, h + pad * 2, true);
    auto dark = f.render(word, Color(26, 0, 0)), gold = f.render(word, Color(255, 194, 86));
    for (int dx = -7; dx < 8; dx += 2)
        for (int dy = -7; dy < 8; dy += 2)
            if (dx * dx + dy * dy <= 49) title->blit(*dark, pad + dx, pad + dy);
    for (int a = 0; a < 12; ++a)
        title->blit(*gold, pad + std::round(std::cos(a * TAU / 12) * 2.5), pad + std::round(std::sin(a * TAU / 12) * 2.5));
    title->blit(grad, pad, pad);
    std::string up = name;
    for (auto& ch : up) ch = char(toupper((unsigned char)ch));
    sub = g->font(60).render(up + " WINS", Color(255, 214, 120));
}

void Annihilation::renderText(Surface& scr) {
    if (!title) buildTitle();
    int tt = t - T_TEXT;
    if (!tint) {
        tint = Surface::make(W, H, false);
        tint->fill(Color(150, 40, 0));
    }
    tint->setAlpha(int(110 * easeOut(tt / 50.0)));
    scr.blit(*tint, 0, 0);
    SurfacePtr img = title;
    if (tt < 10) {
        double sc = 1 + 2.6 * (1 - easeOut(tt / 10.0));
        img = xform::rotozoom(*title, 0, sc);
        img->setAlpha(int(255 * std::min(1.0, tt / 6.0)));
    }
    int sh = titleShake;
    int ox = sh ? rng.randint(-sh, sh) : 0, oy = sh ? rng.randint(-sh, sh) : 0;
    Rect r = img->rect();
    r.setCenter(W / 2 + ox, int(H * 0.42) + oy);
    scr.blit(*img, r.x, r.y);
    if (tt >= 22) {
        double u = easeOut((tt - 22) / 16.0);
        int bw = sub->w + 140, bh = 84;
        Rect bar(0, 0, bw, bh);
        bar.setCenter(int(-bw / 2.0 + (W / 2.0 + bw / 2.0) * u), int(H * 0.42) + 120);
        draw::rect(scr, Color(40, 0, 0), bar.inflate(10, 10));
        draw::rect(scr, Color(140, 12, 10), bar);
        draw::rect(scr, Color(190, 40, 30), bar.inflate(-8, -40).move(0, -18));
        draw::rect(scr, Color(255, 190, 80), bar, 3);
        Rect sr = sub->rect();
        sr.setCenter(bar.centerx(), bar.centery());
        scr.blit(*sub, sr.x, sr.y);
    }
    if (tt >= 70 && (tt / 30) % 2 == 0) g->text(scr, "ENTER  continue", 30, Color(255, 220, 160), Vec2(W / 2, H - 60));
}
