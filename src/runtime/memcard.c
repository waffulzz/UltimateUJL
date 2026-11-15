/* Memory card: HLE of the BIOS card functions + the "bu00:" file system on a standard PS1 card image.
   The card is a raw 128 KB image (the usual .mcd layout, same as real cards and emulators): block 0 holds
   the "MC" header frame, 15 directory frames and the broken-sector list; blocks 1-15 hold file data
   (8 KB each, 64 frames of 128 bytes). A missing card file is created blank (formatted) on first use.
   Writes are flushed to disk shortly after the game stops writing. */
#include "psx.h"
#include <stdlib.h>
#include <SDL3/SDL.h>

#define CARD_SIZE (128 * 1024)
#define FRAME 128
#define BLOCK 8192
extern uint8_t g_card[2][CARD_SIZE];   /* shared with the SIO-level card in sio.c */
extern int g_card_present[2], g_card_dirty[2];

static char card_path[2][1024];
static int dirty_age[2];

/* ---------------------------------------------------------------- image format */
static void frame_checksum(uint8_t *f) {
    uint8_t x = 0;
    for (int i = 0; i < FRAME - 1; i++) x ^= f[i];
    f[FRAME - 1] = x;
}

static void card_format(uint8_t *mc) {
    memset(mc, 0, CARD_SIZE);
    mc[0] = 'M'; mc[1] = 'C';
    frame_checksum(mc);
    for (int i = 1; i < 16; i++) {                   /* directory: all free */
        uint8_t *d = mc + i * FRAME;
        d[0] = 0xA0;
        d[8] = d[9] = 0xFF;
        frame_checksum(d);
    }
    for (int i = 16; i < 36; i++) {                  /* broken sector list: none */
        uint8_t *d = mc + i * FRAME;
        memset(d, 0, FRAME);
        d[0] = d[1] = d[2] = d[3] = 0xFF;
        d[8] = d[9] = 0xFF;
        frame_checksum(d);
    }
    memcpy(mc + 63 * FRAME, mc, FRAME);              /* write-test frame = header copy */
}

static inline uint8_t *dir_entry(int slot, int block) { return g_card[slot] + block * FRAME; } /* block 1..15 */
static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void mark_dirty(int slot) { g_card_dirty[slot] = 1; dirty_age[slot] = 0; }

/* ---------------------------------------------------------------- persistence */
void memcard_flush(void) {
    for (int s = 0; s < 2; s++) {
        if (!g_card_dirty[s] || !card_path[s][0]) continue;
        char tmp[1100];
        SDL_snprintf(tmp, sizeof tmp, "%s.tmp", card_path[s]);
        FILE *f = fopen(tmp, "wb");
        if (!f) { LOGI("memcard: can't write %s", tmp); continue; }
        size_t n = fwrite(g_card[s], 1, CARD_SIZE, f);
        fclose(f);
        if (n != CARD_SIZE) { LOGI("memcard: short write %s", tmp); continue; }
        remove(card_path[s]);
        if (rename(tmp, card_path[s]) != 0) { LOGI("memcard: can't replace %s", card_path[s]); continue; }
        g_card_dirty[s] = 0;
        LOGI("memcard: saved %s", card_path[s]);
    }
}

/* called every vblank: write the image ~1 s after the last change */
void memcard_import_tick(void);
void memcard_tick(void) {
    memcard_import_tick();
    for (int s = 0; s < 2; s++)
        if (g_card_dirty[s] && ++dirty_age[s] >= 60) memcard_flush();
}

/* slot 0 = memory card slot 1. path NULL = no card in that slot. */
void memcard_init(int slot, const char *path) {
    slot &= 1;
    g_card_present[slot] = path != NULL;
    card_path[slot][0] = 0;
    if (!path) return;
    SDL_strlcpy(card_path[slot], path, sizeof card_path[slot]);
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    if (f) { n = fread(g_card[slot], 1, CARD_SIZE, f); fclose(f); }
    if (n == CARD_SIZE && g_card[slot][0] == 'M' && g_card[slot][1] == 'C') {
        LOGI("memcard: slot %d: %s", slot + 1, path);
        return;
    }
    card_format(g_card[slot]);   /* no (valid) card yet: a blank, formatted one in the standard layout */
    mark_dirty(slot);
    memcard_flush();
    LOGI("memcard: slot %d: created blank card %s", slot + 1, path);
}

