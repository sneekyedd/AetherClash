// AETHER CLASH - game rules: data, physics, combat, controls and the CPU player.
// A straight port of the Python version (aether_clash.py); names follow it closely.
#pragma once
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "gfx.h"

// ===================================================================== constants & tuning
constexpr int SCREEN_WIDTH = 1280, SCREEN_HEIGHT = 720, FPS = 60;
constexpr int WORLD_W = 2400, WORLD_H = 1400;      // the arena is bigger than the screen; a camera follows the action
const Rect BLAST_ZONE(280, 120, 1840, 1210);       // leave this box and you lose a life

constexpr int MAX_LIVES = 3;
constexpr double KNOCKBACK_DIVISOR = 200.0;         // Final Force = Base Force * (1.0 + Cumulative Damage / 200.0)
// How a launch behaves, as in Smash Ultimate (see the Python version for the derivation)
constexpr double LAUNCH_SCALE = 0.72;
constexpr double KB_DECAY = 0.051 * 4.8;
constexpr double HITSTUN_PER_SPEED = 0.4 / (0.03 * 4.8);
constexpr double HITSTUN_GRAVITY = 1.0;
constexpr double TUMBLE_SPEED = 80 * 0.03 * 4.8;
constexpr double VERTICAL_LAUNCH_FALL = 1.8 * 4.8;

constexpr double AETHER_MAX = 100.0, AETHER_START = 25.0, AETHER_PARRY_REWARD = 25.0;
constexpr double AETHER_PER_DAMAGE_DEALT = 0.6, AETHER_PER_DAMAGE_TAKEN = 0.25, AETHER_PASSIVE = 0.02;

constexpr double SHIELD_MAX = 50.0;
constexpr int PARRY_WINDOW = 6, PARRY_STUN = 45, SHIELD_BREAK_STUN = 170;
constexpr double ARMOR_LIMIT = 18, IGNITE_MULT = 1.3;
constexpr int BUFFER_FRAMES = 6;

constexpr int GRAB_HOLD_FRAMES = 80, GRAB_MIN_HOLD = 12, POMMEL_COOLDOWN = 14;
constexpr double POMMEL_DAMAGE = 2.0;

inline const std::string SECRET_NAME = "crypticsh";
constexpr int DUEL_TARGET = 25;

const Color WHITE(255, 255, 255);
const Color PLAYER_TAGS[2] = {Color(255, 96, 96), Color(96, 170, 255)};

// movement copies Super Smash Bros. Ultimate (values in Smash units, converted with this scale)
constexpr double SMASH_UNIT = 4.8, FAST_FALL_MULT = 1.6;
constexpr int FULL_HOP_FRAMES = 4;
constexpr double FULL_HOP_FRACTION = 0.55;
constexpr int LIGHT_LANDING = 2, TURN_DASH_DELAY = 3;

constexpr double PI = 3.14159265358979323846, TAU = 2 * PI;

// ===================================================================== helpers
inline double approach(double v, double target, double step) {
    return v < target ? std::min(v + step, target) : std::max(v - step, target);
}
template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int sgn(double v) { return (v > 0) - (v < 0); }
Color lerpColor(Color a, Color b, double t);
inline double radians(double d) { return d * PI / 180.0; }
// Python-style modulo for floats
inline double pymod(double a, double b) { double m = std::fmod(a, b); return (m < 0) ? m + b : m; }

// random numbers (Python's random module equivalents)
struct Rng {
    std::mt19937_64 g;
    explicit Rng(uint64_t seed = 12345) : g(seed) {}
    void seed(uint64_t s) { g.seed(s); }
    double random() { return std::uniform_real_distribution<double>(0.0, 1.0)(g); }
    double uniform(double a, double b) { return a + (b - a) * random(); }
    int randint(int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); }   // inclusive
    double gauss(double mu, double sigma) { return std::normal_distribution<double>(mu, sigma)(g); }
    double expovariate(double lambda) { return std::exponential_distribution<double>(lambda)(g); }
    template <class T> const T& choice(const std::vector<T>& v) { return v[size_t(randint(0, int(v.size()) - 1))]; }
};
Rng& fxRng();          // visual randomness (particles etc.) - never affects the game itself
Rng& aiRng();          // the CPU players' decisions

// ===================================================================== data structures
struct Circle { double dx, dy, r; };
std::vector<Circle> rectToCircles(double w, double h);

