// Turning input into actions: the player controller and the CPU opponent.
#include <SDL.h>

#include "core.h"

const std::vector<std::pair<std::string, Button>> BUTTONS = {
    {"jump", B_JUMP}, {"attack", B_ATTACK}, {"heavy", B_HEAVY}, {"special", B_SPECIAL}, {"shield", B_SHIELD}, {"grab", B_GRAB}};
const std::vector<std::pair<std::string, std::string>> ACTIONS = {
    {"left", "Move left"}, {"right", "Move right"}, {"up", "Up / aim up"}, {"down", "Down / drop"}, {"jump", "Jump"},
    {"attack", "Attack"}, {"heavy", "Heavy attack"}, {"special", "Special"}, {"shield", "Shield / parry"}, {"grab", "Grab"}};

KeyMap defaultKeymap(int slot) {
    if (slot == 0)
        return {{"left", {SDLK_a}}, {"right", {SDLK_d}}, {"up", {SDLK_w}}, {"down", {SDLK_s}}, {"jump", {SDLK_SPACE}},
                {"attack", {SDLK_j}}, {"heavy", {SDLK_k}}, {"special", {SDLK_l}}, {"shield", {SDLK_LSHIFT, SDLK_i}}, {"grab", {SDLK_h}}};
    return {{"left", {SDLK_LEFT}}, {"right", {SDLK_RIGHT}}, {"up", {SDLK_UP}}, {"down", {SDLK_DOWN}},
            {"jump", {SDLK_RCTRL, SDLK_KP_0}}, {"attack", {SDLK_COMMA, SDLK_KP_1}}, {"heavy", {SDLK_PERIOD, SDLK_KP_2}},
            {"special", {SDLK_SLASH, SDLK_KP_3}}, {"shield", {SDLK_RSHIFT, SDLK_KP_ENTER}}, {"grab", {SDLK_m, SDLK_KP_4}}};
}

// ===================================================================== player controller
bool CharacterController::take(Button b) {
    auto it = buffer.find(b);
    if (it != buffer.end() && it->second > 0) { it->second = 0; return true; }
    return false;
}

void CharacterController::processInput(const InputState& inp) {
    Fighter& f = *c;
    f.inX = inp.x; f.inY = inp.y;
    f.shieldHeld = inp.held & B_SHIELD;
    f.jumpHeld = inp.held & B_JUMP;
    for (auto& [b, v] : buffer) v -= 1;
    for (auto& [name, bit] : BUTTONS) if (inp.pressed & bit) buffer[bit] = BUFFER_FRAMES;
    int prev = prevX;
    prevX = inp.x;
    std::string st = f.state;
    if (st == "KO" || f.hitstop > 0) return;
    if (st == "RESPAWN") {
        if (inp.x || inp.y || inp.pressed) { f.state = "JUMPING"; f.intangible = 100; }
        return;
    }
    if (st == "GRABBING") {                              // holding someone: Attack = pommel, direction = throw
        f.moveX = 0;
        if (f.pommelCd == 0 && take(B_ATTACK)) combat->pommel(f);
        else if (f.grabFrames >= GRAB_MIN_HOLD && (inp.x || inp.y)) combat->throwHeld(f, inp.x, inp.y);
        return;
    }
    if (st == "HIT" || st == "STUN" || st == "DODGE" || st == "GRABBED" || f.lag > 0) { f.moveX = 0; return; }
    f.moveX = inp.x;
    if (inp.downTap && !f.ground && f.vy > 0 && st != "LEDGE") f.fastFall = true;
    if (f.jumpCut && (f.vy >= 0 || f.ground)) f.jumpCut = false;
    if (f.jumpCut && !(inp.held & B_JUMP)) { f.vy *= 0.45; f.jumpCut = false; }
    if (st == "ATTACKING" || st == "HELPLESS" || st == "JUMPSQUAT") return;   // (buttons pressed now are buffered)
    if (st == "LEDGE") { ledgeOptions(inp); return; }
    if (st == "SHIELD") { shieldOptions(inp, prev); return; }
    // ---------------- free to act: IDLE / RUNNING / JUMPING / PARRY_SUCCESS ----------------
    bool grounded = f.ground != nullptr;
    if (grounded) {
        if (inp.x) { f.runFrames = f.facing == inp.x ? f.runFrames + 1 : 0; f.facing = inp.x; }
        else f.runFrames = 0;
        if (st != "PARRY_SUCCESS") f.state = inp.x ? "RUNNING" : "IDLE";
    }
    if (grounded && take(B_GRAB)) { combat->processAttack(f, "grab"); return; }
    if (take(B_SPECIAL)) {
        if (!inp.x && !inp.y && f.isCryptic() && f.aether >= AETHER_MAX) { combat->requestDeathRay(f); return; }
        if (inp.x) f.facing = inp.x;
        std::string slot = inp.y < 0 ? "uspecial" : inp.y > 0 ? "dspecial" : inp.x ? "sspecial" : "nspecial";
        combat->processAttack(f, slot);
        return;
    }
    bool heavy = take(B_HEAVY);
    if (heavy || take(B_ATTACK)) {
        // out of a dash or run, straight away - pressing a direction and the button together still gives a tilt
        bool running = grounded && f.state == "RUNNING" && inp.x == f.facing && inp.y == 0 && f.runFrames >= 3 &&
                       (f.dashT > 0 || std::abs(f.vx) >= f.walkSpeed * 0.8);
        std::string slot;
        if (grounded) {
            if (f.shieldHeld && !heavy) slot = "grab";
            else if (running) slot = heavy ? "dashheavy" : "dash";
            else if (heavy) slot = inp.y < 0 ? "usmash" : inp.y > 0 ? "dsmash" : "fsmash";
            else if (inp.y < 0) slot = "utilt";
            else if (inp.y > 0) slot = "dtilt";
            else if (inp.x) slot = "ftilt";
            else slot = "jab";
        } else {
            slot = inp.y < 0 ? "uair" : inp.y > 0 ? "dair" : inp.x == f.facing ? "fair" : inp.x == -f.facing ? "bair" : "nair";
        }
        combat->processAttack(f, slot);
        return;
    }
    if (take(B_JUMP)) { jump(); return; }
    if (grounded) {
        if (f.shieldHeld && f.shieldHp > 4) { f.state = "SHIELD"; f.shieldFrame = 0; }
        else if (inp.downTap && !f.ground->solid) { f.ground = nullptr; f.dropTimer = 10; f.vy = 1.0; }   // drop through
    } else if ((inp.pressed & B_SHIELD) && !f.airDodged) {
        dodge(inp.x, inp.y, true);
    }
}

