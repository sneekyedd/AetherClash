// Online play (the lobby client), a tiny HTTP client, SHA-256 and the auto-updater.
#include "net.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
using SockT = SOCKET;
static const SockT BAD_SOCK = INVALID_SOCKET;
static void closeSock(SockT s) { closesocket(s); }
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
using SockT = int;
static const SockT BAD_SOCK = -1;
static void closeSock(SockT s) { ::close(s); }
#endif

#ifndef AETHER_VERSION
#define AETHER_VERSION "dev"
#endif
#ifndef AETHER_SERVER
#define AETHER_SERVER ""
#endif

const std::string& gameVersion() {
    static std::string v = [] {
        std::string s = AETHER_VERSION;
        while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
        return s.empty() ? std::string("dev") : s;
    }();
    return v;
}
const std::string& defaultServer() { static std::string s = AETHER_SERVER; return s; }

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::optional<std::pair<std::string, int>> splitAddress(const std::string& text) {
    std::string t = trim(text);
    size_t c = t.rfind(':');
    std::string host, port;
    if (c == std::string::npos || c == 0) { host = c == 0 ? "" : t; port = std::to_string(DEFAULT_PORT); }
    else { host = t.substr(0, c); port = t.substr(c + 1); }
    if (host.empty() || port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return std::make_pair(host, std::atoi(port.c_str()));
}

int encodeInput(const InputState& inp) {
    int n = int(BUTTONS.size());
    int bits = (inp.x + 1) | ((inp.y + 1) << 2) | (int(inp.downTap) << 4);
    for (int i = 0; i < n; ++i) {
        uint8_t b = BUTTONS[i].second;
        bits |= (int((inp.held & b) != 0) << (5 + i)) | (int((inp.pressed & b) != 0) << (5 + n + i));
    }
    return bits;
}

InputState decodeInput(int bits) {
    InputState inp;
    int n = int(BUTTONS.size());
    inp.x = (bits & 3) - 1;
    inp.y = ((bits >> 2) & 3) - 1;
    inp.downTap = (bits >> 4) & 1;
    for (int i = 0; i < n; ++i) {
        if ((bits >> (5 + i)) & 1) inp.held |= BUTTONS[i].second;
        if ((bits >> (5 + n + i)) & 1) inp.pressed |= BUTTONS[i].second;
    }
    return inp;
}

// ===================================================================== sockets
static void netInit() {
#ifdef _WIN32
    static bool done = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    (void)done;
#endif
}

static SockT dial(const std::string& host, int port, int timeoutSec, std::string& err) {
    netInit();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res); rc != 0 || !res) {
        err = "could not find " + host;
        return BAD_SOCK;
    }
    SockT s = BAD_SOCK;
    for (addrinfo* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCK) continue;
#ifdef _WIN32
        DWORD tv = timeoutSec * 1000;
#else
        timeval tv{timeoutSec, 0};
#endif
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        if (::connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closeSock(s);
        s = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) err = "could not connect to " + host + ":" + std::to_string(port);
    return s;
}

