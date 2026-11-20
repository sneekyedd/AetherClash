// AETHER CLASH - entry point.
//   AetherClash               play
//   AetherClash --demo        straight to character select, CPU vs CPU
//   AetherClash --selftest    headless CPU matches on every stage (and the secret duel), then every screen
//   AetherClash --make-icon out.png
#include <cstdlib>
#include <cstring>
#include <string>

#include "game.h"

int main(int argc, char** argv) {
    auto has = [&](const char* flag) {
        for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], flag) == 0) return i;
        return 0;
    };
    if (int i = has("--make-icon")) {
        auto icon = makeIcon(256);
        return savePNG(*icon, i + 1 < argc ? argv[i + 1] : "icon.png") ? 0 : 1;
    }
    if (has("--selftest")) {
        Game game(true, false, has("--profiles") ? argv[has("--profiles") + 1] : "selftest_profiles.json");
        game.selftest();
        return 0;
    }
    if (int i = has("--nettest")) {                    // --nettest host:port host|join [frames]
        Game game(true, false, "nettest_profiles.json");
        return game.nettest(argv[i + 1], std::string(argv[i + 2]) == "host", i + 3 < argc ? std::atoi(argv[i + 3]) : 1500);
    }
    Game game(false, has("--demo") != 0);
    game.run();
    return 0;
}