/* ---------------------------------------------------------------- events */
#define EV_IOE 0x0004
#define EV_ERROR 0x8000
#define EV_TIMEOUT 0x0100
/* Card results arrive asynchronously on hardware (from the SIO interrupt, after the transfer). libcard clears
   its result flags after starting an operation and then waits for a callback, so delivering inside the call
   (before it clears them) hangs it. Results are queued and delivered from the next interrupt instead. */
static uint32_t pending_spec[8];
static int npending;
#define HW_ONLY 0x10000u
static void card_event(CPU *c, uint32_t spec) {
    (void)c;
    if (npending < 8) pending_spec[npending++] = spec;
}
static void card_event_hw(uint32_t spec) { if (npending < 8) pending_spec[npending++] = spec | HW_ONLY; }
/* called from the HLE exception handler (with a CPU context to run the game's event callbacks) */
void memcard_deliver(CPU *c) {
    if (!npending) return;
    uint32_t q[8];
    int n = npending;
    memcpy(q, pending_spec, sizeof q);
    npending = 0;
    for (int i = 0; i < n; i++) {
        uint32_t spec = q[i] & 0xFFFF;
        if (!(q[i] & HW_ONLY)) bios_deliver_event(c, 0xF4000001u, spec); /* SwCARD */
        bios_deliver_event(c, 0xF0000011u, spec);                         /* HwCARD */
    }
    { static int k; if (k++ < 8) LOGI("memcard: delivered %d result(s), spec %04x; flags %x %x %x %x", n, q[0],
        mem_r32(0x801c4b08), mem_r32(0x801c4b0c), mem_r32(0x801c4b10), mem_r32(0x801c4b14)); }
}

static int port_slot(uint32_t port) { return (port >> 4) & 1; }

/* A0:AB _card_info / A0:AC _card_load */
void memcard_info(CPU *c, uint32_t port) {
    card_event(c, g_card_present[port_slot(port)] ? EV_IOE : EV_TIMEOUT);
    c->r[2] = 1;
}

/* B0:4E write_card_sector / B0:4F read_card_sector (port, sector, buf) */
void memcard_sector(CPU *c, int write, uint32_t port, uint32_t sector, uint32_t buf) {
    int s = port_slot(port);
    if (!g_card_present[s] || sector >= CARD_SIZE / FRAME) { card_event(c, EV_ERROR); c->r[2] = 0; return; }
    uint8_t *p = g_card[s] + sector * FRAME;
    for (int i = 0; i < FRAME; i++) {
        if (write) p[i] = mem_r8(buf + i);
        else mem_w8(buf + i, p[i]);
    }
    if (write) mark_dirty(s);
    card_event(c, EV_IOE);
    c->r[2] = 1;
}

/* ---------------------------------------------------------------- bu00: file system */
typedef struct { int used, slot, first, async; uint32_t pos, size; } File;
static File files[8];
#define FD_BASE 2   /* 0/1 are stdin/stdout (the game prints through write(1, ...)) */

static int parse_name(uint32_t addr, int *slot, char *name) { /* "bu00:NAME" / "bu10:NAME" */
    char full[64];
    int i = 0;
    for (; i < 63 && (full[i] = (char)mem_r8(addr + i)); i++) {}
    full[i] = 0;
    if (SDL_strncasecmp(full, "bu", 2) || !full[2] || !full[3] || full[4] != ':') return 0;
    *slot = full[2] == '1';
    SDL_strlcpy(name, full + 5, 21);
    return 1;
}

