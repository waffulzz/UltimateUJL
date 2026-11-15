/* GP0/GP1 command processing. VRAM is kept here (16bpp, 1024x512) as the texture source of truth. */
#include "psx.h"
#include "gpu.h"
#include <stdlib.h>

uint16_t *g_vram;
GpuBackend g_gpu_backend;
GpuDisplay g_gpu_display = {0, 0, 320, 240, 0, 0, 0x260, 0xC60, 0x10, 0x100};
uint32_t g_gpu_prims_frame;
uint32_t g_gpu_uploads, g_gpu_upload_px;

static uint32_t cmd[16], cmd_addr[16]; /* cmd_addr: RAM address of each word (linked-list DMA) or ~0 */
extern uint32_t g_gpu_word_addr;
static int ncmd, need;
static int polyline;                 /* in a poly-line stream */
static uint32_t texpage, texwindow;
static int16_t dax0, day0, dax1 = 1023, day1 = 511;
static int16_t offx, offy;
static uint8_t mask_set, mask_check;
static int odd;
static uint32_t gpuread_latch;
static int irq_req;

/* VRAM transfer state */
static enum { XF_NONE, XF_TO_VRAM, XF_FROM_VRAM } xf;
static int xf_x, xf_y, xf_w, xf_h, xf_cx, xf_cy;
static int xf_rx, xf_ry;

static int gp0_len(uint32_t w) {
    uint32_t op = w >> 24;
    if (op >= 0x20 && op < 0x40) {
        int quad = (op & 8) ? 4 : 3, tex = (op & 4) != 0, gour = (op & 0x10) != 0;
        return 1 + quad * (1 + tex) + (gour ? quad - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) {
        int gour = (op & 0x10) != 0;
        if (op & 8) return -1; /* polyline: variable */
        return gour ? 4 : 3;
    }
    if (op >= 0x60 && op < 0x80) {
        int tex = (op & 4) != 0, var = ((op >> 3) & 3) == 0;
        return 2 + tex + var;
    }
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xC0) return 3;
    if (op >= 0xC0 && op < 0xE0) return 3;
    return 1;
}

static inline int16_t sx11(uint32_t v) { return (int16_t)((int32_t)(v << 21) >> 21); }

static void fill_vertex(GpuVertex *o, uint32_t xy, uint32_t col, uint32_t uv) {
    o->x = (int16_t)(sx11(xy) + offx);
    o->y = (int16_t)(sx11(xy >> 16) + offy);
    o->r = col & 0xFF;
    o->g = (col >> 8) & 0xFF;
    o->b = (col >> 16) & 0xFF;
    o->u = uv & 0xFF;
    o->v = (uv >> 8) & 0xFF;
    o->fx = o->fy = 0;
}

static void base_prim(GpuPrim *p, uint32_t op) {
    memset(p, 0, sizeof *p);
    p->semi = (op >> 1) & 1;
    p->raw = op & 1;
    p->texpage = (uint16_t)texpage;
    p->texwindow = texwindow;
    p->clip_x0 = dax0; p->clip_y0 = day0; p->clip_x1 = dax1; p->clip_y1 = day1;
    p->mask_set = mask_set; p->mask_check = mask_check;
    p->gte_ofx = 0x7FFF;
}

