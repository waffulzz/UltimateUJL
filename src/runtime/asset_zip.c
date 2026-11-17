#include "asset_zip.h"
#include <stdlib.h>
#include <string.h>

/* SDL already vendors the public-domain inflater. Keep all its symbols private to this translation unit. */
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_SDL_NOUNUSED
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define MINIZ_LITTLE_ENDIAN 1
#else
#define MINIZ_LITTLE_ENDIAN 0
#endif
#include "ujl_miniz.h"

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static bool read_at(AssetZip *z, uint64_t at, void *bytes, size_t size) {
    return at <= z->file_size && size <= z->file_size - at &&
        SDL_SeekIO(z->io, (Sint64)at, SDL_IO_SEEK_SET) >= 0 && SDL_ReadIO(z->io, bytes, size) == size;
}
uint32_t asset_crc32(uint32_t previous, const void *bytes, size_t size) {
    return (uint32_t)mz_crc32(previous, (const unsigned char *)bytes, size);
}
void asset_zip_close(AssetZip *z) {
    if (z->io) SDL_CloseIO(z->io);
    SDL_free(z->directory);
    SDL_free(z->entry_offsets);
    memset(z, 0, sizeof *z);
}
bool asset_zip_open(AssetZip *z, const char *path) {
    memset(z, 0, sizeof *z);
    z->io = SDL_IOFromFile(path, "rb");
    if (!z->io) return false;
    Sint64 length = SDL_GetIOSize(z->io);
    if (length < 22) goto invalid;
    z->file_size = (uint64_t)length;
    size_t n = z->file_size < 65557 ? (size_t)z->file_size : 65557;
    uint8_t *tail = (uint8_t *)SDL_malloc(n);
    if (!tail) { asset_zip_close(z); return SDL_SetError("Not enough memory to read the media archive"); }
    if (!read_at(z, z->file_size - n, tail, n)) { SDL_free(tail); goto invalid; }
    size_t end = SIZE_MAX;
    for (size_t p = n - 22;; p--) {
        if (u32(tail + p) == 0x06054b50 && p + 22 + u16(tail + p + 20) == n) { end = p; break; }
        if (!p) break;
    }
    if (end == SIZE_MAX) { SDL_free(tail); goto invalid; }
    const uint8_t *e = tail + end;
    uint64_t count = u16(e + 10), cd_size = u32(e + 12), cd_at = u32(e + 16);
    bool disks = u16(e + 4) || u16(e + 6) || u16(e + 8) != u16(e + 10);
    uint64_t end_at = z->file_size - n + end;
    SDL_free(tail);
    if (disks) goto invalid;
    if (count == 65535 || cd_size == UINT32_MAX || cd_at == UINT32_MAX) {
        uint8_t locator[20], record[56];
        if (end_at < 20 || !read_at(z, end_at - 20, locator, sizeof locator) ||
            u32(locator) != 0x07064b50 || u32(locator + 4) || u32(locator + 16) != 1 ||
            !read_at(z, u64(locator + 8), record, sizeof record) || u32(record) != 0x06064b50 ||
            u64(record + 4) < 44 || u32(record + 16) || u32(record + 20) || u64(record + 24) != u64(record + 32)) goto invalid;
        count = u64(record + 32); cd_size = u64(record + 40); cd_at = u64(record + 48);
    }
    if (!count || count > 65536 || cd_size > (16u << 20) || cd_at > end_at || cd_size > end_at - cd_at) goto invalid;
    z->directory = (uint8_t *)SDL_malloc((size_t)cd_size);
    if (!z->directory) { asset_zip_close(z); return SDL_SetError("Not enough memory to read the media archive"); }
    z->directory_size = (size_t)cd_size; z->directory_offset = cd_at; z->entries = count;
    if (!read_at(z, cd_at, z->directory, z->directory_size)) goto invalid;
    z->entry_offsets = SDL_malloc((size_t)count * sizeof *z->entry_offsets);
    if (!z->entry_offsets) { asset_zip_close(z); return SDL_OutOfMemory(); }
    size_t pos = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (pos > z->directory_size || z->directory_size - pos < 46 || u32(z->directory + pos) != 0x02014b50) goto invalid;
        const uint8_t *entry = z->directory + pos;
        size_t length = 46u + u16(entry + 28) + u16(entry + 30) + u16(entry + 32);
        if (length > z->directory_size - pos) goto invalid;
        z->entry_offsets[i] = (uint32_t)pos;
        pos += length;
    }
    return true;
