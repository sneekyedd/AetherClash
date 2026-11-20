// The Game Manager: window and loop, settings, profiles, setup, the match, camera, duel and online lockstep.
// (Menus, HUD and other screens are drawn in screens.cpp.)
#include "game.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "assets.h"

std::string appDir() {
    std::string exe = Updater::exePath();
    size_t s = exe.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : exe.substr(0, s);
}

static std::string homeDir() {
    const char* h = std::getenv("HOME");
    if (!h || !*h) h = std::getenv("USERPROFILE");
    return h ? h : ".";
}

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a.back();
    return (last == '/' || last == '\\') ? a + b : a + "/" + b;
}

static bool readJsonFile(const std::string& path, Json& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    Json j = Json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) return false;
    out = std::move(j);
    return true;
}

static bool writeJsonFile(const std::string& path, const Json& j, int indent = -1) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(indent);
    return bool(f);
}

static int jint(const Json& m, const char* key, int def = 0) {
    auto it = m.find(key);
    if (it == m.end()) return def;
    if (it->is_number()) return it->get<int>();
    if (it->is_string()) return std::atoi(it->get<std::string>().c_str());
    if (it->is_boolean()) return it->get<bool>();
    return def;
}
static std::string jstr(const Json& m, const char* key, const std::string& def = "") {
    auto it = m.find(key);
    if (it == m.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

// ===================================================================== input
InputState KeyboardDevice::poll() {
    InputState inp;
    auto down = [&](const std::string& name) {
        auto it = keymap.find(name);
        if (it == keymap.end()) return false;
        for (int k : it->second) if (game->keyHeld(k)) return true;
        return false;
    };
    inp.x = int(down("right")) - int(down("left"));
    inp.y = int(down("down")) - int(down("up"));
    for (auto& [name, bit] : BUTTONS) {
        if (down(name)) inp.held |= bit;
        auto it = keymap.find(name);
        if (it != keymap.end())
            for (int k : it->second) if (game->keysPressed.count(k)) inp.pressed |= bit;
    }
    if (pad) {
        if (!SDL_GameControllerGetAttached(pad)) {
            pad = nullptr;
        } else {
            double ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0;
            double ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0;
            int hx = int(SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) -
                     int(SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
            int hy = int(SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) -
                     int(SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP));
            if (!inp.x) inp.x = std::abs(ax) > 0.4 ? sgn(ax) : hx;
            if (!inp.y) inp.y = std::abs(ay) > 0.5 ? sgn(ay) : hy;
            const std::pair<SDL_GameControllerButton, Button> map[] = {
                {SDL_CONTROLLER_BUTTON_A, B_JUMP}, {SDL_CONTROLLER_BUTTON_B, B_SPECIAL}, {SDL_CONTROLLER_BUTTON_X, B_ATTACK},
                {SDL_CONTROLLER_BUTTON_Y, B_HEAVY}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_SHIELD},
                {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_GRAB}};
            uint8_t now = 0;
            for (auto& [b, bit] : map)
                if (SDL_GameControllerGetButton(pad, b)) now |= bit;
            if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) now |= B_SHIELD;
            if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) now |= B_GRAB;
            inp.held |= now;
            inp.pressed |= now & ~prevPad;
            prevPad = now;
        }
    }
    inp.downTap = inp.y == 1 && !prevDown;
    prevDown = inp.y == 1;
    return inp;
}

// ===================================================================== profiles
ProfileStore::ProfileStore(const std::string& path) {
    if (!path.empty()) paths = {path};
    else paths = {joinPath(appDir(), "aether_clash_profiles.json"), joinPath(homeDir(), ".aether_clash_profiles.json")};
    for (auto& p : paths) {
        Json data;
        if (!readJsonFile(p, data) || !data.is_object()) continue;
        try {
            std::vector<Profile> out;
            for (auto& d : data.at("profiles")) {
                Profile pr;
                pr.name = d.at("name").is_string() ? d.at("name").get<std::string>() : d.at("name").dump();
                if (pr.name.size() > 12) pr.name.resize(12);
                if (d.contains("keys") && d["keys"].is_object() && !d["keys"].empty()) {
                    pr.hasKeys = true;
                    for (auto& [a, v] : d["keys"].items()) {
                        std::vector<int> ks;
                        for (auto& k : v) ks.push_back(k.get<int>());
                        pr.keys[a] = ks;
                    }
                }
                out.push_back(pr);
            }
            profiles = out;
            break;
        } catch (...) {
            continue;
        }
    }
}

bool ProfileStore::save() {
    Json arr = Json::array();
    for (auto& p : profiles) {
        Json keys = nullptr;
        if (p.hasKeys) {
            keys = Json::object();
            for (auto& [a, v] : p.keys) keys[a] = v;
        }
        arr.push_back({{"name", p.name}, {"keys", keys}});
    }
    Json doc = {{"profiles", arr}};
    for (auto& p : paths)
        if (writeJsonFile(p, doc, 1)) return true;
    return false;
}

