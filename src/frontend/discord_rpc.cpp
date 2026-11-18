// Discord Rich Presence: talks to the Discord desktop client over its local IPC endpoint (Windows named pipe
// \\.\pipe\discord-ipc-N, a Unix socket discord-ipc-N elsewhere), so no SDK is shipped. Frames are
// {u32 opcode, u32 length} little-endian + JSON: 0 handshake, 1 frame, 2 close, 3 ping, 4 pong.
// The pipe lives on its own thread: connecting, waiting for Discord's answer or a full pipe never stall the UI.
// What is shown comes from game RAM (see docs/ARCHITECTURE.md, "Discord Rich Presence").
#include "discord_rpc.h"
#include "platform.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include "psx.h"
const char *rt_current_overlay_name(void);
int autoplay_active(void);
int arcade_stage1_active_slot(void);
}

#if UJL_IOS
// No Discord client on iOS.
void discord_rpc_configure(bool, const std::string &) {}
void discord_rpc_frame(bool) {}
std::string discord_rpc_status() { return "Not available on iOS"; }
void discord_rpc_shutdown() {}
void discord_rpc_describe(bool, std::string &details, std::string &state) { details.clear(); state.clear(); }
#else

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- transport
class Pipe {
public:
    ~Pipe() { close(); }
    bool open() {
        close();
#ifdef _WIN32
        for (int i = 0; i < 10; i++) {
            char name[64];
            snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
            h_ = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h_ != INVALID_HANDLE_VALUE) return true;
        }
        h_ = INVALID_HANDLE_VALUE;
        return false;
#else
        // Discord puts its socket in the runtime/temp folder; Flatpak and Snap builds one level down.
        const char *bases[] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp"};
        const char *subs[] = {"", "app/com.discordapp.Discord/", "snap.discord/", ".flatpak/com.discordapp.Discord/xdg-run/"};
        for (const char *base : bases) {
            if (!base || !*base) continue;
            for (const char *sub : subs)
                for (int i = 0; i < 10; i++) {
                    sockaddr_un addr{};
                    addr.sun_family = AF_UNIX;
                    std::string b = base;
                    if (b.back() != '/') b += '/';
                    std::string path = b + sub + "discord-ipc-" + std::to_string(i);
                    if (path.size() >= sizeof addr.sun_path) continue;
                    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
                    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                    if (fd < 0) return false;
#ifdef SO_NOSIGPIPE
                    int one = 1;
                    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                    if (connect(fd, (sockaddr *)&addr, sizeof addr) == 0) { fd_ = fd; return true; }
                    ::close(fd);
                }
        }
        return false;
#endif
    }
    void close() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
    }
    bool is_open() const {
#ifdef _WIN32
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }
    bool write_all(const void *data, size_t n) {
        const char *p = (const char *)data;
        while (n) {
#ifdef _WIN32
            DWORD done = 0;
            if (!WriteFile(h_, p, (DWORD)n, &done, nullptr) || !done) return false;
#else
#ifdef MSG_NOSIGNAL
            ssize_t done = send(fd_, p, n, MSG_NOSIGNAL);
#else
            ssize_t done = send(fd_, p, n, 0);
#endif
            if (done < 0 && errno == EINTR) continue;
            if (done <= 0) return false;
#endif
            p += done;
            n -= (size_t)done;
        }
        return true;
    }
    // Bytes waiting to be read without blocking; -1 when the other end is gone.
    long available() {
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return -1;
        return (long)avail;
#else
        pollfd pfd{fd_, POLLIN, 0};
        int r = poll(&pfd, 1, 0);
        if (r < 0) return errno == EINTR ? 0 : -1;
        if (!r) return 0;
        if (pfd.revents & (POLLERR | POLLNVAL)) return -1;
        char c;
        ssize_t n = recv(fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) return -1;   // hung up
        return n > 0 ? 1 : (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1);
#endif
    }
    // Reads exactly n bytes, giving up after timeout_ms.
    bool read_exact(void *data, size_t n, int timeout_ms) {
        char *p = (char *)data;
        auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (n) {
#ifdef _WIN32
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (!avail) {
                if (Clock::now() >= end) return false;
                Sleep(10);
                continue;
            }
            DWORD got = 0;
            if (!ReadFile(h_, p, (DWORD)(avail < n ? avail : n), &got, nullptr) || !got) return false;
#else
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
            if (left <= 0) return false;
            pollfd pfd{fd_, POLLIN, 0};
            int r = poll(&pfd, 1, left);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            ssize_t got = recv(fd_, p, n, 0);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return false;
#endif
            p += got;
            n -= (size_t)got;
        }
        return true;
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

bool send_frame(Pipe &p, uint32_t op, const std::string &json) {
    std::string buf(8 + json.size(), '\0');
    uint32_t len = (uint32_t)json.size();
    for (int i = 0; i < 4; i++) {
        buf[i] = (char)(op >> (8 * i));
        buf[4 + i] = (char)(len >> (8 * i));
    }
    memcpy(&buf[8], json.data(), json.size());
    return p.write_all(buf.data(), buf.size());
}

bool read_frame(Pipe &p, uint32_t &op, std::string &json, int timeout_ms) {
    unsigned char hdr[8];
    if (!p.read_exact(hdr, 8, timeout_ms)) return false;
    op = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
    uint32_t len = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    if (len > 1 << 20) return false;
    json.assign(len, '\0');
    return !len || p.read_exact(&json[0], len, 2000);
}

// ---------------------------------------------------------------- tiny JSON helpers
std::string json_str(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out + "\"";
}
// The string value of "key" (first occurrence), unescaped well enough for Discord's error messages.
std::string json_get(const std::string &json, const char *key) {
    std::string k = std::string("\"") + key + "\"";
    size_t i = json.find(k);
    if (i == std::string::npos) return "";
    i = json.find(':', i + k.size());
    if (i == std::string::npos) return "";
    i = json.find_first_not_of(" \t\r\n", i + 1);
    if (i == std::string::npos || json[i] != '"') return "";
    std::string out;
    for (i++; i < json.size() && json[i] != '"'; i++) {
        if (json[i] == '\\' && i + 1 < json.size()) i++;
        out += json[i];
    }
    return out;
}

long current_pid() {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

// ---------------------------------------------------------------- worker
struct Activity {
    std::string details, state;
    int64_t start = 0;   // unix seconds, 0 = no timer
    bool operator==(const Activity &o) const { return details == o.details && state == o.state && start == o.start; }
};

std::mutex g_mtx;
std::condition_variable g_cv;
bool g_enabled;
std::string g_client_id;
Activity g_want;             // what the game shows now
uint64_t g_want_gen = 0;     // bumped whenever g_want changes
bool g_quit;
std::string g_status = "Off";
std::thread g_thread;

void set_status(const std::string &s) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_status != s) fprintf(stderr, "discord: %s\n", s.c_str());
    g_status = s;
}

