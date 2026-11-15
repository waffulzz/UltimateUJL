#include "psx.h"
#include "gpu.h"
#include "gpu_rec.h"
#include <stdlib.h>
#include <SDL3/SDL.h>

void gpu_soft_prim(const GpuPrim *p);

typedef struct { uint8_t *d; size_t len, cap; } Buf;
static Buf rec, pending, taken;          /* rec: game thread; pending: shared; taken: UI thread */
static SDL_Mutex *mtx;
int g_gpu_rec_drops;                  /* backlog drops (UI too slow), for stats */
static SDL_AtomicInt want_on, want_resync;
static int recording;
static int pending_frames;            /* frames waiting for the UI thread */

static void *buf_push(Buf *b, uint32_t type, size_t size) {
    size_t need = b->len + sizeof(RecHdr) + size;
    if (need > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : (1u << 20);
        while (nc < need) nc *= 2;
        b->d = (uint8_t *)realloc(b->d, nc);
        b->cap = nc;
    }
    RecHdr *h = (RecHdr *)(b->d + b->len);
    h->type = type;
    h->size = (uint32_t)size;
    b->len = need;
    return h + 1;
}

static void buf_append(Buf *dst, const Buf *src) {
    if (!src->len) return;
    if (dst->len + src->len > dst->cap) {
        size_t nc = dst->cap ? dst->cap : (1u << 20);
        while (nc < dst->len + src->len) nc *= 2;
        dst->d = (uint8_t *)realloc(dst->d, nc);
        dst->cap = nc;
    }
    memcpy(dst->d + dst->len, src->d, src->len);
    dst->len += src->len;
}

static void rec_upload(int x, int y, int w, int h) {
    if (!recording || w <= 0 || h <= 0) return;
    RecRect *r = (RecRect *)buf_push(&rec, REC_UPLOAD, sizeof(RecRect) + (size_t)w * h * 2);
    r->x = (int16_t)x; r->y = (int16_t)y; r->w = (int16_t)w; r->h = (int16_t)h;
    uint16_t *px = (uint16_t *)(r + 1);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px[j * w + i] = g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
}

uint64_t g_soft_ns; /* host time in the software rasterizer (UJL_CD_STATS) */
/* While the hardware renderer records, primitives are no longer also rasterized in software into g_vram on the
   game thread. That cost ~2.4 ms per vblank on average but spiked to 20-35 ms, which pushed the game past a vblank:
   a 3-vblank frame about once a second in Stage 1, seen as the crowd's bobbing snapping back. g_vram still gets
   uploads, fills and copies (gpu.c); only drawn pixels are missing, and UJL never reads VRAM back to the CPU
   (gpu.c logs it if that ever happens). UJL_SOFT_RASTER=1 keeps the software copy. */
static int soft_raster_on(void) {
    static int keep_soft = -1;
    if (keep_soft < 0) keep_soft = getenv("UJL_SOFT_RASTER") != NULL;
    return keep_soft;
}
static void rec_prim(const GpuPrim *p) {
    if (!recording || soft_raster_on()) {
        uint64_t t0 = SDL_GetTicksNS();
        gpu_soft_prim(p);
        g_soft_ns += SDL_GetTicksNS() - t0;
    }
    if (!recording) return;
    memcpy(buf_push(&rec, REC_PRIM, sizeof(GpuPrim)), p, sizeof(GpuPrim));
}

static void rec_fill(int x, int y, int w, int h, uint32_t rgb) {
    if (!recording) return;
    RecFill *f = (RecFill *)buf_push(&rec, REC_FILL, sizeof(RecFill));
    f->x = (int16_t)x; f->y = (int16_t)y; f->w = (int16_t)w; f->h = (int16_t)h; f->rgb = rgb;
}

static void rec_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!recording) return;
    RecCopy *c = (RecCopy *)buf_push(&rec, REC_COPY, sizeof(RecCopy));
    c->sx = (int16_t)sx; c->sy = (int16_t)sy; c->dx = (int16_t)dx; c->dy = (int16_t)dy;
    c->w = (int16_t)w; c->h = (int16_t)h;
}

void gpu_rec_install(void) {
    if (!mtx) mtx = SDL_CreateMutex();
    g_gpu_backend.prim = rec_prim;
    g_gpu_backend.vram_write = rec_upload;
    g_gpu_backend.fill = rec_fill;
    g_gpu_backend.vram_copy = rec_copy;
}

void gpu_rec_set_enabled(int on) { SDL_SetAtomicInt(&want_on, on); }
void gpu_rec_request_resync(void) { SDL_SetAtomicInt(&want_resync, 1); }

void gpu_rec_vblank(void) {
    if (!mtx) return;
    int on = SDL_GetAtomicInt(&want_on);
    if (on && (!recording || SDL_GetAtomicInt(&want_resync))) {
        recording = 1;
        SDL_SetAtomicInt(&want_resync, 0);
        rec.len = 0;
        rec_upload(0, 0, 1024, 512);          /* start from the current VRAM */
    }
    if (!on) {
        recording = 0;
        rec.len = 0;
        return;
    }
    memcpy(buf_push(&rec, REC_FRAME, sizeof(GpuDisplay)), &g_gpu_display, sizeof(GpuDisplay));
    SDL_LockMutex(mtx);
    /* Without the software raster g_vram has no drawn pixels, so a resync (full upload from g_vram) would replace the
       frame buffers with stale data (a green screen after a fast-forwarded load). Wait for the UI thread instead,
       up to 250 ms, before falling back to dropping. */
    if ((pending_frames >= 6 || pending.len > (64u << 20)) && !soft_raster_on()) {
        uint64_t t0 = SDL_GetTicks();
        while ((pending_frames >= 6 || pending.len > (64u << 20)) && SDL_GetTicks() - t0 < 250) {
            SDL_UnlockMutex(mtx);
            SDL_Delay(1);
            SDL_LockMutex(mtx);
        }
    }
    /* The UI thread fell behind. Replaying a growing backlog only makes each replay slower (and the backlog
       bigger), so drop it and restart from a full VRAM copy at the next vblank: a stutter, not a spiral. */
    if (pending_frames >= 6 || pending.len > (64u << 20)) {
        pending.len = 0;
        pending_frames = 0;
        SDL_SetAtomicInt(&want_resync, 1);
        g_gpu_rec_drops++;
    }
    buf_append(&pending, &rec);
    pending_frames++;
    SDL_UnlockMutex(mtx);
    rec.len = 0;
}

size_t gpu_rec_take(const uint8_t **out) {
    if (!mtx) { *out = NULL; return 0; }
    SDL_LockMutex(mtx);
    Buf t = taken;
    taken = pending;
    pending = t;
    pending.len = 0;
    pending_frames = 0;
    SDL_UnlockMutex(mtx);
    *out = taken.d;
    return taken.len;
}
