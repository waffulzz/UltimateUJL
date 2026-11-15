/* English (USA) title logo on the JP title screen.
   The JP title draws Japanese lettering (LGT1/LGT2: VRAM 512,43 / 512,86, CLUT 560,7 / 560,8) plus
   an animated flame effect (8bpp, VRAM 960,0, CLUT 576,511). The USA title instead draws a 40-quad
   logo mesh from three images that only exist on the USA disc (tail of S0/CMP0.INT, CLUTs at
   752,0 / 752,2 / 752,3). While the title screen is up, this drops the JP lettering + flames and
   draws the USA mesh in their place, uploading the USA images into VRAM from the user's USA disc. */
#include "psx.h"
#include "gpu.h"
#include "title_logo_mesh.h"
#include <stdlib.h>

bool aux_disc_ready(void);
bool aux_disc_read_file(const char *path, uint8_t **data, uint32_t *size);

int g_english_title_logo = 1;

typedef struct {
    uint16_t *pix; int x, y, w, h; uint16_t clut[16]; int cx, cy;
    uint16_t *save; uint16_t save_clut[16];   /* what the game had in those VRAM spots before the logo */
} LogoTex;
static int logo_active;   /* logo images are in VRAM (title screen); restored when it ends */
static LogoTex tex[3];
static int ntex, load_tried;
static uint32_t injected_frame = 0xFFFFFFFFu, title_seen_frame = 0xFFFFFFF0u;
extern uint32_t g_gpu_frame;

static void load_textures(void) {
    load_tried = 1;
    if (!aux_disc_ready()) { LOGI("title logo: no USA disc available"); return; }
    uint8_t *d;
    uint32_t len;
    if (!aux_disc_read_file("S0\\CMP0.INT", &d, &len)) { LOGI("title logo: USA S0/CMP0.INT not found"); return; }
    for (uint32_t i = 0x2000; i + 32 <= len && ntex < 3; i += 4) {
        uint32_t magic, flags, blen;
        memcpy(&magic, d + i, 4);
        memcpy(&flags, d + i + 4, 4);
        if (magic != 0x10 || flags != 8) continue; /* 4bpp with CLUT */
        uint16_t cx, cy, cw, ch;
        memcpy(&blen, d + i + 8, 4);
        memcpy(&cx, d + i + 12, 2); memcpy(&cy, d + i + 14, 2);
        memcpy(&cw, d + i + 16, 2); memcpy(&ch, d + i + 18, 2);
        if (cx != 752 || cy > 3 || cw != 16 || ch != 1 || blen != 44) continue;
        uint32_t o = i + 8 + blen, ib;
        uint16_t ix, iy, iw, ih;
        if (o + 12 > len) continue;
        memcpy(&ib, d + o, 4);
        memcpy(&ix, d + o + 4, 2); memcpy(&iy, d + o + 6, 2);
        memcpy(&iw, d + o + 8, 2); memcpy(&ih, d + o + 10, 2);
        if (o + ib > len || (uint32_t)iw * ih * 2 + 12 != ib) continue;
        int dup = 0;
        for (int k = 0; k < ntex; k++) if (tex[k].cy == cy) dup = 1;
        if (dup) continue;
        LogoTex *t = &tex[ntex++];
        t->x = ix; t->y = iy; t->w = iw; t->h = ih; t->cx = cx; t->cy = cy;
        memcpy(t->clut, d + i + 20, 32);
        t->pix = (uint16_t *)malloc((size_t)iw * ih * 2);
        memcpy(t->pix, d + o + 12, (size_t)iw * ih * 2);
    }
    free(d);
    LOGI("title logo: loaded %d USA logo image(s)", ntex);
}

/* Are the USA logo images available (USA disc present)? Loads them on first call. */
int title_logo_available(void) {
    if (!load_tried) load_textures();
    return ntex > 0;
}

/* The USA logo images live at VRAM spots the JP game uses elsewhere (e.g. the menu's message sheet at
   704,0). Save those areas before the first upload and put them back when the title screen ends. */
static void snapshot_vram(void) {
    for (int k = 0; k < ntex; k++) {
        LogoTex *t = &tex[k];
        if (!t->save) t->save = (uint16_t *)malloc((size_t)t->w * t->h * 2);
        for (int y = 0; y < t->h; y++)
            memcpy(&t->save[y * t->w], &g_vram[((t->y + y) & 511) * 1024 + t->x], (size_t)t->w * 2);
        memcpy(t->save_clut, &g_vram[(t->cy & 511) * 1024 + t->cx], 32);
    }
}

static void restore_vram(void) {
    for (int k = 0; k < ntex; k++) {
        LogoTex *t = &tex[k];
        for (int y = 0; y < t->h; y++)
            memcpy(&g_vram[((t->y + y) & 511) * 1024 + t->x], &t->save[y * t->w], (size_t)t->w * 2);
        memcpy(&g_vram[(t->cy & 511) * 1024 + t->cx], t->save_clut, 32);
        if (g_gpu_backend.vram_write) {
            g_gpu_backend.vram_write(t->x, t->y, t->w, t->h);
            g_gpu_backend.vram_write(t->cx, t->cy, 16, 1);
        }
    }
}