std::string activity_json(const Activity &a, int nonce) {
    std::string act = "null";
    if (!a.details.empty()) {
        act = "{\"details\":" + json_str(a.details);
        if (!a.state.empty()) act += ",\"state\":" + json_str(a.state);
        if (a.start) act += ",\"timestamps\":{\"start\":" + std::to_string(a.start) + "}";
        act += ",\"instance\":false}";
    }
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(current_pid()) + ",\"activity\":" + act +
           "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
}

void worker() {
    Pipe pipe;
    std::string connected_id;
    uint64_t sent_gen = ~0ull;
    Clock::time_point next_try{}, last_send{};
    int nonce = 0;
    bool told_missing = false;
    for (;;) {
        bool enabled;
        std::string id;
        Activity want;
        uint64_t want_gen;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait_for(lk, std::chrono::milliseconds(200));
            if (g_quit) break;
            enabled = g_enabled;
            id = g_client_id;
            want = g_want;
            want_gen = g_want_gen;
        }
        if (pipe.is_open() && (!enabled || id != connected_id)) {   // turned off, or a different application
            send_frame(pipe, OP_FRAME, activity_json(Activity{}, ++nonce));
            send_frame(pipe, OP_CLOSE, "{}");
            pipe.close();
            sent_gen = ~0ull;
        }
        if (!enabled || id.empty()) {
            set_status(id.empty() ? "Needs a Discord application ID" : "Off");
            next_try = {};
            continue;
        }
        if (!pipe.is_open()) {
            if (Clock::now() < next_try) continue;
            next_try = Clock::now() + std::chrono::seconds(10);
            if (!pipe.open()) {
                if (!told_missing) set_status("Discord isn't running");
                told_missing = true;
                continue;
            }
            uint32_t op = 0;
            std::string reply;
            if (!send_frame(pipe, OP_HANDSHAKE, "{\"v\":1,\"client_id\":" + json_str(id) + "}") ||
                !read_frame(pipe, op, reply, 3000)) {
                pipe.close();
                set_status("Discord didn't answer");
                continue;
            }
            if (op != OP_FRAME || json_get(reply, "evt") != "READY") {
                std::string msg = json_get(reply, "message");
                pipe.close();
                set_status("Discord refused: " + (msg.empty() ? std::string("unknown error") : msg));
                next_try = Clock::now() + std::chrono::seconds(30);
                continue;
            }
            connected_id = id;
            sent_gen = ~0ull;
            told_missing = false;
            set_status("Connected");   // (the READY reply names the Discord user: kept out of ujl.log)
        }
        // Discord's messages: pings, answers to SET_ACTIVITY (errors logged), close.
        bool lost = false;
        for (long n; !lost && (n = pipe.available()) != 0;) {
            uint32_t op = 0;
            std::string msg;
            if (n < 0 || !read_frame(pipe, op, msg, 1000)) { lost = true; break; }
            if (op == OP_PING) lost = !send_frame(pipe, OP_PONG, msg);
            else if (op == OP_CLOSE) lost = true;
            else if (json_get(msg, "evt") == "ERROR") fprintf(stderr, "discord: error: %s\n", json_get(msg, "message").c_str());
        }
        // Discord allows about 5 activity updates per 20 s; changes in between are merged into the next one.
        if (!lost && want_gen != sent_gen && Clock::now() - last_send >= std::chrono::seconds(4)) {
            lost = !send_frame(pipe, OP_FRAME, activity_json(want, ++nonce));
            last_send = Clock::now();
            sent_gen = want_gen;
            if (!lost) fprintf(stderr, "discord: activity \"%s\" / \"%s\"\n", want.details.c_str(), want.state.c_str());
        }
        if (lost) {
            pipe.close();
            next_try = Clock::now() + std::chrono::seconds(5);
            set_status("Discord closed the connection");
        }
    }
    if (pipe.is_open()) {
        send_frame(pipe, OP_FRAME, activity_json(Activity{}, ++nonce));
        send_frame(pipe, OP_CLOSE, "{}");
    }
}

