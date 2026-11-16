/* Disc patch layer: files are patched in memory and served in place of the original sectors
   when the game reads them, so the JP game code sees modified assets with no code changes.
   Used for the English-version title logo (and, next, English captions). */
#include "psx.h"
#include <stdlib.h>
#include "gpu.h"
#include "recomp.h"
#include <string.h>
#include <SDL3/SDL_timer.h>

bool disc_read_file(const char *path, uint8_t **data, uint32_t *size);

typedef struct {
    uint32_t lba, nsec;
    uint8_t *data;   /* patched file contents, nsec * 2048 bytes */
} PatchedFile;

#define MAX_PATCHED 64
static PatchedFile files[MAX_PATCHED];
static int nfiles;
int g_patch_english_logo = 1;
int g_patch_english_menus = 1;   /* swap in the US disc's English versions of same-sized menu textures */
bool aux_disc_ready(void);
bool aux_disc_read_file(const char *path, uint8_t **data, uint32_t *size);

/* ---- INT archive helpers (header, 20-byte name entries, TIMs from 0x2000) ---- */
typedef struct { char name[17]; uint32_t off, size; } IntEntry;

static int int_find(const uint8_t *d, uint32_t len, const char *name, IntEntry *out) {
    if (len < 16) return 0;
    uint32_t n;
    memcpy(&n, d + 4, 4);
    if (n > 4096 || 16 + n * 20 > len) return 0;
    uint32_t pos = (16 + n * 20 + 0x1FFF) & ~0x1FFFu;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = d + 16 + i * 20;
        uint32_t size;
        memcpy(&size, e, 4);
        char nm[17] = {0};
        memcpy(nm, e + 4, 16);
        if (!strcmp(nm, name)) {
            if (pos + size > len) return 0;
            memcpy(out->name, nm, 17);
            out->off = pos;
            out->size = size;
            return 1;
        }
        pos += (size + 3) & ~3u;
    }
    return 0;
}

/* Make a 4/8bpp TIM fully transparent: zero its CLUT entries and pixel indices. */
static int tim_blank(uint8_t *t, uint32_t size) {
    uint32_t magic, flags, off = 8;
    memcpy(&magic, t, 4);
    memcpy(&flags, t + 4, 4);
    if (magic != 0x10) return 0;
    if (flags & 8) {
        uint32_t blen;
        memcpy(&blen, t + off, 4);
        if (off + blen > size) return 0;
        memset(t + off + 12, 0, blen - 12);
        off += blen;
    }
    uint32_t blen;
    memcpy(&blen, t + off, 4);
    if (off + blen > size) return 0;
    memset(t + off + 12, 0, blen - 12);
    return 1;
}

static PatchedFile *load_for_patch(const char *path) {
    uint32_t lba, size;
    if (!disc_find_file(path, &lba, &size)) return NULL;
    for (int i = 0; i < nfiles; i++)
        if (files[i].lba == lba) return &files[i];
    if (nfiles >= MAX_PATCHED) return NULL;
    uint8_t *data;
    uint32_t got;
    if (!disc_read_file(path, &data, &got)) return NULL;
    PatchedFile *f = &files[nfiles++];
    f->lba = lba;
    f->nsec = (size + 2047) / 2048;
    f->data = (uint8_t *)calloc(f->nsec, 2048);
    memcpy(f->data, data, got);
    free(data);
    return f;
}

static void blank_tims(const char *file, const char *const *names, int n) {
    PatchedFile *f = load_for_patch(file);
    if (!f) { LOGI("patch: %s not found", file); return; }
    for (int i = 0; i < n; i++) {
        IntEntry e;
        if (int_find(f->data, f->nsec * 2048, names[i], &e) && tim_blank(f->data + e.off, e.size))
            LOGI("patch: %s:%s blanked (file lba %u, tim at +%x)", file, names[i], f->lba, e.off);
        else
            LOGI("patch: %s:%s not found", file, names[i]);
    }
}

/* Some TIMs live in an unindexed tail section of an INT archive. Blank every TIM in the file
   (indexed or not) whose image is uploaded to VRAM (x,y) with size w x h (in VRAM halfwords). */
static void blank_tims_at_vram(const char *file, int vx, int vy, int vw, int vh) {
    PatchedFile *f = load_for_patch(file);
    if (!f) return;
    uint32_t len = f->nsec * 2048;
    int n = 0;
    for (uint32_t i = 0; i + 32 <= len; i += 4) {
        uint8_t *t = f->data + i;
        uint32_t magic, flags;
        memcpy(&magic, t, 4);
        memcpy(&flags, t + 4, 4);
        if (magic != 0x10 || (flags & ~0xBu) || (flags & 3) > 1) continue;
        uint32_t off = 8, blen;
        if (flags & 8) { memcpy(&blen, t + off, 4); if (blen < 12 || blen > 0x10000) continue; off += blen; }
        if (i + off + 12 > len) continue;
        uint16_t ix, iy, iw, ih;
        memcpy(&blen, t + off, 4);
        memcpy(&ix, t + off + 4, 2); memcpy(&iy, t + off + 6, 2);
        memcpy(&iw, t + off + 8, 2); memcpy(&ih, t + off + 10, 2);
        if (ix == vx && iy == vy && iw == vw && ih == vh && i + off + blen <= len) {
            if (tim_blank(t, off + blen)) n++;
        }
    }
    LOGI("patch: %s: blanked %d TIM(s) targeting VRAM (%d,%d) %dx%d", file, n, vx, vy, vw, vh);
}