invalid:
    asset_zip_close(z);
    return SDL_SetError("That file is not a supported single-volume ZIP archive");
}

bool asset_zip_entry(const AssetZip *z, uint64_t index, AssetZipEntry *out) {
    if (index >= z->entries) return false;
    size_t p = z->entry_offsets[index];
    {
        if (p > z->directory_size || z->directory_size - p < 46) goto invalid;
        const uint8_t *e = z->directory + p;
        if (u32(e) != 0x02014b50) goto invalid;
        size_t name = u16(e + 28), extra = u16(e + 30), comment = u16(e + 32);
        if (name + extra + comment > z->directory_size - p - 46) goto invalid;
        memset(out, 0, sizeof *out);
        if (!name || name >= sizeof out->name || memchr(e + 46, 0, name)) goto invalid;
        memcpy(out->name, e + 46, name);
        out->flags = u16(e + 8); out->method = u16(e + 10); out->crc = u32(e + 16);
        out->packed = u32(e + 20); out->size = u32(e + 24); out->offset = u32(e + 42);
        if (u16(e + 34)) goto invalid;
        bool zip64 = out->packed == UINT32_MAX || out->size == UINT32_MAX || out->offset == UINT32_MAX;
        if (zip64) {
            const uint8_t *x = e + 46 + name; size_t left = extra;
            while (left >= 4 && u16(x) != 1) {
                size_t length = u16(x + 2) + 4u;
                if (length > left) goto invalid;
                x += length; left -= length;
            }
            if (left < 4 || u16(x) != 1 || u16(x + 2) > left - 4) goto invalid;
            left = u16(x + 2); x += 4;
#define ZIP64_VALUE(field) do { if (out->field == UINT32_MAX) { if (left < 8) goto invalid; out->field = u64(x); x += 8; left -= 8; } } while (0)
            ZIP64_VALUE(size); ZIP64_VALUE(packed); ZIP64_VALUE(offset);
#undef ZIP64_VALUE
        }
        if (out->offset > z->directory_offset || out->packed > z->directory_offset - out->offset) goto invalid;
        return true;
    }
invalid: return SDL_SetError("The media archive directory is damaged");
}
static const char *basename_of(const char *s) {
    const char *a = strrchr(s, '/'), *b = strrchr(s, '\\');
    if (!a || (b && b > a)) a = b;
    return a ? a + 1 : s;
}
bool asset_zip_find(const AssetZip *z, const char *basename, AssetZipEntry *out) {
    bool found = false;
    for (uint64_t i = 0; i < z->entries; i++) {
        AssetZipEntry e;
        if (!asset_zip_entry(z, i, &e)) return false;
        if (!SDL_strcasecmp(basename_of(e.name), basename)) {
            if (found) return SDL_SetError("The media archive contains duplicate files named %s", basename);
            *out = e; found = true;
        }
    }
    return found || SDL_SetError("The media archive does not contain %s", basename);
}

