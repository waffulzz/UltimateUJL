// Discord Rich Presence transport test: a fake Discord client listens where the real one would (a Unix socket
// found by the same folder search on Linux/macOS, a named pipe given by UJL_DISCORD_IPC_PATH on Windows, where the
// real Discord may own discord-ipc-0) and checks the handshake, a ping, two activity updates made from synthetic
// game RAM, and the clearing activity + close sent at shutdown. No disc, no Discord needed.
#include "discord_rpc.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "psx.h"
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------- synthetic runtime (what discord_rpc.cpp reads)
static std::vector<uint8_t> ram(2 << 20);
extern "C" {
uint8_t *g_ram = ram.data();
uint8_t *g_asset_ram = nullptr;
uint32_t g_asset_ram_capacity = 0;
uint8_t g_scratch[1024];
uint32_t g_rwatch_lo = 0, g_rwatch_hi = 0;
void rt_rwatch_hit(uint32_t) {}
uint32_t mmio_read(uint32_t, int) { return 0; }
static std::atomic<const char *> overlay{"COMOD0.MEN"};
const char *rt_current_overlay_name(void) { return overlay.load(); }
int autoplay_active(void) { return 0; }
int arcade_stage1_active_slot(void) { return -1; }
}
static void poke32(uint32_t a, uint32_t v) { memcpy(&ram[a & 0x1FFFFF], &v, 4); }
static void poke16(uint32_t a, uint16_t v) { memcpy(&ram[a & 0x1FFFFF], &v, 2); }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

// ---------------------------------------------------------------- fake Discord
struct Conn {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    bool read(void *p, size_t n) {
        while (n) {
            DWORD got = 0;
            if (!ReadFile(h, p, (DWORD)n, &got, nullptr) || !got) return false;
            p = (char *)p + got;
            n -= got;
        }
        return true;
    }
    bool write(const void *p, size_t n) { DWORD done = 0; return WriteFile(h, p, (DWORD)n, &done, nullptr) && done == n; }
#else
    int fd = -1;
    bool read(void *p, size_t n) {
        while (n) {
            pollfd pfd{fd, POLLIN, 0};
            if (poll(&pfd, 1, 15000) <= 0) return false;
            ssize_t got = recv(fd, p, n, 0);
            if (got <= 0) return false;
            p = (char *)p + got;
            n -= (size_t)got;
        }
        return true;
    }
    bool write(const void *p, size_t n) { return send(fd, p, n, 0) == (ssize_t)n; }
#endif
    bool read_frame(uint32_t &op, std::string &json) {
        unsigned char h[8];
        if (!read(h, 8)) return false;
        op = h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24;
        uint32_t len = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
        json.assign(len, '\0');
        return !len || read(&json[0], len);
    }
    bool write_frame(uint32_t op, const std::string &json) {
        unsigned char h[8];
        uint32_t len = (uint32_t)json.size();
        for (int i = 0; i < 4; i++) { h[i] = (unsigned char)(op >> 8 * i); h[4 + i] = (unsigned char)(len >> 8 * i); }
        return write(h, 8) && write(json.data(), json.size());
    }
};

struct Frame { uint32_t op; std::string json; };

int main() {
    std::string where;
#ifdef _WIN32
    where = "\\\\.\\pipe\\ujl-discord-test-" + std::to_string(GetCurrentProcessId());
    _putenv_s("UJL_DISCORD_IPC_PATH", where.c_str());
    HANDLE listener = CreateNamedPipeA(where.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, 65536, 65536, 0, nullptr);
    if (listener == INVALID_HANDLE_VALUE) { fprintf(stderr, "FAIL: CreateNamedPipe\n"); return 1; }
#else
    // An empty runtime folder: the game's search finds discord-ipc-0 there before any real Discord's socket.
    char dir[] = "/tmp/ujl-discord-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("XDG_RUNTIME_DIR", dir, 1);
    setenv("TMPDIR", dir, 1);
    unsetenv("UJL_DISCORD_IPC_PATH");
    where = std::string(dir) + "/discord-ipc-0";
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, where.c_str(), where.size() + 1);
    if (listener < 0 || bind(listener, (sockaddr *)&addr, sizeof addr) || listen(listener, 1)) { perror("socket"); return 1; }
