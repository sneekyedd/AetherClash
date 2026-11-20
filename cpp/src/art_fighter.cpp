// Draws a fighter from a skeleton pose (the Python version's render_fighter_surface).
// Every part gets a dark ink outline, a shadow side and a highlight, like hand-drawn sprite art.
#include <cmath>

#include "art.h"

namespace {
using V = Vec2;
using Pts = std::vector<Vec2>;
using Profile = std::function<double(double)>;
const Color INK(30, 18, 32);

std::pair<V, V> ik(V a, V b, double l1, double l2, double bend) {
    double dx = b.x - a.x, dy = b.y - a.y;
    double d = std::hypot(dx, dy);
    if (d == 0) d = 0.001;
    double ux = dx / d, uy = dy / d;
    double reach = l1 + l2 - 0.5;
    if (d > reach) {
        d = reach;
        b = {a.x + ux * d, a.y + uy * d};
    }
    d = std::max(d, std::abs(l1 - l2) + 0.5);
    double k = (l1 * l1 - l2 * l2 + d * d) / (2 * d);
    double hgt = std::sqrt(std::max(0.0, l1 * l1 - k * k));
    return {V(a.x + ux * k - uy * hgt * bend, a.y + uy * k + ux * hgt * bend), b};
}

Pts slice(const Pts& p, int a, int b) {
    a = std::max(0, std::min(a, int(p.size())));
    b = std::max(a, std::min(b, int(p.size())));
    return Pts(p.begin() + a, p.begin() + b);
}
Pts rev(Pts p) { std::reverse(p.begin(), p.end()); return p; }
Pts cat(Pts a, const Pts& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

struct Painter {
    Surface& s;
    const FighterSpec& spec;
    const Palette& col;
    const Pose& P;
    int f;
    double t;
    const std::string& key;
    const AnimInfo& info;
    ClothSim* cloth;
    double w, h, yo = 0;
    Style style;
    Color main, dark, acc, skin, mid, light;
    // skeleton
    double hr, leg, arm;
    int lw, aw;
    V hip, sh, sf, sb, hpf, hpb, kf, ff, kb, fb, ef, hf, eb, hb_, head;
    bool blink, pained, striking;
    double tdx, tdy, flow, tw, hw;

    Painter(Surface& s_, const FighterSpec& sp, const Palette& c, const Pose& p, int facing, double t_,
            const std::string& k, const AnimInfo& in, ClothSim* cl)
        : s(s_), spec(sp), col(c), P(p), f(facing), t(t_), key(k), info(in), cloth(cl) {}

    // ---- drawing helpers (local fighter coordinates -> supersampled surface pixels)
    V M(V p) const { return {(SPR_CX + p.x * f) * SS, (SPR_FEET + p.y + yo) * SS}; }
    Pts M(const Pts& ps) const { Pts o; o.reserve(ps.size()); for (auto& p : ps) o.push_back(M(p)); return o; }
    void C(Color c, V p, double r, double width = 0) {
        V m = M(p);
        draw::circle(s, c, V(double(int(m.x)), double(int(m.y))), std::max(1, int(r * SS)), int(width * SS));
    }
    void oC(Color c, V p, double r, bool hl = true) {
        C(INK, p, r + 1.1);
        C(c, p, r);
        if (hl && r > 3) C(lerpColor(c, WHITE, 0.28), V(p.x + r * 0.2, p.y - r * 0.3), r * 0.45);
    }
    void poly(Color c, const Pts& pts, double width = 0) { draw::polygon(s, c, M(pts), int(width * SS)); }
    void opoly(Color c, const Pts& pts, double ow = 1.1) {
        poly(c, pts);
        draw::polygon(s, INK, M(pts), std::max(1, int(ow * SS)));
    }
    void line(Color c, V a, V b, double width) { draw::line(s, c, M(a), M(b), std::max(1, int(width * SS))); }
    Rect centered(V center, double bw, double bh) const {
        Rect r(0, 0, int(bw * SS), int(bh * SS));
        V m = M(center);
        r.setCenter(m.x, m.y);
        return r;
    }
    void box(Color c, V center, double bw, double bh, double radius = 0, double width = 0) {
        draw::rect(s, c, centered(center, bw, bh), int(width * SS), int(radius * SS));
    }
    void obox(Color c, V center, double bw, double bh, double radius = 0) {
        box(c, center, bw, bh, radius);
        box(INK, center, bw, bh, radius, 1.1);
    }
    void oval(Color c, V center, double bw, double bh, double width = 0) {
        draw::ellipse(s, c, centered(center, bw, bh), int(width * SS));
    }
    void ooval(Color c, V center, double bw, double bh) {
        oval(INK, center, bw + 2.2, bh + 2.2);
        oval(c, center, bw, bh);
    }
    void halo(Color c, V p, double radius, int alpha) { glow(s, c, M(p), radius * SS, std::min(190, alpha)); }

    Pts chain(const std::string& name, V anchor, int n, double seg, double gravity = 0.5, double damp = 0.86,
              double wave = 0.25, double breeze = 0.12) {
        if (cloth) return cloth->chain(name, anchor, n, seg, gravity, damp, wave, t, breeze);
        Pts pts{anchor};
        double ph = name.size() * 1.7;
        for (int i = 1; i <= n; ++i) {
            double wv = std::sin(t * 0.22 + i * 0.9 + ph) * (2 + wave * 6) * i / n;
            if (gravity < 0)
                pts.push_back({anchor.x + (tdx * 0.35 - breeze * 2) * seg * i + wv, anchor.y - seg * i * 0.95});
            else
                pts.push_back({anchor.x + tdx * seg * i * flow - tdy * wv, anchor.y + tdy * seg * i * flow + tdx * wv + i * i * 0.3});
        }
        return pts;
    }

    void ribbon(const Pts& pts, Color c, double w0, double w1 = 1.5, const Profile& profile = nullptr, bool ink = false) {
        int n = int(pts.size());
        if (n < 2) return;
        Pts left, right;
        for (int i = 0; i < n; ++i) {
            const V& p = pts[i];
            const V& a = pts[std::max(0, i - 1)];
            const V& b = pts[std::min(n - 1, i + 1)];
            double dx = b.x - a.x, dy = b.y - a.y;
            double d = std::hypot(dx, dy);
            if (d == 0) d = 1;
            double u = double(i) / (n - 1);
            double wd = (profile ? profile(u) : w0 + (w1 - w0) * u) / 2;
            left.push_back({p.x - dy / d * wd, p.y + dx / d * wd});
            right.push_back({p.x + dy / d * wd, p.y - dx / d * wd});
        }
        if (ink) {
            opoly(c, cat(left, rev(right)));
        } else {
            poly(c, cat(left, rev(right)));
            C(c, pts[0], (profile ? profile(0) : w0) / 2);
        }
    }

    Pts clothStrip(const std::string& name, V anchor, int n, double seg, Color c, double w0, double w1,
                   double gravity = 0.3, double breeze = 0.4, double wave = 0.5) {
        Pts pts = chain(name, anchor, n, seg, gravity, 0.9, wave, breeze);
        ribbon(pts, c, w0, w1, nullptr, true);
        ribbon(slice(pts, 1, int(pts.size())), lerpColor(c, INK, 0.3), 0, 1.5,
               [=](double u) { return (w0 + (w1 - w0) * u) * 0.32; });
        return pts;
    }

    void flameStrand(const Pts& pts, double width, const std::array<Color, 4>& colors, double seed = 0.0) {
        const double ks[4] = {1.0, 0.66, 0.36, 0.16};
        for (int j = 0; j < 4; ++j) {
            double k = ks[j];
            Pts cut = slice(pts, 0, std::max(3, int(pts.size() * (0.5 + 0.5 * k))));
            double tt = t;
            ribbon(cut, colors[j], 0, 1.5, [=](double u) {
                return 2.2 * k + width * k * (0.5 + 0.5 * std::sin(PI * std::min(1.0, u * 2.6 + 0.2))) *
                                     std::pow(1 - u, 0.6) * (0.82 + 0.18 * std::sin(tt * 0.7 + u * 8 + seed));
            });
        }
    }

    void limb(Color c, V a, V j, V b, double width, std::optional<Color> color2 = std::nullopt, double width2 = 0) {
        Color c2 = color2 ? *color2 : c;
        double w2 = width2 ? width2 : width;
        double o = 2.3;
        line(INK, a, j, width + o);
        line(INK, j, b, w2 + o);
        C(INK, a, (width + o) / 2);
        C(INK, j, (std::max(width, w2) + o) / 2);
        C(INK, b, (w2 + o) / 2);
        line(c, a, j, width);
        line(c2, j, b, w2);
        C(c, a, width / 2);
        C(c, j, std::max(width, w2) / 2);
        C(c2, b, w2 / 2);
        struct Seg { V p, q; Color cc; double ww; };
        for (const Seg& sg : {Seg{a, j, c, width}, Seg{j, b, c2, w2}}) {
            double dx = sg.q.x - sg.p.x, dy = sg.q.y - sg.p.y;
            double d = std::hypot(dx, dy);
            if (d == 0) d = 1;
            double nx = -dy / d, ny = dx / d;
            if (ny > 0) { nx = -nx; ny = -ny; }
            double off = sg.ww * 0.24;
            line(lerpColor(sg.cc, WHITE, 0.26), V(sg.p.x + nx * off + dx * 0.12, sg.p.y + ny * off + dy * 0.12),
                 V(sg.q.x + nx * off - dx * 0.12, sg.q.y + ny * off - dy * 0.12), std::max(1.0, sg.ww * 0.22));
        }
    }

    static V trim(V knee, V ank, double r) {
        double dx = knee.x - ank.x, dy = knee.y - ank.y;
        double d = std::hypot(dx, dy);
        if (d == 0) d = 1;
        return {ank.x + dx / d * r, ank.y + dy / d * r};
    }

    using QFn = std::function<V(double, double)>;
    QFn shoe(V knee, V ank, Color c, double L, double H, bool square = false, bool bare = false) {
        double dx = ank.x - knee.x, dy = ank.y - knee.y;
        double d = std::hypot(dx, dy);
        if (d == 0) d = 1;
        double fx = dy / d, fy = -dx / d;
        if (fx < 0.25) fx = 0.25;
        double k = clampv(-ank.y / 9.0, 0.0, 1.0);
        fx = (1 - k) + fx * k;
        fy = fy * k;
        double n_ = std::hypot(fx, fy);
        if (n_ == 0) n_ = 1;
        fx /= n_;
        fy /= n_;
        double ux = fy, uy = -fx;
        QFn Q = [=](double a, double b) { return V(ank.x + fx * a + ux * b, ank.y + fy * a + uy * b); };
        double hb0 = -L * 0.28, toe = L * 0.72;
        Pts pts;
        if (square)
            pts = {Q(hb0, 0), Q(toe, 0), Q(toe + 1, H * 0.55), Q(toe - 4, H * 0.9), Q(L * 0.12, H * 1.05), Q(hb0, H * 1.15)};
        else if (bare)
            pts = {Q(hb0, 0), Q(toe - 3, 0), Q(toe, H * 0.25), Q(toe - 2, H * 0.6), Q(L * 0.25, H * 0.8),
                   Q(hb0 + 2, H * 1.1), Q(hb0 - 1, H * 0.45)};
        else
            pts = {Q(hb0, 0), Q(toe - 2, 0), Q(toe + 1, H * 0.3), Q(toe - 3, H * 0.72), Q(L * 0.18, H * 0.95),
                   Q(hb0 + 1, H * 1.2), Q(hb0 - 1, H * 0.5)};
        opoly(c, pts);
        if (!bare) line(lerpColor(c, INK, 0.5), Q(hb0 + 1, 1.0), Q(toe - 2, 1.0), 2.2);
        line(lerpColor(c, WHITE, 0.3), Q(L * 0.02, H * 0.85), Q(toe - 5, H * 0.55), 1.2);
        return Q;
    }

    void bands(V a, V b, Color c, std::initializer_list<double> ks, double half, double wd = 1.6) {
        double d = std::hypot(b.x - a.x, b.y - a.y);
        if (d == 0) d = 1;
        double nx = -(b.y - a.y) / d, ny = (b.x - a.x) / d;
        for (double k : ks) {
            V q(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k);
            line(c, V(q.x - nx * half, q.y - ny * half), V(q.x + nx * half, q.y + ny * half), wd);
        }
    }

    Pts torsoPts(double tw_, double hw_, double drop = 4) const {
        return {{sh.x - tw_ / 2, sh.y + 2}, {sh.x - tw_ / 2 + 3, sh.y - 1}, {sh.x + tw_ / 2 - 3, sh.y - 1},
                {sh.x + tw_ / 2, sh.y + 2}, {hip.x + hw_ / 2, hip.y + drop}, {hip.x - hw_ / 2, hip.y + drop}};
    }
    void torso(Color c, double tw_, double hw_, double drop = 4, std::optional<Color> shade = std::nullopt) {
        Pts pts = torsoPts(tw_, hw_, drop);
        poly(c, pts);
        if (shade) poly(*shade, {pts[0], pts[1], {sh.x - tw_ * 0.16, sh.y - 1}, {hip.x - hw_ * 0.22, hip.y + drop}, pts[5]});
        draw::polygon(s, INK, M(pts), std::max(1, int(1.1 * SS)));
    }

    void humanEye(V p, double r) {
        if (pained) {
            line(INK, V(p.x - r, p.y - r), V(p.x + r * 0.6, p.y), 1.4);
            line(INK, V(p.x - r, p.y + r), V(p.x + r * 0.6, p.y), 1.4);
        } else if (blink) {
            line(INK, V(p.x - r, p.y), V(p.x + r, p.y), 1.4);
        } else {
            oval(WHITE, p, r * 2.4, r * 2.2);
            C(INK, V(p.x + r * 0.35, p.y + 0.2), r * 0.78);
            C(WHITE, V(p.x + r * 0.62, p.y - r * 0.36), r * 0.26);
        }
    }

    void face(double eye_r = 0.24, double brow = 1.6, bool scar = false) {
        V fc(head.x + hr * 0.12, head.y + hr * 0.12);
        oC(skin, fc, hr * 0.9, false);
        poly(lerpColor(skin, dark, 0.28), {{fc.x - hr * 0.86, fc.y + hr * 0.1}, {fc.x - hr * 0.2, fc.y + hr * 0.86},
                                           {fc.x - hr * 0.62, fc.y + hr * 0.6}});
        C(lerpColor(skin, dark, 0.22), V(fc.x - hr * 0.5, fc.y + hr * 0.12), hr * 0.2);
        humanEye(V(fc.x + hr * 0.42, fc.y - hr * 0.02), hr * eye_r);
        if (!pained) line(INK, V(fc.x + hr * 0.08, fc.y - hr * 0.32), V(fc.x + hr * 0.78, fc.y - hr * 0.16), brow);
        line(lerpColor(skin, dark, 0.4), V(fc.x + hr * 0.8, fc.y + hr * 0.12), V(fc.x + hr * 0.88, fc.y + hr * 0.3), 1.1);
        if (striking || pained)
            oval(INK, V(fc.x + hr * 0.5, fc.y + hr * 0.52), hr * 0.42, hr * 0.32);
        else
            line(INK, V(fc.x + hr * 0.3, fc.y + hr * 0.5), V(fc.x + hr * 0.68, fc.y + hr * 0.46), 1.1);
        if (scar)
            line(lerpColor(skin, dark, 0.5), V(fc.x + hr * 0.0, fc.y + hr * 0.3), V(fc.x + hr * 0.34, fc.y + hr * 0.14), 1.2);
    }

    void setup(Vec2 vel) {
        const auto& st = spec.stats;
        w = st.w;
        h = st.h;
        style = spec.style;
        main = col.main;
        dark = col.dark;
        acc = col.accent;
        skin = col.skin;
        mid = lerpColor(main, dark, 0.5);
        light = lerpColor(main, WHITE, 0.3);
        hr = std::max(10.0, h * HEAD_K);
        hip = P.hip;
        sh = P.sh;
        leg = h * HIP_K / 2 * 1.05;
        arm = h * 0.21;
        lw = std::max(8, int(w * 0.23));
        aw = std::max(6, int(w * 0.18));
        sf = {sh.x + 3, sh.y + 3};
        sb = {sh.x - 4, sh.y + 3};
        hpf = {hip.x + 2, hip.y};
        hpb = {hip.x - 3, hip.y};
        std::tie(kf, ff) = ik(hpf, P.ff, leg, leg, -1);
        std::tie(kb, fb) = ik(hpb, P.fb, leg, leg, -1);
        std::tie(ef, hf) = ik(sf, P.hf, arm, arm, 1);
        std::tie(eb, hb_) = ik(sb, P.hb, arm, arm, 1);
        head = {sh.x + (sh.x - hip.x) * 0.25 + P.head.x, sh.y - hr * 0.95 + P.head.y};
        blink = long(t) % 190 < 7;
        pained = key == "hurt" || key == "stun" || key == "grabbed";
        striking = key == "attack" && info.phase == "strike";
        double tvx = -vel.x * 1.3 - 5, tvy = -vel.y * 0.9 + 2;
        double tn = std::hypot(tvx, tvy);
        if (tn == 0) tn = 1;
        tdx = tvx / tn;
        tdy = tvy / tn;
        flow = std::min(1.6, 0.6 + tn / 14);
        tw = w * 0.6;
        hw = w * 0.46;
    }

    void smear() {
        if (!info.smear) return;
        double L = info.wlen + 6, a0 = *info.smear, a1 = P.wp;
        V hand = (style != Style::Sentinel || info.weapon == "maul") ? hf : hb_;
        int n_ = 10;
        Pts outer, inner;
        for (int q = 0; q <= n_; ++q) {
            double a = a0 + (a1 - a0) * q / n_;
            outer.push_back({hand.x + std::cos(a) * L, hand.y + std::sin(a) * L});
            inner.push_back({hand.x + std::cos(a) * L * 0.3, hand.y + std::sin(a) * L * 0.3});
        }
        poly(lerpColor(acc, WHITE, 0.5).withAlpha(120), cat(outer, rev(inner)));
        line(Color(255, 255, 255, 170), outer.back(), outer[outer.size() / 2], 2);
    }

    // ---------------- KAI: spiky-haired duelist, tattered scarf and tunic, broad steel blade
    void kai() {
        Color hairc = col.get("hair", dark), bandc = col.get("band", acc);
        Color wrapc = col.get("wrap", skin), bootc = col.get("boot", mid);
        Color hair_hi = lerpColor(hairc, WHITE, 0.22), hair_lo = lerpColor(hairc, INK, 0.35);
        Color pants = dark, pants_hi = lerpColor(dark, main, 0.3);
        Color wrap_lo = lerpColor(wrapc, INK, 0.35);
        V neck(sh.x - 3, sh.y + 1);
        limb(mid, sb, eb, hb_, aw + 1, lerpColor(wrapc, INK, 0.2), aw);
        bands(eb, hb_, wrap_lo, {0.3, 0.55, 0.8}, aw / 2.0);
        auto kaiFoot = [&](V knee, V ank) {
            bands(knee, ank, wrapc, {0.5, 0.64, 0.78}, (lw + 1) / 2.0 + 0.5, 2.2);
            QFn Q = shoe(knee, ank, bootc, lw + 11, 8.5);
            line(wrapc, Q(-1.5, 6.5), Q(4.5, 6.0), 1.8);
        };
        Color pl = lerpColor(pants, INK, 0.25);
        limb(pl, hpb, kb, trim(kb, fb, 5), lw + 4, pl, lw + 1);
        kaiFoot(kb, fb);
        clothStrip("scarf2", V(neck.x, neck.y + 3), 8, 8, lerpColor(acc, INK, 0.25), 11, 6, 0.2, 0.5, 0.6);
        Pts tail = clothStrip("scarf1", neck, 9, 8, acc, 13, 7, 0.16, 0.55, 0.6);
        V te = tail.back();
        poly(INK, {te, {te.x - 5, te.y - 3}, {te.x - 4, te.y + 3}});
        clothStrip("sash", V(hip.x - hw * 0.3, hip.y + 2), 4, 6, lerpColor(main, INK, 0.3), 7, 3, 0.5, 0.2, 0.3);
        torso(main, tw, hw, 4, mid);
        line(light, V(sh.x + tw * 0.05, sh.y + 2), V(sh.x + tw * 0.38, sh.y + 2), 1.5);
        line(mid, V(sh.x + tw * 0.25, sh.y + 3), V(hip.x + hw * 0.1, hip.y + 2), 1.4);
        opoly(main, {{hip.x - hw / 2 - 2, hip.y + 1}, {hip.x + hw / 2 + 2, hip.y + 1}, {hip.x + hw / 2 + 4, hip.y + 15},
                     {hip.x + hw * 0.28, hip.y + 9}, {hip.x + 1, hip.y + 18}, {hip.x - hw * 0.25, hip.y + 10},
                     {hip.x - hw / 2 - 4, hip.y + 16}});
        limb(pants, hpf, kf, trim(kf, ff, 5), lw + 4, pants_hi, lw + 1);
        opoly(lerpColor(main, WHITE, 0.08), {{hip.x - 1, hip.y + 1}, {hip.x + hw / 2 + 2, hip.y + 1},
                                             {hip.x + hw / 2 + 4, hip.y + 15}, {hip.x + hw * 0.22, hip.y + 10}});
        line(lerpColor(acc, INK, 0.25), V(hip.x - hw / 2 - 1, hip.y), V(hip.x + hw / 2 + 1, hip.y), 5.5);
        line(acc, V(hip.x - hw / 2, hip.y - 1), V(hip.x + hw / 2, hip.y - 1), 2.2);
        oC(Color(176, 182, 198), V(hip.x + hw * 0.25, hip.y + 4), 2.6, false);
        C(INK, V(hip.x + hw * 0.25, hip.y + 4), 1.1);
        kaiFoot(kf, ff);
        ooval(acc, V(sh.x, sh.y + 1), tw + 9, 12);
        oval(lerpColor(acc, INK, 0.28), V(sh.x - 3, sh.y + 4), tw + 2, 4);
        oval(lerpColor(acc, WHITE, 0.3), V(sh.x + 4, sh.y - 2), tw * 0.5, 2.5);
        clothStrip("band2", V(head.x - hr * 0.9, head.y - hr * 0.25), 5, 6, lerpColor(bandc, INK, 0.35), 5, 2.5, 0.26, 0.4);
        clothStrip("band1", V(head.x - hr * 0.9, head.y - hr * 0.35), 6, 6, bandc, 5.5, 2.5, 0.2, 0.45);
        Color hairs[3] = {hairc, hair_lo, hair_hi};
        for (int i = 0; i < 10; ++i) {
            double a = radians(-8 - i * 24);
            double ln = hr * (2.05 + 0.14 * std::sin(t * 0.09 + i * 1.3) + (i % 2 ? 0.28 : 0));
            opoly(hairs[i % 3], {{head.x + std::cos(a - 0.36) * hr * 0.9, head.y + std::sin(a - 0.36) * hr * 0.9},
                                 {head.x + std::cos(a) * ln - 2, head.y + std::sin(a) * ln * 0.95 - 1},
                                 {head.x + std::cos(a + 0.36) * hr * 0.9, head.y + std::sin(a + 0.36) * hr * 0.9}});
        }
        oC(hairc, V(head.x - hr * 0.14, head.y - hr * 0.24), hr * 1.05, false);
        C(hair_hi, V(head.x - hr * 0.05, head.y - hr * 0.7), hr * 0.4);
        face(0.25);
        const double fringe[4] = {0.95, 0.5, 0.05, -0.4};
        for (int i = 0; i < 4; ++i) {
            double fx_ = fringe[i];
            opoly(i == 1 ? hair_hi : hairc,
                  {{head.x + hr * (fx_ - 0.34), head.y - hr * 0.7}, {head.x + hr * (fx_ + 0.22), head.y - hr * 0.74},
                   {head.x + hr * (fx_ + 0.12), head.y - hr * (0.16 + 0.1 * i)}});
        }
        line(INK, V(head.x - hr * 0.98, head.y - hr * 0.4), V(head.x + hr * 1.02, head.y - hr * 0.52), 6);
        line(bandc, V(head.x - hr * 0.95, head.y - hr * 0.4), V(head.x + hr * 0.98, head.y - hr * 0.52), 4);
        line(lerpColor(bandc, WHITE, 0.35), V(head.x - hr * 0.6, head.y - hr * 0.5), V(head.x + hr * 0.9, head.y - hr * 0.6), 1.1);
        limb(main, sf, ef, hf, aw + 1, wrapc, aw);
        bands(ef, hf, wrap_lo, {0.25, 0.45, 0.65, 0.85}, aw / 2.0);
        double bx = std::cos(P.wp), by = std::sin(P.wp);
        double nx_ = -by, ny_ = bx;
        const double BL = 52;
        auto bpt = [&](double u, double side) {
            double curve = 3.2 * std::sin(PI * u * 0.9);
            double half = (4.0 * (1 - u) + 0.8) * side;
            double d = 6 + (BL - 6) * u;
            return V(hf.x + bx * d + nx_ * (curve + half), hf.y + by * d + ny_ * (curve + half));
        };
        ribbon(chain("thread", V(hf.x - bx * 9, hf.y - by * 9), 6, 6, 0.25, 0.9, 0.5), acc, 1.6, 1);
        Pts spine, edge, polish;
        for (int u = 0; u <= 8; ++u) {
            spine.push_back(bpt(u / 8.0, -1));
            edge.push_back(bpt(u / 8.0, 1));
            polish.push_back(bpt(u / 8.0, 0.15));
        }
        opoly(Color(128, 136, 156), cat(spine, rev(edge)));
        poly(Color(222, 228, 240), cat(polish, rev(edge)));
        line(Color(90, 96, 116), bpt(0.05, -0.3), bpt(0.72, -0.3), 1);
        if (striking) {
            double g_ = pymod(t * 0.25, 1.0);
            C(WHITE, bpt(g_, 0.5), 2.2);
        } else {
            C(WHITE, bpt(0.4 + 0.3 * std::sin(t * 0.06), 0.4), 1.4);
        }
        V gl(hf.x + bx * 5 - nx_ * 9, hf.y + by * 5 - ny_ * 9), gr(hf.x + bx * 5 + nx_ * 9, hf.y + by * 5 + ny_ * 9);
        line(INK, gl, gr, 6.5);
        line(acc, gl, gr, 4);
        oC(acc, gl, 2.4, false);
        oC(acc, gr, 2.4, false);
        line(INK, V(hf.x - bx * 9, hf.y - by * 9), V(hf.x + bx * 3, hf.y + by * 3), 5.5);
        line(Color(120, 34, 40), V(hf.x - bx * 9, hf.y - by * 9), V(hf.x + bx * 3, hf.y + by * 3), 3.4);
        for (double k : {-6.0, -2.5, 1.0})
            line(lerpColor(acc, INK, 0.2), V(hf.x + bx * k - nx_ * 1.7, hf.y + by * k - ny_ * 1.7),
                 V(hf.x + bx * (k + 1.6) + nx_ * 1.7, hf.y + by * (k + 1.6) + ny_ * 1.7), 1);
        oC(Color(190, 196, 210), V(hf.x - bx * 10.5, hf.y - by * 10.5), 2.6, false);
        oC(skin, hf, 4.4, false);
    }

    // ---------------- BRONN: bucket-helmed guardian in blue stone plate, great hammer, gem shield
    int power = 0;
    Color steel, stone;
    void maul(V hand, double a) {
        double bx = std::cos(a), by = std::sin(a);
        V end(hand.x + bx * 50, hand.y + by * 50);
        line(INK, V(hand.x - bx * 14, hand.y - by * 14), end, 8);
        line(Color(92, 62, 44), V(hand.x - bx * 13, hand.y - by * 13), end, 5.5);
        line(Color(146, 106, 70), V(hand.x - bx * 13 - by * 1.2, hand.y - by * 13 + bx * 1.2),
             V(end.x - by * 1.2, end.y + bx * 1.2), 1.8);
        oC(Color(84, 88, 104), V(hand.x - bx * 14, hand.y - by * 14), 3.2, false);
        Pts blk = {{end.x + bx * 16 - by * 23, end.y + by * 16 + bx * 23}, {end.x + bx * 16 + by * 23, end.y + by * 16 - bx * 23},
                   {end.x - bx * 16 + by * 23, end.y - by * 16 - bx * 23}, {end.x - bx * 16 - by * 23, end.y - by * 16 + bx * 23}};
        poly(stone, blk);
        poly(lerpColor(stone, WHITE, 0.32), {blk[0], blk[1], {end.x + bx * 8 + by * 23, end.y + by * 8 - bx * 23},
                                             {end.x + bx * 8 - by * 23, end.y + by * 8 + bx * 23}});
        poly(lerpColor(stone, INK, 0.35), {blk[2], blk[3], {end.x - bx * 9 - by * 23, end.y - by * 9 + bx * 23},
                                           {end.x - bx * 9 + by * 23, end.y - by * 9 - bx * 23}});
        poly(INK, blk, 1.4);
        for (double f_ : {-17.0, 17.0}) {
            Pts band = {{end.x + bx * 16 + by * (f_ - 3), end.y + by * 16 - bx * (f_ - 3)},
                        {end.x + bx * 16 + by * (f_ + 3), end.y + by * 16 - bx * (f_ + 3)},
                        {end.x - bx * 16 + by * (f_ + 3), end.y - by * 16 - bx * (f_ + 3)},
                        {end.x - bx * 16 + by * (f_ - 3), end.y - by * 16 - bx * (f_ - 3)}};
            opoly(Color(92, 96, 112), band, 0.8);
            C(Color(170, 176, 192), V(end.x + bx * 11 + by * f_, end.y + by * 11 - bx * f_), 1.4);
        }
        opoly(Color(78, 82, 98), {{end.x + bx * 8, end.y + by * 8}, {end.x - by * 7, end.y + bx * 7},
                                  {end.x - bx * 8, end.y - by * 8}, {end.x + by * 7, end.y - bx * 7}}, 0.8);
        line(INK, V(end.x + bx * 5 - by * 21, end.y + by * 5 + bx * 21), V(end.x - bx * 5 - by * 13, end.y - by * 5 + bx * 13), 1.1);
        line(INK, V(end.x + bx * 10 + by * 12, end.y + by * 10 - bx * 12), V(end.x + bx * 2 + by * 19, end.y + by * 2 - bx * 19), 1.1);
        if (power) halo(acc, end, 18, 160);
    }
    void shield(V hand, Color fill) {
        double cx_ = hand.x + 6, cy_ = hand.y + 2;
        Pts pts = {{cx_ - 15, cy_ - h * 0.29}, {cx_ + 15, cy_ - h * 0.29}, {cx_ + 17, cy_ + h * 0.05},
                   {cx_, cy_ + h * 0.33}, {cx_ - 17, cy_ + h * 0.05}};
        opoly(steel, pts, 1.3);
        poly(lerpColor(steel, WHITE, 0.35), {pts[0], pts[1], {pts[1].x - 2, pts[1].y + 3}, {pts[0].x + 2, pts[0].y + 3}});
        Pts inner = {{cx_ - 10.5, cy_ - h * 0.24}, {cx_ + 10.5, cy_ - h * 0.24}, {cx_ + 12.5, cy_ + h * 0.04},
                     {cx_, cy_ + h * 0.26}, {cx_ - 12.5, cy_ + h * 0.04}};
        opoly(fill, inner, 0.9);
        poly(lerpColor(fill, WHITE, 0.22), {inner[0], {cx_, inner[0].y}, {cx_, inner[3].y}, inner[4]});
        line(lerpColor(fill, INK, 0.4), V(cx_ + 4, cy_ - h * 0.2), V(cx_ + 8, cy_ - h * 0.1), 1);
        line(lerpColor(fill, INK, 0.4), V(cx_ - 7, cy_ + h * 0.1), V(cx_ - 3, cy_ + h * 0.16), 1);
        halo(acc, V(cx_, cy_ - 1), 10 + power * 0.08, 140 + power);
        Pts gem = {{cx_, cy_ - 12}, {cx_ + 6.5, cy_ - 1}, {cx_, cy_ + 10}, {cx_ - 6.5, cy_ - 1}};
        opoly(acc, gem, 0.9);
        poly(WHITE, {gem[0], {cx_ + 2.4, cy_ - 2}, {cx_ - 2.4, cy_ - 2}});
        const double rv[5][2] = {{-11, -h * 0.262}, {11, -h * 0.262}, {0, h * 0.29}, {-14, 0}, {14, 0}};
        for (auto& r : rv) C(INK, V(cx_ + r[0], cy_ + r[1]), 1.5);
    }
    void bronn() {
        std::string weapon = info.weapon.empty() ? "shield" : info.weapon;
        power = striking ? 60 : 0;
        steel = col.get("steel", lerpColor(skin, WHITE, 0.25));
        stone = col.get("stone", skin);
        Color beltc = col.get("belt", dark);
        Color plate_hi = lerpColor(main, WHITE, 0.28), plate_lo = lerpColor(main, INK, 0.4);
        limb(plate_lo, sb, eb, hb_, aw + 3, plate_lo, aw + 3);
        oC(plate_lo, hb_, aw / 2.0 + 3.5, false);
        if (weapon == "maul") shield(hb_, mid);
        else maul(hb_, -1.72);
        limb(plate_lo, hpb, kb, trim(kb, fb, 6), lw + 4, plate_lo, lw + 3);
        shoe(kb, fb, lerpColor(plate_lo, INK, 0.15), lw + 16, 11, true);
        oC(plate_lo, V(sh.x - w * 0.3, sh.y + 3), w * 0.2, false);
        double tw2 = w * 0.82, hw2 = w * 0.56;
        torso(main, tw2, hw2, 4, mid);
        poly(plate_hi, {{sh.x - tw2 * 0.1, sh.y + 1}, {sh.x + tw2 / 2 - 3, sh.y + 1}, {sh.x + tw2 / 2 - 6, sh.y + 6},
                        {sh.x - tw2 * 0.06, sh.y + 6}});
        for (double k : {0.36, 0.68}) {
            double yy = sh.y + (hip.y - sh.y) * k;
            double xw = (tw2 + (hw2 - tw2) * k) / 2 - 1;
            double xc = sh.x + (hip.x - sh.x) * k;
            line(INK, V(xc - xw, yy), V(xc + xw, yy), 1.3);
            line(plate_hi, V(xc - xw * 0.1, yy + 1.6), V(xc + xw - 2, yy + 1.6), 1);
        }
        line(INK, V(sh.x + 1, sh.y + 2), V(hip.x + 1, hip.y - 1), 1.2);
        line(INK, V(sh.x - 8, sh.y + 9), V(sh.x - 5, sh.y + 13), 1);
        line(INK, V(sh.x - 5, sh.y + 13), V(sh.x - 9, sh.y + 16), 1);
        line(INK, V(sh.x + 9, sh.y + 16), V(sh.x + 12, sh.y + 21), 1);
        line(INK, V(hip.x - hw2 / 2 - 1, hip.y + 1), V(hip.x + hw2 / 2 + 1, hip.y + 1), 8.5);
        line(beltc, V(hip.x - hw2 / 2, hip.y + 1), V(hip.x + hw2 / 2, hip.y + 1), 6);
        line(lerpColor(beltc, WHITE, 0.25), V(hip.x - hw2 / 2, hip.y - 1), V(hip.x + hw2 / 2, hip.y - 1), 1.2);
        obox(steel, V(hip.x + 2, hip.y + 1), 8, 8, 1);
        opoly(mid, {{hip.x - 2, hip.y + 5}, {hip.x + hw2 / 2 + 3, hip.y + 5}, {hip.x + hw2 / 2 - 1, hip.y + 17}, {hip.x + 2, hip.y + 19}});
        limb(main, hpf, kf, trim(kf, ff, 6), lw + 4, mid, lw + 3);
        bands(kf, ff, INK, {0.5}, (lw + 3) / 2.0, 1.2);
        oC(main, kf, lw / 2.0 + 3.5);
        QFn Q = shoe(kf, ff, mid, lw + 16, 11, true);
        line(INK, Q(lw * 0.45, 0.5), Q(lw * 0.45, 10.0), 1.1);
        obox(plate_lo, V(head.x - hr * 0.3, head.y + hr * 1.0), hr * 1.5, 7, 3);
        double hw_ = hr * 2 + 13, hh_ = hr * 2 + 11;
        obox(skin, head, hw_, hh_, 6);
        box(lerpColor(skin, WHITE, 0.3), V(head.x + 1, head.y - hh_ * 0.36), hw_ - 4, hh_ * 0.2, 4);
        poly(lerpColor(skin, INK, 0.3), {{head.x - hw_ / 2 + 1, head.y - hh_ * 0.24}, {head.x - hw_ * 0.2, head.y - hh_ * 0.24},
                                         {head.x - hw_ * 0.2, head.y + hh_ / 2 - 1}, {head.x - hw_ / 2 + 1, head.y + hh_ / 2 - 1}});
        box(INK, head, hw_, hh_, 6, 1.2);
        line(INK, V(head.x - hw_ / 2, head.y - hh_ * 0.25), V(head.x + hw_ / 2, head.y - hh_ * 0.25), 1.2);
        for (double rx : {-0.34, 0.0, 0.34}) C(INK, V(head.x + hw_ * rx, head.y - hh_ * 0.37), 1.3);
        double vis = !pained ? 0.8 + 0.2 * std::sin(t * 0.13) : 0.35 + 0.3 * (long(t) % 6 < 3);
        Color vcol = lerpColor(Color(20, 60, 80), acc, vis);
        halo(acc, V(head.x + hr * 0.6, head.y + 1), 13 + power * 0.1, int(140 * vis) + power);
        double vx0 = head.x - hr * 0.15, vx1 = head.x + hw_ / 2 - 1, vy_ = head.y - 1;
        line(INK, V(vx0 - 1, vy_), V(vx1, vy_), 8);
        line(INK, V(head.x + hr * 0.55, vy_), V(head.x + hr * 0.55, head.y + hh_ * 0.38), 6.5);
        line(vcol, V(vx0, vy_), V(vx1 - 1, vy_), blink ? 1.5 : 5);
        line(vcol, V(head.x + hr * 0.55, vy_), V(head.x + hr * 0.55, head.y + hh_ * 0.34), 3.8);
        line(WHITE, V(vx0 + 3, vy_ - 1), V(vx0 + 8, vy_ - 1), 1);
        for (double k : {0.22, 0.34})
            line(INK, V(head.x - hr * 0.7, head.y + hh_ * k), V(head.x - hr * 0.15, head.y + hh_ * k), 1.3);
        oC(main, V(sh.x + w * 0.17, sh.y + 4), w * 0.27);
        line(INK, V(sh.x + w * 0.17 - w * 0.22, sh.y + 8), V(sh.x + w * 0.17 + w * 0.22, sh.y + 8), 1.2);
        limb(main, sf, ef, hf, aw + 3, mid, aw + 3);
        oC(mid, ef, aw / 2.0 + 2.5, false);
        oC(main, hf, aw / 2.0 + 3.5);
        if (weapon == "maul") maul(hf, P.wp);
        else shield(hf, main);
    }

    // ---------------- NYX: hooded star-mage in a tattered cloak, floppy hat, dark star orb
    void nyx() {
        struct St { double cx, sy; int i; };
        std::vector<St> stars;
        for (int i = 0; i < 3; ++i) stars.push_back({std::cos(t * 0.06 + i * TAU / 3), std::sin(t * 0.06 + i * TAU / 3), i});
        Color hatc = col.get("hat", main), hatband = col.get("hatband", acc);
        Color handc = lerpColor(main, WHITE, 0.35);
        Color cloak_lo = lerpColor(dark, INK, 0.35);
        auto swirl = [&](int j) {
            double a0 = t * 0.055 + j * PI;
            Pts pts;
            for (int q = 0; q < 12; ++q)
                pts.push_back({std::cos(a0 + q * 0.2) * w * (1.05 + q * 0.03),
                               sh.y + h * (0.16 + 0.36 * j) + std::sin(a0 + q * 0.2) * h * 0.12 - q * 1.4});
            ribbon(pts, lerpColor(acc, dark, 0.3), 0, 1.5, [](double u) { return 3.6 * std::sin(PI * u) + 0.5; });
            ribbon(slice(pts, 2, 10), lerpColor(acc, WHITE, 0.4), 0, 1.5, [](double u) { return 1.3 * std::sin(PI * u) + 0.3; });
            for (int q : {1, 5, 9}) poly(WHITE, starPoints(pts[q].x, pts[q].y - 4, 2.2 + std::sin(t * 0.2 + q), t * 0.1));
        };
        auto orbitStar = [&](double cx_, double sy_, int i, double size) {
            for (int back : {2, 1}) {
                double a = t * 0.06 + i * TAU / 3 - back * 0.16;
                poly(lerpColor(acc, dark, 0.35 * back),
                     starPoints(std::cos(a) * w * 0.95, sh.y + h * 0.25 + std::sin(a) * 10, size - back * 1.5, t * 0.05));
            }
            poly(acc, starPoints(cx_ * w * 0.95, sh.y + h * 0.25 + sy_ * 10, size, t * 0.05));
            C(WHITE, V(cx_ * w * 0.95, sh.y + h * 0.25 + sy_ * 10), 1.3);
        };
        auto sleeve = [&](V elbow, V hand, Color c1) {
            double n = std::hypot(hand.x - elbow.x, hand.y - elbow.y);
            if (n == 0) n = 1;
            double px = -(hand.y - elbow.y) / n, py = (hand.x - elbow.x) / n;
            double sag = 3 + std::sin(t * 0.1) * 1.5;
            opoly(c1, {elbow, {hand.x + px * 11, hand.y + py * 11 + sag}, {hand.x + px * 5 - 3, hand.y + py * 5 + sag + 5},
                       {hand.x, hand.y + 1}, {hand.x - px * 5 - 3, hand.y - py * 5 + 4}, {hand.x - px * 10, hand.y - py * 10}});
            line(lerpColor(c1, WHITE, 0.25), elbow, V(hand.x - px * 6, hand.y - py * 6), 1.3);
        };
        swirl(0);
        for (auto& st : stars)
            if (st.sy < 0) orbitStar(st.cx, st.sy, st.i, 5);
        limb(cloak_lo, sb, eb, hb_, aw + 1);
        sleeve(eb, hb_, cloak_lo);
        oC(lerpColor(handc, INK, 0.3), hb_, 3.6, false);
        Pts hang = chain("robe", hip, 3, -hip.y / 3, 0.9, 0.82, 0.3);
        double hx_ = hang.back().x, mx_ = hang[1].x;
        clothStrip("wisp", V(hx_ - w * 0.3, hang.back().y - 8), 5, 7, cloak_lo, 13, 1.5, 0.25, 0.45, 0.6);
        const double rag[11] = {0, -10, 3, -13, -1, -9, 4, -12, -2, -8, 1};
        Pts hem;
        for (int i = 0; i < 11; ++i)
            hem.push_back({hx_ + (i / 10.0 - 0.5) * w * 1.55, hang.back().y + rag[i] + std::sin(t * 0.15 + i) * 2});
        Pts robe = {{sh.x - tw / 2, sh.y}, {sh.x + tw / 2, sh.y}, {mx_ + w * 0.52, hip.y + 6}};
        robe = cat(robe, rev(hem));
        robe.push_back({mx_ - w * 0.52, hip.y + 6});
        opoly(dark, robe);
        double inner_y = hang.back().y - 17;
        opoly(main, {{sh.x - tw / 2 + 3, sh.y}, {sh.x + tw / 2 - 3, sh.y}, {mx_ + w * 0.42, hip.y + 4},
                     {hx_ + w * 0.46, inner_y}, {hx_ + w * 0.2, inner_y + 5}, {hx_ - w * 0.1, inner_y - 2},
                     {hx_ - w * 0.46, inner_y + 3}, {mx_ - w * 0.42, hip.y + 4}}, 0.9);
        poly(light, {{sh.x + tw * 0.12, sh.y + 2}, {sh.x + tw / 2 - 4, sh.y + 2}, {mx_ + w * 0.4, hip.y + 4},
                     {hx_ + w * 0.44, inner_y - 1}, {hx_ + w * 0.3, inner_y}});
        for (double fx_ : {-0.22, 0.05}) line(dark, V(sh.x + fx_ * w, sh.y + 10), V(hx_ + fx_ * w * 1.4, inner_y - 3), 1.2);
        line(lerpColor(acc, dark, 0.4), V(hip.x - hw / 2, hip.y - 4), V(hip.x + hw / 2, hip.y - 2), 2.2);
        const double sp[4][2] = {{0.2, 0.3}, {-0.25, 0.55}, {0.3, 0.75}, {-0.1, 0.42}};
        for (int k = 0; k < 4; ++k) {
            double ax = sp[k][0], ay = sp[k][1];
            double tw_ = 0.6 + 0.4 * std::sin(t * 0.12 + k * 2);
            poly(lerpColor(main, WHITE, tw_), starPoints(sh.x + ax * w * 0.6 + (hx_ - sh.x) * ay,
                                                         sh.y + (inner_y - sh.y) * ay, 1.8 + tw_ * 1.4, t * 0.03));
        }
        opoly(dark, {{head.x - hr * 1.25, head.y + hr * 0.4}, {head.x - hr * 1.0, head.y - hr * 0.6},
                     {head.x + hr * 0.75, head.y - hr * 0.7}, {head.x + hr * 1.15, head.y + hr * 0.6},
                     {sh.x + tw / 2 + 4, sh.y + 6}, {sh.x + 3, sh.y + 11}, {sh.x - tw / 2 - 5, sh.y + 7}});
        poly(main, {{head.x + hr * 0.75, head.y - hr * 0.7}, {head.x + hr * 1.15, head.y + hr * 0.6},
                    {sh.x + tw / 2 + 4, sh.y + 6}, {head.x + hr * 0.7, head.y + hr * 0.9}});
        C(skin, V(head.x + hr * 0.1, head.y + hr * 0.12), hr * 0.9);
        double eye_y = head.y + hr * 0.36;
        for (double ex : {0.16, 0.62}) {
            double ex_ = head.x + hr * ex;
            halo(acc, V(ex_, eye_y), 7, 170);
            if (blink || pained) {
                line(acc, V(ex_ - 3, eye_y), V(ex_ + 3, eye_y), 1.5);
            } else {
                poly(acc, {{ex_ - 3.4, eye_y - 0.5}, {ex_ + 3.6, eye_y - 2.8}, {ex_ + 2.8, eye_y + 3.2}});
                C(WHITE, V(ex_ + 1.3, eye_y), 1);
            }
        }
        ooval(hatc, V(head.x, head.y - hr * 0.5), hr * 4.0, hr * 1.15);
        oval(lerpColor(hatc, WHITE, 0.22), V(head.x + hr * 0.5, head.y - hr * 0.62), hr * 2.3, hr * 0.42);
        oval(lerpColor(hatc, INK, 0.35), V(head.x - hr * 0.2, head.y - hr * 0.3), hr * 3.0, hr * 0.4);
        Pts cone = chain("hat", V(head.x, head.y - hr * 1.25), 3, hr * 0.7, -0.5, 0.84, 0.25);
        V base_l(head.x - hr * 1.0, head.y - hr * 0.55), base_r(head.x + hr * 1.0, head.y - hr * 0.55);
        opoly(hatc, {base_l, base_r, {cone[1].x + 6, cone[1].y}, {cone[2].x + 3, cone[2].y}, cone[3],
                     {cone[2].x - 3, cone[2].y}, {cone[1].x - 6, cone[1].y}});
        poly(lerpColor(hatc, WHITE, 0.24), {base_r, {head.x + hr * 0.3, head.y - hr * 0.55}, {cone[1].x + 1, cone[1].y}, {cone[1].x + 5, cone[1].y}});
        poly(lerpColor(hatc, INK, 0.3), {base_l, {head.x - hr * 0.4, head.y - hr * 0.55}, {cone[1].x - 2, cone[1].y}, {cone[1].x - 5.5, cone[1].y}});
        line(INK, V(head.x - hr * 1.0, head.y - hr * 0.66), V(head.x + hr * 1.0, head.y - hr * 0.66), 5.5);
        line(hatband, V(head.x - hr * 0.97, head.y - hr * 0.66), V(head.x + hr * 0.97, head.y - hr * 0.66), 3.6);
        limb(dark, sf, ef, hf, aw + 1);
        sleeve(ef, hf, main);
        oC(handc, hf, 4, false);
        V orb = P.hasOrb ? P.orb : hf;
        if (std::hypot(orb.x - hf.x, orb.y - hf.y) > 16) {
            line(acc, hf, orb, 1);
            for (int q = 1; q < 6; ++q) {
                double u = q / 6.0;
                V p_(hf.x + (orb.x - hf.x) * u, hf.y + (orb.y - hf.y) * u + std::sin(t * 0.4 + q) * 3);
                poly(lerpColor(acc, WHITE, u), starPoints(p_.x, p_.y, 1.5 + 2.5 * u, t * 0.2 + q));
            }
        }
        double pulse = 1 + 0.15 * std::sin(t * 0.2);
        halo(acc, orb, 20 * pulse, 170);
        C(INK, orb, 11.5 * pulse);
        C(Color(30, 16, 52), orb, 10.5 * pulse);
        C(Color(74, 42, 116), V(orb.x - 2.5, orb.y - 3), 4.2 * pulse);
        C(acc, orb, 10.5 * pulse, 1.5);
        poly(acc, starPoints(orb.x, orb.y, 7 * pulse, t * 0.1));
        C(WHITE, orb, 1.6);
        for (int i = 0; i < 3; ++i) {
            double a = t * 0.17 + i * TAU / 3;
            C(WHITE, V(orb.x + std::cos(a) * 14, orb.y + std::sin(a) * 14), 1.2);
        }
        swirl(1);
        for (auto& st : stars)
            if (st.sy >= 0) orbitStar(st.cx, st.sy, st.i, 6);
    }

    // ---------------- RHEA: brawler with a mane of fire, burning cloth-bound fists, torn clothes
    void rhea() {
        bool hot = info.hot;
        Color flameC = col.get("flame", Color(240, 92, 24));
        std::array<Color, 4> fire = {flameC, lerpColor(flameC, acc, 0.6), acc, Color(255, 250, 228)};
        Color shade = lerpColor(skin, dark, 0.38);
        Color pants = lerpColor(dark, main, 0.45), pants_lo = lerpColor(dark, INK, 0.2);
        Color bandc = col.get("band", dark), wrapc = col.get("wrap", acc);
        Color wrap_lo = lerpColor(wrapc, INK, 0.35);
        auto leg2 = [&](V a, V j, V b, Color c, bool front) {
            limb(c, a, j, trim(j, b, 5), lw + 4, c, lw + 1);
            double sx_ = b.x - j.x, sy_ = b.y - j.y;
            opoly(c, {{j.x + sx_ * 0.5 - lw / 2.0 - 2, j.y + sy_ * 0.5}, {j.x + sx_ * 0.5 + lw / 2.0 + 2, j.y + sy_ * 0.5},
                      {j.x + sx_ * 0.7 + 3, j.y + sy_ * 0.7}, {j.x + sx_ * 0.6, j.y + sy_ * 0.6},
                      {j.x + sx_ * 0.74 - 3, j.y + sy_ * 0.74}}, 0.9);
            bands(j, b, wrapc, {0.74, 0.86}, lw / 2.0 + 0.5, 2.2);
            QFn Q = shoe(j, b, skin, lw + 9, 6.5, false, true);
            line(wrapc, Q(-2.0, 5.5), Q(5.0, 4.5), 2.0);
            if (hot) flameStrand(chain(front ? "foot1" : "foot0", V(b.x - 4, b.y - 1), 4, 5, -0.5, 0.78, 0.9), 9, fire, 5.0);
        };
        auto fist = [&](V elbow, V hand, Color c, const std::string& name) {
            double n = std::hypot(hand.x - elbow.x, hand.y - elbow.y);
            if (n == 0) n = 1;
            double ux = (hand.x - elbow.x) / n, uy = (hand.y - elbow.y) / n;
            if (hot) halo(acc, hand, 24, 170);
            flameStrand(chain(name, V(hand.x - ux * 2, hand.y - uy * 2), 6, hot ? 7 : 5, -0.55, 0.78, 0.9), hot ? 17 : 11, fire, 3.0);
            oC(c, hand, aw / 2.0 + 3.6, false);
            bands(V(hand.x - ux * 4, hand.y - uy * 4), V(hand.x + ux * 4, hand.y + uy * 4), wrapc, {0.1, 0.5, 0.9}, 5, 1.8);
            bands(elbow, hand, wrapc, {0.45, 0.62, 0.78}, aw / 2.0 + 0.5, 1.9);
            bands(elbow, hand, wrap_lo, {0.53, 0.7}, aw / 2.0 + 0.5, 0.9);
        };
        limb(shade, sb, eb, hb_, aw + 1);
        fist(eb, hb_, shade, "fistb");
        leg2(hpb, kb, fb, pants_lo, false);
        clothStrip("band2", V(head.x - hr * 0.95, head.y - hr * 0.2), 5, 6, lerpColor(bandc, INK, 0.3), 5.5, 2.5, 0.26, 0.4);
        clothStrip("band1", V(head.x - hr * 0.95, head.y - hr * 0.3), 6, 6, bandc, 6, 2.5, 0.2, 0.4);
        torso(skin, tw, hw, 4, shade);
        line(shade, V(hip.x + 1, hip.y - 9), V(hip.x + 1, hip.y - 2), 1.3);
        line(shade, V(hip.x - 4, hip.y - 6), V(hip.x + 6, hip.y - 6), 1.3);
        double k = 0.6;
        double yb = sh.y + (hip.y - sh.y) * k;
        double xl = sh.x - tw / 2 + (hip.x - hw / 2 - sh.x + tw / 2) * k;
        double xr = sh.x + tw / 2 + (hip.x + hw / 2 - sh.x - tw / 2) * k;
        Pts top = slice(torsoPts(tw, hw), 0, 4);
        top = cat(top, {{xr, yb}, {xr - (xr - xl) * 0.2, yb + 4}, {xr - (xr - xl) * 0.4, yb - 1},
                        {xr - (xr - xl) * 0.6, yb + 5}, {xr - (xr - xl) * 0.8, yb}, {xl, yb + 3}});
        opoly(main, top);
        poly(mid, {top[0], top[1], {sh.x - tw * 0.14, sh.y - 1}, {xl + (xr - xl) * 0.3, yb}, {xl, yb + 3}});
        line(light, V(sh.x, sh.y + 1.5), V(sh.x + tw * 0.4, sh.y + 1.5), 1.5);
        line(mid, V(sh.x + tw * 0.2, sh.y + 4), V(sh.x + tw * 0.1, yb - 3), 1.1);
        line(INK, V(hip.x - hw / 2 - 1, hip.y), V(hip.x + hw / 2 + 1, hip.y), 7);
        line(bandc, V(hip.x - hw / 2, hip.y), V(hip.x + hw / 2, hip.y), 4.6);
        clothStrip("rsash", V(hip.x - hw * 0.4, hip.y + 2), 4, 7, bandc, 5.5, 2.5, 0.5, 0.2, 0.3);
        leg2(hpf, kf, ff, pants, true);
        double grow = hot ? 1.3 : 1.0;
        halo(fire[0], V(head.x - 2, head.y - hr * 1.6), hr * 3.0 * grow, 90);
        const double mane[7][3] = {{-1.3, 7, 17}, {1.15, 7, 16}, {-0.9, 8, 21}, {0.75, 8, 20},
                                   {-0.45, 9, 25}, {0.32, 9, 25}, {-0.06, 11, 29}};
        for (int i = 0; i < 7; ++i) {
            double off = mane[i][0], wd = mane[i][2];
            int n_ = int(mane[i][1]);
            V base(head.x + off * hr * 0.9 - 2, head.y - hr * (0.5 + 0.35 * (1 - std::abs(off) / 1.25)));
            Pts pts = chain("mane" + std::to_string(i), base, n_, hr * 0.5 * grow, -0.5, 0.78, 1.3, 0.14 - off * 0.55);
            flameStrand(pts, wd * grow, fire, i * 1.7);
        }
        face(0.23, 2.0, true);
        poly(fire[1], {{head.x - hr * 0.95, head.y - hr * 0.2}, {head.x - hr * 0.55, head.y - hr * 1.0},
                       {head.x + hr * 0.8, head.y - hr * 0.86}, {head.x + hr * 1.0, head.y - hr * 0.4}});
        line(INK, V(head.x - hr * 1.02, head.y - hr * 0.3), V(head.x + hr * 1.04, head.y - hr * 0.4), 6.6);
        line(bandc, V(head.x - hr, head.y - hr * 0.3), V(head.x + hr * 1.02, head.y - hr * 0.4), 4.6);
        line(lerpColor(bandc, WHITE, 0.3), V(head.x - hr * 0.7, head.y - hr * 0.42), V(head.x + hr * 0.9, head.y - hr * 0.5), 1);
        limb(skin, sf, ef, hf, aw + 1);
        line(shade, V(sf.x + (ef.x - sf.x) * 0.4 - 2, sf.y + (ef.y - sf.y) * 0.4 - 1),
             V(sf.x + (ef.x - sf.x) * 0.6 + 2, sf.y + (ef.y - sf.y) * 0.6 + 1), 1.2);
        fist(ef, hf, skin, "fistf");
    }
};
}  // namespace

SurfacePtr renderFighterSurface(const FighterSpec& spec, const Palette& col, const Pose& P, int facing, double t,
                                const std::string& key, Vec2 vel, const AnimInfo& info, ClothSim* cloth, Vec2 origin) {
    auto s = Surface::make(SPR * SS, SPR * SS, true);
    Painter p(*s, spec, col, P, facing, t, key, info, cloth);
    bool floaty = spec.style == Style::Voidcaller && key != "ledge" && key != "grabbed";
    p.yo = floaty ? -(8 + std::sin(t * 0.07) * 3) : 0;
    if (cloth) cloth->begin(origin.x, origin.y + p.yo, facing);
    p.setup(vel);
    p.smear();
    switch (spec.style) {
        case Style::Weaver: p.kai(); break;
        case Style::Sentinel: p.bronn(); break;
        case Style::Voidcaller: p.nyx(); break;
        case Style::Kindled: p.rhea(); break;
    }
    outlineSilhouette(*s, 200, INK, 2 * SS);
    return xform::smoothscale(*s, SPR, SPR);
}

SurfacePtr quickSprite(const FighterSpec& spec, const Palette& col, const std::string& key, int facing, double t, double zoom) {
    static std::map<std::string, std::unique_ptr<ClothSim>> menuCloth;
    std::string ck = spec.name + "|" + key + "|" + std::to_string(facing);
    auto it = menuCloth.find(ck);
    if (it == menuCloth.end()) {
        it = menuCloth.emplace(ck, std::make_unique<ClothSim>()).first;
        for (int i = 0; i < 90; ++i)
            renderFighterSurface(spec, col, targetPose(spec, key, t - 90 + i), facing, t - 90 + i, key, {0, 0}, AnimInfo(),
                                 it->second.get());
    }
    auto spr = renderFighterSurface(spec, col, targetPose(spec, key, t), facing, t, key, {0, 0}, AnimInfo(), it->second.get());
    auto small = xform::smoothscale(*spr, int(SPR * zoom / PIXEL), int(SPR * zoom / PIXEL));
    return xform::scale(*small, int(SPR * zoom), int(SPR * zoom));
}