// ===================================================================== setup
Game::Game(bool headless_, bool demo, const std::string& profilePath, bool checkUpdates) : headless(headless_) {
    buildRoster();
    fontData.assign(ASSET_FONT, ASSET_FONT + ASSET_FONT_LEN);
    loadSettings();
    fullscreen = settings.is_object() && settings.value("fullscreen", false) && !headless;
    if (!headless) {
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
        setDisplay();
        auto icon = makeIcon(64);
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(icon->px.data(), icon->w, icon->h, 32, icon->w * 4, SDL_PIXELFORMAT_ARGB8888)) {
            SDL_SetWindowIcon(window, s);
            SDL_FreeSurface(s);
        }
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_IsGameController(i))
                if (auto* p = SDL_GameControllerOpen(i)) pads.push_back(p);
        SDL_StartTextInput();
    }
    stages = buildStages();
    for (auto& st : stages) st.bg = makeBackground(st);
    logo = makeIcon(150);
    uiGround = loadPNG(ASSET_GROUND, ASSET_GROUND_LEN);
    mapData = &stages[0];
    scene = demo ? Scene::MENU : Scene::TITLE;
    if (demo) { p2Mode = 2; p1Cpu = true; }
    profiles = std::make_unique<ProfileStore>(profilePath);
    on.text = loadAddress();
    updater = new Updater(checkUpdates && !headless ? std::vector<std::string>{on.text, defaultServer()}
                                                    : std::vector<std::string>{}, checkUpdates && !headless);   // lives for the whole run
    if (updater->enabled && !demo) scene = Scene::UPDATE;  // opening the game always checks for an update first
}

Game::~Game() {
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (!headless) SDL_Quit();
}

std::vector<std::string> Game::settingsPaths(const char* file) const {
    return {joinPath(appDir(), file), joinPath(homeDir(), std::string(".") + file)};
}

void Game::loadSettings() {
    settings = Json::object();
    for (auto& p : settingsPaths("aether_clash_settings.json")) {
        Json d;
        if (readJsonFile(p, d) && d.is_object()) { settings = d; return; }
    }
}

void Game::saveSettings() {
    for (auto& p : settingsPaths("aether_clash_settings.json"))
        if (writeJsonFile(p, settings)) return;
}

void Game::setDisplay() {
    if (!window) {
        window = SDL_CreateWindow("AETHER CLASH", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_WIDTH, SCREEN_HEIGHT,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT);   // keeps the 16:9 shape (black bars)
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
    }
    SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
}

