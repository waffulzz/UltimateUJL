#include "arcade_media.h"
#include <string.h>

const ArcadeRomFile arcade_rom_files[10] = {
    {"ul1rom0l.ic12", 8388608, 0x7a77cffd}, {"ul1rom0u.ic11", 8388608, 0x1fdb34b3},
    {"ul1rom1l.ic10", 8388608, 0x2d300d8c}, {"ul1rom1u.ic9", 8388608, 0xa4762002},
    {"ul1spro.10c", 524288, 0x1bd0e763}, {"ul1vera.11s", 524288, 0x2f466df3},
    {"ul1vera.2l", 2097152, 0x40251bc3}, {"ul1vera.2p", 2097152, 0x98181955},
    {"ul1wave0.ic2", 8388608, 0x4efbff5d}, {"ul1wave1.ic1", 8388608, 0xad512f4c},
};
extern bool disc_image_has_file(const char *image, const char *file);

bool arcade_rom_read(const char *path, unsigned index, uint8_t *out, AssetZip *zip) {
    if (index >= 10 || !path || !out) return SDL_SetError("Invalid arcade ROM request");
    const ArcadeRomFile *rom = arcade_rom_files + index;
    if (zip && zip->io) {
        AssetZipEntry e;
        if (!asset_zip_find(zip, rom->name, &e)) return false;
        if (e.size != rom->size || e.crc != rom->crc)
            return SDL_SetError("%s does not match the UJL NOW! board ROM", rom->name);
        if (!asset_zip_read(zip, &e, out, rom->size)) return false;
    } else {
        int count = 0;
        char **names = SDL_GlobDirectory(path, rom->name, SDL_GLOB_CASEINSENSITIVE, &count);
        bool ok = false;
        if (names && count == 1) {
            char *file = NULL;
            SDL_asprintf(&file, "%s/%s", path, names[0]);
            SDL_IOStream *io = file ? SDL_IOFromFile(file, "rb") : NULL;
            ok = io && SDL_GetIOSize(io) == rom->size && SDL_ReadIO(io, out, rom->size) == rom->size;
            if (io) SDL_CloseIO(io);
            SDL_free(file);
        }
        SDL_free(names);
        if (!ok) return SDL_SetError("Could not read arcade ROM %s", rom->name);
    }
    if (asset_crc32(0, out, rom->size) != rom->crc)
        return SDL_SetError("Arcade ROM %s failed its CRC check", rom->name);
    return true;
}

typedef struct {
    AssetZipProgress callback;
    void *user;
    uint64_t done, total;
} Progress;
static bool report(void *user, uint64_t done, uint64_t total) {
    Progress *p = user;
    (void)total;
    return !p->callback || p->callback(p->user, p->done + done, p->total);
}
static bool write_entry(AssetZip *z, const AssetZipEntry *e, const char *path, Progress *p) {
    if (!asset_zip_write_file(z, e, path, report, p)) return false;
    p->done += e->size;
    return true;
}
static char *join(const char *directory, const char *file) {
    char *out = NULL;
    SDL_asprintf(&out, "%s/%s", directory, file);
    return out;
}
static bool checked_roms(AssetZip *z, AssetZipEntry entries[10]) {
    for (int i = 0; i < 10; i++) {
        if (!asset_zip_find(z, arcade_rom_files[i].name, entries + i)) return false;
        if (entries[i].size != arcade_rom_files[i].size || entries[i].crc != arcade_rom_files[i].crc)
            return SDL_SetError("%s does not match the UJL NOW! board ROM", arcade_rom_files[i].name);
    }
    return true;
}
static bool cd_archive(AssetZip *z, AssetZipEntry *out) {
    bool found = false, preferred = false;
    for (uint64_t i = 0; i < z->entries; i++) {
        AssetZipEntry e;
        if (!asset_zip_entry(z, i, &e)) return false;
        /* Select a CloneCD image archive, preferring the Hitachi dump when both drives are present. */
        if (!SDL_strcasestr(e.name, "CD image") || !SDL_strcasestr(e.name, "CloneCD") ||
            !SDL_strcasestr(e.name, ".zip")) continue;
        bool hitachi = SDL_strcasestr(e.name, "HL GDR-8164B") != NULL;
        if (!found || (hitachi && !preferred)) { *out = e; found = true; preferred = hitachi; }
    }
    return found || SDL_SetError("The arcade bundle does not contain a CloneCD image archive");
}
static bool cd_entry(AssetZip *z, AssetZipEntry *out) {
    bool found = false;
    for (uint64_t i = 0; i < z->entries; i++) {
        AssetZipEntry e;
        if (!asset_zip_entry(z, i, &e)) return false;
        size_t n = strlen(e.name);
        if (n < 4 || SDL_strcasecmp(e.name + n - 4, ".img")) continue;
        if (found) return SDL_SetError("The CD archive contains more than one raw image");
        *out = e; found = true;
    }
    if (!found) return SDL_SetError("The CD archive does not contain a raw .img image");
    if (!out->size || out->size > UINT64_C(2147483648) || out->size % 2352)
        return SDL_SetError("The arcade CD image has an invalid size");
    return true;
}

