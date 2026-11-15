/* Headless boot probe: runs the game with no window and prints per-second stats. */
#include "runtime/psx.h"
#include "runtime/gpu.h"
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "runtime/gpu_rec.h"

bool rt_boot(const char *disc_path);
void rt_run(void);

static int frames, max_seconds = 10, shots, burst_from = -1, burst_n = 0;
static uint32_t prims_sec;

/* ---- audio: WAV dump + per-second peak level ---- */
static FILE *wav;
static uint32_t wav_bytes;
static int32_t peak;
void host_audio_push(const int16_t *lr, int n) {
    for (int i = 0; i < n * 2; i++) { int a = lr[i] < 0 ? -lr[i] : lr[i]; if (a > peak) peak = a; }
    if (wav) { fwrite(lr, 4, (size_t)n, wav); wav_bytes += (uint32_t)n * 4; }
}
static void wav_close(void) {
    if (!wav) return;
    uint32_t hdr[11] = {0x46464952, 36 + wav_bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 44100, 44100 * 4, 0x00100004, 0x61746164, wav_bytes};
    fseek(wav, 0, SEEK_SET);
    fwrite(hdr, 4, 11, wav);
    fclose(wav);
    wav = NULL;
}

/* ---- scripted input: -press T:BTN ---- */
typedef struct { int frame; uint16_t bit; } Press;
static Press presses[4096];
static int npress;
static uint16_t btn_bit(const char *n) {
    static const char *names[] = {"select","l3","r3","start","up","right","down","left","l2","r2","l1","r1","triangle","circle","cross","square"};
    for (int i = 0; i < 16; i++) if (!strcmp(n, names[i])) return (uint16_t)(1u << i);
    return 0;
}
void gpu_soft_install(void);

static void dump_frame(int t) {
    const GpuDisplay *d = &g_gpu_display;
    int w = d->hres, h = d->vres;
    SDL_Surface *sf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XBGR8888);
    for (int y = 0; y < h; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)sf->pixels + y * sf->pitch);
        int vy = (d->disp_y + y) & 511;
        for (int x = 0; x < w; x++) {
            uint32_t r, g, b;
            if (d->depth24) {
                const uint8_t *row = (const uint8_t *)&g_vram[vy * 1024];
                int bx = d->disp_x * 2 + x * 3;
                r = row[bx & 2047]; g = row[(bx + 1) & 2047]; b = row[(bx + 2) & 2047];
            } else {
                uint16_t c = g_vram[vy * 1024 + ((d->disp_x + x) & 1023)];
                r = (c & 31) << 3; g = ((c >> 5) & 31) << 3; b = ((c >> 10) & 31) << 3;
            }
            dst[x] = r | g << 8 | b << 16;
        }
    }
    char path[128];
    SDL_snprintf(path, sizeof path, t >= 10000 ? "build/shots/b%05d.bmp" : "build/shots/t%03d.bmp", t);
    SDL_SaveBMP(sf, path);
    SDL_DestroySurface(sf);
}
extern int g_in_exception, g_cur_thread;
void bios_dump_threads(void);

