/* Bounded ZIP/ZIP64 reader for user-supplied media. Entries stream to a caller-owned destination. */
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    SDL_IOStream *io;
    uint8_t *directory;
    uint32_t *entry_offsets;
    size_t directory_size;
    uint64_t file_size, directory_offset, entries;
} AssetZip;
typedef struct {
    char name[1024];
    uint64_t offset, packed, size;
    uint32_t crc;
    uint16_t method, flags;
} AssetZipEntry;
typedef bool (*AssetZipSink)(void *user, const uint8_t *bytes, size_t size);
typedef bool (*AssetZipProgress)(void *user, uint64_t done, uint64_t total);
bool asset_zip_open(AssetZip *zip, const char *path);
void asset_zip_close(AssetZip *zip);
bool asset_zip_entry(const AssetZip *zip, uint64_t index, AssetZipEntry *entry);
bool asset_zip_find(const AssetZip *zip, const char *basename, AssetZipEntry *entry);
bool asset_zip_extract(AssetZip *zip, const AssetZipEntry *entry, AssetZipSink sink, void *user,
                       AssetZipProgress progress, void *progress_user);
bool asset_zip_read(AssetZip *zip, const AssetZipEntry *entry, void *bytes, size_t capacity);
bool asset_zip_write_file(AssetZip *zip, const AssetZipEntry *entry, const char *path,
                          AssetZipProgress progress, void *user);
uint32_t asset_crc32(uint32_t previous, const void *bytes, size_t size);
#ifdef __cplusplus
}
#endif
