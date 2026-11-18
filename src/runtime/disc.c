/* Raw Mode2/2352 disc image access + minimal ISO9660 lookup.
   The main disc (JP) feeds the emulated CD drive; an optional auxiliary disc (USA) is used only
   as an asset source (e.g. the English title logo). */
#include "psx.h"
#include <stdlib.h>
#include <ctype.h>

#ifndef _WIN32
#define _ftelli64 ftello
#define _fseeki64 fseeko
#endif

typedef struct { FILE *f; uint32_t sectors; } DiscImg;
static DiscImg main_disc, aux_disc, arcade_disc;

static bool img_open(DiscImg *d, const char *path) {
    if (d->f) fclose(d->f);
    d->f = fopen(path, "rb");
    d->sectors = 0;
    if (!d->f) return false;
    _fseeki64(d->f, 0, SEEK_END);
    d->sectors = (uint32_t)(_ftelli64(d->f) / 2352);
    return true;
}

static bool img_read_sector(DiscImg *d, uint32_t lba, uint8_t out[2352]) {
    if (!d->f || lba >= d->sectors) { memset(out, 0, 2352); return false; }
    _fseeki64(d->f, (long long)lba * 2352, SEEK_SET);
    return fread(out, 1, 2352, d->f) == 2352;
}

void patch_sector(uint32_t lba, uint8_t sec[2352]);
static int patching_enabled;
void disc_enable_patches(int on) { patching_enabled = on; }

bool disc_open(const char *path) { return img_open(&main_disc, path); }

bool disc_read_sector(uint32_t lba, uint8_t out[2352]) {
    bool ok = img_read_sector(&main_disc, lba, out);
    if (ok && patching_enabled) patch_sector(lba, out);
    return ok;
}

static bool read_data(DiscImg *d, uint32_t lba, uint8_t *dst, uint32_t size) {
    uint8_t sec[2352];
    while (size) {
        bool ok = (d == &main_disc) ? disc_read_sector(lba, sec) : img_read_sector(d, lba, sec);
        if (!ok) return false;
        lba++;
        uint32_t n = size < 2048 ? size : 2048;
        memcpy(dst, sec + 24, n);
        dst += n;
        size -= n;
    }
    return true;
}

static bool name_eq(const char *a, const uint8_t *b, int blen) {
    int i = 0;
    for (; i < blen && b[i] != ';'; i++) {
        if (!a[i] || toupper((unsigned char)a[i]) != toupper(b[i])) return false;
    }
    return a[i] == 0;
}

/* path like "\\S1\\ST1.XA;1" or "S1/ST1.XA" */
static bool img_find_file(DiscImg *d, const char *path, uint32_t *out_lba, uint32_t *out_size) {
    uint8_t pvd[2048];
    if (!read_data(d, 16, pvd, 2048) || memcmp(pvd + 1, "CD001", 5)) return false;
    uint32_t lba, size;
    memcpy(&lba, pvd + 156 + 2, 4);
    memcpy(&size, pvd + 156 + 10, 4);
    char comp[64];
    const char *p = path;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && *p != ';' && n < 63) comp[n++] = *p++;
        comp[n] = 0;
        if (*p == ';') while (*p && *p != '\\' && *p != '/') p++;
        if (!n) break;
        uint8_t *dir = (uint8_t *)malloc(size + 2048);
        if (!read_data(d, lba, dir, size)) { free(dir); return false; }
        bool found = false;
        for (uint32_t i = 0; i < size;) {
            uint8_t len = dir[i];
            if (!len) { i = (i / 2048 + 1) * 2048; continue; }
            if (name_eq(comp, dir + i + 33, dir[i + 32])) {
                memcpy(&lba, dir + i + 2, 4);
                memcpy(&size, dir + i + 10, 4);
                found = true;
                break;
            }
            i += len;
        }
        free(dir);
        if (!found) return false;
    }
    *out_lba = lba;
    *out_size = size;
    return true;
}