/* Locate the image block of a TIM: returns offset of the pixel block header (len,x,y,w,h). */
static int tim_layout(const uint8_t *t, uint32_t avail, uint32_t *clut_off, uint32_t *img_off, uint32_t *total) {
    uint32_t magic, flags, off = 8, blen;
    if (avail < 32) return 0;
    memcpy(&magic, t, 4);
    memcpy(&flags, t + 4, 4);
    if (magic != 0x10 || (flags & ~0xBu) || (flags & 3) > 1) return 0;
    *clut_off = 0;
    if (flags & 8) {
        memcpy(&blen, t + off, 4);
        if (blen < 12 || blen > 0x10000 || off + blen + 12 > avail) return 0;
        *clut_off = off;
        off += blen;
    }
    memcpy(&blen, t + off, 4);
    if (blen < 12 || off + blen > avail) return 0;
    *img_off = off;
    *total = off + blen;
    return 1;
}

/* Replace every TIM in `file` whose image goes to VRAM (vx,vy) and has the same block sizes as
   `src` (a TIM) with `src`'s CLUT and pixels. Returns how many were replaced. */
static int replace_tims_at_vram(const char *file, int vx, int vy, const uint8_t *src, uint32_t src_size) {
    uint32_t s_clut, s_img, s_total;
    if (!tim_layout(src, src_size, &s_clut, &s_img, &s_total)) return 0;
    uint16_t sw, sh;
    memcpy(&sw, src + s_img + 8, 2);
    memcpy(&sh, src + s_img + 10, 2);
    PatchedFile *f = load_for_patch(file);
    if (!f) return 0;
    uint32_t len = f->nsec * 2048;
    int n = 0;
    for (uint32_t i = 0; i + 32 <= len; i += 4) {
        uint8_t *t = f->data + i;
        uint32_t c, im, tot;
        if (!tim_layout(t, len - i, &c, &im, &tot)) continue;
        uint16_t ix, iy, iw, ih;
        memcpy(&ix, t + im + 4, 2); memcpy(&iy, t + im + 6, 2);
        memcpy(&iw, t + im + 8, 2); memcpy(&ih, t + im + 10, 2);
        if (ix != vx || iy != vy || iw != sw || ih != sh || tot != s_total || im != s_img) continue;
        if (t == src) continue;
        if (c && s_clut) {
            uint32_t cl;
            memcpy(&cl, t + c, 4);
            memcpy(t + c + 12, src + s_clut + 12, cl - 12); /* palette entries (keep VRAM position) */
        }
        memcpy(t + im + 12, src + s_img + 12, tot - im - 12); /* pixels */
        n++;
        i += tot - 4;
    }
    return n;
}

/* DISABLED: WPn.INT turned out to be a Japanese kana font sheet, not English captions. */
int g_patch_english_captions = 0;

/* English caption sheets: the JP release added Japanese caption sheets (MVSTnSQ, VRAM 768,256)
   to each stage's archives. The original English sheets, same layout, still ship on the JP
   disc in Sn/WPn.INT, which is identical on the USA disc. Copy them over the Japanese ones. */
static void english_captions(void) {
    for (int st = 1; st <= 8; st++) {
        char wp[32];
        snprintf(wp, sizeof wp, "S%d\\WP%d.INT", st, st);
        uint8_t *wd;
        uint32_t wsize;
        if (!disc_read_file(wp, &wd, &wsize)) continue;
        uint32_t c, im, tot;
        if (wsize < 0x2000 + 32 || !tim_layout(wd + 0x2000, wsize - 0x2000, &c, &im, &tot)) { free(wd); continue; }
        const uint8_t *src = wd + 0x2000;
        int total = 0, nf = 0;
        /* every INT in this stage's folder that carries a Japanese sheet */
        static const char *const kinds[] = {"SNG", "CMP", "COP"};
        for (int k = 0; k < 3; k++)
            for (int p = 0; p < 2; p++) {
                char name[40];
                if (p == 0) snprintf(name, sizeof name, "S%d\\CMP%d%s.INT", st, st, kinds[k]);
                else snprintf(name, sizeof name, "S%d\\CMP1%d%s.INT", st, st, kinds[k]);
                uint32_t lba, sz;
                if (!disc_find_file(name, &lba, &sz)) continue;
                int n = replace_tims_at_vram(name, 768, 256, src, wsize - 0x2000);
                if (n) { total += n; nf++; }
            }
        LOGI("patch: stage %d: English captions -> %d sheet(s) in %d archive(s)", st, total, nf);
        free(wd);
    }
}

/* For every TIM in `file` (indexed or in the unindexed tail) find a TIM in the US disc's copy of the same
   file that uploads to the same VRAM rect with the same depth and CLUT position; if its pixels differ, take
   the US pixels and palette. This localizes menu textures that kept their layout (HI-SCORE banner, the
   OPTION candy bar, the English font, credits). Text the US version draws with its font instead (memory card
   messages, the tooltip bar) has no counterpart here. */
