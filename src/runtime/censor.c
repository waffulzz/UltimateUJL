/* Uncensored / censored content (setting "Uncensored content", ini `uncensored`, default on).
 *
 * The base game is the Japanese release, which is the uncensored version. The USA release changed Stages 1, 5 and
 * 6. With the setting off (and the USA disc imported) the USA versions are served in place of the JP data, through
 * the same sector patch layer as the English patches, so the JP code runs unchanged. The setting is read on every
 * sector, so a change applies the next time a stage loads. What differs (found by comparing the two discs; Stages 2,
 * 3, 4 and 7 have no such differences):
 *
 *  - Song vocals: S1/ST1.XA, S5/ST5.XA and S6/ST6.XA have the same size and sector layout on both discs; some vocal
 *    channels differ in places (S1: channels 2-4 and 6, 52 sectors each; S5/S6: channels 2-7). Audio sectors are
 *    copied from the USA file (subheader + audio only, never the header: the game reads positions back as its song
 *    clock). S1 channel 7 is the staff-roll song, which the English credits patch handles.
 *  - Lyrics: each song overlay (COMOD*.SNG/CMP/COP) carries its English lyric timelines as 12-byte records
 *    {duration, end, text pointer}. The USA overlays (loaded at 0x801c5750 instead of 0x801c5110) have the same
 *    timelines with identical timing; a few records point at different lines. Each JP timeline is matched to the
 *    USA copy with the same timing words, and the records whose text differs are pointed at the USA line, copied
 *    into a small pool in kernel RAM the HLE BIOS leaves unused (0x5000-0x6FFF, between its dummy C0 handlers and
 *    the device control block).
 *  - Stage archives (CMP*.INT): sections of named entries (TIMs, sound banks .VB/.VH, models .TMD, animations .TOD,
 *    data). Each section is rebuilt with the USA version of every entry whose data differs (and the USA-only entries
 *    appended), if it still fits the section's sectors. That covers the Stage 5/6 voice sample banks, Lammy's Stage
 *    6 outfit (textures, model and data) and Stage 1's changed animations.
 *  - Stage 6 movie (S6/S6.IKI): re-encoded and shorter on the USA disc (1964 frames vs 2043). The USA stream is
 *    served from the start of the JP file; past its end the USA end-of-file sector repeats.
 *
 * Archives and overlays are rebuilt the first time the game reads them (only text and sizes are logged, never the
 * lines). UJL_FORCE_CENSORED=1 turns the setting off for tests. */
#include "psx.h"
#include "recomp.h"
#include <stdlib.h>
#include <string.h>

bool disc_find_file(const char *path, uint32_t *lba, uint32_t *size);
bool disc_read_file(const char *path, uint8_t **data, uint32_t *size);
bool aux_disc_ready(void);
bool aux_disc_find_file(const char *path, uint32_t *lba, uint32_t *size);
bool aux_disc_read_file(const char *path, uint8_t **data, uint32_t *size);
bool aux_disc_read_sector(uint32_t lba, uint8_t out[2352]);

int g_uncensored = 1;

#define JP_OVL_BASE 0x801c5110u
#define US_OVL_BASE 0x801c5750u
#define POOL_ADDR   0x00005000u   /* kernel RAM unused by the HLE BIOS (bios.c: C0 handlers end at 0x5000, DCB at 0x7000) */
#define POOL_SIZE   0x2000u

static int ready;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* ---------------------------------------------------------------- XA song vocals */
typedef struct { uint32_t jl, ul, n; int skip_ch; } XaSwap;
static XaSwap xa[3];
static int nxa;

/* ---------------------------------------------------------------- Stage 6 movie */
static uint32_t mv_jl, mv_jn, mv_ul, mv_un, mv_ulast;   /* mv_ulast: the USA end-of-file sector */
/* The movie player stops after frame (file size / 20480 - 5) of the size in the directory record (JP: 2043, USA:
   1964), so the /S6 directory record of S6.IKI reports the USA size too, or the player would wait for frames the
   USA stream doesn't have. */
static uint32_t mv_dir_lba, mv_dir_off, mv_us_size;