static void reclog(void) {
    /* PROBE_RECLOG_AT=sec: log the recorded command stream of that frame */
    static int at = -2;
    if (at == -2) { const char *e = getenv("PROBE_RECLOG_AT"); at = e ? (int)(atof(e) * 60) : -1; }
    if (at < 0) return;
    const uint8_t *b;
    size_t n = gpu_rec_take(&b);
    static int span = -1; if (span < 0) { const char *e = getenv("PROBE_RECLOG_N"); span = e ? atoi(e) : 4; }
    if (frames < at + 1 || frames > at + span) return;
    FILE *f = fopen("build/reclog.txt", frames == at + 1 ? "w" : "a");
    for (size_t off = 0; f && off + sizeof(RecHdr) <= n;) {
        const RecHdr *h = (const RecHdr *)(b + off);
        const void *pl = h + 1;
        if (h->type == REC_PRIM) {
            const GpuPrim *p = (const GpuPrim *)pl;
            fprintf(f, "PRIM g%d t%d tex%d semi%d raw%d mode%d tp%u clut%u xy(%d,%d) wh(%d,%d) rgb(%d,%d,%d) clip(%d,%d,%d,%d) v1(%d,%d) v2(%d,%d) uv(%d,%d) f(%d,%d,%d,%d,%d,%d)\n",
                    p->gte3d, p->type, p->textured, p->semi, p->raw, (p->texpage >> 5) & 3, p->texpage, p->clut, p->v[0].x, p->v[0].y,
                    p->w, p->h, p->v[0].r, p->v[0].g, p->v[0].b, p->clip_x0, p->clip_y0, p->clip_x1, p->clip_y1,
                    p->v[1].x, p->v[1].y, p->v[2].x, p->v[2].y, p->v[0].u, p->v[0].v,
                    p->v[0].fx, p->v[0].fy, p->v[1].fx, p->v[1].fy, p->v[2].fx, p->v[2].fy);
        } else if (h->type == REC_UPLOAD) {
            const RecRect *r = (const RecRect *)pl;
            fprintf(f, "UPLOAD %d,%d %dx%d\n", r->x, r->y, r->w, r->h);
        } else if (h->type == REC_FILL) {
            const RecFill *r = (const RecFill *)pl;
            fprintf(f, "FILL %d,%d %dx%d %06x\n", r->x, r->y, r->w, r->h, r->rgb);
        } else if (h->type == REC_COPY) {
            const RecCopy *r = (const RecCopy *)pl;
            fprintf(f, "COPY %d,%d -> %d,%d %dx%d\n", r->sx, r->sy, r->dx, r->dy, r->w, r->h);
        } else if (h->type == REC_FRAME) {
            const GpuDisplay *d = (const GpuDisplay *)pl;
            fprintf(f, "FRAME disp %d,%d %dx%d 24bpp=%d\n", d->disp_x, d->disp_y, d->hres, d->vres, d->depth24);
        }
        off += sizeof(RecHdr) + h->size;
    }
    if (f) fclose(f);
}