struct Hitbox {
    int start = 0, end = 0;
    double x = 0, y = 0, w = 0, h = 0;     // centre relative to the fighter's centre (x is forward)
    std::vector<Circle> circles;
    double damage = 0, force = 0, angle = 0;
    int gid = 0;
    bool grab = false, radial = false, pull = false, spike = false, unblockable = false;
};
struct HbFlags { int gid = 0; bool grab = false, radial = false, pull = false, spike = false, unblockable = false; };
Hitbox hb(int start, int end, double x, double y, double w, double h, double damage, double force, double angle,
          HbFlags f = {});

struct Range { int a = 0, b = -1; bool on = false; bool has(int f) const { return on && a <= f && f <= b; } };
inline Range rng_(int a, int b) { return Range{a, b, true}; }

struct ProjectileSpec {
    bool on = false;
    int frame = 0;
    double x = 0, y = 0, w = 0, h = 0, vx = 0, vy = 0, gravity = 0;
    int life = 0;
    double damage = 0, force = 0, angle = 0;
    std::string kind;
    int rehit = 0;
    bool sturdy = false, ghost = false, grounded = false, pull = false;
};

struct Impulse { bool hasX = false, hasY = false; double x = 0, y = 0; };

struct Move {
    std::string name, type = "Attack", slot;
    int duration = 0;
    std::vector<Hitbox> hitboxes;
    double requiredAether = 0, damage = 0;
    std::map<int, Impulse> impulses;
    Range armor, counter, intangible, noGravity;
    struct { bool on = false; double speed = 0; int a = 0, b = 0; } aim;
    ProjectileSpec projectile;
    std::string effect;
    int effectFrame = -1;
    bool helpless = false;
    int landingLag = 6;
    int firstStart() const;   // first active frame of the (non-spike) hitboxes, or -1
};

enum class Style { Weaver, Sentinel, Voidcaller, Kindled };

struct Palette {
    Color main, dark, accent, skin;
    std::map<std::string, Color> extra;
    Color get(const std::string& k, Color fallback) const {
        auto it = extra.find(k);
        return it == extra.end() ? fallback : it->second;
    }
};

struct SmashStats {
    double walk, initialDash; int dashFrames; double run, runAccel, traction, airSpeed, airAccel, airFriction,
        gravity, fall, fullHop, shortHop, doubleJump; int heavyLanding; int jumpSquat = 3;
};
struct BaseStats { double gravity, weight; int w, h, airJumps; SmashStats smash; };

struct FighterSpec {
    std::string name, title, faction;
    Style style;
    std::map<std::string, Move> moves;
    std::vector<std::string> blurb;
    BaseStats stats;
    std::array<int, 4> bars;
    std::vector<std::string> aiMid, aiClose;
    Palette colors, alt;
};
extern std::vector<FighterSpec> ROSTER;
extern Move DEATH_RAY;
void buildRoster();

struct Platform {
    double baseX, baseY, x, y;
    int width, height;
    bool solid, ledges;
    bool moving = false;
    double ampX = 0, ampY = 0, period = 1, phase = 0;
    double dx = 0, dy = 0, prevTop;
    std::array<double, 8> jag;
    Platform(double x_, double y_, int w_, int h_, bool solid_ = true);
    Platform& motion(double ax, double ay, double per, double ph) { moving = true; ampX = ax; ampY = ay; period = per; phase = ph; return *this; }
    void update(int frame);
};

struct StagePalette { Color skyTop, skyBottom, rock, top, soft; };
struct Stage {
    std::string key, name;
    std::vector<std::string> blurb;
    std::vector<Platform> platforms;
    StagePalette palette;
    double left, right, top;
    Rect blast = BLAST_ZONE;
    std::array<std::array<double, 3>, 2> spawns;
    double respawnX, respawnY;
    SurfacePtr bg;
    void finish();
    void reset();
};
std::vector<Stage> buildStages();

struct Pose;
struct ClothSim;
struct VisTrack;
struct Fighter;

struct Ledge { bool on = false; Platform* p = nullptr; double lx = 0; int inward = 1; };