void CharacterController::jump() {
    Fighter& f = *c;
    if (f.ground) { f.state = "JUMPSQUAT"; f.squat = f.jumpSquat; return; }   // crouch first (see launchJump)
    if (f.state == "LEDGE") f.vy = -f.jumpHeight;
    else if (f.jumpsLeft > 0) {
        f.jumpsLeft -= 1;
        f.hopT = 0;
        f.vy = -f.doubleJump;
        if (f.inX) f.vx = f.inX * f.airSpeed * 0.8;
        combat->fx->ring(f.x, f.y, f.colors.accent, 10, 4, 10);
    } else return;
    f.ground = nullptr; f.fastFall = false;
    f.squash = 0.2;
    f.state = "JUMPING";
}

void CharacterController::dodge(int x, int y, bool air) {
    Fighter& f = *c;
    f.state = "DODGE";
    f.intangible = std::max(f.intangible, 16);
    if (air) {
        f.hopT = 0;
        f.airDodged = true; f.dodgeTimer = 26;
        double n = std::hypot(x, y);
        if (n) { f.vx = x / n * 10; f.vy = y / n * 10; }
    } else {
        f.dodgeTimer = x ? 24 : 20;
        f.vx = x * 11;
    }
}

void CharacterController::shieldOptions(const InputState& inp, int prev) {
    Fighter& f = *c;
    if (f.shieldStun > 0) return;
    if (take(B_ATTACK) || take(B_GRAB)) combat->processAttack(f, "grab");
    else if (take(B_JUMP)) jump();
    else if (inp.downTap) dodge(0, 0);                   // spot dodge
    else if (inp.x && !prev) dodge(inp.x, 0);            // roll
    else if (!f.shieldHeld) { f.state = "IDLE"; f.lag = 5; }
}

void CharacterController::ledgeOptions(const InputState& inp) {
    Fighter& f = *c;
    Platform* p = f.ledge.p;
    double lx = f.ledge.lx;
    int inward = f.ledge.inward;
    if (f.ledgeTimer < 8) return;
    if (take(B_JUMP)) { jump(); f.ledgeCd = 30; f.ledge = Ledge(); }
    else if (inp.y < 0 || inp.x == inward || take(B_ATTACK)) {     // climb up
        f.x = lx + inward * (f.w / 2.0 + 8); f.y = p->y; f.ground = p;
        f.state = "IDLE"; f.lag = 10; f.ledge = Ledge();
        f.intangible = std::max(f.intangible, 14);
    } else if (inp.y > 0 || inp.x == -inward) {
        f.state = "JUMPING"; f.ledgeCd = 30; f.ledge = Ledge();     // let go
    }
}

void CharacterController::updateState() {
    if (c->state != "HIT" && c->state != "STUN") c->knockedDown = false;
}