int g_gpu_dump_prims;
uint32_t g_glyph_clut_watch, g_glyph_packet; /* debug: RAM packet address of first prim using this CLUT */
int g_gpu_log_uploads;
FILE *g_gpu_capture; /* debug: full-detail prim capture (one line per prim) */
uint32_t g_gpu_frame;
int title_logo_filter(const GpuPrim *p, void (*emit)(const GpuPrim *));
static void emit_raw(const GpuPrim *p) {
    g_gpu_prims_frame++;
    if (g_gpu_backend.prim) g_gpu_backend.prim(p);
}
int button_swap_filter(const GpuPrim *p, GpuPrim *out);
int gte_sxy_recent(uint32_t v);
static int gte_hits(const GpuVertex *v, int n) {
    int hits = 0;
    for (int i = 0; i < n; i++)
        hits += gte_sxy_recent((uint32_t)(uint16_t)(v[i].x - offx) | ((uint32_t)(uint16_t)(v[i].y - offy) << 16));
    return hits;
}
int gte_sxy_near(int x, int y);
static int gte_near(const GpuVertex *v, int n) {
    int near = 0;
    for (int i = 0; i < n; i++) near += gte_sxy_near(v[i].x - offx, v[i].y - offy);
    return near;
}
static int g_subpix_by_addr, g_subpix_by_val;
int g_dbg_subpix_by_address = 1; /* debug A/B toggle */
static int poly_tag = -1; /* set by do_polygon for both halves of a quad */
static int poly_ofx = 0x7FFF;
int gte_sxy_ofx(uint32_t v);
int gte_sxy_off(uint32_t v, int16_t *dx, int16_t *dy);
int gte_addr_off(uint32_t phys, uint32_t val, int16_t *dx, int16_t *dy);
static int gte_ofx_of(const GpuVertex *v, int n) { /* OFX of the first exactly matched vertex, VRAM coords */
    for (int i = 0; i < n; i++) {
        int o = gte_sxy_ofx((uint32_t)(uint16_t)(v[i].x - offx) | ((uint32_t)(uint16_t)(v[i].y - offy) << 16));
        if (o != 0x7FFF) return o + offx;
    }
    return 0x7FFF;
}
static uint8_t is_gte3d(const GpuPrim *p) {
    if (p->type != PRIM_TRI && p->type != PRIM_LINE) return 0;
    int hits = 0;
    for (int i = 0; i < p->nverts; i++) {
        uint32_t v = (uint32_t)(uint16_t)(p->v[i].x - offx) | ((uint32_t)(uint16_t)(p->v[i].y - offy) << 16);
        hits += gte_sxy_recent(v);
    }
    return hits >= (p->nverts == 3 ? 2 : 2);
}
int tooltip_english_active(void);
static void emit(const GpuPrim *p) {
    if (title_logo_filter(p, emit_raw)) return;
    { extern int mc_text_filter(const GpuPrim *, void (*)(const GpuPrim *)); if (mc_text_filter(p, emit_raw)) return; }
    GpuPrim swapped;
    if (button_swap_filter(p, &swapped)) p = &swapped;
    GpuPrim tagged;
    if (p->type == PRIM_TRI || p->type == PRIM_LINE) {
        tagged = *p;
        tagged.gte3d = (p->type == PRIM_TRI && poly_tag >= 0) ? (uint8_t)poly_tag : is_gte3d(p);
        if (tagged.gte3d) tagged.gte_ofx = (int16_t)(p->type == PRIM_TRI && poly_tag >= 0 ? poly_ofx : gte_ofx_of(p->v, p->nverts));
        p = &tagged;
    }
    g_gpu_prims_frame++;
    if (g_glyph_clut_watch && !g_glyph_packet && p->textured && p->clut == g_glyph_clut_watch) {
        extern uint32_t g_dma_cur_packet;
        g_glyph_packet = g_dma_cur_packet;
    }
    if (g_gpu_capture)
        fprintf(g_gpu_capture, "%d %d %u %u %u %u %u %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                p->type, p->nverts, p->texpage, p->clut, p->semi, p->raw, p->gouraud, p->w, p->h,
                p->v[0].x, p->v[0].y, p->v[0].u, p->v[0].v, p->v[1].x, p->v[1].y, p->v[1].u, p->v[1].v,
                p->v[2].x, p->v[2].y, p->v[2].u, p->v[2].v, p->v[0].r, p->v[0].g, p->v[0].b, p->gte3d,
                p->v[0].fx, p->v[0].fy, p->v[1].fx, p->v[1].fy, p->v[2].fx, p->v[2].fy);
    if (g_gpu_dump_prims && p->textured && (g_gpu_dump_prims == 2 || p->v[0].y < 70)) {
        int tx = (p->texpage & 15) * 64, ty = ((p->texpage >> 4) & 1) * 256, dep = (p->texpage >> 7) & 3;
        rt_log("prim type%d scr(%d,%d)-(%d,%d) w%d h%d tpage(%d,%d) %dbpp clut(%d,%d) uv(%d,%d)-(%d,%d)",
               p->type, p->v[0].x, p->v[0].y, p->v[p->nverts > 1 ? 2 : 0].x, p->v[p->nverts > 1 ? 2 : 0].y, p->w, p->h,
               tx, ty, dep == 0 ? 4 : dep == 1 ? 8 : 16, (p->clut & 63) * 16, (p->clut >> 6) & 511,
               p->v[0].u, p->v[0].v, p->v[p->nverts > 1 ? 2 : 0].u, p->v[p->nverts > 1 ? 2 : 0].v);
    }
    if (g_gpu_backend.prim) g_gpu_backend.prim(p);
}

