// Everything the Game Manager draws: the arena view, the HUD, and the title, menu, profile, online and result screens.
#include <cmath>

#include "game.h"

static const Color GOLD(255, 220, 120);

const Font& Game::font(int size) {
    auto it = fonts.find(size);
    if (it == fonts.end()) it = fonts.emplace(size, std::make_unique<Font>(fontData.data(), size)).first;
    return *it->second;
}

static Rect anchored(int w, int h, Vec2 pos, const std::string& anchor) {
    int x = int(pos.x), y = int(pos.y);
    if (anchor == "topleft") return Rect(x, y, w, h);
    if (anchor == "topright") return Rect(x - w, y, w, h);
    if (anchor == "midleft") return Rect(x, y - h / 2, w, h);
    if (anchor == "midright") return Rect(x - w, y - h / 2, w, h);
    return Rect(x - w / 2, y - h / 2, w, h);
}

Rect Game::text(Surface& surf, const std::string& s, int size, Color color, Vec2 pos, const std::string& anchor, bool shadow) {
    const Font& f = font(size);
    auto img = f.render(s, color);
    Rect r = anchored(img->w, img->h, pos, anchor);
    if (shadow) {
        auto sh = f.render(s, Color(0, 0, 0));
        surf.blit(*sh, r.x + 2, r.y + 3);
    }
    surf.blit(*img, r.x, r.y);
    return r;
}

void Game::panel(const Rect& rect, std::optional<Color> border, int alpha) {
    Surface p(rect.w, rect.h, true);
    draw::rect(p, Color(10, 8, 30, alpha), p.rect(), 0, 18);
    screen.blit(p, rect.x, rect.y);
    if (border) draw::rect(screen, *border, rect, 3, 18);
}

static std::string upper(std::string s) {
    for (auto& c : s) c = char(toupper((unsigned char)c));
    return s;
}

static std::string keyName(int k) {
    const char* n = SDL_GetKeyName(k);
    return upper(n && *n ? n : "?");
}

std::string Game::keyNames(const std::vector<int>& keys) const {
    std::string out;
    for (size_t i = 0; i < keys.size(); ++i) out += (i ? "  or  " : "") + keyName(keys[i]);
    return out;
}

static void vgradient(Surface& s, Color a, Color b, double power = 1.0) {
    for (int y = 0; y < s.h; ++y) s.fill(lerpColor(a, b, std::pow(double(y) / s.h, power)), Rect(0, y, s.w, 1));
}

void Game::menuBackdrop() {
    if (thumbs.empty()) {
        Stage* keep = mapData;
        for (auto& st : stages) {
            mapData = &st;
            Rect view(400, 330, 1600, 900);
            drawWorld(view);
            auto th = Surface::make(288, 162, false);
            xform::smoothscaleInto(world, view, *th);
            thumbs.push_back(th);
        }
        mapData = keep;
        menuBg = Surface::make(SCREEN_WIDTH, SCREEN_HEIGHT, false);
        vgradient(*menuBg, Color(14, 10, 40), Color(60, 26, 84));
    }
    screen.blit(*menuBg, 0, 0);
}

void Game::drawWorld(const Rect& view) {
    Stage& st = *mapData;
    world.setClip(view);
    world.blit(*st.bg, view.x, view.y, view);
    for (auto& p : st.platforms) drawPlatform(world, p, st.palette);
    bool live = physics && physics->map == mapData && (scene == Scene::FIGHT || scene == Scene::RESULT || scene == Scene::CINEMATIC);
    if (live) {
        for (auto& pr : physics->projectiles) drawProjectile(world, *pr, int(frame));
        translator.updateVisuals(world, fighterList(), *physics, showBoxes);
        fx.draw(world, font(30));
        if (showBoxes) draw::rect(world, Color(255, 0, 0), st.blast, 4);
    }
    world.resetClip();
}

Rect Game::renderWorldView() {
    Rect view = cam;
    if (fx.shake && !paused) {
        Rng& r = fxRng();
        view = view.move(r.randint(-fx.shake, fx.shake), r.randint(-fx.shake, fx.shake));
        view.clampInside(Rect(0, 0, WORLD_W, WORLD_H));
    }
    drawWorld(view);
    // fine-pixel finish: shrink the view to a 640x360 grid, then double every pixel
    xform::smoothscaleInto(world, view, lowres);
    xform::scaleInto(lowres, screen);
    return view;
}