#endif
    fprintf(stderr, "fake Discord at %s\n", where.c_str());

    std::vector<Frame> got;
    std::atomic<bool> server_done{false};
    std::string handshake;
    std::thread server([&] {
        Conn c;
#ifdef _WIN32
        if (!ConnectNamedPipe(listener, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { server_done = true; return; }
        c.h = listener;
#else
        c.fd = accept(listener, nullptr, nullptr);
        if (c.fd < 0) { server_done = true; return; }
#endif
        uint32_t op;
        if (c.read_frame(op, handshake) && op == 0) {
            c.write_frame(1, "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{\"v\":1,\"user\":{\"username\":\"test\"}}}");
            c.write_frame(3, "{\"ping\":1}");
            std::string json;
            while (c.read_frame(op, json)) got.push_back({op, json});   // until the game closes the connection
        }
#ifdef _WIN32
        DisconnectNamedPipe(listener);
        CloseHandle(listener);
#else
        close(c.fd);
#endif
        server_done = true;
    });

    // Menus first.
    discord_rpc_configure(true, "42");
    auto run_for = [](int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            discord_rpc_frame(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };
    run_for(1500);
    CHECK(discord_rpc_status() == "Connected");
    // Then PaRappa solo in Stage 3 at COOL: stage/submode, solo, live, song clock running, meter 1.
    poke32(0x80078ae0, 3);
    poke32(0x80078ae4, 3);
    poke32(0x80078aec, 0);
    poke32(0x80071dd0, 0);
    poke32(0x80071dd4, 0);
    poke32(0x80078af8, 500);
    poke16(0x80071dc0, 1);
    overlay = "COMOD3.SNG";
    std::string details, state;
    discord_rpc_describe(false, details, state);
    CHECK(details == "Stage 3: PaRappa");
    CHECK(state == "Rating: COOL");
    discord_rpc_describe(true, details, state);
    CHECK(state == "Paused");
    run_for(5000);   // updates are at least 4 s apart
    discord_rpc_shutdown();
    for (int i = 0; i < 100 && !server_done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(server_done);
    if (!server_done) { fprintf(stderr, "server still waiting\n"); return 1; }
    server.join();

    CHECK(handshake.find("\"client_id\":\"42\"") != std::string::npos);
    CHECK(handshake.find("\"v\":1") != std::string::npos);
    int pongs = 0, menus = -1, stage = -1, cleared = -1, closed = -1;
    for (size_t i = 0; i < got.size(); i++) {
        const Frame &f = got[i];
        fprintf(stderr, "op %u %s\n", f.op, f.json.c_str());
        if (f.op == 4) { pongs++; CHECK(f.json == "{\"ping\":1}"); }
        if (f.op == 1 && f.json.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos) {
            CHECK(f.json.find("\"pid\":") != std::string::npos);
            if (f.json.find("\"details\":\"In the menus\"") != std::string::npos && menus < 0) menus = (int)i;
            if (f.json.find("\"details\":\"Stage 3: PaRappa\",\"state\":\"Rating: COOL\"") != std::string::npos) stage = (int)i;
            if (f.json.find("\"activity\":null") != std::string::npos) cleared = (int)i;
        }
        if (f.op == 2) closed = (int)i;
    }
    CHECK(pongs == 1);
    CHECK(menus >= 0);
    CHECK(stage > menus);
    CHECK(cleared > stage);
    CHECK(closed > cleared);
#ifndef _WIN32
    unlink(where.c_str());
    rmdir(dir);
#endif
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS\n");
    return 0;
}