static int find_file(int slot, const char *name) { /* first block or -1 */
    for (int b = 1; b < 16; b++) {
        uint8_t *d = dir_entry(slot, b);
        if (d[0] == 0x51 && !SDL_strncmp((char *)d + 10, name, 20)) return b;
    }
    return -1;
}

static int block_of(int slot, int first, uint32_t pos) { /* data block holding byte pos, or -1 */
    int b = first;
    for (uint32_t k = pos / BLOCK; k && b >= 1 && b < 16; k--) {
        uint16_t nx = (uint16_t)(dir_entry(slot, b)[8] | dir_entry(slot, b)[9] << 8);
        b = nx == 0xFFFF ? -1 : nx + 1;
    }
    return (b >= 1 && b < 16) ? b : -1;
}

static int create_file(int slot, const char *name, int blocks) {
    int list[15], n = 0;
    for (int b = 1; b < 16 && n < blocks; b++) {
        uint8_t st = dir_entry(slot, b)[0];
        if (st == 0xA0 || (st >= 0xA1 && st <= 0xA3)) list[n++] = b;
    }
    if (n < blocks || blocks <= 0) return -1;
    for (int i = 0; i < n; i++) {
        uint8_t *d = dir_entry(slot, list[i]);
        memset(d, 0, FRAME);
        d[0] = n == 1 ? 0x51 : i == 0 ? 0x51 : i == n - 1 ? 0x53 : 0x52;
        if (i == 0) { wr32(d + 4, (uint32_t)n * BLOCK); SDL_strlcpy((char *)d + 10, name, 21); }
        uint16_t nx = i == n - 1 ? 0xFFFF : (uint16_t)(list[i + 1] - 1);
        d[8] = (uint8_t)nx; d[9] = (uint8_t)(nx >> 8);
        frame_checksum(d);
        memset(g_card[slot] + list[i] * BLOCK, 0, BLOCK);
    }
    mark_dirty(slot);
    return list[0];
}

/* B0:32 open(name, mode) */
int memcard_open(uint32_t name_addr, uint32_t mode) {
    int slot;
    char name[21];
    if (!parse_name(name_addr, &slot, name) || !g_card_present[slot]) return -1;
    int first = find_file(slot, name);
    if (mode & 0x200) {                 /* FCREAT: blocks in the upper 16 bits; fails if it exists */
        if (first >= 0) { LOGI("memcard: create %s: exists", name); return -1; }
        first = create_file(slot, name, (int)(mode >> 16));
        if (first < 0) { LOGI("memcard: create %s: card full", name); return -1; }
        LOGI("memcard: created %s (%d block(s))", name, (int)(mode >> 16));
    } else if (first < 0) return -1;
    for (int i = 0; i < 8; i++)
        if (!files[i].used) {
            files[i] = (File){1, slot, first, (mode & 0x8000) != 0, 0, rd32(dir_entry(slot, first) + 4)};
            return FD_BASE + i;
        }
    return -1;
}

static File *fd_file(uint32_t fd) {
    if (fd < FD_BASE || fd >= FD_BASE + 8 || !files[fd - FD_BASE].used) return NULL;
    return &files[fd - FD_BASE];
}
int memcard_is_fd(uint32_t fd) { return fd_file(fd) != NULL; }

/* B0:34 read / B0:35 write (fd, buf, len) */
int memcard_rw(CPU *c, int write, uint32_t fd, uint32_t buf, uint32_t len) {
    File *f = fd_file(fd);
    if (!f) return -1;
    uint32_t done = 0;
    while (done < len && f->pos < f->size) {
        int b = block_of(f->slot, f->first, f->pos);
        if (b < 0) break;
        uint8_t *p = g_card[f->slot] + b * BLOCK + (f->pos % BLOCK);
        if (write) *p = mem_r8(buf + done);
        else mem_w8(buf + done, *p);
        done++;
        f->pos++;
    }
    if (write && done) mark_dirty(f->slot);
    if (f->async) { /* async: the call only starts the transfer (0 = accepted); the result comes as an event */
        card_event(c, done ? EV_IOE : EV_ERROR);
        return 0;
    }
    return (int)done;
}