void host_vblank(void) {
    gpu_rec_vblank();
    reclog();
    frames++;
    {   /* PROBE_VRAMDUMP_AT=sec: raw 1024x512x16 VRAM to build/vram.bin */
        const char *e = getenv("PROBE_VRAMDUMP_AT");
        if (e && frames == atoi(e) * 60) { FILE *f = fopen("build/vram.bin", "wb"); if (f) { fwrite(g_vram, 2, 1024 * 512, f); fclose(f); } }
    }
    {   /* PROBE_SPUW_AT=sec,len: log SPU register writes in that window */
        extern int g_spu_wlog; const char *e = getenv("PROBE_SPUW_AT");
        if (e) { float a0 = 0, l = 1; sscanf(e, "%f,%f", &a0, &l); g_spu_wlog = frames >= (int)(a0 * 60) && frames < (int)((a0 + l) * 60); }
    }
    {   /* PROBE_RAMDUMP_AT=s1,s2,...: dump main RAM to build/ram_<s>.bin */
        const char *rd = getenv("PROBE_RAMDUMP_AT");
        for (const char *q = rd; q && *q;) {
            int sec = atoi(q);
            if (frames == sec * 60) {
                char fn[64];
                SDL_snprintf(fn, sizeof fn, "build/ram_%d.bin", sec);
                FILE *f = fopen(fn, "wb");
                if (f) { fwrite(g_ram, 1, PSX_RAM_SIZE, f); fclose(f); }
                { extern int g_xa_filter_chan; rt_log("ramdump %d: xa chan %d", sec, g_xa_filter_chan); }
                extern uint8_t g_spu_ram[];
                SDL_snprintf(fn, sizeof fn, "build/spuram_%d.bin", sec);
                f = fopen(fn, "wb");
                if (f) { fwrite(g_spu_ram, 1, 512 * 1024, f); fclose(f); }
            }
            q = strchr(q, ',');
            if (q) q++;
        }
    }
    {   /* PROBE_CAPTURE_AT=sec: capture all textured prims of one frame (fully) to build/shots/capture.txt */
        extern FILE *g_gpu_capture;
        const char *ca = getenv("PROBE_CAPTURE_AT");
        int at = ca ? (int)(atof(ca) * 60) : -1;
        if (frames == at) { const char *cf = getenv("PROBE_CAPTURE_FILE"); g_gpu_capture = fopen(cf ? cf : "build/shots/capture.txt", "w"); }
        else if (g_gpu_capture && frames >= at + 2) { fclose(g_gpu_capture); g_gpu_capture = NULL; }
    }
    {   /* PROBE_POKE=addr,val,sec: write a guest word once at that second */
        const char *pk = getenv("PROBE_POKE");
        unsigned a, v; float sec;
        if (pk && sscanf(pk, "%x,%x,%f", &a, &v, &sec) == 3 && frames == (int)(sec * 60)) { mem_w32(a, v); rt_log("poke: [%08x] = %08x", a, v); }
    }
    {   /* PROBE_COPY=dst,src,len,sec: copy guest memory once at that second (e.g. a stage-table entry over
           another: scene 8, the staff roll, is 0x80059a18..+0x128; Stage 1 is 0x80058420) */
        const char *cp = getenv("PROBE_COPY");
        unsigned dst, src, len; float sec;
        if (cp && sscanf(cp, "%x,%x,%x,%f", &dst, &src, &len, &sec) == 4 && frames == (int)(sec * 60)) {
            for (unsigned i = 0; i + 4 <= len; i += 4) mem_w32(dst + i, mem_r32(src + i));
            rt_log("copy: %08x <- %08x (%x bytes)", dst, src, len);
        }
    }
    {   /* PROBE_MEMLOG=addr,len,sec,nframes: append guest memory [addr,addr+len) every frame to build/memlog.bin
           (u32 frame number + len bytes per record) */
        const char *ml = getenv("PROBE_MEMLOG");
        unsigned a, len, n; float sec;
        if (ml && sscanf(ml, "%x,%x,%f,%u", &a, &len, &sec, &n) == 4 && frames >= (int)(sec * 60) && frames < (int)(sec * 60) + (int)n) {
            static FILE *mf; if (!mf) mf = fopen("build/memlog.bin", "wb");
            if (mf) { uint32_t fr = (uint32_t)frames; fwrite(&fr, 4, 1, mf); for (unsigned i = 0; i < len; i += 4) { uint32_t w = mem_r32(a + i); fwrite(&w, 4, 1, mf); } fflush(mf); }
        }
    }
    {   /* PROBE_STACK_AT=s1,s2,...: dump the guest stack and parked threads at those seconds */
        const char *sa = getenv("PROBE_STACK_AT");
        if (sa) { const char *p = sa; while (*p) { float t = (float)atof(p); if (frames == (int)(t * 60)) rt_dump_stack("stack-at"); while (*p && *p != ',') p++; if (*p) p++; } }
    }
    {   /* PROBE_FREEZE=addr,val,from[,to] (seconds): write a guest word every frame in that window */
        const char *fz = getenv("PROBE_FREEZE");
        unsigned a, v; float s0, s1 = 1e9f;
        if (fz && sscanf(fz, "%x,%x,%f,%f", &a, &v, &s0, &s1) >= 3 && frames >= (int)(s0 * 60) && frames < (int)(s1 * 60)) mem_w32(a, v);
    }
    {   /* PROBE_WATCH=lo,hi (phys hex): write-watch armed from boot */
        const char *w = getenv("PROBE_WATCH");
        unsigned lo, hi;
        const char *wa = getenv("PROBE_WATCH_AT");
        if (w && frames == (wa ? (int)(atof(wa) * 60) : 1) && sscanf(w, "%x,%x", &lo, &hi) == 2) { g_watch_lo = lo; g_watch_hi = hi; rt_log("watch: armed %06x-%06x", lo, hi); }
    }
    {   /* PROBE_RWATCH=lo,hi (phys hex) armed from PROBE_RWATCH_AT seconds */
        const char *rw = getenv("PROBE_RWATCH"), *ra = getenv("PROBE_RWATCH_AT");
        if (rw && ra && frames == atoi(ra) * 60) {
            unsigned lo, hi;
            if (sscanf(rw, "%x,%x", &lo, &hi) == 2) { g_rwatch_lo = lo; g_rwatch_hi = hi; rt_log("rwatch: armed %06x-%06x", lo, hi); }
        }
    }
    {   /* PROBE_WATCH_CLUT=hex: find the RAM packet of the first prim using that CLUT, then watch writes to it */
        extern uint32_t g_glyph_clut_watch, g_glyph_packet;
        static int armed;
        const char *wc = getenv("PROBE_WATCH_CLUT"), *wt = getenv("PROBE_WATCH_AT");
        if (wc && wt && frames == atoi(wt) * 60) g_glyph_clut_watch = (uint32_t)strtoul(wc, NULL, 16);
        if (g_glyph_packet && !armed) {
            armed = 1;
            g_watch_lo = g_glyph_packet; g_watch_hi = g_glyph_packet + 20;
            rt_log("watch: armed on glyph packet %06x", g_glyph_packet);
        }
    }
    { extern int g_gpu_dump_prims, g_gpu_log_uploads; const char *e = getenv("PROBE_DUMP_AT"); int at = e ? atoi(e) * 60 : -1; g_gpu_dump_prims = (frames >= at && frames < at + 60) ? (getenv("PROBE_DUMP_ALL") ? 2 : 1) : 0; g_gpu_log_uploads = (at > 0 && frames > at - 600 && frames < at); }
    if (burst_from >= 0 && frames >= burst_from && frames < burst_from + burst_n) dump_frame(10000 + frames);
    {   /* hold each scripted press for 6 frames */
        uint16_t held = 0;
        for (int i = 0; i < npress; i++)
            if (frames >= presses[i].frame && frames < presses[i].frame + 6) held |= presses[i].bit;
        g_pad_buttons[0] = (uint16_t)~held;
    }
    if (frames % 60 == 0) {
        rt_log("[probe] t=%ds prims/s=%u uploads/s=%u (%u px) disp=%dx%d@%d,%d en=%d thread=%d",
               frames / 60, prims_sec, g_gpu_uploads, g_gpu_upload_px, g_gpu_display.hres, g_gpu_display.vres,
               g_gpu_display.disp_x, g_gpu_display.disp_y, g_gpu_display.enabled, g_cur_thread);
        g_gpu_uploads = g_gpu_upload_px = 0;
        prims_sec = 0;
        { extern int g_sio_pad_polls; extern int g_xa_underruns, g_xa_sectors, g_cd_sectors; rt_log("[probe]   audio peak %d  pad polls %d  xa underruns %d  xa sectors %d  cd sectors %d", peak, g_sio_pad_polls, g_xa_underruns, g_xa_sectors, g_cd_sectors); g_sio_pad_polls = 0; g_xa_underruns = 0; g_xa_sectors = g_cd_sectors = 0; }
        peak = 0;
        if (shots) dump_frame(frames / 60);
    }
    prims_sec += g_gpu_prims_frame;
    g_gpu_prims_frame = 0;
    if (frames >= max_seconds * 60) {
        rt_dump_stack("probe end");
        bios_dump_threads();
        {   /* libpress-style stream ring state (JP exe addresses) */
            uint32_t base = mem_r32(0x801c03c8);
            char l[512]; int n = 0;
            for (uint32_t a = 0x801c0390; a < 0x801c03d0; a += 4) n += snprintf(l + n, sizeof l - (size_t)n, "%x:%x ", a & 0xFFF, mem_r32(a));
            rt_log("stream globals: %s", l);
            n = 0;
            for (int i = 0; i < 8; i++) n += snprintf(l + n, sizeof l - (size_t)n, "[%d] st=%u ", i, mem_r16(base + i * 32));
            rt_log("ring @%08x: %s", base, l);
            { char l[400]; int n = 0; for (uint32_t a = 0x80069a10; a < 0x80069a50; a += 4) n += snprintf(l + n, sizeof l - (size_t)n, "%08x ", mem_r32(a));
              rt_log("libetc intr @80069a10: %s  vsync counter=%u  a0=%08x a1=%08x [699ac]=%08x [699a8]=%08x", l, mem_r32(0x8006aad8), g_cpu->r[4], g_cpu->r[5], mem_r32(0x800699ac), mem_r32(0x800699a8)); }
            { uint32_t t = mem_r32(0x80076d6c); rt_log("[80076d6c]=%08x -> +16=%08x  ram[100..11c]=%08x %08x %08x %08x %08x %08x", t, mem_r32(t + 16), mem_r32(0x100), mem_r32(0x104), mem_r32(0x108), mem_r32(0x10c), mem_r32(0x110), mem_r32(0x114)); }
            { int nz = 0; for (int y = 43; y < 129; y++) for (int x = 512; x < 546; x++) if (g_vram[y * 1024 + x]) nz++;
              int cz = 0; for (int y = 7; y < 9; y++) for (int x = 560; x < 576; x++) if (g_vram[y * 1024 + x]) cz++;
              rt_log("VRAM logo area nonzero words: %d / %d, logo CLUT nonzero: %d", nz, 86 * 34, cz); }
            { FILE *cv = fopen("build/shots/caption_vram.bin", "wb"); if (cv) { for (int y = 256; y < 512; y++) fwrite(&g_vram[y * 1024 + 768], 2, 64, cv); fclose(cv); } }
            rt_log("dicr=%08x dpcr=%08x istat=%03x imask=%03x", mmio_read(0x1F8010F4, 4), mmio_read(0x1F8010F0, 4), g_istat, g_imask);
        }
        wav_close();
    { extern int g_spu_log; extern void spu_debug_dump(void); if (g_spu_log) spu_debug_dump(); }
        rt_log("[probe] done");
        exit(0);
    }
}