void Game::renderFrame() {
    switch (scene) {
        case Scene::TITLE: menuBackdrop(); renderTitle(); return;
        case Scene::UPDATE: menuBackdrop(); renderUpdate(); return;
        case Scene::ONLINE: menuBackdrop(); renderOnline(); return;
        case Scene::PROFILES: menuBackdrop(); renderProfiles(); return;
        case Scene::MENU: menuBackdrop(); renderMenu(); return;
        default: break;
    }
    if (cine && (scene == Scene::CINEMATIC || cine->t >= Annihilation::T_SPACE)) {
        if (cine->t < Annihilation::T_BLAST) renderWorldView();
        cine->render(screen, scene == Scene::CINEMATIC);
        if (scene == Scene::RESULT) renderResult();
        return;
    }
    Rect view = renderWorldView();
    renderHud(view);
    for (auto& c : fighters)
        if (c->move == &DEATH_RAY) {                       // the Death Ray coming out: cinematic bars
            int bar = int(78 * easeOut(c->moveFrame / 18.0));
            draw::rect(screen, Color(0, 0, 0), Rect(0, 0, SCREEN_WIDTH, bar));
            draw::rect(screen, Color(0, 0, 0), Rect(0, SCREEN_HEIGHT - bar, SCREEN_WIDTH, bar));
            if (c->moveFrame > 8 && (c->moveFrame / 6) % 2) text(screen, "DEATH RAY", 64, Color(255, 70, 70), Vec2(SCREEN_WIDTH / 2, 40));
        }
    if (scene == Scene::RESULT) renderResult();
}

// ------------------------------------------------------------ in-match HUD
void Game::renderHud(const Rect& view) {
    Surface& scr = screen;
    double scale = double(SCREEN_WIDTH) / view.w;
    std::map<int, Vec2> heads;
    for (auto& c : fighters) {
        if (c->state == "KO") continue;
        double sx = (c->x - view.x) * scale, sy = (c->y - c->h - 30 - view.y) * scale;
        heads[c->slot] = {sx, sy};
        Color col = PLAYER_TAGS[c->slot];
        text(scr, c->playerName, 22, col, Vec2(sx, sy - 12));
        draw::polygon(scr, col, {{sx - 7, sy}, {sx + 7, sy}, {sx, sy + 9}});
    }
    for (auto& cp : fighters) {
        Fighter& c = *cp;
        double px = SCREEN_WIDTH * (c.slot == 0 ? 0.28 : 0.72);
        panel(Rect(int(px - 150), SCREEN_HEIGHT - 116, 300, 104), PLAYER_TAGS[c.slot], 170);
        Vec2 icon(int(px - 108), SCREEN_HEIGHT - 66);
        draw::circle(scr, c.colors.dark, icon, 30);
        draw::circle(scr, c.colors.main, icon, 24);
        draw::circle(scr, c.colors.accent, icon, 10);
        double d = c.damage;
        Color col = d < 70 ? lerpColor(WHITE, Color(255, 200, 60), d / 70)
                  : d < 140 ? lerpColor(Color(255, 200, 60), Color(255, 50, 40), (d - 70) / 70)
                            : lerpColor(Color(255, 50, 40), Color(140, 10, 30), (d - 140) / 80);
        int jx = 0, jy = 0;
        if (c.hitstop > 0) { jx = fxRng().randint(-3, 3); jy = fxRng().randint(-3, 3); }
        text(scr, std::to_string(int(d)) + "%", 64, col, Vec2(px + 10 + jx, SCREEN_HEIGHT - 74 + jy));
        text(scr, c.playerName + "  -  " + upper(c.name), 20, WHITE, Vec2(px - 70, SCREEN_HEIGHT - 108), "topleft");
        for (int i = 0; i < c.currentLives; ++i) {
            Vec2 p(int(px + 92 + i * 18), SCREEN_HEIGHT - 80);
            draw::circle(scr, c.colors.main, p, 6);
            draw::circle(scr, WHITE, p, 6, 2);
        }
        Rect bar(int(px - 70), SCREEN_HEIGHT - 38, 208, 14);
        draw::rect(scr, Color(20, 30, 50), bar, 0, 7);
        Rect fill = bar;
        fill.w = int(bar.w * c.aether / AETHER_MAX);
        bool rayReady = c.isCryptic() && c.aether >= AETHER_MAX;
        Color acol = (c.flash > 0 && c.aether < 30) ? Color(255, 120, 120)
                                                    : lerpColor(Color(70, 150, 255), Color(140, 255, 250), c.aether / AETHER_MAX);
        if (rayReady) acol = (frame / 6) % 2 ? Color(255, 70, 70) : Color(255, 230, 120);
        if (fill.w > 3) draw::rect(scr, acol, fill, 0, 7);
        draw::rect(scr, WHITE, bar, 2, 7);
        text(scr, rayReady ? "DEATH RAY READY - press Special" : "AETHER", 16, rayReady ? Color(255, 200, 200) : Color(170, 220, 255),
             Vec2(px - 70, SCREEN_HEIGHT - 52), "topleft", false);
    }
    Vec2 mid(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 60);
    if (intro > 40) text(scr, "READY...", 110, WHITE, mid);
    else if (intro > 0) text(scr, "CLASH!", 150, Color(255, 220, 90), mid);
    if (endTimer > 0) text(scr, "GAME!", 170, Color(255, 90, 90), mid);
    if (netmatch) {
        if (netmatch->stall > 20) text(scr, "Waiting for the other player...", 34, Color(255, 220, 120), Vec2(SCREEN_WIDTH / 2, 150));
        if (netmatch->desync)
            text(scr, "The two games have drifted apart - please rematch", 24, Color(255, 150, 150), Vec2(SCREEN_WIDTH / 2, 24));
        text(scr, "ONLINE   -   ESC leaves the match", 16, Color(170, 170, 220), Vec2(SCREEN_WIDTH / 2, SCREEN_HEIGHT - 10), "center", false);
    }
    if (duel) renderDuel(heads);
    if (paused) {
        Surface dim(SCREEN_WIDTH, SCREEN_HEIGHT, true);
        dim.fill(Color(0, 0, 20, 185));
        scr.blit(dim, 0, 0);
        text(scr, "PAUSED", 90, WHITE, Vec2(SCREEN_WIDTH / 2, 70));
        renderControls(130);
        text(scr, "ENTER / ESC resume      Q quit to menu      F1 show hitboxes      F11 full screen", 26, Color(200, 210, 255),
             Vec2(SCREEN_WIDTH / 2, 580));
    }
}