// ---------------------------------------------------------------- game state
const char *rating_name(int meter) {
    return meter <= 2 ? "COOL" : meter <= 6 ? "GOOD" : meter <= 10 ? "BAD" : "AWFUL";
}

}  // namespace

void discord_rpc_describe(bool paused, std::string &details, std::string &state) {
    static std::string last_details = "Starting up", last_state;
    const char *ov = rt_current_overlay_name();
    if (!ov) { details = last_details; state = last_state; return; }   // between overlays (loading): keep the last line
    details.clear();
    state.clear();
    size_t n = strlen(ov);
    bool song = n >= 4 && (!strcmp(ov + n - 4, ".SNG") || !strcmp(ov + n - 4, ".CMP") || !strcmp(ov + n - 4, ".COP"));
    unsigned stage = mem_r32(0x80078ae0), sub = mem_r32(0x80078ae4);
    if (!strcmp(ov, "COMOD0.MEN")) details = "In the menus";
    else if (!strncmp(ov, "COMOD8.", 7)) details = "Watching the staff roll";
    else if (song && stage >= 1 && stage <= 7) {
        // Stage/submode pair the scene loader picks the scene from (0x80078ae0/e4). Submodes: 0 Lammy, 1 Lammy vs.
        // Rammy, 2 Lammy and Rammy co-op, 3 PaRappa, 4 PaRappa vs. Lammy, 5 Lammy and PaRappa co-op.
        static const char *who[6] = {"Lammy", "Lammy vs. Rammy", "Lammy & Rammy", "PaRappa", "PaRappa vs. Lammy",
                                     "Lammy & PaRappa"};
        unsigned mode = mem_r32(0x80078aec);   // 0 solo, 1 versus, 2 co-op
        unsigned replay = mem_r32(0x80071dd0); // 0 live, 1 replay, 2 attract demo
        char buf[96];
        bool arcade = stage == 1 && arcade_stage1_active_slot() >= 0;
        snprintf(buf, sizeof buf, "Stage %u%s: %s", stage, arcade ? " (arcade)" : "", sub < 6 ? who[sub] : "?");
        if (replay == 2) details = std::string("Watching the demo");
        else if (replay == 1) details = std::string("Watching a replay, ") + buf;
        else details = buf;
        if (paused) state = "Paused";
        else if (replay != 2 && (int32_t)mem_r32(0x80078af8) > 0 && !(mem_r32(0x80071dd4) & 8)) {   // song time
            int m1 = (int16_t)mem_r16(0x80071dc0), m2 = (int16_t)mem_r16(0x80071dc0 + 10);
            if (mode == 1) state = std::string("P1 ") + rating_name(m1) + " · P2 " + rating_name(m2);
            else state = std::string("Rating: ") + rating_name(m1);
            if (replay == 0 && autoplay_active()) state += " (Autoplay)";
        }
    } else details = "Playing";
    last_details = details;
    last_state = state;
}

void discord_rpc_configure(bool enabled, const std::string &client_id) {
    std::string id;
    for (char c : client_id) if (c >= '0' && c <= '9') id += c;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_enabled = enabled;
        g_client_id = id;
    }
    if (!g_thread.joinable()) g_thread = std::thread(worker);
    g_cv.notify_all();
}

void discord_rpc_frame(bool paused) {
    static Clock::time_point next;
    static std::string scene_key;
    static int64_t scene_start;
    Clock::time_point now = Clock::now();
    if (now < next) return;
    next = now + std::chrono::milliseconds(500);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_enabled || g_client_id.empty()) return;
    }
    Activity a;
    discord_rpc_describe(paused, a.details, a.state);
    // The elapsed timer restarts when the scene changes (menus, a stage, the staff roll), not on rating changes.
    if (a.details != scene_key) { scene_key = a.details; scene_start = (int64_t)time(nullptr); }
    a.start = scene_start;
    static bool log = getenv("UJL_DISCORD_LOG") != nullptr;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!(a == g_want)) {
        if (log) fprintf(stderr, "discord: now \"%s\" / \"%s\" (since %lld)\n", a.details.c_str(), a.state.c_str(), (long long)a.start);
        g_want = a;
        g_want_gen++;
        g_cv.notify_all();
    }
}

std::string discord_rpc_status() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_status;
}

void discord_rpc_shutdown() {
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_quit = true;
    }
    g_cv.notify_all();
    g_thread.join();
}
#endif