static bool sendAll(SockT s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = ::send(s, data.data() + off, (int)(data.size() - off), 0);
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

// ===================================================================== lobby client
struct OnlineClient::Conn {
    std::mutex mu;
    std::deque<Json> inbox;
    std::atomic<int> status{OFFLINE};
    std::string error;
    std::atomic<SockT> sock{BAD_SOCK};
    std::mutex sendMu;
};

OnlineClient::OnlineClient() : c(std::make_shared<Conn>()) {}
OnlineClient::~OnlineClient() { close(); }

void OnlineClient::connect(const std::string& host, int port, const std::string& name) {
    close();
    c = std::make_shared<Conn>();
    c->status = CONNECTING;
    auto conn = c;
    std::thread([conn, host, port, name] {
        std::string err;
        SockT s = dial(host, port, 6, err);
        if (s != BAD_SOCK) {
#ifdef _WIN32
            DWORD tv = 0;
#else
            timeval tv{0, 0};
#endif
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
            conn->sock = s;
            conn->status = ONLINE;
            Json hello = {{"type", "hello"}, {"name", name}, {"version", NET_PROTOCOL_VERSION}, {"game", gameVersion()}};
            sendAll(s, hello.dump() + "\n");
            std::string buf;
            char tmp[4096];
            for (;;) {
                int n = recv(s, tmp, sizeof tmp, 0);
                if (n <= 0) break;
                buf.append(tmp, size_t(n));
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    Json j = Json::parse(line, nullptr, false);
                    if (!j.is_discarded() && j.is_object()) {
                        std::lock_guard<std::mutex> lk(conn->mu);
                        conn->inbox.push_back(std::move(j));
                    }
                }
            }
        } else {
            std::lock_guard<std::mutex> lk(conn->mu);
            conn->error = err;
        }
        SockT old = conn->sock.exchange(BAD_SOCK);
        if (old != BAD_SOCK) closeSock(old);
        conn->status = OFFLINE;
        std::lock_guard<std::mutex> lk(conn->mu);
        conn->inbox.push_back(Json{{"type", "closed"}});
    }).detach();
}

void OnlineClient::send(const Json& msg) {
    SockT s = c->sock;
    if (s == BAD_SOCK) return;
    std::lock_guard<std::mutex> lk(c->sendMu);
    sendAll(s, msg.dump() + "\n");
}

void OnlineClient::sendInput(int frame, int bits) {
    SockT s = c->sock;
    if (s == BAD_SOCK) return;
    char line[64];
    std::snprintf(line, sizeof line, "{\"type\":\"i\",\"f\":%d,\"b\":%d}\n", frame, bits);
    std::lock_guard<std::mutex> lk(c->sendMu);
    sendAll(s, line);
}

std::vector<Json> OnlineClient::poll() {
    std::lock_guard<std::mutex> lk(c->mu);
    std::vector<Json> out(c->inbox.begin(), c->inbox.end());
    c->inbox.clear();
    return out;
}

void OnlineClient::requeue(const std::vector<Json>& msgs) {
    std::lock_guard<std::mutex> lk(c->mu);
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) c->inbox.push_front(*it);
}

void OnlineClient::close() {
    if (!c) return;
    SockT s = c->sock.exchange(BAD_SOCK);
    if (s != BAD_SOCK) {
#ifdef _WIN32
        shutdown(s, SD_BOTH);
#else
        shutdown(s, SHUT_RDWR);
#endif
        closeSock(s);
    }
    c->status = OFFLINE;
}

OnlineClient::Status OnlineClient::status() const { return Status(c->status.load()); }
std::string OnlineClient::error() const { std::lock_guard<std::mutex> lk(c->mu); return c->error; }

// ===================================================================== HTTP
struct Url { std::string scheme, host, path; int port = 80; };
static bool parseUrl(const std::string& u, Url& out) {
    size_t p = u.find("://");
    if (p == std::string::npos) return false;
    out.scheme = u.substr(0, p);
    std::string rest = u.substr(p + 3);
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    out.port = out.scheme == "https" ? 443 : 80;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = std::atoi(hostport.c_str() + colon + 1);
    } else {
        out.host = hostport;
    }
    return !out.host.empty();
}

std::string urlJoin(const std::string& base, const std::string& ref) {
    if (ref.find("://") != std::string::npos) return ref;
    Url b;
    if (!parseUrl(base, b)) return ref;
    std::string origin = b.scheme + "://" + b.host + ((b.scheme == "http" && b.port != 80) || (b.scheme == "https" && b.port != 443)
                                                          ? ":" + std::to_string(b.port) : "");
    if (!ref.empty() && ref[0] == '/') return origin + ref;
    std::string dir = b.path.substr(0, b.path.rfind('/') + 1);
    return origin + dir + ref;
}