void Game::renderDuel(const std::map<int, Vec2>& heads) {
    Surface& scr = screen;
    Duel& d = *duel;
    Surface dim(SCREEN_WIDTH, SCREEN_HEIGHT, true);
    dim.fill(Color(30, 0, 10, 130));
    scr.blit(dim, 0, 0);
    text(scr, "DEATH RAY DUEL!", 96, Color(255, 90, 90), Vec2(SCREEN_WIDTH / 2, 80));
    text(scr, "Two " + SECRET_NAME + "s, one gun. First to hit " + std::to_string(DUEL_TARGET) + " keys fires it.", 28, WHITE,
         Vec2(SCREEN_WIDTH / 2, 136));
    if (d.intro > 0) text(scr, "GET READY", 70, Color(255, 220, 120), Vec2(SCREEN_WIDTH / 2, 220));
    for (int slot = 0; slot < 2; ++slot) {
        CharacterController& ctrl = *controllers[size_t(slot)];
        Fighter& c = *fighters[size_t(slot)];
        auto it = heads.find(slot);
        Vec2 hpos = it != heads.end() ? it->second : Vec2(SCREEN_WIDTH * (0.3 + 0.4 * slot), 300);
        double hx = clampv(hpos.x, 110.0, SCREEN_WIDTH - 110.0), hy = clampv(hpos.y - 124, 190.0, SCREEN_HEIGHT - 260.0);
        const std::string& action = d.prompts[slot];
        std::string label;
        if (ctrl.isAI()) {
            for (auto& [a, lab] : ACTIONS)
                if (a == action) label = upper(lab.substr(0, lab.find(" /")));
        } else {
            const KeyMap* km = nullptr;
            if (auto* kd = dynamic_cast<KeyboardDevice*>(ctrl.device.get())) km = &kd->keymap;
            else if (auto* qd = dynamic_cast<QueuedDevice*>(ctrl.device.get())) km = &qd->keymap;
            label = km && km->count(action) && !km->at(action).empty() ? keyName(km->at(action)[0]) : upper(action);
        }
        bool miss = d.miss[slot] > 0;
        Rect cap(0, 0, std::max(84, font(54).width(label) + 36), 76);
        cap.setCenter(hx, hy);
        if (d.intro == 0) {
            draw::rect(scr, miss ? Color(90, 20, 20) : Color(245, 245, 250), cap, 0, 12);
            draw::rect(scr, PLAYER_TAGS[slot], cap, 5, 12);
            text(scr, label, 54, miss ? Color(255, 120, 120) : Color(20, 16, 40), cap.center(), "center", false);
        }
        Rect bar(int(hx - 90), cap.bottom() + 8, 180, 14);
        draw::rect(scr, Color(30, 20, 30), bar, 0, 7);
        draw::rect(scr, c.colors.accent, Rect(bar.x, bar.y, int(bar.w * d.counts[slot] / double(DUEL_TARGET)), bar.h), 0, 7);
        draw::rect(scr, WHITE, bar, 2, 7);
        text(scr, std::to_string(d.counts[slot]) + " / " + std::to_string(DUEL_TARGET), 24, WHITE, Vec2(hx, bar.bottom() + 14));
    }
}

void Game::renderControls(int y) {
    Surface& scr = screen;
    KeyMap kms[2] = {slotKeymap(0), slotKeymap(1)};
    bool cpu[2] = {false, false};
    std::string names[2] = {slotName(0), slotName(1)};
    for (int i = 0; i < 2 && i < int(controllers.size()); ++i) cpu[i] = controllers[size_t(i)]->isAI();
    for (int i = 0; i < 2 && i < int(fighters.size()); ++i) names[i] = fighters[size_t(i)]->playerName;
    text(scr, names[0], 26, PLAYER_TAGS[0], Vec2(560, y), "topleft");
    text(scr, names[1], 26, PLAYER_TAGS[1], Vec2(860, y), "topleft");
    for (size_t i = 0; i < ACTIONS.size(); ++i) {
        int yy = y + 32 + int(i) * 29;
        text(scr, ACTIONS[i].second, 24, Color(190, 200, 240), Vec2(240, yy), "topleft");
        for (int slot = 0; slot < 2; ++slot)
            text(scr, cpu[slot] ? "(CPU)" : keyNames(kms[slot][ACTIONS[i].first]), 24, WHITE, Vec2(slot == 0 ? 560 : 860, yy), "topleft");
    }
    const char* tips[] = {"Hold a direction with Attack / Heavy / Special for a different move. Tap Jump for a short hop.",
                          "Shield just before a hit = PARRY (+25 Aether).  Shield + left/right = roll.  Shield in the air = air dodge.",
                          "GRAB, then press a direction to THROW that way, or Attack to pommel first.",
                          "Down attacks have a SPIKE spot: land it on an airborne rival to send them straight down."};
    for (int i = 0; i < 4; ++i) text(scr, tips[i], 21, Color(220, 220, 240), Vec2(SCREEN_WIDTH / 2, y + 338 + i * 24), "center", false);
}

