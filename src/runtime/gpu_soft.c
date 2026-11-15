/* Reference software rasterizer: draws GPU primitives into g_vram at native resolution,
   following the PS1 rules (top-left fill, CLUT textures, texture window, semi-transparency,
   mask bit). This is the "original" renderer and the ground truth for VRAM readbacks. */
#include "psx.h"
#include "gpu.h"
#include <stdlib.h>

static inline uint16_t vram_get(int x, int y) { return g_vram[(y & 511) * 1024 + (x & 1023)]; }

typedef struct {
    int tex, raw, semi, mode;    /* semi mode 0..3 */
    int depth;                   /* 0=4bpp 1=8bpp 2=15bpp */
    int tpx, tpy;                /* texpage base */
    int clutx, cluty;
    int twmx, twmy, twox, twoy;  /* texture window mask/offset (in pixels) */
    int mask_set, mask_check;
    int cx0, cy0, cx1, cy1;
} RState;

static void rstate(RState *s, const GpuPrim *p) {
    s->tex = p->textured;
    s->raw = p->raw;
    s->semi = p->semi;
    s->mode = (p->texpage >> 5) & 3;
    s->depth = (p->texpage >> 7) & 3;
    if (s->depth == 3) s->depth = 2;
    s->tpx = (p->texpage & 15) * 64;
    s->tpy = ((p->texpage >> 4) & 1) * 256;
    s->clutx = (p->clut & 0x3F) * 16;
    s->cluty = (p->clut >> 6) & 0x1FF;
    s->twmx = (p->texwindow & 0x1F) * 8;
    s->twmy = ((p->texwindow >> 5) & 0x1F) * 8;
    s->twox = ((p->texwindow >> 10) & 0x1F) * 8;
    s->twoy = ((p->texwindow >> 15) & 0x1F) * 8;
    s->mask_set = p->mask_set;
    s->mask_check = p->mask_check;
    s->cx0 = p->clip_x0; s->cy0 = p->clip_y0; s->cx1 = p->clip_x1; s->cy1 = p->clip_y1;
}

static inline uint16_t texel(const RState *s, int u, int v) {
    u &= 0xFF; v &= 0xFF;
    u = (u & ~s->twmx) | (s->twox & s->twmx);
    v = (v & ~s->twmy) | (s->twoy & s->twmy);
    switch (s->depth) {
    case 0: {
        uint16_t w = vram_get(s->tpx + (u >> 2), s->tpy + v);
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return vram_get(s->clutx + idx, s->cluty);
    }
    case 1: {
        uint16_t w = vram_get(s->tpx + (u >> 1), s->tpy + v);
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return vram_get(s->clutx + idx, s->cluty);
    }
    default: return vram_get(s->tpx + u, s->tpy + v);
    }
}

static inline int clamp5(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }

/* write pixel: color c (15-bit with bit15 = semi flag from texel), semi applies if enabled */
static inline void plot(const RState *s, int x, int y, uint16_t c, int use_semi) {
    if (x < s->cx0 || x > s->cx1 || y < s->cy0 || y > s->cy1) return;
    uint16_t *d = &g_vram[(y & 511) * 1024 + (x & 1023)];
    if (s->mask_check && (*d & 0x8000)) return;
    uint16_t out = c;
    if (use_semi) {
        int br = *d & 31, bg = (*d >> 5) & 31, bb = (*d >> 10) & 31;
        int fr = c & 31, fg = (c >> 5) & 31, fb = (c >> 10) & 31;
        int r, g, b;
        switch (s->mode) {
        case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
        case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
        case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
        default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
        }
        out = (uint16_t)(clamp5(r) | clamp5(g) << 5 | clamp5(b) << 10 | (c & 0x8000));
    }
    if (s->mask_set) out |= 0x8000;
    *d = out;
}

static inline uint16_t shade(const RState *s, uint16_t t, int r, int g, int b) {
    if (s->raw) return t;
    int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
    return (uint16_t)(clamp5((tr * r) >> 7) | clamp5((tg * g) >> 7) << 5 | clamp5((tb * b) >> 7) << 10 | (t & 0x8000));
}

static inline uint16_t rgb15(int r, int g, int b) {
    return (uint16_t)(clamp5(r >> 3) | clamp5(g >> 3) << 5 | clamp5(b >> 3) << 10);
}

static void fragment(const RState *s, int x, int y, int r, int g, int b, int u, int v) {
    if (s->tex) {
        uint16_t t = texel(s, u, v);
        if (t == 0) return; /* fully transparent texel */
        uint16_t c = shade(s, t, r, g, b);
        plot(s, x, y, c, s->semi && (t & 0x8000));
    } else {
        plot(s, x, y, rgb15(r, g, b), s->semi);
    }
}