// ===================================================================== CPU
namespace {
struct Level { int think; double guard, aggro; };
Level levelOf(int l) {
    if (l <= 1) return {30, 0.02, 0.45};
    if (l == 2) return {16, 0.09, 0.7};
    return {8, 0.22, 0.9};
}
}  // namespace

void AIController::press(InputState& inp, const std::string& slot, int toward) {
    Fighter& f = *c;
    if (slot == "grab") { inp.pressed |= B_GRAB; return; }
    uint8_t kind = slot.find("special") != std::string::npos ? B_SPECIAL
                   : (slot.find("smash") != std::string::npos || slot == "dashheavy") ? B_HEAVY : B_ATTACK;
    if (slot[0] == 'u') inp.y = -1;
    else if (slot[0] == 'd' && slot != "dash" && slot != "dashheavy") inp.y = 1;
    else if (slot == "ftilt" || slot == "fsmash" || slot == "sspecial" || slot == "dash" || slot == "dashheavy") inp.x = toward;
    else if (slot == "fair") inp.x = f.facing;
    else if (slot == "bair") inp.x = -f.facing;
    else {
        inp.x = 0;
        if (f.facing != toward && f.ground) {            // turn first, act next frame
            inp.x = toward;
            queue.push_back({0, 0, kind});
            return;
        }
    }
    inp.pressed |= kind;
}