typedef struct { uint32_t off, clut_off, img_off, total; uint16_t r[4], c[2]; uint32_t flags; } TimRef;
static int scan_tim_refs(const uint8_t *d, uint32_t len, TimRef *out, int max) {
    int n = 0;
    for (uint32_t i = 0; i + 32 <= len && n < max; i += 4) {
        uint32_t co, io, tot;
        if (!tim_layout(d + i, len - i, &co, &io, &tot)) continue;
        TimRef *t = &out[n++];
        t->off = i; t->clut_off = co; t->img_off = io; t->total = tot;
        memcpy(&t->flags, d + i + 4, 4);
        memcpy(t->r, d + i + io + 4, 8);
        if (co) memcpy(t->c, d + i + co + 4, 4); else t->c[0] = t->c[1] = 0xFFFF;
        i += (tot & ~3u) - 4 > 0 ? (tot & ~3u) - 4 : 0;
    }
    return n;
}

static void swap_tims_from_aux(const char *file) {
    if (!aux_disc_ready()) return;
    uint8_t *us;
    uint32_t uslen;
    if (!aux_disc_read_file(file, &us, &uslen)) return;
    PatchedFile *f = load_for_patch(file);
    if (!f) { free(us); return; }
    uint32_t len = f->nsec * 2048;
    static TimRef ja[1024], ua[1024];
    int nj = scan_tim_refs(f->data, len, ja, 1024), nu = scan_tim_refs(us, uslen, ua, 1024);
    int swapped = 0;
    for (int i = 0; i < nj; i++) {
        TimRef *j = &ja[i];
        for (int k = 0; k < nu; k++) {
            TimRef *u = &ua[k];
            if (u->flags != j->flags || u->total != j->total || memcmp(u->r, j->r, 8) || memcmp(u->c, j->c, 4)) continue;
            if (!memcmp(f->data + j->off, us + u->off, j->total)) break; /* identical already */
            memcpy(f->data + j->off, us + u->off, j->total);
            swapped++;
            break;
        }
    }
    free(us);
    LOGI("patch: %s: %d texture(s) replaced with the US disc's English version", file, swapped);
}

/* ---- English tooltip bar (menus) ----
   The candy-bar menus draw their tooltip line from a word sheet TIM (832,350) as up to five word sprites per
   message (38 messages). JP: sheet 64x98, 14-px rows, CLUT (832,448); the menu overlay COMOD0.MEN builds each
   word from per-slot tables (x, and u/v/width bytes) with a fixed height of 14. US: sheet 64x112 with 16-px
   rows, CLUT (832,462), and a different builder (fixed first word, then three or four words). The US layout fits
   JP's five general slots, so the JP overlay's tables are rewritten with the US words, the JP sheet TIM gets the
   US pixels (first 98 rows; the last 14 rows are uploaded to (832,448) right after the sheet loads) with its
   CLUT moved to (832,462) like on the US disc, and sprites from that CLUT are drawn 16 px tall (gpu.c). */
int g_patch_english_tooltips = 1;
static int tooltip_ready;
static uint16_t tooltip_sheet[112][64]; /* US word sheet (64x112 halfwords, 4bpp) */
static uint16_t tooltip_clut[16];
#define JP_OV_BASE 0x801c5110u
#define US_OV_BASE 0x801c5750u

static int find_tim_rect(const uint8_t *d, uint32_t len, int x, int y, int w, int h, uint32_t *at, int start) {
    for (uint32_t i = (uint32_t)start; i + 32 <= len; i += 4) {
        uint32_t co, io, tot;
        if (!tim_layout(d + i, len - i, &co, &io, &tot) || !co) continue;
        uint16_t r[4];
        memcpy(r, d + i + io + 4, 8);
        if (r[0] == x && r[1] == y && r[2] == w && r[3] == h) { *at = i; return 1; }
    }
    return 0;
}