/* ---- triangles: barycentric with fixed point, top-left rule ---- */
static inline int64_t edge(int ax, int ay, int bx, int by, int px, int py) {
    return (int64_t)(bx - ax) * (py - ay) - (int64_t)(by - ay) * (px - ax);
}

static void draw_tri(const GpuPrim *p) {
    RState s;
    rstate(&s, p);
    const GpuVertex *v0 = &p->v[0], *v1 = &p->v[1], *v2 = &p->v[2];
    int64_t area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (area == 0) return;
    if (area < 0) { const GpuVertex *t = v1; v1 = v2; v2 = t; area = -area; }
    int minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    if (v1->x < minx) minx = v1->x; if (v1->x > maxx) maxx = v1->x;
    if (v2->x < minx) minx = v2->x; if (v2->x > maxx) maxx = v2->x;
    if (v1->y < miny) miny = v1->y; if (v1->y > maxy) maxy = v1->y;
    if (v2->y < miny) miny = v2->y; if (v2->y > maxy) maxy = v2->y;
    if (maxx - minx >= 1024 || maxy - miny >= 512) return; /* hardware drops oversized polys */
    if (minx < s.cx0) minx = s.cx0;
    if (miny < s.cy0) miny = s.cy0;
    if (maxx > s.cx1) maxx = s.cx1;
    if (maxy > s.cy1) maxy = s.cy1;
    /* top-left rule: pixels exactly on a right/bottom edge are excluded, so a quad from x0 to x1 covers
       x0..x1-1 like the hardware (and like sprites). The test was mirrored before, which shifted every
       polygon one pixel right/down and left 1-px gaps where polygons met sprites. */
    int b0 = ((v2->y - v1->y) > 0 || ((v2->y == v1->y) && (v2->x < v1->x))) ? -1 : 0;
    int b1 = ((v0->y - v2->y) > 0 || ((v0->y == v2->y) && (v0->x < v2->x))) ? -1 : 0;
    int b2 = ((v1->y - v0->y) > 0 || ((v1->y == v0->y) && (v1->x < v0->x))) ? -1 : 0;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int64_t w0 = edge(v1->x, v1->y, v2->x, v2->y, x, y);
            int64_t w1 = edge(v2->x, v2->y, v0->x, v0->y, x, y);
            int64_t w2 = edge(v0->x, v0->y, v1->x, v1->y, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            int r, g, b, u = 0, vv = 0;
            if (p->gouraud) {
                r = (int)((w0 * v0->r + w1 * v1->r + w2 * v2->r) / area);
                g = (int)((w0 * v0->g + w1 * v1->g + w2 * v2->g) / area);
                b = (int)((w0 * v0->b + w1 * v1->b + w2 * v2->b) / area);
            } else { r = p->v[0].r; g = p->v[0].g; b = p->v[0].b; }
            if (s.tex) {
                u = (int)((w0 * v0->u + w1 * v1->u + w2 * v2->u) / area);
                vv = (int)((w0 * v0->v + w1 * v1->v + w2 * v2->v) / area);
            }
            fragment(&s, x, y, r, g, b, u, vv);
        }
    }
}

static void draw_rect(const GpuPrim *p) {
    RState s;
    rstate(&s, p);
    const GpuVertex *v = &p->v[0];
    for (int j = 0; j < p->h; j++)
        for (int i = 0; i < p->w; i++)
            fragment(&s, v->x + i, v->y + j, v->r, v->g, v->b, v->u + i, v->v + j);
}

static void draw_line(const GpuPrim *p) {
    RState s;
    rstate(&s, p);
    s.tex = 0;
    const GpuVertex *a = &p->v[0], *b = &p->v[1];
    int dx = b->x - a->x, dy = b->y - a->y;
    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (n >= 1024) return;
    for (int i = 0; i <= n; i++) {
        int x = a->x + (n ? dx * i / n : 0), y = a->y + (n ? dy * i / n : 0);
        int r = a->r, g = a->g, bl = a->b;
        if (p->gouraud && n) {
            r = a->r + (b->r - a->r) * i / n;
            g = a->g + (b->g - a->g) * i / n;
            bl = a->b + (b->b - a->b) * i / n;
        }
        fragment(&s, x, y, r, g, bl, 0, 0);
    }
}

void gpu_soft_prim(const GpuPrim *p) {
    switch (p->type) {
    case PRIM_TRI: draw_tri(p); break;
    case PRIM_RECT: draw_rect(p); break;
    case PRIM_LINE: draw_line(p); break;
    default: break;
    }
}

void gpu_soft_install(void) { g_gpu_backend.prim = gpu_soft_prim; }
