#include "gfx.h"

#include <unordered_map>
#include <mutex>
#include <thread>
#include <functional>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

// ------------------------------------------------------------------ colours and rects
uint32_t pack(Color c) { return (uint32_t(c.a) << 24) | (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b; }
Color unpack(uint32_t p) { return Color((p >> 16) & 255, (p >> 8) & 255, p & 255, p >> 24); }

static inline int A_(uint32_t p) { return p >> 24; }
static inline int R_(uint32_t p) { return (p >> 16) & 255; }
static inline int G_(uint32_t p) { return (p >> 8) & 255; }
static inline int B_(uint32_t p) { return p & 255; }
static inline uint32_t P_(int r, int g, int b, int a) {
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

Rect Rect::clip(const Rect& o) const {
    int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    int x1 = std::min(x + w, o.x + o.w), y1 = std::min(y + h, o.y + o.h);
    if (x1 <= x0 || y1 <= y0) return Rect(x0, y0, 0, 0);
    return Rect(x0, y0, x1 - x0, y1 - y0);
}
bool Rect::colliderect(const Rect& o) const {
    return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h && w > 0 && h > 0 && o.w > 0 && o.h > 0;
}
void Rect::clampInside(const Rect& o) {
    if (w >= o.w) x = o.x + o.w / 2 - w / 2;
    else if (x < o.x) x = o.x;
    else if (x + w > o.x + o.w) x = o.x + o.w - w;
    if (h >= o.h) y = o.y + o.h / 2 - h / 2;
    else if (y < o.y) y = o.y;
    else if (y + h > o.y + o.h) y = o.y + o.h - h;
}

// ------------------------------------------------------------------ surfaces
Surface::Surface(int w_, int h_, bool alpha) : w(std::max(0, w_)), h(std::max(0, h_)), hasAlpha(alpha) {
    px.assign(size_t(w) * h, alpha ? 0u : 0xFF000000u);
    clipRect = rect();
}

void Surface::setAt(int x, int y, Color c) {
    if (!clipRect.collidepoint(x, y)) return;
    put(x, y, pack(hasAlpha ? c : c.withAlpha(255)));
}
Color Surface::getAt(int x, int y) const { return unpack(get(x, y)); }

void Surface::hspan(int y, int x0, int x1, uint32_t c) {
    if (y < clipRect.y || y >= clipRect.bottom()) return;
    x0 = std::max(x0, clipRect.x);
    x1 = std::min(x1, clipRect.right() - 1);
    if (x1 < x0) return;
    uint32_t* row = &px[size_t(y) * w];
    std::fill(row + x0, row + x1 + 1, c);
}

void Surface::fill(Color c) { fill(c, rect()); }
void Surface::fill(Color c, const Rect& r) {
    Rect cr = r.clip(clipRect);
    uint32_t p = pack(hasAlpha ? c : c.withAlpha(255));
    for (int y = cr.y; y < cr.bottom(); ++y) hspan(y, cr.x, cr.right() - 1, p);
}

static inline uint32_t blendPixel(uint32_t d, int sr, int sg, int sb, int sa, Blend mode, bool dstAlpha) {
    int dr = R_(d), dg = G_(d), db = B_(d), da = A_(d);
    switch (mode) {
        case Blend::RGBA_MULT:
            dr = (dr * sr + 255) >> 8; dg = (dg * sg + 255) >> 8; db = (db * sb + 255) >> 8;
            if (dstAlpha) da = (da * sa + 255) >> 8;
            break;
        case Blend::RGB_MULT:
            dr = (dr * sr + 255) >> 8; dg = (dg * sg + 255) >> 8; db = (db * sb + 255) >> 8;
            break;
        case Blend::RGB_ADD:
            dr = std::min(255, dr + sr); dg = std::min(255, dg + sg); db = std::min(255, db + sb);
            break;
        case Blend::RGBA_ADD:
            dr = std::min(255, dr + sr); dg = std::min(255, dg + sg); db = std::min(255, db + sb);
            if (dstAlpha) da = std::min(255, da + sa);
            break;
        case Blend::RGBA_MIN:
            dr = std::min(dr, sr); dg = std::min(dg, sg); db = std::min(db, sb);
            if (dstAlpha) da = std::min(da, sa);
            break;
        case Blend::Normal:
            if (sa >= 255) return P_(sr, sg, sb, 255);
            if (sa <= 0) return d;
            if (dstAlpha && da == 0) return P_(sr, sg, sb, sa);       // (pygame copies onto empty pixels)
            dr = dr + (((sr - dr) * sa + sr) >> 8);
            dg = dg + (((sg - dg) * sa + sg) >> 8);
            db = db + (((sb - db) * sa + sb) >> 8);
            da = dstAlpha ? sa + da - (sa * da) / 255 : 255;
            break;
    }
    return P_(std::clamp(dr, 0, 255), std::clamp(dg, 0, 255), std::clamp(db, 0, 255), dstAlpha ? std::clamp(da, 0, 255) : 255);
}

void Surface::fill(Color c, Blend mode) {
    if (mode == Blend::Normal) { fill(c); return; }
    Rect cr = clipRect;
    for (int y = cr.y; y < cr.bottom(); ++y) {
        uint32_t* row = &px[size_t(y) * w];
        for (int x = cr.x; x < cr.right(); ++x) row[x] = blendPixel(row[x], c.r, c.g, c.b, c.a, mode, hasAlpha);
    }
}

void Surface::blit(const Surface& src, double x, double y, Blend mode) { blit(src, x, y, src.rect(), mode); }

void Surface::blit(const Surface& src, double fx, double fy, const Rect& areaIn, Blend mode) {
    Rect area = areaIn.clip(src.rect());
    if (area.empty()) return;
    int dx0 = int(fx), dy0 = int(fy);                     // (truncates toward zero, as pygame does)
    Rect dst = Rect(dx0, dy0, area.w, area.h).clip(clipRect);
    if (dst.empty()) return;
    int sx0 = area.x + (dst.x - dx0), sy0 = area.y + (dst.y - dy0);
    int sa_mul = src.surfAlpha;
    bool plainCopy = mode == Blend::Normal && !src.hasAlpha && sa_mul >= 255;
    for (int j = 0; j < dst.h; ++j) {
        const uint32_t* srow = &src.px[size_t(sy0 + j) * src.w + sx0];
        uint32_t* drow = &px[size_t(dst.y + j) * w + dst.x];
        if (plainCopy) {
            std::copy(srow, srow + dst.w, drow);
            continue;
        }
        if (mode == Blend::Normal) {                      // the common case, kept tight
            const bool sAlpha = src.hasAlpha, dAlpha = hasAlpha;
            for (int i = 0; i < dst.w; ++i) {
                uint32_t s = srow[i];
                int a = sAlpha ? int(s >> 24) : 255;
                if (sa_mul < 255) a = (a * sa_mul) / 255;
                if (a == 0) continue;
                if (a >= 255) { drow[i] = s | 0xFF000000u; continue; }
                uint32_t d = drow[i];
                int da = int(d >> 24);
                if (dAlpha && da == 0) { drow[i] = (s & 0xFFFFFFu) | (uint32_t(a) << 24); continue; }
                int sr = R_(s), sg = G_(s), sb = B_(s), dr = R_(d), dg = G_(d), db = B_(d);
                dr += ((sr - dr) * a + sr) >> 8;
                dg += ((sg - dg) * a + sg) >> 8;
                db += ((sb - db) * a + sb) >> 8;
                da = dAlpha ? a + da - (a * da) / 255 : 255;
                drow[i] = P_(dr, dg, db, da);
            }
            continue;
        }
        for (int i = 0; i < dst.w; ++i) {
            uint32_t s = srow[i];
            int a = src.hasAlpha ? A_(s) : 255;
            drow[i] = blendPixel(drow[i], R_(s), G_(s), B_(s), a, mode, hasAlpha);
        }
    }
}

// ------------------------------------------------------------------ shapes
namespace {
uint32_t colorFor(const Surface& s, Color c) { return pack(s.hasAlpha ? c : c.withAlpha(255)); }

// scanline polygon fill (even-odd), sampling at pixel centres
void fillPolygon(Surface& s, uint32_t col, const std::vector<Vec2>& pts) {
    if (pts.size() < 3) return;
    double miny = 1e18, maxy = -1e18;
    for (auto& p : pts) { miny = std::min(miny, p.y); maxy = std::max(maxy, p.y); }
    int y0 = std::max(int(std::floor(miny)), s.clipRect.y), y1 = std::min(int(std::ceil(maxy)), s.clipRect.bottom() - 1);
    std::vector<double> xs;
    size_t n = pts.size();
    for (int y = y0; y <= y1; ++y) {
        double sy = y + 0.5;
        xs.clear();
        for (size_t i = 0; i < n; ++i) {
            const Vec2& a = pts[i];
            const Vec2& b = pts[(i + 1) % n];
            if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                xs.push_back(a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            int xa = int(std::ceil(xs[k] - 0.5)), xb = int(std::floor(xs[k + 1] - 0.5));
            if (xb >= xa) s.hspan(y, xa, xb, col);
        }
    }
}

void thinLine(Surface& s, uint32_t col, int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    const Rect& c = s.clipRect;
    for (int guard = 0; guard < 100000; ++guard) {
        if (x0 >= c.x && x0 < c.right() && y0 >= c.y && y0 < c.bottom()) s.put(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

bool inRoundRect(double px, double py, double x0, double y0, double x1, double y1, double r) {
    if (px < x0 || px >= x1 || py < y0 || py >= y1) return false;
    if (r <= 0) return true;
    double cx = px < x0 + r ? x0 + r : (px > x1 - r ? x1 - r : px);
    double cy = py < y0 + r ? y0 + r : (py > y1 - r ? y1 - r : py);
    double dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}
}  // namespace

namespace draw {
void rect(Surface& s, Color c, const Rect& r, int width, int radius) {
    if (r.empty()) return;
    uint32_t col = colorFor(s, c);
    int rad = std::min(radius, std::min(r.w, r.h) / 2);
    if (width <= 0 && rad <= 0) {
        Rect cr = r.clip(s.clipRect);
        for (int y = cr.y; y < cr.bottom(); ++y) s.hspan(y, cr.x, cr.right() - 1, col);
        return;
    }
    Rect cr = r.clip(s.clipRect);
    double x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
    for (int y = cr.y; y < cr.bottom(); ++y) {
        for (int x = cr.x; x < cr.right(); ++x) {
            double px = x + 0.5, py = y + 0.5;
            if (!inRoundRect(px, py, x0, y0, x1, y1, rad)) continue;
            if (width > 0 && inRoundRect(px, py, x0 + width, y0 + width, x1 - width, y1 - width, std::max(0, rad - width)))
                continue;
            s.put(x, y, col);
        }
    }
}

void circle(Surface& s, Color c, Vec2 center, double radius, int width) {
    if (radius < 1) return;
    uint32_t col = colorFor(s, c);
    double cx = std::floor(center.x) + 0.5, cy = std::floor(center.y) + 0.5;     // pygame centres on a pixel
    double r = std::floor(radius);
    double r2 = r * r, ri2 = (width > 0 && width < r) ? (r - width) * (r - width) : -1;
    int y0 = std::max(int(std::floor(cy - r)), s.clipRect.y), y1 = std::min(int(std::ceil(cy + r)), s.clipRect.bottom() - 1);
    for (int y = y0; y <= y1; ++y) {
        double dy = y + 0.5 - cy;
        double span2 = r2 - dy * dy;
        if (span2 < 0) continue;
        double hw = std::sqrt(span2);
        int xa = int(std::ceil(cx - hw - 0.5)), xb = int(std::floor(cx + hw - 0.5));
        if (ri2 < 0) { s.hspan(y, xa, xb, col); continue; }
        double in2 = ri2 - dy * dy;
        if (in2 <= 0) { s.hspan(y, xa, xb, col); continue; }
        double hi = std::sqrt(in2);
        int ia = int(std::ceil(cx - hi - 0.5)), ib = int(std::floor(cx + hi - 0.5));
        s.hspan(y, xa, ia - 1, col);
        s.hspan(y, ib + 1, xb, col);
    }
}

void ellipse(Surface& s, Color c, const Rect& r, int width) {
    if (r.w <= 0 || r.h <= 0) return;
    uint32_t col = colorFor(s, c);
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, a = r.w / 2.0, b = r.h / 2.0;
    double ai = a - width, bi = b - width;
    bool ring = width > 0 && ai > 0 && bi > 0;
    Rect cr = r.clip(s.clipRect);
    for (int y = cr.y; y < cr.bottom(); ++y) {
        double dy = (y + 0.5 - cy) / b;
        double t = 1 - dy * dy;
        if (t < 0) continue;
        double hw = a * std::sqrt(t);
        int xa = int(std::ceil(cx - hw - 0.5)), xb = int(std::floor(cx + hw - 0.5));
        if (!ring) { s.hspan(y, xa, xb, col); continue; }
        double dyi = (y + 0.5 - cy) / bi;
        double ti = 1 - dyi * dyi;
        if (ti <= 0) { s.hspan(y, xa, xb, col); continue; }
        double hi = ai * std::sqrt(ti);
        int ia = int(std::ceil(cx - hi - 0.5)), ib = int(std::floor(cx + hi - 0.5));
        s.hspan(y, xa, ia - 1, col);
        s.hspan(y, ib + 1, xb, col);
    }
}

void polygon(Surface& s, Color c, const std::vector<Vec2>& pts, int width) {
    if (pts.size() < 2) return;
    if (width > 0) { lines(s, c, true, pts, width); return; }
    uint32_t col = colorFor(s, c);
    fillPolygon(s, col, pts);
    for (size_t i = 0; i < pts.size(); ++i) {                 // pygame also draws the edges
        const Vec2& a = pts[i];
        const Vec2& b = pts[(i + 1) % pts.size()];
        thinLine(s, col, int(std::floor(a.x)), int(std::floor(a.y)), int(std::floor(b.x)), int(std::floor(b.y)));
    }
}

void line(Surface& s, Color c, Vec2 a, Vec2 b, int width) {
    if (width < 1) return;
    uint32_t col = colorFor(s, c);
    if (width == 1) {
        thinLine(s, col, int(std::floor(a.x)), int(std::floor(a.y)), int(std::floor(b.x)), int(std::floor(b.y)));
        return;
    }
    // pygame's thick lines are offset straight up/down (shallow lines) or left/right (steep lines)
    double dx = b.x - a.x, dy = b.y - a.y, h = width / 2.0;
    std::vector<Vec2> q;
    if (std::abs(dx) >= std::abs(dy))
        q = {{a.x, a.y - h}, {b.x + (dx >= 0 ? 0.5 : -0.5), b.y - h}, {b.x + (dx >= 0 ? 0.5 : -0.5), b.y + h}, {a.x, a.y + h}};
    else
        q = {{a.x - h, a.y}, {b.x - h, b.y + (dy >= 0 ? 0.5 : -0.5)}, {b.x + h, b.y + (dy >= 0 ? 0.5 : -0.5)}, {a.x + h, a.y}};
    fillPolygon(s, col, q);
}

void lines(Surface& s, Color c, bool closed, const std::vector<Vec2>& pts, int width) {
    if (pts.size() < 2) return;
    for (size_t i = 0; i + 1 < pts.size(); ++i) line(s, c, pts[i], pts[i + 1], width);
    if (closed) line(s, c, pts.back(), pts.front(), width);
}
}  // namespace draw

// ------------------------------------------------------------------ transforms
namespace xform {
SurfacePtr scale(const Surface& s, int w, int h) {
    auto out = Surface::make(std::max(1, w), std::max(1, h), s.hasAlpha);
    out->surfAlpha = 255;
    if (s.w == 0 || s.h == 0) return out;
    scaleInto(s, *out);
    return out;
}

void scaleInto(const Surface& s, Surface& d) {
    std::vector<int> xs(size_t(d.w));
    for (int x = 0; x < d.w; ++x) xs[size_t(x)] = std::min(s.w - 1, int((long long)x * s.w / d.w));
    const uint32_t fix = d.hasAlpha ? 0u : 0xFF000000u;
    int prevSy = -1;
    for (int y = 0; y < d.h; ++y) {
        int sy = std::min(s.h - 1, int((long long)y * s.h / d.h));
        uint32_t* drow = &d.px[size_t(y) * d.w];
        if (sy == prevSy) {                               // same source row as the line above: copy it
            std::copy(drow - d.w, drow, drow);
            continue;
        }
        prevSy = sy;
        const uint32_t* srow = &s.px[size_t(sy) * s.w];
        for (int x = 0; x < d.w; ++x) drow[x] = srow[xs[size_t(x)]] | fix;
    }
}

// Smooth resampling: a box filter when shrinking, linear when stretching (separable, with precomputed weights).
namespace {
struct Tap { int k; float w; };
std::vector<std::vector<Tap>> resampleTaps(int n, int m) {
    std::vector<std::vector<Tap>> taps(m);
    double ratio = double(n) / m;
    for (int i = 0; i < m; ++i) {
        if (m < n) {
            double a = i * ratio, b = a + ratio, total = 0;
            for (int k = int(a); k < n && k < b; ++k) {
                double wgt = std::min<double>(k + 1, b) - std::max<double>(k, a);
                if (wgt > 0) { taps[i].push_back({k, float(wgt)}); total += wgt; }
            }
            for (auto& t : taps[i]) t.w = float(t.w / (total > 0 ? total : 1));
        } else {
            double p = (i + 0.5) * ratio - 0.5;
            int i0 = int(std::floor(p));
            double t = p - i0;
            int a = std::clamp(i0, 0, n - 1), b = std::clamp(i0 + 1, 0, n - 1);
            taps[i].push_back({a, float(1 - t)});
            taps[i].push_back({b, float(t)});
        }
    }
    return taps;
}
}  // namespace

void smoothscaleInto(const Surface& s, const Rect& areaIn, Surface& d) {
    Rect area = areaIn.clip(s.rect());
    if (area.empty() || d.w <= 0 || d.h <= 0) return;
    int sw = area.w, sh = area.h, dw = d.w, dh = d.h;
    bool sa = s.hasAlpha, da = d.hasAlpha;
    if (sw == dw * 2 && sh == dh * 2) {                     // the common case: exactly half size
        for (int y = 0; y < dh; ++y) {
            const uint32_t* r0 = &s.px[size_t(area.y + 2 * y) * s.w + area.x];
            const uint32_t* r1 = r0 + s.w;
            uint32_t* out = &d.px[size_t(y) * d.w];
            for (int x = 0; x < dw; ++x) {
                uint32_t a = r0[2 * x], b = r0[2 * x + 1], c = r1[2 * x], e = r1[2 * x + 1];
                int R = (R_(a) + R_(b) + R_(c) + R_(e) + 2) >> 2, G = (G_(a) + G_(b) + G_(c) + G_(e) + 2) >> 2;
                int B = (B_(a) + B_(b) + B_(c) + B_(e) + 2) >> 2;
                int A = sa ? (A_(a) + A_(b) + A_(c) + A_(e) + 2) >> 2 : 255;
                out[x] = P_(R, G, B, da ? A : 255);
            }
        }
        return;
    }
    auto tx = resampleTaps(sw, dw), ty = resampleTaps(sh, dh);
    std::vector<float> mid(size_t(dw) * sh * 4);
    // big images (the camera view every frame) are split across the CPU cores
    auto parallelRows = [](int n, bool big, const std::function<void(int, int)>& fn) {
        unsigned hw = std::thread::hardware_concurrency();
        int parts = big ? int(std::clamp(hw, 1u, 8u)) : 1;
        if (parts <= 1) { fn(0, n); return; }
        std::vector<std::thread> th;
        int chunk = (n + parts - 1) / parts;
        for (int p = 1; p < parts; ++p) {
            int lo = p * chunk, hi = std::min(n, lo + chunk);
            if (lo < hi) th.emplace_back(fn, lo, hi);
        }
        fn(0, std::min(n, chunk));
        for (auto& t : th) t.join();
    };
    bool big = (long long)sw * sh > 300000;
    parallelRows(sh, big, [&](int lo, int hi) {
    for (int y = lo; y < hi; ++y) {
        const uint32_t* row = &s.px[size_t(area.y + y) * s.w + area.x];
        float* o = &mid[size_t(y) * dw * 4];
        for (int x = 0; x < dw; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (const Tap& t : tx[x]) {
                uint32_t p = row[t.k];
                r += R_(p) * t.w; g += G_(p) * t.w; b += B_(p) * t.w; a += (sa ? A_(p) : 255) * t.w;
            }
            o[x * 4] = r; o[x * 4 + 1] = g; o[x * 4 + 2] = b; o[x * 4 + 3] = a;
        }
    }
    });
    auto cl = [](float v) { return std::clamp(int(v + 0.5f), 0, 255); };
    parallelRows(dh, big, [&](int lo, int hi) {
    std::vector<float> acc(size_t(dw) * 4);
    for (int y = lo; y < hi; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (const Tap& t : ty[y]) {
            const float* m = &mid[size_t(t.k) * dw * 4];
            for (int i = 0; i < dw * 4; ++i) acc[i] += m[i] * t.w;
        }
        uint32_t* out = &d.px[size_t(y) * d.w];
        for (int x = 0; x < dw; ++x)
            out[x] = P_(cl(acc[x * 4]), cl(acc[x * 4 + 1]), cl(acc[x * 4 + 2]), da ? cl(acc[x * 4 + 3]) : 255);
    }
    });
}

SurfacePtr smoothscale(const Surface& s, int w, int h) {
    auto out = Surface::make(std::max(1, w), std::max(1, h), s.hasAlpha);
    smoothscaleInto(s, s.rect(), *out);
    return out;
}

static SurfacePtr rotateImpl(const Surface& s, double degrees, double zoom, bool smooth) {
    double a = degrees * M_PI / 180.0;
    double ca = std::cos(a), sa = std::sin(a);
    double sw = s.w * zoom, sh = s.h * zoom;
    int ow = std::max(1, int(std::ceil(std::abs(sw * ca) + std::abs(sh * sa) - 1e-6)));
    int oh = std::max(1, int(std::ceil(std::abs(sw * sa) + std::abs(sh * ca) - 1e-6)));
    auto out = Surface::make(ow, oh, true);
    double ocx = ow / 2.0, ocy = oh / 2.0, icx = s.w / 2.0, icy = s.h / 2.0;
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            double dx = x + 0.5 - ocx, dy = y + 0.5 - ocy;
            // counter-clockwise rotation on screen (y points down): inverse map
            double sx = (dx * ca - dy * sa) / zoom + icx, sy = (dx * sa + dy * ca) / zoom + icy;
            if (!smooth) {
                int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                if (ix < 0 || iy < 0 || ix >= s.w || iy >= s.h) continue;
                uint32_t p = s.get(ix, iy);
                out->put(x, y, s.hasAlpha ? p : (p | 0xFF000000u));
                continue;
            }
            double fx = sx - 0.5, fy = sy - 0.5;
            int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            double tx = fx - x0, ty = fy - y0;
            if (x0 < -1 || y0 < -1 || x0 >= s.w || y0 >= s.h) continue;
            double acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                double wgt = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
                if (xx < 0 || yy < 0 || xx >= s.w || yy >= s.h) continue;   // outside counts as transparent
                uint32_t p = s.get(xx, yy);
                acc[0] += R_(p) * wgt; acc[1] += G_(p) * wgt; acc[2] += B_(p) * wgt;
                acc[3] += (s.hasAlpha ? A_(p) : 255) * wgt;
            }
            int al = int(acc[3] + 0.5);
            if (al <= 0) continue;
            out->put(x, y, P_(std::clamp(int(acc[0] + 0.5), 0, 255), std::clamp(int(acc[1] + 0.5), 0, 255),
                              std::clamp(int(acc[2] + 0.5), 0, 255), std::min(255, al)));
        }
    }
    return out;
}

SurfacePtr rotate(const Surface& s, double degrees) {
    double m = std::fmod(degrees, 360.0);
    if (m < 0) m += 360;
    if (std::abs(m) < 1e-9) { auto c = s.copy(); c->hasAlpha = true; c->surfAlpha = 255; return c; }
    return rotateImpl(s, degrees, 1.0, false);
}
SurfacePtr rotozoom(const Surface& s, double degrees, double zoom) { return rotateImpl(s, degrees, zoom, true); }

SurfacePtr flipX(const Surface& s) {
    auto out = Surface::make(s.w, s.h, s.hasAlpha);
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) out->put(s.w - 1 - x, y, s.get(x, y));
    return out;
}
}  // namespace xform

// ------------------------------------------------------------------ outlines and glows
void outlineSilhouette(Surface& s, int threshold, Color c, int width) {
    int W = s.w, H = s.h;
    // work only inside the bounding box of the silhouette (plus a one pixel border, which is always "outside")
    int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
    for (int y = 0; y < H; ++y) {
        const uint32_t* row = &s.px[size_t(y) * W];
        for (int x = 0; x < W; ++x)
            if (A_(row[x]) >= threshold) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                by1 = y;
            }
    }
    if (bx1 < 0) return;
    int ox = bx0 - 1, oy = by0 - 1, RW = bx1 - bx0 + 3, RH = by1 - by0 + 3;
    std::vector<uint8_t> m(size_t(RW) * RH, 0);              // 1 = solid, 2 = empty space reachable from outside
    for (int y = 1; y < RH - 1; ++y) {
        const uint32_t* row = &s.px[size_t(oy + y) * W + ox];
        uint8_t* mr = &m[size_t(y) * RW];
        for (int x = 1; x < RW - 1; ++x) mr[x] = A_(row[x]) >= threshold;
    }
    std::vector<int> stack;
    stack.reserve(size_t(RW + RH) * 4);
    auto seed = [&](int i) { if (m[i] == 0) { m[i] = 2; stack.push_back(i); } };
    for (int x = 0; x < RW; ++x) { seed(x); seed((RH - 1) * RW + x); }
    for (int y = 0; y < RH; ++y) { seed(y * RW); seed(y * RW + RW - 1); }
    while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        int x = i % RW;
        if (x > 0) seed(i - 1);
        if (x < RW - 1) seed(i + 1);
        if (i >= RW) seed(i - RW);
        if (i < (RH - 1) * RW) seed(i + RW);
    }
    uint32_t col = pack(c);
    double r = width / 2.0;
    int ri = int(std::ceil(r));
    std::vector<std::pair<int, int>> disk;
    for (int dy = -ri; dy <= ri; ++dy)
        for (int dx = -ri; dx <= ri; ++dx)
            if (dx * dx + dy * dy <= r * r + 0.25) disk.push_back({dx, dy});
    std::vector<int> edge;
    for (int y = 1; y < RH - 1; ++y)
        for (int x = 1; x < RW - 1; ++x) {
            int i = y * RW + x;
            if (m[i] == 1 && (m[i - 1] == 2 || m[i + 1] == 2 || m[i - RW] == 2 || m[i + RW] == 2)) edge.push_back(i);
        }
    for (int i : edge) {
        int x = ox + i % RW, y = oy + i / RW;
        for (auto [dx, dy] : disk) {
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && yy >= 0 && xx < W && yy < H) s.px[size_t(yy) * W + xx] = col;
        }
    }
}