InputState AIController::readInput() {
    InputState inp;
    Fighter& f = *c;
    Stage& st = *physics->map;
    Rng& R = aiRng();
    if (jumpHold > 0) { jumpHold -= 1; inp.held |= B_JUMP; }
    if (!queue.empty()) {
        Q q = queue.front();
        queue.pop_front();
        inp.x = q.x; inp.y = q.y;
        if (q.button) inp.pressed |= q.button;
        return inp;
    }
    std::vector<Fighter*> others;
    for (Fighter* o : physics->fighters) if (o != &f && o->alive()) others.push_back(o);
    Level lv = levelOf(level);
    cool -= 1;
    double L = st.left, Rt = st.right, top = st.top;
    const std::string state = f.state;
    if (state == "GRABBING") {                           // pommel a few times, then throw somewhere nasty
        Fighter* t = f.grabTarget;
        if (f.grabFrames < 2) pommels = R.randint(0, 3);
        if (f.grabFrames < GRAB_MIN_HOLD + 3 || !t) return inp;
        if (pommels > 0) {
            if (f.pommelCd == 0) { inp.pressed |= B_ATTACK; pommels -= 1; }
            return inp;
        }
        int edge = f.x < (L + Rt) / 2 ? -1 : 1;
        std::vector<std::pair<int, int>> opts = t->damage > 110 ? std::vector<std::pair<int, int>>{{edge, 0}, {0, -1}}
                                                                  : std::vector<std::pair<int, int>>{{edge, 0}, {-edge, 0}, {0, -1}, {0, 1}};
        auto o = R.choice(opts);
        inp.x = o.first; inp.y = o.second;
        return inp;
    }
    if (state == "LEDGE") {
        wait += 1;
        if (wait > 14) { inp.y = -1; wait = 0; }
        return inp;
    }
    if (state == "RESPAWN") {
        if (f.stateTimer < 110) inp.y = 1;
        return inp;
    }
    // ---------------- recovery ----------------
    if (!f.ground && (f.x < L - 5 || f.x > Rt + 5 || f.y > top + 12)) {
        double tx = clampv(f.x, L + 60, Rt - 60);
        if (f.y > top + 10 && L - 40 < f.x && f.x < Rt + 40) inp.x = f.x < (L + Rt) / 2 ? -1 : 1;   // under the stage
        else inp.x = sgn(tx - f.x);
        if (state == "JUMPING" && f.vy >= 0) {
            bool far = std::abs(tx - f.x) > 220;
            if (f.jumpsLeft > 0 && (f.y > top - 40 || far)) { inp.pressed |= B_JUMP; jumpHold = 16; }
            else if (f.jumpsLeft == 0 && (f.y > top + 50 || std::abs(tx - f.x) > 330)) { inp.y = -1; inp.pressed |= B_SPECIAL; }
        }
        return inp;
    }
    if (others.empty() || state == "HIT" || state == "STUN" || state == "ATTACKING" || state == "DODGE" || state == "HELPLESS")
        return inp;
    Fighter& o = *others[0];
    double dx = o.x - f.x, dy = o.y - f.y;
    double adx = std::abs(dx);
    int toward = sgn(dx) ? sgn(dx) : 1;
    bool oOff = o.x < L - 20 || o.x > Rt + 20 || o.state == "RESPAWN";
    bool grounded = f.ground != nullptr;
    // ---------------- defence ----------------
    if (shieldHold > 0) {
        shieldHold -= 1;
        inp.held |= B_SHIELD;
        if (o.state == "ATTACKING" || shieldHold > 6) return inp;
    }
    if (grounded && o.state == "ATTACKING" && adx < 190 && std::abs(dy) < 130 && R.random() < lv.guard) {
        if (f.name == "Bronn" && R.random() < 0.4) press(inp, "nspecial", toward);
        else if (R.random() < 0.25) { inp.held |= B_SHIELD; inp.x = -toward; queue.push_back({-toward, 0, 0}); }   // roll away
        else { shieldHold = 14; inp.held |= B_SHIELD; }
        return inp;
    }
    // ---------------- movement ----------------
    double reach = 60 + (f.w + o.w) / 2.0;
    double targetX = oOff ? clampv(o.x, L + 50, Rt - 50) : o.x;
    if (std::abs(targetX - f.x) > (oOff ? 20 : reach)) inp.x = sgn(targetX - f.x);
    if (grounded) {
        if (f.ground->solid && ((f.x < L + 36 && inp.x < 0) || (f.x > Rt - 36 && inp.x > 0))) inp.x = 0;
        if (dy < -90 && adx < 260 && R.random() < 0.06) { inp.pressed |= B_JUMP; jumpHold = 14; }
        else if (dy > 60 && !f.ground->solid && R.random() < 0.08) { inp.downTap = true; inp.y = 1; }
    } else if (dy < -120 && f.vy > 0 && f.jumpsLeft > 0 && adx < 200 && R.random() < 0.05) {
        inp.pressed |= B_JUMP; jumpHold = 12;
    }
    // ---------------- offence ----------------
    if (cool > 0 || !(state == "IDLE" || state == "RUNNING" || state == "JUMPING" || state == "PARRY_SUCCESS") || f.lag > 0)
        return inp;
    if (o.intangible > 30 || R.random() > lv.aggro) { cool = lv.think / 2; return inp; }
    if (f.isCryptic() && f.aether >= AETHER_MAX && grounded) {
        inp.x = inp.y = 0;
        inp.pressed |= B_SPECIAL;
        cool = lv.think;
        return inp;
    }
    double dmg = o.damage;
    auto afford = [&](const std::string& s) { return f.aether >= f.moveSet->at(s).requiredAether; };
    std::string choice;
    if (grounded) {
        if (adx < reach + 20 && std::abs(dy) < 70) {
            if (o.state == "SHIELD" && R.random() < 0.7) choice = "grab";
            else {
                std::vector<std::string> pool = {"jab", "jab", "ftilt", "ftilt", "dtilt", "utilt", "grab", "fsmash", "dsmash"};
                if (dmg > 90) for (int k = 0; k < 1 + int(dmg > 140) * 2; ++k) for (auto s : {"fsmash", "fsmash", "usmash"}) pool.push_back(s);
                for (auto& s : f.spec->aiClose)
                    if ((afford(s) && s != "nspecial") || (s == "nspecial" && f.name != "Bronn" && afford(s))) pool.push_back(s);
                choice = R.choice(pool);
            }
        } else if (dy < -60 && adx < 100) {
            choice = dy > -170 ? R.choice(std::vector<std::string>{"utilt", "usmash", "utilt"}) : "jump";
        } else if (adx < 520 && std::abs(dy) < 150) {
            std::vector<std::string> pool;
            for (auto& s : f.spec->aiMid) if (afford(s)) pool.push_back(s);
            if (f.name == "Rhea" && f.buffTimer > 0) pool.erase(std::remove(pool.begin(), pool.end(), "dspecial"), pool.end());
            if (!pool.empty() && R.random() < 0.45) choice = R.choice(pool);
            else if (adx < 300 && f.runFrames > 8 && std::abs(f.vx) >= f.speed * 0.6)
                choice = (dmg > 100 && R.random() < 0.6) ? "dashheavy" : "dash";
        }
    } else if (adx < reach + 40 && std::abs(dy) < 120) {
        bool safe = L < f.x && f.x < Rt;
        if (dy < -50) choice = "uair";
        else if (dy > 60) choice = safe ? "dair" : "nair";
        else choice = R.choice(std::vector<std::string>{"nair", toward == f.facing ? "fair" : "bair"});
    }
    if (choice == "jump") {
        inp.pressed |= B_JUMP;
        jumpHold = 14;
        for (int k = 0; k < 8; ++k) queue.push_back({toward, 0, 0});
        queue.push_back({0, -1, B_ATTACK});
    } else if (!choice.empty()) {
        press(inp, choice, toward);
    }
    cool = !choice.empty() ? lv.think + R.randint(0, lv.think) : 4;
    return inp;
}
