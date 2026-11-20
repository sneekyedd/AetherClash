// AETHER CLASH - online play (lobby client + lockstep bookkeeping) and the auto-updater.
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core.h"
#include "nlohmann/json.hpp"

using Json = nlohmann::json;

constexpr int NET_PROTOCOL_VERSION = 2;
constexpr int DEFAULT_PORT = 7777;
constexpr int INPUT_DELAY = 3;                 // frames between pressing a button and it taking effect online
inline const char* GITHUB_REPO = "sneekyedd/AetherClash";

const std::string& gameVersion();              // the release this copy was built from ("dev" otherwise)
const std::string& defaultServer();            // the lobby server baked into the build (may be empty)
std::optional<std::pair<std::string, int>> splitAddress(const std::string& text);

int encodeInput(const InputState& inp);
InputState decodeInput(int bits);

struct QueuedDevice : InputDevice {           // plays back the agreed input for the current frame
    KeyMap keymap;
    int bits;
    explicit QueuedDevice(KeyMap km) : keymap(std::move(km)), bits(encodeInput(InputState())) {}
    InputState poll() override { return decodeInput(bits); }
};

struct NetMatch {                              // lockstep: frame N runs once both players' inputs for N exist
    int local, frame = 0, sent = INPUT_DELAY - 1;
    std::map<int, int> inputs[2];
    std::map<int, int> sums;
    int stall = 0;
    bool desync = false;
    explicit NetMatch(int localSlot) : local(localSlot) {
        int neutral = encodeInput(InputState());
        for (auto& m : inputs)
            for (int f = 0; f < INPUT_DELAY; ++f) m[f] = neutral;
    }
};

// The connection to aether_server.py. Reading happens on a background thread so the game never freezes.
class OnlineClient {
public:
    enum Status { OFFLINE, CONNECTING, ONLINE };
    OnlineClient();
    ~OnlineClient();
    void connect(const std::string& host, int port, const std::string& name);
    void send(const Json& msg);
    void sendInput(int frame, int bits);
    std::vector<Json> poll();
    void requeue(const std::vector<Json>& msgs);     // hand messages back for the next poll
    void close();
    Status status() const;
    std::string error() const;
private:
    struct Conn;
    std::shared_ptr<Conn> c;
};

// ---- small HTTP client (plain http everywhere, https through WinHTTP on Windows)
struct HttpResult { bool ok = false; int status = 0; long long length = -1; std::string error; };
HttpResult httpGet(const std::string& url, int timeoutSec, const std::function<bool(const char*, size_t)>& sink,
                   long long* lengthHint = nullptr);
std::string urlJoin(const std::string& base, const std::string& ref);

struct Sha256 {
    uint32_t h[8];
    uint64_t len = 0;
    unsigned char buf[64];
    size_t n = 0;
    Sha256();
    void update(const void* data, size_t size);
    std::string hex();
private:
    void block(const unsigned char* p);
};

// Keeps the packaged game up to date (see the Python version for the full story).
class Updater {
public:
    enum State { OFF, CHECKING, DOWNLOADING, READY, DONE, SKIPPED };
    explicit Updater(std::vector<std::string> servers);     // (create with new and keep for the whole run)
    bool enabled = false;
    std::atomic<int> state{OFF};
    std::atomic<double> progress{0.0};
    std::string status();
    bool apply();
    static std::string exePath();
private:
    std::vector<std::string> servers;
    std::string exe, tag, statusText;
    std::mutex mu;
    void setStatus(const std::string& s);
    void run();
};