// ------------------------------------------------------------ menu screens
void Game::titleBackdrop() {
    if (!titleBg) {
        Surface bg(SCREEN_WIDTH, SCREEN_HEIGHT, false);
        vgradient(bg, Color(16, 10, 40), Color(64, 30, 96), 1.4);
        Rng r(11);
        const std::vector<Color> neb = {Color(120, 60, 170), Color(70, 50, 150), Color(150, 70, 150)};
        for (int i = 0; i < 7; ++i) {
            Color c = r.choice(neb);
            int x = r.randint(0, SCREEN_WIDTH), y = r.randint(60, 520), rad = r.randint(160, 320);
            glow(bg, c, Vec2(x, y), rad, 34);
        }
        const std::vector<int> radii = {1, 1, 1, 2, 2, 3};
        const std::vector<Color> cols = {WHITE, Color(255, 200, 255), Color(160, 200, 255), Color(255, 230, 170)};
        for (int i = 0; i < 150; ++i) {
            TitleStar s;
            s.x = r.randint(0, SCREEN_WIDTH);
            s.y = r.randint(0, 560);
            s.r = r.choice(radii);
            s.col = r.choice(cols);
            s.ph = r.uniform(0, 6);
            titleStars.push_back(s);
        }
        titleGroundY = SCREEN_HEIGHT - 110;
        if (uiGround) {
            auto g = xform::scale(*uiGround, uiGround->w * 2, uiGround->h * 2);
            for (int x = 0; x < SCREEN_WIDTH; x += g->w) bg.blit(*g, x, titleGroundY - 8);
        } else {
            draw::rect(bg, Color(44, 40, 60), Rect(0, titleGroundY, SCREEN_WIDTH, 120));
            draw::rect(bg, Color(70, 110, 70), Rect(0, titleGroundY, SCREEN_WIDTH, 10));
        }
        auto small = xform::smoothscale(bg, SCREEN_WIDTH / PIXEL, SCREEN_HEIGHT / PIXEL);
        titleBg = xform::scale(*small, SCREEN_WIDTH, SCREEN_HEIGHT);
    }
    screen.blit(*titleBg, 0, 0);
    for (auto& s : titleStars) {
        double k = 0.5 + 0.5 * std::sin(frame * 0.05 + s.ph);
        Color c = lerpColor(Color(60, 40, 100), s.col, k);
        if (s.r >= 3) draw::polygon(screen, c, starPoints(s.x, s.y, 4 + 3 * k));
        else draw::rect(screen, c, Rect(s.x, s.y, s.r * 2, s.r * 2));
    }
}

static void blitCentered(Surface& dst, const Surface& src, double cx, double cy) {
    Rect r = src.rect();
    r.setCenter(cx, cy);
    dst.blit(src, r.x, r.y);
}

void Game::renderUpdate() {
    Surface& scr = screen;
    titleBackdrop();
    blitCentered(scr, *logo, SCREEN_WIDTH / 2, 170);
    text(scr, "AETHER CLASH", 96, Color(255, 214, 110), Vec2(SCREEN_WIDTH / 2, 300));
    Rect box(0, 0, 640, 120);
    box.setCenter(SCREEN_WIDTH / 2, 430);
    panel(box, Color(255, 220, 120), 210);
    text(scr, updater->status(), 30, WHITE, Vec2(box.centerx(), box.y + 36));
    Rect bar(box.x + 40, box.y + 72, box.w - 80, 18);
    draw::rect(scr, Color(30, 24, 60), bar, 0, 9);
    if (updater->state == Updater::CHECKING) {
        int x = bar.x + int(frame * 6 % (bar.w + 120)) - 120;
        draw::rect(scr, Color(140, 200, 255), Rect(x, bar.y, 120, bar.h).clip(bar), 0, 9);
    } else {
        draw::rect(scr, Color(120, 255, 170), Rect(bar.x, bar.y, int(bar.w * updater->progress.load()), bar.h), 0, 9);
    }
    draw::rect(scr, WHITE, bar, 2, 9);
    text(scr, "version " + gameVersion() + "        ESC  skip", 20, Color(190, 190, 230), Vec2(SCREEN_WIDTH / 2, 700), "center", false);
}