void Game::present() {
    if (headless || !renderer) return;
    SDL_UpdateTexture(texture, nullptr, screen.px.data(), SCREEN_WIDTH * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

void Game::toggleFullscreen() {
    fullscreen = !fullscreen;
    if (!headless) setDisplay();
    settings["fullscreen"] = fullscreen;
    saveSettings();
}

const Profile* Game::slotProfile(int slot) const {
    int i = profileSel[slot] - 1;
    return (i >= 0 && i < int(profiles->profiles.size())) ? &profiles->profiles[size_t(i)] : nullptr;
}

KeyMap Game::slotKeymap(int slot) const {
    KeyMap km = defaultKeymap(slot);
    if (const Profile* p = slotProfile(slot); p && p->hasKeys)
        for (auto& [a, v] : p->keys) km[a] = v;
    return km;
}

std::string Game::slotName(int slot, bool cpu) const {
    if (const Profile* p = slotProfile(slot)) return p->name;
    return cpu ? "CPU" : "P" + std::to_string(slot + 1);
}

std::vector<Fighter*> Game::fighterList() const {
    std::vector<Fighter*> v;
    for (auto& f : fighters) v.push_back(f.get());
    return v;
}

bool Game::keyHeld(int key) const {
    if (headless) return false;
    int n = 0;
    const Uint8* st = SDL_GetKeyboardState(&n);
    SDL_Scancode sc = SDL_GetScancodeFromKey(key);
    return sc > 0 && sc < n && st[sc];
}

void Game::setupGame(int p1, int p2, int stage, int p2Mode_, bool p1Cpu_, const std::vector<std::string>* names,
                     const std::pair<int, uint64_t>* online) {
    mapData = &stages[size_t(stage)];
    mapData->reset();
    fx = Effects();
    controllers.clear();
    fighters.clear();
    combat = std::make_unique<CombatManager>(&fx);
    physics = std::make_unique<PhysicsEngine>(mapData, combat.get(), &fx);
    combat->physics = physics.get();
    bool human2 = p2Mode_ == 3;
    std::vector<SDL_GameController*> freePads = pads;
    SDL_GameController* pad2 = nullptr;
    SDL_GameController* pad1 = nullptr;
    if (human2 && !freePads.empty()) { pad2 = freePads.front(); freePads.erase(freePads.begin()); }
    if (!freePads.empty()) { pad1 = freePads.front(); freePads.erase(freePads.begin()); }
    int idx[2] = {p1, p2};
    for (int slot = 0; slot < 2; ++slot) {
        const FighterSpec& spec = ROSTER[size_t(idx[slot])];
        const Palette& colors = (slot == 1 && p1 == p2) ? spec.alt : spec.colors;
        auto ch = std::make_unique<Fighter>(spec, slot, colors);
        auto& sp = mapData->spawns[size_t(slot)];
        ch->reset(sp[0], sp[1], int(sp[2]));
        bool cpu = online ? false : (slot == 0 ? p1Cpu_ : !human2);
        ch->isCpu = cpu;
        ch->playerName = names ? (*names)[size_t(slot)] : slotName(slot, cpu);
        physics->fighters.push_back(ch.get());
        std::unique_ptr<CharacterController> ctrl;
        if (online) {
            ctrl = std::make_unique<CharacterController>(ch.get(), combat.get(), std::make_unique<QueuedDevice>(slotKeymap(0)));
        } else if (cpu) {
            int level = slot == 0 ? 3 : p2Mode_ + 1;
            ctrl = std::make_unique<AIController>(ch.get(), combat.get(), physics.get(), std::min(3, level));
        } else {
            ctrl = std::make_unique<CharacterController>(
                ch.get(), combat.get(), std::make_unique<KeyboardDevice>(slotKeymap(slot), this, slot == 0 ? pad1 : pad2));
        }
        fighters.push_back(std::move(ch));
        controllers.push_back(std::move(ctrl));
    }
    scene = Scene::FIGHT;
    paused = false;
    intro = 100;
    endTimer = 0;
    winner = nullptr;
    frame = 0;
    duel.reset();
    cine.reset();
    netmatch = online ? std::make_unique<NetMatch>(online->first) : nullptr;
    rng = online ? Rng(online->second) : Rng(std::random_device{}());
    localDevice = online ? std::make_unique<KeyboardDevice>(slotKeymap(0), this, pads.empty() ? nullptr : pads.front()) : nullptr;
    camf[0] = 600.0;
    camf[1] = 300.0;
    camf[2] = 1200.0;
}

// ===================================================================== the loop
void Game::run() {
    using clock = std::chrono::steady_clock;
    const double step_ = 1.0 / FPS;
    double acc = 0.0;
    auto last = clock::now();
    long loops = 0;
    while (running) {
        if (maxLoops && ++loops > maxLoops) break;
        auto now = clock::now();
        double dt = std::min(std::chrono::duration<double>(now - last).count(), 0.1);
        last = now;
        acc += dt;
        if (scene == Scene::UPDATE) {
            if (updater->state == Updater::READY && updater->apply()) {
                renderFrame();
                present();
                break;                                    // the new version is starting; this one closes
            }
            int st = updater->state;
            if (st == Updater::DONE || st == Updater::OFF) scene = Scene::TITLE;
        }
        handleEvents();
        int steps = 0;
        while (acc >= step_ * 0.99 && steps < 3) {
            step();
            acc -= step_;
            ++steps;
            if (!keepKeys) keysPressed.clear();
            keepKeys = false;
        }
        if (acc > step_ * 3) acc = 0;
        auto r0 = clock::now();
        renderFrame();
        present();
        double renderMs = std::chrono::duration<double, std::milli>(clock::now() - r0).count();
        if (scene == Scene::FIGHT && !paused && SS > 1) {       // too slow for 60 FPS: cheaper fighter drawing
            slowFrames = renderMs > 15 ? slowFrames + 1 : std::max(0, slowFrames - 1);
            if (slowFrames > 90) SS = 1;
        }
        double spent = std::chrono::duration<double>(clock::now() - now).count();
        if (spent < step_) SDL_Delay(Uint32((step_ - spent) * 1000));
    }
}

bool Game::inTextMode() const {
    return (scene == Scene::PROFILES && profMode == "name") || (scene == Scene::ONLINE && on.state == "address");
}

void Game::handleEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                textAllowed = inTextMode();               // (typing only counts if we were already in a text box)
                keysPressed.insert(e.key.keysym.sym);
                onKey(e.key.keysym.sym, "");
                break;
            case SDL_TEXTINPUT:
                if (textAllowed && inTextMode()) onKey(0, e.text.text);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (auto* p = SDL_GameControllerOpen(e.cdevice.which)) {
                    if (std::find(pads.begin(), pads.end(), p) == pads.end()) pads.push_back(p);
                }
                break;
            default: break;
        }
    }
}