struct Fighter {
    std::string name, faction, playerName;
    const FighterSpec* spec;
    int slot = 0, id = 0;
    Palette colors;
    bool isCpu = false;
    int maxLives = MAX_LIVES;
    // movement stats (pixels / frame)
    double walkSpeed, initDash, speed, runAccel, traction, airSpeed, airAccel, airFriction, gravity, maxFall;
    int dashFrames, jumpSquat, heavyLanding;
    double fullHopH, hopBoostH, jumpHeight, afterBoost, shortHop, doubleJump, vscale, weight;
    int airJumps, w, h;
    const std::map<std::string, Move>* moveSet;
    // runtime state
    int currentLives = MAX_LIVES;
    double damage = 0;               // cumulative_damage_taken
    double aether = AETHER_START;
    int kos = 0;
    double x = 0, y = 0, vx = 0, vy = 0, kbx = 0, kby = 0;
    int facing = 1;
    Platform* ground = nullptr;
    std::string state = "IDLE";
    bool knockedDown = false;
    const Move* move = nullptr;
    int moveFrame = 0;
    std::set<std::pair<int, int>> hitLog;
    bool moveFromAir = false;
    int jumpsLeft = 1;
    int moveX = 0, inX = 0, inY = 0;
    double aimX = 0, aimY = -1, visAimX = 1, visAimY = 0;
    int hitstop = 0, hitstun = 0, stunTimer = 0, lag = 0;
    bool frozen = false;
    double shieldHp = SHIELD_MAX;
    int shieldFrame = 0, shieldStun = 0;
    bool shieldHeld = false;
    int intangible = 0, dodgeTimer = 0;
    bool airDodged = false, fastFall = false, jumpCut = false;
    int dropTimer = 0, ledgeCd = 0, ledgeTimer = 0;
    Ledge ledge;
    int stateTimer = 0, buffTimer = 0, respawnTimer = 0, runFrames = 0;
    int dashT = 0, dashDir = 1, squat = 0;
    bool jumpHeld = false;
    int turnT = 0, hopT = 0;
    bool kbVertical = false;
    int flash = 0;
    double anim = 0;
    long ticks = 0;
    Fighter* grabTarget = nullptr;
    Fighter* grabbedBy = nullptr;
    int grabTimer = 0, grabFrames = 0, pommelCd = 0, pommelAnim = 0;
    double squash = 0;
    // visual-only state, owned by the StateTranslator
    std::shared_ptr<Pose> pose;
    std::shared_ptr<ClothSim> cloth;
    std::shared_ptr<VisTrack> vis;

    Fighter(const FighterSpec& spec, int slot, const Palette& colors);
    void reset(double x, double y, int facing);
    void respawnAt(double x, double y, int facing, bool fresh = false);
    bool isCryptic() const;
    Rect rect() const { return Rect(int(x - w / 2.0), int(y - h), w, h); }
    Vec2 center() const { return {x, y - h / 2.0}; }
    bool alive() const { return currentLives > 0 && state != "KO"; }
    Style style() const { return spec->style; }
};

struct Projectile {
    Fighter* owner;
    int facing;
    double x, y, w, h, vx, vy, gravity;
    int life;
    std::string kind;
    int rehit;
    bool sturdy, ghost, grounded;
    Hitbox hit;
    Color color;
    std::map<int, int> hitLog;
    int age = 0;
    bool dead = false;
    Projectile(Fighter* owner, const ProjectileSpec& s, double x, double y, int facing);
    Rect rect() const { return Rect(int(x - w / 2), int(y - h / 2), int(w), int(h)); }
};

// ===================================================================== effects
enum class PShape { Circle, Square, Line, Star };
struct Particle { double x, y, vx, vy; int life, maxLife; Color color; int size; double grav; PShape shape; };
struct Ring { double x, y, r, grow; int life, maxLife; Color color; };
struct PopText { double x, y; std::string s; Color color; int life; };

struct Effects {
    std::vector<Particle> particles;
    std::vector<Ring> rings;
    std::vector<PopText> texts;
    int shake = 0;
    void spark(double x, double y, Color c, int n = 10, double speed = 6.0, int life = 18, int size = 4, double grav = 0.0,
               std::optional<double> direction = std::nullopt, double spread = PI, PShape shape = PShape::Circle);
    void styleBurst(Style style, double x, double y, const Palette& colors, int n = 6, double power = 5.0);
    void ring(double x, double y, Color c, double r = 16, double grow = 6, int life = 14);
    void text(double x, double y, const std::string& s, Color c, int life = 45);
    void update();
    void draw(Surface& surf, const Font& font) const;
};

// ===================================================================== physics & combat
struct CombatManager;

struct PhysicsEngine {
    Stage* map;
    CombatManager* combat = nullptr;
    Effects* fx;
    std::vector<Fighter*> fighters;
    std::vector<std::unique_ptr<Projectile>> projectiles;
    int frame = 0;
    bool gameOver = false;