static void english_tooltips(void) {
    if (!aux_disc_ready() || getenv("UJL_JP_TOOLTIPS")) return;
    uint8_t *usov = NULL, *usint = NULL;
    uint32_t usov_len, usint_len;
    if (!aux_disc_read_file("S0/COMOD0.MEN", &usov, &usov_len)) { LOGI("patch: tooltip: no US COMOD0.MEN"); return; }
    if (!aux_disc_read_file("S0/CMP0.INT", &usint, &usint_len)) { free(usov); return; }
    PatchedFile *ov = load_for_patch("S0/COMOD0.MEN"), *arc = load_for_patch("S0/CMP0.INT");
    #define JO(a) ((a) - JP_OV_BASE)
    #define UO(a) ((a) - US_OV_BASE)
    if (!ov || !arc || ov->nsec * 2048 < JO(0x801c5c10u) || usov_len < UO(0x801c6410u)) { LOGI("patch: tooltip: files missing (%p %p)", (void *)ov, (void *)arc); goto out; }
    {   /* sanity: the JP slot-0 x table starts -64, -138 and the US header words look like sheet rects */
        int16_t j0, j1;
        memcpy(&j0, ov->data + JO(0x801c5848u), 2); memcpy(&j1, ov->data + JO(0x801c584au), 2);
        const uint8_t *hdr = usov + UO(0x801c6230u);
        if (j0 != -64 || j1 != -138 || hdr[0] + hdr[2] > 256 || hdr[3] + hdr[5] > 256 || hdr[1] % 16 || hdr[4] % 16) {
            LOGI("patch: tooltip tables not where expected, leaving the tooltip bar Japanese");
            goto out;
        }
    }
    {
        static const uint32_t jx[5] = {0x801c5848u, 0x801c5894u, 0x801c58e0u, 0x801c592cu, 0x801c5978u};
        static const uint32_t juvw[5] = {0x801c59c4u, 0x801c5a38u, 0x801c5aacu, 0x801c5b24u, 0x801c5b98u};
        const uint8_t *hdr = usov + UO(0x801c6230u);
        const uint8_t *ua = usov + UO(0x801c6238u), *ub = usov + UO(0x801c62acu);
        const uint8_t *uc = usov + UO(0x801c6324u), *ud = usov + UO(0x801c6398u);
        for (int m = 0; m < 38; m++) {
            int16_t xc, xd, x[5] = {0};
            const uint8_t *w[5] = {0};
            memcpy(&xc, usov + UO(0x801c6198u) + 2 * m, 2);
            memcpy(&xd, usov + UO(0x801c61e4u) + 2 * m, 2);
            static const uint8_t none[3] = {0, 0, 0};
            if (m == 0) { x[0] = 96 - 160; w[0] = hdr; }
            else {
                x[0] = 22 - 160; w[0] = hdr + 3;
                x[1] = 60 - 160; w[1] = ua + 3 * m;
                x[2] = 74 - 160; w[2] = ub + 3 * m;
                x[3] = (int16_t)(xc - 160); w[3] = uc + 3 * m;
                if (xd && xd != 160) { x[4] = (int16_t)(xd - 160); w[4] = ud + 3 * m; }
            }
            for (int s = 0; s < 5; s++) {
                memcpy(ov->data + JO(jx[s]) + 2 * m, &x[s], 2);
                memcpy(ov->data + JO(juvw[s]) + 3 * m, w[s] ? w[s] : none, 3);
            }
        }
    }
    {   /* the US sheet goes to free VRAM next to the JP one (see tooltip_note_upload) */
        uint32_t ut, uco, uio, utot;
        if (!find_tim_rect(usint, usint_len, 832, 350, 64, 112, &ut, 0)) { LOGI("patch: tooltip: US sheet not found"); goto out; }
        tim_layout(usint + ut, usint_len - ut, &uco, &uio, &utot);
        memcpy(tooltip_sheet, usint + ut + uio + 12, sizeof tooltip_sheet);
        memcpy(tooltip_clut, usint + ut + uco + 12, 32);
        tooltip_ready = 1;
        LOGI("patch: tooltip bar: stage select words from the US disc");
    }
out:
    free(usov);
    free(usint);
}

/* The US sheet lives at (960,256) with its palette at (960,368): free in the menus (checked against VRAM dumps
   of the candy-bar menu and Stage Select), inside texture page 15/256. It's (re)written whenever the game loads
   the JP sheet, so it's there whenever the tooltip bar can be drawn. */
#define TT_X 960
#define TT_Y 256
#define TT_CLUT_Y 368
void tooltip_note_upload(int x, int y, int w, int h) {
    if (!tooltip_ready || x != 832 || y != 350 || w != 64 || h != 98) return;
    for (int r = 0; r < 112; r++) memcpy(&g_vram[(TT_Y + r) * 1024 + TT_X], tooltip_sheet[r], 128);
    memcpy(&g_vram[TT_CLUT_Y * 1024 + TT_X], tooltip_clut, 32);
    if (g_gpu_backend.vram_write) { g_gpu_backend.vram_write(TT_X, TT_Y, 64, 112); g_gpu_backend.vram_write(TT_X, TT_CLUT_Y, 16, 1); }
}

/* Word correspondence between the sheets for the other menus' tooltip builders, whose JP tables stay as they
   are: both sheets list the same words in the same order per row. JP rows are 14 px (u, width packed edge to
   edge, taken from the JP overlay's tables and the sheet); US rows are 16 px (spans measured on the US sheet).
   Coordinates only; the words themselves come from the discs. JP digit 8 has no US counterpart (stays JP). */
typedef struct { uint8_t row, ju, jw, uu, uw; } TtWord;
static const TtWord tt_words[] = {
    {0, 0, 11, 0, 12}, {0, 11, 11, 12, 12}, {0, 22, 11, 24, 12}, {0, 33, 11, 36, 12}, {0, 44, 11, 48, 12},
    {0, 55, 11, 60, 12}, {0, 66, 11, 72, 12}, {0, 88, 49, 84, 34}, {0, 137, 47, 128, 24}, {0, 184, 26, 160, 30},
    {0, 210, 28, 208, 38},
    {1, 0, 35, 0, 40}, {1, 35, 47, 48, 40}, {1, 82, 51, 96, 48}, {1, 133, 105, 160, 52},
    {2, 0, 28, 0, 15}, {2, 28, 29, 16, 24}, {2, 57, 38, 48, 47}, {2, 95, 40, 96, 35}, {2, 135, 40, 144, 42},
    {2, 175, 52, 208, 30},
    {3, 0, 52, 0, 39}, {3, 52, 64, 48, 44}, {3, 116, 80, 112, 58}, {3, 196, 40, 192, 31},
    {4, 0, 42, 0, 31}, {4, 42, 41, 48, 29}, {4, 83, 43, 80, 54}, {4, 126, 44, 144, 40}, {4, 170, 57, 192, 53},
    {5, 0, 38, 0, 24}, {5, 38, 52, 32, 38}, {5, 90, 68, 80, 49}, {5, 158, 68, 144, 44},
    {6, 0, 39, 0, 26}, {6, 39, 52, 32, 44}, {6, 91, 41, 78, 29},
};
static const TtWord *tt_find(int row, int u) {
    for (size_t i = 0; i < sizeof tt_words / sizeof tt_words[0]; i++)
        if (tt_words[i].row == row && abs(tt_words[i].ju - u) <= 1) return &tt_words[i];
    return NULL;
}

