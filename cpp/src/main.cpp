// AETHER CLASH - entry point.
//   AetherClash               play
//   AetherClash --demo        straight to character select, CPU vs CPU
//   AetherClash --selftest    headless CPU matches on every stage (and the secret duel), then every screen
//   AetherClash --smoke 300   open the window, run 300 frames, close (a test)
//   AetherClash --nettest host:port host|join   two copies play each other online (a test)
//   AetherClash --make-icon out.png|out.ico
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "game.h"

// An .ico file holding PNG images (supported since Windows Vista).
static bool writeIco(const std::string& path) {
    const int sizes[] = {256, 128, 64, 48, 32, 16};
    std::vector<std::vector<unsigned char>> pngs;
    for (int s : sizes) pngs.push_back(encodePNG(*makeIcon(s)));
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u16 = [&](unsigned v) { std::fputc(int(v & 255), f); std::fputc(int(v >> 8), f); };
    auto u32 = [&](unsigned v) { u16(v & 0xFFFF); u16(v >> 16); };
    u16(0); u16(1); u16(unsigned(pngs.size()));
    unsigned offset = 6 + 16 * unsigned(pngs.size());
    for (size_t i = 0; i < pngs.size(); ++i) {
        int s = sizes[i];
        std::fputc(s >= 256 ? 0 : s, f); std::fputc(s >= 256 ? 0 : s, f);
        std::fputc(0, f); std::fputc(0, f);
        u16(1); u16(32);
        u32(unsigned(pngs[i].size())); u32(offset);
        offset += unsigned(pngs[i].size());
    }
    for (auto& p : pngs) std::fwrite(p.data(), 1, p.size(), f);
    return std::fclose(f) == 0;
}

int main(int argc, char** argv) {
    auto has = [&](const char* flag) {
        for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], flag) == 0) return i;
        return 0;
    };
    if (int i = has("--make-icon")) {                  // .png, or .ico (several sizes) for the Windows build
        std::string out = i + 1 < argc ? argv[i + 1] : "icon.png";
        if (out.size() > 4 && out.substr(out.size() - 4) == ".ico") return writeIco(out) ? 0 : 1;
        return savePNG(*makeIcon(256), out) ? 0 : 1;
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
    if (int i = has("--smoke")) {                      // --smoke N: open the window, run N frames, close
        Game game(false, true, "smoke_profiles.json", false);
        game.maxLoops = i + 1 < argc ? std::atol(argv[i + 1]) : 300;
        game.setupGame(0, 1, 0, 2, true);                // CPU vs CPU, so a real match is drawn on screen
        game.run();
        std::printf("WINDOW OK\n");
        return 0;
    }
    Game game(false, has("--demo") != 0);
    game.run();
    return 0;
}