    PhysicsEngine(Stage* m, CombatManager* c, Effects* f) : map(m), combat(c), fx(f) {}
    void updateAll();
    void applyMoveEffects(Fighter& c);
    void applyGravity(Fighter& c);
    void applyMovement(Fighter& c);
    void groundMove(Fighter& c);
    void launchJump(Fighter& c);
    void checkPlatformCollision(Fighter& c);
    void land(Fighter& c, Platform& p);
    void syncGroundState(Fighter& c);
    void checkLedges(Fighter& c);
    void updateProjectiles();
    static std::vector<std::array<double, 3>> hitboxCircles(const Fighter& c, const Hitbox& box);
    Rect hitboxRect(const Fighter& c, const Hitbox& box) const;
    std::vector<const Hitbox*> activeHitboxes(const Fighter& c) const;
    void updateActiveHitboxes();
    double applyKnockback(Fighter& attacker, Fighter& target, double baseForce, double dx, double dy);
};

struct CombatManager {
    PhysicsEngine* physics = nullptr;
    Effects* fx;
    Fighter* pendingDuel = nullptr;
    Fighter* nukeBy = nullptr;

    explicit CombatManager(Effects* f) : fx(f) {}
    bool processAttack(Fighter& a, const Move& m);
    bool processAttack(Fighter& a, const std::string& slot) { return processAttack(a, a.moveSet->at(slot)); }
    void endMove(Fighter& c);
    std::string handleCollision(Fighter& attacker, const Hitbox& hb, Fighter& target, Projectile* source = nullptr);
    std::string startGrab(Fighter& a, Fighter& t);
    void pommel(Fighter& a);
    Fighter* releaseGrab(Fighter& a, bool push = true);
    void throwHeld(Fighter& a, int dirX, int dirY);
    void requestDeathRay(Fighter& c);
    void fireDeathRay(Fighter& c);
    void enterHitState(Fighter& t, double speed);
    void shieldBreak(Fighter& t);
    std::string handleParry(Fighter& defender, Fighter& attacker, const Hitbox& hb, Projectile* source, bool counter = false);
    void handleKo(Fighter& c);
    void checkAllMoveDurations();
};

// ===================================================================== controls
enum Button { B_JUMP = 1, B_ATTACK = 2, B_HEAVY = 4, B_SPECIAL = 8, B_SHIELD = 16, B_GRAB = 32 };
struct InputState {
    int x = 0, y = 0;
    uint8_t pressed = 0, held = 0;
    bool downTap = false;
    bool has(uint8_t set, Button b) const { return set & b; }
};
extern const std::vector<std::pair<std::string, Button>> BUTTONS;          // name -> bit
extern const std::vector<std::pair<std::string, std::string>> ACTIONS;     // (action, label) for key binding
using KeyMap = std::map<std::string, std::vector<int>>;                    // action -> key codes (SDL keycodes)
KeyMap defaultKeymap(int slot);

struct InputDevice {
    virtual ~InputDevice() = default;
    virtual InputState poll() = 0;
};

struct CharacterController {
    Fighter* c;
    CombatManager* combat;
    std::unique_ptr<InputDevice> device;
    std::map<uint8_t, int> buffer;
    int prevX = 0;
    CharacterController(Fighter* f, CombatManager* cm, std::unique_ptr<InputDevice> dev = nullptr)
        : c(f), combat(cm), device(std::move(dev)) {}
    virtual ~CharacterController() = default;
    virtual InputState readInput() { return device ? device->poll() : InputState(); }
    bool take(Button b);
    void processInput(const InputState& inp);
    void jump();
    void dodge(int x, int y, bool air = false);
    void shieldOptions(const InputState& inp, int prevX);
    void ledgeOptions(const InputState& inp);
    void updateState();
    void tick() { processInput(readInput()); updateState(); }
    virtual bool isAI() const { return false; }
};

struct AIController : CharacterController {
    PhysicsEngine* physics;
    int level;
    int cool = 40, shieldHold = 0, jumpHold = 0, wait = 0, pommels = 0;
    struct Q { int x, y; uint8_t button; };
    std::deque<Q> queue;
    AIController(Fighter* f, CombatManager* cm, PhysicsEngine* ph, int lvl)
        : CharacterController(f, cm), physics(ph), level(lvl) {}
    void press(InputState& inp, const std::string& slot, int toward);
    InputState readInput() override;
    bool isAI() const override { return true; }
};