/* Pre-hook on the menu overlay's sprite submit (O00_801cbc4c, a1 = sprite: +8 x, +10 y, +12 u, +13 v, +14 clut,
   +16 w, +18 h, +24 draw-mode word). Sprites from the JP word sheet are pointed at the US sheet:
   - from the Stage Select builder (O00_801cd2a0), whose tables now hold the US words: only v/clut/page/height;
   - from the other builders: each JP word (or run of adjacent JP words) maps to the matching US word(s), and
     words after the first on a line move by the accumulated width difference plus a space.
   Anything unrecognized is left on the JP sheet, which stays in VRAM untouched. */
static void tt_retarget(uint32_t sp, int v_us, int h) {
    mem_w8(sp + 13, (uint8_t)v_us);
    mem_w16(sp + 14, (uint16_t)((TT_CLUT_Y << 6) | (TT_X >> 4)));
    mem_w16(sp + 18, (uint16_t)h);
    uint32_t mode = mem_r32(sp + 24);
    mem_w32(sp + 24, (mode & ~0x1Fu) | (TT_X / 64) | 0x10u);  /* texture page (960,256) */
}
void hook_tooltip_sprite(CPU *c, uint32_t fn) {
    (void)fn;
    {   /* UJL_AL_LOG (debug): sprites from the memory card message page (tpage 704) */
        static int st = -1, n; if (st < 0) st = getenv("UJL_AL_LOG") != NULL;
        uint32_t s5 = c->r[5]; uint16_t md = (uint16_t)mem_r32(s5 + 24);
        if (st && (md & 0xF) == 11 && n < 60) { n++;
            rt_log("AL sprite: ra %08x xy(%d,%d) uv(%d,%d) wh(%d,%d) clut %04x mode %04x", c->r[31], (int16_t)mem_r16(s5 + 8), (int16_t)mem_r16(s5 + 10),
                   mem_r8(s5 + 12), mem_r8(s5 + 13), (int16_t)mem_r16(s5 + 16), (int16_t)mem_r16(s5 + 18), mem_r16(s5 + 14), md); }
    }
    if (!tooltip_ready) return;
    const char *ov = rt_current_overlay_name();
    if (!ov || strcmp(ov, "COMOD0.MEN")) return;
    uint32_t ra = c->r[31], sp = c->r[5];
    int u = mem_r8(sp + 12), v = mem_r8(sp + 13), w = (int16_t)mem_r16(sp + 16), h = (int16_t)mem_r16(sp + 18);
    uint16_t mode = (uint16_t)mem_r32(sp + 24);
    if (ra >= 0x801cd2a0u && ra < 0x801cdc80u) {             /* Stage Select builder: tables already US */
        if (v >= 94) tt_retarget(sp, v - 94, 16);
        return;
    }
    static uint32_t last_ra; static int last_y = -9999, last_jend = -9999, delta;
    int x = (int16_t)mem_r16(sp + 8), y = (int16_t)mem_r16(sp + 10);
    if ((mode & 0x1F) != 29 || h != 14 || v < 94 || (v - 94) % 14) return;
    int row = (v - 94) / 14;
    const TtWord *a = tt_find(row, u);
    if (!a) {
        static uint32_t seen[32]; static int nseen;
        uint32_t key = (uint32_t)u | (uint32_t)v << 8 | (uint32_t)w << 16;
        int k = 0; while (k < nseen && seen[k] != key) k++;
        if (k == nseen && nseen < 32) { seen[nseen++] = key; LOGI("tooltip: unmapped JP word u=%d v=%d w=%d (ra %08x)", u, v, w, ra); }
        return;
    }
    /* the sprite may cover several adjacent words: walk to the last one */
    const TtWord *b = a;
    int uu_end = a->uu + a->uw, ju_end = a->ju + a->jw;
    while (ju_end < u + w - 2) {
        const TtWord *n = tt_find(row, ju_end);
        if (!n) return;
        b = n; ju_end = n->ju + n->jw; uu_end = n->uu + n->uw;
    }
    (void)b;
    if (ra != last_ra || y != last_y || x < last_jend - 1) delta = 0;   /* new line */
    else if (abs(x - last_jend) <= 1) delta += 4;                        /* adjacent JP word: add a space */
    last_ra = ra; last_y = y; last_jend = x + w;
    mem_w8(sp + 12, a->uu);
    mem_w16(sp + 16, (uint16_t)(uu_end - a->uu));
    mem_w16(sp + 8, (uint16_t)(x + delta));
    delta += (uu_end - a->uu) - w;
    tt_retarget(sp, row * 16, 16);
}
int tooltip_english_active(void) { return tooltip_ready; }

int title_logo_available(void);

/* ---- English credits song ----
   The staff-roll song is XA channel 7 of /S1/ST1.XA. The USA disc's file has the same size and sector layout
   (every channel-7 sector sits at the same offset, same coding), but channel 7 holds the vocal version.
   Those sectors are read from the USA disc instead; the other channels stay the JP disc's. */