static void do_polygon(void) {
    uint32_t op = cmd[0] >> 24;
    int quad = (op & 8) != 0, tex = (op & 4) != 0, gour = (op & 0x10) != 0;
    int n = quad ? 4 : 3;
    GpuVertex v[4];
    int k = 0, xyk[4];
    uint32_t col = cmd[k++] & 0xFFFFFF;
    uint16_t clut = 0, tpage = (uint16_t)texpage;
    for (int i = 0; i < n; i++) {
        if (i > 0 && gour) col = cmd[k++] & 0xFFFFFF;
        xyk[i] = k;
        uint32_t xy = cmd[k++], uv = 0;
        if (tex) {
            uv = cmd[k++];
            if (i == 0) clut = (uint16_t)(uv >> 16);
            if (i == 1) tpage = (uint16_t)(uv >> 16);
        }
        fill_vertex(&v[i], xy, col, uv);
    }
    if (tex) {
        /* textured polys update the global texpage (bits 0-8, 11) */
        texpage = (texpage & ~0x9FFu) | (tpage & 0x9FF);
    }
    GpuPrim p;
    base_prim(&p, op);
    p.type = PRIM_TRI;
    p.nverts = 3;
    p.textured = (uint8_t)tex;
    p.gouraud = (uint8_t)gour;
    p.clut = clut;
    p.texpage = (uint16_t)texpage;
    p.v[0] = v[0]; p.v[1] = v[1]; p.v[2] = v[2];
    /* 3D (GTE-projected) or 2D: decided for the whole polygon, so both halves of a quad agree. Cartoon outlines
       offset some projected vertices on the CPU, which leaves only 2 of a quad's 4 vertices as exact matches. */
    int exact = gte_hits(v, n);
    poly_tag = exact >= 2 || gte_near(v, n) >= 2;
    {   /* UJL_SUBPIX_STATS: how many 3D polygons get exact vertex matches (sub-pixel coverage) */
        static int stats = -1, hist[2][5], last;
        if (stats < 0) stats = getenv("UJL_SUBPIX_STATS") != NULL;
        if (stats) {
            if (poly_tag) hist[quad][exact]++;
            if ((int)g_gpu_frame / 120 != last) {
                last = (int)g_gpu_frame / 120;
                rt_log("subpix: tri hits0-3 %d %d %d %d | quad hits0-4 %d %d %d %d %d | by address %d, by value %d", hist[0][0], hist[0][1], hist[0][2], hist[0][3],
                       hist[1][0], hist[1][1], hist[1][2], hist[1][3], hist[1][4], g_subpix_by_addr, g_subpix_by_val);
                g_subpix_by_addr = g_subpix_by_val = 0;
                memset(hist, 0, sizeof hist);
            }
        }
    }
    poly_ofx = poly_tag ? gte_ofx_of(v, n) : 0x7FFF;
    /* Sub-pixel position for the exactly matched vertices of 3D polygons only. 2D polygons whose corners happen
       to land on a projected point (the US title logo, Stage 1's 240 one-line wave strips, which copy the
       screen at 16bpp) got random offsets from unrelated geometry: seams on the logo, a choppy wave. */
    if (exact >= 2 && ((texpage >> 7) & 3) != 2)
        for (int i = 0; i < n; i++) {
            if (g_dbg_subpix_by_address && gte_addr_off(cmd_addr[xyk[i]], cmd[xyk[i]], &v[i].fx, &v[i].fy)) g_subpix_by_addr++; /* by address: no collisions */
            else { g_subpix_by_val++; gte_sxy_off((uint32_t)(uint16_t)(v[i].x - offx) | ((uint32_t)(uint16_t)(v[i].y - offy) << 16), &v[i].fx, &v[i].fy); }
        }
    p.v[0] = v[0]; p.v[1] = v[1]; p.v[2] = v[2];
    emit(&p);
    if (quad) {
        p.v[0] = v[1]; p.v[1] = v[2]; p.v[2] = v[3];
        emit(&p);
    }
    poly_tag = -1;
}