bool asset_zip_extract(AssetZip *z, const AssetZipEntry *e, AssetZipSink sink, void *user,
                       AssetZipProgress progress, void *progress_user) {
    uint8_t local[30];
    if ((e->flags & 1) || (e->method != 0 && e->method != 8))
        return SDL_SetError("That archive entry uses an unsupported compression or encryption method");
    if (!read_at(z, e->offset, local, sizeof local) || u32(local) != 0x04034b50 ||
        u16(local + 8) != e->method || u16(local + 6) != e->flags)
        return SDL_SetError("The media archive entry is damaged");
    uint64_t at = e->offset + 30u + u16(local + 26) + u16(local + 28);
    if (at > z->directory_offset || e->packed > z->directory_offset - at || (e->method == 0 && e->packed != e->size))
        return SDL_SetError("The media archive entry has an invalid size");
    uint8_t *input = (uint8_t *)SDL_malloc(65536), *output = (uint8_t *)SDL_malloc(32768);
    tinfl_decompressor *state = (tinfl_decompressor *)SDL_malloc(sizeof *state);
    if (!input || !output || !state) { SDL_free(input); SDL_free(output); SDL_free(state); return SDL_SetError("Not enough memory to import media"); }
    tinfl_init(state);
    uint64_t remaining = e->packed, made = 0;
    size_t used = 0, available = 0, out_at = 0;
    uint32_t crc = 0;
    bool ok = false;
    SDL_SetError("The media archive entry failed its size or CRC check");
    for (;;) {
        if (progress && !progress(progress_user, made, e->size)) { SDL_SetError("Arcade media import cancelled"); break; }
        if (used == available && remaining) {
            available = remaining < 65536 ? (size_t)remaining : 65536; used = 0;
            if (!read_at(z, at, input, available)) { SDL_SetError("Could not read the media archive"); break; }
            at += available; remaining -= available;
        }
        size_t consumed = available - used, produced = 32768 - out_at;
        tinfl_status status;
        if (e->method == 0) {
            produced = consumed; status = remaining ? TINFL_STATUS_NEEDS_MORE_INPUT : TINFL_STATUS_DONE;
            if (produced > e->size - made || !sink(user, input + used, produced)) break;
            crc = asset_crc32(crc, input + used, produced); made += produced; used += consumed;
        } else {
            status = tinfl_decompress(state, input + used, &consumed, output, output + out_at, &produced,
                remaining ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            used += consumed;
            if (produced > e->size - made || !sink(user, output + out_at, produced)) break;
            crc = asset_crc32(crc, output + out_at, produced); made += produced;
            out_at = (out_at + produced) & 32767;
        }
        if (status == TINFL_STATUS_DONE) { ok = made == e->size && crc == e->crc && !remaining && used == available; break; }
        if (status < 0 || (!produced && !consumed && !remaining)) break;
    }
    SDL_free(input); SDL_free(output); SDL_free(state);
    return ok;
}
typedef struct { uint8_t *bytes; size_t at, capacity; } MemorySink;
static bool memory_sink(void *user, const uint8_t *bytes, size_t size) {
    MemorySink *m = (MemorySink *)user;
    if (size > m->capacity - m->at) return false;
    memcpy(m->bytes + m->at, bytes, size); m->at += size;
    return true;
}
bool asset_zip_read(AssetZip *z, const AssetZipEntry *e, void *bytes, size_t capacity) {
    if (e->size > capacity) return SDL_SetError("That media entry is too large");
    MemorySink m = {(uint8_t *)bytes, 0, capacity};
    return asset_zip_extract(z, e, memory_sink, &m, NULL, NULL);
}
static bool file_sink(void *user, const uint8_t *bytes, size_t size) {
    return SDL_WriteIO((SDL_IOStream *)user, bytes, size) == size;
}
bool asset_zip_write_file(AssetZip *z, const AssetZipEntry *e, const char *path, AssetZipProgress progress, void *user) {
    SDL_IOStream *out = SDL_IOFromFile(path, "wb");
    if (!out) return false;
    bool ok = asset_zip_extract(z, e, file_sink, out, progress, user);
    if (!SDL_CloseIO(out)) ok = false;
    if (!ok) SDL_RemovePath(path);
    return ok;
}
