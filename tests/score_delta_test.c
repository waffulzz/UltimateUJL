/* Native score-delta integration checks with synthetic guest state and captured GPU primitives. */
#include "psx.h"
#include "score_delta.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>

uint8_t *g_ram;
int g_cache_isolated;
uint32_t g_watch_lo, g_watch_hi, g_rwatch_lo, g_rwatch_hi, g_gpu_frame;
int g_autoplay, g_autoplay_used, g_uncensored = 1, g_log_level = 1;
static const char *overlay = "COMOD6.SNG";
const char *rt_current_overlay_name(void) { return overlay; }
void rt_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a); }
uint32_t mmio_read(uint32_t a, int s) { (void)a; (void)s; abort(); }
void mmio_write(uint32_t a, uint32_t v, int s) { (void)a; (void)v; (void)s; abort(); }
void rt_watch_hit(uint32_t a, uint32_t v, int s) { (void)a; (void)v; (void)s; }
void rt_rwatch_hit(uint32_t a) { (void)a; }
static GpuPrim emitted[32], digit;
static int count;
static void emit(const GpuPrim *p) { assert(count < 32); emitted[count++] = *p; }
static void draw(void) { count = 0; g_gpu_frame++; score_delta_draw(&digit, emit); }
static void sample(int time, int value) { mem_w32(0x80078af8, (uint32_t)time); mem_w16(0x80071dbc, (uint16_t)value); score_delta_tick(); }
static void finish(int result) { CPU c = {0}; c.r[4] = (uint32_t)result; hook_score_run_finish(&c, 0x800142e8); }
static void restart(void) {
    CPU c = {0}; hook_score_attempt_reset(&c, 0x8001a940);
    mem_w32(0x80071dd4, 0); mem_w32(0x80078af8, 0); mem_w16(0x80071dbc, 0); draw();
}
static void expect_color(int r, int g, int b) {
    assert(count >= 2);
    for (int i = 0; i < count; i++) {
        assert(emitted[i].v[0].r == r && emitted[i].v[0].g == g && emitted[i].v[0].b == b);
        assert(emitted[i].textured && !emitted[i].raw && !emitted[i].gte3d);
        assert(emitted[i].v[0].x >= 70); /* beside the original left score counter */
    }
}
static char *pb_path(const char *dir) {
    char *s = NULL; SDL_asprintf(&s, "%s/s6-v3-m0-i80-n0-c0-p1.bin", dir); return s;
}