static void do_line(GpuVertex a, GpuVertex b, uint32_t op) {
    GpuPrim p;
    base_prim(&p, op);
    p.type = PRIM_LINE;
    p.nverts = 2;
    p.gouraud = (op & 0x10) != 0;
    p.v[0] = a; p.v[1] = b;
    emit(&p);
}

static void do_rect(void) {
    uint32_t op = cmd[0] >> 24;
    int tex = (op & 4) != 0, sz = (op >> 3) & 3;
    int k = 1;
    GpuVertex v;
    uint32_t xy = cmd[k++], uv = 0;
    if (tex) uv = cmd[k++];
    fill_vertex(&v, xy, cmd[0], uv);
    int w, h;
    if (sz == 0) { w = cmd[k] & 0x3FF; h = (cmd[k] >> 16) & 0x1FF; }
    else if (sz == 1) w = h = 1;
    else if (sz == 2) w = h = 8;
    else w = h = 16;
    GpuPrim p;
    base_prim(&p, op);
    p.type = PRIM_RECT;
    p.nverts = 1;
    p.textured = (uint8_t)tex;
    p.clut = (uint16_t)(uv >> 16);
    p.v[0] = v;
    p.w = (int16_t)w; p.h = (int16_t)h;
    emit(&p);
}

static void vram_put(int x, int y, uint16_t pix) {
    x &= 1023; y &= 511;
    uint16_t *d = &g_vram[y * 1024 + x];
    if (mask_check && (*d & 0x8000)) return;
    *d = pix | (mask_set ? 0x8000 : 0);
}

static void gp0_exec(void) {
    uint32_t op = cmd[0] >> 24;
    if (op >= 0x20 && op < 0x40) { do_polygon(); return; }
    if (op >= 0x40 && op < 0x60) {
        GpuVertex a, b;
        int gour = (op & 0x10) != 0;
        fill_vertex(&a, cmd[1], cmd[0], 0);
        fill_vertex(&b, cmd[gour ? 3 : 2], gour ? cmd[2] : cmd[0], 0);
        do_line(a, b, op);
        return;
    }
    if (op >= 0x60 && op < 0x80) { do_rect(); return; }
    switch (op) {
    case 0x02: { /* fill rectangle (ignores mask/draw area) */
        int x = cmd[1] & 0x3F0, y = (cmd[1] >> 16) & 0x1FF;
        int w = ((cmd[2] & 0x3FF) + 15) & ~15, h = (cmd[2] >> 16) & 0x1FF;
        uint32_t c = cmd[0];
        uint16_t pix = (uint16_t)(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = pix;
        if (g_gpu_backend.fill) g_gpu_backend.fill(x, y, w, h, c & 0xFFFFFF);
        break;
    }
    case 0x1F: irq_req = 1; irq_raise(IRQ_GPU); break;
    case 0xE1: texpage = cmd[0] & 0x3FFF; break;
    case 0xE2: texwindow = cmd[0] & 0xFFFFF; break;
    case 0xE3: dax0 = cmd[0] & 0x3FF; day0 = (cmd[0] >> 10) & 0x3FF; break;
    case 0xE4: dax1 = cmd[0] & 0x3FF; day1 = (cmd[0] >> 10) & 0x3FF; break;
    case 0xE5: offx = sx11(cmd[0]); offy = sx11(cmd[0] >> 11); break;
    case 0xE6: mask_set = cmd[0] & 1; mask_check = (cmd[0] >> 1) & 1; break;
    default:
        if (op >= 0x80 && op < 0xA0) { /* VRAM -> VRAM */
            int sx = cmd[1] & 0x3FF, sy = (cmd[1] >> 16) & 0x1FF;
            int dx = cmd[2] & 0x3FF, dy = (cmd[2] >> 16) & 0x1FF;
            int w = ((cmd[3] - 1) & 0x3FF) + 1, h = (((cmd[3] >> 16) - 1) & 0x1FF) + 1;
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    vram_put(dx + i, dy + j, g_vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)]);
            if (g_gpu_backend.vram_copy) g_gpu_backend.vram_copy(sx, sy, dx, dy, w, h);
        } else if (op >= 0xA0 && op < 0xC0) { /* CPU -> VRAM */
            xf = XF_TO_VRAM;
            xf_x = cmd[1] & 0x3FF; xf_y = (cmd[1] >> 16) & 0x1FF;
            xf_w = ((cmd[2] - 1) & 0x3FF) + 1; xf_h = (((cmd[2] >> 16) - 1) & 0x1FF) + 1;
            xf_cx = xf_cy = 0;
            g_gpu_uploads++; g_gpu_upload_px += (uint32_t)(xf_w * xf_h);
            { extern int g_gpu_log_uploads; if (g_gpu_log_uploads) rt_log("upload %dx%d at (%d,%d)", xf_w, xf_h, xf_x, xf_y); }
            { static int n; if (g_gpu_uploads == 2000 && n++ < 2) rt_dump_stack("vram upload"); }
        } else if (op >= 0xC0 && op < 0xE0) { /* VRAM -> CPU */
            xf = XF_FROM_VRAM;
            xf_x = cmd[1] & 0x3FF; xf_y = (cmd[1] >> 16) & 0x1FF;
            { static int n; if (n++ < 20) rt_log("gpu: VRAM->CPU readback %dx%d at (%d,%d)", ((cmd[2] - 1) & 0x3FF) + 1, (((cmd[2] >> 16) - 1) & 0x1FF) + 1, xf_x, xf_y); }
            xf_w = ((cmd[2] - 1) & 0x3FF) + 1; xf_h = (((cmd[2] >> 16) - 1) & 0x1FF) + 1;
            xf_rx = xf_ry = 0;
            if (g_gpu_backend.vram_read) g_gpu_backend.vram_read(xf_x, xf_y, xf_w, xf_h);
        }
        break;
    }
}