/* B0:33 lseek(fd, off, whence) */
int memcard_seek(uint32_t fd, int32_t off, int whence) {
    File *f = fd_file(fd);
    if (!f) return -1;
    int64_t p = whence == 0 ? off : whence == 1 ? (int64_t)f->pos + off : (int64_t)f->size + off;
    if (p < 0 || p > f->size) return -1;
    f->pos = (uint32_t)p;
    return (int)p;
}

int memcard_close(uint32_t fd) {
    File *f = fd_file(fd);
    if (!f) return -1;
    f->used = 0;
    return (int)fd;
}

/* B0:42 firstfile(pattern, dirent) / B0:43 nextfile(dirent): DIRENTRY {name[20], attr, size, next, head, system[4]} */
static int ff_slot, ff_next;
static char ff_pat[21];
static int pat_match(const char *pat, const char *name) {
    for (; *pat; pat++, name++) {
        if (*pat == '*') return 1;
        if (*pat != '?' && *pat != *name) return 0;
    }
    return *name == 0;
}
static uint32_t ff_scan(uint32_t dirent) {
    for (; ff_next < 16; ff_next++) {
        uint8_t *d = dir_entry(ff_slot, ff_next);
        if (d[0] != 0x51 || !pat_match(ff_pat, (char *)d + 10)) continue;
        for (int i = 0; i < 20; i++) mem_w8(dirent + i, d[10 + i]);
        mem_w32(dirent + 20, 0x50);              /* attr */
        mem_w32(dirent + 24, rd32(d + 4));       /* size */
        mem_w32(dirent + 28, 0);                 /* next */
        mem_w32(dirent + 32, (uint32_t)ff_next); /* head */
        mem_w32(dirent + 36, 0);
        ff_next++;
        return dirent;
    }
    return 0;
}
/* libcard's directory scan treats 0 as "not read yet": it waits for a HwCARD event and retries. Signal that
   the (instant) read finished so a card with no matching files doesn't hang it. */
uint32_t memcard_firstfile(uint32_t pattern, uint32_t dirent) {
    char name[21];
    if (!parse_name(pattern, &ff_slot, name) || !g_card_present[ff_slot]) { card_event_hw(EV_TIMEOUT); return 0; }
    SDL_strlcpy(ff_pat, name[0] ? name : "*", sizeof ff_pat);
    ff_next = 1;
    uint32_t r = ff_scan(dirent);
    if (!r) card_event_hw(EV_IOE);
    return r;
}
uint32_t memcard_nextfile(uint32_t dirent) {
    uint32_t r = ff_scan(dirent);
    if (!r) card_event_hw(EV_IOE);
    return r;
}

/* B0:45 erase(name) */
int memcard_erase(uint32_t name_addr) {
    int slot;
    char name[21];
    if (!parse_name(name_addr, &slot, name)) return 0;
    int b = find_file(slot, name);
    if (b < 0) return 0;
    while (b >= 1 && b < 16) {
        uint8_t *d = dir_entry(slot, b);
        uint16_t nx = (uint16_t)(d[8] | d[9] << 8);
        memset(d, 0, FRAME);
        d[0] = 0xA0;
        d[8] = d[9] = 0xFF;
        frame_checksum(d);
        b = nx == 0xFFFF ? -1 : nx + 1;
    }
    mark_dirty(slot);
    LOGI("memcard: erased %s", name);
    return 1;
}

/* B0:44 rename(old, new) */
int memcard_rename(uint32_t old_addr, uint32_t new_addr) {
    int s1, s2;
    char a[21], b[21];
    if (!parse_name(old_addr, &s1, a) || !parse_name(new_addr, &s2, b) || s1 != s2) return 0;
    int blk = find_file(s1, a);
    if (blk < 0 || find_file(s1, b) >= 0) return 0;
    uint8_t *d = dir_entry(s1, blk);
    memset(d + 10, 0, 21);
    SDL_strlcpy((char *)d + 10, b, 21);
    frame_checksum(d);
    mark_dirty(s1);
    return 1;
}