/* the game uploaded to VRAM while the logo is up: anything it wrote over a logo spot is the new original */
void title_logo_note_upload(int x, int y, int w, int h) {
    if (!logo_active) return;
    for (int k = 0; k < ntex; k++) {
        LogoTex *t = &tex[k];
        int x0 = x > t->x ? x : t->x, x1 = (x + w < t->x + t->w) ? x + w : t->x + t->w;
        int y0 = y > t->y ? y : t->y, y1 = (y + h < t->y + t->h) ? y + h : t->y + t->h;
        for (int yy = y0; yy < y1; yy++)
            if (x1 > x0) memcpy(&t->save[(yy - t->y) * t->w + (x0 - t->x)], &g_vram[(yy & 511) * 1024 + x0], (size_t)(x1 - x0) * 2);
        if (t->cy >= y && t->cy < y + h && t->cx + 16 > x && t->cx < x + w)
            for (int i = 0; i < 16; i++)
                if (t->cx + i >= x && t->cx + i < x + w) t->save_clut[i] = g_vram[(t->cy & 511) * 1024 + t->cx + i];
    }
}

static void upload_textures(void) {
    if (!logo_active) { snapshot_vram(); logo_active = 1; }
    for (int k = 0; k < ntex; k++) {
        LogoTex *t = &tex[k];
        for (int y = 0; y < t->h; y++)
            memcpy(&g_vram[((t->y + y) & 511) * 1024 + t->x], &t->pix[y * t->w], (size_t)t->w * 2);
        memcpy(&g_vram[(t->cy & 511) * 1024 + t->cx], t->clut, 32);
        if (g_gpu_backend.vram_write) {
            g_gpu_backend.vram_write(t->x, t->y, t->w, t->h);
            g_gpu_backend.vram_write(t->cx, t->cy, 16, 1);
        }
    }
}

static int is_jp_lettering(const GpuPrim *p) {
    if (!p->textured || (p->texpage & 15) != 8 || ((p->texpage >> 4) & 1) || ((p->texpage >> 7) & 3) != 0) return 0;
    int cx = (p->clut & 63) * 16, cy = (p->clut >> 6) & 511;
    return cx == 560 && (cy == 7 || cy == 8);
}
static int is_jp_flame(const GpuPrim *p) {
    if (!p->textured || (p->texpage & 15) != 15 || ((p->texpage >> 4) & 1) || ((p->texpage >> 7) & 3) != 1) return 0;
    return (p->clut & 63) * 16 == 576 && ((p->clut >> 6) & 511) == 511;
}

/* Returns 1 if the primitive should be dropped. `emit` draws a replacement primitive. */
int title_logo_filter(const GpuPrim *p, void (*emit)(const GpuPrim *)) {
    if (!g_english_title_logo) return 0;
    if (logo_active && g_gpu_frame - title_seen_frame > 3) { restore_vram(); logo_active = 0; } /* title over */
    if (is_jp_lettering(p)) {
        { static uint32_t last; if (g_gpu_frame / 60 != last) { last = g_gpu_frame / 60; LOGI("title logo: lettering seen at frame %u xy(%d,%d) uv(%d,%d) w%d h%d type %d", g_gpu_frame, p->v[0].x, p->v[0].y, p->v[0].u, p->v[0].v, p->w, p->h, p->type); } }
        title_seen_frame = g_gpu_frame;
        if (!load_tried) load_textures();
        if (!ntex) return 0; /* no USA disc: the JP logo is left as is */
        if (injected_frame != g_gpu_frame) {
            injected_frame = g_gpu_frame;
            upload_textures();
            int x0 = p->clip_x0, y0 = p->clip_y0 & ~255; /* frame-buffer origin being drawn */
            for (size_t i = 0; i < sizeof k_us_logo_mesh / sizeof k_us_logo_mesh[0]; i++) {
                const short *m = k_us_logo_mesh[i];
                GpuPrim q = *p;
                q.type = PRIM_TRI;
                q.nverts = 3;
                q.textured = 1; q.raw = 0; q.semi = 0; q.gouraud = 0;
                q.texpage = (uint16_t)m[0];
                q.clut = (uint16_t)m[1];
                for (int v = 0; v < 3; v++) {
                    q.v[v].x = (int16_t)(m[2 + v * 4] + x0);
                    q.v[v].y = (int16_t)(m[3 + v * 4] + y0);
                    q.v[v].u = (uint8_t)m[4 + v * 4];
                    q.v[v].v = (uint8_t)m[5 + v * 4];
                    q.v[v].r = q.v[v].g = q.v[v].b = 128;
                    q.v[v].fx = q.v[v].fy = 0; /* not the JP polygon's sub-pixel offsets (seams across the logo) */
                }
                emit(&q);
            }
        }
        return 1;
    }
    /* flames only while the title lettering is being drawn (this or the previous frame) */
    if (is_jp_flame(p) && g_gpu_frame - title_seen_frame <= 2 && ntex) return 1;
    return 0;
}