int countOpaque(const Surface& s, int threshold) {
    int n = 0;
    for (uint32_t p : s.px) n += A_(p) >= threshold;
    return n;
}

void glow(Surface& s, Color c, Vec2 pos, double radius, int alpha) {
    int R = std::max(2, int(radius));
    alpha = std::clamp(alpha, 0, 255);
    if (alpha <= 0) return;
    int ox = int(pos.x - R), oy = int(pos.y - R);                 // the Python version's temporary surface
    double cx = ox + R + 0.5, cy = oy + R + 0.5;
    int y0 = std::max(oy, s.clipRect.y), y1 = std::min(oy + 2 * R, s.clipRect.bottom());
    int x0 = std::max(ox, s.clipRect.x), x1 = std::min(ox + 2 * R, s.clipRect.right());
    double rr[5];
    for (int i = 1; i <= 4; ++i) rr[i] = std::floor(R * i / 4.0);
    for (int y = y0; y < y1; ++y) {
        double dy = y + 0.5 - cy;
        for (int x = x0; x < x1; ++x) {
            double dx = x + 0.5 - cx, d2 = dx * dx + dy * dy;
            int ring = 0;
            for (int i = 1; i <= 4; ++i)
                if (d2 <= rr[i] * rr[i]) { ring = i; break; }
            if (!ring) continue;
            int a = alpha / ring;
            if (a <= 0) continue;
            s.put(x, y, blendPixel(s.get(x, y), c.r, c.g, c.b, a, Blend::Normal, s.hasAlpha));
        }
    }
}