/* ---------------------------------------------------------------- importing card dumps and single saves */
/* Whole cards: raw .mcd/.mcr/.srm/.ps (128 KB), DexDrive .gme (3904-byte header), VGS/Connectix .mem/.vgs
   (64-byte header), PSP/Vita .vmp (0x80 header). Single saves: .mcs/.psx/.mcb (a 128-byte directory frame
   then the data blocks) and bare saves (just the data blocks, starting with "SC").
   mode 0: copy the Um Jammer Lammy saves (BISCPS-18011*) into the current card (replacing same-named ones);
   mode 1: replace the whole card. Returns a short message for the UI. */
static const char *k_game_prefix = "BISCPS-18011";

static int free_blocks(int slot) {
    int n = 0;
    for (int b = 1; b < 16; b++) { uint8_t st = dir_entry(slot, b)[0]; if (st == 0xA0 || (st >= 0xA1 && st <= 0xA3)) n++; }
    return n;
}

static void erase_name(int slot, const char *name) {
    int b = find_file(slot, name);
    while (b >= 1 && b < 16) {
        uint8_t *d = dir_entry(slot, b);
        uint16_t nx = (uint16_t)(d[8] | d[9] << 8);
        memset(d, 0, FRAME);
        d[0] = 0xA0; d[8] = d[9] = 0xFF;
        frame_checksum(d);
        b = nx == 0xFFFF ? -1 : nx + 1;
    }
}

/* write one save (name + nblocks of data) into the card; 0 if no room */
static int put_save(int slot, const char *name, const uint8_t *data, int nblocks) {
    erase_name(slot, name);
    if (free_blocks(slot) < nblocks) return 0;
    int first = create_file(slot, name, nblocks);
    if (first < 0) return 0;
    int b = first;
    for (int i = 0; i < nblocks && b >= 1 && b < 16; i++) {
        memcpy(g_card[slot] + b * BLOCK, data + i * BLOCK, BLOCK);
        uint16_t nx = (uint16_t)(dir_entry(slot, b)[8] | dir_entry(slot, b)[9] << 8);
        b = nx == 0xFFFF ? -1 : nx + 1;
    }
    mark_dirty(slot);
    return 1;
}

static int is_card_image(const uint8_t *m) { return m[0] == 'M' && m[1] == 'C'; }

static int copy_game_saves(int slot, const uint8_t *src, char *msg, size_t msgsz) {
    int copied = 0, skipped = 0, others = 0;
    for (int b = 1; b < 16; b++) {
        const uint8_t *d = src + b * FRAME;
        if (d[0] != 0x51) continue;
        char name[21];
        memcpy(name, d + 10, 20); name[20] = 0;
        /* this game's saves; US (BASCUS-94448) saves use the same layout for the part the JP game reads, so
           they're copied under the JP name (same suffix, e.g. replays named after the player) */
        if (!SDL_strncmp(name, "BASCUS-94448", 12)) {
            char jp[21];
            SDL_snprintf(jp, sizeof jp, "%s%s", k_game_prefix, name + 12);
            SDL_strlcpy(name, jp, sizeof name);
        } else if (SDL_strncmp(name, k_game_prefix, SDL_strlen(k_game_prefix))) { others++; continue; }
        uint8_t buf[15 * BLOCK];
        int n = 0, blk = b;
        while (blk >= 1 && blk < 16 && n < 15) {
            memcpy(buf + n * BLOCK, src + blk * BLOCK, BLOCK);
            n++;
            uint16_t nx = (uint16_t)(src[blk * FRAME + 8] | src[blk * FRAME + 9] << 8);
            blk = nx == 0xFFFF ? -1 : nx + 1;
        }
        if (put_save(slot, name, buf, n)) copied++; else skipped++;
    }
    if (copied)
        SDL_snprintf(msg, msgsz, "Copied %d Um Jammer Lammy save(s)%s.", copied, skipped ? " (some didn't fit)" : "");
    else
        SDL_snprintf(msg, msgsz, others ? "No Um Jammer Lammy (Japan) saves on that card (%d other save(s))." :
                                          "That card has no saves.", others);
    return copied > 0;
}