void Game::renderTitle() {
    Surface& scr = screen;
    long t = frame;
    titleBackdrop();
    int gy = titleGroundY;
    const int xs[4] = {120, 320, 960, 1160};
    for (int i = 0; i < int(ROSTER.size()) && i < 4; ++i) {
        const FighterSpec& spec = ROSTER[size_t(i)];
        int x = xs[i];
        auto spr = quickSprite(spec, spec.colors, "idle", i < 2 ? 1 : -1, double(t + i * 37), 1.9);
        scr.blit(*spr, x - SPR_CX * 1.9, gy + 6 - SPR_FEET * 1.9);
        text(scr, upper(spec.name), 24, spec.colors.accent, Vec2(x, gy + 34));
    }
    blitCentered(scr, *logo, SCREEN_WIDTH / 2, 96);
    const std::pair<int, Color> layers[3] = {{6, Color(90, 50, 10)}, {3, Color(170, 100, 20)}, {0, Color(255, 214, 110)}};
    for (auto& [dy, col] : layers) text(scr, "AETHER CLASH", 118, col, Vec2(SCREEN_WIDTH / 2, 226 + dy), "center", false);
    text(scr, "AETHER CLASH", 118, Color(255, 244, 190), Vec2(SCREEN_WIDTH / 2, 223), "center", false);
    text(scr, "AETHER CLASH", 118, Color(255, 214, 110), Vec2(SCREEN_WIDTH / 2, 226), "center", false);
    for (int sx : {-330, 330})
        draw::polygon(scr, Color(255, 240, 180), starPoints(SCREEN_WIDTH / 2 + sx, 196, 12 + 3 * std::sin(t * 0.08)));
    const char* opts[5] = {"Local Play", "Online", "Profiles & Controls", "Full Screen", "Quit"};
    for (int i = 0; i < 5; ++i) {
        bool s = i == titleSel;
        Rect r(0, 0, 400, 46);
        r.setCenter(SCREEN_WIDTH / 2, 316 + i * 56);
        draw::rect(scr, s ? Color(14, 10, 34) : Color(70, 74, 96), r, 0, 12);
        draw::rect(scr, s ? Color(255, 220, 120) : Color(40, 42, 60), r, 3, 12);
        std::string opt = opts[i];
        if (i == 3) opt = std::string("Full Screen:  ") + (fullscreen ? "On" : "Off");
        text(scr, opt, s ? 34 : 30, s ? Color(255, 226, 130) : Color(226, 228, 240), r.center());
    }
    text(scr, "W / S  or  UP / DOWN to choose      ENTER to select      F11  full screen", 20, Color(190, 190, 230),
         Vec2(SCREEN_WIDTH / 2, 704), "center", false);
    std::string st = updater->status();
    text(scr, st.empty() ? "version " + gameVersion() : st, 18, Color(190, 190, 230), Vec2(SCREEN_WIDTH - 12, 12), "topright", false);
}