int g_patch_english_credits_song = 1;
bool disc_find_file(const char *path, uint32_t *lba, uint32_t *size);
bool aux_disc_find_file(const char *path, uint32_t *lba, uint32_t *size);
bool aux_disc_read_sector(uint32_t lba, uint8_t out[2352]);
static uint32_t song_jp_lba, song_us_lba, song_nsec;
static uint32_t song_last_ch7;   /* file-relative index of the last channel-7 sector read, +1 (0 = none) */
static uint32_t song_ch7_end;    /* file-relative index of the final channel-7 sector, +1 */
#define CREDITS_XA_CHANNEL 7

static void english_credits_song(void) {
    song_nsec = 0;
    if (!aux_disc_ready() || getenv("UJL_JP_CREDITS_SONG")) return;
    uint32_t jl, js, ul, us;
    if (!disc_find_file("S1/ST1.XA", &jl, &js) || !aux_disc_find_file("S1/ST1.XA", &ul, &us) || js != us) {
        LOGI("patch: credits song: ST1.XA not found on both discs or sizes differ, keeping the JP song");
        return;
    }
    /* spot-check the layout: the first channel-7 sector must be channel 7 on both discs */
    uint8_t a[2352], b[2352];
    for (uint32_t i = 0; i < 16; i++) {
        if (!disc_read_sector(jl + i, a) || !aux_disc_read_sector(ul + i, b)) return;
        if (a[17] == CREDITS_XA_CHANNEL || b[17] == CREDITS_XA_CHANNEL) {
            if (a[17] != b[17] || a[16] != b[16] || a[19] != b[19]) {
                LOGI("patch: credits song: XA layouts differ, keeping the JP song");
                return;
            }
        }
    }
    song_jp_lba = jl; song_us_lba = ul; song_nsec = js / 2048;
    song_ch7_end = 0; song_last_ch7 = 0;
    for (uint32_t i = song_nsec; i-- > 0 && i + 16 >= song_nsec; )   /* the final channel-7 sector */
        if (disc_read_sector(jl + i, a) && a[17] == CREDITS_XA_CHANNEL && (a[18] & 0x04)) { song_ch7_end = i + 1; break; }
    if (!song_ch7_end) song_ch7_end = song_nsec;
    LOGI("patch: credits song: XA channel %d of S1/ST1.XA from the USA disc", CREDITS_XA_CHANNEL);
}

/* ---- English memory card messages (menus) ----
   The JP menus show each memory card message as a pre-rendered Japanese image (AL_*.TIM in S0/CMP0.INT), drawn by
   O00_801cc188(msg) as one sprite from the menu's sprite table (COMOD0.MEN: 12-byte entries at overlay +0x18194:
   u16 tpage, u8 u, u8 v, u16 w, u16 h, u16 clut x, u16 clut y; message k -> sprite via a 13-byte table at +0x5c8),
   centered, with the dialog box sized from the entry's w/h. The US version draws the same 12 messages as strings
   with the English font (US COMOD0.MEN: pointer table at +0x814, message k = string k-1). Here: the entries' w/h are
   set to the English text's size (so the box fits it), and at the GPU each message sprite is replaced by glyph
   sprites from the English font (LETTER_E, resident at VRAM 960,385, CLUT 1008,475; glyph table in the exe at
   0x80063c98, 10 bytes per character code: u16 u, u16 v, u8 w, u8 h, s16 spacing). Text comes from the USA disc at
   runtime. Yes/No (two 44x14 halves of AL_YESNO) become YES / NO. */
#define MC_SPRITES 0x18194u
#define MC_MSGMAP  0x5c8u
#define MC_LINE_H  15
#define FONT_CLUT  ((475 << 6) | (1008 >> 4))
#define FONT_V0    (385 - 256)
typedef struct { uint16_t u, v; uint8_t w, h; int16_t adj; } McGlyph;
typedef struct { uint16_t tp, clut; uint8_t u, v; char text[160]; } McMsg;
static McGlyph mc_glyph[128];
static McMsg mc_msg[12];
static int mc_nmsg, mc_ready, mc_yesno_tp = -1, mc_yesno_clut;
int g_patch_english_mc = 1;

static int mc_line_width(const char *s, int n) {
    int w = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        int ch = (unsigned char)s[i] & 127;
        const McGlyph *g = &mc_glyph[ch];
        if (i) w += mc_glyph[prev].adj;
        w += (g->u == 0xFFFF || !g->w) ? 8 : g->w;
        prev = ch;
    }
    return w;
}
static int mc_trim(const char *s, int n) { while (n > 0 && s[n - 1] == ' ') n--; return n; } /* US lines end in a space */
static void mc_text_size(const char *s, int *w, int *h) {
    int lines = 0, mw = 0;
    while (*s) {
        const char *e = strchr(s, '\n');
        int n = e ? (int)(e - s) : (int)strlen(s);
        int lw = mc_line_width(s, mc_trim(s, n));
        if (lw > mw) mw = lw;
        lines++;
        s += e ? n + 1 : n;
    }
    *w = mw; *h = lines * MC_LINE_H;
}