static bool printable(const std::string& t) {
    if (t.empty()) return false;
    for (unsigned char c : t) if (c < 32 || c == 127) return false;
    return true;
}
static size_t utf8Len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}
static void popUtf8(std::string& s) {
    while (!s.empty()) {
        unsigned char c = (unsigned char)s.back();
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

void Game::onKey(int k, const std::string& text) {
    bool up = k == SDLK_w || k == SDLK_UP, down = k == SDLK_s || k == SDLK_DOWN;
    bool enter = k == SDLK_RETURN || k == SDLK_KP_ENTER;
    if (k == SDLK_F11 || (enter && (SDL_GetModState() & KMOD_ALT))) {
        toggleFullscreen();
        return;
    }
    switch (scene) {
        case Scene::UPDATE:
            if (k == SDLK_ESCAPE) { updater->state = Updater::SKIPPED; scene = Scene::TITLE; }
            break;
        case Scene::TITLE: {
            const int n = 5;
            if (up || down) titleSel = (titleSel + (down ? 1 : -1) + n) % n;
            else if (enter || k == SDLK_SPACE) {
                if (titleSel == 4) running = false;
                else if (titleSel == 3) toggleFullscreen();
                else scene = titleSel == 0 ? Scene::MENU : titleSel == 1 ? Scene::ONLINE : Scene::PROFILES;
            } else if (k == SDLK_ESCAPE) running = false;
            break;
        }
        case Scene::ONLINE: onlineKey(k, text, up, down, enter); break;
        case Scene::PROFILES: profilesKey(k, text, up, down, enter); break;
        case Scene::MENU: {
            int n = int(ROSTER.size()), np = int(profiles->profiles.size()) + 1;
            if (k == SDLK_ESCAPE) scene = Scene::TITLE;
            else if (k == SDLK_a || k == SDLK_d) sel[0] = (sel[0] + (k == SDLK_d ? 1 : -1) + n) % n;
            else if (k == SDLK_LEFT || k == SDLK_RIGHT) sel[1] = (sel[1] + (k == SDLK_RIGHT ? 1 : -1) + n) % n;
            else if (k == SDLK_q || k == SDLK_e) profileSel[0] = (profileSel[0] + (k == SDLK_e ? 1 : -1) + np) % np;
            else if (k == SDLK_LEFTBRACKET || k == SDLK_RIGHTBRACKET)
                profileSel[1] = (profileSel[1] + (k == SDLK_RIGHTBRACKET ? 1 : -1) + np) % np;
            else if (up || down) stageSel = (stageSel + (down ? 1 : -1) + int(stages.size())) % int(stages.size());
            else if (k == SDLK_TAB) p2Mode = (p2Mode + 1) % 4;
            else if (k == SDLK_F2) p1Cpu = !p1Cpu;
            else if (enter) setupGame(sel[0], sel[1], stageSel, p2Mode, p1Cpu);
            break;
        }
        case Scene::FIGHT:
            if (netmatch) {
                if (k == SDLK_ESCAPE) {
                    net.send({{"type", "bye"}});
                    onlineBackToRoom("You left the match.");
                } else if (k == SDLK_F1) showBoxes = !showBoxes;
            } else {
                if (k == SDLK_ESCAPE || k == SDLK_p) paused = !paused;
                else if (k == SDLK_F1) showBoxes = !showBoxes;
                else if (paused && (k == SDLK_BACKSPACE || k == SDLK_q)) scene = Scene::MENU;
                else if (paused && enter) paused = false;
            }
            break;
        case Scene::CINEMATIC:
            if (enter || k == SDLK_SPACE || k == SDLK_ESCAPE) cine->skip();
            break;
        case Scene::RESULT:
            if (netmatch) {
                if (enter || k == SDLK_SPACE || k == SDLK_ESCAPE) onlineBackToRoom("");
            } else if (enter || k == SDLK_SPACE) {
                setupGame(sel[0], sel[1], stageSel, p2Mode, p1Cpu);
            } else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
                scene = Scene::MENU;
            }
            break;
    }
}

void Game::profilesKey(int k, const std::string& text, bool up, bool down, bool enter) {
    auto& profs = profiles->profiles;
    if (profMode == "name") {
        if (k == SDLK_ESCAPE) profMode = "";
        else if (enter) {
            std::string name = profText;
            size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(' ');
            name = a == std::string::npos ? "" : name.substr(a, b - a + 1);
            if (name.empty()) return;
            if (name.size() > 12) name.resize(12);
            if (profEdit < 0) {
                profs.push_back(Profile{name, false, {}});
                profCursor = int(profs.size()) - 1;
                profEdit = profCursor;
                profMode = "ask";
            } else {
                profs[size_t(profEdit)].name = name;
                profMode = "";
            }
            profiles->save();
        } else if (k == SDLK_BACKSPACE) popUtf8(profText);
        else if (k == 0 && printable(text) && utf8Len(profText) < 12) profText += text;
    } else if (profMode == "ask") {
        if (k == SDLK_y) { profMode = "bind"; profBindI = 0; profNewKeys.clear(); }
        else if (k == SDLK_n || k == SDLK_ESCAPE || enter) profMode = "";
    } else if (profMode == "bind") {
        if (k == 0) return;
        if (k == SDLK_ESCAPE) profMode = "";
        else {
            profNewKeys[ACTIONS[size_t(profBindI)].first] = {k};
            if (++profBindI >= int(ACTIONS.size())) {
                profs[size_t(profEdit)].keys = profNewKeys;
                profs[size_t(profEdit)].hasKeys = true;
                profiles->save();
                profMode = "";
            }
        }
    } else {
        bool has = !profs.empty() && profCursor < int(profs.size());
        if (k == SDLK_ESCAPE) scene = Scene::TITLE;
        else if ((up || down) && !profs.empty()) profCursor = (profCursor + (down ? 1 : -1) + int(profs.size())) % int(profs.size());
        else if (k == SDLK_n && profs.size() < 12) { profMode = "name"; profText = ""; profEdit = -1; }
        else if (has && k == SDLK_r) { profMode = "name"; profText = profs[size_t(profCursor)].name; profEdit = profCursor; }
        else if (has && (enter || k == SDLK_k)) { profEdit = profCursor; profMode = "bind"; profBindI = 0; profNewKeys.clear(); }
        else if (has && k == SDLK_x) { profs[size_t(profCursor)].hasKeys = false; profs[size_t(profCursor)].keys.clear(); profiles->save(); }
        else if (has && k == SDLK_DELETE) {
            profs.erase(profs.begin() + profCursor);
            profCursor = std::max(0, profCursor - 1);
            profileSel[0] = profileSel[1] = 0;
            profiles->save();
        }
    }
}

// ===================================================================== one 60th of a second
void Game::step() {
    ++frame;
    if (scene == Scene::CINEMATIC) {
        cine->step();
        if (cine->done) scene = Scene::RESULT;
        return;
    }
    if (scene == Scene::ONLINE || (netmatch && scene == Scene::RESULT)) onlineUpdate();
    if (scene != Scene::FIGHT || paused) return;
    if (netmatch && !netGate()) return;
    if (duel) { stepDuel(); return; }
    if (intro > 0) {
        --intro;
        if (intro > 40) { updateCamera(); return; }
    }
    if (endTimer > 0) {
        --endTimer;
        if (endTimer == 0) { scene = Scene::RESULT; return; }
        if (endTimer > 60 && endTimer % 3) return;         // dramatic slow motion on the final KO
    } else {
        for (auto& c : controllers) c->tick();
        if (combat->pendingDuel) {
            startDuel();
            combat->pendingDuel = nullptr;
            return;
        }
    }
    physics->updateAll();
    combat->checkAllMoveDurations();
    if (combat->nukeBy) {
        startAnnihilation(combat->nukeBy);
        return;
    }
    fx.update();
    ambient();
    updateCamera();
    if (physics->gameOver && endTimer == 0 && !winner) {
        for (auto& c : fighters)
            if (c->currentLives > 0) { winner = c.get(); break; }
        endTimer = 170;
    }
}

void Game::startAnnihilation(Fighter* shooter) {
    combat->nukeBy = nullptr;
    for (auto& c : fighters)
        if (c.get() != shooter) {
            c->currentLives = 0;
            shooter->kos += 1;
        }
    winner = shooter;
    endTimer = 0;
    duel.reset();
    cine = std::make_unique<Annihilation>(this, shooter);
    scene = Scene::CINEMATIC;
}

std::string Game::newPrompt(const std::string& last) {
    std::vector<std::string> opts;
    for (auto& [a, label] : ACTIONS) if (a != last) opts.push_back(a);
    return rng.choice(opts);
}

void Game::startDuel() {
    duel = std::make_unique<Duel>();
    duel->prompts[0] = newPrompt();
    duel->prompts[1] = newPrompt();
}

void Game::stepDuel() {
    Duel& d = *duel;
    updateCamera();
    if (d.intro > 0) { --d.intro; return; }
    for (int slot = 0; slot < 2; ++slot) {
        CharacterController& ctrl = *controllers[size_t(slot)];
        Fighter& c = *fighters[size_t(slot)];
        d.miss[slot] = std::max(0, d.miss[slot] - 1);
        std::set<std::string> pressed;
        if (ctrl.isAI()) {
            if (--d.cpu[slot] <= 0) {
                pressed.insert(d.prompts[slot]);
                int lvl = static_cast<AIController&>(ctrl).level;
                d.cpu[slot] = (lvl <= 1 ? 26 : lvl == 2 ? 18 : 12) + aiRng().randint(0, 8);
            }
        } else {
            InputState inp = ctrl.device->poll();
            for (auto& [name, bit] : BUTTONS) if (inp.pressed & bit) pressed.insert(name);
            if (inp.x && inp.x != d.prev[slot][0]) pressed.insert(inp.x < 0 ? "left" : "right");
            if (inp.y && inp.y != d.prev[slot][1]) pressed.insert(inp.y < 0 ? "up" : "down");
            d.prev[slot][0] = inp.x;
            d.prev[slot][1] = inp.y;
        }
        if (pressed.count(d.prompts[slot])) {
            d.counts[slot] += 1;
            d.prompts[slot] = newPrompt(d.prompts[slot]);
            fx.spark(c.x, c.y - c.h - 60, c.colors.accent, 5, 5, 14, 4);
        } else if (!pressed.empty()) {
            d.miss[slot] = 12;
        }
        if (d.counts[slot] >= DUEL_TARGET) {
            for (auto& o : fighters) o->aether = 0.0;
            duel.reset();
            combat->fireDeathRay(c);
            return;
        }
    }
    fx.update();
}

void Game::ambient() {
    if (frame % 6 != 0) return;
    Rng& r = fxRng();
    double x = r.uniform(cam.left(), cam.right()), y = r.uniform(cam.top(), cam.bottom());
    if (mapData->key == "crucible") fx.spark(x, cam.bottom(), Color(255, 160, 60), 1, 3, 90, 4, -0.12, -1.57, 0.5);
    else fx.spark(x, y, mapData->palette.soft, 1, 0.6, 70, 3, -0.01);
}

void Game::updateCamera() {
    Fighter* ray = nullptr;
    for (auto& c : fighters) if (c->move == &DEATH_RAY) { ray = c.get(); break; }
    std::vector<Vec2> pts;
    double minw;
    if (ray) {
        pts = {{ray->x, ray->y - ray->h / 2.0}, {ray->x + ray->facing * 330, ray->y - ray->h / 2.0}};
        minw = 700;
    } else {
        for (auto& c : fighters) if (c->state != "KO") pts.push_back({c->x, c->y - c->h / 2.0});
        if (pts.empty()) pts.push_back({mapData->respawnX, mapData->respawnY});
        pts.push_back({WORLD_W / 2.0, mapData->top - 120});
        minw = 860;
    }
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (auto& p : pts) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
    x0 -= 250; x1 += 250; y0 -= 210; y1 += 190;
    double w = clampv(std::max({x1 - x0, (y1 - y0) * 16 / 9, minw}), minw, double(WORLD_W));
    double tgt[3] = {(x0 + x1) / 2 - w / 2, (y0 + y1) / 2 - w * 9 / 32, w};
    for (int i = 0; i < 3; ++i) camf[i] += (tgt[i] - camf[i]) * 0.08;
    w = camf[2];
    double h = w * 9 / 16;
    double x = clampv(camf[0], 0.0, WORLD_W - w), y = clampv(camf[1], 0.0, WORLD_H - h);
    cam = Rect(int(x), int(y), int(w), int(h));
}

// ===================================================================== online
std::string Game::loadAddress() {
    for (auto& p : settingsPaths("aether_clash_online.json")) {
        Json d;
        if (!readJsonFile(p, d) || !d.is_object()) continue;
        std::string s = jstr(d, "server");
        if (s.size() > 60) s.resize(60);
        return s.empty() ? defaultServer() : s;
    }
    return defaultServer();
}

void Game::saveAddress(const std::string& text) {
    for (auto& p : settingsPaths("aether_clash_online.json"))
        if (writeJsonFile(p, Json{{"server", text}})) return;
}

std::string Game::onlineName() const {
    const Profile* p = slotProfile(0);
    return p ? p->name : "Player";
}

void Game::onlineConnect() {
    auto hp = splitAddress(on.text);
    if (!hp) {
        on.note = "Type the server address, for example  myserver.example.com:7777";
        return;
    }
    std::string t = on.text;
    size_t a = t.find_first_not_of(" \t"), b = t.find_last_not_of(" \t");
    saveAddress(a == std::string::npos ? "" : t.substr(a, b - a + 1));
    net.connect(hp->first, hp->second, onlineName());
    on.state = "connecting";
    on.note = "";
}

void Game::onlineDisconnect(const std::string& note) {
    net.close();
    netmatch.reset();
    on.state = "address";
    on.note = note;
    on.rooms.clear();
    on.code = "";
}

void Game::onlineBackToRoom(const std::string& note) {
    netmatch.reset();
    scene = Scene::ONLINE;
    on.state = "room";
    on.ready[0] = on.ready[1] = false;
    on.note = note;
}

void Game::sendPick() {
    net.send({{"type", "pick"}, {"char", on.chars[on.slot]}, {"stage", on.stage}, {"ready", on.ready[on.slot]}});
}

void Game::onlineKey(int k, const std::string& text, bool up, bool down, bool enter) {
    const std::string st = on.state;
    if (st == "address") {
        if (k == SDLK_ESCAPE) scene = Scene::TITLE;
        else if (enter) onlineConnect();
        else if (k == SDLK_BACKSPACE) popUtf8(on.text);
        else if (k == 0 && printable(text) && utf8Len(on.text) < 60) on.text += text;
    } else if (st == "connecting") {
        if (k == SDLK_ESCAPE) onlineDisconnect();
    } else if (st == "lobby") {
        if (k == SDLK_ESCAPE) onlineDisconnect();
        else if (k == SDLK_h) net.send({{"type", "host"}});
        else if (k == SDLK_r) net.send({{"type", "list"}});
        else if ((up || down) && !on.rooms.empty()) on.sel = (on.sel + (down ? 1 : -1) + int(on.rooms.size())) % int(on.rooms.size());
        else if (enter && !on.rooms.empty())
            net.send({{"type", "join"}, {"code", on.rooms[size_t(on.sel) % on.rooms.size()].value("code", Json(""))}});
    } else if (st == "hosting") {
        if (k == SDLK_ESCAPE) {
            net.send({{"type", "leave"}});
            net.send({{"type", "list"}});
            on.state = "lobby";
        }
    } else if (st == "room") {
        int me = on.slot;
        if (k == SDLK_ESCAPE) {
            net.send({{"type", "leave"}});
            net.send({{"type", "list"}});
            on.state = "lobby";
            on.note = "";
        } else if ((k == SDLK_a || k == SDLK_d || k == SDLK_LEFT || k == SDLK_RIGHT) && !on.ready[me]) {
            int n = int(ROSTER.size());
            on.chars[me] = (on.chars[me] + ((k == SDLK_d || k == SDLK_RIGHT) ? 1 : -1) + n) % n;
            sendPick();
        } else if ((up || down) && me == 0 && !on.ready[0]) {
            on.stage = (on.stage + (down ? 1 : -1) + int(stages.size())) % int(stages.size());
            sendPick();
        } else if (enter) {
            on.ready[me] = !on.ready[me];
            sendPick();
        }
    }
}

void Game::onlineUpdate() {
    if (on.state == "connecting" && net.status() == OnlineClient::ONLINE) {
        on.state = "lobby";
        net.send({{"type", "list"}});
    }
    if (on.state == "lobby" && frame % 120 == 0) net.send({{"type", "list"}});
    auto msgs = net.poll();
    for (size_t idx = 0; idx < msgs.size(); ++idx) {
        const Json& m = msgs[idx];
        std::string kind = jstr(m, "type");
        if (kind == "closed") {
            if (on.state != "address") {
                scene = Scene::ONLINE;
                onlineDisconnect(on.state == "connecting" ? "Could not reach the server." : "The connection to the server was lost.");
            }
        } else if (kind == "error") {
            on.note = jstr(m, "reason").substr(0, 90);
            if (on.state == "connecting") onlineDisconnect(on.note);
        } else if (kind == "rooms") {
            on.rooms.clear();
            if (m.contains("rooms") && m["rooms"].is_array())
                for (auto& r : m["rooms"])
                    if (r.is_object() && on.rooms.size() < 10) on.rooms.push_back(r);
        } else if (kind == "hosted") {
            on.state = "hosting";
            on.code = jstr(m, "code");
            on.note = "";
        } else if (kind == "paired") {
            on.state = "room";
            on.slot = jint(m, "slot") ? 1 : 0;
            on.opp = jstr(m, "opponent", "Player").substr(0, 12);
            on.seed = m.contains("seed") && m["seed"].is_number() ? uint64_t(m["seed"].get<long long>()) : 0;
            on.ready[0] = on.ready[1] = false;
            on.note = "";
            net.send({{"type", "ver"}, {"game", gameVersion()}});
            sendPick();
        } else if (kind == "ver" && jstr(m, "game") != gameVersion()) {
            on.note = "Different game versions (you " + gameVersion() + ", them " + jstr(m, "game") + "). Both should update.";
        } else if (kind == "pick" && on.state == "room") {
            int other = 1 - on.slot;
            int n = int(ROSTER.size());
            on.chars[other] = ((jint(m, "char") % n) + n) % n;
            on.ready[other] = jint(m, "ready") != 0;
            if (other == 0) on.stage = ((jint(m, "stage") % int(stages.size())) + int(stages.size())) % int(stages.size());
        } else if (kind == "opponent_left") {
            netmatch.reset();
            scene = Scene::ONLINE;
            if (on.slot == 0 && on.state == "room") {
                on.state = "hosting";
                on.note = on.opp + " left.";
            } else {
                on.state = "lobby";
                on.note = "The host closed the lobby.";
                net.send({{"type", "list"}});
            }
        } else if (kind == "start" && on.state == "room") {
            onlineStart(jint(m, "c0"), jint(m, "c1"), jint(m, "stage"));
            net.requeue(std::vector<Json>(msgs.begin() + long(idx) + 1, msgs.end()));   // the host's first inputs
            return;
        }
    }
    if (on.state == "room" && on.slot == 0 && on.ready[0] && on.ready[1]) {    // the host starts the match for both
        net.send({{"type", "start"}, {"c0", on.chars[0]}, {"c1", on.chars[1]}, {"stage", on.stage}});
        onlineStart(on.chars[0], on.chars[1], on.stage);
    }
}

void Game::onlineStart(int c0, int c1, int stage) {
    on.ready[0] = on.ready[1] = false;
    std::vector<std::string> names = on.slot == 0 ? std::vector<std::string>{onlineName(), on.opp}
                                                  : std::vector<std::string>{on.opp, onlineName()};
    int n = int(ROSTER.size()), ns = int(stages.size());
    std::pair<int, uint64_t> online{on.slot, on.seed};
    setupGame(((c0 % n) + n) % n, ((c1 % n) + n) % n, ((stage % ns) + ns) % ns, 1, false, &names, &online);
}

bool Game::netGate() {
    NetMatch& nm = *netmatch;
    auto msgs = net.poll();
    for (size_t idx = 0; idx < msgs.size(); ++idx) {
        const Json& m = msgs[idx];
        std::string kind = jstr(m, "type");
        if (kind == "i") {
            nm.inputs[1 - nm.local][jint(m, "f")] = jint(m, "b");
        } else if (kind == "c") {
            auto it = nm.sums.find(jint(m, "f", -1));
            if (it != nm.sums.end() && it->second != jint(m, "h", -1)) nm.desync = true;
        } else if (kind == "bye") {
            net.requeue(std::vector<Json>(msgs.begin() + long(idx) + 1, msgs.end()));
            onlineBackToRoom(on.opp + " left the match.");
            return false;
        } else if (kind == "opponent_left" || kind == "closed") {
            net.requeue(std::vector<Json>(msgs.begin() + long(idx), msgs.end()));
            netmatch.reset();
            scene = Scene::ONLINE;
            return false;
        }
    }
    if (nm.sent < nm.frame + INPUT_DELAY) {
        nm.sent += 1;
        int bits = encodeInput(localDevice->poll());
        nm.inputs[nm.local][nm.sent] = bits;
        net.sendInput(nm.sent, bits);
    } else {
        keepKeys = true;                                  // nothing sampled: keep the key presses for next time
    }
    auto a = nm.inputs[0].find(nm.frame), b = nm.inputs[1].find(nm.frame);
    if (a == nm.inputs[0].end() || b == nm.inputs[1].end()) {
        nm.stall += 1;
        return false;
    }
    nm.stall = 0;
    static_cast<QueuedDevice*>(controllers[0]->device.get())->bits = a->second;
    static_cast<QueuedDevice*>(controllers[1]->device.get())->bits = b->second;
    nm.inputs[0].erase(nm.frame - 1);
    nm.inputs[1].erase(nm.frame - 1);
    if (nm.frame % 120 == 0) {
        long long h = 0;
        for (auto& c : fighters)
            h += (long long)(c->x * 8) + (long long)(c->y * 8) * 3 + (long long)(c->damage * 10) * 7 + c->currentLives * 1009;
        int hv = int(h & 0xFFFFFF);
        nm.sums[nm.frame] = hv;
        nm.sums.erase(nm.frame - 1200);
        net.send({{"type", "c"}, {"f", nm.frame}, {"h", hv}});
    }
    nm.frame += 1;
    return true;
}

// ===================================================================== self-test
void Game::selftest(int frames) {
    int n = int(ROSTER.size()), ns = int(stages.size());
    const char* shots = std::getenv("AETHER_SCREENSHOTS");
    double renderMs = 0;
    int renders = 0;
    for (int i = 0; i < n * ns + 1; ++i) {
        int a = i % n, b = (i + 1 + i / n) % n, s = i % ns;
        bool secret = i == n * ns;
        std::vector<std::string> names = {SECRET_NAME, SECRET_NAME};
        setupGame(a, b, s, 2, true, secret ? &names : nullptr);
        int f = 0, duels = 0;
        while (scene == Scene::FIGHT && f < frames) {
            bool was = duel != nullptr;
            step();
            duels += int(duel && !was);
            ++f;
            if (f % 240 == 0 || (secret && f % 7 == 0)) {
                auto r0 = std::chrono::steady_clock::now();
                renderFrame();
                renderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
                ++renders;
            }
            if (shots && f == 600) savePNG(screen, joinPath(shots, "fight_" + std::to_string(i) + ".png"));
        }
        if (scene == Scene::CINEMATIC) {
            int cf = 0;
            while (scene == Scene::CINEMATIC) {
                step();
                ++cf;
                if (cf % 3 == 0) renderFrame();
                if (shots && (cf == 30 || cf == 80 || cf == 300 || cf == 450 || cf == 600))
                    savePNG(screen, joinPath(shots, "cine_" + std::to_string(cf) + ".png"));
            }
            renderFrame();
            std::printf("    Death Ray fired: %d-frame ending played, winner %s (%s)\n", cf, winner->playerName.c_str(),
                        winner->name.c_str());
        }
        Fighter *c1 = fighters[0].get(), *c2 = fighters[1].get();
        std::printf("%-20s %-5s vs %-5s: %5d frames | lives %d-%d | dmg %5.0f%% %5.0f%%", mapData->name.c_str(), c1->name.c_str(),
                    c2->name.c_str(), f, c1->currentLives, c2->currentLives, c1->damage, c2->damage);
        if (secret) std::printf(" | death ray duels: %d", duels);
        std::printf("\n");
        std::fflush(stdout);
    }
    const std::pair<Scene, const char*> scenes[] = {{Scene::TITLE, "title"}, {Scene::ONLINE, "online"},
                                                    {Scene::PROFILES, "profiles"}, {Scene::MENU, "menu"}, {Scene::UPDATE, "update"}};
    for (auto& [sc, label] : scenes) {
        scene = sc;
        renderFrame();
        if (shots) savePNG(screen, joinPath(shots, std::string(label) + ".png"));
    }
    std::printf("average match frame drawn in %.2f ms (%d frames)\n", renders ? renderMs / renders : 0.0, renders);
    std::printf("SELFTEST OK\n");
}

// ===================================================================== online test (two headless copies play each other)
namespace {
struct ScriptDevice : InputDevice {            // mashes buttons in a repeatable pattern
    Rng r;
    InputState cur;
    int hold = 0;
    explicit ScriptDevice(uint64_t seed) : r(seed) {}
    InputState poll() override {
        if (--hold <= 0) {
            cur = InputState();
            cur.x = r.randint(-1, 1);
            cur.y = r.random() < 0.2 ? r.randint(-1, 1) : 0;
            uint8_t b = BUTTONS[size_t(r.randint(0, int(BUTTONS.size()) - 1))].second;
            if (r.random() < 0.6) cur.pressed = cur.held = b;
            hold = r.randint(2, 12);
        } else {
            cur.pressed = 0;
        }
        return cur;
    }
};
}

int Game::nettest(const std::string& address, bool host, int frames) {
    on.text = address;
    scene = Scene::ONLINE;
    onlineConnect();
    bool asked = false;
    int fightFrames = 0;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120)) { std::printf("timed out\n"); return 1; }
        if (scene == Scene::ONLINE) {
            if (on.state == "lobby" && !asked && host) { net.send({{"type", "host"}}); asked = true; }
            if (on.state == "lobby" && !host && !on.rooms.empty() && !asked) {
                net.send({{"type", "join"}, {"code", on.rooms[0].value("code", Json(""))}});
                asked = true;
            }
            if (on.state == "room" && !on.ready[on.slot]) { on.ready[on.slot] = true; sendPick(); }
            if (on.state == "address" && !on.note.empty()) { std::printf("error: %s\n", on.note.c_str()); return 1; }
            if (on.state == "lobby" && !host && frame % 30 == 0) net.send({{"type", "list"}});
        }
        if (scene == Scene::FIGHT && netmatch && !dynamic_cast<ScriptDevice*>(localDevice.get()))
            localDevice = std::make_unique<ScriptDevice>(host ? 101 : 202);
        step();
        if (scene == Scene::FIGHT && netmatch) ++fightFrames;
        if ((scene == Scene::FIGHT || scene == Scene::RESULT) && netmatch && (netmatch->frame >= frames || scene == Scene::RESULT)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(scene == Scene::FIGHT ? 2 : 16));
    }
    long long h = 0;
    for (auto& c : fighters) h = h * 1000003 + (long long)(c->x * 100) * 7 + (long long)(c->y * 100) * 13 + (long long)(c->damage * 100);
    std::printf("%s: slot %d, frame %d, %s %.1f%% vs %s %.1f%%, lives %d-%d, state hash %lld, desync %s\n", host ? "host" : "guest",
                on.slot, netmatch ? netmatch->frame : -1, fighters[0]->name.c_str(), fighters[0]->damage, fighters[1]->name.c_str(),
                fighters[1]->damage, fighters[0]->currentLives, fighters[1]->currentLives, h, netmatch && netmatch->desync ? "YES" : "no");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(3));    // let the other copy finish too
    net.close();
    return 0;
}