void Game::renderOnline() {
    Surface& scr = screen;
    const std::string& st = on.state;
    int mid = SCREEN_WIDTH / 2;
    text(scr, "ONLINE PLAY", 90, GOLD, Vec2(mid, 70));
    Color soft(215, 215, 240);
    std::string foot;
    if (st == "address" || st == "connecting") {
        Rect box(240, 200, 800, 250);
        panel(box, Color(96, 170, 255));
        text(scr, "Server address", 34, WHITE, Vec2(mid, box.y + 44));
        Rect field(box.x + 60, box.y + 84, box.w - 120, 56);
        draw::rect(scr, Color(8, 6, 24), field, 0, 10);
        draw::rect(scr, GOLD, field, 3, 10);
        std::string cursor = ((frame / 20) % 2 && st == "address") ? "_" : "";
        std::string shown = on.text + cursor;
        text(scr, shown.empty() ? " " : shown, 38, WHITE, field.center());
        text(scr, st == "connecting" ? "Connecting..." : "for example   myserver.example.com:7777   or   203.0.113.5:7777", 24,
             st == "connecting" ? GOLD : soft, Vec2(mid, box.y + 176), "center", false);
        text(scr, "You will appear as:  " + onlineName() + "    (choose a profile as Player 1 to change it)", 22, soft,
             Vec2(mid, box.y + 214), "center", false);
        foot = st == "connecting" ? "ESC cancel" : "ENTER connect        ESC back";
    } else if (st == "lobby") {
        Rect box(290, 150, 700, 400);
        panel(box, Color(96, 170, 255));
        text(scr, "Open lobbies", 36, WHITE, Vec2(mid, box.y + 34));
        if (on.rooms.empty()) {
            text(scr, "Nobody is hosting right now.", 28, soft, Vec2(mid, box.y + 130), "center", false);
            text(scr, "Press H to host a lobby and wait for a friend.", 26, GOLD, Vec2(mid, box.y + 170), "center", false);
        }
        for (size_t i = 0; i < on.rooms.size(); ++i) {
            const Json& r = on.rooms[i];
            bool s = int(i) == on.sel % int(on.rooms.size());
            Rect row(box.x + 30, box.y + 70 + int(i) * 32, box.w - 60, 30);
            if (s) draw::rect(scr, Color(70, 50, 110), row, 0, 8);
            std::string host = r.contains("host") && r["host"].is_string() ? r["host"].get<std::string>() : "?";
            std::string code = r.contains("code") ? (r["code"].is_string() ? r["code"].get<std::string>() : r["code"].dump()) : "";
            text(scr, host + "'s lobby", 28, s ? GOLD : WHITE, Vec2(row.x + 14, row.centery()), "midleft");
            text(scr, code, 24, soft, Vec2(row.right() - 14, row.centery()), "midright", false);
        }
        foot = "H host a lobby      UP / DOWN choose      ENTER join      R refresh      ESC disconnect";
    } else if (st == "hosting") {
        Rect box(340, 200, 600, 250);
        panel(box, GOLD);
        text(scr, "Your lobby is open", 40, WHITE, Vec2(mid, box.y + 50));
        text(scr, "Lobby code:  " + on.code, 54, GOLD, Vec2(mid, box.y + 122));
        text(scr, "Waiting for another player to join" + std::string(size_t(1 + (frame / 30) % 3), '.'), 28, soft,
             Vec2(mid, box.y + 194), "center", false);
        foot = "ESC close the lobby";
    } else {
        std::string names[2] = {on.slot == 0 ? onlineName() : on.opp, on.slot == 0 ? on.opp : onlineName()};
        for (int slot = 0; slot < 2; ++slot) {
            const FighterSpec& spec = ROSTER[size_t(on.chars[slot]) % ROSTER.size()];
            Rect card(slot == 0 ? 90 : SCREEN_WIDTH - 470, 130, 380, 400);
            panel(card, PLAYER_TAGS[slot]);
            bool mine = slot == on.slot;
            text(scr, names[slot] + (mine ? "  (you)" : ""), 30, PLAYER_TAGS[slot], Vec2(card.centerx(), card.y + 28));
            auto spr = quickSprite(spec, spec.colors, "idle", slot == 0 ? 1 : -1, double(frame), 1.7);
            scr.blit(*spr, card.centerx() - SPR_CX * 1.7, card.y + 290 - SPR_FEET * 1.7);
            text(scr, mine && !on.ready[slot] ? "<  " + upper(spec.name) + "  >" : upper(spec.name), 40, WHITE,
                 Vec2(card.centerx(), card.y + 320));
            text(scr, on.ready[slot] ? "READY!" : "choosing...", 30, on.ready[slot] ? Color(120, 255, 150) : soft,
                 Vec2(card.centerx(), card.y + 364));
        }
        const Stage& stg = stages[size_t(on.stage) % stages.size()];
        text(scr, "VS", 70, WHITE, Vec2(mid, 190));
        const Surface& thumb = *thumbs[size_t(on.stage) % stages.size()];
        Rect tr = thumb.rect();
        tr.setCenter(mid, 340);
        scr.blit(thumb, tr.x, tr.y);
        draw::rect(scr, GOLD, tr, 3, 4);
        text(scr, on.slot == 0 ? "<  " + stg.name + "  >" : stg.name, 28, GOLD, Vec2(mid, 244));
        text(scr, "the host picks the stage", 20, soft, Vec2(mid, 440), "center", false);
        foot = std::string("A / D fighter      ") + (on.slot == 0 ? "W / S stage      " : "") + "ENTER ready / not ready      ESC leave";
    }
    if (!on.note.empty()) text(scr, on.note, 26, Color(255, 150, 150), Vec2(mid, 590));
    text(scr, foot, 24, GOLD, Vec2(mid, 650));
}