static void english_mc_messages(void) {
    mc_ready = 0; mc_nmsg = 0; mc_yesno_tp = -1;
    if (!aux_disc_ready() || getenv("UJL_JP_MEMCARD_TEXT")) return;
    uint8_t *exe = NULL, *usov = NULL;
    uint32_t exe_len = 0, usov_len = 0;
    if (!disc_read_file("SCPS_180.11", &exe, &exe_len)) return;
    if (!aux_disc_read_file("S0/COMOD0.MEN", &usov, &usov_len)) { free(exe); return; }
    PatchedFile *ov = load_for_patch("S0/COMOD0.MEN");
    uint32_t gofs = 0x80063c98u - 0x80010000u + 0x800u;
    if (!ov || exe_len < gofs + 1280 || usov_len < 0x844 || ov->nsec * 2048 < MC_SPRITES + 220 * 12) goto out;
    for (int ch = 0; ch < 128; ch++) {
        const uint8_t *e = exe + gofs + ch * 10;
        McGlyph *g = &mc_glyph[ch];
        memcpy(&g->u, e, 2); memcpy(&g->v, e + 2, 2); g->w = e[4]; g->h = e[5]; memcpy(&g->adj, e + 6, 2);
        if (g->u != 0xFFFF && (g->u > 255 || g->v > 120 || g->h > 16)) { LOGI("patch: memcard text: font table not as expected"); goto out; }
    }
    for (int k = 1; k <= 12; k++) {
        uint32_t p;
        memcpy(&p, usov + 0x814 + 4 * (k - 1), 4);
        uint32_t so = p - US_OV_BASE;
        if (so < 0x600 || so >= 0x840) { LOGI("patch: memcard text: US string table not as expected"); goto out; }
        int sprite = ov->data[MC_MSGMAP + k];
        uint8_t *ent = ov->data + MC_SPRITES + (uint32_t)sprite * 12;
        McMsg *m = &mc_msg[mc_nmsg];
        uint16_t cx, cy;
        memcpy(&m->tp, ent, 2); m->u = ent[2]; m->v = ent[3]; memcpy(&cx, ent + 8, 2); memcpy(&cy, ent + 10, 2);
        m->clut = (uint16_t)((cy << 6) | (cx >> 4));
        size_t n = 0;
        while (so + n < usov_len && usov[so + n] && n < sizeof m->text - 1) { m->text[n] = (char)usov[so + n]; n++; }
        m->text[n] = 0;
        int w, h;
        mc_text_size(m->text, &w, &h);
        uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
        memcpy(ent + 4, &w16, 2); memcpy(ent + 6, &h16, 2);  /* the box is sized from these */
        mc_nmsg++;
    }
    {   /* Yes/No: sprite 210 (two 44x14 halves) */
        const uint8_t *ent = ov->data + MC_SPRITES + 210 * 12;
        uint16_t tp, cx, cy;
        memcpy(&tp, ent, 2); memcpy(&cx, ent + 8, 2); memcpy(&cy, ent + 10, 2);
        if (ent[2] == 168 && ent[3] == 196) { mc_yesno_tp = tp; mc_yesno_clut = (cy << 6) | (cx >> 4); }
    }
    mc_ready = 1;
    LOGI("patch: memory card messages: %d in English (USA disc text, English font)", mc_nmsg);
out:
    free(exe);
    free(usov);
}

static void mc_draw_line(const GpuPrim *p, const char *s, int n, int x, int y, void (*emit)(const GpuPrim *)) {
    int prev = 0;
    for (int i = 0; i < n; i++) {
        int ch = (unsigned char)s[i] & 127;
        const McGlyph *g = &mc_glyph[ch];
        if (i) x += mc_glyph[prev].adj;
        prev = ch;
        if (g->u == 0xFFFF || !g->w) { x += 8; continue; }
        GpuPrim q = *p;
        q.w = g->w; q.h = g->h;
        q.v[0].x = (int16_t)x; q.v[0].y = (int16_t)y;
        q.v[0].u = (uint8_t)g->u; q.v[0].v = (uint8_t)(FONT_V0 + g->v);
        q.v[0].fx = q.v[0].fy = 0;
        q.texpage = (uint16_t)((p->texpage & ~0x19Fu) | 15 | 0x10);   /* page (960,256), 4bpp */
        q.clut = FONT_CLUT;
        emit(&q);
        x += g->w;
    }
}
static void mc_draw_text(const GpuPrim *p, const char *s, void (*emit)(const GpuPrim *)) {
    int tw, th;
    mc_text_size(s, &tw, &th);
    int y = p->v[0].y + (p->h - th) / 2;
    while (*s) {
        const char *e = strchr(s, '\n');
        int n = e ? (int)(e - s) : (int)strlen(s);
        int m = mc_trim(s, n);
        mc_draw_line(p, s, m, p->v[0].x + (p->w - mc_line_width(s, m)) / 2, y, emit);
        y += MC_LINE_H;
        s += e ? n + 1 : n;
    }
}

/* gpu.c: replace memory card message sprites with English glyphs (returns 1 if handled) */
int mc_text_filter(const GpuPrim *p, void (*emit)(const GpuPrim *)) {
    if (!mc_ready || p->type != PRIM_RECT || !p->textured) return 0;
    const char *ov = rt_current_overlay_name();
    if (!ov || strcmp(ov, "COMOD0.MEN")) return 0;
    for (int i = 0; i < mc_nmsg; i++) {
        const McMsg *m = &mc_msg[i];
        if ((p->texpage & 0x1F) == (m->tp & 0x1F) && p->clut == m->clut && p->v[0].u == m->u && p->v[0].v == m->v) {
            mc_draw_text(p, m->text, emit);
            return 1;
        }
    }
    if (mc_yesno_tp >= 0 && (p->texpage & 0x1F) == (mc_yesno_tp & 0x1F) && p->clut == mc_yesno_clut && p->v[0].u == 168 &&
        (p->v[0].v == 196 || p->v[0].v == 210)) {
        mc_draw_text(p, p->v[0].v == 196 ? "YES" : "NO", emit);
        return 1;
    }
    return 0;
}