static void xf_pixel(uint16_t p) {
    vram_put(xf_x + xf_cx, xf_y + xf_cy, p);
    if (++xf_cx >= xf_w) {
        xf_cx = 0;
        if (++xf_cy >= xf_h) {
            xf = XF_NONE;
            { extern void title_logo_note_upload(int, int, int, int); title_logo_note_upload(xf_x, xf_y, xf_w, xf_h); }
            { extern void tooltip_note_upload(int, int, int, int); tooltip_note_upload(xf_x, xf_y, xf_w, xf_h); }
            if (g_gpu_backend.vram_write) g_gpu_backend.vram_write(xf_x, xf_y, xf_w, xf_h);
        }
    }
}

void gpu_gp0(uint32_t w) {
    if (xf == XF_TO_VRAM) {
        xf_pixel((uint16_t)w);
        if (xf == XF_TO_VRAM) xf_pixel((uint16_t)(w >> 16));
        return;
    }
    if (polyline) {
        static uint32_t next_col;
        static int have_col;
        uint32_t op = cmd[0] >> 24;
        int gour = (op & 0x10) != 0;
        /* terminator only recognised where a vertex/color word is expected after the first segment */
        if (ncmd >= 2 && (w & 0xF000F000u) == 0x50005000u) { polyline = 0; ncmd = 0; have_col = 0; return; }
        if (ncmd == 1) { cmd[1] = w; ncmd = 2; return; }     /* first vertex */
        if (gour && !have_col) { next_col = w; have_col = 1; return; }
        GpuVertex a, b;
        fill_vertex(&a, cmd[1], cmd[0], 0);
        uint32_t cb = gour ? next_col : cmd[0];
        fill_vertex(&b, w, cb, 0);
        do_line(a, b, op);
        cmd[0] = (cmd[0] & 0xFF000000u) | (cb & 0xFFFFFF);
        cmd[1] = w;
        have_col = 0;
        return;
    }
    if (ncmd == 0) {
        need = gp0_len(w);
        if (need < 0) { polyline = 1; cmd[0] = w; ncmd = 1; return; }
    }
    cmd_addr[ncmd] = g_gpu_word_addr;
    cmd[ncmd++] = w;
    if (ncmd >= need) {
        gp0_exec();
        ncmd = 0;
    }
}

static uint32_t gp1_info;
static int dma_dir, disp_off = 1;