static HttpResult httpPlain(const Url& u, int timeoutSec, const std::function<bool(const char*, size_t)>& sink, std::string& redirect,
                            long long* hint) {
    HttpResult r;
    SockT s = dial(u.host, u.port, timeoutSec, r.error);
    if (s == BAD_SOCK) return r;
    std::string req = "GET " + u.path + " HTTP/1.0\r\nHost: " + u.host + "\r\nUser-Agent: AetherClash\r\n"
                      "Accept: application/vnd.github+json\r\nConnection: close\r\n\r\n";
    if (!sendAll(s, req)) { closeSock(s); r.error = "send failed"; return r; }
    std::string head;
    char tmp[1 << 16];
    bool inBody = false;
    long long got = 0;
    for (;;) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        if (inBody) {
            got += n;
            if (!sink(tmp, size_t(n))) { closeSock(s); r.error = "aborted"; return r; }
            continue;
        }
        head.append(tmp, size_t(n));
        size_t e = head.find("\r\n\r\n");
        if (e == std::string::npos) continue;
        std::string body = head.substr(e + 4);
        head.resize(e);
        inBody = true;
        std::sscanf(head.c_str(), "HTTP/%*s %d", &r.status);
        std::string lower = head;
        for (auto& ch : lower) ch = char(tolower((unsigned char)ch));
        size_t cl = lower.find("\r\ncontent-length:");
        if (cl != std::string::npos) r.length = std::atoll(head.c_str() + cl + 17);
        size_t loc = lower.find("\r\nlocation:");
        if (loc != std::string::npos) {
            size_t end = head.find("\r\n", loc + 2);
            redirect = trim(head.substr(loc + 11, end - loc - 11));
        }
        if (r.status >= 300 && r.status < 400 && !redirect.empty()) { closeSock(s); return r; }
        if (r.status != 200) { closeSock(s); r.error = "HTTP " + std::to_string(r.status); return r; }
        if (hint) *hint = r.length;
        if (!body.empty()) {
            got += (long long)body.size();
            if (!sink(body.data(), body.size())) { closeSock(s); r.error = "aborted"; return r; }
        }
    }
    closeSock(s);
    if (!inBody) { r.error = "no response"; return r; }
    r.ok = r.length < 0 || got == r.length;
    if (!r.ok) r.error = "download cut short";
    return r;
}

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static HttpResult httpWin(const Url& u, int timeoutSec, const std::function<bool(const char*, size_t)>& sink, long long* hint) {
    HttpResult r;
    HINTERNET ses = WinHttpOpen(L"AetherClash", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"AetherClash", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { r.error = "WinHttpOpen failed"; return r; }
    int ms = timeoutSec * 1000;
    WinHttpSetTimeouts(ses, ms, ms, ms, ms);
    HINTERNET con = WinHttpConnect(ses, widen(u.host).c_str(), INTERNET_PORT(u.port), 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        r.status = int(code);
        wchar_t lenBuf[64];
        DWORD lsz = sizeof lenBuf;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lsz, WINHTTP_NO_HEADER_INDEX))
            r.length = _wtoi64(lenBuf);
        if (code != 200) { r.error = "HTTP " + std::to_string(code); ok = false; }
        if (hint) *hint = r.length;
    } else {
        r.error = "request failed (" + std::to_string(GetLastError()) + ")";
    }
    long long got = 0;
    while (ok) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; r.error = "read failed"; break; }
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &read)) { ok = false; r.error = "read failed"; break; }
        if (read == 0) break;
        got += read;
        if (!sink(buf.data(), read)) { ok = false; r.error = "aborted"; break; }
    }
    if (ok) {
        r.ok = r.length < 0 || got == r.length;
        if (!r.ok) r.error = "download cut short";
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return r;
}
#endif

