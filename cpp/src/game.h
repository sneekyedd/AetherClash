// AETHER CLASH - the Game Manager: window, scenes, menus, HUD, online lockstep and the Death Ray ending.
#pragma once
#include <SDL.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "art.h"
#include "net.h"

class Game;

// Keyboard (and optional game controller) input for one player.
struct KeyboardDevice : InputDevice {
    KeyMap keymap;
    Game* game;
    SDL_GameController* pad;
    uint8_t prevPad = 0;
    bool prevDown = false;
    KeyboardDevice(KeyMap km, Game* g, SDL_GameController* p) : keymap(std::move(km)), game(g), pad(p) {}
    InputState poll() override;
};

struct Profile { std::string name; bool hasKeys = false; KeyMap keys; };
struct ProfileStore {
    std::vector<std::string> paths;
    std::vector<Profile> profiles;
    explicit ProfileStore(const std::string& path = "");
    bool save();
};

// The cinematic that plays when the Death Ray is fired.
class Annihilation {
public:
    static constexpr int T_BLAST = 46, T_SPACE = 104, T_CRACK = 196, T_BOOM = 420, T_TEXT = 520, T_END = 1000;
    static constexpr int ER = 256;
    Annihilation(Game* g, Fighter* shooter);
    void step(bool fast = false);
    void skip();
    void render(Surface& scr, bool text = true);
    int t = 0;
    bool done = false;
private:
    Game* g;
    Fighter* shooter;
    int W = SCREEN_WIDTH, H = SCREEN_HEIGHT;
    Rng rng{4077};
    std::string name;
    Vec2 P;
    double rot = 3.5;
    SurfacePtr space, blastBg, earthImg, discWhite, glowLayer, coreLayer, comp, title, sub, ov, tint;
    struct Twinkle { double x, y, ph; };
    std::vector<Twinkle> twinkle;
    struct Rock { SurfacePtr img; double a, d0, v, acc, rot, spin, s0, grow; int delay; };
    std::vector<Rock> rocks;
    // the planet texture and per-pixel sphere data
    int TW = 512, TH = 256;
    std::vector<float> tex, lights;                     // TH*TW*3, TH*TW
    std::vector<int> pix, iy;                           // which disc pixels are inside, and their texture rows
    std::vector<float> lon, shade, night, atmo;
    std::vector<uint8_t> alpha;
    struct Seg { double birth; Vec2 p, q; bool big; };
    std::vector<Seg> segs;
    std::vector<std::vector<Vec2>> cells;
    std::vector<double> craterJ;
    size_t segI = 0;
    double viewX, viewY, viewR;
    struct Frag { SurfacePtr img; double x, y, vx, vy, ang, spin; };
    std::vector<Frag> fragments;
    bool boomed = false;
    struct Part { double x, y, vx, vy; int life, age; Color col; int size; };
    std::vector<Part> particles;
    double boomX = 0, boomY = 0, boomK = 1;
    int titleShake = 0;

    void buildSpace();
    void buildBlast();
    SurfacePtr makeRock(int size);
    void buildEarth();
    SurfacePtr renderEarth();
    void buildCracks();
    void drawCrack(const Seg& s);
    SurfacePtr composite();
    void earthView();
    void boom();
    void updateDebris();
    void renderBlast(Surface& scr);
    void blitPlanet(Surface& scr, const Surface& img, double cx, double cy, double rad);
    void renderSpace(Surface& scr);
    void buildTitle();
    void renderText(Surface& scr);
};

enum class Scene { TITLE, UPDATE, MENU, PROFILES, ONLINE, FIGHT, RESULT, CINEMATIC };

class Game {
public:
    explicit Game(bool headless = false, bool demo = false, const std::string& profilePath = "", bool checkUpdates = true);
    long maxLoops = 0;                       // (testing) close by itself after this many frames
    ~Game();
    void run();
    void step();
    void renderFrame();
    void setupGame(int p1, int p2, int stage, int p2Mode, bool p1Cpu, const std::vector<std::string>* names = nullptr,
                   const std::pair<int, uint64_t>* online = nullptr);
    void selftest(int frames = 3600);
    int nettest(const std::string& address, bool host, int frames);   // two copies play each other online

    // helpers shared with the cinematic and the input devices
    const Font& font(int size);
    Rect text(Surface& surf, const std::string& s, int size, Color color, Vec2 pos, const std::string& anchor = "center",
              bool shadow = true);
    bool keyHeld(int key) const;
    std::set<int> keysPressed;
    Effects fx;