// ------------------------------------------------------------------ text
struct GlyphCache {
    stbtt_fontinfo font;
    std::unordered_map<int, std::vector<unsigned char>> bitmaps;
};

Font::Font(const unsigned char* ttf, int pygameSize) {
    auto gc = std::make_shared<GlyphCache>();
    stbtt_InitFont(&gc->font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
    int px = std::max(1, int(pygameSize * 0.6875));               // pygame shrinks its default font like this
    scale = stbtt_ScaleForMappingEmToPixels(&gc->font, float(px));
    int a, d, g;
    stbtt_GetFontVMetrics(&gc->font, &a, &d, &g);
    ascent = int(std::ceil(a * scale));
    int descent = int(std::floor(d * scale));
    lineHeight = ascent - descent;
    info = gc;
}

int Font::width(const std::string& text) const {
    auto* gc = static_cast<GlyphCache*>(info.get());
    double x = 0;
    int prev = 0;
    for (unsigned char ch : text) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&gc->font, ch, &adv, &lsb);
        if (prev) x += stbtt_GetCodepointKernAdvance(&gc->font, prev, ch) * scale;
        x += adv * scale;
        prev = ch;
    }
    return int(std::ceil(x));
}

SurfacePtr Font::render(const std::string& text, Color c) const {
    auto* gc = static_cast<GlyphCache*>(info.get());
    int W = std::max(1, width(text)), H = std::max(1, lineHeight);
    auto out = Surface::make(W, H, true);
    std::vector<unsigned char> cov(size_t(W) * H, 0);
    double x = 0;
    int prev = 0;
    for (unsigned char ch : text) {
        if (prev) x += stbtt_GetCodepointKernAdvance(&gc->font, prev, ch) * scale;
        int x0, y0, x1, y1;
        float sub = float(x - std::floor(x));
        stbtt_GetCodepointBitmapBoxSubpixel(&gc->font, ch, scale, scale, sub, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> bmp(size_t(gw) * gh);
            stbtt_MakeCodepointBitmapSubpixel(&gc->font, bmp.data(), gw, gh, gw, scale, scale, sub, 0, ch);
            int bx = int(std::floor(x)) + x0, by = ascent + y0;
            for (int j = 0; j < gh; ++j)
                for (int i = 0; i < gw; ++i) {
                    int X = bx + i, Y = by + j;
                    if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                    unsigned char& d = cov[size_t(Y) * W + X];
                    d = std::max(d, bmp[size_t(j) * gw + i]);
                }
        }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&gc->font, ch, &adv, &lsb);
        x += adv * scale;
        prev = ch;
    }
    for (size_t i = 0; i < cov.size(); ++i)
        if (cov[i]) out->px[i] = P_(c.r, c.g, c.b, cov[i] * c.a / 255);
    return out;
}