int main(int argc, char **argv) {
    const char *disc = "Um Jammer Lammy (JP).bin";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_log_level = 2;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) max_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-shot")) shots = 1;
        else if (!strcmp(argv[i], "-confirm-cross")) g_confirm_cross = 1;
        else if (!strcmp(argv[i], "-unlock")) { extern int g_unlock_all; g_unlock_all = 1; }
        else if (!strcmp(argv[i], "-spulog")) { extern int g_spu_log; g_spu_log = 1; }
        else if (!strcmp(argv[i], "-xscale") && i + 1 < argc) g_gte_xscale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-burst") && i + 2 < argc) { burst_from = (int)(atof(argv[i + 1]) * 60); burst_n = atoi(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "-wav")) { wav = fopen("build/shots/audio.wav", "wb"); fseek(wav, 44, SEEK_SET); }
        else if (!strcmp(argv[i], "-press") && i + 1 < argc && npress < 4096) {
            char name[32] = {0}; float t = 0;
            if (sscanf(argv[++i], "%f:%31s", &t, name) == 2 && btn_bit(name))
                presses[npress++] = (Press){(int)(t * 60), btn_bit(name)};
        }
        else disc = argv[i];
    }
    if (!SDL_Init(0)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    { extern bool aux_disc_open(const char *); const char *u = getenv("UJL_USA_DISC"); aux_disc_open(u ? u : "Um Jammer Lammy (USA).bin"); }
    { extern void memcard_init(int, const char *); const char *cp = getenv("PROBE_CARD"); memcard_init(0, cp ? cp : "build/probe_card1.mcd"); memcard_init(1, NULL); }
    {   /* PROBE_IMPORT=path (PROBE_IMPORT_MODE=1 replaces the card) */
        extern int memcard_import(int, const char *, int, char *, size_t);
        const char *ip = getenv("PROBE_IMPORT");
        if (ip) { char msg[256]; const char *m = getenv("PROBE_IMPORT_MODE"); memcard_import(0, ip, m ? atoi(m) : 0, msg, sizeof msg); rt_log("import: %s", msg); }
    }
    if (!rt_boot(disc)) return 1;
    if (getenv("PROBE_RECLOG_AT")) { gpu_rec_install(); gpu_rec_set_enabled(1); } else gpu_soft_install();
    rt_run();
    return 0;
}
