/* Replay files keep the game's native SC block, including its disc-provided icon/title and checksum.
   The hooks replace replay slots/transfers only; system saves and the disabled path stay on the card. */
#include "psx.h"
#include "replay_files.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

#ifndef UJL_REPLAY_FILES
#define UJL_REPLAY_FILES 0
#endif

enum { BLOCK = 8192, HEADER = 512, PAYLOAD = 0x1dd4, CHECKSUM = 0x1dd0 };
#define LIVE_REPLAY 0x801a3538u
#define MENU_SLOTS  0x801deaa8u
static SDL_AtomicInt enabled, state; /* 0 idle, 1 requested, 2 dialog open, 3 completed */
static int request_kind, result;
static uint8_t image[BLOCK];
static char suggested[128], error[256];
static int played_file;

int replay_files_enabled(void) { return UJL_REPLAY_FILES && SDL_GetAtomicInt(&enabled); }
int replay_files_busy(void) { return SDL_GetAtomicInt(&state) != 0; }
void replay_files_set_enabled(int on) { if (!replay_files_busy()) SDL_SetAtomicInt(&enabled, UJL_REPLAY_FILES && on); }
const char *replay_files_error(void) { return error; }
int replay_files_menu_screen(int screen) {
    /* The game's replay-return entry starts screen 5 (the card slot picker). A host-file replay has
       already been chosen, so returning there must not launch a second dialog. Clear at the main menu
       as well, so selecting Replay again remains an explicit, fresh open request. */
    if (played_file && screen == 5) {
        LOGI("replay: returned to main menu");
        screen = 3;
    }
    if (screen == 3) played_file = 0;
    return screen;
}
int replay_files_take_request(const char **name) {
    if (!SDL_CompareAndSwapAtomicInt(&state, 1, 2)) return 0;
    *name = suggested;
    return request_kind;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int valid_image(const uint8_t *p) {
    if (p[0] != 'S' || p[1] != 'C' || p[3] != 1) return 0;
    const uint8_t *data = p + HEADER;
    uint32_t stage = le32(data), mode = le32(data + 4), sum = 0;
    /* Replay stage/mode select code/assets: reject invalid indices before copying anything to RAM. */
    if (stage < 1 || stage > 7 || mode > 5) return 0;
    for (int i = 0; i < PAYLOAD; i++) if (i < CHECKSUM || i >= CHECKSUM + 4) sum += data[i];
    return sum == le32(data + CHECKSUM);
}

int replay_files_complete(const char *path) {
    if (SDL_GetAtomicInt(&state) != 2) return 0;
    result = 0;
    error[0] = 0;
    if (path) {
        if (request_kind == 2) {
            /* Bound reads to one save plus an optional .mcs directory frame. */
            uint8_t bytes[BLOCK + 129];
            SDL_IOStream *io = SDL_IOFromFile(path, "rb");
            if (!io) goto io_error;
            size_t n = SDL_ReadIO(io, bytes, sizeof bytes);
            bool closed = SDL_CloseIO(io);
            if (!closed) goto io_error;
            const uint8_t *p = bytes;
            if (n == BLOCK + 128 && bytes[0] == 0x51 &&
                (!SDL_strncmp((char *)bytes + 10, "BISCPS-180110", 13) ||
                 !SDL_strncmp((char *)bytes + 10, "BASCUS-944480", 13))) p += 128;
            else if (n != BLOCK) goto invalid;
            if (!valid_image(p)) goto invalid;
            memcpy(image, p, BLOCK);
        } else {
            /* Write beside the destination, then replace it only after a complete, closed write. */
            char *tmp = NULL;
            SDL_asprintf(&tmp, "%s.tmp-%llu", path, (unsigned long long)SDL_GetTicksNS());
            if (!tmp) goto io_error;
            bool ok = SDL_SaveFile(tmp, image, BLOCK);
            if (ok) ok = SDL_RenamePath(tmp, path);
            if (!ok) SDL_strlcpy(error, SDL_GetError(), sizeof error);
            SDL_RemovePath(tmp);
            SDL_free(tmp);
            if (!ok) goto retry;
        }
        result = 1;
        LOGI("replay: %s %s", request_kind == 1 ? "saved" : "loaded", path);
    }
    SDL_SetAtomicInt(&state, 3);
    return 1;
invalid:
    SDL_strlcpy(error, "This is not a valid Um Jammer Lammy replay (single SC block or .mcs save).", sizeof error);
    goto retry;
io_error:
    SDL_strlcpy(error, SDL_GetError(), sizeof error);
retry:
    LOGI("replay: %s", error);
    SDL_SetAtomicInt(&state, 1);
    return 0;
}

static int exchange(CPU *c, int kind) {
    result = 0;
    request_kind = kind;
    SDL_SetAtomicInt(&state, 1);
    /* Deterministic probe/frontend tests; empty value means cancel, an error cancels after logging. */
    const char *path = getenv(kind == 1 ? "UJL_REPLAY_SAVE" : "UJL_REPLAY_OPEN");
    if (path) {
        const char *unused;
        replay_files_take_request(&unused);
        if (!replay_files_complete(path[0] ? path : NULL)) {
            replay_files_take_request(&unused);
            replay_files_complete(NULL);
        }
    }
    while (SDL_GetAtomicInt(&state) != 3) rt_idle(c);
    int ok = result;
    SDL_SetAtomicInt(&state, 0);
    return ok;
}

static void restore_registers(CPU *c, const CPU *saved) {
    memcpy(c->r, saved->r, sizeof c->r);
    c->hi = saved->hi;
    c->lo = saved->lo;
}

static int save_image(CPU *c, uint32_t stage, uint32_t mode, uint32_t score, uint32_t rating);

int filter_replay_directory(CPU *c, uint32_t fn) {
    (void)fn;
    if (!replay_files_enabled()) return 0;
    /* The replay prompt checks count + card free space. A host slot always exists, even on a full card. */
    uint32_t dst = c->r[4];
    for (int i = 0; i < 600; i++) mem_w8(dst + i, 0);
    c->r[2] = 1;
    return 1;
}

int filter_replay_slots(CPU *c, uint32_t fn) {
    (void)fn;
    if (!replay_files_enabled()) return 0;
    int save = c->r[4] != 0;
    CPU saved = *c;
    int ok = 1;
    for (int i = 0; i < 40; i++) mem_w8(MENU_SLOTS + i, 0);
    if (save) {
        /* YES already authorizes saving. Use the name entered after the song, skip the card slots and
           second name-entry screen, and let the original caller take its normal continuation. */
        uint32_t score = mem_r32(0x80078aecu) ? 0 : mem_r32(0x80078af0u);
        save_image(c, mem_r32(0x80078ae0u), mem_r32(0x80078ae4u), score, mem_r32(0x80078af4u));
        ok = 0;
    } else {
        suggested[0] = 0;
        ok = exchange(c, 2);
        if (ok) {
            for (int i = 0; i < PAYLOAD; i++) mem_w8(LIVE_REPLAY + i, image[HEADER + i]);
            mem_w32(MENU_SLOTS + 32, le32(image + HEADER));
            mem_w32(MENU_SLOTS + 36, le32(image + HEADER + 4));
            played_file = 1;
        }
    }
    /* The caller handles stage setup/fades/cancellation exactly as for a selected card slot. */
    restore_registers(c, &saved);
    c->r[2] = ok ? 0 : (uint32_t)-1;
    return 1;
}

static int save_image(CPU *c, uint32_t stage, uint32_t mode, uint32_t score, uint32_t rating) {
    mem_w32(LIVE_REPLAY, stage);
    mem_w32(LIVE_REPLAY + 4, mode);
    mem_w32(LIVE_REPLAY + 8, score);
    mem_w32(LIVE_REPLAY + 12, rating);
    /* Use the game's own buffer allocator and builder: all title/icon content comes from the disc. */
    c->r[4] = BLOCK;
    call_indirect(c, 0x8001473cu);
    uint32_t buf = c->r[2];
    if (!buf) {
        LOGI("replay: no guest buffer available for saving");
        return 4;
    }
    c->r[4] = buf;
    c->r[5] = 0; /* icon/title variant, same as the first replay slot */
    call_indirect(c, 0x80020060u);
    for (int i = 0; i < BLOCK; i++) image[i] = (uint8_t)mem_r8(buf + i);
    call_indirect(c, 0x800147d0u);
    SDL_snprintf(suggested, sizeof suggested, "Stage %u.ujlreplay", stage);
    exchange(c, 1);
    return 0;
}

int filter_replay_save(CPU *c, uint32_t fn) {
    (void)fn;
    if (!replay_files_enabled()) return 0;
    CPU saved = *c;
    int error_code = save_image(c, c->r[5], c->r[6], c->r[7], mem_r32(c->r[29] + 16));
    restore_registers(c, &saved);
    c->r[2] = (uint32_t)error_code; /* cancellation skips saving without a card-error retry */
    return 1;
}