// ------------------------------------------------------------------ images
static SurfacePtr fromRGBA(const unsigned char* data, int w, int h) {
    auto s = Surface::make(w, h, true);
    for (int i = 0; i < w * h; ++i) s->px[i] = P_(data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]);
    return s;
}

SurfacePtr loadPNG(const unsigned char* data, size_t len) {
    int w, h, n;
    unsigned char* rgba = stbi_load_from_memory(data, int(len), &w, &h, &n, 4);
    if (!rgba) return nullptr;
    auto s = fromRGBA(rgba, w, h);
    stbi_image_free(rgba);
    return s;
}

SurfacePtr loadPNGFile(const std::string& path) {
    int w, h, n;
    unsigned char* rgba = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!rgba) return nullptr;
    auto s = fromRGBA(rgba, w, h);
    stbi_image_free(rgba);
    return s;
}

static std::vector<unsigned char> toRGBA(const Surface& s) {
    std::vector<unsigned char> out(size_t(s.w) * s.h * 4);
    for (size_t i = 0; i < s.px.size(); ++i) {
        uint32_t p = s.px[i];
        out[i * 4] = R_(p); out[i * 4 + 1] = G_(p); out[i * 4 + 2] = B_(p); out[i * 4 + 3] = s.hasAlpha ? A_(p) : 255;
    }
    return out;
}

bool savePNG(const Surface& s, const std::string& path) {
    auto rgba = toRGBA(s);
    return stbi_write_png(path.c_str(), s.w, s.h, 4, rgba.data(), s.w * 4) != 0;
}

std::vector<unsigned char> encodePNG(const Surface& s) {
    auto rgba = toRGBA(s);
    int len = 0;
    unsigned char* png = stbi_write_png_to_mem(rgba.data(), s.w * 4, s.w, s.h, 4, &len);
    std::vector<unsigned char> out(png, png + len);
    STBIW_FREE(png);
    return out;
}