static bool img_read_file(DiscImg *d, const char *path, uint8_t **data, uint32_t *size) {
    uint32_t lba;
    if (!img_find_file(d, path, &lba, size)) return false;
    *data = (uint8_t *)malloc(*size + 2048);
    return read_data(d, lba, *data, *size);
}

bool disc_find_file(const char *path, uint32_t *lba, uint32_t *size) { return img_find_file(&main_disc, path, lba, size); }
bool disc_read_file(const char *path, uint8_t **data, uint32_t *size) { return img_read_file(&main_disc, path, data, size); }

/* auxiliary (USA) disc: asset source only */
bool aux_disc_open(const char *path) { return img_open(&aux_disc, path); }
bool aux_disc_ready(void) { return aux_disc.f != NULL; }
bool aux_disc_read_file(const char *path, uint8_t **data, uint32_t *size) { return aux_disc.f && img_read_file(&aux_disc, path, data, size); }
bool aux_disc_find_file(const char *path, uint32_t *lba, uint32_t *size) { return aux_disc.f && img_find_file(&aux_disc, path, lba, size); }
bool aux_disc_read_sector(uint32_t lba, uint8_t out[2352]) { return img_read_sector(&aux_disc, lba, out); }

/* UJL NOW! arcade CD (raw image the setup wizard imported): asset source only (its Stage 1 song XA) */
bool arcade_disc_open(const char *path) { return img_open(&arcade_disc, path); }
bool arcade_disc_ready(void) { return arcade_disc.f != NULL; }
bool arcade_disc_find_file(const char *path, uint32_t *lba, uint32_t *size) { return arcade_disc.f && img_find_file(&arcade_disc, path, lba, size); }
bool arcade_disc_read_sector(uint32_t lba, uint8_t out[2352]) { return img_read_sector(&arcade_disc, lba, out); }

/* Identification (setup wizard): read the primary volume descriptor or one file of any image without touching the
   main or auxiliary disc. False for unreadable files and images without raw 2352-byte Mode 2 sectors. */
static bool img_open_checked(DiscImg *d, const char *path) {
    if (!img_open(d, path)) return false;
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    uint8_t sec[2352];
    if (d->sectors > 16 && img_read_sector(d, 16, sec) && !memcmp(sec, sync, 12)) return true;
    fclose(d->f);
    d->f = NULL;
    return false;
}
bool disc_image_pvd(const char *path, uint8_t pvd[2048]) {
    DiscImg d = {0};
    if (!img_open_checked(&d, path)) return false;
    bool ok = read_data(&d, 16, pvd, 2048) && !memcmp(pvd + 1, "CD001", 5);
    fclose(d.f);
    return ok;
}
bool disc_image_read_file(const char *path, const char *file, uint8_t **data, uint32_t *size) {
    DiscImg d = {0};
    if (!img_open_checked(&d, path)) return false;
    bool ok = img_read_file(&d, file, data, size);
    fclose(d.f);
    return ok;
}
bool disc_image_has_file(const char *path, const char *file) {
    DiscImg d = {0};
    if (!img_open_checked(&d, path)) return false;
    uint32_t lba, size;
    bool ok = img_find_file(&d, file, &lba, &size);
    fclose(d.f);
    return ok;
}

/* Does `path` boot the given executable name (e.g. "SCUS_944.48")? Uses the aux slot temporarily. */
bool disc_image_boots(const char *path, const char *exe_name) {
    DiscImg d = {0};
    if (!img_open(&d, path)) return false;
    uint8_t *cnf; uint32_t sz;
    bool ok = false;
    if (img_read_file(&d, "SYSTEM.CNF", &cnf, &sz)) {
        cnf[sz < 2048 ? sz : 2047] = 0;
        ok = strstr((char *)cnf, exe_name) != NULL;
        free(cnf);
    }
    fclose(d.f);
    return ok;
}
