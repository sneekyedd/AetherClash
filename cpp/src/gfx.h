// A small software drawing library that reproduces the parts of pygame the game uses:
// surfaces with or without per-pixel alpha, shapes, alpha blending, blend modes, scaling,
// rotation, silhouette outlines and anti-aliased text. Everything is drawn on the CPU into
// plain pixel buffers; the window only ever shows the finished 1280x720 frame.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    Color() = default;
    constexpr Color(int r_, int g_, int b_, int a_ = 255)
        : r((uint8_t)r_), g((uint8_t)g_), b((uint8_t)b_), a((uint8_t)a_) {}
    Color withAlpha(int a_) const { return Color(r, g, b, std::clamp(a_, 0, 255)); }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double k) const { return {x * k, y * k}; }
};

// pygame.Rect: integer x, y, width, height
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
    int left() const { return x; }
    int top() const { return y; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    int centerx() const { return x + w / 2; }
    int centery() const { return y + h / 2; }
    Vec2 center() const { return {double(centerx()), double(centery())}; }
    void setCenter(double cx, double cy) { x = int(cx) - w / 2; y = int(cy) - h / 2; }
    Rect clip(const Rect& o) const;
    bool colliderect(const Rect& o) const;
    bool collidepoint(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect inflate(int dw, int dh) const { return Rect(x - dw / 2, y - dh / 2, w + dw, h + dh); }
    Rect move(int dx, int dy) const { return Rect(x + dx, y + dy, w, h); }
    void clampInside(const Rect& o);           // pygame clamp_ip
    bool empty() const { return w <= 0 || h <= 0; }
};

enum class Blend { Normal, RGBA_MULT, RGB_MULT, RGB_ADD, RGBA_ADD, RGBA_MIN };

class Surface;
using SurfacePtr = std::shared_ptr<Surface>;

class Surface {
public:
    int w = 0, h = 0;
    bool hasAlpha = false;          // pygame.SRCALPHA: each pixel has its own alpha
    int surfAlpha = 255;            // set_alpha(): extra transparency applied when blitted
    std::vector<uint32_t> px;       // 0xAARRGGBB
    Rect clipRect;

    Surface() = default;
    Surface(int w_, int h_, bool alpha = false);
    static SurfacePtr make(int w, int h, bool alpha = false) { return std::make_shared<Surface>(w, h, alpha); }
    SurfacePtr copy() const { return std::make_shared<Surface>(*this); }
    Rect rect() const { return Rect(0, 0, w, h); }

    inline uint32_t get(int x, int y) const { return px[size_t(y) * w + x]; }
    inline void put(int x, int y, uint32_t c) { px[size_t(y) * w + x] = c; }
    void setAt(int x, int y, Color c);
    Color getAt(int x, int y) const;
    void setClip(const Rect& r) { clipRect = r.clip(rect()); }
    void resetClip() { clipRect = rect(); }

    void fill(Color c);                                      // whole surface (respects the clip)
    void fill(Color c, const Rect& r);
    void fill(Color c, Blend mode);                          // fill with a blend mode (MULT, ADD...)
    // blit another surface onto this one at (x, y); area = the part of src to copy
    void blit(const Surface& src, double x, double y, Blend mode = Blend::Normal);
    void blit(const Surface& src, double x, double y, const Rect& area, Blend mode = Blend::Normal);
    void setAlpha(int a) { surfAlpha = std::clamp(a, 0, 255); }

    // span helpers used by the shape drawing
    void hspan(int y, int x0, int x1, uint32_t c);           // inclusive x range, clipped, direct write
};

uint32_t pack(Color c);
Color unpack(uint32_t p);

// ---- shapes (like pygame.draw: they write colours directly, no blending) ----
namespace draw {
void rect(Surface& s, Color c, const Rect& r, int width = 0, int radius = 0);
void circle(Surface& s, Color c, Vec2 center, double radius, int width = 0);
void ellipse(Surface& s, Color c, const Rect& r, int width = 0);
void polygon(Surface& s, Color c, const std::vector<Vec2>& pts, int width = 0);
void line(Surface& s, Color c, Vec2 a, Vec2 b, int width = 1);
void lines(Surface& s, Color c, bool closed, const std::vector<Vec2>& pts, int width = 1);
}

// ---- transforms ----
namespace xform {
SurfacePtr scale(const Surface& s, int w, int h);                  // nearest neighbour
SurfacePtr smoothscale(const Surface& s, int w, int h);            // box filter / bilinear
void smoothscaleInto(const Surface& s, const Rect& area, Surface& dst);   // area of s -> the whole of dst
void scaleInto(const Surface& s, Surface& dst);                    // nearest, whole -> whole
SurfacePtr rotate(const Surface& s, double degrees);               // counter-clockwise, nearest
SurfacePtr rotozoom(const Surface& s, double degrees, double zoom); // smooth
SurfacePtr flipX(const Surface& s);
}

// Paints an outline `width` pixels thick around everything in the surface whose alpha is at least `threshold`
// (what pygame.mask.from_surface(...).outline() + draw.lines does in the Python version).
void outlineSilhouette(Surface& s, int threshold, Color c, int width);
int countOpaque(const Surface& s, int threshold);

// A soft glow: four nested translucent discs, blended on (the Python version's glow()).
void glow(Surface& s, Color c, Vec2 pos, double radius, int alpha);

// ---- text ----
class Font {
public:
    Font(const unsigned char* ttf, int pygameSize);   // sized like pygame.font.Font(None, size)
    SurfacePtr render(const std::string& text, Color c) const;
    int width(const std::string& text) const;
    int height() const { return lineHeight; }
private:
    std::shared_ptr<void> info;
    float scale = 1;
    int ascent = 0, lineHeight = 0;
};

// ---- images ----
SurfacePtr loadPNG(const unsigned char* data, size_t len);
SurfacePtr loadPNGFile(const std::string& path);
bool savePNG(const Surface& s, const std::string& path);
std::vector<unsigned char> encodePNG(const Surface& s);