void Game::renderProfiles() {
    Surface& scr = screen;
    auto& profs = profiles->profiles;
    text(scr, "PROFILES & CONTROLS", 76, GOLD, Vec2(SCREEN_WIDTH / 2, 60));
    text(scr, "A profile is your player name plus (optionally) your own key bindings. Pick it on the character screen.", 22,
         Color(200, 200, 235), Vec2(SCREEN_WIDTH / 2, 110), "center", false);
    Rect left(60, 140, 420, 470);
    panel(left, GOLD);
    if (profs.empty()) {
        text(scr, "No profiles yet.", 30, WHITE, Vec2(left.centerx(), left.y + 60));
        text(scr, "Press N to make one.", 26, Color(200, 200, 235), Vec2(left.centerx(), left.y + 100));
    }
    for (size_t i = 0; i < profs.size(); ++i) {
        bool s = int(i) == profCursor;
        Rect row(left.x + 14, left.y + 14 + int(i) * 37, left.w - 28, 33);
        if (s) draw::rect(scr, Color(70, 50, 110), row, 0, 8);
        text(scr, profs[i].name, 30, s ? GOLD : WHITE, Vec2(row.x + 12, row.centery()), "midleft");
        text(scr, profs[i].hasKeys ? "custom keys" : "default keys", 20, profs[i].hasKeys ? Color(170, 220, 255) : Color(150, 150, 190),
             Vec2(row.right() - 12, row.centery()), "midright", false);
    }
    Rect right(520, 140, 700, 470);
    panel(right, Color(96, 170, 255));
    const Profile* cur = (!profs.empty() && profCursor < int(profs.size())) ? &profs[size_t(profCursor)] : nullptr;
    text(scr, cur ? "Keys for:  " + cur->name : "Built-in default keys", 30, GOLD, Vec2(right.x + 24, right.y + 16), "topleft");
    bool showDefaults = !(cur && cur->hasKeys) && profMode != "bind";
    if (showDefaults) {
        text(scr, "as Player 1", 22, PLAYER_TAGS[0], Vec2(right.x + 300, right.y + 56), "topleft", false);
        text(scr, "as Player 2", 22, PLAYER_TAGS[1], Vec2(right.x + 480, right.y + 56), "topleft", false);
    }
    KeyMap d1 = defaultKeymap(0), d2 = defaultKeymap(1);
    for (size_t i = 0; i < ACTIONS.size(); ++i) {
        const std::string& a = ACTIONS[i].first;
        int yy = right.y + 86 + int(i) * 36;
        bool binding = profMode == "bind" && int(i) == profBindI;
        text(scr, ACTIONS[i].second, 26, binding ? GOLD : Color(200, 205, 240), Vec2(right.x + 24, yy), "topleft", false);
        if (profMode == "bind") {
            auto it = profNewKeys.find(a);
            std::string txt = it != profNewKeys.end() ? keyNames(it->second) : (binding ? "press a key..." : "");
            if (!txt.empty()) text(scr, txt, 26, it != profNewKeys.end() ? WHITE : GOLD, Vec2(right.x + 300, yy), "topleft", false);
        } else if (cur && cur->hasKeys) {
            auto it = cur->keys.find(a);
            std::string txt = it != cur->keys.end() ? keyNames(it->second) : "";
            if (!txt.empty()) text(scr, txt, 26, WHITE, Vec2(right.x + 300, yy), "topleft", false);
        } else {
            text(scr, keyNames(d1[a]), 24, WHITE, Vec2(right.x + 300, yy), "topleft", false);
            text(scr, keyNames(d2[a]), 24, WHITE, Vec2(right.x + 480, yy), "topleft", false);
        }
    }
    std::string help;
    if (profMode == "name") {
        Rect box(0, 0, 620, 150);
        box.setCenter(SCREEN_WIDTH / 2, 360);
        panel(box, GOLD, 250);
        text(scr, "Type your name (up to 12 letters), then press ENTER", 26, WHITE, Vec2(box.centerx(), box.y + 34));
        text(scr, profText + ((frame / 20) % 2 ? "_" : " "), 54, GOLD, Vec2(box.centerx(), box.y + 96));
        help = "ENTER confirm      ESC cancel";
    } else if (profMode == "ask") {
        Rect box(0, 0, 700, 130);
        box.setCenter(SCREEN_WIDTH / 2, 360);
        panel(box, GOLD, 250);
        std::string nm = profEdit >= 0 && profEdit < int(profs.size()) ? profs[size_t(profEdit)].name : "";
        text(scr, "Set your own keys for " + nm + "?", 34, WHITE, Vec2(box.centerx(), box.y + 40));
        text(scr, "Y = yes, choose my keys          N = no, use the default keys", 26, GOLD, Vec2(box.centerx(), box.y + 90));
    } else if (profMode == "bind") {
        help = "Press the key you want for:  " + upper(ACTIONS[size_t(profBindI)].second) + "   (" + std::to_string(profBindI + 1) +
               " of " + std::to_string(ACTIONS.size()) + ")      ESC cancel";
    } else {
        help = "N new profile     ENTER set keys     R rename     X reset to default keys     DELETE remove     ESC back";
    }
    if (!help.empty()) text(scr, help, 24, GOLD, Vec2(SCREEN_WIDTH / 2, 650));
}