bool disc_read_sector(uint32_t lba, uint8_t out[2352]);
static int name_is(const uint8_t *rec, const char *name) {
    size_t n = strlen(name);
    return rec[32] >= n && !memcmp(rec + 33, name, n) && (rec[32] == n || rec[33 + n] == ';');
}
/* Sector and byte offset (in the sector's user data) of file `name`'s record in top-level directory `dir`. */
static int find_dir_record(const char *dir, const char *name, uint32_t *lba_out, uint32_t *off_out) {
    uint8_t sec[2352];
    if (!disc_read_sector(16, sec)) return 0;
    uint32_t lba = rd32(sec + 24 + 156 + 2), size = rd32(sec + 24 + 156 + 10);
    for (int level = 0; level < 2; level++) {
        const char *want = level ? name : dir;
        int found = 0;
        for (uint32_t s = 0; s < (size + 2047) / 2048 && !found; s++) {
            if (!disc_read_sector(lba + s, sec)) return 0;
            for (uint32_t i = 0; i < 2048;) {
                const uint8_t *r = sec + 24 + i;
                if (!r[0]) break;
                if (name_is(r, want)) {
                    if (level) { *lba_out = lba + s; *off_out = i; return 1; }
                    lba = rd32(r + 2); size = rd32(r + 10); found = 1;
                    break;
                }
                i += r[0];
            }
        }
        if (!found) return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- rebuilt files (overlays, archives) */
typedef enum { CF_OVERLAY, CF_ARCHIVE } CfKind;
typedef struct {
    const char *path;
    CfKind kind;
    uint32_t lba, nsec;
    int state;          /* 0 not built yet, 1 built (data != NULL when something changed), -1 failed/unchanged */
    uint8_t *data;      /* nsec * 2048 bytes */
} CensorFile;

static CensorFile cfiles[] = {
    {"S1/COMOD1.SNG", CF_OVERLAY}, {"S1/COMOD11.SNG", CF_OVERLAY},
    {"S5/COMOD5.SNG", CF_OVERLAY}, {"S5/COMOD5.CMP", CF_OVERLAY}, {"S5/COMOD5.COP", CF_OVERLAY},
    {"S5/COMOD15.SNG", CF_OVERLAY}, {"S5/COMOD15.CMP", CF_OVERLAY}, {"S5/COMOD15.COP", CF_OVERLAY},
    {"S6/COMOD6.SNG", CF_OVERLAY}, {"S6/COMOD6.CMP", CF_OVERLAY}, {"S6/COMOD6.COP", CF_OVERLAY},
    {"S6/COMOD16.SNG", CF_OVERLAY}, {"S6/COMOD16.CMP", CF_OVERLAY}, {"S6/COMOD16.COP", CF_OVERLAY},
    {"S1/CMP1SNG.INT", CF_ARCHIVE}, {"S1/CMP11SNG.INT", CF_ARCHIVE},
    {"S5/CMP5SNG.INT", CF_ARCHIVE}, {"S5/CMP5CMP.INT", CF_ARCHIVE}, {"S5/CMP5COP.INT", CF_ARCHIVE},
    {"S5/CMP15SNG.INT", CF_ARCHIVE}, {"S5/CMP15CMP.INT", CF_ARCHIVE}, {"S5/CMP15COP.INT", CF_ARCHIVE},
    {"S6/CMP6SNG.INT", CF_ARCHIVE}, {"S6/CMP6CMP.INT", CF_ARCHIVE}, {"S6/CMP6COP.INT", CF_ARCHIVE},
    {"S6/CMP16SNG.INT", CF_ARCHIVE}, {"S6/CMP16CMP.INT", CF_ARCHIVE}, {"S6/CMP16COP.INT", CF_ARCHIVE},
};
#define NCF ((int)(sizeof cfiles / sizeof cfiles[0]))

static uint8_t pool[POOL_SIZE];
static uint32_t pool_used;
static int building;


/* ---- lyric timelines ---- */
/* Length of a caption string at file offset `off` (printable ASCII and '\n', NUL-terminated), or -1. */
static int caption_len(const uint8_t *d, uint32_t n, uint32_t off) {
    for (uint32_t i = 0; i < 200 && off + i < n; i++) {
        uint8_t c = d[off + i];
        if (!c) return (int)i;
        if ((c < 0x20 && c != '\n') || c >= 0x7F) return -1;
    }
    return -1;
}

/* Number of valid timeline records starting at offset i. */
static int timeline_len(const uint8_t *d, uint32_t n, uint32_t i, uint32_t base) {
    int k = 0;
    uint32_t prev_end = 0;
    for (uint32_t j = i; j + 12 <= n; j += 12, k++) {
        uint32_t dur = rd32(d + j), end = rd32(d + j + 4), ptr = rd32(d + j + 8);
        if (ptr < base || ptr >= base + n || end < prev_end || end > 200000 || dur > 20000) break;
        if (caption_len(d, n, ptr - base) < 0) break;
        prev_end = end;
    }
    return k;
}

static uint32_t pool_add(const uint8_t *s, uint32_t len) {
    for (uint32_t o = 0; o + len < pool_used; o++)   /* the same line is often used by several records */
        if (!memcmp(pool + o, s, len) && pool[o + len] == 0 && (o == 0 || pool[o - 1] == 0)) return o;
    if (pool_used + len + 1 > POOL_SIZE) return UINT32_MAX;
    uint32_t o = pool_used;
    memcpy(pool + o, s, len);
    pool[o + len] = 0;
    pool_used += (len + 4) & ~3u;
    return o;
}

/* Point the JP lyric records whose line differs from the USA overlay's at the USA line. Returns records changed. */
static int patch_lyrics(uint8_t *jp, uint32_t jn, const uint8_t *us, uint32_t un) {
    int changed = 0;
    for (uint32_t i = 0; i + 12 * 10 <= jn;) {
        int c = timeline_len(jp, jn, i, JP_OVL_BASE);
        if (c < 10) { i += 4; continue; }          /* lyrics have 10+ records; shorter runs are menus/messages */
        /* the USA copy with the same timing words and the fewest differing lines */
        uint32_t best = UINT32_MAX;
        int best_diff = c + 1;
        for (uint32_t p = 0; p + 12u * (uint32_t)c <= un; p += 4) {
            if (rd32(us + p) != rd32(jp + i) || rd32(us + p + 4) != rd32(jp + i + 4)) continue;
            int k, diff = 0;
            for (k = 0; k < c; k++) {
                const uint8_t *a = jp + i + 12 * k, *b = us + p + 12 * k;
                if (rd32(a) != rd32(b) || rd32(a + 4) != rd32(b + 4)) break;
                uint32_t ub = rd32(b + 8) - US_OVL_BASE;
                int ul = ub < un ? caption_len(us, un, ub) : -1;
                if (ul < 0) break;
                uint32_t jb = rd32(a + 8) - JP_OVL_BASE;
                if (strcmp((const char *)jp + jb, (const char *)us + ub)) diff++;
            }
            if (k == c && diff < best_diff) { best_diff = diff; best = p; }
        }
        if (best != UINT32_MAX && best_diff > 0 && best_diff <= c / 4) {
            for (int k = 0; k < c; k++) {
                uint8_t *a = jp + i + 12 * k;
                uint32_t ub = rd32(us + best + 12 * k + 8) - US_OVL_BASE, jb = rd32(a + 8) - JP_OVL_BASE;
                if (!strcmp((const char *)jp + jb, (const char *)us + ub)) continue;
                uint32_t o = pool_add(us + ub, (uint32_t)caption_len(us, un, ub));
                if (o == UINT32_MAX) { LOGI("censor: lyric pool full"); return changed; }
                wr32(a + 8, 0x80000000u | (POOL_ADDR + o));
                changed++;
            }
        }
        i += 12u * (uint32_t)c;
    }
    return changed;
}

/* ---- archives ---- */
typedef struct { char name[17]; const uint8_t *data; uint32_t size; } IntEnt;
typedef struct { uint32_t off, type, n, span; IntEnt *ents; } IntSec;

/* Section headers sit on 0x800 boundaries: {type, entry count, data sectors, ?} + 20-byte {size, name[16]}
   entries, padded to a 0x2000 boundary; then the data (that many sectors), entries back to back on 4-byte
   boundaries. The next section follows right after. span = header block + data sectors. */
static int int_sections(const uint8_t *d, uint32_t n, IntSec *out, int max) {
    int ns = 0;
    for (uint32_t o = 0; o + 36 <= n && ns < max; o += 0x800) {
        uint32_t t = rd32(d + o), cnt = rd32(d + o + 4), sec = rd32(d + o + 8);
        if (!t || t >= 16 || !cnt || cnt >= 4000 || !sec) continue;
        uint32_t hdr = (16 + cnt * 20 + 0x1FFF) & ~0x1FFFu;
        if (o + hdr + sec * 0x800 > n || !memchr(d + o + 20, '.', 16)) continue;
        IntSec *s = &out[ns];
        s->off = o; s->type = t; s->n = cnt; s->span = hdr + sec * 0x800;
        s->ents = (IntEnt *)calloc(cnt, sizeof(IntEnt));
        uint32_t pos = o + hdr;
        int ok = 1;
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *e = d + o + 16 + i * 20;
            IntEnt *en = &s->ents[i];
            en->size = rd32(e);
            memcpy(en->name, e + 4, 16);
            if (pos + en->size > n) { ok = 0; break; }
            en->data = d + pos;
            pos += (en->size + 3) & ~3u;
        }
        if (!ok) { free(s->ents); continue; }
        ns++;
        o += s->span - 0x800;   /* skip the section body */
    }
    return ns;
}

static const IntEnt *int_lookup(const IntSec *s, const char *name) {
    for (uint32_t i = 0; i < s->n; i++)
        if (!strncmp(s->ents[i].name, name, 16)) return &s->ents[i];
    return NULL;
}
static const IntEnt *int_lookup_any(const IntSec *s, int n, const char *name) {
    for (int i = 0; i < n; i++) { const IntEnt *e = int_lookup(&s[i], name); if (e) return e; }
    return NULL;
}

/* Rebuild the JP archive's sections with the USA entries. The game addresses entries by index (no names in its
   code), so every JP index keeps its meaning: an entry the USA archives also have gets the USA data in place;
   an entry no USA archive has (e.g. the Japanese caption sheet MVST*SQ.TIM) gives its slot to an entry only the
   USA archive has (Stage 6: the LA12 boot texture); further USA-only entries are appended. Entries the USA disc
   merely moved to another section count as present. Returns entries replaced or added. */
static int patch_archive(uint8_t *out, const uint8_t *jp, uint32_t jn, const uint8_t *us, uint32_t un, const char *path) {
    IntSec js[8], usec[8];
    int nj = int_sections(jp, jn, js, 8), nu = int_sections(us, un, usec, 8);
    int total = 0, paired = 1;
    /* A section that needs more room than it has grows into the free space after the last section; the sections
       after it move down (each header gives its data sector count, which is how the loader walks them). */
    uint32_t last_end = nj ? js[nj - 1].off + js[nj - 1].span : jn;
    uint32_t room = jn > last_end ? jn - last_end : 0, shift = 0;
    memset(out, 0, jn);
    memcpy(out, jp, nj ? js[0].off : jn);
    for (int si = 0; si < nj; si++) {
        IntSec *a = &js[si];
        uint32_t dst = a->off + shift;
        int built = 0;
        if (paired && si < nu && usec[si].type != a->type) paired = 0;
        if (paired && si < nu) {
            IntSec *b = &usec[si];
            uint32_t cap = a->n + b->n;
            const IntEnt **list = (const IntEnt **)calloc(cap, sizeof *list);
            uint32_t *donor = (uint32_t *)calloc(a->n, sizeof *donor);
            uint32_t cnt = 0, ndonor = 0;
            int repl = 0, add = 0;
            for (uint32_t i = 0; i < a->n; i++) {
                const IntEnt *u = int_lookup(b, a->ents[i].name);
                if (!u) u = int_lookup_any(usec, nu, a->ents[i].name);
                if (u && (u->size != a->ents[i].size || memcmp(u->data, a->ents[i].data, u->size))) { list[cnt++] = u; repl++; }
                else {
                    if (!u) donor[ndonor++] = cnt;   /* JP-only entry: its slot can hold a USA-only one */
                    list[cnt++] = &a->ents[i];
                }
            }
            uint32_t used_donor = 0;
            for (uint32_t i = 0; i < b->n; i++) {
                if (int_lookup_any(js, nj, b->ents[i].name)) continue;
                if (used_donor < ndonor) { list[donor[used_donor++]] = &b->ents[i]; repl++; }
            }
            uint32_t base_cnt = cnt, skip = used_donor;
            for (uint32_t i = 0; i < b->n; i++) {
                if (int_lookup_any(js, nj, b->ents[i].name)) continue;
                if (skip) { skip--; continue; }   /* already placed in a donor slot */
                list[cnt++] = &b->ents[i];
                add++;
            }
            free(donor);
            for (int attempt = 0; attempt < 2 && (repl || add); attempt++) {
                uint32_t data_at = (16 + cnt * 20 + 0x1FFF) & ~0x1FFFu, sz = data_at, span = a->span;
                for (uint32_t i = 0; i < cnt; i++) sz += (list[i]->size + 3) & ~3u;
                if (sz > span && ((sz - span + 0x7FF) & ~0x7FFu) <= room) span += (sz - span + 0x7FF) & ~0x7FFu;
                if (sz <= span) {
                    uint8_t *s = out + dst;
                    memcpy(s, jp + a->off, 16);
                    wr32(s + 4, cnt);
                    wr32(s + 8, (span - data_at) / 0x800);
                    uint32_t pos = data_at;
                    for (uint32_t i = 0; i < cnt; i++) {
                        wr32(s + 16 + i * 20, list[i]->size);
                        memcpy(s + 16 + i * 20 + 4, list[i]->name, 16);
                        memcpy(s + pos, list[i]->data, list[i]->size);
                        pos += (list[i]->size + 3) & ~3u;
                    }
                    LOGI("censor: %s section %d (type %u): %d entr%s from the USA disc, %d added (%u of %u bytes%s)",
                         path, si, a->type, repl, repl == 1 ? "y" : "ies", (int)(cnt - base_cnt), sz, span,
                         span > a->span ? ", grown" : "");
                    total += repl + (int)(cnt - base_cnt);
                    room -= span - a->span;
                    shift += span - a->span;
                    built = 1;
                    break;
                }
                if (attempt == 0 && add) { cnt = base_cnt; add = 0; continue; }   /* retry without the USA-only entries */
                LOGI("censor: %s section %d: the USA entries don't fit (%u > %u bytes), kept the JP section", path, si, sz, span);
                break;
            }
            free(list);
        }
        if (!built) memcpy(out + dst, jp + a->off, a->span);
        /* whatever sits between this section and the next (or the end of the file) */
        uint32_t from = a->off + a->span, to = si + 1 < nj ? js[si + 1].off : jn;
        if (to > from && from + shift < jn) memcpy(out + from + shift, jp + from, (to - from < jn - from - shift) ? to - from : jn - from - shift);
    }
    for (int i = 0; i < nj; i++) free(js[i].ents);
    for (int i = 0; i < nu; i++) free(usec[i].ents);
    return total;
}

static void build_file(CensorFile *f) {
    f->state = -1;
    uint8_t *jp = NULL, *us = NULL;
    uint32_t jn = 0, un = 0;
    building = 1;
    int ok = disc_read_file(f->path, &jp, &jn) && aux_disc_read_file(f->path, &us, &un);
    building = 0;
    if (!ok) { free(jp); free(us); LOGI("censor: %s: not on both discs", f->path); return; }
    uint8_t *out = (uint8_t *)calloc(f->nsec, 2048);
    memcpy(out, jp, jn);
    int n = 0;
    if (f->kind == CF_OVERLAY) {
        n = patch_lyrics(out, jn, us, un);
        if (n) LOGI("censor: %s: %d lyric record(s) use the USA line", f->path, n);
    } else
        n = patch_archive(out, jp, jn, us, un, f->path);
    free(jp);
    free(us);
    if (!n) { free(out); return; }
    f->data = out;
    f->state = 1;
}

/* ---------------------------------------------------------------- setup and the sector hook */
void censor_init(void) {
    ready = 0;
    nxa = 0;
    mv_jn = 0;
    pool_used = 0;
    if (getenv("UJL_FORCE_CENSORED")) g_uncensored = 0;
    for (int i = 0; i < NCF; i++) {
        CensorFile *f = &cfiles[i];
        free(f->data);
        f->data = NULL;
        f->state = -1;
        uint32_t lba, size;
        if (disc_find_file(f->path, &lba, &size)) { f->lba = lba; f->nsec = (size + 2047) / 2048; f->state = 0; }
    }
    if (!aux_disc_ready()) return;
    static const struct { const char *path; int skip; } songs[] = {{"S1/ST1.XA", 7}, {"S5/ST5.XA", -1}, {"S6/ST6.XA", -1}};
    for (int i = 0; i < 3; i++) {
        uint32_t jl, js, ul, us;
        if (disc_find_file(songs[i].path, &jl, &js) && aux_disc_find_file(songs[i].path, &ul, &us) && js == us)
            xa[nxa++] = (XaSwap){jl, ul, js / 2048, songs[i].skip};
    }
    uint32_t js, us;
    if (disc_find_file("S6/S6.IKI", &mv_jl, &js) && aux_disc_find_file("S6/S6.IKI", &mv_ul, &us) && us <= js &&
        find_dir_record("S6", "S6.IKI", &mv_dir_lba, &mv_dir_off)) {
        mv_jn = js / 2048;
        mv_un = us / 2048;
        mv_ulast = mv_ul + mv_un - 1;
        mv_us_size = us;
    }
    ready = 1;
    LOGI("censor: USA versions of Stages 1, 5 and 6 available (%d songs, movie %s); uncensored=%d", nxa,
         mv_jn ? "yes" : "no", g_uncensored);
}

int censor_available(void) { return ready; }

/* Called for every sector the game reads (before the other patches). Returns 1 when it supplied the data. */
int censor_sector(uint32_t lba, uint8_t sec[2352]) {
    if (!ready || g_uncensored || building) return 0;
    if (mv_jn && lba == mv_dir_lba) {   /* the movie's directory record: USA size (both-endian) */
        uint8_t *r = sec + 24 + mv_dir_off;
        uint32_t s = mv_us_size;
        memcpy(r + 10, &s, 4);
        r[14] = (uint8_t)(s >> 24); r[15] = (uint8_t)(s >> 16); r[16] = (uint8_t)(s >> 8); r[17] = (uint8_t)s;
        return 1;
    }
    for (int i = 0; i < nxa; i++) {
        XaSwap *x = &xa[i];
        if (lba - x->jl >= x->n) continue;
        if (!(sec[18] & 0x04) || sec[17] == x->skip_ch) return 0;
        uint8_t us[2352];
        if (aux_disc_read_sector(x->ul + (lba - x->jl), us) && !memcmp(us + 16, sec + 16, 4))
            memcpy(sec + 16, us + 16, 2352 - 16);   /* subheader + audio; the header stays the JP disc's */
        return 1;
    }
    if (mv_jn && lba - mv_jl < mv_jn) {
        uint32_t i = lba - mv_jl;
        uint8_t us[2352];
        if (aux_disc_read_sector(i < mv_un ? mv_ul + i : mv_ulast, us)) memcpy(sec + 16, us + 16, 2352 - 16);
        return 1;
    }
    for (int i = 0; i < NCF; i++) {
        CensorFile *f = &cfiles[i];
        if (f->state < 0 || lba - f->lba >= f->nsec) continue;
        if (f->state == 0) build_file(f);
        if (!f->data) return 0;
        if (f->kind == CF_OVERLAY) memcpy(g_ram + POOL_ADDR, pool, POOL_SIZE);   /* the USA lines the records point at */
        memcpy(sec + 24, f->data + (size_t)(lba - f->lba) * 2048, 2048);
        return 1;
    }
    return 0;
}