void gpu_gp1(uint32_t w) {
    uint32_t op = (w >> 24) & 0x3F;
    switch (op) {
    case 0x00: ncmd = 0; polyline = 0; xf = XF_NONE; disp_off = 1; texpage = 0; dax0 = day0 = 0; offx = offy = 0; break;
    case 0x01: ncmd = 0; polyline = 0; xf = XF_NONE; break;
    case 0x02: irq_req = 0; break;
    case 0x03: disp_off = w & 1; g_gpu_display.enabled = !disp_off; break;
    case 0x04: dma_dir = w & 3; break;
    case 0x05: {
        int ny = (w >> 10) & 0x1FF;
        {   /* UJL_FLIP_STATS: vblanks between display flips (30 fps games should show 2,2,2...) */
            static int st = -1, hist[8], n; static uint32_t lastf;
            if (st < 0) st = getenv("UJL_FLIP_STATS") != NULL;
            if (st && ny != g_gpu_display.disp_y) {
                uint32_t d = g_gpu_frame - lastf; lastf = g_gpu_frame;
                hist[d > 7 ? 7 : d]++;
                if (d >= 3 && d < 8) rt_log("flip: %u vblanks at frame %u", d, g_gpu_frame);
                if (++n % 300 == 0) { rt_log("flips: 1:%d 2:%d 3:%d 4+:%d", hist[1], hist[2], hist[3], hist[4] + hist[5] + hist[6] + hist[7]); memset(hist, 0, sizeof hist); }
            }
        }
        g_gpu_display.disp_x = w & 0x3FE; g_gpu_display.disp_y = ny; break;
    }
    case 0x06: g_gpu_display.x1 = w & 0xFFF; g_gpu_display.x2 = (w >> 12) & 0xFFF; break;
    case 0x07: g_gpu_display.y1 = w & 0x3FF; g_gpu_display.y2 = (w >> 10) & 0x3FF; break;
    case 0x08: {
        static const int hr[4] = {256, 320, 512, 640};
        g_gpu_display.hres = (w & 0x40) ? 368 : hr[w & 3];
        g_gpu_display.vres = ((w & 4) && (w & 0x20)) ? 480 : 240;
        g_gpu_display.depth24 = (w >> 4) & 1;
        gp1_info = w & 0xFF;
        break;
    }
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (w & 7) {
        case 2: gpuread_latch = texwindow; break;
        case 3: gpuread_latch = (uint32_t)dax0 | ((uint32_t)day0 << 10); break;
        case 4: gpuread_latch = (uint32_t)dax1 | ((uint32_t)day1 << 10); break;
        case 5: gpuread_latch = ((uint32_t)offx & 0x7FF) | (((uint32_t)offy & 0x7FF) << 11); break;
        case 7: gpuread_latch = 2; break;
        }
        break;
    default: break;
    }
}

uint32_t gpu_read(void) {
    if (xf == XF_FROM_VRAM) {
        uint32_t v = 0;
        for (int k = 0; k < 2; k++) {
            uint16_t p = g_vram[((xf_y + xf_ry) & 511) * 1024 + ((xf_x + xf_rx) & 1023)];
            v |= (uint32_t)p << (16 * k);
            if (++xf_rx >= xf_w) { xf_rx = 0; if (++xf_ry >= xf_h) { xf = XF_NONE; break; } }
        }
        return v;
    }
    return gpuread_latch;
}

uint32_t gpu_stat(void) {
    uint32_t s = (texpage & 0x7FF) | ((uint32_t)mask_set << 11) | ((uint32_t)mask_check << 12);
    s |= 1u << 13; /* interlace field */
    s |= (gp1_info & 0x3F) << 17 | ((gp1_info >> 6) & 1) << 16;
    s |= (uint32_t)disp_off << 23;
    s |= (uint32_t)irq_req << 24;
    s |= 1u << 26;                                   /* ready for command */
    s |= (xf == XF_FROM_VRAM ? 1u : 0u) << 27;       /* ready to send VRAM */
    s |= 1u << 28;                                   /* ready for DMA block */
    s |= (uint32_t)dma_dir << 29;
    if (dma_dir == 1 || dma_dir == 2) s |= 1u << 25;
    else if (dma_dir == 3 && xf == XF_FROM_VRAM) s |= 1u << 25;
    s |= (uint32_t)odd << 31;
    return s;
}

void gpu_vblank(void) { odd ^= 1; g_gpu_frame++; }

void gpu_init(void) {
    g_vram = (uint16_t *)calloc(1024 * 512, 2);
}