int memcard_import(int slot, const char *path, int mode, char *msg, size_t msgsz) {
    slot &= 1;
    FILE *f = fopen(path, "rb");
    if (!f) { SDL_snprintf(msg, msgsz, "Can't open that file."); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); SDL_snprintf(msg, msgsz, "Not a memory card file."); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if ((long)got != size) { free(buf); SDL_snprintf(msg, msgsz, "Couldn't read that file."); return 0; }

    const uint8_t *card = NULL;
    static const long hdrs[] = {0, 3904, 64, 0x80, 0x200};  /* raw, .gme, .vgs/.mem, .vmp, misc */
    for (size_t i = 0; i < sizeof hdrs / sizeof hdrs[0] && !card; i++)
        if (size >= hdrs[i] + CARD_SIZE && is_card_image(buf + hdrs[i])) card = buf + hdrs[i];

    int ok = 0;
    if (card) {
        if (mode == 1) {
            memcpy(g_card[slot], card, CARD_SIZE);
            mark_dirty(slot);
            SDL_snprintf(msg, msgsz, "Replaced the memory card with that dump.");
            ok = 1;
        } else ok = copy_game_saves(slot, card, msg, msgsz);
    } else if (size >= FRAME + BLOCK && (size - FRAME) % BLOCK == 0 && buf[0] == 0x51) {
        /* single save with its directory frame (.mcs / .psx / .mcb) */
        char name[21];
        memcpy(name, buf + 10, 20); name[20] = 0;
        int n = (int)((size - FRAME) / BLOCK);
        if (n > 15) n = 15;
        ok = put_save(slot, name, buf + FRAME, n);
        SDL_snprintf(msg, msgsz, ok ? "Imported save %s." : "Not enough free blocks for %s.", name);
    } else if (size >= BLOCK && size % BLOCK == 0 && buf[0] == 'S' && buf[1] == 'C') {
        /* bare save data: assume it's this game's system data */
        int n = (int)(size / BLOCK);
        if (n > 15) n = 15;
        ok = put_save(slot, k_game_prefix, buf, n);
        SDL_snprintf(msg, msgsz, ok ? "Imported the save as %s." : "Not enough free blocks.", k_game_prefix);
    } else {
        SDL_snprintf(msg, msgsz, "Not a memory card dump or save this port recognises.");
    }
    free(buf);
    if (ok) memcard_flush();
    return ok;
}

/* list this game's saves on the card (for the settings panel) */
int memcard_list(int slot, char names[][21], int *blocks, int max) {
    int n = 0;
    for (int b = 1; b < 16 && n < max; b++) {
        uint8_t *d = dir_entry(slot & 1, b);
        if (d[0] != 0x51) continue;
        memcpy(names[n], d + 10, 20); names[n][20] = 0;
        blocks[n] = (int)(rd32(d + 4) / BLOCK);
        n++;
    }
    return n;
}
int memcard_free_blocks(int slot) { return free_blocks(slot & 1); }
const char *memcard_path(int slot) { return card_path[slot & 1]; }

/* Imports requested from the UI run on the game thread (at vblank), so they never race the game's card access. */
static char imp_path[1024], imp_msg[256];
static int imp_mode, imp_pending, imp_done;
void memcard_request_import(const char *path, int mode) {
    SDL_strlcpy(imp_path, path, sizeof imp_path);
    imp_mode = mode;
    imp_done = 0;
    imp_pending = 1;
}
const char *memcard_import_result(void) { return imp_done ? imp_msg : NULL; }
void memcard_import_tick(void) {
    if (!imp_pending) return;
    imp_pending = 0;
    memcard_import(0, imp_path, imp_mode, imp_msg, sizeof imp_msg);
    LOGI("memcard: import %s: %s", imp_path, imp_msg);
    imp_done = 1;
}
