/* File export is asynchronous on iOS. Verify immutable pending data, completion/cancellation, import
   validation and the existing desktop save path with synthetic RAM only (no disc assets). */
#include "psx.h"
#include "replay_files.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>

uint8_t *g_ram;
uint8_t g_scratch[1024];
int g_cache_isolated, g_log_level = 1;
uint32_t g_watch_lo, g_watch_hi, g_rwatch_lo, g_rwatch_hi;
void rt_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a); }
uint32_t mmio_read(uint32_t a, int s) { (void)a; (void)s; abort(); }
void mmio_write(uint32_t a, uint32_t v, int s) { (void)a; (void)v; (void)s; abort(); }
void rt_watch_hit(uint32_t a, uint32_t v, int s) { (void)a; (void)v; (void)s; }
void rt_rwatch_hit(uint32_t a) { (void)a; }

enum { BLOCK = 8192, HEADER = 512, PAYLOAD = 0x1dd4, CHECKSUM = 0x1dd0 };
enum { EXPORT_OK, EXPORT_CANCEL, DESKTOP_SAVE, IMPORT_OK, IMPORT_BAD };
static int mode, waits;
static uint8_t captured[BLOCK];
static char path[2048], bad_path[2048];

void call_indirect(CPU *c, uint32_t addr) {
    if (addr == 0x8001473c) { c->r[2] = 0x80090000; return; }
    if (addr == 0x800147d0) return;
    assert(addr == 0x80020060 && c->r[4] == 0x80090000);
    uint8_t *out = ram_ptr(c->r[4]); memset(out, 0, BLOCK);
    out[0] = 'S'; out[1] = 'C'; out[3] = 1;
    memcpy(out + HEADER, ram_ptr(0x801a3538), PAYLOAD);
    memset(out + HEADER + CHECKSUM, 0, 4);
    uint32_t sum = 0;
    for (int i = 0; i < PAYLOAD; i++) sum += out[HEADER + i];
    for (int i = 0; i < 4; i++) out[HEADER + CHECKSUM + i] = (uint8_t)(sum >> (i * 8));
    c->r[2] = 0;
}

void rt_idle(CPU *c) {
    (void)c; waits++;
    const char *name = NULL;
    int kind = replay_files_take_request(&name);
    assert(kind == (mode >= IMPORT_OK ? 2 : 1));
    assert(replay_files_busy());
    replay_files_set_enabled(0); assert(replay_files_enabled()); /* disabled while pending */
    size_t n = 99;
    const void *bytes = replay_files_export_data(&n);
    if (kind == 1) {
        assert(!strcmp(name, "Stage 3.ujlreplay") && bytes && n == BLOCK);
        memcpy(captured, bytes, BLOCK);
        assert(captured[0] == 'S' && captured[HEADER] == 3 && captured[HEADER + 8] == 123);
        assert(replay_files_take_request(&name) == 0); /* picker owns it until its callback */
        assert(replay_files_export_data(&n) == bytes && !memcmp(bytes, captured, BLOCK));
        if (mode == DESKTOP_SAVE) assert(replay_files_complete(path));
        else assert(replay_files_complete_export(mode == EXPORT_OK));
    } else {
        assert(!bytes && n == 0 && !replay_files_complete_export(1)); /* never accept export for an import */
        if (mode == IMPORT_BAD) {
            assert(!replay_files_complete(bad_path));
            assert(replay_files_error()[0]);
            assert(replay_files_take_request(&name) == 2);
            assert(replay_files_complete(NULL));
        } else assert(replay_files_complete(path));
    }
    assert(!replay_files_export_data(&n) && n == 0);
    assert(!replay_files_complete_export(1)); /* reject duplicate callback */
}

static void save(int operation) {
    CPU c = {0}; c.r[5] = 3; c.r[6] = 0; c.r[7] = 123; c.r[29] = 0x801ff000;
    mem_w32(c.r[29] + 16, 1);
    mode = operation; waits = 0;
    assert(filter_replay_save(&c, 0x8002011c));
    assert(c.r[2] == 0 && waits == 1 && !replay_files_busy());
}

int main(int argc, char **argv) {
    assert(argc == 2 && SDL_Init(0));
    g_ram = calloc(PSX_RAM_SIZE, 1); assert(g_ram);
    SDL_snprintf(path, sizeof path, "%s/replay-export-%llu.ujlreplay", argv[1], (unsigned long long)SDL_GetTicksNS());
    SDL_snprintf(bad_path, sizeof bad_path, "%s.bad", path);
    size_t n = 1;
    assert(!replay_files_export_data(&n) && n == 0 && !replay_files_complete_export(1));
    replay_files_set_enabled(1);
    save(EXPORT_OK);
    size_t size; void *file = SDL_LoadFile(path, &size); assert(!file); /* export callback does not write a provider path */
    save(EXPORT_CANCEL); file = SDL_LoadFile(path, &size); assert(!file);
    save(DESKTOP_SAVE); file = SDL_LoadFile(path, &size);
    assert(file && size == BLOCK && !memcmp(file, captured, BLOCK)); SDL_free(file);
    CPU c = {0}; mode = IMPORT_OK; c.r[4] = 0; waits = 0;
    assert(filter_replay_slots(&c, 0x801c7adc) && c.r[2] == 0 && waits == 1);
    assert(!memcmp(ram_ptr(0x801a3538), captured + HEADER, PAYLOAD));
    assert(replay_files_menu_screen(5) == 3 && replay_files_menu_screen(5) == 5);
    assert(SDL_SaveFile(bad_path, "invalid", 7));
    mode = IMPORT_BAD; c.r[4] = 0; waits = 0;
    assert(filter_replay_slots(&c, 0x801c7adc) && c.r[2] == (uint32_t)-1 && waits == 1);
    assert(!memcmp(ram_ptr(0x801a3538), captured + HEADER, PAYLOAD));
    file = SDL_LoadFile(bad_path, &size); assert(file && size == 7 && !memcmp(file, "invalid", 7)); SDL_free(file);
    replay_files_set_enabled(0); assert(!replay_files_enabled());
    SDL_RemovePath(path); SDL_RemovePath(bad_path); free(g_ram);
    puts("replay export: pending data, success, cancellation, import validation and desktop save passed");
    return 0;
}