    Scene scene = Scene::TITLE;
    bool running = true;
    Fighter* winner = nullptr;

private:
    bool headless;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    bool fullscreen = false;
    Json settings;
    Surface screen{SCREEN_WIDTH, SCREEN_HEIGHT, false};
    Surface world{WORLD_W, WORLD_H, false};
    Surface lowres{SCREEN_WIDTH / PIXEL, SCREEN_HEIGHT / PIXEL, false};
    std::vector<unsigned char> fontData;
    std::map<int, std::unique_ptr<Font>> fonts;
    StateTranslator translator;
    std::vector<Stage> stages;
    std::vector<SurfacePtr> thumbs;
    SurfacePtr menuBg, titleBg, logo, uiGround;
    struct TitleStar { int x, y, r; Color col; double ph; };
    std::vector<TitleStar> titleStars;
    int titleGroundY = SCREEN_HEIGHT - 110;
    std::vector<SDL_GameController*> pads;

    std::unique_ptr<CombatManager> combat;
    std::unique_ptr<PhysicsEngine> physics;
    Stage* mapData = nullptr;
    std::vector<std::unique_ptr<Fighter>> fighters;               // slot 0 and 1
    std::vector<std::unique_ptr<CharacterController>> controllers;

    int titleSel = 0;
    int sel[2] = {0, 1};
    int stageSel = 0, p2Mode = 1;
    bool p1Cpu = false;
    std::unique_ptr<ProfileStore> profiles;
    int profileSel[2] = {0, 0};
    int profCursor = 0;
    std::string profMode, profText;
    int profBindI = 0;
    int profEdit = -1;
    KeyMap profNewKeys;
    bool textAllowed = false;

    bool paused = false, showBoxes = false;
    long frame = 0;
    int intro = 0, endTimer = 0;
    struct Duel {
        int counts[2] = {0, 0};
        std::string prompts[2];
        int prev[2][2] = {{0, 0}, {0, 0}};
        int miss[2] = {0, 0}, cpu[2] = {40, 40}, intro = 70;
    };
    std::unique_ptr<Duel> duel;
    std::unique_ptr<Annihilation> cine;
    Rng rng{std::random_device{}()};

    OnlineClient net;
    std::unique_ptr<NetMatch> netmatch;
    std::unique_ptr<InputDevice> localDevice;
    bool keepKeys = false;
    struct Online {
        std::string state = "address", text, code, opp, note;
        std::vector<Json> rooms;
        int sel = 0, slot = 0, stage = 0;
        uint64_t seed = 0;
        int chars[2] = {0, 1};
        bool ready[2] = {false, false};
    } on;
    Updater* updater = nullptr;

    Rect cam{0, 0, WORLD_W, WORLD_W * 9 / 16};
    double camf[3] = {600.0, 300.0, 1200.0};
    int slowFrames = 0;

    // settings / display
    std::vector<std::string> settingsPaths(const char* file) const;
    void loadSettings();
    void saveSettings();
    void setDisplay();
    void present();
    void toggleFullscreen();
    // profiles
    const Profile* slotProfile(int slot) const;
    KeyMap slotKeymap(int slot) const;
    std::string slotName(int slot, bool cpu = false) const;
    // loop
    void handleEvents();
    bool inTextMode() const;
    void onKey(int k, const std::string& text);
    void profilesKey(int k, const std::string& text, bool up, bool down, bool enter);
    void startAnnihilation(Fighter* shooter);
    std::string newPrompt(const std::string& last = "");
    void startDuel();
    void stepDuel();
    void ambient();
    void updateCamera();
    // drawing
    void panel(const Rect& r, std::optional<Color> border = std::nullopt, int alpha = 190);
    std::string keyNames(const std::vector<int>& keys) const;
    void menuBackdrop();
    void drawWorld(const Rect& view);
    Rect renderWorldView();
    void renderHud(const Rect& view);
    void renderDuel(const std::map<int, Vec2>& heads);
    void renderControls(int y);
    void titleBackdrop();
    void renderUpdate();
    void renderTitle();
    void renderOnline();
    void renderProfiles();
    void renderMenu();
    void renderResult();
    // online
    std::string loadAddress();
    void saveAddress(const std::string& text);
    std::string onlineName() const;
    void onlineConnect();
    void onlineDisconnect(const std::string& note = "");
    void onlineBackToRoom(const std::string& note);
    void sendPick();
    void onlineKey(int k, const std::string& text, bool up, bool down, bool enter);
    void onlineUpdate();
    void onlineStart(int c0, int c1, int stage);
    bool netGate();
    Fighter* fighter(int slot) { return slot < int(fighters.size()) ? fighters[size_t(slot)].get() : nullptr; }
    std::vector<Fighter*> fighterList() const;
};

double easeOut(double u);
double easeIn(double u);
std::string appDir();