HttpResult httpGet(const std::string& url, int timeoutSec, const std::function<bool(const char*, size_t)>& sink, long long* hint) {
    std::string cur = url;
    for (int hops = 0; hops < 6; ++hops) {
        Url u;
        if (!parseUrl(cur, u)) return HttpResult{false, 0, -1, "bad address"};
        if (u.scheme == "https") {
#ifdef _WIN32
            return httpWin(u, timeoutSec, sink, hint);           // (WinHTTP follows redirects itself)
#else
            return HttpResult{false, 0, -1, "https is not supported on this system"};
#endif
        }
        std::string redirect;
        HttpResult r = httpPlain(u, timeoutSec, sink, redirect, hint);
        if (r.status >= 300 && r.status < 400 && !redirect.empty()) {
            cur = urlJoin(cur, redirect);
            continue;
        }
        return r;
    }
    return HttpResult{false, 0, -1, "too many redirects"};
}

// ===================================================================== SHA-256
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

Sha256::Sha256() {
    const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h, init, sizeof h);
}
void Sha256::block(const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
void Sha256::update(const void* data, size_t size) {
    const unsigned char* p = (const unsigned char*)data;
    len += size;
    while (size) {
        size_t k = std::min(size, 64 - n);
        std::memcpy(buf + n, p, k);
        n += k; p += k; size -= k;
        if (n == 64) { block(buf); n = 0; }
    }
}
std::string Sha256::hex() {
    uint64_t bits = len * 8;
    unsigned char pad = 0x80;
    update(&pad, 1);
    unsigned char z = 0;
    while (n != 56) update(&z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    update(lb, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

// ===================================================================== updater
std::string Updater::exePath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    int m = WideCharToMultiByte(CP_UTF8, 0, buf, int(n), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, int(n), s.data(), m, nullptr, nullptr);
    return s;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
#endif
}

static void removeFile(const std::string& p) {
#ifdef _WIN32
    DeleteFileW(widen(p).c_str());
#else
    std::remove(p.c_str());
#endif
}
static bool moveFile(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return MoveFileExW(widen(a).c_str(), widen(b).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(a.c_str(), b.c_str()) == 0;
#endif
}
static std::vector<int> versionNumbers(std::string tag) {
    while (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    std::vector<int> out;
    size_t i = 0;
    while (i <= tag.size()) {
        size_t j = tag.find('.', i);
        if (j == std::string::npos) j = tag.size();
        std::string part = tag.substr(i, j - i);
        if (!part.empty() && part.find_first_not_of("0123456789") == std::string::npos) out.push_back(std::atoi(part.c_str()));
        i = j + 1;
    }
    return out;
}

Updater::Updater(std::vector<std::string> srv, bool allowed) : exe(exePath()) {
    for (auto& s : srv) if (!trim(s).empty()) servers.push_back(trim(s));
    const char* env = std::getenv("AETHER_UPDATE_URL");
#ifdef _WIN32
    enabled = !exe.empty() && (gameVersion() != "dev" || (env && *env));
#else
    enabled = !exe.empty() && env && *env && std::getenv("AETHER_UPDATE_TEST");      // only for testing off Windows
#endif
    enabled = enabled && allowed;
    if (!enabled) return;
    removeFile(exe + ".old");
    removeFile(exe + ".new");
    state = CHECKING;
    setStatus("Checking for updates...");
    std::thread([this] { run(); }).detach();
}

void Updater::setStatus(const std::string& s) { std::lock_guard<std::mutex> lk(mu); statusText = s; }
std::string Updater::status() { std::lock_guard<std::mutex> lk(mu); return statusText; }

void Updater::run() {
    const std::string& ver = gameVersion();
    struct Found { std::string tag, url, sha; };
    std::optional<Found> found;
    std::vector<std::pair<std::string, bool>> feeds;          // (url, is mirror)
    if (const char* env = std::getenv("AETHER_UPDATE_URL"); env && *env) feeds.push_back({env, true});
    for (auto& a : servers)
        if (auto hp = splitAddress(a)) feeds.push_back({"http://" + hp->first + ":" + std::to_string(hp->second) + "/aether/latest.json", true});
    feeds.push_back({std::string("https://api.github.com/repos/") + GITHUB_REPO + "/releases/latest", false});
    for (auto& [url, mirror] : feeds) {
        std::string body;
        HttpResult r = httpGet(url, 4, [&](const char* d, size_t n) { body.append(d, n); return body.size() < (8u << 20); });
        if (!r.ok) continue;
        Json data = Json::parse(body, nullptr, false);
        if (data.is_discarded() || !data.is_object()) continue;
        try {
            if (mirror) {
                found = Found{data.value("tag", ""), urlJoin(url, data.at("url").get<std::string>()),
                              data.contains("sha256") && data["sha256"].is_string() ? data["sha256"].get<std::string>() : ""};
            } else {
                for (auto& a : data.at("assets")) {
                    std::string name = a.value("name", "");
                    std::string lower = name;
                    for (auto& ch : lower) ch = char(tolower((unsigned char)ch));
                    if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".exe") continue;
                    std::string digest = a.contains("digest") && a["digest"].is_string() ? a["digest"].get<std::string>() : "";
                    found = Found{data.value("tag_name", ""), a.at("browser_download_url").get<std::string>(),
                                  digest.rfind("sha256:", 0) == 0 ? digest.substr(7) : ""};
                    break;
                }
            }
        } catch (...) {
            found.reset();
        }
        if (found) break;
    }
    if (!found) {
        setStatus(ver + "  (could not check for updates)");
        state = DONE;
        return;
    }
    if (versionNumbers(found->tag) <= versionNumbers(ver)) {
        setStatus("Up to date  (" + ver + ")");
        state = DONE;
        return;
    }
    tag = found->tag;
    setStatus("Downloading update " + tag + "...");
    state = DOWNLOADING;
    std::string newPath = exe + ".new";
    std::ofstream out(newPath, std::ios::binary);
    if (!out) {
        setStatus(ver + "  (update failed; try again later)");
        state = DONE;
        return;
    }
    Sha256 digest;
    long long done = 0, total = -1;
    long long hint = -1;
    HttpResult r = httpGet(found->url, 20, [&](const char* d, size_t n) {
        out.write(d, std::streamsize(n));
        digest.update(d, n);
        done += (long long)n;
        if (hint > 0) {
            progress = double(done) / double(hint);
            setStatus("Downloading update " + tag + "...  " + std::to_string(done * 100 / hint) + "%");
        }
        return bool(out);
    }, &hint);
    total = r.length;
    if (total > 0) {
        progress = double(done) / double(total);
        setStatus("Downloading update " + tag + "...  " + std::to_string(done * 100 / total) + "%");
    }
    out.close();
    std::string sha = found->sha;
    for (auto& ch : sha) ch = char(tolower((unsigned char)ch));
    if (!r.ok || (!sha.empty() && digest.hex() != sha) || (total > 0 && done != total) || done < 100000) {
        removeFile(newPath);
        setStatus(ver + "  (the update download was damaged; try again later)");
        state = DONE;
        return;
    }
    progress = 1.0;
    setStatus("Installing " + tag + "...");
    state = READY;
}

bool Updater::apply() {
    std::string old = exe + ".old", fresh = exe + ".new";
    if (moveFile(exe, old) && moveFile(fresh, exe)) {
#ifdef _WIN32
        std::wstring cmd = L"\"" + widen(exe) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(widen(exe).c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
#else
        chmod(exe.c_str(), 0755);
        if (fork() == 0) {
            execl(exe.c_str(), exe.c_str(), (char*)nullptr);
            _exit(1);
        }
        return true;
#endif
    }
    if (!std::ifstream(exe).good()) moveFile(old, exe);                // put the old copy back
    setStatus(gameVersion() + "  (could not install the update)");
    state = DONE;
    return false;
}