int main(int argc, char **argv) {
    assert(argc == 2 && SDL_Init(0));
    g_ram = calloc(PSX_RAM_SIZE, 1); assert(g_ram);
    mem_w32(0x80078ae0, 6); mem_w32(0x80078ae4, 3);
    mem_w8(0x80071cb8 + 48, 0); mem_w8(0x80071d18 + 48, 5);
    digit = (GpuPrim){0}; digit.type = PRIM_RECT; digit.nverts = 1; digit.textured = 1;
    digit.texpage = 542; digit.clut = 16820; digit.w = 10; digit.h = 12;
    digit.clip_x1 = 319; digit.clip_y1 = 239;
    digit.v[0] = (GpuVertex){.x = 52, .y = 170, .u = 32, .r = 128, .g = 128, .b = 128};
    char *dir = NULL; SDL_asprintf(&dir, "%s/case-%llu", argv[1], (unsigned long long)SDL_GetTicksNS());
    score_delta_init(dir);
    draw(); assert(!count); /* no PB: capture silently, no placeholder */
    sample(100, 100); sample(200, 200); finish(1);
    char *path = pb_path(dir); size_t n = 0;
    void *original = SDL_LoadFile(path, &n); assert(original && n >= 64);

    restart(); expect_color(128, 128, 128); /* +/-0, on track */
    assert(count == 2 && emitted[0].type == PRIM_RECT && emitted[0].w == 12 &&
           emitted[0].v[0].u == 184 && emitted[0].v[0].v == 12); /* actual native S_PLMI0 */
    sample(100, 150); draw(); expect_color(32, 64, 128); /* +50 */
    assert(emitted[0].v[0].u == 196); /* native S_PLUS0 */
    assert(emitted[count - 2].v[0].u == 82 && emitted[count - 1].v[0].u == 32);
    sample(200, 150); draw(); expect_color(128, 32, 32); /* -50 */
    assert(emitted[0].v[0].u == 172 && emitted[0].v[0].v == 12); /* actual native S_MINUS0 */
    /* The HUD can disappear for a no-note break. A temporary missing overlay or CD clock must recover
       without losing the reference, changing its time origin, or restarting the score trace. */
    overlay = NULL; score_delta_tick(); draw(); assert(!count);
    overlay = "COMOD6.SNG"; sample(220, 150); draw(); expect_color(128, 32, 32);
    sample(0, 150); sample(240, 200); draw(); expect_color(128, 128, 128);
    assert(emitted[0].v[0].u == 184); /* +/-0 after the HUD returns */
    /* Cinematic clipping is not a coordinate translation; both native display buffers still match. */
    digit.clip_x0 = 16; digit.clip_y0 = 24; draw(); expect_color(128, 128, 128);
    digit.v[0].y += 256; digit.clip_y0 += 256; digit.clip_y1 += 256;
    draw(); expect_color(128, 128, 128);
    assert(emitted[0].v[0].y == 426);
    digit.v[0].y -= 256; digit.clip_y0 = digit.clip_x0 = 0; digit.clip_y1 = 239;
    finish(0); /* an aborted/failed run cannot replace a PB */
    size_t m = 0; void *after = SDL_LoadFile(path, &m); assert(after && m == n && !memcmp(after, original, n)); SDL_free(after);

    restart(); g_autoplay = 1; sample(100, 300); sample(200, 500); finish(1); g_autoplay = 0;
    after = SDL_LoadFile(path, &m); assert(after && m == n && !memcmp(after, original, n)); SDL_free(after);
    restart(); mem_w32(0x80071dd0, 1); sample(100, 300); sample(200, 500); finish(1); mem_w32(0x80071dd0, 0);
    after = SDL_LoadFile(path, &m); assert(after && m == n && !memcmp(after, original, n)); SDL_free(after);

    /* Improvement persists, including a final award occurring after the last tick/visible HUD. */
    restart(); sample(100, 150); sample(200, 250); overlay = "COMOD0.MEN";
    score_delta_tick(); mem_w16(0x80071dbc, 300); finish(1); overlay = "COMOD6.SNG";
    restart(); sample(200, 300); draw(); expect_color(128, 128, 128);

    /* Stage 3's ending movie resets the clock while the song overlay and old HUD are still present.
       Keep the complete trace through that reset, then save it at the later results hook. */
    restart(); sample(100, 200); sample(200, 400);
    mem_w32(0x80071dd4, 8); score_delta_tick();
    mem_w32(0x80078af8, 0); score_delta_tick(); draw(); assert(!count);
    mem_w32(0x80078af8, 250); score_delta_tick(); draw(); assert(!count); /* ending movie cannot restart capture */
    finish(1); restart(); sample(200, 400); draw(); expect_color(128, 128, 128);
    /* A real retry calls the initializer and must not inherit the previous attempt's score points. */
    restart(); sample(100, 1000); restart(); sample(100, 250); sample(200, 500); finish(1);
    restart(); sample(100, 250); draw(); expect_color(128, 128, 128);
    /* A full song counts even below GOOD (result zero). Early failure and Start/quit have separate bits. */
    restart(); sample(100, 300); mem_w32(0x80071dd4, 8); sample(200, 600); finish(0);
    restart(); sample(200, 600); draw(); expect_color(128, 128, 128);
    restart(); sample(100, 1500); mem_w32(0x80071dd4, 0x29); sample(200, 2000); finish(0);
    restart(); sample(200, 600); draw(); expect_color(128, 128, 128);
    restart(); sample(100, 1500); mem_w32(0x80071dd4, 9); sample(200, 2000); finish(0);
    restart(); sample(200, 600); draw(); expect_color(128, 128, 128);
    /* Saving after an interlude retains the original start and all later points, not just the final HUD. */
    restart(); sample(100, 700); overlay = NULL; score_delta_tick(); overlay = "COMOD6.SNG";
    sample(0, 700); sample(200, 800); finish(1);
    after = SDL_LoadFile(path, &m); assert(after && m == 72);
    uint32_t initial; memcpy(&initial, (uint8_t *)after + 52, 4);
    assert(SDL_SwapLE32(initial) == 0); SDL_free(after);
    restart(); sample(100, 700); draw(); expect_color(128, 128, 128);

    /* Invalid files and a different mode never supply a bogus reference. */
    assert(SDL_SaveFile(path, "bad", 3)); score_delta_init(dir); mem_w32(0x80078af8, 0); draw(); assert(!count);
    mem_w32(0x80078af8, 0); mem_w32(0x80078ae4, 5); score_delta_tick(); draw(); assert(!count);
    overlay = "COMOD0.MEN"; draw(); assert(!count);
    overlay = "COMOD6.SNG"; mem_w32(0x80071dd0, 2); draw(); assert(!count);

    /* Versus keeps independent timelines, with the right delta to the left of its score. */
    score_delta_init(dir); overlay = "COMOD6.CMP"; mem_w32(0x80071dd0, 0);
    mem_w32(0x80078ae4, 4); mem_w32(0x80078aec, 1); mem_w8(0x80071d18 + 48, 1);
    mem_w32(0x80078af8, 0); mem_w16(0x80071dbc, 300); mem_w16(0x80071dc6, 300);
    digit.v[0].x = 52; draw(); assert(!count);
    mem_w16(0x80071dc6, 400); sample(100, 350); finish(1);
    score_delta_init(dir); mem_w32(0x80078af8, 0); mem_w16(0x80071dbc, 300); mem_w16(0x80071dc6, 300);
    draw(); expect_color(128, 128, 128);
    mem_w16(0x80071dc6, 350); sample(100, 400); draw(); expect_color(32, 64, 128);
    digit.v[0].x = 292; draw(); expect_color(128, 32, 32);
    for (int i = 0; i < count; i++) assert(emitted[i].v[0].x < 266);
    finish(0);
    /* A COM opponent gets no delta, and co-op shares the one native score. */
    score_delta_init(dir); mem_w8(0x80071d18 + 48, 3); draw(); assert(!count);
    score_delta_init(dir); overlay = "COMOD6.COP"; mem_w32(0x80078aec, 2); mem_w32(0x80078ae4, 5);
    mem_w8(0x80071d18 + 48, 1); mem_w32(0x80078af8, 0); mem_w16(0x80071dbc, 0);
    digit.v[0].x = 52; draw(); assert(!count); sample(100, 500); finish(1);
    score_delta_init(dir); mem_w32(0x80078af8, 0); mem_w16(0x80071dbc, 0); draw();
    sample(100, 450); draw(); expect_color(128, 32, 32);
    digit.v[0].x = 292; draw(); assert(!count);
    SDL_free(original); SDL_free(path); SDL_free(dir); free(g_ram);
    puts("score delta: native HUD, timeline, persistence, completion and exclusion checks passed");
    return 0;
}