bool arcade_media_import(const char *archive, const char *destination, bool *has_cd,
                         AssetZipProgress progress, void *user) {
    AssetZip outer = {0}, roms = {0}, cd = {0};
    AssetZipEntry entries[10], rom_archive, image_archive, image;
    Progress p = {progress, user, 0, 0};
    char *rom_tmp = NULL, *cd_tmp = NULL, *image_path = NULL;
    char *paths[10] = {0};
    bool made = false, ok = false, bundle = false;
    *has_cd = false;
    if (!asset_zip_open(&outer, archive)) return false;
    if (!checked_roms(&outer, entries)) {
        if (!asset_zip_find(&outer, "ujlnow.zip", &rom_archive) || !cd_archive(&outer, &image_archive)) goto end;
        if (rom_archive.size > UINT64_C(1073741824) || image_archive.size > UINT64_C(2147483648)) {
            SDL_SetError("The nested arcade archives exceed the supported size"); goto end;
        }
        bundle = true;
        p.total = rom_archive.size + image_archive.size;
    }
    for (int i = 0; i < 10; i++) p.total += arcade_rom_files[i].size;
    SDL_PathInfo info;
    if (SDL_GetPathInfo(destination, &info) && info.type != SDL_PATHTYPE_NONE) {
        SDL_SetError("The arcade import destination already exists"); goto end;
    }
    if (!SDL_CreateDirectory(destination)) goto end;
    made = true;
    rom_tmp = join(destination, ".roms.zip"); cd_tmp = join(destination, ".cd.zip");
    image_path = join(destination, "arcade.img");
    if (!rom_tmp || !cd_tmp || !image_path) { SDL_OutOfMemory(); goto end; }
    AssetZip *source = &outer;
    if (bundle) {
        if (!write_entry(&outer, &rom_archive, rom_tmp, &p) || !asset_zip_open(&roms, rom_tmp) ||
            !checked_roms(&roms, entries)) goto end;
        source = &roms;
    }
    for (int i = 0; i < 10; i++) {
        paths[i] = join(destination, arcade_rom_files[i].name);
        if (!paths[i]) { SDL_OutOfMemory(); goto end; }
        if (!write_entry(source, entries + i, paths[i], &p)) goto end;
    }
    asset_zip_close(&roms);
    if (bundle) {
        SDL_RemovePath(rom_tmp);
        if (!write_entry(&outer, &image_archive, cd_tmp, &p) || !asset_zip_open(&cd, cd_tmp) || !cd_entry(&cd, &image)) goto end;
        p.total += image.size;
        if (!write_entry(&cd, &image, image_path, &p)) goto end;
        if (!disc_image_has_file(image_path, "MOVIE/AM01.IKI") || !disc_image_has_file(image_path, "MOVIE/NAMCO.IKI") ||
            !disc_image_has_file(image_path, "XA/JOE1.XA")) {
            SDL_SetError("The extracted CD is not the UJL NOW! arcade disc"); goto end;
        }
    }
    if (!report(&p, 0, 0)) { SDL_SetError("Arcade import cancelled"); goto end; }
    *has_cd = bundle; ok = true;
end:;
    char error[1024]; SDL_strlcpy(error, SDL_GetError(), sizeof error);
    asset_zip_close(&outer); asset_zip_close(&roms); asset_zip_close(&cd);
    if (made) {
        if (rom_tmp) SDL_RemovePath(rom_tmp);
        if (cd_tmp) SDL_RemovePath(cd_tmp);
        if (!ok) {
            for (int i = 0; i < 10; i++) if (paths[i]) SDL_RemovePath(paths[i]);
            if (image_path) SDL_RemovePath(image_path);
            SDL_RemovePath(destination);
        }
    }
    for (int i = 0; i < 10; i++) SDL_free(paths[i]);
    SDL_free(rom_tmp); SDL_free(cd_tmp); SDL_free(image_path);
    if (!ok) SDL_SetError("%s", error);
    return ok;
}