void Game::renderMenu() {
    Surface& scr = screen;
    long t = frame;
    static const char* P2_MODES[4] = {"CPU - Easy", "CPU - Normal", "CPU - Hard", "Human"};
    text(scr, "CHOOSE YOUR FIGHTER", 80, GOLD, Vec2(SCREEN_WIDTH / 2, 62));
    for (int slot = 0; slot < 2; ++slot) {
        const FighterSpec& spec = ROSTER[size_t(sel[slot])];
        const Palette& col = (slot == 1 && sel[0] == sel[1]) ? spec.alt : spec.colors;
        int x0 = slot == 0 ? 40 : SCREEN_WIDTH - 480;
        Rect card(x0, 120, 440, 350);
        panel(card, PLAYER_TAGS[slot]);
        bool cpu = slot == 0 ? p1Cpu : p2Mode != 3;
        std::string who = slot == 0 ? (p1Cpu ? "CPU - Hard" : "Human") : P2_MODES[p2Mode];
        text(scr, "PLAYER " + std::to_string(slot + 1) + "  -  " + who, 26, PLAYER_TAGS[slot], Vec2(card.x + 18, card.y + 12), "topleft");
        std::string pose = (t / 180) % 2 ? "run" : "idle";
        auto spr = quickSprite(spec, col, pose, slot == 0 ? 1 : -1, double(t));
        scr.blit(*spr, card.x + 110 - SPR_CX * 3 / 2, card.y + 238 - SPR_FEET * 3 / 2);
        int tx = card.x + 210;
        text(scr, "<  " + upper(spec.name) + "  >", 46, WHITE, Vec2(tx, card.y + 46), "topleft");
        text(scr, spec.title + "  -  " + spec.faction, 24, col.accent, Vec2(tx, card.y + 86), "topleft");
        const char* labels[4] = {"Speed", "Power", "Weight", "Range"};
        for (int i = 0; i < 4; ++i) {
            int yy = card.y + 120 + i * 23;
            text(scr, labels[i], 20, Color(190, 200, 240), Vec2(tx, yy), "topleft", false);
            for (int k = 0; k < 5; ++k)
                draw::rect(scr, k < spec.bars[size_t(i)] ? col.main : Color(50, 46, 80), Rect(tx + 70 + k * 24, yy + 1, 20, 12), 0, 3);
        }
        const Profile* prof = slotProfile(slot);
        std::string keys = (prof && prof->hasKeys) ? "custom keys" : "default keys";
        text(scr, "Profile:  <  " + slotName(slot, cpu) + "  >", 26, GOLD, Vec2(tx, card.y + 216), "topleft");
        text(scr, keys + "   -   change with " + (slot == 0 ? "Q / E" : "[  /  ]"), 18, Color(170, 170, 220), Vec2(tx, card.y + 242),
             "topleft", false);
        for (size_t i = 0; i < spec.blurb.size(); ++i)
            text(scr, spec.blurb[i], 21, Color(220, 220, 240), Vec2(card.x + 18, card.y + 268 + int(i) * 24), "topleft", false);
    }
    text(scr, "A / D  fighter", 24, PLAYER_TAGS[0], Vec2(260, 488));
    text(scr, "LEFT / RIGHT  fighter   -   TAB: human or CPU", 24, PLAYER_TAGS[1], Vec2(SCREEN_WIDTH - 260, 488));
    const Stage& st = stages[size_t(stageSel)];
    text(scr, "VS", 70, WHITE, Vec2(SCREEN_WIDTH / 2, 160));
    const Surface& thumb = *thumbs[size_t(stageSel)];
    Rect tr = thumb.rect();
    tr.setCenter(SCREEN_WIDTH / 2, 320);
    scr.blit(thumb, tr.x, tr.y);
    draw::rect(scr, GOLD, tr, 3, 4);
    text(scr, "<  " + st.name + "  >", 30, GOLD, Vec2(SCREEN_WIDTH / 2, 218));
    text(scr, "W / S  change stage", 20, Color(190, 180, 240), Vec2(SCREEN_WIDTH / 2, 418));
    for (size_t i = 0; i < st.blurb.size(); ++i)
        text(scr, st.blurb[i], 20, Color(220, 220, 240), Vec2(SCREEN_WIDTH / 2, 440 + int(i) * 20), "center", false);
    KeyMap km = slotKeymap(0);
    auto n = [&](const char* a) { return km[a].empty() ? std::string("?") : keyName(km[a][0]); };
    text(scr, "P1:  " + n("left") + "/" + n("right") + " move   " + n("jump") + " jump   " + n("attack") + " attack   " + n("heavy") +
                  " heavy   " + n("special") + " special   " + n("shield") + " shield   " + n("grab") + " grab",
         23, WHITE, Vec2(SCREEN_WIDTH / 2, 540));
    text(scr, "Hold a direction with an attack button for a different move.   ESC in a match = pause + full controls.", 21,
         Color(190, 200, 240), Vec2(SCREEN_WIDTH / 2, 568));
    text(scr, "Knock your rival off the screen. The more damage (%) they have, the further they fly. 3 lives each.", 21,
         Color(190, 200, 240), Vec2(SCREEN_WIDTH / 2, 594));
    if ((t / 30) % 2) text(scr, "PRESS ENTER TO CLASH", 44, GOLD, Vec2(SCREEN_WIDTH / 2, 650));
    text(scr, "F2: make Player 1 a CPU (watch mode)     ESC: back", 18, Color(150, 150, 200), Vec2(SCREEN_WIDTH / 2, 700), "center", false);
}

void Game::renderResult() {
    Surface& scr = screen;
    Surface dim(SCREEN_WIDTH, SCREEN_HEIGHT, true);
    dim.fill(Color(0, 0, 20, 190));
    scr.blit(dim, 0, 0);
    if (Fighter* w = winner) {
        auto spr = quickSprite(*w->spec, w->colors, "idle", 1, double(frame), 2);
        scr.blit(*spr, SCREEN_WIDTH / 2 - SPR_CX * 2, 640 - SPR_FEET * 2);
        text(scr, w->playerName + " WINS!", 110, w->colors.accent, Vec2(SCREEN_WIDTH / 2, 100));
        text(scr, w->name + ", " + w->spec->title + "  -  " + std::to_string(w->currentLives) + " lives left", 30, WHITE,
             Vec2(SCREEN_WIDTH / 2, 170));
    } else {
        text(scr, "DRAW!", 110, WHITE, Vec2(SCREEN_WIDTH / 2, 100));
    }
    text(scr, netmatch ? "ENTER: back to the lobby" : "ENTER: rematch        ESC: character select", 32, GOLD, Vec2(SCREEN_WIDTH / 2, 680));
}