void patch_init(void) {
    nfiles = 0;
    /* Only with a USA disc: without one there's no English logo to draw, so the JP logo stays intact. */
    if (g_patch_english_logo && title_logo_available()) {
        /* The English release shows the title emblem (which already reads "Um Jammer Lammy")
           without the Japanese lettering drawn over it. */
        /* LGT1/LGT2 (136x43 4bpp = 34x43 VRAM halfwords at 512,43 and 512,86): the indexed copies
           plus the extra copies in the archive's unindexed tail, which the title screen uses */
        blank_tims_at_vram("S0\\CMP0.INT", 512, 43, 34, 43);
        blank_tims_at_vram("S0\\CMP0.INT", 512, 86, 34, 43);
    }
    if (g_patch_english_menus) swap_tims_from_aux("S0\\CMP0.INT");
    tooltip_ready = 0;
    if (g_patch_english_tooltips) english_tooltips();
    if (g_patch_english_mc) english_mc_messages();
    if (g_patch_english_captions) english_captions();
    if (g_patch_english_credits_song) english_credits_song();
    { void censor_init(void); censor_init(); }   /* censor.c: USA versions of Stages 1, 5 and 6 (setting) */
}

/* Staff roll length: the JP roll (COMOD8.SNG) ends ~82 s into the song, when its timeline raises event bit 0x100
   in the per-frame event buffer (0x80078330). The timeline thread (O41_801c5168) then sets the overlay flag
   0x801c6368, and the main loop (O41_801c5370) fades out over 90 frames and returns to the menu. That suits the JP
   instrumental, but the US vocal song swapped in here runs the whole 145.8 s of channel 7 and was cut mid-verse.
   While the US song plays, this hook (on O41_801c624c, which the timeline thread calls every frame right after
   the flag check, with no yield in between) holds the flag clear, so the roll stays on its final copyright card.
   The cue fires only once, so the hook raises the flag itself when the CD gets within CREDITS_FADE_SECTORS of the
   song's last sector: the 90-frame fade then covers the final chord and the scene stops the CD at the end of the
   file (left alone, it streams on into the next file). A skip button still ends it early (the main loop reads the
   pad itself). UJL_JP_CREDITS_SONG=1 keeps the JP song and the JP length. */
#define CREDITS_FADE_SECTORS 120   /* ~1.6 s at 1x: the 1.5 s fade plus a frame or two before it starts */
void hook_credits_hold(CPU *c, uint32_t fn) {
    (void)c; (void)fn;
    static int held;
    if (!song_nsec) return;
    const char *ov = rt_current_overlay_name();
    if (!ov || strcmp(ov, "COMOD8.SNG")) return;
    if (song_last_ch7 < 64) held = 0;               /* a new run of the song */
    int done = song_last_ch7 + CREDITS_FADE_SECTORS >= song_ch7_end;
    if (mem_r32(0x801c6368u)) {
        if (done) return;
        if (!held) LOGI("patch: credits song: holding the staff roll's end until the US song finishes");
        held = 1;
        mem_w32(0x801c6368u, 0);
    } else if (held && done) {
        LOGI("patch: credits song: song finished, ending the staff roll");
        held = 0;
        mem_w32(0x801c6368u, 1);
    }
}

/* Called for every sector read: substitute patched user data. */
void patch_sector(uint32_t lba, uint8_t sec[2352]) {
    if (song_nsec && lba - song_jp_lba < song_nsec && sec[17] == CREDITS_XA_CHANNEL && (sec[18] & 0x04)) {
        song_last_ch7 = lba - song_jp_lba + 1;
        uint8_t us[2352];
        uint64_t t0 = SDL_GetPerformanceCounter();
        int ok = aux_disc_read_sector(song_us_lba + (lba - song_jp_lba), us);
        {   /* UJL_GTE_STATS: cost of the USA-disc read on the game thread */
            static int st = -1, n; static double mx, sum; static uint64_t last;
            if (st < 0) st = getenv("UJL_GTE_STATS") != NULL;
            if (st) {
                double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
                n++; sum += ms; if (ms > mx) mx = ms;
                if (SDL_GetTicks() - last > 2000) { last = SDL_GetTicks(); rt_log("credits song: %d aux reads, avg %.3f ms, max %.3f ms", n, sum / n, mx); n = 0; sum = mx = 0; }
            }
        }
        if (ok && us[17] == sec[17] && us[16] == sec[16])
            /* Subheader + audio only. The header (bytes 12-15) is the sector's address on the disc it came from, and
               the game reads it back through GetlocL as its song clock: the US file sits 34 sectors earlier, so a
               whole-sector copy made the clock jump back ~0.45 s whenever a channel-7 sector was the last one read,
               firing Stage 1's choreography cues early (the crowd's dance snapping back about once a second). */
            memcpy(sec + 16, us + 16, 2352 - 16);
        return;
    }
    { int censor_sector(uint32_t lba, uint8_t sec[2352]); if (censor_sector(lba, sec)) return; }
    for (int i = 0; i < nfiles; i++) {
        PatchedFile *f = &files[i];
        if (lba >= f->lba && lba < f->lba + f->nsec) {
            memcpy(sec + 24, f->data + (size_t)(lba - f->lba) * 2048, 2048);
            return;
        }
    }
}
